// Interface between the SOF DSP driver (sofapl) and the Windows audio driver
// (sofaudio). sofapl creates a child device SOFAPL\Speaker and exposes this
// interface on it; sofaudio binds to that child and gets it with
// IRP_MN_QUERY_INTERFACE.
//
// Endpoints:
//   SOFAPL_EP_SPEAKER   (SSP5, MAX98357A): S16_LE / 48 kHz / stereo playback
//   SOFAPL_EP_HEADPHONE (SSP1, DA7219):    S16_LE / 48 kHz / stereo playback
//   SOFAPL_EP_MIC       (DMIC0, internal digital mics): S32_LE / 48 kHz /
//                       stereo capture
// Each endpoint supports one stream.
#pragma once

// {3E7C2F51-8B0A-4C6D-9A4E-5D1F0B7C2A93}
DEFINE_GUID(GUID_SOFAPL_AUDIO_INTERFACE,
    0x3e7c2f51, 0x8b0a, 0x4c6d, 0x9a, 0x4e, 0x5d, 0x1f, 0x0b, 0x7c, 0x2a, 0x93);

#define SOFAPL_AUDIO_INTERFACE_VERSION 3

#define SOFAPL_EP_SPEAKER    0
#define SOFAPL_EP_HEADPHONE  1
#define SOFAPL_EP_MIC        2
#define SOFAPL_EP_COUNT      3

// All callbacks except StreamPosition must be called at PASSIVE_LEVEL.
// StreamPosition may be called at IRQL <= DISPATCH_LEVEL.

// Buffer must be physically contiguous, below 4 GB, 128-byte aligned and its
// size a multiple of 256 bytes.
typedef NTSTATUS (*PSOFAPL_STREAM_OPEN)(_In_ PVOID Context, _In_ ULONG Endpoint,
    _In_ PHYSICAL_ADDRESS BufferPhys, _In_ ULONG BufferSize, _In_ ULONG PeriodBytes);
typedef NTSTATUS (*PSOFAPL_STREAM_TRIGGER)(_In_ PVOID Context, _In_ ULONG Endpoint, _In_ BOOLEAN Start);
// Returns the DMA position (bytes, 0 .. BufferSize-1): how far the DSP has
// read (playback) or written (capture).
typedef ULONG    (*PSOFAPL_STREAM_POSITION)(_In_ PVOID Context, _In_ ULONG Endpoint);
typedef VOID     (*PSOFAPL_STREAM_CLOSE)(_In_ PVOID Context, _In_ ULONG Endpoint);
// Level in Q1.16 relative to the endpoint's safe maximum:
// 65536 = 100% (the endpoint maximum), 0 = silent.
typedef NTSTATUS (*PSOFAPL_SET_VOLUME)(_In_ PVOID Context, _In_ ULONG Endpoint, _In_ ULONG LevelQ16);

typedef struct _SOFAPL_AUDIO_INTERFACE {
    // standard INTERFACE header
    USHORT                 Size;
    USHORT                 Version;
    PVOID                  Context;
    PINTERFACE_REFERENCE   InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    PSOFAPL_STREAM_OPEN     StreamOpen;
    PSOFAPL_STREAM_TRIGGER  StreamTrigger;
    PSOFAPL_STREAM_POSITION StreamPosition;
    PSOFAPL_STREAM_CLOSE    StreamClose;
    PSOFAPL_SET_VOLUME      SetVolume;
} SOFAPL_AUDIO_INTERFACE, *PSOFAPL_AUDIO_INTERFACE;
