// Access to the SOF DSP driver (sofapl) from the audio driver.
#pragma once

// adapter.cpp includes <initguid.h> before this header so the GUID is defined once.
#include "..\..\..\include\sofapl_iface.h"

// Filled in StartDevice from the SOFAPL\Speaker PDO; valid while started.
extern SOFAPL_AUDIO_INTERFACE g_SofIface;
extern BOOLEAN g_SofIfaceValid;

// Initial DSP gain applied when a stream opens: -20 dB (Q1.16).
// Windows' own volume slider is applied on top of this by the audio engine.
#define SOF_STREAM_DSP_GAIN 6554u
