// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A2a (ADR 0008 implementation outline item 2): the settings snapshot, the serializer without a
// component, the pose frame pool, curve filtering after sampling and the sampling-time clock. No
// world, mesh, transport or network. White-box access goes through
// Open3DSender/Public/Testing/O3DSenderTesting.h (ADR 0006).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DHelpers.h"
#include "O3DSPoseFramePool.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DSenderTesting.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <limits>
#include <string>

namespace O3DSenderSnapshotTests
{
	TSharedPtr<const FO3DSSkeletonDescriptor> MakeTwoBoneDescriptor()
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		Descriptor->BoneNames = { FName(TEXT("Root")), FName(TEXT("Head")) };
		Descriptor->ParentIndices = { -1, 0 };
		Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
		return Descriptor;
	}

	void FillBones(FO3DSPoseFrame& Frame, double T)
	{
		Frame.BoneLocalTransforms.Reset();
		Frame.BoneLocalTransforms.Add(FTransform(FQuat::Identity, FVector(1.0 + T, 0.0, 90.0), FVector::OneVector));
		Frame.BoneLocalTransforms.Add(FTransform(FQuat::Identity, FVector(0.0, 0.0, 15.0), FVector::OneVector));
	}

	/** Collects every serialized packet and its timestamp. */
	struct FSnapshotPacketSink
	{
		TArray<TArray<uint8>> Packets;
		TArray<double> Timestamps;
		FDelegateHandle Handle;
		FO3DSenderSerializer* Serializer = nullptr;

		explicit FSnapshotPacketSink(FO3DSenderSerializer& InSerializer)
			: Serializer(&InSerializer)
		{
			Handle = InSerializer.OnSerializedFrame.AddLambda([this](const FString&, const TArray<uint8>& Buffer, double Timestamp)
			{
				Packets.Add(Buffer);
				Timestamps.Add(Timestamp);
			});
		}

		~FSnapshotPacketSink()
		{
			Serializer->OnSerializedFrame.Remove(Handle);
		}
	};

	bool ParsePacket(O3DS::SubjectList& Receiver, const TArray<uint8>& Packet)
	{
		return Receiver.Parse(reinterpret_cast<const char*>(Packet.GetData()), (size_t)Packet.Num(), nullptr, true);
	}

	TSharedPtr<const TArray<FString>> SharedPatterns(const TArray<FString>& Patterns)
	{
		return MakeShared<TArray<FString>>(Patterns);
	}
}

