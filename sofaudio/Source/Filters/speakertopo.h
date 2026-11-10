
/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertopo.h

Abstract:

    Declaration of topology miniport for the speaker (internal).
--*/

#ifndef _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_
#define _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_

NTSTATUS PropertyHandler_SpeakerTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS PropertyHandler_SpeakerTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// SOF: headphone endpoint (3.5 mm jack on the DA7219), same topology shape
NTSTATUS PropertyHandler_HeadphoneTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS PropertyHandler_HeadphoneTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

#endif // _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_
