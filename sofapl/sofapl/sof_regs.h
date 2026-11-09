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

// ===========================================================================
// IPC3 messages used by milestone 3 (layouts from Linux include/sound/sof/*.h,
// BSD-3-Clause). All structures are packed.
// ===========================================================================

#define SOF_CMD_TYPE(x)                 ((UINT32)(x) << 16)
#define SOF_IPC_GLB_REPLY               SOF_GLB_TYPE(0x1)
#define SOF_IPC_GLB_TPLG_MSG            SOF_GLB_TYPE(0x3)
#define SOF_IPC_GLB_COMP_MSG            SOF_GLB_TYPE(0x5)
#define SOF_IPC_GLB_STREAM_MSG          SOF_GLB_TYPE(0x6)
#define SOF_IPC_GLB_DAI_MSG             SOF_GLB_TYPE(0x8)
#define SOF_IPC_GLB_TRACE_MSG           SOF_GLB_TYPE(0x9)

#define SOF_IPC_TPLG_COMP_NEW           SOF_CMD_TYPE(0x001)
#define SOF_IPC_TPLG_COMP_FREE          SOF_CMD_TYPE(0x002)
#define SOF_IPC_TPLG_COMP_CONNECT       SOF_CMD_TYPE(0x003)
#define SOF_IPC_TPLG_PIPE_NEW           SOF_CMD_TYPE(0x010)
#define SOF_IPC_TPLG_PIPE_FREE          SOF_CMD_TYPE(0x011)
#define SOF_IPC_TPLG_PIPE_COMPLETE      SOF_CMD_TYPE(0x013)
#define SOF_IPC_TPLG_BUFFER_NEW         SOF_CMD_TYPE(0x020)
#define SOF_IPC_TPLG_BUFFER_FREE        SOF_CMD_TYPE(0x021)
#define SOF_IPC_COMP_SET_VALUE          SOF_CMD_TYPE(0x001)
#define SOF_IPC_DAI_CONFIG              SOF_CMD_TYPE(0x001)

// component types
#define SOF_COMP_HOST                   1
#define SOF_COMP_DAI                    2
#define SOF_COMP_VOLUME                 5
#define SOF_COMP_BUFFER                 12

#define SOF_IPC_STREAM_PLAYBACK         0
#define SOF_IPC_STREAM_CAPTURE          1

#define SOF_IPC_FRAME_S16_LE            0
#define SOF_IPC_FRAME_S32_LE            2

#define SOF_TIME_DOMAIN_TIMER           1
#define SOF_VOLUME_LINEAR_ZC            2

#define SOF_DAI_INTEL_SSP               1
#define SOF_DAI_FMT_I2S                 1
#define SOF_DAI_FMT_NB_NF               (0 << 8)
#define SOF_DAI_FMT_CBC_CFC             (4 << 12)   // codec consumes BCLK + FSYNC
#define SOF_DAI_CONFIG_FLAGS_HW_PARAMS  (1 << 0)

#define SOF_CTRL_TYPE_VALUE_CHAN_SET    1
#define SOF_CTRL_CMD_VOLUME             0

// ext data / windows
#define SOF_IPC_EXT_WINDOW              1
#define SOF_IPC_REGION_DOWNBOX          0
#define SOF_IPC_REGION_UPBOX            1

// volume gain is Q1.16 (VOLUME_FWL = 16): 0 dB = 65536
#define SOF_VOL_0DB                     0x10000u

#pragma pack(push, 1)
typedef struct _SOF_IPC_REPLY {
    SOF_IPC_CMD_HDR hdr;
    INT32 error;
} SOF_IPC_REPLY;

typedef struct _SOF_IPC_EXT_DATA_HDR {
    SOF_IPC_CMD_HDR hdr;
    UINT32 type;
} SOF_IPC_EXT_DATA_HDR;

typedef struct _SOF_IPC_WINDOW_ELEM {
    SOF_IPC_HDR hdr;
    UINT32 type;
    UINT32 id;
    UINT32 flags;
    UINT32 size;
    UINT32 offset;
} SOF_IPC_WINDOW_ELEM;

typedef struct _SOF_IPC_COMP {
    SOF_IPC_CMD_HDR hdr;
    UINT32 id;
    UINT32 type;
    UINT32 pipeline_id;
    UINT32 core;
    UINT32 ext_data_length;
} SOF_IPC_COMP;

typedef struct _SOF_IPC_COMP_CONFIG {
    SOF_IPC_CMD_HDR hdr;
    UINT32 periods_sink;
    UINT32 periods_source;
    UINT32 reserved1;
    UINT32 frame_fmt;
    UINT32 xrun_action;
    UINT32 reserved[2];
} SOF_IPC_COMP_CONFIG;

typedef struct _SOF_IPC_COMP_EXT {
    UINT8 uuid[16];
} SOF_IPC_COMP_EXT;

typedef struct _SOF_IPC_BUFFER {
    SOF_IPC_COMP comp;
    UINT32 size;
    UINT32 caps;
    UINT32 flags;
    UINT32 reserved;
} SOF_IPC_BUFFER;

typedef struct _SOF_IPC_COMP_HOST {
    SOF_IPC_COMP comp;
    SOF_IPC_COMP_CONFIG config;
    UINT32 direction;
    UINT32 no_irq;
    UINT32 dmac_config;
    SOF_IPC_COMP_EXT ext;
} SOF_IPC_COMP_HOST;

