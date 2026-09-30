// Copyright Lifelike & Believable. All Rights Reserved.

// WP-S9 (ADR 0004): secret store resolution order, redaction helpers and the token endpoint URL
// policy. No network, no disk: the store under test gets a fake environment and a fake per-user
// settings backend. Self-contained so it can move to the Open3DBroadcastTests module (WP-T2).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DHelpers.h"
#include "O3DRedact.h"
#include "O3DSecretStore.h"
#include "O3DTransportTypes.h"

namespace O3DSecretsTestUtil
{
	/** In-memory stand-in for the per-user EditorPerProjectUserSettings store (R3). */
	class FFakeSecretUserStore final : public IO3DSecretUserStore
	{
	public:
		virtual bool Load(const FString& CompositeKey, FString& OutValue) const override
		{
			++LoadCount;
			if (const FString* Existing = Values.Find(CompositeKey))
			{
				OutValue = *Existing;
				return true;
			}
			return false;
		}

		virtual void Save(const FString& CompositeKey, const FString& Value) override
		{
			Values.Add(CompositeKey, Value);
		}

		virtual void Remove(const FString& CompositeKey) override
		{
			Values.Remove(CompositeKey);
		}

		TMap<FString, FString> Values;
		mutable int32 LoadCount = 0;
	};

	/** Environment stand-in: a map of variable name to value. */
	struct FFakeEnvironment
	{
		TSharedRef<TMap<FString, FString>> Vars = MakeShared<TMap<FString, FString>>();

		FO3DSecretStore::FEnvironmentReader MakeReader() const
		{
			TSharedRef<TMap<FString, FString>> Captured = Vars;
			return [Captured](const FString& Name) -> FString
			{
				const FString* Value = Captured->Find(Name);
				return Value ? *Value : FString();
			};
		}
	};

