// Copyright (c) Open3DStream Contributors
//
// WP-S3 sender wire-correctness tests (findings SND-1, SND-2, SND-3, SND-4, SND-5, SND-13, SND-14,
// SND-19, SND-20; ADR 0005). The serializer is driven directly with frames that carry their own
// descriptor and encoding settings, and its bytes are parsed by the real core SubjectList. No world,
// mesh, transport or network is involved. The pure policy logic (rate limiter, curve filter,
// full-sync tracker, anchor re-sync) is covered in more depth by test/sender_wire_tests.cpp (CTest).
// These tests will move to the Open3DBroadcastTests module in WP-T2 (ADR 0006).

#include "O3DSenderComponent.h"
#include "O3DSenderCurveProcessor.h"
#include "O3DSenderSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "UObject/Package.h"

#include "o3ds/model.h"

#include <string>

/** White-box access for these tests (friend of UO3DSenderComponent and FO3DSenderCurveProcessor). */
struct FO3DSenderWireTestAccess
{
	static bool ConsumeCaptureBudget(double NowSeconds, double& InOutLastCaptureTime, float CaptureRateHz)
	{
		return UO3DSenderComponent::ConsumeCaptureBudget(NowSeconds, InOutLastCaptureTime, CaptureRateHz);
	}

	static void SetCurves(FO3DSenderCurveProcessor& Processor, const TArray<FName>& Names, const TArray<float>& Values)
	{
		Processor.CurveNames = Names;
		Processor.CurveValues = Values;
		Processor.LastSentCurveValues.SetNumZeroed(Names.Num());
		Processor.LastSentHasValue.SetNumZeroed(Names.Num());
		Processor.CurveNameSet.Reset();
		for (const FName& Name : Names)
		{
			Processor.CurveNameSet.Add(Name);
		}
		Processor.bCurveCacheInitialized = true;
		++Processor.CurveRevision;
	}

	static void SetCurveValues(FO3DSenderCurveProcessor& Processor, const TArray<float>& Values)
	{
		Processor.CurveValues = Values;
	}

	static void SetDescriptor(UO3DSenderComponent& Component, const FO3DSSkeletonDescriptor& Descriptor)
	{
		Component.DescriptorCache = Descriptor;
		Component.DescriptorSnapshot = MakeShared<FO3DSSkeletonDescriptor>(Descriptor);
	}

	static const FO3DSSkeletonDescriptor& GetDescriptorCache(const UO3DSenderComponent& Component) { return Component.DescriptorCache; }
	static bool HasDescriptorSnapshot(const UO3DSenderComponent& Component) { return Component.DescriptorSnapshot.IsValid(); }
	static void SetCapturing(UO3DSenderComponent& Component, bool bCapturing) { Component.bIsCapturing = bCapturing; }
	static bool HasSerializer(const UO3DSenderComponent& Component) { return Component.Serializer.IsValid(); }
	static void EnsureSubjectNameCached(UO3DSenderComponent& Component) { Component.EnsureSubjectNameCached(nullptr); }
	static FO3DSPoseFrame CreateFrameShell(UO3DSenderComponent& Component, double CaptureTimeSec) { return Component.CreateFrameShell(nullptr, CaptureTimeSec); }
};

namespace O3DSenderWireTests
{
	struct FBone
	{
		const TCHAR* Name;
		int32 Parent;
	};

