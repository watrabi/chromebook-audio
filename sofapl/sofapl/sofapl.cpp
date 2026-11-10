// sofapl: Sound Open Firmware DSP driver for Intel Apollo Lake Chromebooks.
//
// Milestone 2: bind to the Audio DSP child device created by sklhdaudbus,
// power the DSP, download sof-apl.ri over the HDA code-loader DMA stream and
// wait for the firmware's FW_READY message. Progress is written to the device's
// registry key ("Device Parameters\BootLog") so it can be read without a debugger.
//
// The boot sequence follows the Linux SOF driver (sound/soc/sof/intel/hda-loader.c,
// hda-dsp.c; GPL-2.0 OR BSD-3-Clause, Copyright(c) 2018 Intel Corporation).

#pragma warning(disable:4201)  // nameless struct/union
#pragma warning(disable:4996)  // ExAllocatePoolWithTag deprecation

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include <ntstrsafe.h>
#include <hdaudio.h>

#include "adsp_iface.h"
#include "sof_regs.h"
#include "..\..\include\sofapl_iface.h"

#define SOFAPL_POOL_TAG 'LPAS'
#define FW_PATH L"\\SystemRoot\\System32\\drivers\\intel-sof-apl.ri"
#define FW_MAX_SIZE (4 * 1024 * 1024)
#define LOG_CHARS 8192

typedef struct _EP_STATE {
    BOOLEAN PipelineReady;
    HANDLE Stream;
    UINT8 Tag;
    ULONG Idx;
    ULONG Size;
    BOOLEAN ParamsSet;
    BOOLEAN Running;
    BOOLEAN Paused;     // started once, currently paused (resume with TRIG_RELEASE)
} EP_STATE;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE Device;

    ADSP_BUS_INTERFACE Bus;
    BOOLEAN BusValid;

    PCI_BAR HdaBar;
    PCI_BAR DspBar;
    PUINT8 Ppcap;
    NHLT_INFO Nhlt;

    PUINT8 FwFile;
    ULONG FwFileSize;

    BOOLEAN PowerHeld;
    BOOLEAN FwRunning;

    // IPC mailboxes (offsets in the DSP BAR), from the FW_READY window info
    ULONG HostBoxOff;   // host -> DSP ("downbox")
    ULONG HostBoxSize;
    ULONG DspBoxOff;    // DSP -> host ("upbox")
    ULONG DspBoxSize;
    ULONG IpcMaxSize;   // from the extended manifest config data
    ULONG IpcCount;
    ULONG NotifyCount;

    FAST_MUTEX IpcLock;

    // Playback endpoints (speaker, headphone): pipeline state and the HDA
    // host stream owned through sklhdaudbus
    EP_STATE Ep[SOFAPL_EP_COUNT];

    // \Callback\CsAudioCallbackAPI (speaker amp enable via max98357a driver)
    PCALLBACK_OBJECT CsAudioCallback;

    WCHAR Log[LOG_CHARS];
} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SofEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SofEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SofEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SofEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT SofEvtD0Exit;

// ---------------------------------------------------------------------------
// Logging (to DbgPrint and to the device registry key)
// ---------------------------------------------------------------------------

static void LogLine(PDEVICE_CONTEXT ctx, _Printf_format_string_ PCWSTR fmt, ...)
{
    WCHAR line[256];
    va_list args;
    va_start(args, fmt);
    NTSTATUS st = RtlStringCchVPrintfW(line, ARRAYSIZE(line), fmt, args);
    va_end(args);
    if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW) {
        return;
    }
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "sofapl: %ws\n", line);
    // keep the most recent activity once the buffer fills up
    size_t used = 0;
    RtlStringCchLengthW(ctx->Log, LOG_CHARS, &used);
    if (used + 300 >= LOG_CHARS) {
        RtlStringCchCopyW(ctx->Log, LOG_CHARS, L"(earlier log lines dropped)\r\n");
    }
    RtlStringCchCatW(ctx->Log, LOG_CHARS, line);
    RtlStringCchCatW(ctx->Log, LOG_CHARS, L"\r\n");
}

static void SaveRegistry(PDEVICE_CONTEXT ctx, PCWSTR stage, NTSTATUS result)
{
    WDFKEY key;
    NTSTATUS st = WdfDeviceOpenRegistryKey(ctx->Device, PLUGPLAY_REGKEY_DEVICE,
        KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(st)) {
        return;
    }

    UNICODE_STRING name, value;

    RtlInitUnicodeString(&name, L"BootLog");
    RtlInitUnicodeString(&value, ctx->Log);
    WdfRegistryAssignUnicodeString(key, &name, &value);

    RtlInitUnicodeString(&name, L"BootStage");
    RtlInitUnicodeString(&value, stage);
    WdfRegistryAssignUnicodeString(key, &name, &value);

    RtlInitUnicodeString(&name, L"BootStatus");
    WdfRegistryAssignULong(key, &name, (ULONG)result);

    WdfRegistryClose(key);
}

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------

static __forceinline ULONG DspRead(PDEVICE_CONTEXT ctx, ULONG off)
{
    return READ_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + off));
}

static __forceinline void DspWrite(PDEVICE_CONTEXT ctx, ULONG off, ULONG val)
{
    WRITE_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + off), val);
}

static __forceinline void DspUpdate(PDEVICE_CONTEXT ctx, ULONG off, ULONG mask, ULONG val)
{
    DspWrite(ctx, off, (DspRead(ctx, off) & ~mask) | (val & mask));
}

static void SleepUs(ULONG us)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -((LONGLONG)us * 10);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// Poll a DSP register until (value & mask) == expected, or timeout.
static NTSTATUS DspPoll(PDEVICE_CONTEXT ctx, ULONG off, ULONG mask, ULONG expected,
    ULONG timeoutUs, PULONG lastValue)
{
    ULONGLONG start = KeQueryInterruptTime();
    ULONGLONG limit = (ULONGLONG)timeoutUs * 10;
    ULONG v;
    for (;;) {
        v = DspRead(ctx, off);
        if ((v & mask) == expected) {
            if (lastValue) *lastValue = v;
            return STATUS_SUCCESS;
        }
        if (KeQueryInterruptTime() - start > limit) {
            break;
        }
        SleepUs(500);
    }
    if (lastValue) *lastValue = v;
    return STATUS_IO_TIMEOUT;
}

// Poll the ROM/FW status register (window 0) for a given state code.
static NTSTATUS DspPollRomState(PDEVICE_CONTEXT ctx, ULONG state, ULONG timeoutUs, PULONG lastValue)
{
    return DspPoll(ctx, SRAM_REG_ROM_STATUS, FSR_STATE_MASK, state, timeoutUs, lastValue);
}

// ---------------------------------------------------------------------------
// DSP core control (hda-dsp.c)
// ---------------------------------------------------------------------------

static NTSTATUS CoreResetEnter(PDEVICE_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), ADSPCS_CRST(cm));
    return DspPoll(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), ADSPCS_CRST(cm), TIMEOUT_CORE_US, NULL);
}

static NTSTATUS CoreResetLeave(PDEVICE_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), 0);
    return DspPoll(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), 0, TIMEOUT_CORE_US, NULL);
}

static NTSTATUS CoreStallReset(PDEVICE_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CSTALL(cm), ADSPCS_CSTALL(cm));
    return CoreResetEnter(ctx, cm);
}

static BOOLEAN CoreIsEnabled(PDEVICE_CONTEXT ctx, ULONG cm)
{
    ULONG v = DspRead(ctx, DSP_REG_ADSPCS);
    return ((v & ADSPCS_CPA(cm)) == ADSPCS_CPA(cm)) &&
        ((v & ADSPCS_SPA(cm)) == ADSPCS_SPA(cm)) &&
        !(v & ADSPCS_CRST(cm)) &&
        !(v & ADSPCS_CSTALL(cm));
}

static NTSTATUS CorePowerUp(PDEVICE_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_SPA(cm), ADSPCS_SPA(cm));
    return DspPoll(ctx, DSP_REG_ADSPCS, ADSPCS_CPA(cm), ADSPCS_CPA(cm), TIMEOUT_CORE_US, NULL);
}

static NTSTATUS CorePowerDown(PDEVICE_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_SPA(cm), 0);
    return DspPoll(ctx, DSP_REG_ADSPCS, ADSPCS_CPA(cm), 0, TIMEOUT_CORE_US, NULL);
}

static NTSTATUS CoreRun(PDEVICE_CONTEXT ctx, ULONG cm)
{
    NTSTATUS st = CoreResetLeave(ctx, cm);
    if (!NT_SUCCESS(st)) return st;
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CSTALL(cm), 0);
    if (!CoreIsEnabled(ctx, cm)) {
        CoreStallReset(ctx, cm);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS CoreResetPowerDown(PDEVICE_CONTEXT ctx, ULONG cm)
{
    NTSTATUS st = CoreStallReset(ctx, cm);
    if (!NT_SUCCESS(st)) return st;
    return CorePowerDown(ctx, cm);
}

// ---------------------------------------------------------------------------
// Firmware file
// ---------------------------------------------------------------------------

static NTSTATUS LoadFirmwareFile(PDEVICE_CONTEXT ctx)
{
    if (ctx->FwFile) return STATUS_SUCCESS;

    UNICODE_STRING path;
    RtlInitUnicodeString(&path, FW_PATH);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    HANDLE h;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"Open firmware %s failed: 0x%08X", FW_PATH, st);
        return st;
    }

    FILE_STANDARD_INFORMATION fsi;
    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (NT_SUCCESS(st) && (fsi.EndOfFile.QuadPart <= 0 || fsi.EndOfFile.QuadPart > FW_MAX_SIZE)) {
        st = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS(st)) {
        ULONG size = (ULONG)fsi.EndOfFile.QuadPart;
        PUINT8 buf = (PUINT8)ExAllocatePoolWithTag(NonPagedPoolNx, size, SOFAPL_POOL_TAG);
        if (!buf) {
            st = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            LARGE_INTEGER offset = { 0 };
            st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, size, &offset, NULL);
            if (NT_SUCCESS(st) && iosb.Information != size) {
                st = STATUS_END_OF_FILE;
            }
            if (NT_SUCCESS(st)) {
                ctx->FwFile = buf;
                ctx->FwFileSize = size;
            } else {
                ExFreePoolWithTag(buf, SOFAPL_POOL_TAG);
            }
        }
    }
    ZwClose(h);

    if (NT_SUCCESS(st)) {
        LogLine(ctx, L"Firmware loaded from disk: %u bytes", ctx->FwFileSize);
    } else {
        LogLine(ctx, L"Reading firmware failed: 0x%08X", st);
    }
    return st;
}

