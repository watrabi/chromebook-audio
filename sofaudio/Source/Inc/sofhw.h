// Access to the SOF DSP driver (sofapl) from the audio driver.
#pragma once

// adapter.cpp includes <initguid.h> before this header so the GUID is defined once.
#include "..\..\..\include\sofapl_iface.h"

// Filled in StartDevice from the SOFAPL\Speaker PDO; valid while started.
extern SOFAPL_AUDIO_INTERFACE g_SofIface;
extern BOOLEAN g_SofIfaceValid;

// Topology volume/mute node storage: the adapter keeps one array for all
// filters, so each endpoint's nodes are stored at an offset.
//   speaker   : volume 0,  mute 1
//   headphone : volume 10, mute 11
#define SOF_NODE_OFFSET_HEADPHONE  10
#define SOF_NODE_VOLUME            0
#define SOF_NODE_MUTE              1

// Map a stored node index back to its DSP endpoint.
#define SOF_NODE_TO_ENDPOINT(n) (((n) >= SOF_NODE_OFFSET_HEADPHONE) ? SOFAPL_EP_HEADPHONE : SOFAPL_EP_SPEAKER)
#define SOF_ENDPOINT_NODE_BASE(e) (((e) == SOFAPL_EP_HEADPHONE) ? SOF_NODE_OFFSET_HEADPHONE : 0)

// Apply the topology volume (KS units: 1/65536 dB, <= 0) and mute of an
// endpoint to the DSP. PASSIVE_LEVEL.
VOID SofApplyMixerGain(_In_ ULONG Endpoint, _In_ LONG Volume1616, _In_ BOOL Mute);

// Re-apply the last volume/mute of an endpoint (used when a stream opens).
VOID SofReapplyMixerGain(_In_ ULONG Endpoint);