	TSharedPtr<const FO3DSSkeletonDescriptor> MakeDescriptor(const TArray<FBone>& Bones)
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		for (const FBone& Bone : Bones)
		{
			Descriptor->BoneNames.Add(FName(Bone.Name));
			Descriptor->ParentIndices.Add(Bone.Parent);
		}
		Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
		return Descriptor;
	}

	FO3DSenderEncodingSettings MakeSettings(EO3DSenderEncodingMode Mode)
	{
		FO3DSenderEncodingSettings Settings;
		Settings.Mode = Mode;
		Settings.ResidualDeltaThreshold = 1.0e-6f;
		Settings.QuantizationDeltaThreshold = 1.0e-6f;
		Settings.FullSyncIntervalSeconds = 1.0f;
		return Settings;
	}

	FO3DSPoseFrame MakeFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor,
		const TArray<FVector>& Translations, double CaptureTimeSec, const FO3DSenderEncodingSettings& Settings)
	{
		FO3DSPoseFrame Frame;
		Frame.Subject = Subject;
		Frame.Descriptor = Descriptor;
		Frame.CaptureTimeSec = CaptureTimeSec;
		Frame.Encoding = Settings;
		for (const FVector& Translation : Translations)
		{
			Frame.BoneLocalTransforms.Add(FTransform(FQuat::Identity, Translation, FVector::OneVector));
		}
		return Frame;
	}

	/** Collects every serialized packet. */
	struct FPacketSink
	{
		TArray<TArray<uint8>> Packets;
		FDelegateHandle Handle;
		FO3DSenderSerializer* Serializer = nullptr;

		explicit FPacketSink(FO3DSenderSerializer& InSerializer)
			: Serializer(&InSerializer)
		{
			Handle = InSerializer.OnSerializedFrame.AddLambda([this](const FString&, const TArray<uint8>& Buffer, double)
			{
				Packets.Add(Buffer);
			});
		}

		~FPacketSink()
		{
			Serializer->OnSerializedFrame.Remove(Handle);
		}
	};

	bool Parse(O3DS::SubjectList& Receiver, const TArray<uint8>& Packet)
	{
		return Receiver.Parse(reinterpret_cast<const char*>(Packet.GetData()), (size_t)Packet.Num(), nullptr, true);
	}

	O3DS::Subject* FindSubject(O3DS::SubjectList& Receiver, const FString& Name)
	{
		return Receiver.findSubject(std::string(TCHAR_TO_UTF8(*Name)));
	}

	/** Checks the receiver holds `Subject` with exactly the descriptor's bone names and parents. */
	bool CheckTopology(FAutomationTestBase& Test, const FString& Context, O3DS::SubjectList& Receiver, const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)
	{
		O3DS::Subject* Parsed = FindSubject(Receiver, Subject);
		if (!Test.TestNotNull(*FString::Printf(TEXT("%s: subject '%s' present"), *Context, *Subject), Parsed))
		{
			return false;
		}
		if (!Test.TestEqual(*FString::Printf(TEXT("%s: bone count"), *Context), (int32)Parsed->mTransforms.size(), Descriptor.BoneNames.Num()))
		{
			return false;
		}
		bool bAllMatch = true;
		for (int32 Index = 0; Index < Descriptor.BoneNames.Num(); ++Index)
		{
			const FString ParsedName = UTF8_TO_TCHAR(Parsed->mTransforms[Index]->mName.c_str());
			bAllMatch &= Test.TestEqual(*FString::Printf(TEXT("%s: bone %d name"), *Context, Index), ParsedName, Descriptor.BoneNames[Index].ToString());
			bAllMatch &= Test.TestEqual(*FString::Printf(TEXT("%s: bone %d parent"), *Context, Index), Parsed->mTransforms[Index]->mParentId, Descriptor.ParentIndices[Index]);
		}
		return bAllMatch;
	}

	const TCHAR* ModeName(EO3DSenderEncodingMode Mode)
	{
		switch (Mode)
		{
		case EO3DSenderEncodingMode::Residual: return TEXT("Residual");
		case EO3DSenderEncodingMode::Quantized: return TEXT("Quantized");
		case EO3DSenderEncodingMode::Legacy:
		default: return TEXT("Legacy");
		}
	}

	const TArray<FBone>& ThreeBones()
	{
		static const TArray<FBone> Bones = { { TEXT("Root"), -1 }, { TEXT("Spine"), 0 }, { TEXT("Head"), 1 } };
		return Bones;
	}

	TArray<FVector> ThreePose(double T)
	{
		return { FVector(10.0 + 0.3 * T, 0.0, 90.0), FVector(0.0, 0.0, 20.0 + 0.002 * T), FVector(0.0, 0.001 * T, 15.0) };
	}

	/** Deterministic jitter in [-1, 1]. */
	struct FLcg
	{
		uint32 State;
		explicit FLcg(uint32 Seed) : State(Seed) {}
		double Next()
		{
			State = State * 1664525u + 1013904223u;
			return ((double)(State >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
		}
	};
}

// SND-1: after Stop/Start (the serializer's caches are cleared), the first frame is a full sync
// carrying the real bone names and parents, in every encoding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireStopStartTest, "Open3DBroadcast.Sender.Wire.StopStartKeepsNamesAndParents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireStopStartTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());
	const FString Subject = TEXT("Actor");

	for (EO3DSenderEncodingMode Mode : { EO3DSenderEncodingMode::Legacy, EO3DSenderEncodingMode::Quantized, EO3DSenderEncodingMode::Residual })
	{
		const FString Context = ModeName(Mode);
		FO3DSenderSerializer Serializer;
		FPacketSink Sink(Serializer);
		const FO3DSenderEncodingSettings Settings = MakeSettings(Mode);

		for (int32 Index = 0; Index < 3; ++Index)
		{
			Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, Descriptor, ThreePose(Index / 60.0), 10.0 + Index / 60.0, Settings));
		}

		// StopCapture() detaches and clears the serializer; a restart resamples with the same
		// descriptor. Two cycles, as a details-panel edit during PIE would do.
		for (int32 Cycle = 0; Cycle < 2; ++Cycle)
		{
			Serializer.ClearAllCaches();
			const int32 FirstPacket = Sink.Packets.Num();
			for (int32 Index = 0; Index < 3; ++Index)
			{
				Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, Descriptor, ThreePose(Index / 60.0), 20.0 + Cycle + Index / 60.0, Settings));
			}
			TestEqual(*FString::Printf(TEXT("%s cycle %d: three packets"), *Context, Cycle), Sink.Packets.Num(), FirstPacket + 3);
			TestEqual(*FString::Printf(TEXT("%s cycle %d: first frame after restart is a full sync"), *Context, Cycle),
				Serializer.GetSubjectStats(Subject).FullSyncsSent, (uint64)(Mode == EO3DSenderEncodingMode::Legacy ? 3 : 1));

			// A receiver that only sees the restarted stream gets correct names and parents, and keeps
			// them through the following updates.
			O3DS::SubjectList Receiver;
			for (int32 Packet = FirstPacket; Packet < Sink.Packets.Num(); ++Packet)
			{
				TestTrue(*FString::Printf(TEXT("%s cycle %d: packet %d parses"), *Context, Cycle, Packet), Parse(Receiver, Sink.Packets[Packet]));
				CheckTopology(*this, FString::Printf(TEXT("%s cycle %d packet %d"), *Context, Cycle, Packet), Receiver, Subject, *Descriptor);
			}
		}
	}
	return true;
}