// ---------------------------------------------------------------------------
// Mailbox access and IPC3 (polling mode)
// ---------------------------------------------------------------------------

static void MboxRead(PDEVICE_CONTEXT ctx, ULONG off, PVOID buf, ULONG len)
{
    PUINT8 dst = (PUINT8)buf;
    for (ULONG i = 0; i < len; i += 4) {
        ULONG v = READ_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + off + i));
        ULONG n = min(4u, len - i);
        RtlCopyMemory(dst + i, &v, n);
    }
}

static void MboxWrite(PDEVICE_CONTEXT ctx, ULONG off, const VOID* buf, ULONG len)
{
    const UINT8* src = (const UINT8*)buf;
    for (ULONG i = 0; i < len; i += 4) {
        ULONG v = 0;
        ULONG n = min(4u, len - i);
        RtlCopyMemory(&v, src + i, n);
        WRITE_REGISTER_ULONG((PULONG)(ctx->DspBar.Base.baseptr + off + i), v);
    }
}

// Parse the extended data that follows FW_READY (ipc3_fw_parse_ext_data)
// to find the IPC mailbox windows.
static void ParseExtChain(PDEVICE_CONTEXT ctx, ULONG offset);

// SOF 1.6+ firmware describes its SRAM windows in the extended manifest at the
// start of the .ri file (snd_sof_fw_ext_man_parse / SOF_EXT_MAN_ELEM_WINDOW).
#define SOF_EXT_MAN_ELEM_WINDOW       1
#define SOF_EXT_MAN_ELEM_CONFIG_DATA  5
#define SOF_EXT_MAN_CONFIG_IPC_MSG_SIZE 1

static void ParseFwExtManifest(PDEVICE_CONTEXT ctx, ULONG manSize)
{
    if (manSize < sizeof(SOF_EXT_MAN_HEADER) || manSize > ctx->FwFileSize) return;
    const SOF_EXT_MAN_HEADER* mh = (const SOF_EXT_MAN_HEADER*)ctx->FwFile;
    ULONG off = mh->header_size;
    while (off + 8 <= manSize) {
        UINT32 type = *(const UINT32*)(ctx->FwFile + off);
        UINT32 size = *(const UINT32*)(ctx->FwFile + off + 4);
        if (size < 8 || off + size > manSize) break;

        if (type == SOF_EXT_MAN_ELEM_WINDOW && size >= 8 + sizeof(SOF_IPC_EXT_DATA_HDR) + 4) {
            const UINT8* w = ctx->FwFile + off + 8;    // struct sof_ipc_window
            UINT32 num = *(const UINT32*)(w + sizeof(SOF_IPC_EXT_DATA_HDR));
            for (UINT32 i = 0; i < num && i < 15; i++) {
                ULONG eoff = sizeof(SOF_IPC_EXT_DATA_HDR) + 4 + i * sizeof(SOF_IPC_WINDOW_ELEM);
                if (8 + eoff + sizeof(SOF_IPC_WINDOW_ELEM) > size) break;
                const SOF_IPC_WINDOW_ELEM* e = (const SOF_IPC_WINDOW_ELEM*)(w + eoff);
                ULONG abs = SRAM_WINDOW_OFFSET(e->id) + e->offset;
                LogLine(ctx, L"  manifest window type %u id %u offset 0x%X size 0x%X -> BAR 0x%X",
                    e->type, e->id, e->offset, e->size, abs);
                if (abs + e->size > ctx->DspBar.Len) continue;
                if (e->type == SOF_IPC_REGION_DOWNBOX) {
                    ctx->HostBoxOff = abs;
                    ctx->HostBoxSize = e->size;
                } else if (e->type == SOF_IPC_REGION_UPBOX) {
                    ctx->DspBoxOff = abs;
                    ctx->DspBoxSize = e->size;
                }
            }
        } else if (type == SOF_EXT_MAN_ELEM_CONFIG_DATA) {
            for (ULONG j = off + 8; j + 8 <= off + size; j += 8) {
                UINT32 token = *(const UINT32*)(ctx->FwFile + j);
                UINT32 value = *(const UINT32*)(ctx->FwFile + j + 4);
                if (token == SOF_EXT_MAN_CONFIG_IPC_MSG_SIZE && value >= 64) {
                    ctx->IpcMaxSize = value;
                }
            }
        }
        off += size;
    }
    // never write past what the firmware accepts as one message
    if (ctx->IpcMaxSize && ctx->HostBoxSize > ctx->IpcMaxSize) {
        ctx->HostBoxSize = ctx->IpcMaxSize;
    }
    LogLine(ctx, L"Manifest: host->DSP mailbox 0x%X (%u), DSP->host 0x%X (%u), max IPC %u",
        ctx->HostBoxOff, ctx->HostBoxSize, ctx->DspBoxOff, ctx->DspBoxSize, ctx->IpcMaxSize);
}