typedef struct _SOF_IPC_COMP_DAI {
    SOF_IPC_COMP comp;
    SOF_IPC_COMP_CONFIG config;
    UINT32 direction;
    UINT32 dai_index;
    UINT32 type;
    UINT32 reserved;
    SOF_IPC_COMP_EXT ext;
} SOF_IPC_COMP_DAI;

typedef struct _SOF_IPC_COMP_VOLUME {
    SOF_IPC_COMP comp;
    SOF_IPC_COMP_CONFIG config;
    UINT32 channels;
    UINT32 min_value;
    UINT32 max_value;
    UINT32 ramp;
    UINT32 initial_ramp;
    SOF_IPC_COMP_EXT ext;
} SOF_IPC_COMP_VOLUME;

typedef struct _SOF_IPC_PIPE_NEW {
    SOF_IPC_CMD_HDR hdr;
    UINT32 comp_id;
    UINT32 pipeline_id;
    UINT32 sched_id;
    UINT32 core;
    UINT32 period;
    UINT32 priority;
    UINT32 period_mips;
    UINT32 frames_per_sched;
    UINT32 xrun_limit_usecs;
    UINT32 time_domain;
} SOF_IPC_PIPE_NEW;

typedef struct _SOF_IPC_PIPE_READY {
    SOF_IPC_CMD_HDR hdr;
    UINT32 comp_id;
} SOF_IPC_PIPE_READY;

typedef struct _SOF_IPC_PIPE_COMP_CONNECT {
    SOF_IPC_CMD_HDR hdr;
    UINT32 source_id;
    UINT32 sink_id;
} SOF_IPC_PIPE_COMP_CONNECT;

typedef struct _SOF_IPC_DAI_SSP_PARAMS {
    SOF_IPC_HDR hdr;
    UINT16 reserved1;
    UINT16 mclk_id;
    UINT32 mclk_rate;
    UINT32 fsync_rate;
    UINT32 bclk_rate;
    UINT32 tdm_slots;
    UINT32 rx_slots;
    UINT32 tx_slots;
    UINT32 sample_valid_bits;
    UINT16 tdm_slot_width;
    UINT16 reserved2;
    UINT32 mclk_direction;
    UINT16 frame_pulse_width;
    UINT16 tdm_per_slot_padding_flag;
    UINT32 clks_control;
    UINT32 quirks;
    UINT32 bclk_delay;
} SOF_IPC_DAI_SSP_PARAMS;

// The kernel's union is sized by the DMIC params (164 bytes); send the same size.
#define SOF_DAI_PARAMS_UNION_SIZE 164

typedef struct _SOF_IPC_DAI_CONFIG_MSG {
    SOF_IPC_CMD_HDR hdr;
    UINT32 type;
    UINT32 dai_index;
    UINT16 format;
    UINT8 group_id;
    UINT8 flags;
    UINT32 reserved[8];
    union {
        SOF_IPC_DAI_SSP_PARAMS ssp;
        UINT8 raw[SOF_DAI_PARAMS_UNION_SIZE];
    };
} SOF_IPC_DAI_CONFIG_MSG;

typedef struct _SOF_IPC_HOST_BUFFER {
    SOF_IPC_HDR hdr;
    UINT32 phy_addr;
    UINT32 pages;
    UINT32 size;
    UINT32 reserved[3];
} SOF_IPC_HOST_BUFFER;

typedef struct _SOF_IPC_CTRL_VALUE_CHAN {
    UINT32 channel;
    UINT32 value;
} SOF_IPC_CTRL_VALUE_CHAN;

typedef struct _SOF_IPC_CTRL_DATA_2CH {
    SOF_IPC_REPLY rhdr;
    UINT32 comp_id;
    UINT32 type;
    UINT32 cmd;
    UINT32 index;
    SOF_IPC_HOST_BUFFER buffer;
    UINT32 num_elems;
    UINT32 elems_remaining;
    UINT32 msg_index;
    UINT32 reserved[6];
    SOF_IPC_CTRL_VALUE_CHAN chanv[2];
} SOF_IPC_CTRL_DATA_2CH;
#pragma pack(pop)

// Compile-time checks against the kernel's sizes
static_assert(sizeof(SOF_IPC_COMP) == 28, "sof_ipc_comp");
static_assert(sizeof(SOF_IPC_COMP_CONFIG) == 36, "sof_ipc_comp_config");
static_assert(sizeof(SOF_IPC_BUFFER) == 44, "sof_ipc_buffer");
static_assert(sizeof(SOF_IPC_COMP_HOST) == 76 + 16, "sof_ipc_comp_host + uuid");
static_assert(sizeof(SOF_IPC_COMP_DAI) == 80 + 16, "sof_ipc_comp_dai + uuid");
static_assert(sizeof(SOF_IPC_COMP_VOLUME) == 84 + 16, "sof_ipc_comp_volume + uuid");
static_assert(sizeof(SOF_IPC_PIPE_NEW) == 48, "sof_ipc_pipe_new");
static_assert(sizeof(SOF_IPC_DAI_SSP_PARAMS) == 60, "sof_ipc_dai_ssp_params");
static_assert(sizeof(SOF_IPC_DAI_CONFIG_MSG) == 216, "sof_ipc_dai_config");
static_assert(sizeof(SOF_IPC_CTRL_DATA_2CH) == 92 + 16, "sof_ipc_ctrl_data + 2 chanv");
static_assert(sizeof(SOF_IPC_FW_READY_MSG) == 108, "sof_ipc_fw_ready");
