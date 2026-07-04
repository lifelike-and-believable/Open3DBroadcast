// Core serialization/parse round-trip tests for O3DS::SubjectList.
//
// This suite replaces the two root-level, never-built test files that
// predated it (test_curves.cpp, test_curve_comprehensive.cpp) - their
// coverage is folded in here, now actually wired into CTest.
#include "test_framework.h"

#include "o3ds/model.h"

using namespace O3DS;

namespace
{
	SubjectList RoundTrip(const SubjectList& original)
	{
		std::vector<char> buffer;
		int size = const_cast<SubjectList&>(original).Serialize(buffer);
		O3DS_CHECK(size > 0);

		SubjectList parsed;
		bool ok = parsed.Parse(buffer.data(), buffer.size(), /*builder*/ nullptr, /*clearInactive*/ true);
		O3DS_CHECK(ok);

		return parsed;
	}
}

O3DS_TEST(BasicCurveRoundTrip)
{
	SubjectList subjects;
	auto* subject = subjects.addSubject("FaceCharacter");
	subject->mCurveNames = { "EyeBrowUp_L", "EyeBrowUp_R", "Smile" };
	subject->mCurveValues = { 0.5f, 0.3f, 0.8f };

	auto* root = subject->addTransform("Root", -1);
	root->translation.value.v[0] = 1.0;
	root->translation.value.v[1] = 2.0;
	root->translation.value.v[2] = 3.0;
	// The sender always writes the raw translation/rotation/scale values onto
	// the wire, but ParseSubject() on the receiving end only *applies* a field
	// to the parsed Transform if its Component type is listed here - this is
	// what lets a transform update carry just e.g. rotation. Omitting it is a
	// silent no-op on the receiver, not a serialization error.
	root->transformOrder.push_back(O3DS::TTranslation);

	SubjectList parsed = RoundTrip(subjects);

	Subject* parsedSubject = parsed.findSubject("FaceCharacter");
	O3DS_CHECK(parsedSubject != nullptr);
	O3DS_CHECK_EQ(parsedSubject->mCurveNames.size(), (size_t)3);
	O3DS_CHECK_EQ(parsedSubject->mCurveNames[0], std::string("EyeBrowUp_L"));
	O3DS_CHECK_EQ(parsedSubject->mCurveNames[2], std::string("Smile"));
	O3DS_CHECK_EQ(parsedSubject->mCurveValues[2], 0.8f);

	O3DS_CHECK_EQ(parsedSubject->mTransforms.size(), (size_t)1);
	O3DS_CHECK_EQ(parsedSubject->mTransforms[0]->translation.value.v[0], 1.0);
	O3DS_CHECK_EQ(parsedSubject->mTransforms[0]->translation.value.v[2], 3.0);
}

O3DS_TEST(TransformHierarchyRoundTrip)
{
	SubjectList subjects;
	auto* subject = subjects.addSubject("Skeleton");

	subject->addTransform("Root", -1);
	subject->addTransform("Spine", 0);
	subject->addTransform("Head", 1);
	subject->addTransform("LeftArm", 1);

	SubjectList parsed = RoundTrip(subjects);

	Subject* parsedSubject = parsed.findSubject("Skeleton");
	O3DS_CHECK(parsedSubject != nullptr);
	O3DS_CHECK_EQ(parsedSubject->mTransforms.size(), (size_t)4);

	// Names and parent-child relationships must survive the wire round trip
	// unchanged - this is the structure downstream retargeting depends on.
	O3DS_CHECK_EQ(parsedSubject->mTransforms[0]->mName, std::string("Root"));
	O3DS_CHECK_EQ(parsedSubject->mTransforms[0]->mParentId, -1);
	O3DS_CHECK_EQ(parsedSubject->mTransforms[1]->mName, std::string("Spine"));
	O3DS_CHECK_EQ(parsedSubject->mTransforms[1]->mParentId, 0);
	O3DS_CHECK_EQ(parsedSubject->mTransforms[2]->mName, std::string("Head"));
	O3DS_CHECK_EQ(parsedSubject->mTransforms[2]->mParentId, 1);
	O3DS_CHECK_EQ(parsedSubject->mTransforms[3]->mName, std::string("LeftArm"));
	O3DS_CHECK_EQ(parsedSubject->mTransforms[3]->mParentId, 1);
}

O3DS_TEST(MultipleSubjectsRoundTrip)
{
	SubjectList subjects;

	auto* a = subjects.addSubject("Performer1");
	a->addTransform("Root", -1);
	a->mCurveNames = { "Smile" };
	a->mCurveValues = { 1.0f };

	auto* b = subjects.addSubject("Performer2");
	b->addTransform("Root", -1);
	b->mCurveNames = { "Frown" };
	b->mCurveValues = { 0.25f };

	SubjectList parsed = RoundTrip(subjects);

	O3DS_CHECK(parsed.findSubject("Performer1") != nullptr);
	O3DS_CHECK(parsed.findSubject("Performer2") != nullptr);
	O3DS_CHECK_EQ(parsed.findSubject("Performer1")->mCurveValues[0], 1.0f);
	O3DS_CHECK_EQ(parsed.findSubject("Performer2")->mCurveValues[0], 0.25f);
}
