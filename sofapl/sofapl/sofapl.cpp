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

#define SOFAPL_POOL_TAG 'LPAS'
#define FW_PATH L"\\SystemRoot\\System32\\drivers\\intel-sof-apl.ri"
#define FW_MAX_SIZE (4 * 1024 * 1024)
#define LOG_CHARS 8192

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
    ULONG IpcCount;
    ULONG NotifyCount;

    BOOLEAN SpeakerPipelineReady;

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
static void ParseExtData(PDEVICE_CONTEXT ctx, ULONG offset)
{
    for (int guard = 0; guard < 16; guard++) {
        if (offset + sizeof(SOF_IPC_EXT_DATA_HDR) > ctx->DspBar.Len) break;
        SOF_IPC_EXT_DATA_HDR eh;
        MboxRead(ctx, offset, &eh, sizeof(eh));
        if (eh.hdr.cmd != SOF_IPC_FW_READY || eh.hdr.size < sizeof(eh) || eh.hdr.size > 4096) {
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
static NTSTATUS IpcTx(PDEVICE_CONTEXT ctx, PCWSTR what, const VOID* msg, ULONG size,
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
// Speaker pipeline (pipeline 1 of sof-apl-da7219.tplg):
//   PCM0P (host) -> BUF1.0 -> PGA1.0 (volume) -> BUF1.1 -> SSP5.OUT (MAX98357A)
// Values come from the decoded Linux topology (see README).
// ---------------------------------------------------------------------------

#define SPK_PIPE_ID     1
#define SPK_COMP_HOST   1
#define SPK_COMP_BUF0   2
#define SPK_COMP_PGA    3
#define SPK_COMP_BUF1   4
#define SPK_COMP_DAI    5
#define SPK_COMP_PIPE   6

// Start the speaker volume very low: -40 dB (Q1.16 gain ~= 655)
#define SPK_INITIAL_GAIN 655u

static const UINT8 UUID_HOST[16] = { 0x0c,0x10,0x9d,0x8b,0x78,0x6d,0x8f,0x41,0x90,0xa3,0xe0,0xe8,0x05,0xd0,0x85,0x2b };
static const UINT8 UUID_PGA[16]  = { 0x7e,0x67,0x7e,0xb7,0xf4,0x5f,0x88,0x41,0xaf,0x14,0xfb,0xa8,0xbd,0xbf,0x86,0x82 };
static const UINT8 UUID_DAI[16]  = { 0x27,0x0d,0xb0,0xc2,0xbc,0xff,0x50,0x41,0xa5,0x1a,0x24,0x5c,0x79,0xc5,0xe5,0x4b };

static void InitComp(SOF_IPC_COMP* c, ULONG totalSize, ULONG id, ULONG type, BOOLEAN hasUuid)
{
    c->hdr.size = totalSize;
    c->hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_NEW;
    c->id = id;
    c->type = type;
    c->pipeline_id = SPK_PIPE_ID;
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

static NTSTATUS SetSpeakerVolume(PDEVICE_CONTEXT ctx, ULONG gainQ16)
{
    SOF_IPC_CTRL_DATA_2CH m;
    RtlZeroMemory(&m, sizeof(m));
    m.rhdr.hdr.size = sizeof(m);
    m.rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | SOF_IPC_COMP_SET_VALUE;
    m.comp_id = SPK_COMP_PGA;
    m.type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
    m.cmd = SOF_CTRL_CMD_VOLUME;
    m.index = 0;
    m.num_elems = 2;
    for (UINT32 i = 0; i < 2; i++) {
        m.chanv[i].channel = i;
        m.chanv[i].value = gainQ16;
    }
    return IpcTx(ctx, L"set volume", &m, sizeof(m), NULL, 0);
}

// Creates the pipeline in the firmware. Nothing is started: no PCM params,
// no trigger, SSP clocks are not requested (clks_control = 0).
static NTSTATUS SetupSpeakerPipeline(PDEVICE_CONTEXT ctx)
{
    NTSTATUS st;

    // 1. pipeline, scheduled by the DAI component
    {
        SOF_IPC_PIPE_NEW m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_NEW;
        m.comp_id = SPK_COMP_PIPE;
        m.pipeline_id = SPK_PIPE_ID;
        m.sched_id = SPK_COMP_DAI;
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

    // 2. host component (PCM0P)
    {
        SOF_IPC_COMP_HOST m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), SPK_COMP_HOST, SOF_COMP_HOST, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = 2;
        m.config.periods_source = 0;
        m.config.frame_fmt = SOF_IPC_FRAME_S16_LE;   // no format token: 0
        m.direction = SOF_IPC_STREAM_PLAYBACK;
        RtlCopyMemory(m.ext.uuid, UUID_HOST, 16);
        st = IpcTx(ctx, L"host comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 3. buffers
    for (ULONG id = SPK_COMP_BUF0; id <= SPK_COMP_BUF1; id += (SPK_COMP_BUF1 - SPK_COMP_BUF0)) {
        SOF_IPC_BUFFER m;
        RtlZeroMemory(&m, sizeof(m));
        m.comp.hdr.size = sizeof(m);
        m.comp.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_BUFFER_NEW;
        m.comp.id = id;
        m.comp.type = SOF_COMP_BUFFER;
        m.comp.pipeline_id = SPK_PIPE_ID;
        m.comp.core = 0;
        m.size = 768;
        m.caps = 113;   // RAM | HP | DMA | CACHE
        st = IpcTx(ctx, (id == SPK_COMP_BUF0) ? L"buffer 1.0" : L"buffer 1.1", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 4. volume (PGA1.0)
    {
        SOF_IPC_COMP_VOLUME m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), SPK_COMP_PGA, SOF_COMP_VOLUME, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = 2;
        m.config.periods_source = 2;
        m.config.frame_fmt = SOF_IPC_FRAME_S32_LE;
        m.channels = 2;
        m.min_value = 0;
        m.max_value = SOF_VOL_0DB;
        m.ramp = SOF_VOLUME_LINEAR_ZC;
        m.initial_ramp = 20;
        RtlCopyMemory(m.ext.uuid, UUID_PGA, 16);
        st = IpcTx(ctx, L"volume comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 5. DAI component (SSP5, playback)
    {
        SOF_IPC_COMP_DAI m;
        RtlZeroMemory(&m, sizeof(m));
        InitComp(&m.comp, sizeof(m), SPK_COMP_DAI, SOF_COMP_DAI, TRUE);
        m.config.hdr.size = sizeof(m.config);
        m.config.periods_sink = 0;
        m.config.periods_source = 2;
        m.config.frame_fmt = SOF_IPC_FRAME_S16_LE;
        m.direction = SOF_IPC_STREAM_PLAYBACK;
        m.dai_index = 5;
        m.type = SOF_DAI_INTEL_SSP;
        RtlCopyMemory(m.ext.uuid, UUID_DAI, 16);
        st = IpcTx(ctx, L"SSP5 dai comp", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 6. graph
    if (!NT_SUCCESS(st = IpcConnect(ctx, SPK_COMP_HOST, SPK_COMP_BUF0))) return st;
    if (!NT_SUCCESS(st = IpcConnect(ctx, SPK_COMP_BUF0, SPK_COMP_PGA))) return st;
    if (!NT_SUCCESS(st = IpcConnect(ctx, SPK_COMP_PGA, SPK_COMP_BUF1))) return st;
    if (!NT_SUCCESS(st = IpcConnect(ctx, SPK_COMP_BUF1, SPK_COMP_DAI))) return st;

    // 7. pipeline complete
    {
        SOF_IPC_PIPE_READY m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_COMPLETE;
        m.comp_id = SPK_COMP_PIPE;
        st = IpcTx(ctx, L"pipe complete", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    // 8. low volume before anything can ever play
    st = SetSpeakerVolume(ctx, SPK_INITIAL_GAIN);
    if (!NT_SUCCESS(st)) return st;

    // 9. SSP5 configuration (I2S, SSP provides BCLK/FSYNC, 19.2 MHz MCLK,
    //    1.536 MHz BCLK, 2 x 16-bit slots). clks_control = 0: no early clocks.
    {
        SOF_IPC_DAI_CONFIG m;
        RtlZeroMemory(&m, sizeof(m));
        m.hdr.size = sizeof(m);
        m.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
        m.type = SOF_DAI_INTEL_SSP;
        m.dai_index = 5;
        m.format = SOF_DAI_FMT_I2S | SOF_DAI_FMT_NB_NF | SOF_DAI_FMT_CBC_CFC;
        m.group_id = 0;
        m.flags = SOF_DAI_CONFIG_FLAGS_HW_PARAMS;
        m.ssp.mclk_id = 0;
        m.ssp.mclk_rate = 19200000;
        m.ssp.fsync_rate = 48000;
        m.ssp.bclk_rate = 1536000;
        m.ssp.tdm_slots = 2;
        m.ssp.rx_slots = 3;
        m.ssp.tx_slots = 3;
        m.ssp.sample_valid_bits = 16;
        m.ssp.tdm_slot_width = 16;
        m.ssp.mclk_direction = 1;   // codec MCLK in
        m.ssp.frame_pulse_width = 0;
        m.ssp.tdm_per_slot_padding_flag = 0;
        m.ssp.clks_control = 0;
        m.ssp.quirks = 0;
        m.ssp.bclk_delay = 0;
        st = IpcTx(ctx, L"SSP5 dai config", &m, sizeof(m), NULL, 0);
        if (!NT_SUCCESS(st)) return st;
    }

    ctx->SpeakerPipelineReady = TRUE;
    return STATUS_SUCCESS;
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
            ParseExtData(ctx, MBOX_UPLINK_OFFSET + sizeof(SOF_IPC_FW_READY_MSG));
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
    LogLine(ctx, L"sofapl 0.3.0 starting (milestone 3a: IPC + speaker pipeline, silent)");

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

    SaveRegistry(ctx, L"prepared", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS SofEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
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
        // Milestone 3a: build the speaker pipeline in the firmware. Silent:
        // no stream parameters and no trigger are ever sent here.
        st = SetupSpeakerPipeline(ctx);
        stage = NT_SUCCESS(st) ? L"speaker-pipeline-ready" : L"speaker-pipeline-failed";
        LogLine(ctx, L"Speaker pipeline: %s (0x%08X), %u IPCs, %u notifications",
            stage, st, ctx->IpcCount, ctx->NotifyCount);
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

    if (ctx->DspBar.Base.baseptr) {
        CoreResetPowerDown(ctx, APL_CORE_MASK_ALL);
    }
    ctx->FwRunning = FALSE;
    ctx->SpeakerPipelineReady = FALSE;
    ctx->HostBoxOff = ctx->HostBoxSize = 0;
    ctx->DspBoxOff = ctx->DspBoxSize = 0;

    if (ctx->PowerHeld) {
        ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD3);
        ctx->PowerHeld = FALSE;
    }
    return STATUS_SUCCESS;
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
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SofEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
