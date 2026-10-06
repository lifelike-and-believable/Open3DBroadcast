// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// RCV-21: which stream a remote audio component plays. It plays one stream at a time (a source and
// its stream label), the first that matches, until that stream has been idle for a second; packets
// from other matching streams are dropped instead of interleaved into the same wave.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioJitterBuffer.h"
#include "O3DRemoteAudioComponent.h"
#include "Testing/O3DReceiverTesting.h"

namespace O3DRemoteAudioRoutingTests
{
	O3DS::FAudioFrameMeta MakeMeta(const FGuid& Source, const TCHAR* Label, int32 SampleRate = 48000)
	{
		O3DS::FAudioFrameMeta Meta;
		Meta.SourceGuid = Source;
		Meta.StreamLabel = Label;
		Meta.SubjectName = Label;
		Meta.NumChannels = 1;
		Meta.SampleRate = SampleRate;
		return Meta;
	}

	/** 10 ms of mono audio at 48 kHz. */
	TArray<uint8> Packet()
	{
		TArray<uint8> Bytes;
		Bytes.SetNumZeroed(480 * sizeof(int16));
		return Bytes;
	}

	int32 Queued(const UO3DRemoteAudioComponent* Component)
	{
		const FO3DAudioJitterBuffer* Buffer = FO3DRemoteAudioComponentTestAccessor::GetJitterBuffer(Component);
		return Buffer ? Buffer->GetQueuedSamples() : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioOneStreamTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.PlaysOneStreamAtATime", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioOneStreamTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioRoutingTests;
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();
	const FGuid SourceA = FGuid::NewGuid();
	const FGuid SourceB = FGuid::NewGuid();

	// Two receiver sources whose transports leave the label empty: both arrive as "o3ds:mix".
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceA, TEXT("o3ds:mix")), Packet());
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceB, TEXT("o3ds:mix")), Packet());
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceA, TEXT("o3ds:mix")), Packet());
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceB, TEXT("o3ds:mix")), Packet());
	TestEqual(TEXT("Only the first source's audio is queued, not both interleaved"), Queued(Component), 2 * 480);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioFormatFlipTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.OtherStreamsDoNotRecreateTheWave", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioFormatFlipTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioRoutingTests;
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();
	const FGuid SourceA = FGuid::NewGuid();
	const FGuid SourceB = FGuid::NewGuid();

	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceA, TEXT("o3ds:mix"), 48000), Packet());
	const USoundWaveProcedural* First = FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component);
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceB, TEXT("o3ds:mix"), 44100), Packet());
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16(Component, MakeMeta(SourceA, TEXT("o3ds:mix"), 48000), Packet());
	TestTrue(TEXT("A second stream in another format does not replace the wave"), FO3DRemoteAudioComponentTestAccessor::GetSoundWave(Component) == First);
	TestEqual(TEXT("The playing stream's format is kept"), FO3DRemoteAudioComponentTestAccessor::GetCurrentSampleRate(Component), 48000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioIdleReleaseTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.IdleStreamReleasesTheLock", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioIdleReleaseTest::RunTest(const FString& Parameters)
{
	using namespace O3DRemoteAudioRoutingTests;
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();
	Component->ReceiveMode = EO3DRemoteAudioMode::AnyStream;
	const FGuid Source = FGuid::NewGuid();

	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16At(Component, MakeMeta(Source, TEXT("hero")), Packet(), 10.0);
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16At(Component, MakeMeta(Source, TEXT("villain")), Packet(), 10.5);
	TestEqual(TEXT("Another stream is dropped while the first is live"), Queued(Component), 480);

	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16At(Component, MakeMeta(Source, TEXT("villain")), Packet(), 11.6);
	TestEqual(TEXT("After a second of silence the other stream plays, from its own audio"), Queued(Component), 480);
	FO3DRemoteAudioComponentTestAccessor::CallOnAudioPcm16At(Component, MakeMeta(Source, TEXT("hero")), Packet(), 11.7);
	TestEqual(TEXT("And the first is now the one dropped"), Queued(Component), 480);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DRemoteAudioModesTest, "Open3DBroadcast.Receiver.RemoteAudioComponent.ModesAndLabelFilter", O3DB_TEST_FLAGS)
bool FO3DRemoteAudioModesTest::RunTest(const FString& Parameters)
{
	UO3DRemoteAudioComponent* Component = NewObject<UO3DRemoteAudioComponent>();
	const auto Matches = [Component](const TCHAR* Subject, const TCHAR* Label)
	{
		return FO3DRemoteAudioComponentTestAccessor::CallMatchesFilter(Component, Subject, Label);
	};

	// Mix (the default) stays strict: only the receiver's fallback label.
	TestTrue(TEXT("Mix plays o3ds:mix"), Matches(TEXT("x"), O3DS::MixAudioStreamLabel));
	TestFalse(TEXT("Mix does not play a labelled stream"), Matches(TEXT("hero"), TEXT("hero")));

	Component->ReceiveMode = EO3DRemoteAudioMode::AnyStream;
	TestTrue(TEXT("Any Stream plays a labelled stream"), Matches(TEXT("hero"), TEXT("hero")));
	TestTrue(TEXT("And the mix stream"), Matches(TEXT("x"), O3DS::MixAudioStreamLabel));

	Component->StreamLabelFilter = TEXT("Hero");
	TestTrue(TEXT("The label filter matches case-insensitively"), Matches(TEXT("hero"), TEXT("hero")));
	TestFalse(TEXT("And rejects other labels"), Matches(TEXT("villain"), TEXT("villain")));

	Component->ReceiveMode = EO3DRemoteAudioMode::Subject;
	Component->LiveLinkSubjectName = FLiveLinkSubjectName(TEXT("hero"));
	TestTrue(TEXT("In Subject mode too"), Matches(TEXT("hero"), TEXT("hero")));
	TestFalse(TEXT("A subject match with another label is filtered out"), Matches(TEXT("hero"), TEXT("hero-mic")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