// SND-1 / ADR 0005 (i): frames are dropped, never padded, when the descriptor is missing or does not
// match the bone count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireDropMismatchTest, "Open3DBroadcast.Sender.Wire.DropsFrameWithoutMatchingDescriptor", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireDropMismatchTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const FString Subject = TEXT("Actor");
	const FO3DSenderEncodingSettings Settings = MakeSettings(EO3DSenderEncodingMode::Quantized);

	// One warning for both drops: the second falls inside the rate-limit window.
	AddExpectedError(TEXT("Dropping frame for subject"), EAutomationExpectedMessageFlags::Contains, 1);

	FO3DSenderSerializer Serializer;
	FPacketSink Sink(Serializer);

	Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, nullptr, ThreePose(0.0), 1.0, Settings));

	const TSharedPtr<const FO3DSSkeletonDescriptor> TwoBones = MakeDescriptor({ { TEXT("Root"), -1 }, { TEXT("Spine"), 0 } });
	Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, TwoBones, ThreePose(0.0), 1.1, Settings));

	TestEqual(TEXT("No packet for dropped frames"), Sink.Packets.Num(), 0);
	TestEqual(TEXT("Both frames counted as dropped"), Serializer.GetSubjectStats(Subject).DroppedFrames, (uint64)2);

	// A valid frame afterwards is serialized normally.
	Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, MakeDescriptor(ThreeBones()), ThreePose(0.0), 1.2, Settings));
	TestEqual(TEXT("Valid frame serialized"), Sink.Packets.Num(), 1);
	return true;
}

