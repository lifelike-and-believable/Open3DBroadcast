// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"

/**
 * Version of the transport interface that Open3DBroadcast exports to transport modules in other
 * plugins, such as the Open3DBroadcastWebRTC add-on (ADR 0007 item 2, ADR 0002, WP-F11; the
 * minimal form of SHR-14).
 *
 * The interface is every exported type and function an add-on transport uses: IOpen3DSender and
 * IOpen3DReceiver (with their audio sinks), FO3DTransportRegistry and FO3DTransportDescriptor,
 * FO3DTransportConfig, FO3DTransportOptionSchema, FO3DFfiLibrary, FO3DSecretStore and
 * ISerializedFrameConsumer. (The deprecated registries and customizations that forwarded to the
 * registry were removed in WP-A1 step 6.)
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
 *   5  WP-A1 PR 5a (ADR 0007 item 8, step 5; SHR-36, SND-35): typed config. The configure
 *      functions of FO3DTransportDescriptor take (const FO3DTransportOptionsView&,
 *      FO3DTransportConfig&) instead of the sender component or the receiver source settings.
 *      FO3DTransportConfig lost Token, bPersistToken, bUseAutoTokenFetch, TokenEndpointUrl,
 *      TokenRefreshLeadTimeSec and Backend, and gained SubjectName and OptionSchema. The
 *      O3DTransportOptions getters take an FO3DTransportOptionsView (a plain map still converts),
 *      and TryParseBool is new. The deprecated customization shims keep their old signatures and
 *      adapt. Removing the shims (step 6) takes the next number.
 *      WP-A1 PR 5b (ADR 0007 item 3, step 5; SHR-16) is part of 5, because no release carried 5
 *      before it (the last tag, v0.9.6, predates 5a; 5a is still under Unreleased in the
 *      CHANGELOG): ISerializedFrameConsumer::SubmitFrame takes a TConstArrayView<uint8> (the view
 *      form) and gained SubmitFrameOwned (the owned form, defaulting to the view form);
 *      IOpen3DSender::Send(const O3DS::SubjectList&) is deleted; FO3DUnifiedReceiveDemux::
 *      DeliverMocap takes a view, DeliverMocapOwned is new, and its scratch buffer is gone.
 *      WP-A1 PR 5c (ADR 0007 item 8, step 5; TRB-27) is part of 5 for the same reason (still no
 *      tag after v0.9.6, 5a and 5b still under Unreleased): FO3DTransportConfig::Transport is an
 *      FName and Role an EO3DTransportRole (with a (Transport, Role) constructor);
 *      FO3DTransportOptionField gained SecretEnvVar, bRestartOnChange and Validate, its Min and
 *      Max became doubles, and EO3DTransportOptionType gained Float (appended);
 *      FO3DTransportRoleOptions gained GetSecretDeclaration (the registry's merge of Secret
 *      entries with SecretOptionKeys and SecretEnvVars); O3DTransportOptions gained
 *      ValidateOptionValue and ValidateOptions.
 *      WP-A1 step 6 (ADR 0007 item 9) is part of 5 as well, by the maintainer's decision: no
 *      release carried the shims (still no tag after v0.9.6; no tag contains 5a's merge c98c92c)
 *      and no third-party add-on builds against them, so they are removed in this unreleased
 *      cycle instead of one release later. Removed: O3DTransport::RegisterSender, UnregisterSender,
 *      CreateSender, GetRegisteredSenders and the receiver counterparts; the sender and receiver
 *      transport customizations (RegisterTransportCustomization, UnregisterTransportCustomization,
 *      FindTransportCustomization, GetRegisteredTransportNames, GetTransportSecretDeclaration,
 *      GetTransportOptionSchema); FO3DTransportRegistry::EditLegacyDescriptor; the forwarding
 *      headers; and FO3DTransportRoleOptions::SecretOptionKeys and SecretEnvVars (a Secret schema
 *      entry is the only secret declaration).
 *      ADR 0005 (vi) is part of 5 as well (still no tag after v0.9.6): IOpen3DSender gained
 *      SetPeerJoinedCallback (default no-op) and FO3DPeerJoinedCallback; TCP and NNG set
 *      bPeerJoinSignal.
 *      ADR 0012 PR 2 (SHR-38) is part of 5 as well (still no tag after v0.9.6):
 *      FO3DTransportConfig gained Context (the FO3DRuntimeContext the transport records its
 *      metrics into; empty means the default context), and IOpen3DSender::Initialize documents
 *      that no send may be in flight while it runs.
 *      ADR 0012 PR 3b (SHR-38) is part of 5 as well: FO3DTransportConfig gained SenderMetrics
 *      (an FO3DSenderMetricsHandle), through which sender transports record sender metrics.
 */
#define O3D_TRANSPORT_API_VERSION 5

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
