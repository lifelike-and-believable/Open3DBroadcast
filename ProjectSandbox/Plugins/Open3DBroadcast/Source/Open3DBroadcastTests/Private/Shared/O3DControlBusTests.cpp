// Copyright 2026 Lifelike & Believable. All Rights Reserved.

//
// ADR 0011 (CTL-2): FO3DControlBus. Changes reach listeners and the value cache; a change heard
// twice (two receiver sources on one sender) is published once; stale values never overwrite
// newer ones; keys are case-sensitive; the runtime receive override round-trips.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DControlBus.h"

namespace O3DControlBusTests
{
	FO3DControlChange Value(const FString& Source, const FString& Key, const FO3DControlValue& V, int64 Epoch, int64 Version, const FString& Target = FString())
	{
		FO3DControlChange C;
		C.Kind = FO3DControlChange::EKind::ValueChanged;
		C.Name = Key;
		C.Value = V;
		C.Meta.SourceId = Source;
		C.Meta.TargetSubject = Target;
		C.Meta.Epoch = Epoch;
		C.Meta.Version = Version;
		return C;
	}

	FO3DControlChange Cleared(const FString& Source, const FString& Key, int64 Epoch, int64 Version)
	{
		FO3DControlChange C = Value(Source, Key, FO3DControlValue(), Epoch, Version);
		C.Kind = FO3DControlChange::EKind::ValueCleared;
		return C;
	}

	FO3DControlChange Event(const FString& Source, const FString& Name, int64 Epoch, int64 EventId)
	{
		FO3DControlChange C;
		C.Kind = FO3DControlChange::EKind::Event;
		C.Name = Name;
		C.Meta.SourceId = Source;
		C.Meta.Epoch = Epoch;
		C.Meta.EventId = EventId;
		return C;
	}

	/** Records every broadcast and resets the bus around the test. */
	struct FScopedListener
	{
		TArray<FO3DControlChange> Seen;
		FDelegateHandle Handle;

		FScopedListener()
		{
			FO3DControlBus::ResetForTesting();
			Handle = FO3DControlBus::OnChange().AddLambda([this](const FO3DControlChange& Change) { Seen.Add(Change); });
		}
		~FScopedListener()
		{
			FO3DControlBus::ResetForTesting();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlBusValuesTest, "Open3DBroadcast.Shared.Control.Bus.ValuesAndCache", O3DB_TEST_FLAGS)
bool FO3DControlBusValuesTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlBusTests;
	FScopedListener Listener;

	TestTrue(TEXT("New value published"), FO3DControlBus::Publish(Value(TEXT("A"), TEXT("env.fog"), FO3DControlValue::MakeFloat(0.2), 1, 5)));
	const FO3DControlValue* Cached = FO3DControlBus::FindValue(TEXT("A"), TEXT("env.fog"), FString());
	TestTrue(TEXT("Cached"), Cached != nullptr && *Cached == FO3DControlValue::MakeFloat(0.2));

	TestTrue(TEXT("Newer version published"), FO3DControlBus::Publish(Value(TEXT("A"), TEXT("env.fog"), FO3DControlValue::MakeFloat(0.4), 1, 9)));
	TestFalse(TEXT("Older version dropped"), FO3DControlBus::Publish(Value(TEXT("A"), TEXT("env.fog"), FO3DControlValue::MakeFloat(0.1), 1, 7)));
	TestFalse(TEXT("Same version again dropped (a second source heard it too)"), FO3DControlBus::Publish(Value(TEXT("A"), TEXT("env.fog"), FO3DControlValue::MakeFloat(0.4), 1, 9)));
	TestTrue(TEXT("Newer epoch wins over a higher version"), FO3DControlBus::Publish(Value(TEXT("A"), TEXT("env.fog"), FO3DControlValue::MakeFloat(0.5), 2, 1)));
	TestTrue(TEXT("Cache holds the newest"), *FO3DControlBus::FindValue(TEXT("A"), TEXT("env.fog"), FString()) == FO3DControlValue::MakeFloat(0.5));
	TestEqual(TEXT("Three broadcasts"), Listener.Seen.Num(), 3);

	TestFalse(TEXT("Stale clear dropped"), FO3DControlBus::Publish(Cleared(TEXT("A"), TEXT("env.fog"), 1, 50)));
	TestTrue(TEXT("Newer clear published"), FO3DControlBus::Publish(Cleared(TEXT("A"), TEXT("env.fog"), 2, 3)));
	TestTrue(TEXT("Cleared from the cache"), FO3DControlBus::FindValue(TEXT("A"), TEXT("env.fog"), FString()) == nullptr);
	TestFalse(TEXT("Clearing an absent key is not broadcast"), FO3DControlBus::Publish(Cleared(TEXT("A"), TEXT("env.fog"), 2, 4)));

