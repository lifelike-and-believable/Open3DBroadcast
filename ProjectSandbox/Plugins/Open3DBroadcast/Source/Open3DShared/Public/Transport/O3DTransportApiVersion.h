// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"

/**
 * Version of the transport interface that Open3DBroadcast exports to transport modules in other
 * plugins, such as the Open3DBroadcastWebRTC add-on (ADR 0007 item 2, ADR 0002, WP-F11; the
 * minimal form of SHR-14).
 *
 * The interface is every exported type and function an add-on transport uses: IOpen3DSender and
 * IOpen3DReceiver (with their audio sinks), FO3DTransportRegistry and FO3DTransportDescriptor (and,
 * until they are removed, the deprecated sender and receiver registries and transport
 * customizations that forward to it), FO3DTransportConfig, FO3DTransportOptionSchema,
 * FO3DFfiLibrary, FO3DSecretStore and ISerializedFrameConsumer.
 *
 * Bump rule: increase the number in the same PR as any change to one of those types that alters
 * a class layout, a virtual function table, an exported function signature or a documented
 * threading rule. An add-on compares the value it was compiled with against
 * O3DTransport::GetHostApiVersion() in StartupModule and registers nothing on a mismatch, so a
 * stale add-on logs an error instead of crashing.
 *
 * History:
 *   1  WP-F11: first version (the interface as of the add-on split).
 *      WP-A1 PR 1 moved IOpen3DSender, IOpen3DReceiver and ISerializedFrameConsumer into
 *      Open3DShared without changing their layout or virtual function tables, and added
 *      FO3DTransportRegistry beside forwarding shims for the old headers and functions. The
 *      number stays 1 while the shims exist (ADR 0007 open question 5); the release that removes
 *      them bumps it. An add-on must still be rebuilt against the release it loads into, because
 *      the interface classes are now exported from Open3DShared instead of Open3DSender and
 *      Open3DReceiver.
 *   2  ADR 0011 (CTL-2): the control channel appended SupportsControl and SendControl to
 *      IOpen3DSender and SupportsControl and SetControlSink to IOpen3DReceiver, and added
 *      IO3DReceiverControlSink.
 *   3  WP-A1 PR 2 (ADR 0007 item 5, lifetime): the registry tracks the instances it creates and
 *      drains a transport when it unregisters (OnTransportUnregistering, GetNumLiveInstances);
 *      register and unregister now check() the game thread. FO3DFfiLibrary changed layout: it
 *      asks the registry for live instances (FO3DFfiLibraryDesc::TransportNames,
 *      FO3DFfiLibraryOps::CountLiveInstances) instead of tracking them, and TrackInstance and
 *      StopLiveInstances are gone. The add-on constructs FO3DFfiLibrary and inlines its
 *      descriptor, so an add-on built for 2 must not load.
 *   4  WP-A1 PR 3 (ADR 0007 items 3 and 4; SHR-14): results, state and capabilities. Initialize
 *      and Start return FO3DTransportResult; SendSerialized is pure virtual, takes an owned
 *      FO3DSendPayload and returns EO3DSendResult, as SendControl does; IOpen3DSender and
 *      IOpen3DReceiver gained GetCapabilities, GetConnectionState and SetStateChangedCallback, and
 *      their SupportsAudio and SupportsControl are no longer virtual (they forward to
 *      GetCapabilities). FO3DTransportStats gained State, SendErrors, ReceiveErrors, PendingFrames
 *      and PendingBytes; FO3DTransportDescriptor gained GetCapabilities; receivers return
 *      NoConsumer from Start without a consumer. Removing the WP-A1 forwarding shims (step 6)
 *      takes the next number.
 *      WP-A1 PR 4a (step 4) stays at 4: it adds standalone types (FO3DSendQueue,
 *      FO3DTransportWorker, FO3DReconnectPolicy, FO3DUnifiedReceiveDemux, FO3DAudioPublishState,
 *      FO3DQueuedSenderAudioSink, O3DTransportOptions) and moves FO3DSenderAudioSinkBase and
 *      FO3DGatedSenderAudioSink from Open3DSender to Open3DShared unchanged; no type above changes
 *      layout, vtable or threading rule, and the add-on uses none of the moved classes.
 */
#define O3D_TRANSPORT_API_VERSION 4

namespace O3DTransport
{
	/**
	 * O3D_TRANSPORT_API_VERSION as compiled into the loaded Open3DShared module, as opposed to the
	 * value compiled into the caller. The name and signature of this function never change, so an
	 * add-on built against any version can call it. Any thread.
	 */
	OPEN3DSHARED_API int32 GetHostApiVersion();

	/**
	 * The check an add-on transport runs in StartupModule before it registers anything. Inline, so
	 * the logic is compiled into the add-on and does not depend on the host build it meets.
	 *
	 * @param AddOnName      Plugin name for the message, e.g. "Open3DBroadcastWebRTC".
	 * @param AddOnVersion   O3D_TRANSPORT_API_VERSION as the add-on saw it at compile time.
	 * @param HostVersion    GetHostApiVersion() of the loaded Open3DBroadcast.
	 * @param OutError       Empty on success; otherwise a message for an Error log line.
	 * @return true when the versions are equal. Any difference, older or newer, is a mismatch:
	 *         the add-on links against exported C++ classes whose layout must match exactly.
	 */
	inline bool CheckApiVersion(const FString& AddOnName, int32 AddOnVersion, int32 HostVersion, FString& OutError)
	{
		if (AddOnVersion == HostVersion)
		{
			OutError.Reset();
			return true;
		}
		OutError = FString::Printf(
			TEXT("%s was built for Open3DBroadcast transport API version %d, but the loaded Open3DBroadcast provides version %d. ")
			TEXT("%s registers nothing. Install the %s build made for this Open3DBroadcast release."),
			*AddOnName, AddOnVersion, HostVersion, *AddOnName, *AddOnName);
		return false;
	}
}