static void DumpWords(PDEVICE_CONTEXT ctx, PCWSTR label, ULONG offset, ULONG words)
{
    for (ULONG i = 0; i < words; i += 8) {
        ULONG w[8] = { 0 };
        MboxRead(ctx, offset + i * 4, w, sizeof(w));
        LogLine(ctx, L"  %s 0x%05X: %08X %08X %08X %08X %08X %08X %08X %08X",
            label, offset + i * 4, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

static void ParseExtData(PDEVICE_CONTEXT ctx, ULONG offset)
{
    DumpWords(ctx, L"after FW_READY", offset, 16);
    ParseExtChain(ctx, offset);
    if (ctx->HostBoxSize && ctx->DspBoxSize) return;

    // Not where Linux expects it: scan SRAM window 0 for a window ext header
    // (cmd = FW_READY, type = EXT_WINDOW) and report where it was found.
    for (ULONG off = SRAM_WINDOW_OFFSET(0); off + 16 <= SRAM_WINDOW_OFFSET(0) + 0x4000; off += 4) {
        SOF_IPC_EXT_DATA_HDR eh;
        MboxRead(ctx, off, &eh, sizeof(eh));
        if (eh.hdr.cmd == SOF_IPC_FW_READY && eh.type == SOF_IPC_EXT_WINDOW &&
            eh.hdr.size >= sizeof(eh) + 4 && eh.hdr.size <= 4096) {
            LogLine(ctx, L"  found window ext header at 0x%X (expected 0x%X)", off, offset);
            ParseExtChain(ctx, off);
            if (ctx->HostBoxSize && ctx->DspBoxSize) return;
        }
    }
    LogLine(ctx, L"  no window ext header found in window 0");
}

static void ParseExtChain(PDEVICE_CONTEXT ctx, ULONG offset)
{
    for (int guard = 0; guard < 16; guard++) {
        if (offset + sizeof(SOF_IPC_EXT_DATA_HDR) > ctx->DspBar.Len) break;
        SOF_IPC_EXT_DATA_HDR eh;
        MboxRead(ctx, offset, &eh, sizeof(eh));
        if (eh.hdr.cmd != SOF_IPC_FW_READY || eh.hdr.size < sizeof(eh) || eh.hdr.size > 4096) {
            if (guard == 0) {
                LogLine(ctx, L"  no ext header at 0x%X (cmd 0x%08X size %u type %u)",
                    offset, eh.hdr.cmd, eh.hdr.size, eh.type);
            }
            break;
        }
        LogLine(ctx, L"  ext data type %u, %u bytes", eh.type, eh.hdr.size);

        if (eh.type == SOF_IPC_EXT_WINDOW) {
            UINT32 num = 0;
            MboxRead(ctx, offset + sizeof(eh), &num, sizeof(num));
            for (UINT32 i = 0; i < num && i < 15; i++) {
                ULONG eoff = offset + sizeof(eh) + 4 + i * sizeof(SOF_IPC_WINDOW_ELEM);
                if (eoff + sizeof(SOF_IPC_WINDOW_ELEM) > offset + eh.hdr.size) break;
                SOF_IPC_WINDOW_ELEM w;
                MboxRead(ctx, eoff, &w, sizeof(w));
                ULONG abs = SRAM_WINDOW_OFFSET(w.id) + w.offset;
                LogLine(ctx, L"    window type %u id %u offset 0x%X size 0x%X -> BAR 0x%X",
                    w.type, w.id, w.offset, w.size, abs);
                if (abs + w.size > ctx->DspBar.Len) continue;
                if (w.type == SOF_IPC_REGION_DOWNBOX) {
                    ctx->HostBoxOff = abs;
                    ctx->HostBoxSize = w.size;
                } else if (w.type == SOF_IPC_REGION_UPBOX) {
                    ctx->DspBoxOff = abs;
                    ctx->DspBoxSize = w.size;
                }
            }
        }
        offset += eh.hdr.size;
    }
    LogLine(ctx, L"IPC mailboxes: host->DSP 0x%X (%u bytes), DSP->host 0x%X (%u bytes)",
        ctx->HostBoxOff, ctx->HostBoxSize, ctx->DspBoxOff, ctx->DspBoxSize);
}

// Acknowledge any DSP-initiated message (notifications, trace, position).
static void IpcServiceNotifications(PDEVICE_CONTEXT ctx)
{
    ULONG hipct = DspRead(ctx, DSP_REG_HIPCT);
    if (!(hipct & HIPCT_BUSY)) return;

    SOF_IPC_CMD_HDR hdr = { 0 };
    if (ctx->DspBoxSize >= sizeof(hdr)) {
        MboxRead(ctx, ctx->DspBoxOff, &hdr, sizeof(hdr));
    }
    ctx->NotifyCount++;
    if (ctx->NotifyCount <= 8) {
        LogLine(ctx, L"  DSP notification: HIPCT=0x%08X cmd=0x%08X size=%u",
            hipct & HIPCT_MSG_MASK, hdr.cmd, hdr.size);
    }
    DspUpdate(ctx, DSP_REG_HIPCT, HIPCT_BUSY, HIPCT_BUSY);
}

// Send one IPC3 message and wait (polling) for the reply.
// Returns STATUS_SUCCESS only if the firmware replied with error == 0.
static NTSTATUS IpcTxLocked(PDEVICE_CONTEXT ctx, PCWSTR what, const VOID* msg, ULONG size,
    PVOID reply, ULONG replySize);

// Serialises IPC between the boot path and (later) audio stream callbacks.
static NTSTATUS IpcTx(PDEVICE_CONTEXT ctx, PCWSTR what, const VOID* msg, ULONG size,
    PVOID reply, ULONG replySize)
{
    ExAcquireFastMutex(&ctx->IpcLock);
    NTSTATUS st = IpcTxLocked(ctx, what, msg, size, reply, replySize);
    ExReleaseFastMutex(&ctx->IpcLock);
    return st;
}

static NTSTATUS IpcTxLocked(PDEVICE_CONTEXT ctx, PCWSTR what, const VOID* msg, ULONG size,
    PVOID reply, ULONG replySize)
{
    const SOF_IPC_CMD_HDR* hdr = (const SOF_IPC_CMD_HDR*)msg;
    if (!ctx->FwRunning || !ctx->HostBoxSize) return STATUS_DEVICE_NOT_READY;
    if (size > ctx->HostBoxSize || hdr->size != size) {
        LogLine(ctx, L"IPC %s: bad size %u (hdr %u, box %u)", what, size, hdr->size, ctx->HostBoxSize);
        return STATUS_INVALID_PARAMETER;
    }

    IpcServiceNotifications(ctx);

    // previous message must have been consumed
    NTSTATUS st = DspPoll(ctx, DSP_REG_HIPCI, HIPCI_BUSY, 0, 10000, NULL);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"IPC %s: HIPCI still busy", what);
        return STATUS_DEVICE_BUSY;
    }

    MboxWrite(ctx, ctx->HostBoxOff, msg, size);
    KeMemoryBarrier();
    DspWrite(ctx, DSP_REG_HIPCI, (hdr->cmd & 0x7FFFFFFFu) | HIPCI_BUSY);
    ctx->IpcCount++;

    ULONG v = 0;
    st = DspPoll(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE, 500000, &v);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"IPC %s (cmd 0x%08X): reply timeout, HIPCI=0x%08X HIPCIE=0x%08X",
            what, hdr->cmd, DspRead(ctx, DSP_REG_HIPCI), v);
        IpcServiceNotifications(ctx);
        return STATUS_IO_TIMEOUT;
    }

    SOF_IPC_REPLY r;
    MboxRead(ctx, ctx->HostBoxOff, &r, sizeof(r));
    if (reply && replySize) {
        ULONG n = min(replySize, (ULONG)min(r.hdr.size, ctx->HostBoxSize));
        MboxRead(ctx, ctx->HostBoxOff, reply, n);
    }

    // acknowledge the reply (set DONE to clear it)
    DspUpdate(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE);

    IpcServiceNotifications(ctx);

    LogLine(ctx, L"IPC %s: reply error %d", what, r.error);
    return (r.error == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------------------
// Endpoints, from sof-apl-da7219.tplg (decoded, see README):
//   speaker   : pipeline 1  PCM0P -> BUF1.0 -> PGA1.0 -> BUF1.1 -> SSP5 (MAX98357A)
//               SSP5: I2S, 19.2 MHz MCLK, 1.536 MHz BCLK, 2 x 16-bit slots
//   headphone : pipeline 2  PCM1P -> BUF2.0 -> PGA2.0 -> BUF2.1 -> SSP1 (DA7219)
//               SSP1: I2S, 19.2 MHz MCLK, 1.920 MHz BCLK, 2 x 20-bit slots
//   mic       : pipeline 4  DMIC0 -> BUF -> PGA -> BUF -> PCM99C (capture)
//               DMIC0: PDM controller 0, mics A+B, 2.4-4.8 MHz clock,
//               40-60 % duty, 48 kHz, 32-bit FIFO, 400 ms unmute ramp.
//               The Linux topology has no volume on this pipeline; we add
//               one (as the SOF pipe-volume-capture topologies do) so Windows
//               mic level/mute work.
// All: 1 ms timer-scheduled pipeline, 768-byte buffers, s32le volume.
// Playback: 16-bit DAI, codec consumes BCLK/FSYNC. Capture: 32-bit DMIC.
// ---------------------------------------------------------------------------

typedef struct _EP_DESC {
    PCWSTR Name;
    UINT32 PipeId;
    UINT32 CompBase;        // host +0, buf0 +1, pga +2, buf1 +3, dai +4, pipe +5
    UINT32 Direction;       // SOF_IPC_STREAM_PLAYBACK / _CAPTURE
    UINT32 DaiType;         // SOF_DAI_INTEL_SSP / _DMIC
    UINT32 DaiIndex;        // SSP port or DMIC FIFO
    UINT32 BclkRate;        // SSP only
    UINT16 SlotWidth;       // SSP only
    UINT32 FrameFmt;        // DAI and host stream format (SOF_IPC_FRAME_*)
    UINT32 SampleBytes;     // container bytes per sample in the host buffer
    USHORT HdaFormat;       // HDA SDnFMT for the host DMA stream
    ULONG MaxGain;          // Q1.16 gain at 100% Windows volume (safety cap)
    ULONG InitialGain;      // Q1.16 gain set when the pipeline is created
    UINT32 Ramp;            // volume ramp type
    UINT32 RampMs;          // volume ramp length
    INT32 CsAudioType;      // CoolStar endpoint type for amp/codec drivers, -1 = none
} EP_DESC;

#define EP_HOST(e)  (g_Ep[e].CompBase + 0)
#define EP_BUF0(e)  (g_Ep[e].CompBase + 1)
#define EP_PGA(e)   (g_Ep[e].CompBase + 2)
#define EP_BUF1(e)  (g_Ep[e].CompBase + 3)
#define EP_DAI(e)   (g_Ep[e].CompBase + 4)
#define EP_PIPE(e)  (g_Ep[e].CompBase + 5)

// CoolStar endpoint types (\Callback\CsAudioCallbackAPI)
enum { CSAudioEndpointTypeDSP = 0, CSAudioEndpointTypeSpeaker = 1, CSAudioEndpointTypeHeadphone = 2 };
enum { CSAudioEndpointRegister = 0, CSAudioEndpointStart = 1, CSAudioEndpointStop = 2 };

// HDA SDnFMT: 48 kHz base rate, BITS (6:4) 1 = 16-bit / 4 = 32-bit, channels - 1
#define HDA_FMT_48K_16BIT_2CH  0x0011
#define HDA_FMT_48K_32BIT_2CH  0x0041

// Playback volume right after the pipeline is created: -40 dB
#define EP_INITIAL_GAIN_PLAYBACK 655u

static const EP_DESC g_Ep[SOFAPL_EP_COUNT] = {
    // speaker: up to -6 dB in the DSP
    { L"speaker",   1, 1,  SOF_IPC_STREAM_PLAYBACK, SOF_DAI_INTEL_SSP, 5, 1536000, 16,
      SOF_IPC_FRAME_S16_LE, 2, HDA_FMT_48K_16BIT_2CH,
      32768, EP_INITIAL_GAIN_PLAYBACK, SOF_VOLUME_LINEAR_ZC, 20, CSAudioEndpointTypeSpeaker },
    // headphone: the DA7219 HP amp is set to +6 dB by its driver, so cap the
    // DSP much lower for ear safety: up to -18 dB
    { L"headphone", 2, 11, SOF_IPC_STREAM_PLAYBACK, SOF_DAI_INTEL_SSP, 1, 1920000, 20,
      SOF_IPC_FRAME_S16_LE, 2, HDA_FMT_48K_16BIT_2CH,
      8250,  EP_INITIAL_GAIN_PLAYBACK, SOF_VOLUME_LINEAR_ZC, 20, CSAudioEndpointTypeHeadphone },
    // internal mic: DMIC data is quiet, so allow up to +20 dB (10x) of digital
    // gain at 100% Windows mic level; start at 0 dB.
    { L"mic",       4, 21, SOF_IPC_STREAM_CAPTURE,  SOF_DAI_INTEL_DMIC, 0, 0, 0,
      SOF_IPC_FRAME_S32_LE, 4, HDA_FMT_48K_32BIT_2CH,
      655360, SOF_VOL_0DB, SOF_VOLUME_LINEAR, 250, -1 },
};

static const UINT8 UUID_HOST[16] = { 0x0c,0x10,0x9d,0x8b,0x78,0x6d,0x8f,0x41,0x90,0xa3,0xe0,0xe8,0x05,0xd0,0x85,0x2b };
static const UINT8 UUID_PGA[16]  = { 0x7e,0x67,0x7e,0xb7,0xf4,0x5f,0x88,0x41,0xaf,0x14,0xfb,0xa8,0xbd,0xbf,0x86,0x82 };
static const UINT8 UUID_DAI[16]  = { 0x27,0x0d,0xb0,0xc2,0xbc,0xff,0x50,0x41,0xa5,0x1a,0x24,0x5c,0x79,0xc5,0xe5,0x4b };

static void InitComp(SOF_IPC_COMP* c, ULONG totalSize, ULONG id, ULONG type, ULONG pipeId, BOOLEAN hasUuid)
{
    c->hdr.size = totalSize;
    c->hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_NEW;
    c->id = id;
    c->type = type;
    c->pipeline_id = pipeId;
    c->core = 0;
    c->ext_data_length = hasUuid ? sizeof(SOF_IPC_COMP_EXT) : 0;
}

static NTSTATUS IpcConnect(PDEVICE_CONTEXT ctx, ULONG src, ULONG sink)
{
    SOF_IPC_PIPE_COMP_CONNECT m;
    RtlZeroMemory(&m, sizeof(m));
    m.hdr.size = sizeof(m);
    m.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_CONNECT;
    m.source_id = src;
    m.sink_id = sink;
    WCHAR what[32];
    RtlStringCchPrintfW(what, ARRAYSIZE(what), L"connect %u->%u", src, sink);
    return IpcTx(ctx, what, &m, sizeof(m), NULL, 0);
}

// Raw DSP gain (Q1.16) for endpoint e.
static NTSTATUS EpSetGain(PDEVICE_CONTEXT ctx, ULONG e, ULONG gainQ16)
{
    SOF_IPC_CTRL_DATA_2CH m;
    RtlZeroMemory(&m, sizeof(m));
    m.rhdr.hdr.size = sizeof(m);
    m.rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | SOF_IPC_COMP_SET_VALUE;
    m.comp_id = EP_PGA(e);
    m.type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
    m.cmd = SOF_CTRL_CMD_VOLUME;
    m.index = 0;
    m.num_elems = 2;
    for (UINT32 i = 0; i < 2; i++) {
        m.chanv[i].channel = i;
        m.chanv[i].value = gainQ16;
    }
    WCHAR what[32];
    RtlStringCchPrintfW(what, ARRAYSIZE(what), L"%s volume", g_Ep[e].Name);
    return IpcTx(ctx, what, &m, sizeof(m), NULL, 0);
}

// Creates pipeline e in the firmware. Nothing is started: no PCM params, no
// trigger, SSP clocks are not requested early (clks_control = 0).
static NTSTATUS EpSetupPipeline(PDEVICE_CONTEXT ctx, ULONG e)
{
    const EP_DESC* d = &g_Ep[e];
    const BOOLEAN capture = (d->Direction == SOF_IPC_STREAM_CAPTURE);
    NTSTATUS st;

    // 1. pipeline, scheduled by the DAI component
    {
        SOF_IPC_PIPE_NEW m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_NEW;
        m.comp_id = EP_PIPE(e);
        m.pipeline_id = d->PipeId;
        m.sched_id = EP_DAI(e);
        m.core = 0;
        m.period = 1000;
        m.priority = 0;
        m.period_mips = 5000;
        m.frames_per_sched = 0;
        m.xrun_limit_usecs = 0;
        m.time_domain = SOF_TIME_DOMAIN_TIMER;
        st = IpcTx(ctx, L"pipe new", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 2. host component
    {
        SOF_IPC_COMP_HOST m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), EP_HOST(e), SOF_COMP_HOST, d->PipeId, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = capture ? 0 : 2;
        m.config.periods_source = capture ? 2 : 0;
        m.config.frame_fmt = SOF_IPC_FRAME_S16_LE;   // no format token: 0
        m.direction = d->Direction;
        RtlCopyMemory(m.ext.uuid, UUID_HOST, 16);
        st = IpcTx(ctx, L"host comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 3. buffers
    for (int b = 0; b < 2; b++) {
        SOF_IPC_BUFFER m;
        RtlZeroMemory(&m, sizeof(m));
        m.comp.hdr.size = sizeof(m);
        m.comp.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_BUFFER_NEW;
        m.comp.id = (b == 0) ? EP_BUF0(e) : EP_BUF1(e);
        m.comp.type = SOF_COMP_BUFFER;
        m.comp.pipeline_id = d->PipeId;
        m.comp.core = 0;
        m.size = 768;
        m.caps = 113;   // RAM | HP | DMA | CACHE
        st = IpcTx(ctx, (b == 0) ? L"buffer 0" : L"buffer 1", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 4. volume
    {
        SOF_IPC_COMP_VOLUME m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), EP_PGA(e), SOF_COMP_VOLUME, d->PipeId, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = 2;
        m.config.periods_source = 2;
        m.config.frame_fmt = SOF_IPC_FRAME_S32_LE;
        m.channels = 2;
        m.min_value = 0;
        m.max_value = max(d->MaxGain, (ULONG)SOF_VOL_0DB);
        m.ramp = d->Ramp;
        m.initial_ramp = d->RampMs;
        RtlCopyMemory(m.ext.uuid, UUID_PGA, 16);
        st = IpcTx(ctx, L"volume comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 5. DAI component (SSPn playback or DMIC capture)
    {
        SOF_IPC_COMP_DAI m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), EP_DAI(e), SOF_COMP_DAI, d->PipeId, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = capture ? 2 : 0;
        m.config.periods_source = capture ? 0 : 2;
        m.config.frame_fmt = d->FrameFmt;
        m.direction = d->Direction;
        m.dai_index = d->DaiIndex;
        m.type = d->DaiType;
        RtlCopyMemory(m.ext.uuid, UUID_DAI, 16);
        st = IpcTx(ctx, L"dai comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 6. graph: host -> buf0 -> pga -> buf1 -> dai for playback, reversed
    //    (dai -> buf1 -> pga -> buf0 -> host) for capture
    if (!capture) {
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_HOST(e), EP_BUF0(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_BUF0(e), EP_PGA(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_PGA(e), EP_BUF1(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_BUF1(e), EP_DAI(e)))) return st;
    } else {
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_DAI(e), EP_BUF1(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_BUF1(e), EP_PGA(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_PGA(e), EP_BUF0(e)))) return st;
        if (!NT_SUCCESS(st = IpcConnect(ctx, EP_BUF0(e), EP_HOST(e)))) return st;
    }

    // 7. pipeline complete
    {
        SOF_IPC_PIPE_READY m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_COMPLETE;
        m.comp_id = EP_PIPE(e);
        st = IpcTx(ctx, L"pipe complete", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 8. initial volume (low for playback, before anything can ever play)
    st = EpSetGain(ctx, e, d->InitialGain);
    if (!NT_SUCCESS(st)) return st;

    // 9a. DMIC configuration (sof_link_dmic_load): driver version 1, PDM
    //     controller 0 with both mics, values from the dmic01 topology tuples.
    if (d->DaiType == SOF_DAI_INTEL_DMIC) {
        SOF_IPC_DAI_CONFIG_MSG m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
        m.type = SOF_DAI_INTEL_DMIC;
        m.dai_index = d->DaiIndex;
        m.format = 0;
        m.group_id = 0;
        m.flags = SOF_DAI_CONFIG_FLAGS_HW_PARAMS;
        m.dmic.hdr.size = sizeof(m.dmic);
        m.dmic.driver_ipc_version = 1;
        m.dmic.pdmclk_min = 2400000;
        m.dmic.pdmclk_max = 4800000;
        m.dmic.fifo_fs = 48000;
        m.dmic.fifo_bits = 32;
        m.dmic.duty_min = 40;
        m.dmic.duty_max = 60;
        m.dmic.num_pdm_active = 1;
        m.dmic.unmute_ramp_time = 400;
        m.dmic.pdm[0].hdr.size = sizeof(m.dmic.pdm[0]);
        m.dmic.pdm[0].id = 0;
        m.dmic.pdm[0].enable_mic_a = 1;
        m.dmic.pdm[0].enable_mic_b = 1;
        st = IpcTx(ctx, L"dmic config", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
        ctx->Ep[e].PipelineReady = TRUE;
        return STATUS_SUCCESS;
    }

    // 9b. SSP configuration: I2S, SSP provides BCLK/FSYNC, 19.2 MHz MCLK,
    //     2 slots. clks_control = 0: no early clocks.
    {
        SOF_IPC_DAI_CONFIG_MSG m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
        m.type = SOF_DAI_INTEL_SSP;
        m.dai_index = d->DaiIndex;
        m.format = SOF_DAI_FMT_I2S | SOF_DAI_FMT_NB_NF | SOF_DAI_FMT_CBC_CFC;
        m.group_id = 0;
        m.flags = SOF_DAI_CONFIG_FLAGS_HW_PARAMS;
        m.ssp.mclk_id = 0;
        m.ssp.mclk_rate = 19200000;
        m.ssp.fsync_rate = 48000;
        m.ssp.bclk_rate = d->BclkRate;
        m.ssp.tdm_slots = 2;
        m.ssp.rx_slots = 3;
        m.ssp.tx_slots = 3;
        m.ssp.sample_valid_bits = 16;
        m.ssp.tdm_slot_width = d->SlotWidth;
        m.ssp.mclk_direction = 1;   // codec MCLK in
        m.ssp.frame_pulse_width = 0;
        m.ssp.tdm_per_slot_padding_flag = 0;
        m.ssp.clks_control = 0;
        m.ssp.quirks = 0;
        m.ssp.bclk_delay = 0;
        st = IpcTx(ctx, L"dai config", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    ctx->Ep[e].PipelineReady = TRUE;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Stream API per endpoint.
//
// Order follows Linux sof_pcm_hw_params / sof_pcm_trigger for IPC3 on HDA:
//   open:  HDA host stream setup (APL PROCEN/format quirk) -> STREAM_PCM_PARAMS
//   start: HDA host DMA run -> STREAM_TRIG_START -> codec/amp start notification
//   stop:  codec/amp stop notification -> STREAM_TRIG_STOP -> HDA host DMA stop
//   close: STREAM_PCM_FREE -> release HDA stream
// Only 48 kHz / stereo; S16_LE for playback, S32_LE for the DMIC capture.
// ---------------------------------------------------------------------------

#define EP_RATE        48000
#define EP_CHANNELS    2

// Intel vendor-specific DPIB registers (include/sound/hda_register.h):
// per-stream "DMA position in buffer" that reflects data actually transferred.
#define AZX_REG_VS_SDXDPIB_XBASE      0x1084
#define AZX_REG_VS_SDXDPIB_XINTERVAL  0x20

typedef struct _CS_AUDIO_FORMAT_OVERRIDE {
    UINT16 channels;
    UINT16 frequency;
    UINT16 bitsPerSample;
    UINT16 validBitsPerSample;
    INT32 force32BitOutputContainer;   // Win32 BOOL
} CS_AUDIO_FORMAT_OVERRIDE;

// CoolStar audio callback argument, layout from coolstar/max98357a driver.h
// (Apache-2.0). The amp driver drives the MAX98357A SD_MODE pin high on a
// Speaker "Start" and low on "Stop".
typedef struct _CS_AUDIO_ARG {
    UINT32 argSz;
    INT32 endpointType;                // enum (int)
    INT32 endpointRequest;             // enum (int)
    union {
        CS_AUDIO_FORMAT_OVERRIDE formatOverride;
    };
} CS_AUDIO_ARG;

// Identifies our own notifications (receivers compare Argument2 pointers).
static int g_SofCsAudioTag = 1;

static void CsAudioNotify(PDEVICE_CONTEXT ctx, INT32 endpointType, INT32 request)
{
    if (!ctx->CsAudioCallback || endpointType < 0) return;
    CS_AUDIO_ARG arg;
    RtlZeroMemory(&arg, sizeof(arg));
    arg.argSz = sizeof(arg);
    arg.endpointType = endpointType;
    arg.endpointRequest = request;
    ExNotifyCallback(ctx->CsAudioCallback, &arg, &g_SofCsAudioTag);
}

static void EpStreamRelease(PDEVICE_CONTEXT ctx, ULONG e)
{
    EP_STATE* s = &ctx->Ep[e];
    if (!s->Stream) return;
    ctx->Bus.CleanupDSP(ctx->Bus.Context, s->Stream);
    NTSTATUS st = ctx->Bus.FreeStream(ctx->Bus.Context, s->Stream);
    if (!NT_SUCCESS(st)) LogLine(ctx, L"%s FreeStream failed: 0x%08X", g_Ep[e].Name, st);
    s->Stream = NULL;
    s->Running = FALSE;
    s->Paused = FALSE;
    s->ParamsSet = FALSE;
}

// bufPhys must be physically contiguous, 128-byte aligned; size multiple of 256.
static NTSTATUS EpStreamOpen(PDEVICE_CONTEXT ctx, ULONG e, PHYSICAL_ADDRESS bufPhys, ULONG size, ULONG periodBytes)
{
    EP_STATE* s = &ctx->Ep[e];
    const EP_DESC* d = &g_Ep[e];
    const BOOLEAN capture = (d->Direction == SOF_IPC_STREAM_CAPTURE);
    if (!s->PipelineReady) return STATUS_DEVICE_NOT_READY;
    if (s->Stream) return STATUS_DEVICE_BUSY;
    if (size < 512 || (size % 256) != 0 || (bufPhys.LowPart & 127)) return STATUS_INVALID_PARAMETER;

    HDAUDIO_STREAM_FORMAT fmt;
    RtlZeroMemory(&fmt, sizeof(fmt));
    fmt.SampleRate = EP_RATE;
    fmt.ValidBitsPerSample = (USHORT)(d->SampleBytes * 8);
    fmt.ContainerSize = (USHORT)(d->SampleBytes * 8);
    fmt.NumberOfChannels = EP_CHANNELS;

    NTSTATUS st = capture
        ? ctx->Bus.GetCaptureStream(ctx->Bus.Context, fmt, &s->Stream, &s->Tag)
        : ctx->Bus.GetRenderStream(ctx->Bus.Context, fmt, &s->Stream, &s->Tag);
    if (!NT_SUCCESS(st)) {
        s->Stream = NULL;
        LogLine(ctx, L"%s Get%sStream failed: 0x%08X", d->Name, capture ? L"Capture" : L"Render", st);
        return st;
    }

    PVOID bdlPtr = NULL;
    st = ctx->Bus.PrepareDSP(ctx->Bus.Context, s->Stream, size, 2, &bdlPtr);
    if (!NT_SUCCESS(st) || !bdlPtr) {
        LogLine(ctx, L"%s PrepareDSP failed: 0x%08X", g_Ep[e].Name, st);
        EpStreamRelease(ctx, e);
        return NT_SUCCESS(st) ? STATUS_UNSUCCESSFUL : st;
    }

    ULONG half = size / 2;   // size % 256 == 0 -> half is 128-aligned
    PHDAC_BDLENTRY bdl = (PHDAC_BDLENTRY)bdlPtr;
    bdl[0].lowAddr = bufPhys.LowPart;
    bdl[0].highAddr = (UINT32)bufPhys.HighPart;
    bdl[0].len = half;
    bdl[0].ioc = 0;
    bdl[1].lowAddr = bufPhys.LowPart + half;
    bdl[1].highAddr = (UINT32)bufPhys.HighPart;
    bdl[1].len = size - half;
    bdl[1].ioc = 0;
    KeMemoryBarrier();

    // Apollo Lake: program the format with host/link DMA coupled.
    // Stream descriptors: capture streams first (index = tag - 1), then
    // playback streams (index = ISS + tag - 1).
    USHORT gcap = READ_REGISTER_USHORT((PUSHORT)(ctx->HdaBar.Base.baseptr + HDA_REG_GCAP));
    s->Idx = (capture ? 0 : ((gcap >> 8) & 0xF)) + s->Tag - 1;
    PULONG ppctl = (PULONG)(ctx->Ppcap + PP_REG_PPCTL);
    PUSHORT sdFmt = (PUSHORT)(ctx->HdaBar.Base.baseptr + HDA_SD_BASE + HDA_SD_STRIDE * s->Idx + HDA_SD_FORMAT);
    WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) & ~PPCTL_PROCEN(s->Idx));
    WRITE_REGISTER_USHORT(sdFmt, d->HdaFormat);
    WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) | PPCTL_PROCEN(s->Idx));

    SOF_IPC_PCM_PARAMS m;
    RtlZeroMemory(&m, sizeof(m));
    m.hdr.size = sizeof(m);
    m.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;
    m.comp_id = EP_HOST(e);
    m.params.hdr.size = sizeof(m.params);
    m.params.buffer.phy_addr = 0;                 // no host page table on cAVS/HDA
    m.params.buffer.pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    m.params.buffer.size = size;
    m.params.direction = d->Direction;
    m.params.frame_fmt = d->FrameFmt;
    m.params.buffer_fmt = SOF_IPC_BUFFER_INTERLEAVED;
    m.params.rate = EP_RATE;
    m.params.stream_tag = s->Tag;
    m.params.channels = EP_CHANNELS;
    m.params.sample_valid_bytes = d->SampleBytes;
    m.params.sample_container_bytes = d->SampleBytes;
    m.params.host_period_bytes = periodBytes;
    m.params.no_stream_position = 1;              // position comes from the HDA registers

    SOF_IPC_PCM_PARAMS_REPLY reply;
    RtlZeroMemory(&reply, sizeof(reply));
    st = IpcTx(ctx, L"pcm params", &m, sizeof(m), &reply, sizeof(reply));
    if (!NT_SUCCESS(st)) {
        EpStreamRelease(ctx, e);
        return st;
    }
    s->ParamsSet = TRUE;
    s->Size = size;
    LogLine(ctx, L"%s stream open: %s tag %u index %u, %u bytes, period %u",
        d->Name, capture ? L"capture" : L"playback", s->Tag, s->Idx, size, periodBytes);
    return STATUS_SUCCESS;
}

static NTSTATUS EpStreamTrigger(PDEVICE_CONTEXT ctx, ULONG e, BOOLEAN start)
{
    EP_STATE* s = &ctx->Ep[e];
    if (!s->Stream || !s->ParamsSet) return STATUS_INVALID_DEVICE_STATE;

    SOF_IPC_STREAM m;
    RtlZeroMemory(&m, sizeof(m));
    m.hdr.size = sizeof(m);
    m.comp_id = EP_HOST(e);
    NTSTATUS st;

    // IPC3 semantics (Linux sof_pcm_trigger): START once after PCM_PARAMS,
    // PAUSE/RELEASE for run<->pause, STOP only when the stream is torn down
    // (after STOP the firmware needs new PCM_PARAMS; START again fails -EBUSY).
    if (start) {
        if (s->Running) return STATUS_SUCCESS;
        BOOLEAN resume = s->Paused;
        ctx->Bus.TriggerDSP(ctx->Bus.Context, s->Stream, TRUE);
        m.hdr.cmd = SOF_IPC_GLB_STREAM_MSG |
            (resume ? SOF_IPC_STREAM_TRIG_RELEASE : SOF_IPC_STREAM_TRIG_START);
        st = IpcTx(ctx, resume ? L"trigger release" : L"trigger start", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) {
            ctx->Bus.TriggerDSP(ctx->Bus.Context, s->Stream, FALSE);
            return st;
        }
        s->Running = TRUE;
        s->Paused = FALSE;
        // amp/codec on once clocks are running (the amp driver waits 5 ms itself)
        CsAudioNotify(ctx, g_Ep[e].CsAudioType, CSAudioEndpointStart);
    } else {
        if (!s->Running) return STATUS_SUCCESS;
        // amp/codec off before the clocks stop, to avoid a pop
        CsAudioNotify(ctx, g_Ep[e].CsAudioType, CSAudioEndpointStop);
        m.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_PAUSE;
        st = IpcTx(ctx, L"trigger pause", &m, sizeof(m), NULL, 0);
        // stop the host DMA even if the IPC failed
        ctx->Bus.TriggerDSP(ctx->Bus.Context, s->Stream, FALSE);
        s->Running = FALSE;
        s->Paused = NT_SUCCESS(st);
    }
    return st;
}

// Full stop before PCM_FREE (stream teardown).
static void EpStreamStopForClose(PDEVICE_CONTEXT ctx, ULONG e)
{
    EP_STATE* s = &ctx->Ep[e];
    if (!s->Stream || !s->ParamsSet) return;
    if (!s->Running && !s->Paused) return;   // never started: nothing to stop

    if (s->Running) {
        CsAudioNotify(ctx, g_Ep[e].CsAudioType, CSAudioEndpointStop);
    }
    SOF_IPC_STREAM m;
    RtlZeroMemory(&m, sizeof(m));
    m.hdr.size = sizeof(m);
    m.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_STOP;
    m.comp_id = EP_HOST(e);
    IpcTx(ctx, L"trigger stop", &m, sizeof(m), NULL, 0);
    ctx->Bus.TriggerDSP(ctx->Bus.Context, s->Stream, FALSE);
    s->Running = FALSE;
    s->Paused = FALSE;
}

// Linux (SOF_HDA_POSITION_QUIRK_USE_DPIB_REGISTERS, the default) reads the DPIB
// register for playback instead of the DDR position buffer. For capture it
// does a dummy DPIB read (flushes the DMA position) and then uses the position
// buffer, since DPIB is not accurate for capture. Any IRQL.
static ULONG EpStreamPosition(PDEVICE_CONTEXT ctx, ULONG e)
{
    EP_STATE* s = &ctx->Ep[e];
    if (!s->Stream || !ctx->HdaBar.Base.baseptr || !s->Size) return 0;
    ULONG off = AZX_REG_VS_SDXDPIB_XBASE + AZX_REG_VS_SDXDPIB_XINTERVAL * s->Idx;
    BOOLEAN dpibOk = (off + 4 <= ctx->HdaBar.Len);
    ULONG pos;
    if (g_Ep[e].Direction == SOF_IPC_STREAM_CAPTURE) {
        if (dpibOk) {
            (void)READ_REGISTER_ULONG((PULONG)(ctx->HdaBar.Base.baseptr + off));
        }
        pos = ctx->Bus.StreamPosition(ctx->Bus.Context, s->Stream);
    } else if (dpibOk) {
        pos = READ_REGISTER_ULONG((PULONG)(ctx->HdaBar.Base.baseptr + off));
    } else {
        pos = ctx->Bus.StreamPosition(ctx->Bus.Context, s->Stream);
    }
    if (pos >= s->Size) pos = 0;
    return pos;
}

static void EpStreamClose(PDEVICE_CONTEXT ctx, ULONG e)
{
    EP_STATE* s = &ctx->Ep[e];
    if (!s->Stream) return;
    EpStreamStopForClose(ctx, e);
    if (s->ParamsSet) {
        SOF_IPC_STREAM m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_FREE;
        m.comp_id = EP_HOST(e);
        IpcTx(ctx, L"pcm free", &m, sizeof(m), NULL, 0);
        s->ParamsSet = FALSE;
    }
    EpStreamRelease(ctx, e);
}

// ---------------------------------------------------------------------------
// Boot sequence
// ---------------------------------------------------------------------------

// cl_dsp_init() from hda-loader.c (cold boot path)
static NTSTATUS ClDspInit(PDEVICE_CONTEXT ctx, UINT8 streamTag)
{
    NTSTATUS st;
    ULONG v = 0;

    // step 1: power up all host-managed cores
    st = CorePowerUp(ctx, APL_CORE_MASK_ALL);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"  core power up failed, ADSPCS=0x%08X", DspRead(ctx, DSP_REG_ADSPCS));
        goto err;
    }

    // set all SSPs to clock consumer / codec provider mode
    for (ULONG i = 0; i < APL_SSP_COUNT; i++) {
        ULONG off = APL_SSP_BASE_OFFSET + i * SSP_DEV_MEM_SIZE + SSP_SSC1_OFFSET;
        DspUpdate(ctx, off, SSP_SET_CBP_CFP, SSP_SET_CBP_CFP);
    }

    // step 2: ROM_CONTROL command with the code loader stream tag
    DspWrite(ctx, DSP_REG_HIPCI,
        HIPCI_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((ULONG)(streamTag - 1) << 9));

    // step 3: take core 0 out of reset and run it
    st = CoreRun(ctx, APL_CORE_MASK_BOOT);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"  core run failed, ADSPCS=0x%08X", DspRead(ctx, DSP_REG_ADSPCS));
        goto err;
    }

    // step 4: wait for DONE from ROM
    st = DspPoll(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE, TIMEOUT_ROM_IPC_US, &v);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"  timeout waiting for ROM IPC DONE, HIPCIE=0x%08X ROM_STATUS=0x%08X",
            v, DspRead(ctx, SRAM_REG_ROM_STATUS));
        goto err;
    }
    DspUpdate(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE);

    // step 5: power down cores that are not needed
    st = CoreResetPowerDown(ctx, APL_CORE_MASK_ALL & ~APL_CORE_MASK_BOOT);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"  core 1 power down failed, ADSPCS=0x%08X", DspRead(ctx, DSP_REG_ADSPCS));
        goto err;
    }

    // step 6 (IPC interrupts) is skipped: this milestone polls instead.

    // step 7: wait for ROM init done
    st = DspPollRomState(ctx, FSR_STATE_INIT_DONE, TIMEOUT_ROM_INIT_US, &v);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"  timeout waiting for ROM INIT_DONE, ROM_STATUS=0x%08X ROM_ERROR=0x%08X",
            v, DspRead(ctx, SRAM_REG_ROM_ERROR));
        goto err;
    }
    return STATUS_SUCCESS;

