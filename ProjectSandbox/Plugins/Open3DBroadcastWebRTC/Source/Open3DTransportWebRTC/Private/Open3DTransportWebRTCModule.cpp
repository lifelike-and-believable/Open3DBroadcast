// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_WEBRTC
#include "O3DFfiLibrary.h"
#include "Sender/WebRTCSender.h"
#include "Receiver/WebRTCReceiver.h"
#include "Shared/WebRTCAddOn.h"
#include "Shared/WebRTCUtils.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"
#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportApiVersion.h"
#include "Transport/O3DTransportRegistry.h"

DEFINE_LOG_CATEGORY(LogO3DWebRTCSender);
DEFINE_LOG_CATEGORY(LogO3DWebRTCReceiver);

#define LOCTEXT_NAMESPACE "Open3DTransportWebRTC"

namespace WebRTCConfig
{
	/** Registered transport name; the secret store and the credential profile option are keyed by it. */
	static constexpr TCHAR TransportName[] = TEXT("WebRTC");

	static constexpr TCHAR UrlOptionKey[] = TEXT("webrtc.url");
	static constexpr const TCHAR* TokenOptionKey = WebRTCUtils::TokenOptionKey;
	static constexpr const TCHAR* TokenEndpointAuthKey = WebRTCUtils::TokenEndpointAuthOptionKey;
	static constexpr TCHAR UseAutoTokenFetchKey[] = TEXT("webrtc.useAutoTokenFetch");
	static constexpr TCHAR TokenEndpointUrlKey[] = TEXT("webrtc.tokenEndpointUrl");
	static constexpr TCHAR TokenRefreshLeadTimeKey[] = TEXT("webrtc.tokenRefreshLeadTimeSec");
	// LiveKit room requested from the token endpoint; must match on sender and receiver (TRF-25).
	static constexpr const TCHAR* RoomOptionKey = WebRTCUtils::RoomOptionKey;

	// Token refresh lead time constants
	static constexpr int32 MinTokenRefreshLeadTimeSec = 60;     // 1 minute
	static constexpr int32 MaxTokenRefreshLeadTimeSec = 3600;   // 1 hour
	static constexpr int32 DefaultTokenRefreshLeadTimeSec = 300; // 5 minutes

	/** Non-secret option selecting the credential profile ("webrtc.credentialProfile"). */
	static FString GetCredentialProfileKey()
	{
		return FO3DSecretStore::MakeCredentialProfileOptionKey(TransportName);
	}

	/** Secret keys and their environment variables (ADR 0004 item 1). */
	static void DeclareSecrets(TArray<FString>& OutKeys, TMap<FString, FString>& OutEnvVars)
	{
		OutKeys = { FString(TokenOptionKey), FString(TokenEndpointAuthKey) };
		OutEnvVars.Add(TokenOptionKey, WebRTCUtils::TokenEnvVar);
		OutEnvVars.Add(TokenEndpointAuthKey, WebRTCUtils::TokenEndpointAuthEnvVar);
	}

	// Sender config helpers
	static FString GetSenderOption(const UO3DSenderComponent* Component, const TCHAR* Key)
	{
		return Component ? Component->GetTransportOption(Key) : FString();
	}

	// Receiver config helpers
	static FString GetReceiverOption(const FO3DReceiverSourceConfig& Settings, const TCHAR* Key)
	{
		if (const FString* Existing = Settings.TransportOptions.Find(Key))
		{
			return *Existing;
		}
		return FString();
	}
}

/**
 * Option schema (ADR 0010 §4), the same for both roles; the editor module renders it. The two
 * secrets are Secret fields: the editor shows a password box that opens empty, a Clear button, a
 * "Remember on this machine" box and a status line, and writes to FO3DSecretStore only (ADR 0004).
 */
