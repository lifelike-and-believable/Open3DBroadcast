// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// What makes Open3DTransportWebRTC an add-on (WP-F11, ADR 0002): the plugin it ships in, where
// its livekit_ffi.dll lives relative to that plugin, and the transport-interface version check it
// runs before registering with Open3DBroadcast. Kept out of the module class so tests can call it.

#include "CoreMinimal.h"
#include "O3DFfiLibrary.h"

namespace O3DWebRTCAddOn
{
	/** Name of the plugin that ships this module (Open3DBroadcastWebRTC.uplugin). */
	extern const TCHAR* const PluginName;

	/**
	 * livekit_ffi.dll relative to this add-on's own plugin folder, not Open3DBroadcast's
	 * (ADR 0002 blocker 1, TRF-28). FO3DFfiLibrary resolves the plugin through IPluginManager, so
	 * the add-on can be installed in a project's Plugins/ folder or under the engine.
	 */
	FO3DFfiLibraryDesc MakeLiveKitLibraryDesc();

	/**
	 * Compares the O3D_TRANSPORT_API_VERSION this module was compiled with against HostVersion
	 * (O3DTransport::GetHostApiVersion() of the loaded Open3DBroadcast). Returns false and sets
	 * OutError when they differ; StartupModule then logs OutError and registers nothing.
	 */
	bool CheckHostApiVersion(int32 HostVersion, FString& OutError);
}