// SND-1: a subject rename starts the new name with a full sync carrying names and parents.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireRenameTest, "Open3DBroadcast.Sender.Wire.RenameSendsDescriptorUnderNewName", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireRenameTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());

	for (EO3DSenderEncodingMode Mode : { EO3DSenderEncodingMode::Legacy, EO3DSenderEncodingMode::Quantized, EO3DSenderEncodingMode::Residual })
	{
		for (bool bPurgeOldName : { true, false })
		{
			const FString Context = FString::Printf(TEXT("%s purge=%d"), ModeName(Mode), bPurgeOldName ? 1 : 0);
			FO3DSenderSerializer Serializer;
			FPacketSink Sink(Serializer);
			const FO3DSenderEncodingSettings Settings = MakeSettings(Mode);

			for (int32 Index = 0; Index < 3; ++Index)
			{
				Serializer.SerializePoseFrame(TEXT("Alpha"), MakeFrame(TEXT("Alpha"), Descriptor, ThreePose(Index / 60.0), 5.0 + Index / 60.0, Settings));
			}

			// The component purges the old name's cache on rename (EnsureSubjectNameCached).
			if (bPurgeOldName)
			{
				Serializer.RemoveSubjectCache(TEXT("Alpha"));
			}

			const int32 FirstRenamed = Sink.Packets.Num();
			for (int32 Index = 3; Index < 6; ++Index)
			{
				Serializer.SerializePoseFrame(TEXT("Beta"), MakeFrame(TEXT("Beta"), Descriptor, ThreePose(Index / 60.0), 5.0 + Index / 60.0, Settings));
			}

			TestTrue(*FString::Printf(TEXT("%s: new name starts with a full sync"), *Context), Serializer.GetSubjectStats(TEXT("Beta")).FullSyncsSent >= 1);

			O3DS::SubjectList Receiver;
			for (int32 Packet = FirstRenamed; Packet < Sink.Packets.Num(); ++Packet)
			{
				TestTrue(*FString::Printf(TEXT("%s: packet %d parses"), *Context, Packet), Parse(Receiver, Sink.Packets[Packet]));
				CheckTopology(*this, FString::Printf(TEXT("%s packet %d"), *Context, Packet), Receiver, TEXT("Beta"), *Descriptor);
			}
		}
	}
	return true;
}

