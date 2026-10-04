// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 5 (SHR-38): selection by context name, end to end. Two receiver sources, each on
// its own Loopback channel, with Context Names "O3DTest.CtxA" and "O3DTest.CtxB". Control changes
// (FO3DControlPublisher -> Loopback -> receiver source -> its context's control bus) and audio
// frames (the source's real audio sink -> game thread -> its context's audio bus) reach only the
// remote control and audio components with the same name; a component with no name (the default
// context) hears neither. Named contexts last for the process, so the test forgets its control
// sources at the end.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "O3DControlBus.h"
#include "O3DControlPublisher.h"
#include "O3DControlSettings.h"
#include "O3DControlTestListener.h"
#include "O3DReceiverSource.h"
#include "O3DRemoteAudioComponent.h"
#include "O3DRemoteControlComponent.h"
#include "O3DRuntimeSubsystem.h"
#include "Sound/SoundWaveProcedural.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DRuntimeContextSelectionTests
{
	const FName ContextA(TEXT("O3DTest.CtxA"));
	const FName ContextB(TEXT("O3DTest.CtxB"));

	/** A Loopback sender and a receiver source with ContextName on one fresh channel. */
	struct FNamedPair
	{
		FString Channel = O3DTests::MakeUniqueName(TEXT("O3DContextSel"));
		TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender;
		TSharedPtr<FO3DReceiverSource> Source;
		TSharedRef<FO3DReceiverCorrectnessTestAccessor::FRecorder> Recorder = MakeShared<FO3DReceiverCorrectnessTestAccessor::FRecorder>();

		bool Start(FAutomationTestBase& Test, FName ContextName)
		{
			Sender = FO3DTransportRegistry::Get().CreateSender(FName(TEXT("Loopback")));
			FO3DTransportConfig SenderConfig(TEXT("Loopback"), EO3DTransportRole::Sender);
			SenderConfig.StreamId = Channel;
			SenderConfig.Uri = FString::Printf(TEXT("loopback://%s?role=pub"), *Channel);
			if (!Test.TestTrue(TEXT("Loopback sender starts"), Sender.IsValid() && Sender->Initialize(SenderConfig) && Sender->Start()))
			{
				return false;
			}
			FO3DReceiverSourceConfig Config;
			Config.TransportName = FName(TEXT("Loopback"));
			Config.TransportOptions.Add(TEXT("channel"), Channel);
			Config.ContextName = ContextName;
			Source = MakeShared<FO3DReceiverSource>(Config);
			FO3DReceiverCorrectnessTestAccessor::BindRecorder(*Source, Recorder);
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

	UO3DRemoteControlComponent* MakeControlComponent(FName ContextName, UO3DControlTestListener*& OutListener)
	{
		UO3DRemoteControlComponent* Component = NewObject<UO3DRemoteControlComponent>();
		Component->ContextName = ContextName;
		OutListener = NewObject<UO3DControlTestListener>();
		Component->OnControlValueChanged.AddDynamic(OutListener, &UO3DControlTestListener::OnValueChanged);
		FO3DRemoteControlComponentTestAccessor::Bind(*Component);
		return Component;
	}

	UO3DRemoteAudioComponent* MakeAudioComponent(FName ContextName)
	{
		UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();
		Component->ContextName = ContextName;
		FO3DRemoteAudioComponentTestAccessor::CallEnsureSoundWave(Component, 1, 48000);
		FO3DRemoteAudioComponentTestAccessor::BindBus(*Component);
		return Component;
	}

	int32 QueuedAudioBytes(UO3DRemoteAudioComponent* Component)
	{
		USoundWaveProcedural* Wave = FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component);
		return Wave ? Wave->GetAvailableAudioByteCount() : 0;
	}

	/** Submits one PCM16 frame through Source's real audio sink and runs the game-thread hop. */
	void SubmitAudio(FO3DReceiverSource& Source, const FString& StreamId, int32 NumBytes)
	{
		FO3DTransportConfig Config;
		Config.StreamId = StreamId;
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		FO3DReceiverSourceTestAccessor::SetActiveConfig(Source, Config);
		const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> Sink = FO3DReceiverSourceTestAccessor::MakeAudioSink(Source);
		TArray<uint8> Pcm;
		Pcm.SetNumZeroed(NumBytes);
		O3DS::FAudioFrameMeta Meta;
		Meta.StreamLabel = TEXT("o3ds:mix");
		Sink->SubmitPcm16(Meta, Pcm.GetData(), Pcm.Num());
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRuntimeContextSelectionTest, "Open3DBroadcast.Receiver.RuntimeContext.SelectionByName", O3DB_TEST_FLAGS)
bool FO3DRuntimeContextSelectionTest::RunTest(const FString& Parameters)
{
	using namespace O3DRuntimeContextSelectionTests;

	// The subsystem gives one context per name, and the empty name is the default context.
	TestTrue(TEXT("Same name, same context"), UO3DRuntimeSubsystem::Resolve(ContextA) == UO3DRuntimeSubsystem::Resolve(FName(TEXT("o3dtest.ctxa"))));
	TestTrue(TEXT("Two names, two contexts"), UO3DRuntimeSubsystem::Resolve(ContextA) != UO3DRuntimeSubsystem::Resolve(ContextB));
	TestTrue(TEXT("No name is the default context"), UO3DRuntimeSubsystem::Resolve(NAME_None) == FO3DRuntimeContext::Default());

	UO3DControlSettings* ControlSettings = GetMutableDefault<UO3DControlSettings>();
	const bool bAcceptBefore = ControlSettings->bAcceptControl;
	const bool bAlignBefore = ControlSettings->bAlignControlToMocap;
	ControlSettings->bAcceptControl = true;
	ControlSettings->bAlignControlToMocap = false;

	FNamedPair PairA;
	FNamedPair PairB;
	UO3DControlTestListener* ListenerA = nullptr;
	UO3DControlTestListener* ListenerB = nullptr;
	UO3DControlTestListener* ListenerDefault = nullptr;
	UO3DRemoteControlComponent* ControlA = MakeControlComponent(ContextA, ListenerA);
	UO3DRemoteControlComponent* ControlB = MakeControlComponent(ContextB, ListenerB);
	UO3DRemoteControlComponent* ControlDefault = MakeControlComponent(NAME_None, ListenerDefault);
	UO3DRemoteAudioComponent* AudioA = MakeAudioComponent(ContextA);
	UO3DRemoteAudioComponent* AudioB = MakeAudioComponent(ContextB);
	UO3DRemoteAudioComponent* AudioDefault = MakeAudioComponent(NAME_None);
	ON_SCOPE_EXIT
	{
		FO3DRemoteControlComponentTestAccessor::Unbind(*ControlA);
		FO3DRemoteControlComponentTestAccessor::Unbind(*ControlB);
		FO3DRemoteControlComponentTestAccessor::Unbind(*ControlDefault);
		FO3DRemoteAudioComponentTestAccessor::UnbindBus(*AudioA);
		FO3DRemoteAudioComponentTestAccessor::UnbindBus(*AudioB);
		FO3DRemoteAudioComponentTestAccessor::UnbindBus(*AudioDefault);
		PairA.Stop();
		PairB.Stop();
		for (const FName Name : { ContextA, ContextB })
		{
			FO3DControlBus::FInstance& Bus = UO3DRuntimeSubsystem::Resolve(Name)->GetControlBus();
			for (const FString& SourceId : Bus.GetSources())
			{
				Bus.ForgetSource(SourceId);
			}
		}
		ControlSettings->bAcceptControl = bAcceptBefore;
		ControlSettings->bAlignControlToMocap = bAlignBefore;
	};

	if (!PairA.Start(*this, ContextA) || !PairB.Start(*this, ContextB))
	{
		return false;
	}
	TestTrue(TEXT("Source A is in context A"), PairA.Source->GetContext() == UO3DRuntimeSubsystem::Resolve(ContextA));
	TestTrue(TEXT("Source A's metrics handle is context A's"), &PairA.Source->GetMetricsHandle()->GetAggregate() == &UO3DRuntimeSubsystem::Resolve(ContextA)->GetMetrics());

	// Control: a change sent to source A reaches only the component in context A, and the same for B.
	FO3DControlPublisher PublisherA(TEXT("StageA"));
	FO3DControlPublisher PublisherB(TEXT("StageB"));
	PublisherA.Start();
	PublisherB.Start();
	PublisherA.SetValue(TEXT("env.fog"), FString(), FO3DControlValue::MakeFloat(0.25));
	PublisherA.Tick(*PairA.Sender);
	PairA.Source->Tick(0.0f);
	TestEqual(TEXT("A's change reaches the context A component"), ListenerA->Changed.Num(), 1);
	TestEqual(TEXT("A's change does not reach the context B component"), ListenerB->Changed.Num(), 0);
	TestEqual(TEXT("A's change does not reach the default-context component"), ListenerDefault->Changed.Num(), 0);

	PublisherB.SetValue(TEXT("env.fog"), FString(), FO3DControlValue::MakeFloat(0.75));
	PublisherB.Tick(*PairB.Sender);
	PairB.Source->Tick(0.0f);
	TestEqual(TEXT("B's change reaches the context B component"), ListenerB->Changed.Num(), 1);
	TestEqual(TEXT("B's change does not reach the context A component"), ListenerA->Changed.Num(), 1);
	TestEqual(TEXT("Still nothing in the default context"), ListenerDefault->Changed.Num(), 0);

	FO3DControlValue Value;
	TestTrue(TEXT("Component A's query finds A's value"), ControlA->GetControlValue(TEXT("env.fog"), FString(), Value) && Value == FO3DControlValue::MakeFloat(0.25));
	TestTrue(TEXT("Component B's query finds B's value"), ControlB->GetControlValue(TEXT("env.fog"), FString(), Value) && Value == FO3DControlValue::MakeFloat(0.75));
	TestFalse(TEXT("The default-context component finds neither"), ControlDefault->GetControlValue(TEXT("env.fog"), FString(), Value));

	// Audio: a frame through source A's sink reaches only the context A component.
	constexpr int32 FrameBytes = 64;
	SubmitAudio(*PairA.Source, PairA.Channel, FrameBytes);
	const int32 QueuedA = QueuedAudioBytes(AudioA);
	TestTrue(TEXT("A's audio reaches the context A component"), QueuedA >= FrameBytes);
	TestEqual(TEXT("A's audio does not reach the context B component"), QueuedAudioBytes(AudioB), 0);
	TestEqual(TEXT("A's audio does not reach the default-context component"), QueuedAudioBytes(AudioDefault), 0);
	SubmitAudio(*PairB.Source, PairB.Channel, FrameBytes);
	TestTrue(TEXT("B's audio reaches the context B component"), QueuedAudioBytes(AudioB) >= FrameBytes);
	TestEqual(TEXT("B's audio does not reach the context A component"), QueuedAudioBytes(AudioA), QueuedA);
	TestEqual(TEXT("Still no audio in the default context"), QueuedAudioBytes(AudioDefault), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
