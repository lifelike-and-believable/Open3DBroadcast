// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// ADR 0011 (CTL-4): control on the receiving side, end to end over the real Loopback transport:
// FO3DControlPublisher -> Loopback -> FO3DReceiverSource -> FO3DControlBus ->
// UO3DRemoteControlComponent. Covers enablement (project setting, runtime override, per-source
// setting), silent discard when turned off, component filters, and alignment to the mocap
// stream (held while its pose is behind, released at once for a control-only sender).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "O3DControlBus.h"
#include "O3DControlPublisher.h"
#include "O3DControlSettings.h"
#include "O3DControlTestListener.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DControlReceiverTests
{
	/** Restores the project settings and the bus when a test ends. */
	struct FScopedControlSettings
	{
		UO3DControlSettings* Settings = GetMutableDefault<UO3DControlSettings>();
		const bool bAccept = Settings->bAcceptControl;
		const bool bAlign = Settings->bAlignControlToMocap;
		const int32 HoldMs = Settings->MaxAlignmentHoldMs;
		const TArray<FString> Allowlist = Settings->ControlAllowlist;

		FScopedControlSettings(bool bInAccept, bool bInAlign)
		{
			FO3DControlBus::ResetForTesting();
			Settings->bAcceptControl = bInAccept;
			Settings->bAlignControlToMocap = bInAlign;
		}
		~FScopedControlSettings()
		{
			Settings->bAcceptControl = bAccept;
			Settings->bAlignControlToMocap = bAlign;
			Settings->MaxAlignmentHoldMs = HoldMs;
			Settings->ControlAllowlist = Allowlist;
			FO3DControlBus::ResetForTesting();
		}
	};

	/** A Loopback sender and a receiver source on one fresh channel. */
	struct FLoopbackPair
	{
		FString Channel = O3DTests::MakeUniqueName(TEXT("O3DControlE2E"));
		TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender;
		TSharedPtr<FO3DReceiverSource> Source;
		TSharedRef<FO3DReceiverCorrectnessTestAccessor::FRecorder> Recorder = MakeShared<FO3DReceiverCorrectnessTestAccessor::FRecorder>();

		bool Start(FAutomationTestBase& Test, ULiveLinkSourceSettings* SourceSettings = nullptr)
		{
			Sender = FO3DTransportRegistry::Get().CreateSender(FName(TEXT("Loopback")));
			FO3DTransportConfig SenderConfig;
			SenderConfig.Transport = TEXT("Loopback");
			SenderConfig.Role = EO3DTransportRole::Sender;
			SenderConfig.StreamId = Channel;
			SenderConfig.Uri = FString::Printf(TEXT("loopback://%s?role=pub"), *Channel);
			if (!Test.TestTrue(TEXT("Loopback sender starts"), Sender.IsValid() && Sender->Initialize(SenderConfig) && Sender->Start()))
			{
				return false;
			}

			FO3DReceiverSourceConfig Config;
			Config.TransportName = FName(TEXT("Loopback"));
			Config.TransportOptions.Add(TEXT("channel"), Channel);
			Source = MakeShared<FO3DReceiverSource>(Config);
			if (SourceSettings)
			{
				Source->InitializeSettings(SourceSettings);
			}
			FO3DReceiverCorrectnessTestAccessor::BindRecorder(*Source, Recorder); // lets mocap frames through without a LiveLink client
			return Test.TestTrue(TEXT("Receiver source starts"), FO3DReceiverSourceTestAccessor::StartTransport(*Source));
		}

		void Stop()
		{
			if (Source.IsValid())
			{
				FO3DReceiverSourceTestAccessor::StopTransport(*Source);
			}
			if (Sender.IsValid())
			{
				Sender->Stop();
			}
		}
	};

	/** Publisher ticks, then the receiver source ticks (polls Loopback and runs control). */
	void Pump(FO3DControlPublisher& Publisher, FLoopbackPair& Pair)
	{
		Publisher.Tick(*Pair.Sender);
		Pair.Source->Tick(0.0f);
	}

	UO3DRemoteControlComponent* MakeComponent(UO3DControlTestListener*& OutListener)
	{
		UO3DRemoteControlComponent* Component = NewObject<UO3DRemoteControlComponent>();
		OutListener = NewObject<UO3DControlTestListener>();
		Component->OnControlEvent.AddDynamic(OutListener, &UO3DControlTestListener::OnEvent);
		Component->OnControlValueChanged.AddDynamic(OutListener, &UO3DControlTestListener::OnValueChanged);
		Component->OnControlValueCleared.AddDynamic(OutListener, &UO3DControlTestListener::OnValueCleared);
		FO3DRemoteControlComponentTestAccessor::Bind(*Component);
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverEndToEndTest, "Open3DBroadcast.Receiver.Control.EndToEndOverLoopback", O3DB_TEST_FLAGS)
bool FO3DControlReceiverEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/true, /*bAlign=*/true); // aligned by default; a control-only sender is never held
	FLoopbackPair Pair;
	ON_SCOPE_EXIT { Pair.Stop(); };
	if (!Pair.Start(*this))
	{
		return false;
	}
	UO3DControlTestListener* Listener = nullptr;
	UO3DRemoteControlComponent* Component = MakeComponent(Listener);
	ON_SCOPE_EXIT { FO3DRemoteControlComponentTestAccessor::Unbind(*Component); };

	FO3DControlPublisher Publisher(TEXT("BP_StageController"));
	Publisher.SetConfig(/*Snapshot*/ 1.0, /*Redundancy*/ 3, /*Rate*/ 30.0);
	TestTrue(TEXT("A value can be set before the session starts"), Publisher.SetValue(TEXT("env.fog_density"), FString(), FO3DControlValue::MakeFloat(0.35)));
	Publisher.Start();
	TestTrue(TEXT("Targeted value"), Publisher.SetValue(TEXT("char.emotion"), TEXT("Hero"), FO3DControlValue::MakeName(TEXT("Joy"))));
	TestTrue(TEXT("Event fired"), Publisher.FireEvent(TEXT("vfx.muzzle_flash"), TEXT("Hero"), FO3DControlValue::MakeName(TEXT("left_hand"))));
	Pump(Publisher, Pair);

	TestEqual(TEXT("The event reached the component once"), Listener->Events.Num(), 1);
	TestEqual(TEXT("Event name"), Listener->Events.Num() > 0 ? Listener->Events[0] : FString(), FString(TEXT("vfx.muzzle_flash")));
	TestTrue(TEXT("Event payload"), Listener->LastEventValue == FO3DControlValue::MakeName(TEXT("left_hand")));
	TestEqual(TEXT("Event target"), Listener->LastEventMeta.TargetSubject, FString(TEXT("Hero")));
	TestEqual(TEXT("Source name"), Listener->LastEventMeta.SourceName, FString(TEXT("BP_StageController")));
	TestEqual(TEXT("Source id"), Listener->LastEventMeta.SourceId, Publisher.GetSourceId());
	TestEqual(TEXT("Stream id is the Loopback channel"), Listener->LastEventMeta.StreamId, Pair.Channel);
	TestTrue(TEXT("Both values arrived"), Listener->Changed.Contains(TEXT("env.fog_density")) && Listener->Changed.Contains(TEXT("char.emotion")));

	FO3DControlValue Fog;
	TestTrue(TEXT("Queryable on the component"), Component->GetControlValue(TEXT("env.fog_density"), FString(), Fog) && Fog == FO3DControlValue::MakeFloat(0.35));
	FO3DControlValue Emotion;
	TestTrue(TEXT("Targeted value queryable"), Component->GetControlValue(TEXT("char.emotion"), TEXT("Hero"), Emotion) && Emotion == FO3DControlValue::MakeName(TEXT("Joy")));
	TestEqual(TEXT("GetAllControlValues"), Component->GetAllControlValues().Num(), 2);

	// Clear, and the redundant event copies arriving later are not fired again.
	Publisher.ClearValue(TEXT("env.fog_density"), FString());
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Pump(Publisher, Pair);
	}
	TestTrue(TEXT("Clear reached the component"), Listener->Cleared.Contains(TEXT("env.fog_density")));
	TestEqual(TEXT("Redundant event copies were de-duplicated"), Listener->Events.Num(), 1);
	TestEqual(TEXT("Nothing reached LiveLink"), Pair.Recorder->Frames.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverFiltersTest, "Open3DBroadcast.Receiver.Control.ComponentFilters", O3DB_TEST_FLAGS)