// SND-2 / SND-13: quantized full sync, updates, periodic resync, updates; every decoded translation is
// within the quantization tolerance, for a receiver present from the start and one that joins late.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireQuantizedResyncTest, "Open3DBroadcast.Sender.Wire.QuantizedResyncStaysWithinTolerance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireQuantizedResyncTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());
	const FString Subject = TEXT("Actor");
	FO3DSenderEncodingSettings Settings = MakeSettings(EO3DSenderEncodingMode::Quantized);
	Settings.QuantizationByteRange = 0.01f;
	Settings.QuantizationHalfRange = 1.0f;

	FO3DSenderSerializer Serializer;
	FPacketSink Sink(Serializer);
	O3DS::SubjectList Receiver;
	O3DS::SubjectList LateReceiver;
	bool bLateJoined = false;
	uint64 LastFullSyncs = 0;

	// Half-tier step plus float32 rounding of values around 100.
	const double Tolerance = 1.0 / 32767.0 + 2.0e-5;

	// 2.5 s at 60 Hz: full syncs at t=100, 101 and 102.
	for (int32 Index = 0; Index < 150; ++Index)
	{
		const double T = Index / 60.0;
		const TArray<FVector> Pose = ThreePose(T);
		Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, Descriptor, Pose, 100.0 + T, Settings));
		if (!TestEqual(TEXT("One packet per frame"), Sink.Packets.Num(), Index + 1))
		{
			return false;
		}

		const uint64 FullSyncs = Serializer.GetSubjectStats(Subject).FullSyncsSent;
		const bool bFull = FullSyncs != LastFullSyncs;
		LastFullSyncs = FullSyncs;
		if (bFull && FullSyncs == 2)
		{
			bLateJoined = true;
		}

		TestTrue(TEXT("Packet parses"), Parse(Receiver, Sink.Packets.Last()));
		if (bLateJoined)
		{
			TestTrue(TEXT("Packet parses (late joiner)"), Parse(LateReceiver, Sink.Packets.Last()));
		}

		for (O3DS::SubjectList* Rx : { &Receiver, bLateJoined ? &LateReceiver : nullptr })
		{
			if (Rx == nullptr)
			{
				continue;
			}
			O3DS::Subject* Parsed = FindSubject(*Rx, Subject);
			if (!TestNotNull(TEXT("Subject present"), Parsed) || !TestEqual(TEXT("Bone count"), (int32)Parsed->mTransforms.size(), 3))
			{
				return false;
			}
			for (int32 Bone = 0; Bone < 3; ++Bone)
			{
				const O3DS::Vector3d& Got = Parsed->mTransforms[Bone]->translation.value;
				const FVector& Want = Pose[Bone];
				const double Error = FMath::Max3(FMath::Abs(Got.v[0] - Want.X), FMath::Abs(Got.v[1] - Want.Y), FMath::Abs(Got.v[2] - Want.Z));
				if (Error > Tolerance)
				{
					AddError(FString::Printf(TEXT("Frame %d bone %d%s: translation error %.6f exceeds %.6f"), Index, Bone, Rx == &LateReceiver ? TEXT(" (late joiner)") : TEXT(""), Error, Tolerance));
					return false;
				}
			}
		}
	}

	TestEqual(TEXT("Full syncs at start and every FullSyncIntervalSeconds"), LastFullSyncs, (uint64)3);
	return true;
}

// SND-3: curves added, swapped (same count, different names) and removed in residual and quantized
// modes. Each change resyncs exactly once and the receiver's names and values always match.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireCurveMembershipTest, "Open3DBroadcast.Sender.Wire.CurveMembershipChangeResyncs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireCurveMembershipTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor({ { TEXT("Root"), -1 }, { TEXT("Head"), 0 } });
	const FString Subject = TEXT("Actor");

	for (EO3DSenderEncodingMode Mode : { EO3DSenderEncodingMode::Quantized, EO3DSenderEncodingMode::Residual })
	{
		const FString Context = ModeName(Mode);
		FO3DSenderSerializer Serializer;
		FPacketSink Sink(Serializer);
		O3DS::SubjectList Receiver;
		const FO3DSenderEncodingSettings Settings = MakeSettings(Mode);

		TArray<FName> Names = { TEXT("Blink"), TEXT("Smile") };
		for (int32 Index = 0; Index < 40; ++Index)
		{
			if (Index == 10) { Names = { TEXT("Blink"), TEXT("JawOpen"), TEXT("Smile") }; }
			if (Index == 20) { Names = { TEXT("Frown"), TEXT("JawOpen"), TEXT("Smile") }; }
			if (Index == 30) { Names = { TEXT("Frown"), TEXT("Smile") }; }

			const double T = Index / 60.0;
			FO3DSPoseFrame Frame = MakeFrame(Subject, Descriptor, { FVector(0.01 * T, 0.0, 0.0), FVector(0.0, 15.0, 0.0) }, 50.0 + T, Settings);
			Frame.CurveNames = Names;
			for (int32 Curve = 0; Curve < Names.Num(); ++Curve)
			{
				Frame.CurveValues.Add((float)(0.5 + 0.4 * FMath::Sin(0.3 * Index + Curve)));
			}
			Serializer.SerializePoseFrame(Subject, Frame);

			if (!TestTrue(*FString::Printf(TEXT("%s frame %d parses"), *Context, Index), Sink.Packets.Num() == Index + 1 && Parse(Receiver, Sink.Packets.Last())))
			{
				return false;
			}
			O3DS::Subject* Parsed = FindSubject(Receiver, Subject);
			if (!TestNotNull(TEXT("Subject present"), Parsed) || !TestEqual(*FString::Printf(TEXT("%s frame %d curve count"), *Context, Index), (int32)Parsed->mCurveNames.size(), Names.Num()))
			{
				return false;
			}
			for (int32 Curve = 0; Curve < Names.Num(); ++Curve)
			{
				TestEqual(*FString::Printf(TEXT("%s frame %d curve %d name"), *Context, Index, Curve), FString(UTF8_TO_TCHAR(Parsed->mCurveNames[Curve].c_str())), Names[Curve].ToString());
				TestEqual(*FString::Printf(TEXT("%s frame %d curve %d value"), *Context, Index, Curve), Parsed->mCurveValues[Curve], Frame.CurveValues[Curve], 1.0e-5f);
			}
		}
		TestEqual(*FString::Printf(TEXT("%s: first frame plus one full sync per curve list change"), *Context), Serializer.GetSubjectStats(Subject).FullSyncsSent, (uint64)4);
	}
	return true;
}

