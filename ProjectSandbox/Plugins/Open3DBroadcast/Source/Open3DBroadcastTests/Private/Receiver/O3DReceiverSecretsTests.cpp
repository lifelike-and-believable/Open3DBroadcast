// Copyright Lifelike & Believable. All Rights Reserved.

// WP-S9 (ADR 0004): a receiver secret never reaches the LiveLink connection string or the
// GameUserSettings.ini text, is resolved into FO3DTransportConfig::Secrets when the source builds
// its transport config, and a legacy connection string that still carries one is migrated with a
// Warning that never contains the value.
//
// Uses a test-only transport name and key, so no transport module (and no LiveKit key name) is
// involved. No network, no ini file is written. Self-contained so it can move to the
// Open3DBroadcastTests module (WP-T2).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceFactory.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSecretStore.h"

/** White-box access to FO3DReceiverSource for the WP-S9 tests (befriended in O3DReceiverSource.h). */
struct FO3DReceiverSecretsTestAccess
{
	static FO3DTransportConfig BuildTransportConfig(const FO3DReceiverSource& Source)
	{
		return Source.BuildTransportConfig();
	}
};

namespace O3DReceiverSecretsTestUtil
{
	static const TCHAR* const TransportName = TEXT("O3DSecretsTestReceiver");
	static const TCHAR* const SecretKey = TEXT("o3dsecretstestreceiver.token");
	static const TCHAR* const UrlKey = TEXT("o3dsecretstestreceiver.url");
	static const TCHAR* const EnvVar = TEXT("O3DB_SECRETSTEST_RECEIVER_TOKEN");

	/** Registers a receiver customization declaring one secret key; unregisters and clears the store on exit. */
	struct FScopedSecretsTestTransport
	{
		FScopedSecretsTestTransport()
		{
			FO3DReceiverTransportCustomization Customization;
			Customization.SecretOptionKeys.Add(SecretKey);
			Customization.SecretEnvVars.Add(SecretKey, EnvVar);
			O3DReceiver::RegisterTransportCustomization(TransportName, MoveTemp(Customization));
			ClearStore();
		}

		~FScopedSecretsTestTransport()
		{
			ClearStore();
			O3DReceiver::UnregisterTransportCustomization(TransportName);
		}

		static void ClearStore()
		{
			FO3DSecretStore::Get().Clear(TransportName, TEXT("default"), SecretKey);
		}
	};

