// Copyright Lifelike & Believable. All Rights Reserved.

// FO3DSenderTransportSettings (WP-A3 step 8, SND-22, ADR 0004): option reads and writes with
// secret keys routed to the secret store, migration of secrets out of old data, switching options
// between transports (SND-35) and the edit-restart properties, reached through
// FO3DSenderTransportSettingsProbe against a sender transport registered by the test.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSenderTransportSettingsTests
{
	static const TCHAR* const TransportName = TEXT("O3DTransportSettingsTestSender");
	static const TCHAR* const OtherTransportName = TEXT("O3DTransportSettingsTestOther");
	static const TCHAR* const SecretKey = TEXT("o3dtransportsettingstest.token");
	static const TCHAR* const UrlKey = TEXT("o3dtransportsettingstest.url");

	/** Registers a sender transport declaring one secret key; unregisters and clears the store on exit. */
	struct FScopedTestTransport
	{
		FScopedTestTransport()
		{
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = TransportName;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
			FO3DTransportOptionField Secret;
			Secret.Key = SecretKey;
			Secret.Type = EO3DTransportOptionType::Secret;
			Secret.SecretEnvVar = TEXT("O3DB_TRANSPORTSETTINGSTEST_TOKEN");
			Descriptor.SenderOptions.OptionSchema.Add(Secret);
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
			ClearStore();
		}
		~FScopedTestTransport()
		{
			ClearStore();
			Registration.Reset();
		}
		static void ClearStore()
		{
			FO3DSecretStore::Get().Clear(TransportName, FO3DSecretStore::NormalizeProfile(FString()), SecretKey);
		}
		FO3DTransportRegistration Registration;
	};

	/** The secret a transport config would get, from the store. */
	FString ResolvedSecret(const TMap<FString, FString>& Options)
	{
		TMap<FString, FString> ConfigOptions;
		TMap<FString, FString> Secrets;
		FO3DSenderTransportSettingsProbe::BuildConfigOptions(Options, TransportName, ConfigOptions, Secrets);
		return Secrets.FindRef(SecretKey);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTransportSettingsOptionsTest, "Open3DBroadcast.Sender.TransportSettings.RoutesSecretsOutOfOptions", O3DB_TEST_FLAGS)
bool FO3DSenderTransportSettingsOptionsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTransportSettingsTests;
	FScopedTestTransport Transport;
	using FProbe = FO3DSenderTransportSettingsProbe;

	TestTrue(TEXT("The declared key is a secret"), FProbe::IsSecretKey(TransportName, SecretKey));
	TestFalse(TEXT("Another key is not"), FProbe::IsSecretKey(TransportName, UrlKey));
	TestFalse(TEXT("Nothing is a secret of an unregistered transport"), FProbe::IsSecretKey(OtherTransportName, SecretKey));

	// A plain option is stored in the map, recorded for undo first; empty removes it.
	TMap<FString, FString> Options;
	TestEqual(TEXT("A plain option is recorded for undo"), FProbe::SetOption(Options, TransportName, UrlKey, TEXT("wss://a")), 1);
	TestEqual(TEXT("And read back"), FProbe::GetOption(Options, TransportName, UrlKey), FString(TEXT("wss://a")));
	TestEqual(TEXT("An empty key does nothing"), FProbe::SetOption(Options, TransportName, FString(), TEXT("x")), 0);

	// A secret goes to the session store, never into the map, and is never read back.
	TestEqual(TEXT("A secret is not recorded for undo"), FProbe::SetOption(Options, TransportName, SecretKey, TEXT("s3cr3t")), 0);
	TestFalse(TEXT("Nor stored in the options"), Options.Contains(SecretKey));
	TestTrue(TEXT("Nor read back"), FProbe::GetOption(Options, TransportName, SecretKey).IsEmpty());
	TestEqual(TEXT("The transport config gets it from the store"), ResolvedSecret(Options), FString(TEXT("s3cr3t")));

	// A copy older data left in the map is removed when the secret is set, recorded for undo.
	Options.Add(SecretKey, TEXT("old"));
	TestEqual(TEXT("Removing an old copy is recorded"), FProbe::SetOption(Options, TransportName, SecretKey, TEXT("new")), 1);
	TestFalse(TEXT("The old copy is gone"), Options.Contains(SecretKey));
	TMap<FString, FString> ConfigOptions;
	TMap<FString, FString> Secrets;
	Options.Add(SecretKey, TEXT("stray"));
	FProbe::BuildConfigOptions(Options, TransportName, ConfigOptions, Secrets);
	TestFalse(TEXT("A secret key is never copied into the config's options"), ConfigOptions.Contains(SecretKey));
	TestEqual(TEXT("Plain options are"), ConfigOptions.FindRef(UrlKey), FString(TEXT("wss://a")));

	TestEqual(TEXT("Removing a plain option is recorded"), FProbe::SetOption(Options, TransportName, UrlKey, FString()), 1);
	TestFalse(TEXT("And removes it"), Options.Contains(UrlKey));

	// The credential profile is an option of its own, normalized.
	TMap<FString, FString> WithProfile;
	WithProfile.Add(FO3DSecretStore::MakeCredentialProfileOptionKey(TransportName), TEXT(" Stage "));
	TestEqual(TEXT("Credential profile"), FProbe::GetCredentialProfile(WithProfile, TransportName), FO3DSecretStore::NormalizeProfile(TEXT(" Stage ")));
	TestEqual(TEXT("Default profile"), FProbe::GetCredentialProfile(Options, TransportName), FO3DSecretStore::NormalizeProfile(FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTransportSettingsMigrateTest, "Open3DBroadcast.Sender.TransportSettings.MigratesAndSwitchesOptions", O3DB_TEST_FLAGS)
bool FO3DSenderTransportSettingsMigrateTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTransportSettingsTests;
	FScopedTestTransport Transport;
	using FProbe = FO3DSenderTransportSettingsProbe;

	// Old data with a secret in the options: it moves to the session store.
	TMap<FString, FString> Options;
	Options.Add(UrlKey, TEXT("wss://a"));
	Options.Add(SecretKey, TEXT("legacy"));
	TestTrue(TEXT("The secret key is moved"), FProbe::MigrateLegacySecrets(Options, TransportName) == TArray<FString>({ SecretKey }));
	TestFalse(TEXT("Out of the options"), Options.Contains(SecretKey));
	TestTrue(TEXT("Plain options stay"), Options.Contains(UrlKey));
	TestEqual(TEXT("Into the store"), ResolvedSecret(Options), FString(TEXT("legacy")));

	// A value set this session wins over one found in old data.
	FProbe::SetOption(Options, TransportName, SecretKey, TEXT("session"));
	Options.Add(SecretKey, TEXT("older"));
	TestEqual(TEXT("Moved again"), FProbe::MigrateLegacySecrets(Options, TransportName).Num(), 1);
	TestEqual(TEXT("The session value is kept"), ResolvedSecret(Options), FString(TEXT("session")));
	TestEqual(TEXT("Nothing to move without a secret in the map"), FProbe::MigrateLegacySecrets(Options, TransportName).Num(), 0);
	TMap<FString, FString> Unregistered;
	Unregistered.Add(SecretKey, TEXT("x"));
	TestEqual(TEXT("Nothing is moved for an unregistered transport"), FProbe::MigrateLegacySecrets(Unregistered, OtherTransportName).Num(), 0);

	// SND-35: the outgoing transport's options are put away and come back.
	TMap<FName, FO3DTransportOptionSet> Inactive;
	FProbe::SwitchOptions(Options, Inactive, TransportName, OtherTransportName);
	TestEqual(TEXT("The other transport starts with no options"), Options.Num(), 0);
	TestTrue(TEXT("The outgoing ones are kept"), Inactive.Contains(FName(TransportName)) && Inactive[FName(TransportName)].Options.Contains(UrlKey));
	Options.Add(TEXT("other.key"), TEXT("1"));
	FProbe::SwitchOptions(Options, Inactive, OtherTransportName, TransportName);
	TestEqual(TEXT("Switching back restores them"), Options.FindRef(UrlKey), FString(TEXT("wss://a")));
	TestFalse(TEXT("An unregistered transport's options are not kept (its secrets are unknown)"), Inactive.Contains(FName(OtherTransportName)));

	// Edits that restart capture.
	TestTrue(TEXT("TransportName restarts"), FProbe::IsRestartProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName)));
	TestTrue(TEXT("AudioCaptureConfig restarts"), FProbe::IsRestartProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig)));
	TestFalse(TEXT("TransportOptions does not"), FProbe::IsRestartProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportOptions)));
	TestFalse(TEXT("CurveEpsilon does not"), FProbe::IsRestartProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CurveEpsilon)));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
