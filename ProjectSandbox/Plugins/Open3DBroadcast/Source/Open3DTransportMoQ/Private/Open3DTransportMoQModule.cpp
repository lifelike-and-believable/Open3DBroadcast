// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Shared/MoQAsyncDispatcher.h"
#include "MoQFfiApi.h"
#include "Shared/MoQFfiSupport.h"
#include "O3DFfiLibrary.h"
#include "Shared/MoQHelpers.h"
#include "Sender/MoQSender.h"
#include "Receiver/MoQReceiver.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportRegistry.h"

#define LOCTEXT_NAMESPACE "Open3DTransportMoQ"

namespace MoQConfig
{
	// Reads the sender component's option map with the MoQHelpers key set (relay_url,
	// track_namespace, track_name, delivery_mode, queue_bytes - see MoQHelpers.h).
	static FString GetSenderOption(const UO3DSenderComponent* Component, const TCHAR* Key)
	{
		return Component ? Component->GetTransportOption(Key) : FString();
	}
}

/**
 * Option schemas (ADR 0010 §4), rendered by the editor module. Same fields for both roles: the
 * relay URL is required (MoQHelpers::ResolveRelayUrl has no fallback); namespace and track name
 * are optional overrides of the auto-derived mocap/<session>/<track> naming (README.md,
 * "Configuration Options").
 */
namespace MoQSchema
{
	static constexpr int64 BytesPerMiB = 1024ll * 1024ll;

	static FO3DTransportOptionSchema Make(bool bSender)
	{
		FO3DTransportOptionField Relay;
		Relay.Key = MoQHelpers::kKeyRelayUrl;
		Relay.DisplayName = LOCTEXT("MoQRelayLabel", "Relay URL");
		Relay.Tooltip = LOCTEXT("MoQRelayTooltip", "MoQ relay to connect to. The relay must speak draft-ietf-moq-transport-07.");
		Relay.Type = EO3DTransportOptionType::Url;
		Relay.Hint = LOCTEXT("MoQRelayUrlHint", "https://relay.example.com:443");

		FO3DTransportOptionField Namespace;
		Namespace.Key = MoQHelpers::kKeyTrackNamespace;
		Namespace.DisplayName = LOCTEXT("MoQNamespaceLabel", "Track Namespace (optional)");
		Namespace.Type = EO3DTransportOptionType::String;
		Namespace.Hint = LOCTEXT("MoQNamespaceHint", "auto: mocap/<session> if blank");

		FO3DTransportOptionField Track;
		Track.Key = MoQHelpers::kKeyTrackName;
		Track.DisplayName = LOCTEXT("MoQTrackLabel", "Track Name (optional)");
		Track.Type = EO3DTransportOptionType::String;
		Track.Hint = bSender
			? LOCTEXT("MoQSenderTrackNameHint", "auto: from Subject Name if blank")
			: LOCTEXT("MoQReceiverTrackNameHint", "auto: from Stream Id if blank");

		FO3DTransportOptionField Delivery;
		Delivery.Key = MoQHelpers::kKeyDeliveryMode;
		Delivery.DisplayName = LOCTEXT("MoQDeliveryLabel", "Delivery Mode");
		Delivery.Tooltip = LOCTEXT("MoQDeliveryTooltip", "Stream delivers every frame in order. Datagram drops late frames instead of waiting for them.");
		Delivery.Type = EO3DTransportOptionType::Enum;
		Delivery.Default = TEXT("stream");
		{
			FO3DTransportOptionEnumValue Stream;
			Stream.Value = TEXT("stream");
			Stream.DisplayName = LOCTEXT("MoQDeliveryStream", "Stream");
			Delivery.EnumValues.Add(MoveTemp(Stream));

			FO3DTransportOptionEnumValue Datagram;
			Datagram.Value = TEXT("datagram");
			Datagram.DisplayName = LOCTEXT("MoQDeliveryDatagram", "Datagram");
			Delivery.EnumValues.Add(MoveTemp(Datagram));
		}

		FO3DTransportOptionField Queue;
		Queue.Key = MoQHelpers::kKeyQueueBytes;
		Queue.DisplayName = LOCTEXT("MoQQueueLabel", "Queue Capacity (MiB)");
		Queue.Tooltip = LOCTEXT("MoQQueueTooltip", "Bytes queued for the relay before frames are dropped. Stored in bytes.");
		Queue.Type = EO3DTransportOptionType::Int;
		Queue.Default = LexToString(MoQHelpers::kDefaultQueueBytes);
		Queue.StoredUnitScale = BytesPerMiB;
		Queue.Min = 1;
		Queue.Max = static_cast<int32>(MoQHelpers::kMaxQueueBytes / static_cast<uint64>(BytesPerMiB));

		FO3DTransportOptionSchema Schema;
		Schema.Add(MoveTemp(Relay));
		Schema.Add(MoveTemp(Namespace));
		Schema.Add(MoveTemp(Track));
		Schema.Add(MoveTemp(Delivery));
		Schema.Add(MoveTemp(Queue));
		return Schema;
	}
}

/**
 * Open3DTransportMoQ Module
 *
 * - Loads and validates the moq-ffi runtime through FO3DFfiLibrary (TRF-28)
 * - Wires the async dispatcher used by the session wrapper
 * - Registers sender and receiver factories with audio support, only when the library loaded,
 *   validated and initialized (TRF-14)
 */
