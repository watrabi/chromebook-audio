// Access to the SOF DSP driver (sofapl) from the audio driver.
#pragma once

// adapter.cpp includes <initguid.h> before this header so the GUID is defined once.
#include "..\..\..\include\sofapl_iface.h"

// Filled in StartDevice from the SOFAPL\Speaker PDO; valid while started.
extern SOFAPL_AUDIO_INTERFACE g_SofIface;
extern BOOLEAN g_SofIfaceValid;

// Gain the DSP uses when Windows' volume is at 100% (0 dB). sofapl also caps
// at this value (-6 dB, Q1.16).
#define SOF_DSP_MAX_GAIN 32768u

// Apply the topology volume (KS units: 1/65536 dB, <= 0) and mute to the DSP.
// PASSIVE_LEVEL.
VOID SofApplyMixerGain(_In_ LONG Volume1616, _In_ BOOL Mute);

// Re-apply the last volume/mute (used when a stream opens). PASSIVE_LEVEL.
VOID SofReapplyMixerGain();