bool FO3DControlReceiverFiltersTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/true, /*bAlign=*/false);
	FLoopbackPair Pair;
	ON_SCOPE_EXIT { Pair.Stop(); };
	if (!Pair.Start(*this))
	{
		return false;
	}
	UO3DControlTestListener* HeroListener = nullptr;
	UO3DRemoteControlComponent* Hero = MakeComponent(HeroListener);
	Hero->TargetSubjectFilter = TEXT("Hero");
	Hero->NamePrefixFilter = TEXT("char.");
	UO3DControlTestListener* VillainListener = nullptr;
	UO3DRemoteControlComponent* Villain = MakeComponent(VillainListener);
	Villain->TargetSubjectFilter = TEXT("Villain");
	Villain->bIncludeUntargeted = false;
	UO3DControlTestListener* OtherStreamListener = nullptr;
	UO3DRemoteControlComponent* OtherStream = MakeComponent(OtherStreamListener);
	OtherStream->StreamIdFilter = TEXT("some-other-stream");
	ON_SCOPE_EXIT
	{
		FO3DRemoteControlComponentTestAccessor::Unbind(*Hero);
		FO3DRemoteControlComponentTestAccessor::Unbind(*Villain);
		FO3DRemoteControlComponentTestAccessor::Unbind(*OtherStream);
	};

	FO3DControlPublisher Publisher(TEXT("Stage"));
	Publisher.Start();
	Publisher.SetValue(TEXT("char.wetness"), TEXT("Hero"), FO3DControlValue::MakeFloat(0.5));
	Publisher.SetValue(TEXT("char.wetness"), TEXT("Villain"), FO3DControlValue::MakeFloat(0.9));
	Publisher.SetValue(TEXT("char.global_tint"), FString(), FO3DControlValue::MakeColor(FLinearColor::Red));
	Publisher.SetValue(TEXT("env.fog_density"), FString(), FO3DControlValue::MakeFloat(0.2));
	Pump(Publisher, Pair);

	TestEqual(TEXT("Hero: its own value plus the untargeted char. value"), HeroListener->Changed.Num(), 2);
	TestEqual(TEXT("Villain: its own value only (untargeted excluded)"), VillainListener->Changed.Num(), 1);
	TestTrue(TEXT("Villain got 0.9"), VillainListener->LastChangedValue == FO3DControlValue::MakeFloat(0.9));
	TestEqual(TEXT("A different stream filter receives nothing"), OtherStreamListener->Changed.Num(), 0);
	TestEqual(TEXT("Hero's value query respects the prefix and target"), Hero->GetAllControlValues().Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverEnablementTest, "Open3DBroadcast.Receiver.Control.EnablementPrecedence", O3DB_TEST_FLAGS)
