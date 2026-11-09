// Interface between the SOF DSP driver (sofapl) and the Windows audio driver
// (sofaudio). sofapl creates a child device SOFAPL\Speaker and exposes this
// interface on it; sofaudio binds to that child and gets it with
// IRP_MN_QUERY_INTERFACE.
//
// Only one speaker stream is supported: S16_LE, 48 kHz, stereo.
#pragma once

// {3E7C2F51-8B0A-4C6D-9A4E-5D1F0B7C2A93}
DEFINE_GUID(GUID_SOFAPL_AUDIO_INTERFACE,
    0x3e7c2f51, 0x8b0a, 0x4c6d, 0x9a, 0x4e, 0x5d, 0x1f, 0x0b, 0x7c, 0x2a, 0x93);

#define SOFAPL_AUDIO_INTERFACE_VERSION 1

// All callbacks except Position must be called at PASSIVE_LEVEL.
// Position may be called at IRQL <= DISPATCH_LEVEL.

// Buffer must be physically contiguous, below 4 GB, 128-byte aligned and its
// size a multiple of 256 bytes.
typedef NTSTATUS (*PSOFAPL_STREAM_OPEN)(_In_ PVOID Context, _In_ PHYSICAL_ADDRESS BufferPhys,
    _In_ ULONG BufferSize, _In_ ULONG PeriodBytes);
typedef NTSTATUS (*PSOFAPL_STREAM_TRIGGER)(_In_ PVOID Context, _In_ BOOLEAN Start);
// Returns the DMA read position (bytes, 0 .. BufferSize-1).
typedef ULONG    (*PSOFAPL_STREAM_POSITION)(_In_ PVOID Context);
typedef VOID     (*PSOFAPL_STREAM_CLOSE)(_In_ PVOID Context);
// Gain in Q1.16 (65536 = 0 dB). The DSP driver clamps it to a safe maximum.
typedef NTSTATUS (*PSOFAPL_SET_VOLUME)(_In_ PVOID Context, _In_ ULONG GainQ16);

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