// SND-22 / ADR 0008 item 6: the serializer is built standalone and encodes a frame from the
// frame's own snapshot; a snapshot taken from a component does not change when the component's
// properties change afterwards, and an unchanged pattern list is shared, not copied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderSnapshotSerializerStandaloneTest, "Open3DBroadcast.Sender.EncodingSnapshot.SerializerWorksWithoutComponent", O3DB_TEST_FLAGS)
bool FO3DSenderSnapshotSerializerStandaloneTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSnapshotTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeTwoBoneDescriptor();

	{
		// No UObject anywhere: a serializer, a settings snapshot and frames.
		FO3DSenderSerializer Serializer;
		FSnapshotPacketSink Sink(Serializer);
		O3DS::SubjectList Receiver;

		FO3DSenderEncodingSettings Settings;
		Settings.Mode = EO3DSenderEncodingMode::Quantized;
		Settings.QuantizationDeltaThreshold = 1.0e-6f;

		for (int32 Index = 0; Index < 4; ++Index)
		{
			FO3DSPoseFrame Frame;
			Frame.Subject = TEXT("Standalone");
			Frame.Descriptor = Descriptor;
			Frame.CaptureTimeSec = 10.0 + Index / 60.0;
			Frame.Encoding = Settings;
			FillBones(Frame, Index / 60.0);
			Frame.CurveNames = { FName(TEXT("Blink")) };
			Frame.CurveValues = { 0.25f * Index };
			Serializer.SerializePoseFrame(Frame.Subject, Frame);

			if (!TestEqual(*FString::Printf(TEXT("Frame %d: one packet"), Index), Sink.Packets.Num(), Index + 1)
				|| !TestTrue(*FString::Printf(TEXT("Frame %d: packet parses"), Index), ParsePacket(Receiver, Sink.Packets.Last())))
			{
				return false;
			}
		}

		TestEqual(TEXT("One full sync, then updates"), Serializer.GetSubjectStats(TEXT("Standalone")).FullSyncsSent, (uint64)1);
		TestEqual(TEXT("One cached subject"), Serializer.GetCacheCount(), 1);
		O3DS::Subject* Parsed = Receiver.findSubject(std::string("Standalone"));
		if (TestNotNull(TEXT("Subject present"), Parsed))
		{
			TestEqual(TEXT("Bone count"), (int32)Parsed->mTransforms.size(), 2);
			TestEqual(TEXT("Root X"), Parsed->mTransforms[0]->translation.value.v[0], 1.0 + 3.0 / 60.0, 1.0e-3);
			TestTrue(TEXT("Curve value"), Parsed->mCurveValues.size() == 1 && FMath::IsNearlyEqual(Parsed->mCurveValues[0], 0.75f, 1.0e-5f));
		}
	}

	{
		// The component takes the snapshot; later edits do not reach frames already sampled.
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		FO3DSenderComponentTestAccess::SetDescriptor(*Component, *Descriptor);
		Component->SubjectName = TEXT("Snap");
		Component->bEnableQuantization = true;
		Component->bEnableCurveFiltering = true;
		Component->ExcludeCurvePatterns = { TEXT("Tongue*") };

		FO3DSPoseFrame First = FO3DSenderComponentTestAccess::CreateFrameShell(*Component, 42.0);

		Component->bEnableQuantization = false;
		Component->CurveEpsilon = 0.25f;
		Component->ExcludeCurvePatterns = { TEXT("Jaw*") };
		const FO3DSPoseFrame Second = FO3DSenderComponentTestAccess::CreateFrameShell(*Component, 43.0);
		const FO3DSPoseFrame Third = FO3DSenderComponentTestAccess::CreateFrameShell(*Component, 44.0);

		TestTrue(TEXT("First frame keeps the quantized mode"), First.Encoding.Mode == EO3DSenderEncodingMode::Quantized);
		TestTrue(TEXT("Second frame sees the edit"), Second.Encoding.Mode == EO3DSenderEncodingMode::Legacy);
		TestEqual(TEXT("First frame keeps its epsilon"), First.Encoding.CurveEpsilon, 0.0005f);
		TestEqual(TEXT("Second frame sees the new epsilon"), Second.Encoding.CurveEpsilon, 0.25f);
		TestTrue(TEXT("Value filters on in legacy mode with filtering on"), Second.Encoding.bApplyCurveValueFilters);
		TestFalse(TEXT("Value filters off in quantized mode"), First.Encoding.bApplyCurveValueFilters);
		TestTrue(TEXT("First frame keeps its exclude list"), First.Encoding.ExcludeCurvePatterns.IsValid()
			&& First.Encoding.ExcludeCurvePatterns->Num() == 1 && (*First.Encoding.ExcludeCurvePatterns)[0] == TEXT("Tongue*"));
		TestTrue(TEXT("Second frame has the new exclude list"), Second.Encoding.ExcludeCurvePatterns.IsValid()
			&& Second.Encoding.ExcludeCurvePatterns->Num() == 1 && (*Second.Encoding.ExcludeCurvePatterns)[0] == TEXT("Jaw*"));
		TestTrue(TEXT("An unchanged list is shared, not copied"), Second.Encoding.ExcludeCurvePatterns == Third.Encoding.ExcludeCurvePatterns);
		TestTrue(TEXT("A changed list is a new array"), First.Encoding.ExcludeCurvePatterns != Second.Encoding.ExcludeCurvePatterns);

		// The first frame still serializes as quantized, with a serializer that never saw the component.
		FO3DSenderSerializer Serializer;
		FSnapshotPacketSink Sink(Serializer);
		FillBones(First, 0.0);
		Serializer.SerializePoseFrame(First.Subject, First);
		O3DS::SubjectList Receiver;
		TestTrue(TEXT("Snapshot frame serialized and parses"), Sink.Packets.Num() == 1 && ParsePacket(Receiver, Sink.Packets[0]));
		TestEqual(TEXT("Snapshot frame is the quantized full sync"), Serializer.GetSubjectStats(TEXT("Snap")).FullSyncsSent, (uint64)1);
		TestNotNull(TEXT("Receiver has the subject"), Receiver.findSubject(std::string("Snap")));
	}
	return true;
}