	FO3DControlBus::Publish(Value(TEXT("A"), TEXT("k"), FO3DControlValue::MakeBool(true), 2, 10));
	TestTrue(TEXT("A version-0 clear (source pruned) always applies"), FO3DControlBus::Publish(Cleared(TEXT("A"), TEXT("k"), 2, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlBusKeysTest, "Open3DBroadcast.Shared.Control.Bus.KeysTargetsAndSources", O3DB_TEST_FLAGS)
bool FO3DControlBusKeysTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlBusTests;
	FScopedListener Listener;

	FO3DControlBus::Publish(Value(TEXT("A"), TEXT("Light.Intensity"), FO3DControlValue::MakeFloat(1.0), 1, 1));
	FO3DControlBus::Publish(Value(TEXT("A"), TEXT("light.intensity"), FO3DControlValue::MakeFloat(2.0), 1, 2));
	TestEqual(TEXT("Two casings are two keys"), FO3DControlBus::GetValues(TEXT("A")).Num(), 2);
	TestTrue(TEXT("Exact-case lookup"), *FO3DControlBus::FindValue(TEXT("A"), TEXT("Light.Intensity"), FString()) == FO3DControlValue::MakeFloat(1.0));

	FO3DControlBus::Publish(Value(TEXT("A"), TEXT("char.emotion"), FO3DControlValue::MakeName(TEXT("joy")), 1, 3, TEXT("Hero")));
	FO3DControlBus::Publish(Value(TEXT("A"), TEXT("char.emotion"), FO3DControlValue::MakeName(TEXT("anger")), 1, 4, TEXT("Villain")));
	TestTrue(TEXT("Targets are separate"), *FO3DControlBus::FindValue(TEXT("A"), TEXT("char.emotion"), TEXT("Hero")) == FO3DControlValue::MakeName(TEXT("joy")));
	TestTrue(TEXT("No untargeted value"), FO3DControlBus::FindValue(TEXT("A"), TEXT("char.emotion"), FString()) == nullptr);

	// ("ab", "c") and ("a", "bc") must not collide.
	FO3DControlBus::Publish(Value(TEXT("B"), TEXT("ab"), FO3DControlValue::MakeInt(1), 1, 1, TEXT("c")));
	FO3DControlBus::Publish(Value(TEXT("B"), TEXT("a"), FO3DControlValue::MakeInt(2), 1, 2, TEXT("bc")));
	TestEqual(TEXT("No key/target collision"), FO3DControlBus::GetValues(TEXT("B")).Num(), 2);

	const TArray<TTuple<FString, FString, FO3DControlValue>> Values = FO3DControlBus::GetValues(TEXT("A"));
	TestTrue(TEXT("Sorted by key"), Values.Num() == 4 && Values[0].Get<0>().Compare(Values[1].Get<0>(), ESearchCase::CaseSensitive) <= 0);
	TestEqual(TEXT("Two sources"), FO3DControlBus::GetSources().Num(), 2);
	FO3DControlBus::Publish(Value(TEXT("a"), TEXT("Light.Intensity"), FO3DControlValue::MakeFloat(9.0), 1, 1));
	TestEqual(TEXT("Source ids are case-sensitive too"), FO3DControlBus::GetSources().Num(), 3);
	TestTrue(TEXT("Source A unaffected"), *FO3DControlBus::FindValue(TEXT("A"), TEXT("Light.Intensity"), FString()) == FO3DControlValue::MakeFloat(1.0));

	const int32 Broadcasts = Listener.Seen.Num();
	FO3DControlBus::ForgetSource(TEXT("A"));
	TestEqual(TEXT("Forget does not broadcast"), Listener.Seen.Num(), Broadcasts);
	TestTrue(TEXT("Forgotten"), FO3DControlBus::GetValues(TEXT("A")).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlBusEventsTest, "Open3DBroadcast.Shared.Control.Bus.EventsOncePerSourceAndEpoch", O3DB_TEST_FLAGS)
bool FO3DControlBusEventsTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlBusTests;
	FScopedListener Listener;

	TestTrue(TEXT("First delivery"), FO3DControlBus::Publish(Event(TEXT("A"), TEXT("vfx.spark"), 1, 1)));
	TestFalse(TEXT("Same event from a second receiver source"), FO3DControlBus::Publish(Event(TEXT("A"), TEXT("vfx.spark"), 1, 1)));
	TestTrue(TEXT("Same id from another source"), FO3DControlBus::Publish(Event(TEXT("B"), TEXT("vfx.spark"), 1, 1)));
	TestTrue(TEXT("Same id in a new epoch"), FO3DControlBus::Publish(Event(TEXT("A"), TEXT("vfx.spark"), 2, 1)));
	TestFalse(TEXT("Older epoch dropped"), FO3DControlBus::Publish(Event(TEXT("A"), TEXT("vfx.spark"), 1, 2)));
	TestEqual(TEXT("Three broadcasts"), Listener.Seen.Num(), 3);

	// The history is bounded: very old ids are forgotten.
	for (int64 Id = 100; Id < 100 + 600; ++Id)
	{
		FO3DControlBus::Publish(Event(TEXT("C"), TEXT("cue"), 1, Id));
	}
	TestTrue(TEXT("A recent id is still remembered"), !FO3DControlBus::Publish(Event(TEXT("C"), TEXT("cue"), 1, 650)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlBusOverrideTest, "Open3DBroadcast.Shared.Control.Bus.ReceiveOverride", O3DB_TEST_FLAGS)
bool FO3DControlBusOverrideTest::RunTest(const FString& Parameters)
{
	FO3DControlBus::ResetForTesting();
	TestFalse(TEXT("Unset by default"), FO3DControlBus::GetReceiveOverride().IsSet());
	FO3DControlBus::SetReceiveOverride(true);
	TestTrue(TEXT("Set true"), FO3DControlBus::GetReceiveOverride().IsSet() && FO3DControlBus::GetReceiveOverride().GetValue());
	FO3DControlBus::SetReceiveOverride(false);
	TestTrue(TEXT("Set false"), FO3DControlBus::GetReceiveOverride().IsSet() && !FO3DControlBus::GetReceiveOverride().GetValue());
	FO3DControlBus::SetReceiveOverride(TOptional<bool>());
	TestFalse(TEXT("Cleared"), FO3DControlBus::GetReceiveOverride().IsSet());
	FO3DControlBus::ResetForTesting();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
