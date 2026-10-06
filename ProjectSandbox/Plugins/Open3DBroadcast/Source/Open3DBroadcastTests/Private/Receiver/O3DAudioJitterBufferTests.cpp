// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// RCV-20: FO3DAudioJitterBuffer, the queue between received audio and the mixer. Pre-roll to the
// target latency, drop the oldest audio above target + 60 ms, pre-roll again after an underrun,
// and whole frames only. 48 kHz: 60 ms is 2880 samples per channel.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioJitterBuffer.h"

namespace O3DAudioJitterBufferTests
{
	/** Samples numbered from First, as PCM16 bytes, so a test can tell which audio survived. */
	TArray<uint8> Ramp(int32 First, int32 Count)
	{
		TArray<int16> Values;
		Values.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Values[Index] = static_cast<int16>((First + Index) % 32000);
		}
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Values.GetData()), Count * sizeof(int16));
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioJitterBufferPrerollTest, "Open3DBroadcast.Receiver.AudioJitterBuffer.PrerollsToTarget", O3DB_TEST_FLAGS)
bool FO3DAudioJitterBufferPrerollTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioJitterBufferTests;
	FO3DAudioJitterBuffer Buffer;
	Buffer.Configure(48000, 1, 60.0f);
	int16 Out[480];

	Buffer.Push(Ramp(0, 2000));
	TestTrue(TEXT("Buffering below the target"), Buffer.IsBuffering());
	TestEqual(TEXT("Nothing is played below the target"), Buffer.Pull(Out, 480), 0);
	TestEqual(TEXT("The audio waits"), Buffer.GetQueuedSamples(), 2000);

	Buffer.Push(Ramp(2000, 1000));
	TestEqual(TEXT("At the target, playback starts"), Buffer.Pull(Out, 480), 480);
	TestFalse(TEXT("No longer buffering"), Buffer.IsBuffering());
	TestEqual(TEXT("From the oldest audio"), static_cast<int32>(Out[0]), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioJitterBufferTrimTest, "Open3DBroadcast.Receiver.AudioJitterBuffer.DropsOldestAboveTargetPlusMargin", O3DB_TEST_FLAGS)
bool FO3DAudioJitterBufferTrimTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioJitterBufferTests;
	FO3DAudioJitterBuffer Buffer;
	Buffer.Configure(48000, 1, 60.0f);

	// 120 ms (target + margin) is kept as it is.
	Buffer.Push(Ramp(0, 5760));
	TestEqual(TEXT("Target + margin is kept"), Buffer.GetQueuedSamples(), 5760);
	TestEqual(TEXT("Nothing dropped yet"), Buffer.GetDroppedSamples(), static_cast<int64>(0));

	// One more packet: back down to the target, dropping the oldest.
	Buffer.Push(Ramp(5760, 480));
	TestEqual(TEXT("Trimmed to the target"), Buffer.GetQueuedSamples(), 2880);
	TestEqual(TEXT("The excess is counted"), Buffer.GetDroppedSamples(), static_cast<int64>(5760 + 480 - 2880));

	int16 Out[480];
	TestEqual(TEXT("Plays"), Buffer.Pull(Out, 480), 480);
	TestEqual(TEXT("The newest audio survived, the oldest was dropped"), static_cast<int32>(Out[0]), 5760 + 480 - 2880);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioJitterBufferUnderrunTest, "Open3DBroadcast.Receiver.AudioJitterBuffer.PrerollsAgainAfterUnderrun", O3DB_TEST_FLAGS)
bool FO3DAudioJitterBufferUnderrunTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioJitterBufferTests;
	FO3DAudioJitterBuffer Buffer;
	Buffer.Configure(48000, 1, 60.0f);
	TArray<int16> Out;
	Out.SetNumZeroed(4000);

	Buffer.Push(Ramp(0, 2880));
	TestEqual(TEXT("Asked for more than is queued: gets what there is"), Buffer.Pull(Out.GetData(), 4000), 2880);
	TestTrue(TEXT("The underrun starts a new pre-roll"), Buffer.IsBuffering());

	Buffer.Push(Ramp(2880, 960));
	TestEqual(TEXT("Nothing plays until the target is queued again"), Buffer.Pull(Out.GetData(), 480), 0);
	Buffer.Push(Ramp(3840, 1920));
	TestEqual(TEXT("Then it plays"), Buffer.Pull(Out.GetData(), 480), 480);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioJitterBufferFramesTest, "Open3DBroadcast.Receiver.AudioJitterBuffer.WholeFramesOnly", O3DB_TEST_FLAGS)
bool FO3DAudioJitterBufferFramesTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioJitterBufferTests;
	FO3DAudioJitterBuffer Buffer;
	Buffer.Configure(48000, 2, 0.0f);

	// Three stereo frames and a stray sample: the stray sample is not kept.
	TArray<uint8> Bytes = Ramp(0, 7);
	Buffer.Push(Bytes);
	TestEqual(TEXT("Only whole frames are queued"), Buffer.GetQueuedSamples(), 6);

	// An odd request is rounded down to whole frames, so left and right stay in place.
	int16 Out[5];
	TestEqual(TEXT("Whole frames handed out"), Buffer.Pull(Out, 5), 4);
	TestEqual(TEXT("Left first"), static_cast<int32>(Out[0]), 0);
	TestEqual(TEXT("Then right"), static_cast<int32>(Out[1]), 1);

	// Trimming keeps frame boundaries too. 20 ms target, stereo: 1920 samples, at most 80 ms (7680).
	FO3DAudioJitterBuffer Trimmed;
	Trimmed.Configure(48000, 2, 20.0f);
	Trimmed.Push(Ramp(0, 7680 + 2));
	TestEqual(TEXT("Trimmed to the target in whole frames"), Trimmed.GetQueuedSamples(), 1920);
	int16 Frame[2];
	TestEqual(TEXT("Plays a frame"), Trimmed.Pull(Frame, 2), 2);
	TestEqual(TEXT("Which starts on a left sample"), static_cast<int32>(Frame[0]) % 2, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
