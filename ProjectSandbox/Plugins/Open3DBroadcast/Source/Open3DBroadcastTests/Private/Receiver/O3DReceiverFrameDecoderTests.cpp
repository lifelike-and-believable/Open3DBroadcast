// Copyright Lifelike & Believable. All Rights Reserved.

// FO3DReceiverFrameDecoder (WP-A3, RCV-29): converts parsed O3DS subjects for LiveLink and caches
// what only changes with the topology (RCV-4, RCV-11, RCV-12). Reached through the exported
// FO3DReceiverFrameDecoderProbe; buffers come from the core's own serializer.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DPerformanceMetrics.h"
#include "Testing/O3DReceiverTesting.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <limits>
#include <string>
#include <vector>

namespace O3DReceiverFrameDecoderTests
{
	struct FDecoderBone
	{
		std::string Name;
		int Parent;
		O3DS::Vector3d Translation;
		O3DS::Vector4d Rotation;
	};

	TArray<uint8> MakeDecoderBuffer(const std::string& Subject, const std::vector<FDecoderBone>& Bones,
		const std::vector<std::string>& CurveNames, const std::vector<float>& CurveValues)
	{
		O3DS::SubjectList List;
		O3DS::Subject* SubjectObject = List.addSubject(Subject);
		for (const FDecoderBone& Bone : Bones)
		{
			O3DS::Transform* Transform = SubjectObject->addTransform(Bone.Name, Bone.Parent);
			Transform->translation.value = Bone.Translation;
			Transform->rotation.value = Bone.Rotation;
			Transform->scale.value = O3DS::Vector3d(1.0, 1.0, 1.0);
			Transform->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
		}
		SubjectObject->mCurveNames = CurveNames;
		SubjectObject->mCurveValues = CurveValues;
		std::vector<char> Buffer;
		List.Serialize(Buffer, 1.0);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Buffer.data()), static_cast<int32>(Buffer.size()));
		return Bytes;
	}

	std::vector<FDecoderBone> TwoBones(double X)
	{
		return {
			{ "rig:root", -1, O3DS::Vector3d(X, 2.0, 3.0), O3DS::Vector4d(0.0, 0.0, 0.0, 2.0) },
			{ "rig:arm", 0, O3DS::Vector3d(0.0, X, 0.0), O3DS::Vector4d(0.0, 0.0, 0.0, 1.0) },
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFrameDecoderConvertTest, "Open3DBroadcast.Receiver.FrameDecoder.ConvertsPoseAndCurves", O3DB_TEST_FLAGS)
bool FO3DReceiverFrameDecoderConvertTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverFrameDecoderTests;

	FO3DReceiverFrameDecoderProbe Probe;
	if (!TestTrue(TEXT("Decodes"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(1.0), { "brow", "jaw" }, { 0.25f, 0.75f }), true)))
	{
		return false;
	}

	TestEqual(TEXT("Subject name"), Probe.SubjectName, FName(TEXT("Hero")));
	TestEqual(TEXT("Two bones"), Probe.BoneNames.Num(), 2);
	TestTrue(TEXT("Namespace prefixes are dropped"), Probe.BoneNames.Num() == 2 && Probe.BoneNames[0] == FName(TEXT("root")) && Probe.BoneNames[1] == FName(TEXT("arm")));
	TestTrue(TEXT("Parents"), Probe.BoneParents == TArray<int32>({ -1, 0 }));
	TestTrue(TEXT("Translation"), Probe.BoneTransforms.Num() == 2 && Probe.BoneTransforms[0].GetTranslation().Equals(FVector(1.0, 2.0, 3.0)));
	TestTrue(TEXT("Rotation is normalized"), Probe.BoneTransforms.Num() == 2 && Probe.BoneTransforms[0].GetRotation().Equals(FQuat::Identity));
	TestTrue(TEXT("Curve names"), Probe.CurveNames == TArray<FName>({ FName(TEXT("brow")), FName(TEXT("jaw")) }));
	TestTrue(TEXT("Curve values"), Probe.CurveValues == TArray<float>({ 0.25f, 0.75f }));
	TestEqual(TEXT("Skeleton hash is the names-and-parents hash"), Probe.SkeletonHash, O3DHelpers::HashNamesAndParents(Probe.BoneNames, Probe.BoneParents));
	TestEqual(TEXT("Curve hash is the names hash"), Probe.CurveHash, O3DHelpers::HashNames(Probe.CurveNames));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFrameDecoderCacheTest, "Open3DBroadcast.Receiver.FrameDecoder.ReusesTopologyUntilItChanges", O3DB_TEST_FLAGS)
bool FO3DReceiverFrameDecoderCacheTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverFrameDecoderTests;

	FO3DReceiverFrameDecoderProbe Probe;
	TestTrue(TEXT("First frame decodes"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(1.0), { "brow" }, { 0.1f }), true));
	TestEqual(TEXT("First frame builds the bone names"), Probe.GetSkeletonBuilds(), 1ull);
	TestEqual(TEXT("And the curve names"), Probe.GetCurveNameBuilds(), 1ull);
	const uint64 FirstHash = Probe.SkeletonHash;

	// Same topology, new values, no full descriptor: names are reused, values are new.
	TestTrue(TEXT("Second frame decodes"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(5.0), { "brow" }, { 0.9f }), false));
	TestEqual(TEXT("Bone names reused"), Probe.GetSkeletonBuilds(), 1ull);
	TestEqual(TEXT("Curve names reused"), Probe.GetCurveNameBuilds(), 1ull);
	TestTrue(TEXT("New translation"), Probe.BoneTransforms.Num() == 2 && Probe.BoneTransforms[0].GetTranslation().X == 5.0);
	TestTrue(TEXT("New curve value"), Probe.CurveValues == TArray<float>({ 0.9f }));
	TestEqual(TEXT("Same skeleton hash"), Probe.SkeletonHash, FirstHash);

	// A full descriptor always rebuilds the bone names (RCV-4).
	TestTrue(TEXT("Full descriptor decodes"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(6.0), { "brow" }, { 0.9f }), true));
	TestEqual(TEXT("A full descriptor rebuilds the names"), Probe.GetSkeletonBuilds(), 2ull);

	// A renamed bone changes the fingerprint even without a full descriptor.
	std::vector<FDecoderBone> Renamed = TwoBones(6.0);
	Renamed[1].Name = "rig:hand";
	TestTrue(TEXT("Renamed bone decodes"), Probe.Decode(MakeDecoderBuffer("Hero", Renamed, { "brow" }, { 0.9f }), false));
	TestEqual(TEXT("A renamed bone rebuilds the names"), Probe.GetSkeletonBuilds(), 3ull);
	TestTrue(TEXT("With the new name"), Probe.BoneNames.Num() == 2 && Probe.BoneNames[1] == FName(TEXT("hand")));
	TestEqual(TEXT("And a new skeleton hash"), Probe.SkeletonHash, O3DHelpers::HashNamesAndParents(Probe.BoneNames, Probe.BoneParents));

	// New curve names rebuild only the curve names.
	TestTrue(TEXT("New curves decode"), Probe.Decode(MakeDecoderBuffer("Hero", Renamed, { "brow", "smile" }, { 0.2f, 0.3f }), false));
	TestEqual(TEXT("Curve names rebuilt"), Probe.GetCurveNameBuilds(), 2ull);
	TestEqual(TEXT("Bone names not"), Probe.GetSkeletonBuilds(), 3ull);
	TestTrue(TEXT("New curve names"), Probe.CurveNames == TArray<FName>({ FName(TEXT("brow")), FName(TEXT("smile")) }));
	TestEqual(TEXT("New curve hash"), Probe.CurveHash, O3DHelpers::HashNames(Probe.CurveNames));

	// A forgotten subject starts over.
	Probe.ForgetSubject(FName(TEXT("Hero")));
	TestTrue(TEXT("Decodes after ForgetSubject"), Probe.Decode(MakeDecoderBuffer("Hero", Renamed, { "brow", "smile" }, { 0.2f, 0.3f }), false));
	TestEqual(TEXT("Forgetting rebuilds the bone names"), Probe.GetSkeletonBuilds(), 4ull);
	TestEqual(TEXT("And the curve names"), Probe.GetCurveNameBuilds(), 3ull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFrameDecoderRejectTest, "Open3DBroadcast.Receiver.FrameDecoder.RejectsUnusablePoses", O3DB_TEST_FLAGS)
bool FO3DReceiverFrameDecoderRejectTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverFrameDecoderTests;

	FO3DReceiverFrameDecoderProbe Probe;
	std::vector<FDecoderBone> ZeroRotation = TwoBones(1.0);
	ZeroRotation[1].Rotation = O3DS::Vector4d(0.0, 0.0, 0.0, 0.0);
	TestFalse(TEXT("A zero rotation makes the pose unusable"), Probe.Decode(MakeDecoderBuffer("Hero", ZeroRotation, {}, {}), true));
	TestEqual(TEXT("Nothing was cached for it"), Probe.GetSkeletonBuilds(), 0ull);

	TestFalse(TEXT("A subject without transforms has no pose"), Probe.Decode(MakeDecoderBuffer("Empty", {}, { "brow" }, { 1.0f }), true));

	TestTrue(TEXT("A good frame still decodes afterwards"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(1.0), {}, {}), true));
	TestEqual(TEXT("No curves"), Probe.CurveNames.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFrameDecoderPrecisionTest, "Open3DBroadcast.Receiver.FrameDecoder.KeepsDoublePrecision", O3DB_TEST_FLAGS)