namespace WebRTCSchema
{
	static FO3DTransportOptionSchema Make()
	{
		const TFunction<bool(const TMap<FString, FString>&)> WhenAutoFetch = O3DTransportOptions::VisibleWhenBool(WebRTCConfig::UseAutoTokenFetchKey, true);
		const TFunction<bool(const TMap<FString, FString>&)> WhenManualToken = O3DTransportOptions::VisibleWhenBool(WebRTCConfig::UseAutoTokenFetchKey, false);

		FO3DTransportOptionField Url;
		Url.Key = WebRTCConfig::UrlOptionKey;
		Url.DisplayName = LOCTEXT("WebRTCUrlLabel", "LiveKit Host");
		Url.Tooltip = LOCTEXT("WebRTCUrlTooltip", "The WebSocket URL of your LiveKit server (e.g., wss://livekit.example.com)");
		Url.Type = EO3DTransportOptionType::Url;
		Url.Hint = LOCTEXT("WebRTCUrlHint", "e.g., wss://livekit.example.com or ws://127.0.0.1:7880");

		FO3DTransportOptionField AutoFetch;
		AutoFetch.Key = WebRTCConfig::UseAutoTokenFetchKey;
		AutoFetch.DisplayName = LOCTEXT("WebRTCUseAutoTokenFetchLabel", "Use Auto Token Fetch");
		AutoFetch.Tooltip = LOCTEXT("WebRTCUseAutoTokenFetchTooltip", "Automatically fetch JWT tokens from a token generator endpoint instead of manually entering them. Requires a token server that implements the LiveKit token generation API.");
		AutoFetch.Type = EO3DTransportOptionType::Bool;
		AutoFetch.Default = TEXT("false");

		FO3DTransportOptionField Profile;
		Profile.Key = WebRTCConfig::GetCredentialProfileKey();
		Profile.DisplayName = LOCTEXT("WebRTCProfileLabel", "Credential Profile");
		Profile.Tooltip = LOCTEXT("WebRTCProfileTooltip", "Names the stored credentials this component or source uses (saved with it; the credentials are not). Leave empty for 'default'.");
		Profile.Type = EO3DTransportOptionType::String;
		Profile.Default = FO3DSecretStore::DefaultProfile();

		FO3DTransportOptionField Token;
		Token.Key = WebRTCConfig::TokenOptionKey;
		Token.DisplayName = LOCTEXT("WebRTCTokenLabel", "Access Token");
		Token.Tooltip = FText::Format(LOCTEXT("WebRTCTokenTooltip", "LiveKit JWT access token, used when Auto Token Fetch is off. Kept out of the level, Blueprint, ini and LiveLink presets. Can also come from the {0} environment variable."),
			FText::FromString(WebRTCUtils::TokenEnvVar));
		Token.Type = EO3DTransportOptionType::Secret;
		Token.Hint = LOCTEXT("WebRTCTokenHint", "Paste a LiveKit access token");
		Token.VisibleWhen = WhenManualToken;

		FO3DTransportOptionField Endpoint;
		Endpoint.Key = WebRTCConfig::TokenEndpointUrlKey;
		Endpoint.DisplayName = LOCTEXT("WebRTCTokenEndpointLabel", "Token Endpoint URL");
		Endpoint.Tooltip = LOCTEXT("WebRTCTokenEndpointTooltip", "The HTTPS endpoint that issues LiveKit tokens (e.g., https://myserver.com/token). It receives room, identity and role, must authenticate the caller and decides the grants. Plain http:// is accepted only for localhost, 127.0.0.1 and ::1.");
		Endpoint.Type = EO3DTransportOptionType::Url;
		Endpoint.Hint = LOCTEXT("WebRTCTokenEndpointHint", "https://myserver.com/token");
		Endpoint.VisibleWhen = WhenAutoFetch;

		FO3DTransportOptionField EndpointAuth;
		EndpointAuth.Key = WebRTCConfig::TokenEndpointAuthKey;
		EndpointAuth.DisplayName = LOCTEXT("WebRTCEndpointAuthLabel", "Token Endpoint Credential");
		EndpointAuth.Tooltip = FText::Format(LOCTEXT("WebRTCEndpointAuthTooltip", "Credential for your token endpoint, sent as 'Authorization: Bearer <value>'. The endpoint must authenticate callers and decide their grants. Can also come from the {0} environment variable."),
			FText::FromString(WebRTCUtils::TokenEndpointAuthEnvVar));
		EndpointAuth.Type = EO3DTransportOptionType::Secret;
		EndpointAuth.Hint = LOCTEXT("WebRTCEndpointAuthHint", "Bearer credential for your token endpoint");
		EndpointAuth.VisibleWhen = WhenAutoFetch;

		FO3DTransportOptionField Room;
		Room.Key = WebRTCConfig::RoomOptionKey;
		Room.DisplayName = LOCTEXT("WebRTCRoomLabel", "Room");
		Room.Tooltip = LOCTEXT("WebRTCRoomTooltip", "LiveKit room requested from the token endpoint. Use the same room on the sender and the receiver. Required for Auto Token Fetch.");
		Room.Type = EO3DTransportOptionType::String;
		Room.Hint = LOCTEXT("WebRTCRoomHint", "e.g., my-stage");
		Room.VisibleWhen = WhenAutoFetch;

		FO3DTransportOptionField LeadTime;
		LeadTime.Key = WebRTCConfig::TokenRefreshLeadTimeKey;
		LeadTime.DisplayName = LOCTEXT("WebRTCTokenRefreshLeadTimeLabel", "Token Refresh Lead Time (seconds)");
		LeadTime.Tooltip = LOCTEXT("WebRTCTokenRefreshLeadTimeTooltip", "How many seconds before token expiry to trigger an automatic refresh. Default is 300 seconds (5 minutes).");
		LeadTime.Type = EO3DTransportOptionType::Int;
		LeadTime.Default = FString::FromInt(WebRTCConfig::DefaultTokenRefreshLeadTimeSec);
		LeadTime.Min = WebRTCConfig::MinTokenRefreshLeadTimeSec;
		LeadTime.Max = WebRTCConfig::MaxTokenRefreshLeadTimeSec;
		LeadTime.VisibleWhen = WhenAutoFetch;

		FO3DTransportOptionSchema Schema;
		Schema.Add(MoveTemp(Url));
		Schema.Add(MoveTemp(AutoFetch));
		Schema.Add(MoveTemp(Profile));
		Schema.Add(MoveTemp(Token));
		Schema.Add(MoveTemp(Endpoint));
		Schema.Add(MoveTemp(EndpointAuth));
		Schema.Add(MoveTemp(Room));
		Schema.Add(MoveTemp(LeadTime));
		return Schema;
	}
}

