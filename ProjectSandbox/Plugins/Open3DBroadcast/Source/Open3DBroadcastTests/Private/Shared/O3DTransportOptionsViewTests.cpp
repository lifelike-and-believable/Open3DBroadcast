// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5a (ADR 0007 item 8, step 5; SHR-36, SND-35): typed config.
// - FO3DTransportOptionsView: typed getters with the schema's defaults, strict parsing, the
//   caller's default winning in the free O3DTransportOptions getters, VisibleWhen.
// - FO3DTransportConfig::GetOptions() carries the shared schema across copies.
// - O3DTransportOptions::SwitchTransportOptions keeps the other transports' options (SND-35) and
//   never puts a secret away.
// - The interface version is 5.
// Pure data; no transport, registry or network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportApiVersion.h"
#include "Transport/O3DTransportOptionSet.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportOptionsView.h"
#include "Transport/O3DTransportTypes.h"

namespace O3DOptionsViewTest
{
	static FO3DTransportOptionField MakeField(const TCHAR* Key, EO3DTransportOptionType Type, const TCHAR* Default)
	{
		FO3DTransportOptionField Field;
		Field.Key = Key;
		Field.Type = Type;
		Field.Default = Default;
		return Field;
	}

	/** A schema like a transport declares: one field of each typed kind, with and without defaults. */
	static FO3DTransportOptionSchema MakeSchema()
	{
		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeField(TEXT("viewtest.url"), EO3DTransportOptionType::Url, TEXT("wss://default.invalid")));
		Schema.Add(MakeField(TEXT("viewtest.count"), EO3DTransportOptionType::Int, TEXT("7")));
		Schema.Add(MakeField(TEXT("viewtest.ratio"), EO3DTransportOptionType::String, TEXT("1.5")));
		Schema.Add(MakeField(TEXT("viewtest.flag"), EO3DTransportOptionType::Bool, TEXT("true")));
		Schema.Add(MakeField(TEXT("viewtest.nodefault"), EO3DTransportOptionType::Int, TEXT("")));
		Schema.Add(MakeField(TEXT("viewtest.baddefault"), EO3DTransportOptionType::Int, TEXT("12abc")));

		FO3DTransportOptionField Endpoint = MakeField(TEXT("viewtest.endpoint"), EO3DTransportOptionType::Url, TEXT(""));
		Endpoint.VisibleWhen = O3DTransportOptions::VisibleWhenBool(TEXT("viewtest.flag"), /*bExpected=*/true, /*bDefault=*/true);
		Schema.Add(MoveTemp(Endpoint));

		FO3DTransportOptionField Topic = MakeField(TEXT("viewtest.topic"), EO3DTransportOptionType::String, TEXT(""));
		Topic.VisibleWhen = O3DTransportOptions::VisibleWhenEquals(TEXT("viewtest.mode"), TEXT("pub"), TEXT("pair"));
		Schema.Add(MoveTemp(Topic));
		return Schema;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportOptionsViewGettersTest, "Open3DBroadcast.Shared.TransportOptionsView.TypedGettersAndDefaults", O3DB_TEST_FLAGS)