err:
    CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);
    return st;
}

static NTSTATUS SofBootFirmware(PDEVICE_CONTEXT ctx, PCWSTR* stageOut)
{
    NTSTATUS st;
    HANDLE stream = NULL;
    UINT8 tag = 0;
    PVOID dmaBuf = NULL;
    ULONG v = 0;
    PCWSTR stage = L"start";
    ULONG payloadOffset = 0;
    ULONG payloadSize = 0;
    PVOID bdlPtr = NULL;
    HDAUDIO_STREAM_FORMAT fmt;
    BOOLEAN spibEnabled = FALSE;
    PUINT8 sdBase = NULL;

    // ---- validate resources ----
    stage = L"resources";
    if (!ctx->DspBar.Base.baseptr || ctx->DspBar.Len < MBOX_UPLINK_OFFSET + sizeof(SOF_IPC_FW_READY_MSG) ||
        !ctx->HdaBar.Base.baseptr || !ctx->Ppcap) {
        LogLine(ctx, L"Missing resources: HDA=%p DSP=%p (len 0x%X) PPCAP=%p",
            ctx->HdaBar.Base.Base, ctx->DspBar.Base.Base, ctx->DspBar.Len, ctx->Ppcap);
        st = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto out;
    }

    // ---- firmware payload ----
    stage = L"firmware-file";
    st = LoadFirmwareFile(ctx);
    if (!NT_SUCCESS(st)) goto out;

    if (ctx->FwFileSize >= sizeof(SOF_EXT_MAN_HEADER)) {
        SOF_EXT_MAN_HEADER* xm = (SOF_EXT_MAN_HEADER*)ctx->FwFile;
        if (xm->magic == SOF_EXT_MAN_MAGIC) {
            payloadOffset = xm->full_size;
        }
    }
    if (payloadOffset >= ctx->FwFileSize) {
        LogLine(ctx, L"Bad firmware: payload offset %u >= size %u", payloadOffset, ctx->FwFileSize);
        st = STATUS_INVALID_IMAGE_FORMAT;
        goto out;
    }
    payloadSize = ctx->FwFileSize - payloadOffset;
    ParseFwExtManifest(ctx, payloadOffset);
    LogLine(ctx, L"Firmware payload: offset 0x%X, %u bytes, signature '%c%c%c%c'",
        payloadOffset, payloadSize,
        ctx->FwFile[payloadOffset], ctx->FwFile[payloadOffset + 1],
        ctx->FwFile[payloadOffset + 2], ctx->FwFile[payloadOffset + 3]);

    // ---- initial state ----
    LogLine(ctx, L"Initial ADSPCS=0x%08X ROM_STATUS=0x%08X PPCTL=0x%08X GCAP=0x%04X",
        DspRead(ctx, DSP_REG_ADSPCS), DspRead(ctx, SRAM_REG_ROM_STATUS),
        READ_REGISTER_ULONG((PULONG)(ctx->Ppcap + PP_REG_PPCTL)),
        READ_REGISTER_USHORT((PUSHORT)(ctx->HdaBar.Base.baseptr + HDA_REG_GCAP)));

    // make sure the cores start from a known (off) state
    CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);

    // enable DSP processing pipes (hda_dsp_ctrl_ppcap_enable)
    {
        PULONG ppctl = (PULONG)(ctx->Ppcap + PP_REG_PPCTL);
        WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) | PPCTL_GPROCEN);
    }

    // ---- code loader stream ----
    stage = L"cl-stream";
    RtlZeroMemory(&fmt, sizeof(fmt));
    fmt.SampleRate = 48000;
    fmt.ValidBitsPerSample = 32;
    fmt.ContainerSize = 32;
    fmt.NumberOfChannels = 1;
    st = ctx->Bus.GetRenderStream(ctx->Bus.Context, fmt, &stream, &tag);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"GetRenderStream failed: 0x%08X", st);
        stream = NULL;
        goto out;
    }

    {
        PHYSICAL_ADDRESS low, high, skip;
        low.QuadPart = 0;
        high.QuadPart = 0xFFFFFFFF;  // keep it under 4 GB to be safe
        skip.QuadPart = 0;
        dmaBuf = MmAllocateContiguousMemorySpecifyCache(ROUND_TO_PAGES(payloadSize), low, high, skip, MmNonCached);
    }
    if (!dmaBuf) {
        LogLine(ctx, L"Could not allocate %u bytes of DMA memory", payloadSize);
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlCopyMemory(dmaBuf, ctx->FwFile + payloadOffset, payloadSize);

    st = ctx->Bus.PrepareDSP(ctx->Bus.Context, stream, payloadSize, 2, &bdlPtr);
    if (!NT_SUCCESS(st) || !bdlPtr) {
        LogLine(ctx, L"PrepareDSP failed: 0x%08X", st);
        if (NT_SUCCESS(st)) st = STATUS_UNSUCCESSFUL;
        goto out;
    }

    {
        // Two BDL entries (HDA requires LVI >= 1); split on a 128-byte boundary.
        PHYSICAL_ADDRESS phys = MmGetPhysicalAddress(dmaBuf);
        ULONG half = (payloadSize / 2) & ~127u;
        PHDAC_BDLENTRY bdl = (PHDAC_BDLENTRY)bdlPtr;
        bdl[0].lowAddr = phys.LowPart;
        bdl[0].highAddr = (UINT32)phys.HighPart;
        bdl[0].len = half;
        bdl[0].ioc = 0;
        bdl[1].lowAddr = phys.LowPart + half;
        bdl[1].highAddr = (UINT32)phys.HighPart;
        bdl[1].len = payloadSize - half;
        bdl[1].ioc = 0;
        KeMemoryBarrier();

        // Apollo Lake quirk (SOF_INTEL_PROCEN_FMT_QUIRK): program the stream
        // format while host and link DMA are coupled (PROCEN cleared).
        USHORT gcap = READ_REGISTER_USHORT((PUSHORT)(ctx->HdaBar.Base.baseptr + HDA_REG_GCAP));
        ULONG iss = (gcap >> 8) & 0xF;
        ULONG idx = iss + tag - 1;
        PULONG ppctl = (PULONG)(ctx->Ppcap + PP_REG_PPCTL);
        PUSHORT sdFmt = (PUSHORT)(ctx->HdaBar.Base.baseptr + HDA_SD_BASE + HDA_SD_STRIDE * idx + HDA_SD_FORMAT);
        WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) & ~PPCTL_PROCEN(idx));
        WRITE_REGISTER_USHORT(sdFmt, CL_STREAM_FORMAT);
        WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) | PPCTL_PROCEN(idx));

        // Enable SPIB with the full payload size so the DMA engine is allowed
        // to move the whole buffer (hda_dsp_stream_spib_config in Linux).
        ctx->Bus.DSPEnableSPIB(ctx->Bus.Context, stream, payloadSize);
        spibEnabled = TRUE;

        sdBase = ctx->HdaBar.Base.baseptr + HDA_SD_BASE + HDA_SD_STRIDE * idx;
        LogLine(ctx, L"Code loader stream: tag %u, index %u, DMA phys 0x%llX, format 0x%04X, SPIB %u",
            tag, idx, phys.QuadPart, READ_REGISTER_USHORT(sdFmt), payloadSize);
    }

    // ---- ROM init (with retries) ----
    stage = L"rom-init";
    for (int attempt = 1; attempt <= FW_BOOT_ATTEMPTS; attempt++) {
        st = ClDspInit(ctx, tag);
        LogLine(ctx, L"ROM init attempt %d: 0x%08X", attempt, st);
        if (NT_SUCCESS(st)) break;
    }
    if (!NT_SUCCESS(st)) goto out;

    // ---- DMA firmware into the DSP ----
    stage = L"fw-download";
    ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, TRUE);
    st = DspPollRomState(ctx, FSR_STATE_FW_ENTERED, TIMEOUT_BASEFW_US, &v);
    {
        // stream diagnostics: SD_CTL (24-bit) + SD_STS, LPIB, position buffer
        ULONG sdCtlSts = READ_REGISTER_ULONG((PULONG)(sdBase + 0x0));
        ULONG lpib = READ_REGISTER_ULONG((PULONG)(sdBase + 0x4));
        ULONG posbuf = ctx->Bus.StreamPosition(ctx->Bus.Context, stream);
        LogLine(ctx, L"After DMA: SD_CTL/STS=0x%08X LPIB=%u POSBUF=%u PPCTL=0x%08X",
            sdCtlSts, lpib, posbuf, READ_REGISTER_ULONG((PULONG)(ctx->Ppcap + PP_REG_PPCTL)));
    }
    ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, FALSE);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"Timeout waiting for FW_ENTERED, ROM_STATUS=0x%08X ROM_ERROR=0x%08X",
            v, DspRead(ctx, SRAM_REG_ROM_ERROR));
        CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);
        goto out;
    }
    LogLine(ctx, L"Firmware entered, ROM_STATUS=0x%08X", v);

    // ---- wait for FW_READY (polling HIPCT) ----
    stage = L"fw-ready";
    st = DspPoll(ctx, DSP_REG_HIPCT, HIPCT_BUSY, HIPCT_BUSY, TIMEOUT_FW_READY_US, &v);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"Timeout waiting for FW_READY, HIPCT=0x%08X ROM_STATUS=0x%08X",
            v, DspRead(ctx, SRAM_REG_ROM_STATUS));
        CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);
        goto out;
    }

    {
        ULONG msg = v & HIPCT_MSG_MASK;
        ULONG msgExt = DspRead(ctx, DSP_REG_HIPCTE);
        SOF_IPC_FW_READY_MSG ready;
        RtlZeroMemory(&ready, sizeof(ready));
        PUINT8 src = ctx->DspBar.Base.baseptr + MBOX_UPLINK_OFFSET;
        for (ULONG i = 0; i < sizeof(ready); i += 4) {
            *(PULONG)((PUINT8)&ready + i) = READ_REGISTER_ULONG((PULONG)(src + i));
        }

        LogLine(ctx, L"DSP message: HIPCT=0x%08X HIPCTE=0x%08X mailbox cmd=0x%08X size=%u",
            msg, msgExt, ready.hdr.cmd, ready.hdr.size);

        if ((ready.hdr.cmd & SOF_GLB_TYPE_MASK) == SOF_IPC_FW_READY) {
            if (!ctx->HostBoxSize || !ctx->DspBoxSize) {
                // Older firmware: windows follow FW_READY in the mailbox.
                ParseExtData(ctx, MBOX_UPLINK_OFFSET + sizeof(SOF_IPC_FW_READY_MSG));
            }
            CHAR tagStr[7] = { 0 };
            RtlCopyMemory(tagStr, ready.version.tag, 6);
            LogLine(ctx, L"FW_READY: SOF %u.%u.%u build %u tag '%S' ABI 0x%08X",
                ready.version.major, ready.version.minor, ready.version.micro,
                ready.version.build, tagStr, ready.version.abi_version);
            LogLine(ctx, L"Mailboxes: dspbox 0x%X (%u bytes), hostbox 0x%X (%u bytes)",
                ready.dspbox_offset, ready.dspbox_size, ready.hostbox_offset, ready.hostbox_size);
            ctx->FwRunning = (ctx->HostBoxSize != 0 && ctx->DspBoxSize != 0);
            st = ctx->FwRunning ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
            if (!ctx->FwRunning) {
                LogLine(ctx, L"No IPC mailbox windows found in FW_READY ext data");
            }
        } else {
            LogLine(ctx, L"First DSP message was not FW_READY");
            st = STATUS_UNEXPECTED_IO_ERROR;
        }

        // tell the DSP we consumed the message (clear BUSY)
        DspUpdate(ctx, DSP_REG_HIPCT, HIPCT_BUSY, HIPCT_BUSY);
    }
    stage = ctx->FwRunning ? L"running" : L"fw-ready";