class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// This module ships in the Open3DBroadcastWebRTC add-on and links against Open3DBroadcast's
		// exported transport interface. Check the interface version before anything else, so an
		// add-on built for another Open3DBroadcast release registers nothing instead of crashing
		// (ADR 0002, ADR 0007 item 2, WP-F11).
		FString VersionError;
		if (!O3DWebRTCAddOn::CheckHostApiVersion(O3DTransport::GetHostApiVersion(), VersionError))
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC transport not registered: %s"), *VersionError);
			return;
		}

		// livekit_ffi.dll is delay-loaded, so it must be loaded from this add-on's own plugin
		// folder before the first call (ADR 0002 blocker 1, TRF-28). Without it the transport is
		// not registered at all: a factory would otherwise hand out instances whose first FFI
		// call fails on delay-load (TRF-14).
		TSharedRef<FO3DFfiLibrary, ESPMode::ThreadSafe> NewLibrary = MakeShared<FO3DFfiLibrary, ESPMode::ThreadSafe>(O3DWebRTCAddOn::MakeLiveKitLibraryDesc());
		if (!NewLibrary->Load())
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC transport not registered: %s"), *NewLibrary->GetStatusMessage());
			return;
		}
		Library = NewLibrary;

		// One descriptor for the transport name (ADR 0007 item 4, WP-A1): factories, configure
		// functions, secret declarations and option schemas.
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = WebRTCConfig::TransportName;
		Descriptor.OwningModule = TEXT("Open3DTransportWebRTC");

		// Every instance is tracked so ShutdownModule can stop it before unloading livekit_ffi.
		const TSharedRef<FO3DFfiLibrary, ESPMode::ThreadSafe> LibraryRef = Library.ToSharedRef();
		Descriptor.CreateSender = [LibraryRef]() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCSender, ESPMode::ThreadSafe>());
		};
		Descriptor.CreateReceiver = [LibraryRef]() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCReceiver, ESPMode::ThreadSafe>());
		};

		// Sender side
		WebRTCConfig::DeclareSecrets(Descriptor.SenderOptions.SecretOptionKeys, Descriptor.SenderOptions.SecretEnvVars);
		Descriptor.ConfigureSender = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			Config.Transport = WebRTCConfig::TransportName;

			const FString UrlValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::UrlOptionKey);
			const FString UseAutoTokenFetchStr = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::UseAutoTokenFetchKey);
			const FString TokenEndpointUrlValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::TokenEndpointUrlKey);
			const FString TokenRefreshLeadTimeStr = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::TokenRefreshLeadTimeKey);
			const FString RoomValue = WebRTCConfig::GetSenderOption(SenderComponent, WebRTCConfig::RoomOptionKey);

			Config.Uri = UrlValue;
			// Resolved from the secret store by the component (ADR 0004); never in AdvancedParams.
			Config.Token = WebRTCUtils::FindSecret(Config.Secrets, WebRTCConfig::TokenOptionKey);
			Config.Role = TEXT("publisher");

			// Configure auto-fetch fields
			Config.bUseAutoTokenFetch = UseAutoTokenFetchStr.ToBool();
			Config.TokenEndpointUrl = TokenEndpointUrlValue;
			Config.TokenRefreshLeadTimeSec = TokenRefreshLeadTimeStr.IsEmpty() ? WebRTCConfig::DefaultTokenRefreshLeadTimeSec : FCString::Atoi(*TokenRefreshLeadTimeStr);

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::RoomOptionKey, RoomValue.TrimStartAndEnd());
		};
		Descriptor.SenderOptions.OptionSchema = WebRTCSchema::Make();

		// Receiver side
		WebRTCConfig::DeclareSecrets(Descriptor.ReceiverOptions.SecretOptionKeys, Descriptor.ReceiverOptions.SecretEnvVars);
		Descriptor.ConfigureReceiver = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			Config.Transport = WebRTCConfig::TransportName;

			const FString UrlValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::UrlOptionKey);
			const FString UseAutoTokenFetchStr = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::UseAutoTokenFetchKey);
			const FString TokenEndpointUrlValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::TokenEndpointUrlKey);
			const FString TokenRefreshLeadTimeStr = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::TokenRefreshLeadTimeKey);
			const FString RoomValue = WebRTCConfig::GetReceiverOption(Settings, WebRTCConfig::RoomOptionKey);

			Config.Uri = UrlValue;
			// Resolved from the secret store by the source (ADR 0004); never in AdvancedParams.
			Config.Token = WebRTCUtils::FindSecret(Config.Secrets, WebRTCConfig::TokenOptionKey);
			Config.StreamId = TEXT("WebRTCStream");
			Config.Role = TEXT("subscriber");

			// Configure auto-fetch fields
			Config.bUseAutoTokenFetch = UseAutoTokenFetchStr.ToBool();
			Config.TokenEndpointUrl = TokenEndpointUrlValue;
			Config.TokenRefreshLeadTimeSec = TokenRefreshLeadTimeStr.IsEmpty() ? WebRTCConfig::DefaultTokenRefreshLeadTimeSec : FCString::Atoi(*TokenRefreshLeadTimeStr);

			Config.AdvancedParams.Add(WebRTCConfig::UrlOptionKey, UrlValue);
			Config.AdvancedParams.Add(WebRTCConfig::RoomOptionKey, RoomValue.TrimStartAndEnd());

			Config.Audio.bEnableAudio = Settings.bEnableAudio;
			// Note: Audio stream label is now automatically derived from StreamId
		};
		Descriptor.ReceiverOptions.OptionSchema = WebRTCSchema::Make();

		Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		if (!Registration.IsValid())
		{
			// The registry logged why (for example the name is already taken). The library stays
			// loaded so ShutdownModule unloads it as usual.
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC transport not registered; see the previous message."));
			return;
		}

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module started (LiveKit FFI backend)"));
	}

	virtual void ShutdownModule() override
	{
		if (!Library.IsValid())
		{
			// StartupModule registered nothing (version mismatch or livekit_ffi missing), so there
			// is nothing to unregister, and a registration under this name belongs to someone else.
			return;
		}

		// Unregister the transport. The handle only removes this module's own registration.
		Registration.Reset();

		// TRF-14: stop instances that outlive the module (components, LiveLink sources), then
		// unload. FO3DFfiLibrary keeps the DLL loaded if an instance is still referenced. The
		// factories were unregistered above, so no new instance can appear in between.
		Library->StopLiveInstances();
		Library->Unload();
		Library.Reset();

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module shut down"));
	}