bool FO3DControlReceiverEnablementTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/false, /*bAlign=*/false);

	TestFalse(TEXT("Off by default (project setting false)"), UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::ProjectDefault));
	UO3DControlLibrary::SetControlReceiveEnabled(true);
	TestTrue(TEXT("Runtime override enables"), UO3DControlLibrary::IsControlReceiveEnabled());
	TestFalse(TEXT("A per-source Disabled beats a runtime true"), UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::Disabled));
	UO3DControlLibrary::ClearControlReceiveOverride();
	TestFalse(TEXT("Clearing the override returns to the project setting"), UO3DControlLibrary::IsControlReceiveEnabled());
	TestTrue(TEXT("A per-source Enabled beats a project false"), UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::Enabled));
	Scoped.Settings->bAcceptControl = true;
	TestTrue(TEXT("Project setting true"), UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::ProjectDefault));
	UO3DControlLibrary::SetControlReceiveEnabled(false);
	TestFalse(TEXT("Runtime false beats a project true"), UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::ProjectDefault));
	UO3DControlLibrary::ClearControlReceiveOverride();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverRuntimeToggleTest, "Open3DBroadcast.Receiver.Control.OffByDefaultThenEnabledAtRuntime", O3DB_TEST_FLAGS)
bool FO3DControlReceiverRuntimeToggleTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/false, /*bAlign=*/false);
	FLoopbackPair Pair;
	ON_SCOPE_EXIT { Pair.Stop(); UO3DControlLibrary::ClearControlReceiveOverride(); };
	if (!Pair.Start(*this))
	{
		return false;
	}
	UO3DControlTestListener* Listener = nullptr;
	UO3DRemoteControlComponent* Component = MakeComponent(Listener);
	ON_SCOPE_EXIT { FO3DRemoteControlComponentTestAccessor::Unbind(*Component); };

	FO3DControlPublisher Publisher(TEXT("Stage"));
	Publisher.SetConfig(/*Snapshot*/ 0.25, /*Redundancy*/ 1, /*Rate*/ 30.0);
	Publisher.Start();
	Publisher.SetValue(TEXT("light.intensity"), FString(), FO3DControlValue::MakeFloat(2.0));
	Publisher.FireEvent(TEXT("light.cue"), FString(), FO3DControlValue::MakeInt(4));
	Pump(Publisher, Pair);
	TestEqual(TEXT("Off by default: nothing delivered"), Listener->Changed.Num() + Listener->Events.Num(), 0);
	TestTrue(TEXT("Dropped payloads counted"), FO3DReceiverSourceTestAccessor::GetControlPayloadsDroppedDisabled(*Pair.Source) > 0);

	// The client turns control on itself; the next snapshot brings the current values.
	UO3DControlLibrary::SetControlReceiveEnabled(true);
	const bool bArrived = O3DTests::PollUntil(2.0,
		[&Listener]() { return Listener->Changed.Contains(TEXT("light.intensity")); },
		[&Publisher, &Pair]() { Pump(Publisher, Pair); });
	TestTrue(TEXT("Enabled at runtime: values arrive within a snapshot interval"), bArrived);

	// Turning it off again discards silently: no Cleared, and the bus forgets the sender.
	const int32 ClearedBefore = Listener->Cleared.Num();
	UO3DControlLibrary::SetControlReceiveEnabled(false);
	Pump(Publisher, Pair);
	TestEqual(TEXT("Disabling fires no Cleared"), Listener->Cleared.Num(), ClearedBefore);
	FO3DControlValue Ignored;
	TestFalse(TEXT("Disabling forgets the values"), Component->GetControlValue(TEXT("light.intensity"), FString(), Ignored));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverPerSourceTest, "Open3DBroadcast.Receiver.Control.PerSourceDisabledWins", O3DB_TEST_FLAGS)
bool FO3DControlReceiverPerSourceTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/true, /*bAlign=*/false);
	UO3DReceiverSourceSettings* SourceSettings = NewObject<UO3DReceiverSourceSettings>();
	SourceSettings->ControlAccept = EO3DControlAcceptMode::Disabled;
	FLoopbackPair Pair;
	ON_SCOPE_EXIT { Pair.Stop(); };
	if (!Pair.Start(*this, SourceSettings))
	{
		return false;
	}
	UO3DControlTestListener* Listener = nullptr;
	UO3DRemoteControlComponent* Component = MakeComponent(Listener);
	ON_SCOPE_EXIT { FO3DRemoteControlComponentTestAccessor::Unbind(*Component); };

	FO3DControlPublisher Publisher(TEXT("Stage"));
	Publisher.Start();
	Publisher.SetValue(TEXT("env.fog_density"), FString(), FO3DControlValue::MakeFloat(0.1));
	Pump(Publisher, Pair);
	TestEqual(TEXT("This source refuses control even though the project accepts it"), Listener->Changed.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlReceiverAlignmentTest, "Open3DBroadcast.Receiver.Control.AlignedToTheMocapStream", O3DB_TEST_FLAGS)
bool FO3DControlReceiverAlignmentTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlReceiverTests;
	FScopedControlSettings Scoped(/*bAccept=*/true, /*bAlign=*/true);
	Scoped.Settings->MaxAlignmentHoldMs = 300;
	FLoopbackPair Pair;
	ON_SCOPE_EXIT { Pair.Stop(); };
	if (!Pair.Start(*this))
	{
		return false;
	}
	UO3DControlTestListener* Listener = nullptr;
	UO3DRemoteControlComponent* Component = MakeComponent(Listener);
	ON_SCOPE_EXIT { FO3DRemoteControlComponentTestAccessor::Unbind(*Component); };

	// Mocap frames for subject "Hero" whose SubjectList.time is far behind the sender clock: the
	// pose on screen is "older" than any cue fired now.
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("Hero"), 1);
	auto SendPose = [&Pair, &Frames]()
	{
		Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frames[0].GetData(), Frames[0].Num(), TEXT("Hero"), 0.0));
	};

	FO3DControlPublisher Publisher(TEXT("Stage"));
	Publisher.SetMocapSubjects({ TEXT("Hero") });
	Publisher.Start();
	SendPose();
	Pair.Source->Tick(0.0f);

	TestTrue(TEXT("Cue fired"), Publisher.FireEvent(TEXT("audio.sting"), TEXT("Hero"), FO3DControlValue()));
	SendPose();
	Pump(Publisher, Pair);
	TestEqual(TEXT("Held while the mocap stream's pose is behind the cue"), Listener->Events.Num(), 0);
	TestTrue(TEXT("The cue is in the hold queue"), FO3DReceiverSourceTestAccessor::GetHeldControlChanges(*Pair.Source) > 0);

	// Keep the stream live; the cue is released no later than the hold cap, never dropped.
	const double Start = FPlatformTime::Seconds();
	const bool bReleased = O3DTests::PollUntil(2.0,
		[&Listener]() { return Listener->Events.Num() > 0; },
		[&SendPose, &Publisher, &Pair]() { SendPose(); Pump(Publisher, Pair); });
	TestTrue(TEXT("Released late rather than dropped"), bReleased);
	TestTrue(TEXT("Released at about the hold cap"), FPlatformTime::Seconds() - Start < 1.5);
	TestEqual(TEXT("Counted as late"), FO3DReceiverSourceTestAccessor::GetAlignerStats(*Pair.Source).released_late, static_cast<uint64_t>(1));

	// A sender with no mocap stream here is never held.
	FO3DControlPublisher ControlOnly(TEXT("LightingDesk"));
	ControlOnly.Start();
	ControlOnly.FireEvent(TEXT("light.cue"), FString(), FO3DControlValue::MakeInt(7));
	Pump(ControlOnly, Pair);
	TestEqual(TEXT("Control-only sender: delivered at once"), Listener->Events.Num(), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