out:
    if (stream) {
        if (spibEnabled) {
            ctx->Bus.DSPDisableSPIB(ctx->Bus.Context, stream);
        }
        ctx->Bus.CleanupDSP(ctx->Bus.Context, stream);
        NTSTATUS fst = ctx->Bus.FreeStream(ctx->Bus.Context, stream);
        if (!NT_SUCCESS(fst)) {
            LogLine(ctx, L"FreeStream failed: 0x%08X", fst);
        }
    }
    if (dmaBuf) {
        MmFreeContiguousMemorySpecifyCache(dmaBuf, ROUND_TO_PAGES(payloadSize), MmNonCached);
    }
    *stageOut = stage;
    return st;
}

// ---------------------------------------------------------------------------
// WDF callbacks
// ---------------------------------------------------------------------------

NTSTATUS SofEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST, WDFCMRESLIST)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    ctx->Log[0] = L'\0';
    LogLine(ctx, L"sofapl 0.7.0 starting (speaker + headphone + DMIC capture)");

    NTSTATUS st = WdfFdoQueryForInterface(Device, &GUID_ADSP_BUS_INTERFACE,
        (PINTERFACE)&ctx->Bus, sizeof(ADSP_BUS_INTERFACE), 1, NULL);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"Query ADSP bus interface failed: 0x%08X", st);
        SaveRegistry(ctx, L"bus-interface", st);
        return st;
    }
    if (ctx->Bus.Size < sizeof(ADSP_BUS_INTERFACE) || !ctx->Bus.GetResources) {
        LogLine(ctx, L"ADSP bus interface too small: %u", ctx->Bus.Size);
        SaveRegistry(ctx, L"bus-interface", STATUS_REVISION_MISMATCH);
        return STATUS_REVISION_MISMATCH;
    }
    ctx->BusValid = TRUE;

    BUS_INTERFACE_STANDARD pci;
    RtlZeroMemory(&pci, sizeof(pci));
    st = ctx->Bus.GetResources(ctx->Bus.Context, &ctx->HdaBar, &ctx->DspBar,
        (PVOID*)&ctx->Ppcap, &ctx->Nhlt, &pci);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"GetResources failed: 0x%08X", st);
        SaveRegistry(ctx, L"resources", st);
        return st;
    }

    LogLine(ctx, L"Controller 0x%04X, HDA BAR %p (0x%X), DSP BAR %p (0x%X), PPCAP %p, NHLT %p (%llu bytes)",
        ctx->Bus.CtlrDevId, ctx->HdaBar.Base.Base, ctx->HdaBar.Len,
        ctx->DspBar.Base.Base, ctx->DspBar.Len, ctx->Ppcap,
        ctx->Nhlt.nhlt, ctx->Nhlt.nhltSz);

    // Callback object shared with CoolStar codec/amp drivers.
    if (!ctx->CsAudioCallback) {
        UNICODE_STRING cbName;
        RtlInitUnicodeString(&cbName, L"\\Callback\\CsAudioCallbackAPI");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &cbName, OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);
        NTSTATUS cst = ExCreateCallback(&ctx->CsAudioCallback, &oa, TRUE, TRUE);
        if (!NT_SUCCESS(cst)) {
            ctx->CsAudioCallback = NULL;
        }
        LogLine(ctx, L"CsAudio callback object: 0x%08X", cst);
    }

    SaveRegistry(ctx, L"prepared", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS SofEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    if (ctx->CsAudioCallback) {
        ObDereferenceObject(ctx->CsAudioCallback);
        ctx->CsAudioCallback = NULL;
    }
    if (ctx->FwFile) {
        ExFreePoolWithTag(ctx->FwFile, SOFAPL_POOL_TAG);
        ctx->FwFile = NULL;
        ctx->FwFileSize = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SofEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    if (!ctx->BusValid) return STATUS_SUCCESS;

    NTSTATUS st = ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD0);
    if (NT_SUCCESS(st)) {
        ctx->PowerHeld = TRUE;
    } else {
        LogLine(ctx, L"SetDSPPowerState(D0) failed: 0x%08X", st);
    }

    PCWSTR stage = L"start";
    st = SofBootFirmware(ctx, &stage);
    LogLine(ctx, L"Boot result: %s (0x%08X)", stage, st);

    if (NT_SUCCESS(st) && ctx->FwRunning) {
        // Build the playback pipelines in the firmware. Silent: no stream
        // parameters and no trigger are sent here. Streams are opened by the
        // audio driver through the SOFAPL\Speaker child device.
        for (ULONG e = 0; e < SOFAPL_EP_COUNT; e++) {
            NTSTATUS est = EpSetupPipeline(ctx, e);
            LogLine(ctx, L"Pipeline %u (%s): 0x%08X", g_Ep[e].PipeId, g_Ep[e].Name, est);
            if (e == SOFAPL_EP_SPEAKER) st = est;   // the speaker decides the stage
        }
        stage = NT_SUCCESS(st) ? L"pipelines-ready" : L"speaker-pipeline-failed";
        LogLine(ctx, L"Pipelines: %s (0x%08X), headphone %s, mic %s, %u IPCs, %u notifications",
            stage, st, ctx->Ep[SOFAPL_EP_HEADPHONE].PipelineReady ? L"ready" : L"FAILED",
            ctx->Ep[SOFAPL_EP_MIC].PipelineReady ? L"ready" : L"FAILED",
            ctx->IpcCount, ctx->NotifyCount);
    }
    SaveRegistry(ctx, stage, st);

    // Never fail D0 entry for a DSP boot problem: keep the device started so
    // the log can be read and the driver can be updated without a reboot loop.
    return STATUS_SUCCESS;
}