private:
	/**
	 * livekit_ffi and the instances created from it. Set only once the transport is registered,
	 * so ShutdownModule knows whether there is anything to undo. The registered factories hold a
	 * reference too.
	 */
	TSharedPtr<FO3DFfiLibrary, ESPMode::ThreadSafe> Library;

	/** The one registration of "WebRTC" (ADR 0007 item 4, WP-A1). */
	FO3DTransportRegistration Registration;
};

#else // O3D_WITH_TRANSPORT_WEBRTC

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportWebRTCModule, Log, All);

/**
 * Stub module, compiled when O3D_WITH_TRANSPORT_WEBRTC is 0: the transport was switched off with that
 * environment variable, or the target platform has no prebuilt livekit_ffi
 * (O3DWebRtcBuildFlags in Open3DTransportWebRTC.Build.cs). It registers nothing.
 */
class FOpen3DTransportWebRTCModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogOpen3DTransportWebRTCModule, Display, TEXT("Open3D WebRTC transport is not available in this build (O3D_WITH_TRANSPORT_WEBRTC=0)."));
	}

	virtual void ShutdownModule() override {}
};

#endif // O3D_WITH_TRANSPORT_WEBRTC

IMPLEMENT_MODULE(FOpen3DTransportWebRTCModule, Open3DTransportWebRTC)

#undef LOCTEXT_NAMESPACE
