// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_WEBRTC
#include "O3DFfiLibrary.h"
#include "Sender/WebRTCSender.h"
#include "Receiver/WebRTCReceiver.h"
#include "Shared/WebRTCUtils.h"
#include "O3DSenderRegistry.h"
#include "O3DReceiverRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"
#include "O3DTransportOptionSchema.h"

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
		// livekit_ffi.dll is delay-loaded, so it must be loaded from the plugin before the first
		// call. Without it the transport is not registered at all: a factory would otherwise
		// hand out instances whose first FFI call fails on delay-load (TRF-14).
		Library = MakeShared<FO3DFfiLibrary, ESPMode::ThreadSafe>(MakeLiveKitLibraryDesc());
		if (!Library->Load())
		{
			UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC transport not registered: %s"), *Library->GetStatusMessage());
			return;
		}

		// Every instance is tracked so ShutdownModule can stop it before unloading livekit_ffi.
		const TSharedRef<FO3DFfiLibrary, ESPMode::ThreadSafe> LibraryRef = Library.ToSharedRef();
		O3DTransport::RegisterSender(WebRTCConfig::TransportName, [LibraryRef]() -> TSharedPtr<IOpen3DSender>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCSender, ESPMode::ThreadSafe>());
		});
		O3DTransport::RegisterReceiver(WebRTCConfig::TransportName, [LibraryRef]() -> TSharedPtr<IOpen3DReceiver>
		{
			return LibraryRef->TrackInstance(MakeShared<FO3DWebRTCReceiver, ESPMode::ThreadSafe>());
		});

		// Register WebRTC sender customization
		FO3DSenderTransportCustomization SenderCustomization;
		WebRTCConfig::DeclareSecrets(SenderCustomization.SecretOptionKeys, SenderCustomization.SecretEnvVars);
		SenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
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
		SenderCustomization.OptionSchema = WebRTCSchema::Make();
		O3DSender::RegisterTransportCustomization(WebRTCConfig::TransportName, MoveTemp(SenderCustomization));

		// Register WebRTC receiver customization
		FO3DReceiverTransportCustomization ReceiverCustomization;
		WebRTCConfig::DeclareSecrets(ReceiverCustomization.SecretOptionKeys, ReceiverCustomization.SecretEnvVars);
		ReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
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
		ReceiverCustomization.OptionSchema = WebRTCSchema::Make();
		O3DReceiver::RegisterTransportCustomization(WebRTCConfig::TransportName, MoveTemp(ReceiverCustomization));

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module started (LiveKit FFI backend)"));
	}

	virtual void ShutdownModule() override
	{
		// Unregister transport customizations
		O3DSender::UnregisterTransportCustomization(WebRTCConfig::TransportName);
		O3DReceiver::UnregisterTransportCustomization(WebRTCConfig::TransportName);

		// Unregister transport factories
		O3DTransport::UnregisterSender(WebRTCConfig::TransportName);
		O3DTransport::UnregisterReceiver(WebRTCConfig::TransportName);

		// TRF-14: stop instances that outlive the module (components, LiveLink sources), then
		// unload. FO3DFfiLibrary keeps the DLL loaded if an instance is still referenced.
		if (Library.IsValid())
		{
			Library->StopLiveInstances();
			Library->Unload();
		}

		UE_LOG(LogO3DWebRTCSender, Log, TEXT("Open3D WebRTC transport module shut down"));
	}

private:
	/** livekit_ffi and the instances created from it. The registered factories hold a reference too. */
	TSharedPtr<FO3DFfiLibrary, ESPMode::ThreadSafe> Library;

	/**
	 * Location of livekit_ffi relative to the plugin that ships this module. WP-F11 moves the
	 * module to the Open3DBroadcastWebRTC add-on and changes OwningPluginName with it.
	 */
	static FO3DFfiLibraryDesc MakeLiveKitLibraryDesc()
	{
		FO3DFfiLibraryDesc Desc;
		Desc.DisplayName = TEXT("LiveKit FFI");
		Desc.OwningPluginName = TEXT("Open3DBroadcast");
		// Win64 only: O3D_WITH_TRANSPORT_WEBRTC is 0 on every other platform, so this file then
		// compiles to the stub module below (ADR 0001).
		Desc.RelativePath = TEXT("Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.dll");
		return Desc;
	}
};

#else // O3D_WITH_TRANSPORT_WEBRTC

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportWebRTCModule, Log, All);

/**
 * Stub module, compiled when O3D_WITH_TRANSPORT_WEBRTC is 0: the transport was switched off with that
 * environment variable, or the target platform has no prebuilt livekit_ffi
 * (O3DBuildFlags in Open3DBroadcastBuildFlags.Build.cs). It registers nothing.
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