NTSTATUS SofEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    if (!ctx->BusValid) return STATUS_SUCCESS;

    for (ULONG e = 0; e < SOFAPL_EP_COUNT; e++) {
        if (ctx->FwRunning) {
            EpStreamClose(ctx, e);
        } else {
            EpStreamRelease(ctx, e);
        }
        ctx->Ep[e].Running = FALSE;
        ctx->Ep[e].ParamsSet = FALSE;
        ctx->Ep[e].PipelineReady = FALSE;
    }

    if (ctx->DspBar.Base.baseptr) {
        CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);
    }
    ctx->FwRunning = FALSE;
    ctx->HostBoxOff = ctx->HostBoxSize = 0;
    ctx->DspBoxOff = ctx->DspBoxSize = 0;

    if (ctx->PowerHeld) {
        ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD3);
        ctx->PowerHeld = FALSE;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SOFAPL\Speaker child device + interface for the audio driver
// ---------------------------------------------------------------------------

static NTSTATUS IfStreamOpen(PVOID Context, ULONG Endpoint, PHYSICAL_ADDRESS BufferPhys, ULONG BufferSize, ULONG PeriodBytes)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    if (Endpoint >= SOFAPL_EP_COUNT) return STATUS_INVALID_PARAMETER;
    if (!ctx->FwRunning || !ctx->Ep[Endpoint].PipelineReady) return STATUS_DEVICE_NOT_READY;
    if (BufferPhys.HighPart != 0) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = EpStreamOpen(ctx, Endpoint, BufferPhys, BufferSize, PeriodBytes);
    SaveRegistry(ctx, NT_SUCCESS(st) ? L"stream-open" : L"stream-open-failed", st);
    return st;
}

