// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

#include "WebRTCAddOn.h"

#include "Transport/O3DTransportApiVersion.h"

namespace O3DWebRTCAddOn
{
	const TCHAR* const PluginName = TEXT("Open3DBroadcastWebRTC");

	FO3DFfiLibraryDesc MakeLiveKitLibraryDesc()
	{
		FO3DFfiLibraryDesc Desc;
		Desc.DisplayName = TEXT("LiveKit FFI");
		Desc.OwningPluginName = PluginName;
		// Win64 only: O3D_WITH_TRANSPORT_WEBRTC is 0 on every other platform, so this file then
		// compiles to nothing (ADR 0001).
		Desc.RelativePath = TEXT("Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.dll");
		return Desc;
	}

	bool CheckHostApiVersion(int32 HostVersion, FString& OutError)
	{
		return O3DTransport::CheckApiVersion(PluginName, O3D_TRANSPORT_API_VERSION, HostVersion, OutError);
	}
}

#endif // O3D_WITH_TRANSPORT_WEBRTC
