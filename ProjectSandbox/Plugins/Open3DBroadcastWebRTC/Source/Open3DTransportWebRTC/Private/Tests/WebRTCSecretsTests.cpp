// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

//
// WP-S9 (ADR 0004 items 1, 6 and 7) for the WebRTC transport: the descriptor declares the
// secret keys, the token reaches the transport only from Config.Secrets (never AdvancedParams), the
// token request carries no grants and sends the endpoint credential as a bearer header, and plain
// http:// endpoints other than localhost are refused. A fake token fetcher is used; no network.

#if WITH_DEV_AUTOMATION_TESTS

#include "../Shared/WebRTCTokenFetcher.h"
#include "../Shared/WebRTCTokenManager.h"
#include "../Shared/WebRTCUtils.h"

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportRegistry.h"
#include "Transport/O3DTransportTypes.h"

// Named namespace (not anonymous) so unity builds cannot collide with other test files.
namespace WebRTCS9Test
{
	/** Records fetch requests and never completes them. */
	class FRecordingTokenFetcher : public IO3DTokenFetcher
	{
	public:
		virtual void FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete) override
		{
			Requests.Add(Request);
		}

		virtual void CancelPendingRequests() override
		{
		}

		TArray<FO3DTokenFetchRequest> Requests;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSecretsDeclarationTest, "Open3DBroadcast.Transport.WebRTC.Secrets.CustomizationsUseDeclaredSecrets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSecretsDeclarationTest::RunTest(const FString& Parameters)
{
	const FString Token = TEXT("WEBRTC-CONFIG-TOKEN-1a2b3c");

	FO3DTransportRegistry& Registry = FO3DTransportRegistry::Get();

	TArray<FString> SenderKeys;
	TMap<FString, FString> SenderEnvVars;
	TestTrue(TEXT("Sender customization registered"), Registry.GetSecretDeclaration(TEXT("WebRTC"), EO3DTransportRole::Sender, SenderKeys, SenderEnvVars));
	TestTrue(TEXT("Sender declares webrtc.token"), SenderKeys.Contains(FString(WebRTCUtils::TokenOptionKey)));
	TestTrue(TEXT("Sender declares webrtc.tokenEndpointAuth"), SenderKeys.Contains(FString(WebRTCUtils::TokenEndpointAuthOptionKey)));
	TestEqual(TEXT("Token env var"), SenderEnvVars.FindRef(WebRTCUtils::TokenOptionKey), FString(WebRTCUtils::TokenEnvVar));

	TArray<FString> ReceiverKeys;
	TMap<FString, FString> ReceiverEnvVars;
	TestTrue(TEXT("Receiver customization registered"), Registry.GetSecretDeclaration(TEXT("WebRTC"), EO3DTransportRole::Receiver, ReceiverKeys, ReceiverEnvVars));
	TestTrue(TEXT("Receiver declares webrtc.token"), ReceiverKeys.Contains(FString(WebRTCUtils::TokenOptionKey)));
	TestTrue(TEXT("Receiver declares webrtc.tokenEndpointAuth"), ReceiverKeys.Contains(FString(WebRTCUtils::TokenEndpointAuthOptionKey)));

	const FO3DTransportDescriptorPtr Descriptor = Registry.Find(TEXT("WebRTC"));
	if (Descriptor.IsValid() && Descriptor->ConfigureSender)
	{
		FO3DTransportConfig Config;
		Config.Secrets.Add(WebRTCUtils::TokenOptionKey, Token);
		Descriptor->ConfigureSender(FO3DTransportOptionsView(), Config);
		TestEqual(TEXT("Sender: token taken from Config.Secrets"), WebRTCUtils::ReadTokenSettings(Config).ManualToken, Token);
		TestFalse(TEXT("Sender: token not in AdvancedParams"), Config.AdvancedParams.Contains(WebRTCUtils::TokenOptionKey));
	}

	if (Descriptor.IsValid() && Descriptor->ConfigureReceiver)
	{
		FO3DTransportConfig Config;
		Config.Secrets.Add(WebRTCUtils::TokenOptionKey, Token);
		Descriptor->ConfigureReceiver(FO3DTransportOptionsView(), Config);
		TestEqual(TEXT("Receiver: token taken from Config.Secrets"), WebRTCUtils::ReadTokenSettings(Config).ManualToken, Token);
		TestFalse(TEXT("Receiver: token not in AdvancedParams"), Config.AdvancedParams.Contains(WebRTCUtils::TokenOptionKey));
	}
	return true;
}

// WP-A1 PR 5c (ADR 0007 item 8): the schema's Secret entries are WebRTC's whole secret declaration,
// environment variables included.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSecretsSchemaEntriesTest, "Open3DBroadcast.Transport.WebRTC.Secrets.SchemaEntriesCarryEnvVars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSecretsSchemaEntriesTest::RunTest(const FString& Parameters)
{
	const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(TEXT("WebRTC"));
	if (!TestTrue(TEXT("WebRTC registered"), Descriptor.IsValid()))
	{
		return false;
	}

	for (const EO3DTransportRole Role : { EO3DTransportRole::Sender, EO3DTransportRole::Receiver })
	{
		const FString Side = LexToString(Role);
		const FO3DTransportRoleOptions& Options = Descriptor->GetRoleOptions(Role);

		const FO3DTransportOptionField* Token = Options.OptionSchema.FindByPredicate([](const FO3DTransportOptionField& Field) { return Field.Key == WebRTCUtils::TokenOptionKey; });
		const FO3DTransportOptionField* EndpointAuth = Options.OptionSchema.FindByPredicate([](const FO3DTransportOptionField& Field) { return Field.Key == WebRTCUtils::TokenEndpointAuthOptionKey; });
		if (TestTrue(*(Side + TEXT(": token entry")), Token != nullptr))
		{
			TestTrue(*(Side + TEXT(": token entry is Secret")), Token->Type == EO3DTransportOptionType::Secret);
			TestEqual(*(Side + TEXT(": token entry's env var")), Token->SecretEnvVar, FString(WebRTCUtils::TokenEnvVar));
		}
		if (TestTrue(*(Side + TEXT(": endpoint credential entry")), EndpointAuth != nullptr))
		{
			TestTrue(*(Side + TEXT(": endpoint credential entry is Secret")), EndpointAuth->Type == EO3DTransportOptionType::Secret);
			TestEqual(*(Side + TEXT(": endpoint credential entry's env var")), EndpointAuth->SecretEnvVar, FString(WebRTCUtils::TokenEndpointAuthEnvVar));
		}

		// What the hosts and the secret store get: the same two keys and variables as before.
		TArray<FString> Keys;
		TMap<FString, FString> EnvVars;
		FO3DTransportRegistry::Get().GetSecretDeclaration(TEXT("WebRTC"), Role, Keys, EnvVars);
		TestEqual(*(Side + TEXT(": two secret keys")), Keys.Num(), 2);
		TestEqual(*(Side + TEXT(": endpoint credential env var")), EnvVars.FindRef(WebRTCUtils::TokenEndpointAuthOptionKey), FString(WebRTCUtils::TokenEndpointAuthEnvVar));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSecretsRequestBodyTest, "Open3DBroadcast.Transport.WebRTC.Secrets.RequestBodyHasNoGrants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSecretsRequestBodyTest::RunTest(const FString& Parameters)
{
	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT("https://tokens.example.invalid/token");
	Request.RoomName = TEXT("stage");
	Request.Identity = TEXT("sender-1-abc");
	Request.Role = TEXT("publisher");
	Request.AuthBearer = TEXT("ENDPOINT-AUTH-SECRET-77");

	const FString Body = FO3DTokenFetcher::BuildRequestBody(Request);
	TestTrue(TEXT("Room sent"), Body.Contains(TEXT("\"room\"")));
	TestTrue(TEXT("Identity sent"), Body.Contains(TEXT("\"identity\"")));
	TestTrue(TEXT("Role sent"), Body.Contains(TEXT("\"role\"")));
	TestFalse(TEXT("No grants sent"), Body.Contains(TEXT("grants")));
	TestFalse(TEXT("No canPublish sent"), Body.Contains(TEXT("canPublish")));
	TestFalse(TEXT("Credential is not in the body"), Body.Contains(Request.AuthBearer));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSecretsEndpointPolicyTest, "Open3DBroadcast.Transport.WebRTC.Secrets.EndpointAuthAndHttpsPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebRTCSecretsEndpointPolicyTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCS9Test;

	TSharedRef<FRecordingTokenFetcher, ESPMode::ThreadSafe> Fetcher = MakeShared<FRecordingTokenFetcher, ESPMode::ThreadSafe>();
	const FO3DTokenFetcherFactory Factory = [Fetcher]() -> TSharedRef<IO3DTokenFetcher, ESPMode::ThreadSafe>
	{
		return Fetcher;
	};

	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.RoomName = TEXT("stage");
	Config.Identity = TEXT("sender-1-abc");
	Config.Role = EO3DTokenRole::Publisher;
	Config.EndpointAuth = TEXT("ENDPOINT-AUTH-SECRET-77");

	// Plain http:// to another host is refused before any request is made.
	{
		FO3DTokenManager Manager(Factory);
		Config.EndpointUrl = TEXT("http://tokens.example.invalid/token?key=QUERYSECRET");
		AddExpectedError(TEXT("refused: use https://"), EAutomationExpectedMessageFlags::Contains, 1);
		TestFalse(TEXT("Remote http:// refused"), Manager.Initialize(Config));
	}

	// https:// is accepted and the credential travels in the request, not in a log or the body.
	{
		FO3DTokenManager Manager(Factory);
		Config.EndpointUrl = TEXT("https://tokens.example.invalid/token");
		TestTrue(TEXT("https:// accepted"), Manager.Initialize(Config));
		Manager.RefreshTokenAsync();
		TestEqual(TEXT("One request"), Fetcher->Requests.Num(), 1);
		if (Fetcher->Requests.Num() == 1)
		{
			TestEqual(TEXT("Bearer credential passed to the fetcher"), Fetcher->Requests[0].AuthBearer, Config.EndpointAuth);
		}
		Manager.Reset();
	}

	// http:// to this machine is still allowed for local testing.
	{
		FO3DTokenManager Manager(Factory);
		Config.EndpointUrl = TEXT("http://127.0.0.1:8080/token");
		TestTrue(TEXT("Loopback http:// accepted"), Manager.Initialize(Config));
		Manager.Reset();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // O3D_WITH_TRANSPORT_WEBRTC