// ADR 0008 item 4 / SND-9: frames are reused with their allocations, and the pool never holds or
// hands out more than its capacity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderFramePoolTest, "Open3DBroadcast.Sender.FramePool.ReusesFramesAndIsBounded", O3DB_TEST_FLAGS)
bool FO3DSenderFramePoolTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSnapshotTests;
	FO3DSPoseFramePool Pool(2);
	TestEqual(TEXT("Capacity"), Pool.GetCapacity(), 2);
	TestEqual(TEXT("Nothing allocated up front"), Pool.GetNumAllocated(), 0);

	TUniquePtr<FO3DSPoseFrame> First = Pool.Acquire();
	if (!TestTrue(TEXT("First frame"), First.IsValid()))
	{
		return false;
	}
	First->Subject = TEXT("Alpha");
	First->FrameIndex = 7;
	First->CaptureTimeSec = 3.0;
	First->BoneLocalTransforms.SetNum(250);
	First->RawCurveValues.SetNum(250);
	First->CurveNames.SetNum(250);
	First->CurveValues.SetNum(250);
	First->Descriptor = MakeTwoBoneDescriptor();
	First->CurveList = MakeShared<FO3DSCurveList>();
	First->Encoding.Mode = EO3DSenderEncodingMode::Residual;
	const FO3DSPoseFrame* FirstAddress = First.Get();
	const int32 BoneCapacity = First->BoneLocalTransforms.Max();

	Pool.Release(MoveTemp(First));
	TestFalse(TEXT("Release takes the frame"), First.IsValid());
	TestEqual(TEXT("One free frame"), Pool.GetNumFree(), 1);

	TUniquePtr<FO3DSPoseFrame> Reused = Pool.Acquire();
	if (!TestTrue(TEXT("Reused frame"), Reused.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Same frame object"), Reused.Get() == FirstAddress);
	TestEqual(TEXT("Bones emptied"), Reused->BoneLocalTransforms.Num(), 0);
	TestTrue(TEXT("Bone allocation kept"), Reused->BoneLocalTransforms.Max() >= BoneCapacity && BoneCapacity >= 250);
	TestTrue(TEXT("Raw curve allocation kept"), Reused->RawCurveValues.Num() == 0 && Reused->RawCurveValues.Max() >= 250);
	TestTrue(TEXT("Curve allocations kept"), Reused->CurveNames.Max() >= 250 && Reused->CurveValues.Max() >= 250);
	TestTrue(TEXT("Subject emptied"), Reused->Subject.IsEmpty());
	TestEqual(TEXT("Frame index reset"), Reused->FrameIndex, (uint64)0);
	TestEqual(TEXT("Sampling time reset"), Reused->CaptureTimeSec, 0.0);
	TestFalse(TEXT("Descriptor released"), Reused->Descriptor.IsValid());
	TestFalse(TEXT("Curve list released"), Reused->CurveList.IsValid());
	TestTrue(TEXT("Settings reset"), Reused->Encoding.Mode == EO3DSenderEncodingMode::Legacy);

	TUniquePtr<FO3DSPoseFrame> Second = Pool.Acquire();
	TestTrue(TEXT("Second frame while the first is out"), Second.IsValid() && Second.Get() != Reused.Get());
	TUniquePtr<FO3DSPoseFrame> Third = Pool.Acquire();
	TestFalse(TEXT("No third frame: the pool is bounded"), Third.IsValid());
	TestEqual(TEXT("Two frames allocated"), Pool.GetNumAllocated(), 2);

	Pool.Release(MoveTemp(Reused));
	Pool.Release(MoveTemp(Second));
	TestEqual(TEXT("Both frames free"), Pool.GetNumFree(), 2);

	for (int32 Cycle = 0; Cycle < 100; ++Cycle)
	{
		TUniquePtr<FO3DSPoseFrame> Frame = Pool.Acquire();
		TestTrue(TEXT("Steady capture always gets a frame"), Frame.IsValid());
		Pool.Release(MoveTemp(Frame));
	}
	TestEqual(TEXT("Steady capture allocates nothing new"), Pool.GetNumAllocated(), 2);

	Pool.Release(TUniquePtr<FO3DSPoseFrame>());
	Pool.Release(MakeUnique<FO3DSPoseFrame>());
	TestEqual(TEXT("A null or foreign frame is not kept"), Pool.GetNumFree(), 2);

	FO3DSPoseFramePool Tiny(0);
	TestEqual(TEXT("Capacity is at least 1"), Tiny.GetCapacity(), 1);
	return true;
}

// WP-A2a moves curve filtering after sampling: the filter works from the sampled frame (raw values
// against the shared curve list) and its snapshot. The expected values below are what the
// pre-WP-A2a FO3DSenderCurveProcessor::BuildFilteredCurves (develop at 5ee5ac6) produced for the
// same curves and settings: morph clamp, NaN replaced by 0, an exclude pattern, epsilon and delta
// filters, a return to zero sent once, filtering switched off, and a new curve list resetting the
// last-sent values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderCurveFilterAfterSamplingTest, "Open3DBroadcast.Sender.CurveFilter.AfterSamplingMatchesBefore", O3DB_TEST_FLAGS)
bool FO3DSenderCurveFilterAfterSamplingTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSnapshotTests;

	// Capture order (sorted): Blink, BrowUp (morph), JawOpen (morph), Smile, TongueOut.
	TSharedRef<FO3DSCurveList> List = MakeShared<FO3DSCurveList>();
	List->Names = { FName(TEXT("Blink")), FName(TEXT("BrowUp")), FName(TEXT("JawOpen")), FName(TEXT("Smile")), FName(TEXT("TongueOut")) };
	List->MorphMask.Init(false, List->Names.Num());
	List->MorphMask[1] = true;
	List->MorphMask[2] = true;
	const TSharedPtr<const FO3DSCurveList> SharedList = List;

	FO3DSenderEncodingSettings Filtering;
	Filtering.bClampMorphCurvesToUnit = true;
	Filtering.bDropNaNAndInfinity = true;
	Filtering.bEnableCurveFiltering = true;
	Filtering.bApplyCurveValueFilters = true;
	Filtering.CurveEpsilon = 0.0005f;
	Filtering.CurveDeltaThreshold = 0.001f;
	Filtering.IncludeCurvePatterns = SharedPatterns({});
	Filtering.ExcludeCurvePatterns = SharedPatterns({ TEXT("Tongue*") });

	FO3DSenderEncodingSettings Unfiltered = Filtering;
	Unfiltered.bEnableCurveFiltering = false;
	Unfiltered.bApplyCurveValueFilters = false;

	struct FStep
	{
		const TCHAR* Label;
		const FO3DSenderEncodingSettings* Settings;
		TArray<float> Raw;
		TArray<FName> ExpectedNames;
		TArray<float> ExpectedValues;
	};
	const float NotANumber = std::numeric_limits<float>::quiet_NaN();
	const TArray<FStep> Steps = {
		{ TEXT("first frame: clamp, NaN as 0, exclude"), &Filtering, { 0.5f, 1.4f, -0.2f, NotANumber, 0.9f },
			{ FName(TEXT("Blink")), FName(TEXT("BrowUp")) }, { 0.5f, 1.0f } },
		{ TEXT("delta below threshold suppressed, new values sent"), &Filtering, { 0.5004f, 1.0f, 0.3f, 0.2f, 0.9f },
			{ FName(TEXT("JawOpen")), FName(TEXT("Smile")) }, { 0.3f, 0.2f } },
		{ TEXT("return to zero sent once as exactly 0"), &Filtering, { 0.0001f, 1.0f, 0.0f, 0.2f, 0.0f },
			{ FName(TEXT("Blink")), FName(TEXT("JawOpen")) }, { 0.0f, 0.0f } },
		{ TEXT("second zero suppressed"), &Filtering, { 0.0001f, 1.0f, 0.0f, 0.2f, 0.0f },
			{}, {} },
		{ TEXT("filtering off: every curve, clamped, NaN as 0"), &Unfiltered, { 0.2f, 1.7f, -0.5f, NotANumber, 0.4f },
			{ FName(TEXT("Blink")), FName(TEXT("BrowUp")), FName(TEXT("JawOpen")), FName(TEXT("Smile")), FName(TEXT("TongueOut")) },
			{ 0.2f, 1.0f, 0.0f, 0.0f, 0.4f } },
	};

	FO3DSenderCurveProcessorProbe Filter;
	TArray<FName> FirstFrameNames;
	for (const FStep& Step : Steps)
	{
		FO3DSPoseFrame Frame;
		Frame.CurveList = SharedList;
		Frame.RawCurveValues = Step.Raw;
		Frame.Encoding = *Step.Settings;
		Filter.FilterFrame(Frame);

		if (!TestEqual(*FString::Printf(TEXT("%s: curve count"), Step.Label), Frame.CurveNames.Num(), Step.ExpectedNames.Num())
			|| !TestEqual(*FString::Printf(TEXT("%s: one value per name"), Step.Label), Frame.CurveValues.Num(), Frame.CurveNames.Num()))
		{
			return false;
		}
		for (int32 Index = 0; Index < Step.ExpectedNames.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("%s: name %d"), Step.Label, Index), Frame.CurveNames[Index].ToString(), Step.ExpectedNames[Index].ToString());
			TestEqual(*FString::Printf(TEXT("%s: value %d"), Step.Label, Index), Frame.CurveValues[Index], Step.ExpectedValues[Index], 1.0e-6f);
		}
		TestEqual(*FString::Printf(TEXT("%s: raw values untouched"), Step.Label), Frame.RawCurveValues.Num(), Step.Raw.Num());
		if (FirstFrameNames.Num() == 0)
		{
			FirstFrameNames = Frame.CurveNames;
		}
	}

	{
		// A new curve list (a curve cache refresh) resets the last-sent values: Blink 0.2 was the
		// last value sent and is sent again.
		TSharedRef<FO3DSCurveList> NewList = MakeShared<FO3DSCurveList>();
		NewList->Names = { FName(TEXT("Blink")), FName(TEXT("Smile")) };
		NewList->MorphMask.Init(false, NewList->Names.Num());
		FO3DSPoseFrame Frame;
		Frame.CurveList = NewList;
		Frame.RawCurveValues = { 0.2f, 0.2f };
		Frame.Encoding = Filtering;
		Filter.FilterFrame(Frame);
		TestTrue(TEXT("new curve list: both curves sent"), Frame.CurveNames.Num() == 2 && Frame.CurveValues.Num() == 2
			&& Frame.CurveValues[0] == 0.2f && Frame.CurveValues[1] == 0.2f);
	}

	{
		// A frame without a curve list (built by hand) is left as it is.
		FO3DSPoseFrame Frame;
		Frame.CurveNames = { FName(TEXT("Smile")) };
		Frame.CurveValues = { 2.0f };
		Frame.Encoding = Filtering;
		Filter.FilterFrame(Frame);
		TestTrue(TEXT("no curve list: curves unchanged"), Frame.CurveNames.Num() == 1 && Frame.CurveValues[0] == 2.0f);
	}

	{
		// The filtered first frame reaches the receiver with exactly the filtered curves.
		FO3DSenderCurveProcessorProbe FreshFilter;
		FO3DSPoseFrame Frame;
		Frame.Subject = TEXT("Face");
		Frame.Descriptor = MakeTwoBoneDescriptor();
		Frame.CaptureTimeSec = 5.0;
		FillBones(Frame, 0.0);
		Frame.CurveList = SharedList;
		Frame.RawCurveValues = Steps[0].Raw;
		Frame.Encoding = Filtering;
		FreshFilter.FilterFrame(Frame);

		FO3DSenderSerializer Serializer;
		FSnapshotPacketSink Sink(Serializer);
		Serializer.SerializePoseFrame(Frame.Subject, Frame);
		O3DS::SubjectList Receiver;
		if (TestTrue(TEXT("filtered frame parses"), Sink.Packets.Num() == 1 && ParsePacket(Receiver, Sink.Packets[0])))
		{
			O3DS::Subject* Parsed = Receiver.findSubject(std::string("Face"));
			if (TestNotNull(TEXT("subject present"), Parsed) && TestEqual(TEXT("receiver curve count"), (int32)Parsed->mCurveNames.size(), FirstFrameNames.Num()))
			{
				for (int32 Index = 0; Index < FirstFrameNames.Num(); ++Index)
				{
					TestEqual(*FString::Printf(TEXT("receiver curve %d"), Index), FString(UTF8_TO_TCHAR(Parsed->mCurveNames[Index].c_str())), FirstFrameNames[Index].ToString());
					TestEqual(*FString::Printf(TEXT("receiver value %d"), Index), Parsed->mCurveValues[Index], Steps[0].ExpectedValues[Index], 1.0e-6f);
				}
			}
		}
	}
	return true;
}