// SND-14: encoding setting changes take effect atomically: a mode, predictor, keyframe interval or
// quantization range change forces exactly one full sync; a delta threshold change forces none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireEncodingChangeTest, "Open3DBroadcast.Sender.Wire.EncodingChangeForcesFullSync", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireEncodingChangeTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());
	const FString Subject = TEXT("Actor");
	FO3DSenderSerializer Serializer;
	FPacketSink Sink(Serializer);
	O3DS::SubjectList Receiver;

	double Time = 1.0;
	auto Send = [&](const FO3DSenderEncodingSettings& Settings, int32 Frames)
	{
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			Time += 1.0 / 60.0;
			Serializer.SerializePoseFrame(Subject, MakeFrame(Subject, Descriptor, ThreePose(Time), Time, Settings));
			Parse(Receiver, Sink.Packets.Last());
		}
		return Serializer.GetSubjectStats(Subject).FullSyncsSent;
	};

	FO3DSenderEncodingSettings Settings = MakeSettings(EO3DSenderEncodingMode::Quantized);
	TestEqual(TEXT("Initial full sync"), Send(Settings, 5), (uint64)1);

	Settings.QuantizationDeltaThreshold = 0.001f;
	TestEqual(TEXT("Delta threshold change needs no full sync"), Send(Settings, 5), (uint64)1);

	Settings.QuantizationByteRange = 0.02f;
	TestEqual(TEXT("Quantization range change forces one full sync"), Send(Settings, 5), (uint64)2);

	Settings.Mode = EO3DSenderEncodingMode::Residual;
	TestEqual(TEXT("Switch to residual forces one full sync"), Send(Settings, 5), (uint64)3);

	Settings.ResidualPredictor = EO3DSenderResidualPredictor::Quadratic;
	TestEqual(TEXT("Predictor change forces one full sync"), Send(Settings, 5), (uint64)4);

	Settings.ResidualKeyframeIntervalFrames = 30;
	TestEqual(TEXT("Keyframe interval change forces one full sync"), Send(Settings, 5), (uint64)5);

	Settings.Mode = EO3DSenderEncodingMode::Legacy;
	TestEqual(TEXT("Legacy sends a full pose every frame"), Send(Settings, 3), (uint64)8);

	Settings.Mode = EO3DSenderEncodingMode::Residual;
	TestEqual(TEXT("Returning to residual after legacy forces one full sync"), Send(Settings, 5), (uint64)9);

	// The receiver ends up with the sender's last pose.
	O3DS::Subject* Parsed = FindSubject(Receiver, Subject);
	if (TestNotNull(TEXT("Subject present"), Parsed))
	{
		const TArray<FVector> Want = ThreePose(Time);
		TestEqual(TEXT("Root X after mode changes"), Parsed->mTransforms[0]->translation.value.v[0], Want[0].X, 1.0e-3);
	}
	return true;
}