bool FO3DTransportOptionsViewGettersTest::RunTest(const FString& Parameters)
{
	using namespace O3DOptionsViewTest;
	const FO3DTransportOptionSchema Schema = MakeSchema();

	// Nothing set: every getter falls back to the schema's default.
	{
		const TMap<FString, FString> Empty;
		const FO3DTransportOptionsView View(Empty, &Schema);
		TestEqual(TEXT("String default"), View.GetString(TEXT("viewtest.url")), FString(TEXT("wss://default.invalid")));
		TestEqual(TEXT("Int default"), View.GetInt(TEXT("viewtest.count")), 7);
		TestEqual(TEXT("Double default"), View.GetDouble(TEXT("viewtest.ratio")), 1.5);
		TestTrue(TEXT("Bool default"), View.GetBool(TEXT("viewtest.flag")));
		TestEqual(TEXT("No schema default: 0"), View.GetInt(TEXT("viewtest.nodefault")), 0);
		TestEqual(TEXT("A default that does not parse: 0"), View.GetInt(TEXT("viewtest.baddefault")), 0);
		TestEqual(TEXT("Undeclared key: empty"), View.GetString(TEXT("viewtest.unknown")), FString());
		TestFalse(TEXT("Nothing is set"), View.IsSet(TEXT("viewtest.url")));
		TestNotNull(TEXT("The schema entry is found"), View.FindField(TEXT("viewtest.count")));
		TestNull(TEXT("No entry for an undeclared key"), View.FindField(TEXT("viewtest.unknown")));
	}

	// Set values win; keys are case-insensitive; values are trimmed and parsed strictly.
	{
		TMap<FString, FString> Values;
		Values.Add(TEXT("ViewTest.URL"), TEXT("  wss://set.invalid  "));
		Values.Add(TEXT("viewtest.count"), TEXT(" 42 "));
		Values.Add(TEXT("viewtest.ratio"), TEXT("0.25"));
		Values.Add(TEXT("viewtest.flag"), TEXT("off"));
		const FO3DTransportOptionsView View(Values, &Schema);
		TestEqual(TEXT("Set string, trimmed, any key case"), View.GetString(TEXT("viewtest.url")), FString(TEXT("wss://set.invalid")));
		TestEqual(TEXT("Set int, trimmed"), View.GetInt(TEXT("viewtest.count")), 42);
		TestEqual(TEXT("Set double"), View.GetDouble(TEXT("viewtest.ratio")), 0.25);
		TestFalse(TEXT("Set bool (off)"), View.GetBool(TEXT("viewtest.flag")));
		TestTrue(TEXT("IsSet"), View.IsSet(TEXT("VIEWTEST.COUNT")));
	}

	// A value that does not parse falls back to the schema default, not to 0 or a half-parse.
	{
		TMap<FString, FString> Values;
		Values.Add(TEXT("viewtest.count"), TEXT("80abc"));
		Values.Add(TEXT("viewtest.ratio"), TEXT("1.5x"));
		Values.Add(TEXT("viewtest.flag"), TEXT("maybe"));
		Values.Add(TEXT("viewtest.url"), TEXT("   "));
		const FO3DTransportOptionsView View(Values, &Schema);
		TestEqual(TEXT("Garbage int: schema default"), View.GetInt(TEXT("viewtest.count")), 7);
		TestEqual(TEXT("Garbage double: schema default"), View.GetDouble(TEXT("viewtest.ratio")), 1.5);
		TestTrue(TEXT("Garbage bool: schema default"), View.GetBool(TEXT("viewtest.flag")));
		TestEqual(TEXT("Blank string: schema default"), View.GetString(TEXT("viewtest.url")), FString(TEXT("wss://default.invalid")));
		TestFalse(TEXT("A blank value is not set"), View.IsSet(TEXT("viewtest.url")));

		// The free getters keep the caller's default (unchanged behaviour for every transport).
		TestEqual(TEXT("Free GetInt: caller default"), O3DTransportOptions::GetInt(View, TEXT("viewtest.count"), 3), 3);
		TestFalse(TEXT("Free GetBool: caller default"), O3DTransportOptions::GetBool(View, TEXT("viewtest.flag"), false));
		TestEqual(TEXT("Free GetString: caller default"), O3DTransportOptions::GetString(View, TEXT("viewtest.url"), TEXT("x")), FString(TEXT("x")));
	}

	// A plain map converts to a view, so existing calls with AdvancedParams keep working.
	{
		TMap<FString, FString> Map;
		Map.Add(TEXT("viewtest.count"), TEXT("5"));
		TestEqual(TEXT("Map passed where a view is taken"), O3DTransportOptions::GetInt(Map, TEXT("viewtest.count"), 0), 5);
		const FO3DTransportOptionsView NoSchema(Map);
		TestNull(TEXT("No schema"), NoSchema.GetSchema());
		TestEqual(TEXT("No schema: typed getter default 0"), NoSchema.GetInt(TEXT("viewtest.nodefault")), 0);
		TestEqual(TEXT("No schema: set value still read"), NoSchema.GetInt(TEXT("viewtest.count")), 5);
	}

	// A default-constructed view is empty, never null.
	{
		const FO3DTransportOptionsView Empty;
		TestEqual(TEXT("Empty view has no values"), Empty.GetValues().Num(), 0);
		TestNull(TEXT("Empty view finds nothing"), Empty.Find(TEXT("viewtest.count")));
		TestEqual(TEXT("Empty view: string"), Empty.GetString(TEXT("viewtest.url")), FString());
	}

	// TryParseBool, new in this PR, accepts exactly the GetBool words.
	bool Parsed = false;
	TestTrue(TEXT("TryParseBool yes"), O3DTransportOptions::TryParseBool(TEXT(" Yes "), Parsed) && Parsed);
	TestTrue(TEXT("TryParseBool 0"), O3DTransportOptions::TryParseBool(TEXT("0"), Parsed) && !Parsed);
	TestFalse(TEXT("TryParseBool 2"), O3DTransportOptions::TryParseBool(TEXT("2"), Parsed));
	TestFalse(TEXT("TryParseBool empty"), O3DTransportOptions::TryParseBool(TEXT(""), Parsed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportOptionsViewVisibleWhenTest, "Open3DBroadcast.Shared.TransportOptionsView.VisibleWhen", O3DB_TEST_FLAGS)
bool FO3DTransportOptionsViewVisibleWhenTest::RunTest(const FString& Parameters)
{
	using namespace O3DOptionsViewTest;
	const FO3DTransportOptionSchema Schema = MakeSchema();

	TMap<FString, FString> Values;
	const FO3DTransportOptionsView View(Values, &Schema);
	TestTrue(TEXT("Bool unset reads as its default (true): shown"), View.IsVisible(TEXT("viewtest.endpoint")));
	TestFalse(TEXT("Mode unset reads as pair: topic hidden"), View.IsVisible(TEXT("viewtest.topic")));
	TestTrue(TEXT("A field without VisibleWhen is shown"), View.IsVisible(TEXT("viewtest.count")));
	TestTrue(TEXT("An undeclared key is shown"), View.IsVisible(TEXT("viewtest.unknown")));

	// The view reads the map it was made from, so later changes show.
	Values.Add(TEXT("viewtest.flag"), TEXT("false"));
	Values.Add(TEXT("viewtest.mode"), TEXT("PUB"));
	TestFalse(TEXT("Flag false: endpoint hidden"), View.IsVisible(TEXT("viewtest.endpoint")));
	TestTrue(TEXT("Mode pub (any case): topic shown"), View.IsVisible(TEXT("viewtest.topic")));

	const FO3DTransportOptionsView NoSchema(Values);
	TestTrue(TEXT("Without a schema everything is shown"), NoSchema.IsVisible(TEXT("viewtest.endpoint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportOptionsViewConfigTest, "Open3DBroadcast.Shared.TransportOptionsView.ConfigCarriesSchema", O3DB_TEST_FLAGS)
bool FO3DTransportOptionsViewConfigTest::RunTest(const FString& Parameters)
{
	using namespace O3DOptionsViewTest;

	FO3DTransportConfig Original;
	Original.AdvancedParams.Add(TEXT("viewtest.count"), TEXT("9"));
	Original.OptionSchema = MakeShared<FO3DTransportOptionSchema>(MakeSchema());
	Original.SubjectName = TEXT("Hero");

	// Transports copy their config (ActiveConfig = Config); the schema is shared, the values copied.
	FO3DTransportConfig Copy = Original;
	Original.OptionSchema.Reset();
	Original.AdvancedParams.Reset();

	const FO3DTransportOptionsView Options = Copy.GetOptions();
	TestNotNull(TEXT("The copy keeps the schema"), Options.GetSchema());
	TestEqual(TEXT("The copy keeps its values"), Options.GetInt(TEXT("viewtest.count")), 9);
	TestTrue(TEXT("Schema default through the config"), Options.GetBool(TEXT("viewtest.flag")));
	TestEqual(TEXT("SubjectName is copied"), Copy.SubjectName, FString(TEXT("Hero")));

	const FO3DTransportConfig Plain;
	TestNull(TEXT("A config built without a host has no schema"), Plain.GetOptions().GetSchema());
	TestEqual(TEXT("And no values"), Plain.GetOptions().GetValues().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportOptionsSwitchTest, "Open3DBroadcast.Shared.TransportOptions.SwitchKeepsOtherTransportsOptions", O3DB_TEST_FLAGS)
bool FO3DTransportOptionsSwitchTest::RunTest(const FString& Parameters)
{
	const FName Tcp(TEXT("SwitchTestTCP"));
	const FName Udp(TEXT("SwitchTestUDP"));
	const TArray<FString> TcpSecrets = { TEXT("switchtest.token") };

	TMap<FString, FString> Active;
	TMap<FName, FO3DTransportOptionSet> Inactive;
	Active.Add(TEXT("host"), TEXT("10.0.0.5"));
	Active.Add(TEXT("port"), TEXT("9100"));
	// What a pre-ADR 0004 asset could hold; never put away.
	Active.Add(TEXT("switchtest.token"), TEXT("SECRET-VALUE"));

	O3DTransportOptions::SwitchTransportOptions(Active, Inactive, Tcp, Udp, TcpSecrets);
	TestEqual(TEXT("The new transport starts empty (it has nothing put away)"), Active.Num(), 0);
	TestTrue(TEXT("TCP's options are put away"), Inactive.Contains(Tcp));
	if (const FO3DTransportOptionSet* Kept = Inactive.Find(Tcp))
	{
		TestEqual(TEXT("TCP host kept"), Kept->Options.FindRef(TEXT("host")), FString(TEXT("10.0.0.5")));
		TestEqual(TEXT("TCP port kept"), Kept->Options.FindRef(TEXT("port")), FString(TEXT("9100")));
		TestFalse(TEXT("The secret is not put away"), Kept->Options.Contains(TEXT("switchtest.token")));
	}

	// UDP reads "port" too: it must not see TCP's.
	Active.Add(TEXT("port"), TEXT("7000"));
	O3DTransportOptions::SwitchTransportOptions(Active, Inactive, Udp, Tcp, TArray<FString>());
	TestEqual(TEXT("Back on TCP: its host"), Active.FindRef(TEXT("host")), FString(TEXT("10.0.0.5")));
	TestEqual(TEXT("Back on TCP: its port, not UDP's"), Active.FindRef(TEXT("port")), FString(TEXT("9100")));
	TestFalse(TEXT("TCP is no longer put away"), Inactive.Contains(Tcp));
	TestEqual(TEXT("UDP's port is put away"), Inactive.FindRef(Udp).Options.FindRef(TEXT("port")), FString(TEXT("7000")));

	// Same transport: nothing changes.
	O3DTransportOptions::SwitchTransportOptions(Active, Inactive, Tcp, Tcp, TcpSecrets);
	TestEqual(TEXT("Same transport keeps the map"), Active.Num(), 2);
	TestEqual(TEXT("Same transport keeps the others"), Inactive.Num(), 1);

	// An empty map is not stored, so switching through unused transports leaves no entries.
	TMap<FString, FString> Empty;
	TMap<FName, FO3DTransportOptionSet> NoneKept;
	O3DTransportOptions::SwitchTransportOptions(Empty, NoneKept, Tcp, Udp, TArray<FString>());
	TestEqual(TEXT("Nothing put away for an empty map"), NoneKept.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportApiVersionTypedConfigTest, "Open3DBroadcast.Shared.TransportApiVersion.TypedConfigIsVersion5", O3DB_TEST_FLAGS)
bool FO3DTransportApiVersionTypedConfigTest::RunTest(const FString& Parameters)
{
	FString Error;
	TestTrue(TEXT("The typed-config interface is version 5 or later"), O3DTransport::GetHostApiVersion() >= 5);
	TestFalse(TEXT("An add-on built for version 4 (before typed config) is refused"),
		O3DTransport::CheckApiVersion(TEXT("OldAddOn"), 4, O3DTransport::GetHostApiVersion(), Error));
	TestTrue(TEXT("The refusal names the add-on"), Error.Contains(TEXT("OldAddOn")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