// ADR 0008 item 7 (WP-A2a): the wire time is the frame's sampling time, in every encoding and for
// full syncs and updates alike, not the time the serializer ran.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderSamplingTimeTest, "Open3DBroadcast.Sender.Wire.SerializedTimeIsSamplingTime", O3DB_TEST_FLAGS)
bool FO3DSenderSamplingTimeTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSnapshotTests;
	const TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor = MakeTwoBoneDescriptor();

	for (EO3DSenderEncodingMode Mode : { EO3DSenderEncodingMode::Legacy, EO3DSenderEncodingMode::Quantized, EO3DSenderEncodingMode::Residual })
	{
		FO3DSenderSerializer Serializer;
		FSnapshotPacketSink Sink(Serializer);
		O3DS::SubjectList Receiver;

		FO3DSenderEncodingSettings Settings;
		Settings.Mode = Mode;
		Settings.FullSyncIntervalSeconds = 1.0f;

		// Exactly representable times, far from the process clock, with a full sync at 1000.25
		// and 1001.25 and updates in between (persistent encodings).
		for (int32 Index = 0; Index < 6; ++Index)
		{
			FO3DSPoseFrame Frame;
			Frame.Subject = TEXT("Clock");
			Frame.Descriptor = Descriptor;
			Frame.CaptureTimeSec = 1000.25 + 0.25 * Index;
			Frame.Encoding = Settings;
			FillBones(Frame, 0.25 * Index);
			Serializer.SerializePoseFrame(Frame.Subject, Frame);

			const FString Context = FString::Printf(TEXT("mode %d frame %d"), (int32)Mode, Index);
			if (!TestEqual(*FString::Printf(TEXT("%s: one packet"), *Context), Sink.Packets.Num(), Index + 1)
				|| !TestTrue(*FString::Printf(TEXT("%s: packet parses"), *Context), ParsePacket(Receiver, Sink.Packets.Last())))
			{
				return false;
			}
			TestEqual(*FString::Printf(TEXT("%s: OnSerializedFrame timestamp"), *Context), Sink.Timestamps.Last(), Frame.CaptureTimeSec);
			TestEqual(*FString::Printf(TEXT("%s: wire time"), *Context), Receiver.mTime, Frame.CaptureTimeSec);
		}
		if (Mode != EO3DSenderEncodingMode::Legacy)
		{
			TestEqual(*FString::Printf(TEXT("mode %d: full syncs at 1000.25 and 1001.25 only"), (int32)Mode), Serializer.GetSubjectStats(TEXT("Clock")).FullSyncsSent, (uint64)2);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