// SND-5: a 60 Hz tick with jitter and a 60 Hz capture rate captures about 60 frames per second.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireCaptureRateTest, "Open3DBroadcast.Sender.CaptureRate.Jittered60HzAcceptsAbout60PerSecond", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireCaptureRateTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;
	FLcg Rng(12345u);
	double LastCapture = 0.0;
	int32 Accepted = 0;
	const int32 Ticks = 600; // 10 s
	for (int32 Tick = 0; Tick < Ticks; ++Tick)
	{
		const double Now = 100.0 + Tick / 60.0 + 0.001 * Rng.Next();
		if (FO3DSenderWireTestAccess::ConsumeCaptureBudget(Now, LastCapture, 60.0f))
		{
			++Accepted;
		}
	}
	TestTrue(*FString::Printf(TEXT("Accepted %d of %d jittered 60 Hz ticks (expected 590..601)"), Accepted, Ticks), Accepted >= 590 && Accepted <= 601);
	return true;
}

// SND-4 and SND-20: the curve filter sends a return to zero once; patterns apply only while filtering is
// enabled and pattern edits take effect on the next frame; value filters are off in persistent modes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireCurveFilterTest, "Open3DBroadcast.Sender.CurveProcessor.ReturnToZeroAndPatterns", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireCurveFilterTest::RunTest(const FString& Parameters)
{
	TArray<FString> Include;
	TArray<FString> Exclude;
	FO3DSenderCurveConfig Config;
	Config.bClampMorphCurvesToUnit = false;
	Config.bEnableCurveFiltering = true;
	Config.bApplyValueFilters = true;
	Config.CurveEpsilon = 0.0005f;
	Config.CurveDeltaThreshold = 0.001f;
	Config.IncludeCurvePatterns = &Include;
	Config.ExcludeCurvePatterns = &Exclude;

	TArray<FName> Names;
	TArray<float> Values;

	{
		// Acceptance case: 0.8, 0, 0.
		FO3DSenderCurveProcessor Processor;
		FO3DSenderWireTestAccess::SetCurves(Processor, { TEXT("Smile") }, { 0.8f });
		Processor.BuildFilteredCurves(Config, Names, Values);
		TestTrue(TEXT("0.8 sent"), Names.Num() == 1 && Values.Num() == 1 && Values[0] == 0.8f);

		FO3DSenderWireTestAccess::SetCurveValues(Processor, { 0.0f });
		Processor.BuildFilteredCurves(Config, Names, Values);
		TestTrue(TEXT("Return to 0 sent once"), Names.Num() == 1 && Values.Num() == 1 && Values[0] == 0.0f);

		Processor.BuildFilteredCurves(Config, Names, Values);
		TestEqual(TEXT("Second 0 suppressed"), Names.Num(), 0);
	}

	{
		// Persistent encodings: value filters off, so the curve list stays stable.
		FO3DSenderCurveConfig Persistent = Config;
		Persistent.bApplyValueFilters = false;
		FO3DSenderCurveProcessor Processor;
		FO3DSenderWireTestAccess::SetCurves(Processor, { TEXT("Smile") }, { 0.8f });
		for (float Value : { 0.8f, 0.0f, 0.0f })
		{
			FO3DSenderWireTestAccess::SetCurveValues(Processor, { Value });
			Processor.BuildFilteredCurves(Persistent, Names, Values);
			TestTrue(TEXT("Curve present every frame with value filters off"), Names.Num() == 1 && Values[0] == Value);
		}
	}

	{
		// SND-20: patterns are ignored with filtering disabled, and edits apply on the next frame.
		FO3DSenderCurveProcessor Processor;
		FO3DSenderWireTestAccess::SetCurves(Processor, { TEXT("Blink"), TEXT("Smile") }, { 0.5f, 0.6f });
		Exclude = { TEXT("Smile") };

		FO3DSenderCurveConfig Disabled = Config;
		Disabled.bEnableCurveFiltering = false;
		Disabled.bApplyValueFilters = false;
		Processor.BuildFilteredCurves(Disabled, Names, Values);
		TestEqual(TEXT("Exclude pattern ignored while filtering is disabled"), Names.Num(), 2);

		FO3DSenderCurveConfig Enabled = Config;
		Enabled.bApplyValueFilters = false;
		Processor.BuildFilteredCurves(Enabled, Names, Values);
		TestTrue(TEXT("Exclude pattern applied while filtering is enabled"), Names.Num() == 1 && Names[0] == FName(TEXT("Blink")));

		Exclude.Reset();
		Processor.BuildFilteredCurves(Enabled, Names, Values);
		TestEqual(TEXT("Removing the pattern brings the curve back"), Names.Num(), 2);
	}
	return true;
}

