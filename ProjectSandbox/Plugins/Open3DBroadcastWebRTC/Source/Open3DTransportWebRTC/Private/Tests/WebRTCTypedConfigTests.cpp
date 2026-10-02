// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

//
// WP-A1 PR 5a (ADR 0007 item 8, SHR-36): the LiveKit fields left FO3DTransportConfig. The values
// users saved were always the namespaced "webrtc.*" options of the sender component or receiver
// source (FO3DTransportConfig was never saved), so this checks that exactly such a saved option
// map, passed through the registered configure functions as the hosts do, still gives the
// transport the same URL, room and token settings, and that the token comes only from
// Config.Secrets (ADR 0004). No network, no LiveKit library.
//

#if WITH_DEV_AUTOMATION_TESTS

#include "../Shared/WebRTCUtils.h"

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportOptionsView.h"
#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DTransportTypes.h"

namespace WebRTCTypedConfigTest
{
	/** What a host does: copy the options (secrets excluded) into the config, add the schema, call the configure function. */
	static FO3DTransportConfig ConfigureLikeHost(const FO3DTransportDescriptor& Descriptor, EO3DTransportRole Role,
		const TMap<FString, FString>& SavedOptions, const TMap<FString, FString>& Secrets)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("WebRTC");
		Config.AdvancedParams = SavedOptions;
		Config.Secrets = Secrets;
		Config.OptionSchema = MakeShared<FO3DTransportOptionSchema>(Descriptor.GetRoleOptions(Role).OptionSchema);
		const FO3DTransportOptionsView Options(SavedOptions, Config.OptionSchema.Get());
		if (Role == EO3DTransportRole::Sender)
		{
			Descriptor.ConfigureSender(Options, Config);
		}
		else
		{
			Descriptor.ConfigureReceiver(Options, Config);
		}
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCTypedConfigSavedOptionsTest, "Open3DBroadcast.Transport.WebRTC.TypedConfig.SavedOptionsReachTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCTypedConfigSavedOptionsTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCTypedConfigTest;
	const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(TEXT("WebRTC"));
	if (!TestTrue(TEXT("WebRTC registered with both configure functions"),
		Descriptor.IsValid() && static_cast<bool>(Descriptor->ConfigureSender) && static_cast<bool>(Descriptor->ConfigureReceiver)))
	{
		return false;
	}

	// The option map an asset saved before this change holds for auto-fetch (keys as the panel
	// wrote them; values as a user typed them).
	TMap<FString, FString> AutoFetch;
	AutoFetch.Add(WebRTCUtils::UrlOptionKey, TEXT("  wss://livekit.example.invalid  "));
	AutoFetch.Add(WebRTCUtils::UseAutoTokenFetchOptionKey, TEXT("true"));
	AutoFetch.Add(WebRTCUtils::TokenEndpointUrlOptionKey, TEXT("https://tokens.example.invalid/token"));
	AutoFetch.Add(WebRTCUtils::TokenRefreshLeadTimeOptionKey, TEXT("120"));
	AutoFetch.Add(WebRTCUtils::RoomOptionKey, TEXT(" stage "));

	for (const EO3DTransportRole Role : { EO3DTransportRole::Sender, EO3DTransportRole::Receiver })
	{
		const TCHAR* RoleName = Role == EO3DTransportRole::Sender ? TEXT("sender") : TEXT("receiver");
		const FO3DTransportConfig Config = ConfigureLikeHost(*Descriptor, Role, AutoFetch, TMap<FString, FString>());
		const WebRTCUtils::FTokenSettings Token = WebRTCUtils::ReadTokenSettings(Config);
		TestEqual(*FString::Printf(TEXT("%s: Uri from webrtc.url, trimmed"), RoleName), Config.Uri, FString(TEXT("wss://livekit.example.invalid")));
		TestTrue(*FString::Printf(TEXT("%s: auto-fetch on"), RoleName), Token.bAutoFetch);
		TestEqual(*FString::Printf(TEXT("%s: endpoint"), RoleName), Token.EndpointUrl, FString(TEXT("https://tokens.example.invalid/token")));
		TestEqual(*FString::Printf(TEXT("%s: refresh lead time"), RoleName), Token.RefreshLeadTimeSec, 120);
		TestEqual(*FString::Printf(TEXT("%s: room, trimmed"), RoleName), WebRTCUtils::ResolveRoomName(Config.AdvancedParams), FString(TEXT("stage")));
		TestTrue(*FString::Printf(TEXT("%s: no token without the secret"), RoleName), Token.ManualToken.IsEmpty());
	}

	// Manual mode: the token is a secret (ADR 0004), resolved by the host into Config.Secrets.
	TMap<FString, FString> Manual;
	Manual.Add(WebRTCUtils::UrlOptionKey, TEXT("wss://livekit.example.invalid"));
	TMap<FString, FString> Secrets;
	const FString TokenValue = TEXT("WEBRTC-TYPED-TOKEN-3c9e");
	Secrets.Add(WebRTCUtils::TokenOptionKey, TokenValue);
	const FO3DTransportConfig ManualConfig = ConfigureLikeHost(*Descriptor, EO3DTransportRole::Sender, Manual, Secrets);
	const WebRTCUtils::FTokenSettings ManualToken = WebRTCUtils::ReadTokenSettings(ManualConfig);
	TestFalse(TEXT("Manual: auto-fetch off by default"), ManualToken.bAutoFetch);
	TestEqual(TEXT("Manual: token from Config.Secrets"), ManualToken.ManualToken, TokenValue);
	TestEqual(TEXT("Manual: default refresh lead time"), ManualToken.RefreshLeadTimeSec, WebRTCUtils::DefaultTokenRefreshLeadTimeSec);
	TestFalse(TEXT("Manual: token not copied into the options"), ManualConfig.AdvancedParams.Contains(WebRTCUtils::TokenOptionKey));
	TestFalse(TEXT("Manual: token not in the debug string"), ManualConfig.ToDebugString().Contains(TokenValue));

	// The schema the editor draws still declares every token option, with the old defaults.
	const FO3DTransportOptionsView Schemaed(Manual, &Descriptor->SenderOptions.OptionSchema);
	TestNotNull(TEXT("Schema declares webrtc.useAutoTokenFetch"), Schemaed.FindField(WebRTCUtils::UseAutoTokenFetchOptionKey));
	TestNotNull(TEXT("Schema declares webrtc.tokenEndpointUrl"), Schemaed.FindField(WebRTCUtils::TokenEndpointUrlOptionKey));
	TestEqual(TEXT("Schema default refresh lead time"), Schemaed.GetInt(WebRTCUtils::TokenRefreshLeadTimeOptionKey), WebRTCUtils::DefaultTokenRefreshLeadTimeSec);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