bool FO3DReceiverFrameDecoderPrecisionTest::RunTest(const FString& Parameters)
{
	// RCV-13: the core holds doubles (a quantized translation is anchor plus delta, in double);
	// the decoder used to cast each component to float. Values a float cannot hold come through.
	// None of these fits float's 24-bit mantissa (the nearest floats are 123456.7890625, 1.0 and 1.0).
	const double FarX = 123456.789012345;
	const double FineY = 1.0 + 1.0e-12;
	const double ScaleZ = 1.0000000001;

	O3DS::SubjectList List;
	O3DS::Subject* Subject = List.addSubject("Hero");
	O3DS::Transform* Root = Subject->addTransform("root", -1);
	Root->translation.value = O3DS::Vector3d(FarX, FineY, -FarX);
	Root->rotation.value = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
	Root->scale.value = O3DS::Vector3d(1.0, 1.0, ScaleZ);

	FO3DReceiverFrameDecoderProbe Probe;
	if (!TestTrue(TEXT("Decodes"), Probe.DecodeSubject(*Subject, true)) || !TestEqual(TEXT("One bone"), Probe.BoneTransforms.Num(), 1))
	{
		return false;
	}
	const FVector Translation = Probe.BoneTransforms[0].GetTranslation();
	TestTrue(TEXT("Translation X keeps its double value"), Translation.X == FarX);
	TestTrue(TEXT("Translation Y keeps its double value"), Translation.Y == FineY);
	TestTrue(TEXT("Translation Z keeps its double value"), Translation.Z == -FarX);
	TestTrue(TEXT("Scale keeps its double value"), Probe.BoneTransforms[0].GetScale3D().Z == ScaleZ);

	// A rotation is normalized in double: a unit quaternion off float's grid stays unit length.
	const double Half = FMath::Sqrt(0.5);
	Root->rotation.value = O3DS::Vector4d(Half, 0.0, 0.0, Half);
	TestTrue(TEXT("Decodes a rotation"), Probe.DecodeSubject(*Subject, false));
	TestTrue(TEXT("Normalized rotation"), Probe.BoneTransforms.Num() == 1 && FMath::IsNearlyEqual(Probe.BoneTransforms[0].GetRotation().Size(), 1.0, 1.0e-12));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFrameDecoderDroppedPoseMetricTest, "Open3DBroadcast.Receiver.FrameDecoder.CountsDroppedPoses", O3DB_TEST_FLAGS)
bool FO3DReceiverFrameDecoderDroppedPoseMetricTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverFrameDecoderTests;

	// RCV-13: a pose dropped for an unusable transform used to leave no trace.
	const auto Dropped = []() { return FO3DPerformanceMetrics::Get().GetReceiverMetrics().InvalidPosesDropped.load(); };
	FO3DReceiverFrameDecoderProbe Probe;

	uint64 Before = Dropped();
	std::vector<FDecoderBone> ZeroRotation = TwoBones(1.0);
	ZeroRotation[1].Rotation = O3DS::Vector4d(0.0, 0.0, 0.0, 0.0);
	TestFalse(TEXT("A zero rotation is rejected"), Probe.Decode(MakeDecoderBuffer("Hero", ZeroRotation, {}, {}), true));
	TestEqual(TEXT("And counted"), Dropped() - Before, 1ull);

	O3DS::SubjectList List;
	O3DS::Subject* Subject = List.addSubject("Hero");
	O3DS::Transform* Root = Subject->addTransform("root", -1);
	Root->rotation.value = O3DS::Vector4d(0.0, 0.0, 0.0, 1.0);
	Root->scale.value = O3DS::Vector3d(1.0, 1.0, 1.0);
	Root->translation.value = O3DS::Vector3d(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
	Before = Dropped();
	TestFalse(TEXT("A NaN translation is rejected"), Probe.DecodeSubject(*Subject, true));
	TestEqual(TEXT("And counted"), Dropped() - Before, 1ull);

	Root->translation.value = O3DS::Vector3d(0.0, 0.0, 0.0);
	Root->scale.value = O3DS::Vector3d(std::numeric_limits<double>::infinity(), 1.0, 1.0);
	Before = Dropped();
	TestFalse(TEXT("An infinite scale is rejected"), Probe.DecodeSubject(*Subject, true));
	TestEqual(TEXT("And counted"), Dropped() - Before, 1ull);

	// A subject without transforms (curves only) has no pose to drop: not counted.
	Before = Dropped();
	TestFalse(TEXT("A subject without transforms has no pose"), Probe.Decode(MakeDecoderBuffer("Empty", {}, { "brow" }, { 1.0f }), true));
	TestEqual(TEXT("Not counted"), Dropped() - Before, 0ull);

	Before = Dropped();
	TestTrue(TEXT("A good frame decodes"), Probe.Decode(MakeDecoderBuffer("Hero", TwoBones(1.0), {}, {}), true));
	TestEqual(TEXT("Not counted"), Dropped() - Before, 0ull);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