	static const TCHAR* const Transport = TEXT("SecretsTestTransport");
	static const TCHAR* const Key = TEXT("secretstesttransport.token");
	static const TCHAR* const EnvBase = TEXT("O3DB_SECRETSTEST_TOKEN");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsRedactUrlTest, "Open3DBroadcast.Shared.Secrets.RedactUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsRedactUrlTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Plain URL unchanged"),
		O3DRedact::Url(TEXT("wss://livekit.example.com:7880/rtc")), FString(TEXT("wss://livekit.example.com:7880/rtc")));
	TestEqual(TEXT("Query values redacted, names kept"),
		O3DRedact::Url(TEXT("https://relay.example.com:443/moq?jwt=abc.def&room=stage")),
		FString(TEXT("https://relay.example.com:443/moq?jwt=<redacted>&room=<redacted>")));
	TestEqual(TEXT("User-info and fragment redacted"),
		O3DRedact::Url(TEXT("https://user:pw@host.example.com/token#frag")),
		FString(TEXT("https://<redacted>@host.example.com/token#<redacted>")));
	TestEqual(TEXT("Bare query component redacted"),
		O3DRedact::Url(TEXT("tcp://127.0.0.1:9000?s3cr3t")), FString(TEXT("tcp://127.0.0.1:9000?<redacted>")));
	TestEqual(TEXT("No scheme keeps host and path"),
		O3DRedact::Url(TEXT("host:1234/path?token=abc")), FString(TEXT("host:1234/path?token=<redacted>")));
	TestEqual(TEXT("'@' in the path is not user-info"),
		O3DRedact::Url(TEXT("https://host/a@b")), FString(TEXT("https://host/a@b")));
	TestEqual(TEXT("Empty stays empty"), O3DRedact::Url(FString()), FString());

	const FString Redacted = O3DRedact::Url(TEXT("wss://u:hunter2@h/p?token=abc123#xyz789"));
	TestFalse(TEXT("Password gone"), Redacted.Contains(TEXT("hunter2")));
	TestFalse(TEXT("Query value gone"), Redacted.Contains(TEXT("abc123")));
	TestFalse(TEXT("Fragment gone"), Redacted.Contains(TEXT("xyz789")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsRedactValueTest, "Open3DBroadcast.Shared.Secrets.RedactValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsRedactValueTest::RunTest(const FString& Parameters)
{
	const TCHAR* const SensitiveKeys[] =
	{
		TEXT("example.token"), TEXT("x.Secret"), TEXT("PASSWORD"), TEXT("db.passwd"), TEXT("tokenEndpointAuth"),
		TEXT("my.credential"), TEXT("jwt"), TEXT("x.apikey"), TEXT("X_API_KEY"), TEXT("tls.key"), TEXT("client_key"),
	};
	for (const TCHAR* Key : SensitiveKeys)
	{
		TestTrue(FString::Printf(TEXT("'%s' is sensitive"), Key), O3DRedact::IsSensitiveKey(Key));
		TestEqual(FString::Printf(TEXT("'%s' value redacted"), Key), O3DRedact::Value(Key, TEXT("v")), FString(O3DRedact::Marker()));
	}

	const TCHAR* const PlainKeys[] = { TEXT("example.url"), TEXT("udp.maxdatagram"), TEXT("moq.relay"), TEXT("keyframe"), TEXT("") };
	for (const TCHAR* Key : PlainKeys)
	{
		TestFalse(FString::Printf(TEXT("'%s' is not sensitive"), Key), O3DRedact::IsSensitiveKey(Key));
		TestEqual(FString::Printf(TEXT("'%s' value kept"), Key), O3DRedact::Value(Key, TEXT("v")), FString(TEXT("v")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsToDebugStringTest, "Open3DBroadcast.Shared.Secrets.ToDebugString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsToDebugStringTest::RunTest(const FString& Parameters)
{
	const FString SecretToken = TEXT("eyJTOKENVALUE.payload.sig");
	const FString QueryToken = TEXT("abcQUERYVALUE");
	const FString ApiKey = TEXT("APIKEYVALUE42");
	const FString Legacy = TEXT("LEGACYTOKENFIELD");

	FO3DTransportConfig Config;
	Config.Transport = TEXT("SecretsTestTransport");
	Config.Uri = FString::Printf(TEXT("wss://host.example.com/path?token=%s"), *QueryToken);
	Config.Secrets.Add(O3DSecretsTestUtil::Key, SecretToken);
	Config.AdvancedParams.Add(TEXT("x.apikey"), ApiKey);
	Config.AdvancedParams.Add(TEXT("x.url"), FString::Printf(TEXT("https://h/?sig=%s"), *QueryToken));
	Config.AdvancedParams.Add(TEXT("x.plain"), TEXT("visible-value"));
	Config.Token = Legacy;

	const FString Debug = Config.ToDebugString();
	TestFalse(TEXT("Secret value absent"), Debug.Contains(SecretToken));
	TestFalse(TEXT("URI query value absent"), Debug.Contains(QueryToken));
	TestFalse(TEXT("Sensitive AdvancedParams value absent"), Debug.Contains(ApiKey));
	TestFalse(TEXT("Token field absent"), Debug.Contains(Legacy));
	TestTrue(TEXT("Secret key shown with presence only"),
		Debug.Contains(FString::Printf(TEXT("%s=<set>"), O3DSecretsTestUtil::Key)));
	TestTrue(TEXT("Host still shown"), Debug.Contains(TEXT("host.example.com")));
	TestTrue(TEXT("Non-sensitive value still shown"), Debug.Contains(TEXT("visible-value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsResolutionOrderTest, "Open3DBroadcast.Shared.Secrets.ResolutionOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsResolutionOrderTest::RunTest(const FString& Parameters)
{
	using namespace O3DSecretsTestUtil;

	FFakeEnvironment Env;
	TSharedRef<FFakeSecretUserStore> UserStore = MakeShared<FFakeSecretUserStore>();
	FO3DSecretStore Store(Env.MakeReader(), UserStore);
	const FString Profile = TEXT("default");

	TestFalse(TEXT("Nothing resolves when nothing is set"), Store.Resolve(Transport, Profile, Key, EnvBase).IsSet());

	// R3 only.
	Store.Set(Transport, Profile, Key, TEXT("from-user-settings"), EO3DSecretPersistence::RememberOnThisMachine);
	Store.SetPersistence(Transport, Profile, Key, EO3DSecretPersistence::RememberOnThisMachine);
	{
		// Drop the session copy but keep the remembered one, as after an editor restart.
		FO3DSecretStore Restarted(Env.MakeReader(), UserStore);
		TOptional<FO3DResolvedSecret> Resolved = Restarted.Resolve(Transport, Profile, Key, EnvBase);
		TestTrue(TEXT("Remembered value resolves after restart"), Resolved.IsSet());
		if (Resolved.IsSet())
		{
			TestEqual(TEXT("Remembered value"), Resolved->Value, FString(TEXT("from-user-settings")));
			TestTrue(TEXT("Source is user settings"), Resolved->Source == EO3DSecretSource::UserSettings);
		}

		// Environment beats user settings.
		Env.Vars->Add(EnvBase, TEXT("from-env"));
		Resolved = Restarted.Resolve(Transport, Profile, Key, EnvBase);
		TestTrue(TEXT("Environment resolves"), Resolved.IsSet() && Resolved->Source == EO3DSecretSource::Environment);
		TestTrue(TEXT("Environment value"), Resolved.IsSet() && Resolved->Value == TEXT("from-env"));
		TestTrue(TEXT("Environment variable named"), Resolved.IsSet() && Resolved->EnvVarName == EnvBase);

		// Session beats environment.
		Restarted.Set(Transport, Profile, Key, TEXT("from-session"));
		Resolved = Restarted.Resolve(Transport, Profile, Key, EnvBase);
		TestTrue(TEXT("Session resolves"), Resolved.IsSet() && Resolved->Source == EO3DSecretSource::Session);
		TestTrue(TEXT("Session value"), Resolved.IsSet() && Resolved->Value == TEXT("from-session"));
	}

	// Profile-suffixed variable beats the plain one; a different profile does not see the session value.
	Env.Vars->Add(FString(EnvBase) + TEXT("__STAGE_B"), TEXT("from-env-profile"));
	TOptional<FO3DResolvedSecret> ProfileResolved = Store.Resolve(Transport, TEXT("stage-b"), Key, EnvBase);
	TestTrue(TEXT("Profile env var resolves"), ProfileResolved.IsSet() && ProfileResolved->Value == TEXT("from-env-profile"));
	TestTrue(TEXT("Profile env var named"), ProfileResolved.IsSet() && ProfileResolved->EnvVarName == FString(EnvBase) + TEXT("__STAGE_B"));

	// Transport name and profile are matched case-insensitively; keys are exact.
	Store.Set(Transport, Profile, Key, TEXT("from-session"));
	TestTrue(TEXT("Transport case-insensitive"), Store.Resolve(TEXT("secretstesttransport"), TEXT("DEFAULT"), Key).IsSet());
	TestTrue(TEXT("Empty profile is default"), Store.Resolve(Transport, TEXT(""), Key).IsSet());

	// A packaged-style store (no user-settings backend) never reads R3.
	{
		FFakeEnvironment EmptyEnv;
		const int32 LoadsBefore = UserStore->LoadCount;
		FO3DSecretStore Packaged(EmptyEnv.MakeReader(), nullptr);
		TestFalse(TEXT("Packaged store has no R3"), Packaged.SupportsRememberOnThisMachine());
		Packaged.Set(Transport, Profile, Key, TEXT("session-only"), EO3DSecretPersistence::RememberOnThisMachine);
		TestTrue(TEXT("Packaged store keeps the session value"), Packaged.Resolve(Transport, Profile, Key).IsSet());
		FO3DSecretStore PackagedRestarted(EmptyEnv.MakeReader(), nullptr);
		TestFalse(TEXT("Nothing survives a packaged restart"), PackagedRestarted.Resolve(Transport, Profile, Key, EnvBase).IsSet());
		TestEqual(TEXT("R3 backend never read"), UserStore->LoadCount, LoadsBefore);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsPersistenceTest, "Open3DBroadcast.Shared.Secrets.PersistenceAndClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsPersistenceTest::RunTest(const FString& Parameters)
{
	using namespace O3DSecretsTestUtil;

	FFakeEnvironment Env;
	TSharedRef<FFakeSecretUserStore> UserStore = MakeShared<FFakeSecretUserStore>();
	FO3DSecretStore Store(Env.MakeReader(), UserStore);
	const FString Profile = TEXT("default");

	Store.Set(Transport, Profile, Key, TEXT("value-1"));
	TestEqual(TEXT("Session Set writes nothing to R3"), UserStore->Values.Num(), 0);
	TestTrue(TEXT("Session value present"), Store.HasSessionValue(Transport, Profile, Key));

	TestTrue(TEXT("Promote to remembered"), Store.SetPersistence(Transport, Profile, Key, EO3DSecretPersistence::RememberOnThisMachine));
	TestEqual(TEXT("Remembered copy written"), UserStore->Values.Num(), 1);
	FO3DSecretStatus Status = Store.Describe(Transport, Profile, Key, EnvBase);
	TestTrue(TEXT("Status: session wins"), Status.Source == EO3DSecretSource::Session);
	TestTrue(TEXT("Status: remembered"), Status.bRemembered);

	TestTrue(TEXT("Demote to session"), Store.SetPersistence(Transport, Profile, Key, EO3DSecretPersistence::Session));
	TestEqual(TEXT("Remembered copy removed"), UserStore->Values.Num(), 0);

	Store.Set(Transport, Profile, Key, TEXT("value-2"), EO3DSecretPersistence::RememberOnThisMachine);
	TestEqual(TEXT("Remember writes R3"), UserStore->Values.Num(), 1);
	Store.Clear(Transport, Profile, Key);
	TestFalse(TEXT("Clear removes the session value"), Store.HasSessionValue(Transport, Profile, Key));
	TestEqual(TEXT("Clear removes the remembered copy"), UserStore->Values.Num(), 0);
	TestFalse(TEXT("Nothing resolves after Clear"), Store.Resolve(Transport, Profile, Key, EnvBase).IsSet());

	Store.Set(Transport, Profile, Key, TEXT("value-3"));
	Store.Set(Transport, Profile, Key, FString());
	TestFalse(TEXT("Empty Set clears"), Store.HasSessionValue(Transport, Profile, Key));

	Env.Vars->Add(EnvBase, TEXT("env-value"));
	Status = Store.Describe(Transport, Profile, Key, EnvBase);
	TestTrue(TEXT("Status: environment"), Status.Source == EO3DSecretSource::Environment);
	TestEqual(TEXT("Status names the variable"), Status.EnvVarName, FString(EnvBase));

	TestFalse(TEXT("No session value to promote"), Store.SetPersistence(Transport, Profile, Key, EO3DSecretPersistence::RememberOnThisMachine));

	TMap<FString, FString> Resolved;
	TMap<FString, FString> EnvVars;
	EnvVars.Add(Key, EnvBase);
	Store.ResolveAll(Transport, Profile, { FString(Key), FString(TEXT("secretstesttransport.unset")) }, EnvVars, Resolved);
	TestEqual(TEXT("ResolveAll returns only resolved keys"), Resolved.Num(), 1);
	TestEqual(TEXT("ResolveAll value"), Resolved.FindRef(Key), FString(TEXT("env-value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsNamingTest, "Open3DBroadcast.Shared.Secrets.ProfileAndEnvVarNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsNamingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Profile option key"), FO3DSecretStore::MakeCredentialProfileOptionKey(TEXT("ExampleTransport")), FString(TEXT("exampletransport.credentialProfile")));
	TestEqual(TEXT("Blank profile"), FO3DSecretStore::NormalizeProfile(TEXT("  ")), FString(TEXT("default")));
	TestEqual(TEXT("Trimmed profile"), FO3DSecretStore::NormalizeProfile(TEXT(" stage ")), FString(TEXT("stage")));

	const TArray<FString> DefaultCandidates = FO3DSecretStore::GetEnvVarCandidates(TEXT("O3DB_X_TOKEN"), TEXT("default"));
	TestEqual(TEXT("Default profile: plain name only"), DefaultCandidates.Num(), 1);
	const TArray<FString> StageCandidates = FO3DSecretStore::GetEnvVarCandidates(TEXT("O3DB_X_TOKEN"), TEXT("stage.1"));
	TestEqual(TEXT("Named profile: two names"), StageCandidates.Num(), 2);
	if (StageCandidates.Num() == 2)
	{
		TestEqual(TEXT("Suffixed first"), StageCandidates[0], FString(TEXT("O3DB_X_TOKEN__STAGE_1")));
		TestEqual(TEXT("Plain second"), StageCandidates[1], FString(TEXT("O3DB_X_TOKEN")));
	}
	TestEqual(TEXT("No base, no names"), FO3DSecretStore::GetEnvVarCandidates(FString(), TEXT("stage")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedSecretsEndpointPolicyTest, "Open3DBroadcast.Shared.Secrets.TokenEndpointUrlPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSharedSecretsEndpointPolicyTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("https"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("https://tokens.example.com/token")));
	TestTrue(TEXT("HTTPS upper case"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("HTTPS://tokens.example.com")));
	TestTrue(TEXT("http localhost"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://localhost:8080/token")));
	TestTrue(TEXT("http 127.0.0.1"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://127.0.0.1:8080/token")));
	TestTrue(TEXT("http [::1]"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://[::1]:8080/token")));
	TestFalse(TEXT("http remote"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://tokens.example.com/token")));
	TestFalse(TEXT("http localhost prefix trick"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://localhost.example.com/token")));
	TestFalse(TEXT("http user-info trick"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://localhost@evil.example.com/token")));
	TestFalse(TEXT("http LAN address"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("http://192.168.1.10:8080/token")));
	TestFalse(TEXT("No scheme"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("tokens.example.com/token")));
	TestFalse(TEXT("Other scheme"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("ftp://localhost/token")));
	TestFalse(TEXT("https without host"), O3DHelpers::IsHttpsOrLoopbackHttpUrl(TEXT("https:///token")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
