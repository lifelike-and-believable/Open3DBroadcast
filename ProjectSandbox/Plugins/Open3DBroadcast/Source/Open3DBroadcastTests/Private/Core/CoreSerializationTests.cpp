// Copyright (c) Open3DStream Contributors
//
// Open3DBroadcast.Core.*: the core library compiled into the editor serializes and parses back
// (ADR 0006 §7: a UE build of the core, not a re-test of its arithmetic, which CTest covers).
// Formerly Open3DShared/Private/Tests/GenericTransportTests.cpp. Its three transport placeholders
// that asserted TestTrue(..., true) (SHR-5) are replaced by the conformance suite
// (Conformance/O3DConformanceSuite.cpp); the serialization checks below now parse their output.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "o3ds/model.h"

#include <string>
#include <vector>

namespace O3DCoreSerializationTests
{
	bool Parse(O3DS::SubjectList& Out, const std::vector<char>& Buffer)
	{
		return Out.Parse(Buffer.data(), Buffer.size(), nullptr, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DCoreLargePayloadTest, "Open3DBroadcast.Core.Serialization.LargePayloadRoundTrip", O3DB_TEST_FLAGS)
bool FO3DCoreLargePayloadTest::RunTest(const FString& Parameters)
{
	// 10 subjects x 100 bones plus six curves each.
	O3DS::SubjectList List;
	for (int32 SubjectIdx = 0; SubjectIdx < 10; ++SubjectIdx)
	{
		const FTCHARToUTF8 SubjectUtf8(*FString::Printf(TEXT("LargeSubject_%d"), SubjectIdx));
		O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
		for (int32 BoneIdx = 0; BoneIdx < 100; ++BoneIdx)
		{
			const FTCHARToUTF8 BoneUtf8(*FString::Printf(TEXT("Bone_%d"), BoneIdx));
			O3DS::Transform* Transform = Subject->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), BoneIdx == 0 ? -1 : 0);
			Transform->transformOrder.push_back(O3DS::TTranslation); // only listed components go on the wire
			Transform->translation.value = O3DS::Vector3d(BoneIdx, BoneIdx * 2.0, BoneIdx * 3.0);
		}
		Subject->mCurveNames = { "Smile", "Frown", "EyeBrowUp_L", "EyeBrowUp_R", "EyeWide_L", "EyeWide_R" };
		Subject->mCurveValues = { 0.5f, 0.3f, 0.7f, 0.8f, 0.4f, 0.6f };
	}

	std::vector<char> Buffer;
	List.Serialize(Buffer, 1.0);
	TestTrue(TEXT("Serialized payload is larger than 10 KiB"), Buffer.size() > 10240);

	O3DS::SubjectList Parsed;
	if (!TestTrue(TEXT("Payload parses"), O3DCoreSerializationTests::Parse(Parsed, Buffer)))
	{
		return false;
	}
	TestEqual(TEXT("Ten subjects"), static_cast<int32>(Parsed.mItems.size()), 10);
	O3DS::Subject* Last = Parsed.findSubject("LargeSubject_9");
	if (TestNotNull(TEXT("Last subject present"), Last))
	{
		TestEqual(TEXT("100 bones"), static_cast<int32>(Last->mTransforms.size()), 100);
		TestEqual(TEXT("Six curves"), static_cast<int32>(Last->mCurveNames.size()), 6);
		TestEqual(TEXT("Bone 42 translation Z"), Last->mTransforms[42]->translation.value.v[2], 126.0, 1.0e-4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DCoreMultiSubjectTest, "Open3DBroadcast.Core.Serialization.MultipleSubjectsRoundTrip", O3DB_TEST_FLAGS)
bool FO3DCoreMultiSubjectTest::RunTest(const FString& Parameters)
{
	O3DS::SubjectList List;

	O3DS::Subject* Alice = List.addSubject("Alice");
	Alice->addTransform("Root", -1);
	Alice->addTransform("Spine", 0);
	Alice->addTransform("Head", 1);

	O3DS::Subject* Bob = List.addSubject("Bob");
	Bob->addTransform("Root", -1);
	Bob->addTransform("LeftArm", 0);
	Bob->addTransform("RightArm", 0);
	Bob->addTransform("LeftLeg", 0);
	Bob->addTransform("RightLeg", 0);

	O3DS::Subject* Charlie = List.addSubject("Charlie");
	Charlie->addTransform("Root", -1);

	O3DS::Subject* Diana = List.addSubject("Diana");
	for (int32 Index = 0; Index < 20; ++Index)
	{
		const FTCHARToUTF8 BoneUtf8(*FString::Printf(TEXT("Bone_%d"), Index));
		Diana->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), Index == 0 ? -1 : 0);
	}

	O3DS::Subject* Eve = List.addSubject("Eve");
	Eve->addTransform("Root", -1);
	Eve->addTransform("Body", 0);
	Eve->mCurveNames = { "Smile", "Frown" };
	Eve->mCurveValues = { 0.8f, 0.2f };

	std::vector<char> Buffer;
	List.Serialize(Buffer, 2.0);

	O3DS::SubjectList Parsed;
	if (!TestTrue(TEXT("Payload parses"), O3DCoreSerializationTests::Parse(Parsed, Buffer)))
	{
		return false;
	}
	TestEqual(TEXT("Five subjects"), static_cast<int32>(Parsed.mItems.size()), 5);

	const struct { const char* Name; int32 Bones; } Expected[] = { { "Alice", 3 }, { "Bob", 5 }, { "Charlie", 1 }, { "Diana", 20 }, { "Eve", 2 } };
	for (const auto& Entry : Expected)
	{
		O3DS::Subject* Subject = Parsed.findSubject(Entry.Name);
		if (TestNotNull(*FString::Printf(TEXT("%s present"), UTF8_TO_TCHAR(Entry.Name)), Subject))
		{
			TestEqual(*FString::Printf(TEXT("%s bone count"), UTF8_TO_TCHAR(Entry.Name)), static_cast<int32>(Subject->mTransforms.size()), Entry.Bones);
		}
	}

	O3DS::Subject* ParsedEve = Parsed.findSubject("Eve");
	if (ParsedEve && TestEqual(TEXT("Eve keeps two curves"), static_cast<int32>(ParsedEve->mCurveNames.size()), 2))
	{
		TestEqual(TEXT("Eve curve 0 name"), FString(UTF8_TO_TCHAR(ParsedEve->mCurveNames[0].c_str())), FString(TEXT("Smile")));
		TestEqual(TEXT("Eve curve 0 value"), ParsedEve->mCurveValues[0], 0.8f, 1.0e-6f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DCoreEmptySubjectListTest, "Open3DBroadcast.Core.Serialization.EmptySubjectList", O3DB_TEST_FLAGS)
bool FO3DCoreEmptySubjectListTest::RunTest(const FString& Parameters)
{
	O3DS::SubjectList EmptyList;
	std::vector<char> Buffer;
	EmptyList.Serialize(Buffer, 3.0);
	TestTrue(TEXT("Empty list serializes to a header"), Buffer.size() > 0);

	O3DS::SubjectList Parsed;
	TestTrue(TEXT("Empty list parses"), O3DCoreSerializationTests::Parse(Parsed, Buffer));
	TestEqual(TEXT("No subjects"), static_cast<int32>(Parsed.mItems.size()), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
