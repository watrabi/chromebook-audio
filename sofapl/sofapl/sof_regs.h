// Register and protocol definitions for the Intel cAVS 1.5 (Apollo Lake) audio DSP
// running Sound Open Firmware (IPC3).
//
// Values taken from the Linux kernel SOF driver (sound/soc/sof/intel/hda.h,
// include/sound/sof/header.h, include/sound/sof/info.h), which is
// dual-licensed GPL-2.0 / BSD-3-Clause; used here under BSD-3-Clause.
// Copyright(c) 2018 Intel Corporation.
#pragma once

// ---- HDA controller (BAR0) ----
#define HDA_REG_GCAP            0x00
#define HDA_SD_BASE             0x80
#define HDA_SD_STRIDE           0x20
#define HDA_SD_FORMAT           0x12

// ---- Processing pipe capability (PPCAP, in BAR0) ----
#define PP_REG_PPCTL            0x04
#define PPCTL_GPROCEN           (1u << 30)
#define PPCTL_PROCEN(idx)       (1u << (idx))

// ---- DSP (BAR4) general registers ----
#define DSP_REG_ADSPCS          0x04
#define DSP_REG_ADSPIC          0x08
#define DSP_REG_ADSPIS          0x0C

#define ADSPCS_CRST(cm)         ((cm) << 0)
#define ADSPCS_CSTALL(cm)       ((cm) << 8)
#define ADSPCS_SPA(cm)          ((cm) << 16)
#define ADSPCS_CPA(cm)          ((cm) << 24)

// ---- IPC registers (BAR4) ----
#define DSP_REG_HIPCT           0x40
#define DSP_REG_HIPCTE          0x44
#define DSP_REG_HIPCI           0x48
#define DSP_REG_HIPCIE          0x4C
#define DSP_REG_HIPCCTL         0x50

#define HIPCI_BUSY              (1u << 31)
#define HIPCIE_DONE             (1u << 30)
#define HIPCT_BUSY              (1u << 31)
#define HIPCT_MSG_MASK          0x7FFFFFFFu

// ---- ROM boot protocol ----
#define ROM_IPC_CONTROL         0x01000000u
#define ROM_IPC_PURGE_FW        0x00004000u

// ---- SRAM windows (BAR4) ----
#define SRAM_WINDOW_OFFSET(x)   (0x80000 + (x) * 0x20000)
#define MBOX_OFFSET             SRAM_WINDOW_OFFSET(0)
#define SRAM_REG_ROM_STATUS     (MBOX_OFFSET + 0x0)
#define SRAM_REG_ROM_ERROR      (MBOX_OFFSET + 0x4)
#define MBOX_UPLINK_OFFSET      0x81000

#define FSR_STATE_MASK          0x00FFFFFFu
#define FSR_HALTED              (1u << 31)
#define FSR_STATE_INIT_DONE     0x1
#define FSR_STATE_FW_ENTERED    0x5

// ---- SSP ports (BAR4) ----
#define APL_SSP_BASE_OFFSET     0x2000
#define APL_SSP_COUNT           6
#define SSP_DEV_MEM_SIZE        0x1000
#define SSP_SSC1_OFFSET         0x4
#define SSP_SET_CBP_CFP         ((1u << 25) | (1u << 24))

// ---- Apollo Lake chip description ----
#define APL_CORE_MASK_ALL       0x3
#define APL_CORE_MASK_BOOT      0x1

// ---- Timeouts (microseconds) ----
#define TIMEOUT_CORE_US         50000     // HDA_DSP_RESET_TIMEOUT_US / PD timeout
#define TIMEOUT_ROM_IPC_US      500000    // HDA_DSP_INIT_TIMEOUT_US
#define TIMEOUT_ROM_INIT_US     150000    // apl rom_init_timeout (150 ms)
#define TIMEOUT_BASEFW_US       3000000   // HDA_DSP_BASEFW_TIMEOUT_US
#define TIMEOUT_FW_READY_US     3000000
#define FW_BOOT_ATTEMPTS        3

// Code loader stream format: 48 kHz, 32-bit, mono (HDA_CL_STREAM_FORMAT 0x40)
#define CL_STREAM_FORMAT        0x40

// ---- SOF IPC3 ----
#define SOF_GLB_TYPE(x)         ((UINT32)(x) << 28)
#define SOF_GLB_TYPE_MASK       (0xFu << 28)
#define SOF_IPC_FW_READY        SOF_GLB_TYPE(0x7)

#define SOF_EXT_MAN_MAGIC       0x6e614d58u   // "XMan"

#pragma pack(push, 1)
typedef struct _SOF_IPC_HDR {
    UINT32 size;
} SOF_IPC_HDR;

typedef struct _SOF_IPC_CMD_HDR {
    UINT32 size;
    UINT32 cmd;
} SOF_IPC_CMD_HDR;

typedef struct _SOF_IPC_FW_VERSION {
    SOF_IPC_HDR hdr;
    UINT16 major;
    UINT16 minor;
    UINT16 micro;
    UINT16 build;
    UINT8 date[12];
    UINT8 time[10];
    UINT8 tag[6];
    UINT32 abi_version;
    UINT32 src_hash;
    UINT32 reserved[3];
} SOF_IPC_FW_VERSION;

typedef struct _SOF_IPC_FW_READY_MSG {
    SOF_IPC_CMD_HDR hdr;
    UINT32 dspbox_offset;
    UINT32 hostbox_offset;
    UINT32 dspbox_size;
    UINT32 hostbox_size;
    SOF_IPC_FW_VERSION version;
    UINT64 flags;
    UINT32 reserved[4];
} SOF_IPC_FW_READY_MSG;

typedef struct _SOF_EXT_MAN_HEADER {
    UINT32 magic;
    UINT32 full_size;
    UINT32 header_size;
    UINT32 header_version;
} SOF_EXT_MAN_HEADER;
#pragma pack(pop)