// SND-1 / SND-19: component glue. StartCapture without a mesh or audio starts nothing and reports why;
// StopCapture forgets the skeleton cache; a rename re-broadcasts the descriptor under the new name;
// sampled frames carry the descriptor snapshot and encoding settings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderWireComponentGlueTest, "Open3DBroadcast.Sender.Component.StartStopRenameGlue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderWireComponentGlueTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderWireTests;

	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		Component->bEnableAudio = false;
		AddExpectedError(TEXT("Sender capture not started"), EAutomationExpectedMessageFlags::Contains, 1);
		Component->StartCapture();
		TestFalse(TEXT("Not capturing without mesh or audio"), Component->IsCapturing());
		TestFalse(TEXT("Failure reported"), Component->GetLastStartCaptureError().IsEmpty());
		TestFalse(TEXT("No serializer created on a failed start"), FO3DSenderWireTestAccess::HasSerializer(*Component));
	}

	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());
		FO3DSenderWireTestAccess::SetDescriptor(*Component, *Descriptor);
		FO3DSenderWireTestAccess::SetCapturing(*Component, true);
		Component->StopCapture();
		TestFalse(TEXT("StopCapture clears the descriptor cache"), FO3DSenderWireTestAccess::GetDescriptorCache(*Component).IsValid());
		TestEqual(TEXT("StopCapture clears the descriptor hash"), FO3DSenderWireTestAccess::GetDescriptorCache(*Component).Hash, (uint64)0);
		TestFalse(TEXT("StopCapture drops the descriptor snapshot"), FO3DSenderWireTestAccess::HasDescriptorSnapshot(*Component));
	}

	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeDescriptor(ThreeBones());
		FO3DSenderWireTestAccess::SetDescriptor(*Component, *Descriptor);

		TArray<FString> Broadcasts;
		Component->OnDescriptorReady.AddLambda([&Broadcasts](const FString& Subject, const FO3DSSkeletonDescriptor& Broadcast)
		{
			Broadcasts.Add(Subject);
		});

		Component->SubjectName = TEXT("Alpha");
		FO3DSenderWireTestAccess::EnsureSubjectNameCached(*Component);
		TestEqual(TEXT("No broadcast for the first name"), Broadcasts.Num(), 0);

		Component->SubjectName = TEXT("Beta");
		FO3DSenderWireTestAccess::EnsureSubjectNameCached(*Component);
		TestTrue(TEXT("Rename re-broadcasts the descriptor under the new name"), Broadcasts.Num() == 1 && Broadcasts[0] == TEXT("Beta"));

		Component->bEnableQuantization = true;
		Component->FullSyncIntervalSeconds = 2.0f;
		const FO3DSPoseFrame Frame = FO3DSenderWireTestAccess::CreateFrameShell(*Component, 42.0);
		TestEqual(TEXT("Frame subject"), Frame.Subject, FString(TEXT("Beta")));
		TestTrue(TEXT("Frame carries the descriptor snapshot"), Frame.Descriptor.IsValid() && Frame.Descriptor->Hash == Descriptor->Hash);
		TestEqual(TEXT("Frame carries the sampling time"), Frame.CaptureTimeSec, 42.0);
		TestTrue(TEXT("Frame carries the encoding mode"), Frame.Encoding.Mode == EO3DSenderEncodingMode::Quantized);
		TestEqual(TEXT("Frame carries the full-sync interval"), Frame.Encoding.FullSyncIntervalSeconds, 2.0f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