static NTSTATUS IfStreamTrigger(PVOID Context, ULONG Endpoint, BOOLEAN Start)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    if (Endpoint >= SOFAPL_EP_COUNT) return STATUS_INVALID_PARAMETER;
    if (!ctx->FwRunning) return STATUS_DEVICE_NOT_READY;
    NTSTATUS st = EpStreamTrigger(ctx, Endpoint, Start);
    LogLine(ctx, L"%s stream %s: 0x%08X, position %u", g_Ep[Endpoint].Name,
        Start ? L"start" : L"stop", st, EpStreamPosition(ctx, Endpoint));
    SaveRegistry(ctx, Start ? L"stream-running" : L"stream-stopped", st);
    return st;
}

static ULONG IfStreamPosition(PVOID Context, ULONG Endpoint)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    if (Endpoint >= SOFAPL_EP_COUNT) return 0;
    return EpStreamPosition(ctx, Endpoint);
}

static VOID IfStreamClose(PVOID Context, ULONG Endpoint)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    if (Endpoint >= SOFAPL_EP_COUNT) return;
    if (ctx->FwRunning) {
        EpStreamClose(ctx, Endpoint);
    } else {
        EpStreamRelease(ctx, Endpoint);
        ctx->Ep[Endpoint].Running = FALSE;
        ctx->Ep[Endpoint].ParamsSet = FALSE;
    }
    SaveRegistry(ctx, L"stream-closed", STATUS_SUCCESS);
}

