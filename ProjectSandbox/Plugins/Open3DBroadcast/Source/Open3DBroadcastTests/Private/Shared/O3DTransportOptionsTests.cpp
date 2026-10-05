// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// O3DTransportOptions (ADR 0007 item 7, WP-A1 step 4; TRB-26, SHR-9): the strict host:port parser,
// hostname resolution and the typed option getters.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Transport/O3DTransportOptions.h"

namespace O3DTransportOptionsTests
{
	struct FHostPortCase
	{
		const TCHAR* Input;
		int32 DefaultPort;
		bool bValid;
		const TCHAR* Host;
		int32 Port;
		bool bIPv6;
	};

	void RunCases(FAutomationTestBase& Test, const FHostPortCase* Cases, int32 Num)
	{
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const FHostPortCase& Case = Cases[Index];
			FO3DHostPort Endpoint;
			FString Error;
			const bool bParsed = O3DTransportOptions::ParseHostPort(Case.Input, Endpoint, Case.DefaultPort, &Error);
			if (!Test.TestTrue(*FString::Printf(TEXT("'%s' (default port %d) %s"), Case.Input, Case.DefaultPort, Case.bValid ? TEXT("parses") : TEXT("is refused")), bParsed == Case.bValid))
			{
				continue;
			}
			if (Case.bValid)
			{
				Test.TestEqual(*FString::Printf(TEXT("'%s' host"), Case.Input), Endpoint.Host, FString(Case.Host));
				Test.TestEqual(*FString::Printf(TEXT("'%s' port"), Case.Input), Endpoint.Port, Case.Port);
				Test.TestTrue(*FString::Printf(TEXT("'%s' IPv6 flag"), Case.Input), Endpoint.bIPv6 == Case.bIPv6);
			}
			else
			{
				Test.TestFalse(*FString::Printf(TEXT("'%s' explains the failure"), Case.Input), Error.IsEmpty());
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortBasicTest, "Open3DBroadcast.Shared.HostPort.Basic", O3DB_TEST_FLAGS)
bool FO3DHostPortBasicTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportOptionsTests;
	const FHostPortCase Cases[] = {
		{ TEXT("127.0.0.1:9000"), 0, true, TEXT("127.0.0.1"), 9000, false },
		{ TEXT("  localhost:1  "), 0, true, TEXT("localhost"), 1, false },
		{ TEXT("tcp://mocap-pc.local:7000"), 0, true, TEXT("mocap-pc.local"), 7000, false },
		{ TEXT("udp://studio_host:65535/path?x=1#frag"), 0, true, TEXT("studio_host"), 65535, false },
		{ TEXT("tcp://0.0.0.0:00080"), 0, true, TEXT("0.0.0.0"), 80, false },
		{ TEXT("*:9000"), 0, true, TEXT("*"), 9000, false },
		{ TEXT("host:7000?role=pub"), 0, true, TEXT("host"), 7000, false },
	};
	RunCases(*this, Cases, static_cast<int32>(UE_ARRAY_COUNT(Cases)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortIPv6Test, "Open3DBroadcast.Shared.HostPort.IPv6Brackets", O3DB_TEST_FLAGS)
bool FO3DHostPortIPv6Test::RunTest(const FString& Parameters)
{
	using namespace O3DTransportOptionsTests;
	const FHostPortCase Cases[] = {
		{ TEXT("[::1]:9000"), 0, true, TEXT("::1"), 9000, true },
		{ TEXT("tcp://[2001:db8::7]:443/x"), 0, true, TEXT("2001:db8::7"), 443, true },
		{ TEXT("[fe80::1%eth0]:80"), 0, true, TEXT("fe80::1%eth0"), 80, true },
		{ TEXT("[::ffff:192.168.1.10]:5000"), 0, true, TEXT("::ffff:192.168.1.10"), 5000, true },
		{ TEXT("[::1]"), 7000, true, TEXT("::1"), 7000, true },
		{ TEXT("::1"), 7000, true, TEXT("::1"), 7000, true },
		{ TEXT("fe80::1"), 7000, true, TEXT("fe80::1"), 7000, true },
		{ TEXT("[::1]"), 0, false, nullptr, 0, false },
		{ TEXT("[::1"), 0, false, nullptr, 0, false },
		{ TEXT("[::1]9000"), 0, false, nullptr, 0, false },
		{ TEXT("[]:80"), 0, false, nullptr, 0, false },
		{ TEXT("[not-ipv6]:80"), 0, false, nullptr, 0, false },
		{ TEXT("[::1]:0"), 0, false, nullptr, 0, false },
		{ TEXT("[fe80::1%]:80"), 0, false, nullptr, 0, false },
		{ TEXT("::zz"), 7000, false, nullptr, 0, false },
	};
	RunCases(*this, Cases, static_cast<int32>(UE_ARRAY_COUNT(Cases)));

	FO3DHostPort Endpoint;
	Endpoint.Host = TEXT("::1");
	Endpoint.Port = 9000;
	Endpoint.bIPv6 = true;
	TestEqual(TEXT("IPv6 ToString brackets the host"), Endpoint.ToString(), FString(TEXT("[::1]:9000")));
	Endpoint.Host = TEXT("10.0.0.1");
	Endpoint.bIPv6 = false;
	TestEqual(TEXT("IPv4 ToString"), Endpoint.ToString(), FString(TEXT("10.0.0.1:9000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortMissingAndBadPortTest, "Open3DBroadcast.Shared.HostPort.MissingAndBadPort", O3DB_TEST_FLAGS)
bool FO3DHostPortMissingAndBadPortTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportOptionsTests;
	const FHostPortCase Cases[] = {
		// SHR-9: a dotted address without a port is never rewritten (tcp://192.168.1.10 used to become 192.168.1:10).
		{ TEXT("192.168.1.10"), 9000, true, TEXT("192.168.1.10"), 9000, false },
		{ TEXT("tcp://192.168.1.10"), 9000, true, TEXT("192.168.1.10"), 9000, false },
		{ TEXT("tcp://192.168.1.10"), 0, false, nullptr, 0, false },
		{ TEXT("mocap-pc"), 0, false, nullptr, 0, false },
		{ TEXT("mocap-pc"), 70000, false, nullptr, 0, false },
		{ TEXT("host:"), 0, false, nullptr, 0, false },
		{ TEXT("host:0"), 0, false, nullptr, 0, false },
		{ TEXT("host:65536"), 0, false, nullptr, 0, false },
		{ TEXT("host:99999999999"), 0, false, nullptr, 0, false },
		{ TEXT("host:80abc"), 0, false, nullptr, 0, false },
		{ TEXT("host:-1"), 0, false, nullptr, 0, false },
		{ TEXT("host:+80"), 0, false, nullptr, 0, false },
		{ TEXT("host: 80"), 0, false, nullptr, 0, false },
		{ TEXT("host:8 0"), 0, false, nullptr, 0, false },
	};
	RunCases(*this, Cases, static_cast<int32>(UE_ARRAY_COUNT(Cases)));

	int32 Port = 0;
	TestTrue(TEXT("TryParsePort 65535"), O3DTransportOptions::TryParsePort(TEXT("65535"), Port) && Port == 65535);
	TestFalse(TEXT("TryParsePort empty"), O3DTransportOptions::TryParsePort(TEXT(""), Port));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortBadHostTest, "Open3DBroadcast.Shared.HostPort.BadHost", O3DB_TEST_FLAGS)
bool FO3DHostPortBadHostTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportOptionsTests;
	const FHostPortCase Cases[] = {
		{ TEXT(""), 9000, false, nullptr, 0, false },
		{ TEXT("   "), 9000, false, nullptr, 0, false },
		{ TEXT(":80"), 0, false, nullptr, 0, false },
		{ TEXT("tcp://"), 9000, false, nullptr, 0, false },
		{ TEXT("tcp:///path"), 9000, false, nullptr, 0, false },
		{ TEXT("user@host:80"), 0, false, nullptr, 0, false },
		{ TEXT("bad host:80"), 0, false, nullptr, 0, false },
		{ TEXT("bad;host:80"), 0, false, nullptr, 0, false },
	};
	RunCases(*this, Cases, static_cast<int32>(UE_ARRAY_COUNT(Cases)));

	// A failed parse leaves the output untouched.
	FO3DHostPort Endpoint;
	Endpoint.Host = TEXT("kept");
	Endpoint.Port = 1;
	TestFalse(TEXT("Parse fails"), O3DTransportOptions::ParseHostPort(TEXT("host:0"), Endpoint));
	TestTrue(TEXT("Output untouched"), Endpoint.Host == TEXT("kept") && Endpoint.Port == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortResolveTest, "Open3DBroadcast.Shared.HostPort.Resolve", O3DB_TEST_FLAGS)
bool FO3DHostPortResolveTest::RunTest(const FString& Parameters)
{
	// Literals and "localhost" resolve without a network; nothing here needs DNS beyond the host.
	FO3DHostPort Endpoint;
	TSharedPtr<FInternetAddr> Address;
	FString Error;

	TestTrue(TEXT("Parse an IPv4 literal"), O3DTransportOptions::ParseHostPort(TEXT("127.0.0.1:9123"), Endpoint));
	TestTrue(*FString::Printf(TEXT("Resolve an IPv4 literal (%s)"), *Error), O3DTransportOptions::ResolveHostPort(Endpoint, Address, &Error));
	if (Address.IsValid())
	{
		TestEqual(TEXT("Address"), Address->ToString(false), FString(TEXT("127.0.0.1")));
		TestEqual(TEXT("Port"), Address->GetPort(), 9123);
	}

	Address.Reset();
	TestTrue(TEXT("Parse a host name"), O3DTransportOptions::ParseHostPort(TEXT("localhost:9124"), Endpoint));
	TestTrue(*FString::Printf(TEXT("Resolve localhost (%s)"), *Error), O3DTransportOptions::ResolveHostPort(Endpoint, Address, &Error));
	TestTrue(TEXT("localhost has an address with the port"), Address.IsValid() && Address->GetPort() == 9124);

	Address.Reset();
	TestTrue(TEXT("Parse the wildcard"), O3DTransportOptions::ParseHostPort(TEXT("*:9125"), Endpoint));
	TestTrue(*FString::Printf(TEXT("Resolve the wildcard to the any-address (%s)"), *Error), O3DTransportOptions::ResolveHostPort(Endpoint, Address, &Error));
	TestTrue(TEXT("Any-address with the port"), Address.IsValid() && Address->GetPort() == 9125);

	FO3DHostPort NoPort;
	NoPort.Host = TEXT("127.0.0.1");
	Address.Reset();
	TestFalse(TEXT("Port 0 does not resolve"), O3DTransportOptions::ResolveHostPort(NoPort, Address, &Error));
	TestFalse(TEXT("No address"), Address.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportOptionsGettersTest, "Open3DBroadcast.Shared.TransportOptions.TypedGetters", O3DB_TEST_FLAGS)
bool FO3DTransportOptionsGettersTest::RunTest(const FString& Parameters)
{
	TMap<FString, FString> Options;
	Options.Add(TEXT("Queue"), TEXT(" 128 "));
	Options.Add(TEXT("bad.int"), TEXT("80abc"));
	Options.Add(TEXT("big"), TEXT("5000000000"));
	Options.Add(TEXT("negative"), TEXT("-7"));
	Options.Add(TEXT("flag.on"), TEXT("Yes"));
	Options.Add(TEXT("flag.off"), TEXT("off"));
	Options.Add(TEXT("flag.bad"), TEXT("maybe"));
	Options.Add(TEXT("ratio"), TEXT("1.5e-1"));
	Options.Add(TEXT("bad.ratio"), TEXT("1.5x"));
	Options.Add(TEXT("name"), TEXT("  studio  "));
	Options.Add(TEXT("blank"), TEXT("   "));

	using namespace O3DTransportOptions;
	TestEqual(TEXT("Keys are case-insensitive and values trimmed"), GetInt(Options, TEXT("queue"), 1), 128);
	TestEqual(TEXT("Trailing garbage gives the default (TRB-26)"), GetInt(Options, TEXT("bad.int"), 64), 64);
	TestEqual(TEXT("Out of int32 range clamps"), GetInt(Options, TEXT("big"), 1), MAX_int32);
	TestEqual(TEXT("Clamped to Max"), GetInt(Options, TEXT("queue"), 1, 1, 100), 100);
	TestEqual(TEXT("Clamped to Min"), GetInt(Options, TEXT("negative"), 1, 0, 10), 0);
	TestEqual(TEXT("Absent gives the default"), GetInt(Options, TEXT("absent"), 42), 42);

	TestTrue(TEXT("yes is true"), GetBool(Options, TEXT("flag.on"), false));
	TestFalse(TEXT("off is false"), GetBool(Options, TEXT("flag.off"), true));
	TestTrue(TEXT("Anything else gives the default"), GetBool(Options, TEXT("flag.bad"), true));

	TestEqual(TEXT("Double"), GetDouble(Options, TEXT("ratio"), 0.0), 0.15);
	TestEqual(TEXT("Bad double gives the default"), GetDouble(Options, TEXT("bad.ratio"), 2.0), 2.0);
	TestEqual(TEXT("Double clamped"), GetDouble(Options, TEXT("ratio"), 0.0, 0.5, 1.0), 0.5);

	TestEqual(TEXT("String trimmed"), GetString(Options, TEXT("NAME")), FString(TEXT("studio")));
	TestEqual(TEXT("Blank gives the default"), GetString(Options, TEXT("blank"), TEXT("dflt")), FString(TEXT("dflt")));
	TestTrue(TEXT("Find"), Find(Options, TEXT("FLAG.ON")) != nullptr && Find(Options, TEXT("nope")) == nullptr);

	int64 Value = 0;
	TestTrue(TEXT("int64 max"), TryParseInt(TEXT("9223372036854775807"), Value) && Value == MAX_int64);
	TestTrue(TEXT("int64 min"), TryParseInt(TEXT("-9223372036854775808"), Value) && Value == MIN_int64);
	TestFalse(TEXT("int64 overflow"), TryParseInt(TEXT("9223372036854775808"), Value));
	TestFalse(TEXT("Sign only"), TryParseInt(TEXT("-"), Value));
	TestFalse(TEXT("Empty"), TryParseInt(TEXT(""), Value));
	double Number = 0.0;
	TestFalse(TEXT("Exponent without digits"), TryParseDouble(TEXT("1e"), Number));
	TestFalse(TEXT("No digits"), TryParseDouble(TEXT("."), Number));
	TestTrue(TEXT("Leading dot"), TryParseDouble(TEXT(".5"), Number) && Number == 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DHostPortIsIpLiteralTest, "Open3DBroadcast.Shared.HostPort.IsIpLiteral", O3DB_TEST_FLAGS)
bool FO3DHostPortIsIpLiteralTest::RunTest(const FString& Parameters)
{
	auto IsLiteral = [](const TCHAR* Input)
	{
		FO3DHostPort Endpoint;
		return O3DTransportOptions::ParseHostPort(Input, Endpoint, 9000) && O3DTransportOptions::IsIpLiteral(Endpoint);
	};
	TestTrue(TEXT("IPv4"), IsLiteral(TEXT("192.168.1.10")));
	TestTrue(TEXT("IPv4 any-address"), IsLiteral(TEXT("0.0.0.0")));
	TestTrue(TEXT("IPv6"), IsLiteral(TEXT("[::1]")));
	TestFalse(TEXT("Host name"), IsLiteral(TEXT("localhost")));
	TestFalse(TEXT("Dotted host name"), IsLiteral(TEXT("mocap.local")));
	TestFalse(TEXT("Octet over 255"), IsLiteral(TEXT("192.168.1.256")));
	TestFalse(TEXT("Three parts"), IsLiteral(TEXT("192.168.1")));
	TestFalse(TEXT("Empty part"), IsLiteral(TEXT("192..1.1")));
	TestFalse(TEXT("Wildcard"), IsLiteral(TEXT("*")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
