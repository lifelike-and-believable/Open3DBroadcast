// Copyright Lifelike & Believable. All Rights Reserved.

// The legacy encoding keeps its core Subject across frames and copies only the values in (WP-A2
// follow-up, ADR 0008 addendum "Insights numbers"). The wire must not change: after every frame of
// a sequence that hits each reuse branch, the serializer's bytes must equal those of a Subject
// built from scratch for that frame, exactly as the serializer did before (new SubjectList,
// transforms from the descriptor, values, curves, CalcMatrices, Serialize).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

namespace O3DSenderLegacyReuseTests
{
	TSharedPtr<const FO3DSSkeletonDescriptor> MakeReuseDescriptor(const TArray<FString>& Names, const TArray<int32>& Parents)
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		for (const FString& Name : Names)
		{
			Descriptor->BoneNames.Add(FName(*Name));
		}
		Descriptor->ParentIndices = Parents;
		Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
		return Descriptor;
	}

	FO3DSPoseFrame MakeReuseFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, double Time,
		double Offset, const TArray<FName>& CurveNames)
	{
		FO3DSPoseFrame Frame;
		Frame.Subject = Subject;
		Frame.Descriptor = Descriptor;
		Frame.CaptureTimeSec = Time;
		Frame.Encoding.Mode = EO3DSenderEncodingMode::Legacy;
		for (int32 Index = 0; Index < Descriptor->BoneNames.Num(); ++Index)
		{
			const FQuat Rotation(FVector(0.0, 0.0, 1.0), 0.1 * Index + Offset);
			Frame.BoneLocalTransforms.Add(FTransform(Rotation, FVector(Index + Offset, 2.0 * Offset, -Index), FVector(1.0, 1.0 + 0.01 * Offset, 1.0)));
		}
		Frame.CurveNames = CurveNames;
		for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
		{
			Frame.CurveValues.Add(static_cast<float>(0.1 * Index + Offset));
		}
		return Frame;
	}

	/** What SerializeFrameLegacy produced before the reuse change, rebuilt here step by step. */
	TArray<uint8> BuildFromScratch(const FString& Subject, const FO3DSPoseFrame& Frame)
	{
		O3DS::SubjectList List;
		O3DS::Subject* SubjectObject = List.addSubject(std::string(TCHAR_TO_UTF8(*Subject)));
		const FO3DSSkeletonDescriptor& Descriptor = *Frame.Descriptor;
		for (int32 Index = 0; Index < Descriptor.BoneNames.Num(); ++Index)
		{
			O3DS::Transform* Transform = SubjectObject->addTransform(std::string(TCHAR_TO_UTF8(*Descriptor.BoneNames[Index].ToString())), Descriptor.ParentIndices[Index]);
			const FTransform& Rel = Frame.BoneLocalTransforms[Index];
			const FVector T = Rel.GetTranslation();
			const FQuat R = Rel.GetRotation();
			const FVector S = Rel.GetScale3D();
			Transform->translation.value = O3DS::Vector3d(T.X, T.Y, T.Z);
			Transform->rotation.value = O3DS::Vector4d(R.X, R.Y, R.Z, R.W);
			Transform->scale.value = O3DS::Vector3d(S.X, S.Y, S.Z);
			Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
		}
		for (int32 Index = 0; Index < Frame.CurveNames.Num(); ++Index)
		{
			SubjectObject->mCurveNames.push_back(std::string(TCHAR_TO_UTF8(*Frame.CurveNames[Index].ToString())));
			SubjectObject->mCurveValues.push_back(Frame.CurveValues[Index]);
		}
		SubjectObject->CalcMatrices();
		std::vector<char> Buffer;
		List.Serialize(Buffer, Frame.CaptureTimeSec);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Buffer.data()), static_cast<int32>(Buffer.size()));
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderLegacyReuseTest, "Open3DBroadcast.Sender.Wire.LegacyReuseKeepsBytes", O3DB_TEST_FLAGS)
bool FO3DSenderLegacyReuseTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderLegacyReuseTests;

	const TArray<FString> ThreeNames = { TEXT("root"), TEXT("spine"), TEXT("head") };
	const TArray<int32> ThreeParents = { -1, 0, 1 };
	const TSharedPtr<const FO3DSSkeletonDescriptor> Three = MakeReuseDescriptor(ThreeNames, ThreeParents);
	const TSharedPtr<const FO3DSSkeletonDescriptor> ThreeAgain = MakeReuseDescriptor(ThreeNames, ThreeParents);
	const TSharedPtr<const FO3DSSkeletonDescriptor> Four = MakeReuseDescriptor({ TEXT("root"), TEXT("spine"), TEXT("neck"), TEXT("head") }, { -1, 0, 1, 2 });
	const TSharedPtr<const FO3DSSkeletonDescriptor> FourRenamed = MakeReuseDescriptor({ TEXT("Root"), TEXT("spine"), TEXT("neck"), TEXT("head") }, { -1, 0, 1, 2 });
	const TSharedPtr<const FO3DSSkeletonDescriptor> FourReparented = MakeReuseDescriptor({ TEXT("Root"), TEXT("spine"), TEXT("neck"), TEXT("head") }, { -1, 0, 0, 2 });

	const TArray<FName> TwoCurves = { FName(TEXT("brow_l")), FName(TEXT("brow_r")) };
	const TArray<FName> TwoCurvesCase = { FName(TEXT("Brow_L")), FName(TEXT("brow_r")) };
	const TArray<FName> ThreeCurves = { FName(TEXT("jaw")), FName(TEXT("brow_l")), FName(TEXT("brow_r")) };
	const TArray<FName> NoCurves;

	struct FStep
	{
		const TCHAR* What;
		FString Subject;
		TSharedPtr<const FO3DSSkeletonDescriptor> Descriptor;
		TArray<FName> Curves;
	};
	const FString Hero = TEXT("Hero");
	const FString Villain = TEXT("Villain");
	const TArray<FStep> Steps = {
		{ TEXT("first frame"), Hero, Three, TwoCurves },
		{ TEXT("values move"), Hero, Three, TwoCurves },
		{ TEXT("values move again"), Hero, Three, TwoCurves },
		{ TEXT("curve names differ only in case"), Hero, Three, TwoCurvesCase },
		{ TEXT("curves removed"), Hero, Three, NoCurves },
		{ TEXT("curves back, three of them"), Hero, Three, ThreeCurves },
		{ TEXT("equal descriptor in a new snapshot"), Hero, ThreeAgain, ThreeCurves },
		{ TEXT("another subject in between"), Villain, Four, TwoCurves },
		{ TEXT("skeleton grows"), Hero, Four, ThreeCurves },
		{ TEXT("bone name differs only in case"), Hero, FourRenamed, ThreeCurves },
		{ TEXT("a parent changes"), Hero, FourReparented, ThreeCurves },
		{ TEXT("back to the first skeleton"), Hero, Three, TwoCurves },
		{ TEXT("the other subject again"), Villain, Four, TwoCurves },
	};

	FO3DSenderSerializer Serializer;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		const FStep& Step = Steps[Index];
		const FO3DSPoseFrame Frame = MakeReuseFrame(Step.Subject, Step.Descriptor, 10.0 + Index / 60.0, 0.5 * Index, Step.Curves);

		TArray<uint8> Bytes;
		bool bFullSync = false;
		const bool bSerialized = Serializer.SerializePoseFrameTo(Step.Subject, Frame, Bytes, bFullSync);
		TestTrue(FString::Printf(TEXT("%s: serialized"), Step.What), bSerialized);
		TestTrue(FString::Printf(TEXT("%s: a legacy frame is a full sync"), Step.What), bFullSync);

		const TArray<uint8> Expected = BuildFromScratch(Step.Subject, Frame);
		TestEqual(FString::Printf(TEXT("%s: same size as a Subject built for the frame"), Step.What), Bytes.Num(), Expected.Num());
		TestTrue(FString::Printf(TEXT("%s: same bytes as a Subject built for the frame"), Step.What), Bytes == Expected);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