// LevelQ16: 65536 = 100% = the endpoint's safe maximum gain.
static NTSTATUS IfSetVolume(PVOID Context, ULONG Endpoint, ULONG LevelQ16)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    if (Endpoint >= SOFAPL_EP_COUNT) return STATUS_INVALID_PARAMETER;
    if (!ctx->FwRunning || !ctx->Ep[Endpoint].PipelineReady) return STATUS_DEVICE_NOT_READY;
    if (LevelQ16 > 0x10000) LevelQ16 = 0x10000;
    ULONG gain = (ULONG)(((ULONGLONG)LevelQ16 * g_Ep[Endpoint].MaxGain) >> 16);
    return EpSetGain(ctx, Endpoint, gain);
}

static NTSTATUS CreateSpeakerChild(WDFDEVICE parent, PDEVICE_CONTEXT ctx)
{
    PWDFDEVICE_INIT init = WdfPdoInitAllocate(parent);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;

    DECLARE_CONST_UNICODE_STRING(hwId, L"SOFAPL\\Speaker");
    DECLARE_CONST_UNICODE_STRING(instId, L"0");
    DECLARE_CONST_UNICODE_STRING(desc, L"Chromebook Speakers (SOF)");

    NTSTATUS st = WdfPdoInitAssignDeviceID(init, &hwId);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddHardwareID(init, &hwId);
    if (NT_SUCCESS(st)) st = WdfPdoInitAssignInstanceID(init, &instId);
    if (NT_SUCCESS(st)) st = WdfPdoInitAddDeviceText(init, &desc, &desc, 0x409);
    if (!NT_SUCCESS(st)) {
        WdfDeviceInitFree(init);
        return st;
    }
    WdfPdoInitSetDefaultLocale(init, 0x409);

    WDFDEVICE child;
    st = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
    if (!NT_SUCCESS(st)) {
        // WdfDeviceCreate frees init on failure
        return st;
    }

    SOFAPL_AUDIO_INTERFACE iface;
    RtlZeroMemory(&iface, sizeof(iface));
    iface.Size = sizeof(iface);
    iface.Version = SOFAPL_AUDIO_INTERFACE_VERSION;
    iface.Context = ctx;
    iface.InterfaceReference = WdfDeviceInterfaceReferenceNoOp;
    iface.InterfaceDereference = WdfDeviceInterfaceDereferenceNoOp;
    iface.StreamOpen = IfStreamOpen;
    iface.StreamTrigger = IfStreamTrigger;
    iface.StreamPosition = IfStreamPosition;
    iface.StreamClose = IfStreamClose;
    iface.SetVolume = IfSetVolume;

    WDF_QUERY_INTERFACE_CONFIG qic;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&qic, (PINTERFACE)&iface, &GUID_SOFAPL_AUDIO_INTERFACE, NULL);
    st = WdfDeviceAddQueryInterface(child, &qic);
    if (!NT_SUCCESS(st)) {
        WdfObjectDelete(child);
        return st;
    }

    st = WdfFdoAddStaticChild(parent, child);
    if (!NT_SUCCESS(st)) {
        WdfObjectDelete(child);
    }
    return st;
}

NTSTATUS SofEvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SofEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = SofEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = SofEvtD0Entry;
    pnp.EvtDeviceD0Exit = SofEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);

    WDFDEVICE device;
    NTSTATUS st = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(st)) return st;

    PDEVICE_CONTEXT ctx = GetDeviceContext(device);
    ctx->Device = device;
    ExInitializeFastMutex(&ctx->IpcLock);

    // Child device for the Windows audio driver. Failure here is not fatal for
    // the DSP itself, so only log it.
    st = CreateSpeakerChild(device, ctx);
    if (!NT_SUCCESS(st)) {
        LogLine(ctx, L"Creating SOFAPL\\Speaker child failed: 0x%08X", st);
    }
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SofEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