	/** Captures every log line while alive, so a test can assert a value was never logged. */
	class FSecretsTestLogCapture final : public FOutputDevice
	{
	public:
		FSecretsTestLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FSecretsTestLogCapture() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Mutex);
			Lines.Add(FString(V));
		}

		virtual bool CanBeUsedOnAnyThread() const override
		{
			return true;
		}

		bool AnyLineContains(const FString& Needle)
		{
			GLog->Flush();
			FScopeLock Lock(&Mutex);
			for (const FString& Line : Lines)
			{
				if (Line.Contains(Needle, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

	private:
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	static FO3DReceiverSourceConfig MakeSettings(const FString& Token)
	{
		FO3DReceiverSourceConfig Settings;
		Settings.TransportName = TransportName;
		Settings.TransportOptions.Add(UrlKey, TEXT("wss://example.invalid"));
		if (!Token.IsEmpty())
		{
			Settings.TransportOptions.Add(SecretKey, Token);
		}
		return Settings;
	}

	/** Exports UO3DReceiverSettingsObject::Settings the way SaveConfig writes the property value to the ini. */
	static FString ExportSettingsPropertyText(const UO3DReceiverSettingsObject* Object)
	{
		const FProperty* Property = UO3DReceiverSettingsObject::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UO3DReceiverSettingsObject, Settings));
		FString Text;
		if (Property)
		{
			Property->ExportText_InContainer(0, Text, Object, nullptr, const_cast<UO3DReceiverSettingsObject*>(Object), PPF_None);
		}
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSecretsPersistenceTest, "Open3DBroadcast.Receiver.Secrets.ConnectionStringAndIniTextHaveNoToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverSecretsPersistenceTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverSecretsTestUtil;
	FScopedSecretsTestTransport ScopedTransport;

	const FString Token = TEXT("RECEIVER-PERSIST-TOKEN-3b9e11");
	FO3DReceiverSourceConfig Settings = MakeSettings(Token);

	TestTrue(TEXT("Secret key is declared"), O3DReceiver::IsSecretOptionKey(TransportName, SecretKey));

	// The connection string the factory hands to LiveLink (and LiveLink presets store).
	const FString ConnectionString = O3DReceiver::ExportConnectionString(Settings);
	TestTrue(TEXT("Sanity: connection string carries the plain option"), ConnectionString.Contains(TEXT("wss://example.invalid")));
	TestFalse(TEXT("Token absent from the connection string"), ConnectionString.Contains(Token));
	TestTrue(TEXT("Input settings are not modified by the export"), Settings.TransportOptions.Contains(SecretKey));

	// What the factory saves to GameUserSettings.ini: the settings after the same strip/migration
	// step, exported as the Settings property text. No ini file is written by this test.
	FO3DReceiverSourceConfig Persisted = Settings;
	AddExpectedError(TEXT("Moved credential option"), EAutomationExpectedMessageFlags::Contains, 1);
	{
		FSecretsTestLogCapture Capture;
		TestEqual(TEXT("One key moved"), O3DReceiver::MigrateLegacySecretOptions(Persisted, TEXT("the receiver source settings")), 1);
		TestFalse(TEXT("The Warning does not contain the value"), Capture.AnyLineContains(Token));
	}
	UO3DReceiverSettingsObject* SettingsObject = NewObject<UO3DReceiverSettingsObject>(GetTransientPackage());
	SettingsObject->Settings = Persisted;
	const FString IniText = ExportSettingsPropertyText(SettingsObject);
	TestTrue(TEXT("Sanity: ini text carries the plain option"), IniText.Contains(TEXT("wss://example.invalid")));
	TestFalse(TEXT("Token absent from the ini text"), IniText.Contains(Token));

	TOptional<FO3DResolvedSecret> Resolved = FO3DSecretStore::Get().Resolve(TransportName, TEXT("default"), SecretKey);
	TestTrue(TEXT("Token kept for this session"), Resolved.IsSet() && Resolved->Value == Token);

	// The source resolves it into Config.Secrets, not AdvancedParams.
	TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Persisted);
	const FO3DTransportConfig Config = FO3DReceiverSecretsTestAccess::BuildTransportConfig(*Source);
	TestEqual(TEXT("Secret resolved into Config.Secrets"), Config.Secrets.FindRef(SecretKey), Token);
	TestFalse(TEXT("Secret not in AdvancedParams"), Config.AdvancedParams.Contains(SecretKey));
	TestFalse(TEXT("ToDebugString does not show the secret"), Config.ToDebugString().Contains(Token));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSecretsMigrationTest, "Open3DBroadcast.Receiver.Secrets.CreateSourceMigratesLegacyConnectionString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DReceiverSecretsMigrationTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverSecretsTestUtil;
	FScopedSecretsTestTransport ScopedTransport;

	// A connection string written before WP-S9, with the secret inside.
	const FString LegacyToken = TEXT("RECEIVER-LEGACY-TOKEN-6d2f40");
	const FO3DReceiverSourceConfig LegacySettings = MakeSettings(LegacyToken);
	FString LegacyConnectionString;
	FO3DReceiverSourceConfig::StaticStruct()->ExportText(LegacyConnectionString, &LegacySettings, nullptr, nullptr, PPF_None, nullptr);
	TestTrue(TEXT("Sanity: legacy string carries the token"), LegacyConnectionString.Contains(LegacyToken));

	UO3DReceiverSourceFactory* Factory = NewObject<UO3DReceiverSourceFactory>(GetTransientPackage());
	TSharedPtr<ILiveLinkSource> CreatedSource;
	AddExpectedError(TEXT("Moved credential option"), EAutomationExpectedMessageFlags::Contains, 1);
	{
		FSecretsTestLogCapture Capture;
		CreatedSource = Factory->CreateSource(LegacyConnectionString);
		TestFalse(TEXT("The Warning does not contain the value"), Capture.AnyLineContains(LegacyToken));
		TestTrue(TEXT("The Warning names the source"), Capture.AnyLineContains(TEXT("LiveLink connection string")));
	}

	TestTrue(TEXT("Source created"), CreatedSource.IsValid());
	if (CreatedSource.IsValid())
	{
		const TSharedPtr<FO3DReceiverSource> Source = StaticCastSharedPtr<FO3DReceiverSource>(CreatedSource);
		TestFalse(TEXT("Secret removed from the source settings"), Source->GetSourceSettings().TransportOptions.Contains(SecretKey));
		TestTrue(TEXT("Plain option kept"), Source->GetSourceSettings().TransportOptions.Contains(UrlKey));
		TestFalse(TEXT("Re-exported connection string has no token"),
			O3DReceiver::ExportConnectionString(Source->GetSourceSettings()).Contains(LegacyToken));

		const FO3DTransportConfig Config = FO3DReceiverSecretsTestAccess::BuildTransportConfig(*Source);
		TestEqual(TEXT("Migrated token still used for this session"), Config.Secrets.FindRef(SecretKey), LegacyToken);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