class FOpen3DTransportMoQModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Library = MakeShared<FO3DFfiLibrary, ESPMode::ThreadSafe>(FMoQFfiSupport::MakeLibraryDesc());
		if (!Library->Load())
		{
			UE_LOG(LogO3DMoQSender, Error, TEXT("MoQ transport not registered: %s"), *Library->GetStatusMessage());
			return;
		}

		FString ValidationError;
		if (!FMoQFfiSupport::ValidateLibrary(*Library, ValidationError))
		{
			UE_LOG(LogO3DMoQSender, Error, TEXT("MoQ transport not registered: %s"), *ValidationError);
			Library->Unload();
			return;
		}

		// Initialize the MoQ FFI crypto provider - must be called before any TLS operations
		// BUG-1 fix: Check return value of moq_init()
		if (!FMoQFfiApi::GetProduction()->Init())
		{
			UE_LOG(LogO3DMoQSender, Error, TEXT("MoQ transport not registered: failed to initialize the MoQ FFI crypto provider"));
			Library->Unload();
			return;
		}

		// Delivers FFI callbacks to the game thread. Nothing restarts it after ShutdownModule.
		FMoQAsyncDispatcher::Get().Initialize();

		RegisterTransports();

		UE_LOG(LogO3DMoQSender, Log, TEXT("Open3D MoQ transport module started"));
	}

	virtual void ShutdownModule() override
	{
		// TRF-13/TRF-14 ordering (ADR 0007 item 5, WP-A1 PR 2): unregister, which drains "MoQ":
		// no new instances, the sender components and LiveLink sources stop and release theirs
		// (OnTransportUnregistering), and the registry stops and reports any left. Then stop
		// delivering FFI callbacks (queued ones are discarded and later ones dropped), then unload
		// the library. FO3DFfiLibrary keeps the DLL loaded while the registry still counts a live
		// MoQ instance.
		UnregisterTransports();

		FMoQAsyncDispatcher::Get().Shutdown();

		if (Library.IsValid())
		{
			Library->Unload();
		}

		UE_LOG(LogO3DMoQSender, Log, TEXT("Open3D MoQ transport module shut down"));
	}

private:
	/** moq_ffi. Unloaded in ShutdownModule after the registration is reset (drained). */
	TSharedPtr<FO3DFfiLibrary, ESPMode::ThreadSafe> Library;

	/** The one registration of "MoQ" (ADR 0007 item 4, WP-A1); valid only while the library is loaded. */
	FO3DTransportRegistration Registration;

	void RegisterTransports()
	{
		// One descriptor: factories, configure functions and option schemas. The pickers list the
		// names that have a factory, so "MoQ" appears in both transport dropdowns from this alone.
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = TEXT("MoQ");
		Descriptor.OwningModule = TEXT("Open3DTransportMoQ");

		// The registry tracks every instance it creates, so unregistering drains them before
		// ShutdownModule unloads moq_ffi (ADR 0007 item 5).
		Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
		{
			return MakeShared<FO3DMoQSender, ESPMode::ThreadSafe>();
		};
		Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>
		{
			return MakeShared<FO3DMoQReceiver, ESPMode::ThreadSafe>();
		};

		Descriptor.ConfigureSender = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			// AdvancedParams (relay_url/track_namespace/track_name/delivery_mode/
			// queue_bytes) are already copied generically from
			// SenderComponent->TransportOptions by BuildTransportConfig() before
			// this runs; MoQHelpers reads them directly from Config.AdvancedParams.
			// Mirror the relay URL into Config.Uri/StreamId too, for parity with
			// other transports and any code that inspects those fields directly
			// instead of going through MoQHelpers::ResolveRelayUrl.
			Config.Transport = TEXT("MoQ");
			Config.Uri = MoQConfig::GetSenderOption(SenderComponent, MoQHelpers::kKeyRelayUrl);
			if (Config.StreamId.IsEmpty() && SenderComponent)
			{
				Config.StreamId = SenderComponent->SubjectName;
			}
		};
		Descriptor.SenderOptions.OptionSchema = MoQSchema::Make(/*bSender=*/true);

		Descriptor.ConfigureReceiver = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			// Deliberately does NOT set Config.Uri here (unlike the sender
			// side): O3DReceiverSource::BuildTransportConfig() auto-fills
			// Config.StreamId = Config.Uri whenever StreamId is still empty
			// after this callback returns, and MoQReceiver::ParseOptions()
			// only derives its own mocap/<namespace>/<track> default StreamId
			// when Config.StreamId arrives empty - so setting Config.Uri to
			// the relay URL here would silently make the receiver use the
			// relay URL itself as the stream/subject identifier instead.
			// relay_url is already read directly from Config.AdvancedParams
			// by MoQHelpers::ResolveRelayUrl, so Config.Uri isn't needed.
			Config.Transport = TEXT("MoQ");
		};
		Descriptor.ReceiverOptions.OptionSchema = MoQSchema::Make(/*bSender=*/false);

		Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport registered"));
	}

	/** Unregisters "MoQ"; the registry drains its live instances before this returns. */
	void UnregisterTransports()
	{
		Registration.Reset();
		UE_LOG(LogO3DMoQSender, Verbose, TEXT("MoQ transport unregistered"));
	}
};

#undef LOCTEXT_NAMESPACE

#else // O3D_WITH_TRANSPORT_MOQ

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportMoQModule, Log, All);

/**
 * Stub module, compiled when O3D_WITH_TRANSPORT_MOQ is 0: the transport was switched off with that
 * environment variable, or the target platform has no prebuilt moq_ffi
 * (O3DBuildFlags in Open3DBroadcastBuildFlags.Build.cs). It registers nothing.
 */
class FOpen3DTransportMoQModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogOpen3DTransportMoQModule, Display, TEXT("Open3D MoQ transport is not available in this build (O3D_WITH_TRANSPORT_MOQ=0)."));
	}

	virtual void ShutdownModule() override {}
};

#endif // O3D_WITH_TRANSPORT_MOQ

IMPLEMENT_MODULE(FOpen3DTransportMoQModule, Open3DTransportMoQ)
