// Regression tests for WP-S1 (core parser hardening), see
// docs/roadmap/plugin-hardening-and-fab-readiness.md §5 and the findings in
// docs/review/2026-09-plugin-review/core-library.md:
//   CORE-1  matrix components without matching matrices (heap OOB read)
//   CORE-8  O(N^2) hierarchy solve, parent cycles, dead NaN check, limits
//   CORE-9  non-finite floats from the wire
//   CORE-23 ParseUpdate index validation
//   CORE-26 ClockOffsetEstimator signed overflow on hostile timestamps
//   CORE-17 SubjectList copy assignment double-delete
//
// Most cases build hostile buffers directly with the FlatBuffers builder,
// because the high-level Serialize() API cannot express them. Every such
// buffer carries a valid CRC and passes the FlatBuffers Verifier: that is the
// point, the semantic checks under test sit behind both.
#include "test_framework.h"

#include "o3ds/clock_offset.h"
#include "o3ds/model.h"
#include "o3ds/parse_limits.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace O3DS;

namespace
{
	const float kNaN = std::numeric_limits<float>::quiet_NaN();
	const float kInf = std::numeric_limits<float>::infinity();

	using NodeOffsets = std::vector<flatbuffers::Offset<O3DS::Data::Transform>>;
	using SubjectOffsets = std::vector<flatbuffers::Offset<O3DS::Data::Subject>>;
	using UpdateOffsets = std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>>;

	std::vector<char> Finish(flatbuffers::FlatBufferBuilder& b,
		const SubjectOffsets* subjects, const UpdateOffsets* updates, double time = 1.0)
	{
		b.Finish(O3DS::Data::CreateSubjectListDirect(b, subjects, updates, time));
		std::vector<char> out;
		finalize(b, out, 1);
		return out;
	}

	// A one-subject buffer whose nodes are all plain translation-only
	// transforms with the given parent ids.
	std::vector<char> BuildHierarchy(const std::vector<int>& parents)
	{
		flatbuffers::FlatBufferBuilder b;
		std::vector<int8_t> comps{ O3DS::Data::Component_Translation };
		O3DS::Data::Translation t(0.0f, 1.0f, 0.0f);
		NodeOffsets nodes;
		for (size_t i = 0; i < parents.size(); i++)
		{
			const std::string name = "n" + std::to_string(i);
			nodes.push_back(O3DS::Data::CreateTransformDirect(b, parents[i], name.c_str(), &t, nullptr, nullptr, nullptr, &comps));
		}
		SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "s") };
		return Finish(b, &subjects, nullptr);
	}

	bool ParseBuffer(SubjectList& list, const std::vector<char>& buffer, bool clearInactive = true)
	{
		return list.Parse(buffer.data(), buffer.size(), nullptr, clearInactive);
	}

	// A well-formed three-transform subject ("root" -> "a" -> "b") with two
	// curves, serialized through the normal API.
	std::vector<char> BuildWellFormedFull(SubjectList& sender)
	{
		auto* s = sender.addSubject("rig");
		auto* root = s->addTransform("root", -1);
		auto* a = s->addTransform("a", 0);
		auto* b = s->addTransform("b", 1);
		for (Transform* t : { root, a, b })
		{
			t->transformOrder = { TTranslation, TRotation, TScale };
			t->rotation.value = Vector4d(0.0, 0.0, 0.0, 1.0);
			t->scale.value = Vector3d(1.0, 1.0, 1.0);
		}
		root->translation.value = Vector3d(1.0, 0.0, 0.0);
		a->translation.value = Vector3d(0.0, 2.0, 0.0);
		b->translation.value = Vector3d(0.0, 0.0, 3.0);
		s->mCurveNames = { "c0", "c1" };
		s->mCurveValues = { 0.25f, 0.75f };

		std::vector<char> out;
		sender.Serialize(out, 1.0);
		return out;
	}

	// Builds a legacy (predictor_id == 0) update buffer for subject "rig".
	std::vector<char> BuildUpdate(
		const std::vector<O3DS::Data::TranslationUpdate>* translations,
		const std::vector<O3DS::Data::RotationUpdate>* rotations,
		const std::vector<O3DS::Data::ScaleUpdate>* scales,
		const std::vector<O3DS::Data::CurveUpdate>* curves,
		uint32_t predictorId = 0)
	{
		flatbuffers::FlatBufferBuilder b;
		UpdateOffsets updates{ O3DS::Data::CreateSubjectUpdateDirect(b, "rig", translations, rotations, scales, curves, predictorId) };
		return Finish(b, nullptr, &updates, 2.0);
	}
}

// ---------------------------------------------------------------------------
// CORE-1: a Component_Matrix entry with no matching matrix
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_MatrixComponentWithoutMatrices_IsRejected)
{
	// PoC `poc calc`: components=[Matrix], no `matrix` vector. On the
	// unfixed parser, CalcMatrices() reads matrices[0] of an empty vector
	// (ASan SEGV / UBSan null reference).
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps{ O3DS::Data::Component_Matrix };
	NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", nullptr, nullptr, nullptr, nullptr, &comps) };
	SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "evil") };
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

O3DS_TEST(ParserHardening_MoreMatrixComponentsThanMatrices_IsRejected)
{
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps{ O3DS::Data::Component_Matrix, O3DS::Data::Component_Matrix };
	std::vector<O3DS::Data::Matrix> matrices(1, O3DS::Data::Matrix(
		1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1));
	NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", nullptr, nullptr, nullptr, &matrices, &comps) };
	SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "evil") };
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

O3DS_TEST(ParserHardening_CalcMatrices_BoundsChecksMatrixIndex)
{
	// Sender-side (programmatic) path: CalcMatrices() itself must not index
	// past `matrices` either, independently of ParseSubject's check.
	Subject s("direct");
	auto* root = s.addTransform("root", -1);
	root->transformOrder = { TMatrix };
	O3DS_CHECK(!s.CalcMatrices());
	O3DS_CHECK(!s.mError.empty());
}

O3DS_TEST(ParserHardening_MatrixComponentsWithMatrices_StillParse)
{
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps{ O3DS::Data::Component_Matrix };
	std::vector<O3DS::Data::Matrix> matrices(1, O3DS::Data::Matrix(
		1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1));
	NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", nullptr, nullptr, nullptr, &matrices, &comps) };
	SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "ok") };
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(ParseBuffer(list, buffer));
	O3DS_CHECK_EQ(list.mItems[0]->mTransforms[0]->mMatrix.m[3][0], 5.0);
	O3DS_CHECK_EQ(list.mItems[0]->mTransforms[0]->mWorldMatrix.m[3][2], 7.0);
}

// ---------------------------------------------------------------------------
// CORE-8: hierarchy validation
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_ParentCycle_IsRejected)
{
	// PoC `poc cycle`: root plus a <-> b. The unfixed parser returned true
	// with a and b silently left without a world matrix.
	auto buffer = BuildHierarchy({ -1, 2, 1 });
	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(list.mError.find("ycle") != std::string::npos);
}

O3DS_TEST(ParserHardening_LongParentCycle_IsRejected)
{
	// root, then a 5-node ring 1 -> 2 -> 3 -> 4 -> 5 -> 1.
	auto buffer = BuildHierarchy({ -1, 5, 1, 2, 3, 4 });
	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(list.mError.find("ycle") != std::string::npos);
}

O3DS_TEST(ParserHardening_SelfParent_IsRejected)
{
	auto buffer = BuildHierarchy({ -1, 0, 2 });
	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(list.mError.find("self") != std::string::npos);
}

O3DS_TEST(ParserHardening_OutOfRangeParent_IsRejected)
{
	{
		auto buffer = BuildHierarchy({ -1, 0, 3 });
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, buffer));
		O3DS_CHECK(list.mError.find("Parent") != std::string::npos);
	}
	{
		auto buffer = BuildHierarchy({ -1, 0, -7 });
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, buffer));
		O3DS_CHECK(list.mError.find("Parent") != std::string::npos);
	}
}

O3DS_TEST(ParserHardening_ReverseOrderedChain_SolvesWorldMatrices)
{
	// Child-before-parent ordering is legal on the wire; the single-pass
	// solve must still produce the same world matrices as the old
	// iterate-until-stable loop. Node i's parent is i+1, the last node is the
	// root, and every node translates +1 in Y, so node i sits at
	// y = (N - i).
	const int N = 64;
	std::vector<int> parents;
	for (int i = 0; i < N; i++)
		parents.push_back(i == N - 1 ? -1 : i + 1);
	auto buffer = BuildHierarchy(parents);

	SubjectList list;
	O3DS_CHECK(ParseBuffer(list, buffer));
	auto* s = list.mItems[0];
	for (int i = 0; i < N; i++)
	{
		O3DS_CHECK(s->mTransforms[i]->bWorldMatrix);
		O3DS_CHECK_EQ(s->mTransforms[i]->mWorldMatrix.GetTranslation().v[1], (double)(N - i));
	}
}

O3DS_TEST(ParserHardening_WorldMatrices_CanBeDisabled)
{
	auto buffer = BuildHierarchy({ -1, 0, 1 });
	SubjectList list;
	list.mComputeWorldMatrices = false;
	O3DS_CHECK(ParseBuffer(list, buffer));
	for (Transform* t : list.mItems[0]->mTransforms)
		O3DS_CHECK(!t->bWorldMatrix);
	// Local matrices are still built (and checked for non-finite values).
	O3DS_CHECK_EQ(list.mItems[0]->mTransforms[2]->mMatrix.GetTranslation().v[1], 1.0);

	// Validation still runs with world matrices off.
	auto cyclic = BuildHierarchy({ -1, 2, 1 });
	SubjectList list2;
	list2.mComputeWorldMatrices = false;
	O3DS_CHECK(!ParseBuffer(list2, cyclic));
}

O3DS_TEST(ParserHardening_NonFiniteLocalMatrix_IsRejectedByCalcMatrices)
{
	// The NaN check used to run on the identity matrix before anything was
	// multiplied in, so it could never fire.
	Subject s("direct");
	auto* root = s.addTransform("root", -1);
	root->transformOrder = { TTranslation };
	root->translation.value = Vector3d(std::numeric_limits<double>::infinity(), 0.0, 0.0);
	O3DS_CHECK(!s.CalcMatrices());
	O3DS_CHECK(!s.mError.empty());
}

// ---------------------------------------------------------------------------
// CORE-8 / CORE-9: per-buffer limits
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_TooManyTransforms_IsRejected)
{
	std::vector<int> parents;
	for (size_t i = 0; i < ParseLimits::kMaxTransformsPerSubject + 1; i++)
		parents.push_back((int)i - 1);
	auto buffer = BuildHierarchy(parents);
	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

O3DS_TEST(ParserHardening_TransformsAtLimit_Parse)
{
	std::vector<int> parents;
	for (size_t i = 0; i < ParseLimits::kMaxTransformsPerSubject; i++)
		parents.push_back((int)i - 1);
	auto buffer = BuildHierarchy(parents);
	SubjectList list;
	O3DS_CHECK(ParseBuffer(list, buffer));
	O3DS_CHECK_EQ(list.mItems[0]->size(), ParseLimits::kMaxTransformsPerSubject);
}

O3DS_TEST(ParserHardening_SixteenThousandNodeChain_IsRejectedByLimit)
{
	// WP-S1 acceptance: a 16k-node packet parses in under 10 ms or is rejected
	// by the size limit. It is rejected here; the timing is measured
	// separately (docs/review/2026-09-plugin-review/poc/chain.cpp) so this
	// test cannot flake on a slow CI runner.
	SubjectList sender;
	auto* s = sender.addSubject("c");
	const int N = 16000;
	for (int i = 0; i < N; i++)
		s->addTransform("n" + std::to_string(i), i == N - 1 ? -1 : i + 1);
	std::vector<char> buffer;
	sender.Serialize(buffer, 1.0);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

O3DS_TEST(ParserHardening_TooManyCurves_IsRejected)
{
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps{ O3DS::Data::Component_Translation };
	O3DS::Data::Translation t(0, 0, 0);
	NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", &t, nullptr, nullptr, nullptr, &comps) };
	std::vector<flatbuffers::Offset<O3DS::Data::Curve>> curves;
	for (size_t i = 0; i < ParseLimits::kMaxCurvesPerSubject + 1; i++)
		curves.push_back(O3DS::Data::CreateCurveDirect(b, "c", 0.0f));
	SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "s", O3DS::Data::Direction_None,
		O3DS::Data::Direction_None, O3DS::Data::Direction_None, nullptr, &curves) };
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

O3DS_TEST(ParserHardening_TooManySubjects_IsRejected)
{
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps{ O3DS::Data::Component_Translation };
	O3DS::Data::Translation t(0, 0, 0);
	SubjectOffsets subjects;
	for (size_t i = 0; i < ParseLimits::kMaxSubjects + 1; i++)
	{
		NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", &t, nullptr, nullptr, nullptr, &comps) };
		const std::string name = "s" + std::to_string(i);
		subjects.push_back(O3DS::Data::CreateSubjectDirect(b, &nodes, name.c_str()));
	}
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
	O3DS_CHECK(list.size() <= ParseLimits::kMaxSubjects);
}

O3DS_TEST(ParserHardening_SubjectsAccumulatedAcrossBuffers_AreCapped)
{
	// clearInactive=false keeps subjects from earlier buffers, so the cap
	// must hold across buffers too, not only within one.
	SubjectList list;
	bool rejected = false;
	for (size_t i = 0; i < ParseLimits::kMaxSubjects + 1; i++)
	{
		flatbuffers::FlatBufferBuilder b;
		std::vector<int8_t> comps{ O3DS::Data::Component_Translation };
		O3DS::Data::Translation t(0, 0, 0);
		NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", &t, nullptr, nullptr, nullptr, &comps) };
		const std::string name = "s" + std::to_string(i);
		SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, name.c_str()) };
		auto buffer = Finish(b, &subjects, nullptr);
		if (!ParseBuffer(list, buffer, /*clearInactive*/ false))
			rejected = true;
	}
	O3DS_CHECK(rejected);
	O3DS_CHECK(list.size() <= ParseLimits::kMaxSubjects);
}

O3DS_TEST(ParserHardening_TooManyComponents_IsRejected)
{
	flatbuffers::FlatBufferBuilder b;
	std::vector<int8_t> comps(ParseLimits::kMaxComponentsPerTransform + 1, O3DS::Data::Component_Translation);
	O3DS::Data::Translation t(0, 0, 0);
	NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", &t, nullptr, nullptr, nullptr, &comps) };
	SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "s") };
	auto buffer = Finish(b, &subjects, nullptr);

	SubjectList list;
	O3DS_CHECK(!ParseBuffer(list, buffer));
	O3DS_CHECK(!list.mError.empty());
}

// ---------------------------------------------------------------------------
// CORE-9: non-finite floats
// ---------------------------------------------------------------------------

namespace
{
	std::vector<char> BuildSingleNode(const O3DS::Data::Translation* t, const O3DS::Data::Rotation* r,
		const O3DS::Data::Scale* s, const std::vector<O3DS::Data::Matrix>* m, float curveValue, double time = 1.0)
	{
		flatbuffers::FlatBufferBuilder b;
		std::vector<int8_t> comps;
		if (t) comps.push_back(O3DS::Data::Component_Translation);
		if (r) comps.push_back(O3DS::Data::Component_Rotation);
		if (s) comps.push_back(O3DS::Data::Component_Scale);
		if (m) comps.push_back(O3DS::Data::Component_Matrix);
		NodeOffsets nodes{ O3DS::Data::CreateTransformDirect(b, -1, "root", t, r, s, m, &comps) };
		std::vector<flatbuffers::Offset<O3DS::Data::Curve>> curves{ O3DS::Data::CreateCurveDirect(b, "c", curveValue) };
		SubjectOffsets subjects{ O3DS::Data::CreateSubjectDirect(b, &nodes, "s", O3DS::Data::Direction_None,
			O3DS::Data::Direction_None, O3DS::Data::Direction_None, nullptr, &curves) };
		return Finish(b, &subjects, nullptr, time);
	}
}

O3DS_TEST(ParserHardening_NonFiniteSubjectValues_AreRejected)
{
	const O3DS::Data::Translation goodT(1, 2, 3);
	const O3DS::Data::Rotation goodR(0, 0, 0, 1);
	const O3DS::Data::Scale goodS(1, 1, 1);

	{
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &goodS, nullptr, 0.5f)));
	}
	{
		const O3DS::Data::Translation t(kNaN, 0, 0);
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&t, &goodR, &goodS, nullptr, 0.5f)));
	}
	{
		const O3DS::Data::Rotation r(0, kInf, 0, 1);
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &r, &goodS, nullptr, 0.5f)));
	}
	{
		const O3DS::Data::Scale s(1, 1, -kInf);
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &s, nullptr, 0.5f)));
	}
	{
		std::vector<O3DS::Data::Matrix> m(1, O3DS::Data::Matrix(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kNaN, 0, 0, 1));
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &goodS, &m, 0.5f)));
	}
	{
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &goodS, nullptr, kNaN)));
	}
	{
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &goodS, nullptr, kInf)));
	}
	{
		SubjectList list;
		O3DS_CHECK(!ParseBuffer(list, BuildSingleNode(&goodT, &goodR, &goodS, nullptr, 0.5f,
			std::numeric_limits<double>::quiet_NaN())));
	}
}

O3DS_TEST(ParserHardening_NonFiniteUpdateValues_AreRejected)
{
	SubjectList sender;
	auto full = BuildWellFormedFull(sender);

	using TU = O3DS::Data::TranslationUpdate;
	using RU = O3DS::Data::RotationUpdate;
	using SU = O3DS::Data::ScaleUpdate;
	using CU = O3DS::Data::CurveUpdate;

	{
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		std::vector<TU> t{ TU(kNaN, 0, 0, 1) };
		O3DS_CHECK(!ParseBuffer(list, BuildUpdate(&t, nullptr, nullptr, nullptr), false));
		// Rejected before anything was applied.
		O3DS_CHECK_EQ(list.mItems[0]->mTransforms[1]->translation.value.v[1], 2.0);
	}
	{
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		std::vector<RU> r{ RU(0, 0, 0, kInf, 0) };
		O3DS_CHECK(!ParseBuffer(list, BuildUpdate(nullptr, &r, nullptr, nullptr), false));
	}
	{
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		std::vector<SU> s{ SU(1, kNaN, 1, 2) };
		O3DS_CHECK(!ParseBuffer(list, BuildUpdate(nullptr, nullptr, &s, nullptr), false));
	}
	{
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		std::vector<CU> c{ CU(kNaN, 0) };
		O3DS_CHECK(!ParseBuffer(list, BuildUpdate(nullptr, nullptr, nullptr, &c), false));
		O3DS_CHECK_EQ(list.mItems[0]->mCurveValues[0], 0.25f);
	}
	{
		// Residual-coded update (predictor_id != 0) takes the other branch.
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		std::vector<TU> t{ TU(0, -kInf, 0, 0) };
		O3DS_CHECK(!ParseBuffer(list, BuildUpdate(&t, nullptr, nullptr, nullptr, (uint32_t)ResidualPredictorId::Hold), false));
	}
	{
		// A non-finite quantization range would turn every Q8 delta into NaN.
		SubjectList list;
		O3DS_CHECK(ParseBuffer(list, full));
		flatbuffers::FlatBufferBuilder b;
		std::vector<O3DS::Data::TranslationUpdateQ8> q8{ O3DS::Data::TranslationUpdateQ8(1, 1, 1, 0) };
		UpdateOffsets updates{ O3DS::Data::CreateSubjectUpdateDirect(b, "rig", nullptr, nullptr, nullptr, nullptr,
			0, false, kNaN, 0.0f, &q8) };
		O3DS_CHECK(!ParseBuffer(list, Finish(b, nullptr, &updates, 2.0), false));
	}
}

// ---------------------------------------------------------------------------
// CORE-23: ParseUpdate index validation
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_ParseUpdate_SkipsBadIndicesAndAppliesTheRest)
{
	SubjectList sender;
	auto full = BuildWellFormedFull(sender);
	SubjectList list;
	O3DS_CHECK(ParseBuffer(list, full));

	using TU = O3DS::Data::TranslationUpdate;
	using RU = O3DS::Data::RotationUpdate;
	using SU = O3DS::Data::ScaleUpdate;
	using CU = O3DS::Data::CurveUpdate;

	// A negative and a too-large index come BEFORE a valid one in each
	// vector. The unfixed parser stopped at the first bad index and dropped
	// the valid entry after it.
	std::vector<TU> t{ TU(9, 9, 9, -1), TU(9, 9, 9, 3), TU(9, 9, 9, 1000000), TU(4, 5, 6, 2) };
	std::vector<RU> r{ RU(0, 0, 0, 1, -5), RU(0, 0, 0, 1, 3), RU(0, 0, 1, 0, 1) };
	std::vector<SU> s{ SU(7, 7, 7, std::numeric_limits<int>::min()), SU(7, 7, 7, 3), SU(2, 2, 2, 0) };
	std::vector<CU> c{ CU(9, -1), CU(9, 2), CU(0.5f, 1) };
	O3DS_CHECK(ParseBuffer(list, BuildUpdate(&t, &r, &s, &c), false));

	auto* subject = list.mItems[0];
	O3DS_CHECK_EQ(subject->mTransforms[2]->translation.value.v[0], 4.0);
	O3DS_CHECK_EQ(subject->mTransforms[2]->translation.value.v[2], 6.0);
	O3DS_CHECK_EQ(subject->mTransforms[1]->rotation.value.v[2], 1.0);
	O3DS_CHECK_EQ(subject->mTransforms[0]->scale.value.v[0], 2.0);
	O3DS_CHECK_EQ(subject->mCurveValues[1], 0.5f);
	// Untouched channels keep their full-sync values.
	O3DS_CHECK_EQ(subject->mTransforms[0]->translation.value.v[0], 1.0);
	O3DS_CHECK_EQ(subject->mCurveValues[0], 0.25f);
}

// ---------------------------------------------------------------------------
// CORE-26: ClockOffsetEstimator on hostile timestamps
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_ClockOffset_HostileTimestamps_AreIgnored)
{
	ClockOffsetEstimator est;
	const uint64_t local = 1700000000000000ull; // ~2023 in UTC microseconds

	// tx above INT64_MAX: the unfixed estimator computed
	// (int64)local - (int64)tx, which overflows.
	const std::vector<uint64_t> hostile{
		(uint64_t)std::numeric_limits<int64_t>::max() + 1u,
		std::numeric_limits<uint64_t>::max(),
		(uint64_t)std::numeric_limits<int64_t>::max(),
		local + (uint64_t)10u * 86400u * 1000000u, // 10 days ahead
		1u,                                        // ~54 years behind
	};
	for (uint64_t tx : hostile)
	{
		auto s = est.Observe(tx, local);
		O3DS_CHECK_EQ(s.mapped_presentation_time_us, local);
		O3DS_CHECK_EQ(s.offset_estimate_us, (int64_t)0);
		O3DS_CHECK_EQ(s.excess_delay_us, (int64_t)0);
	}

	// State was left untouched: the first sane sample initializes the
	// estimate exactly, as it would on a fresh estimator.
	auto s = est.Observe(local - 500, local);
	O3DS_CHECK_EQ(s.offset_estimate_us, (int64_t)500);
	O3DS_CHECK_EQ(s.mapped_presentation_time_us, local);

	// A hostile local clock is handled the same way.
	auto s2 = est.Observe(local, std::numeric_limits<uint64_t>::max());
	O3DS_CHECK_EQ(s2.offset_estimate_us, (int64_t)0);
	O3DS_CHECK_EQ(s2.mapped_presentation_time_us, std::numeric_limits<uint64_t>::max());
}

O3DS_TEST(ParserHardening_ClockOffset_BackwardStep_DoesNotGrantSlewBudget)
{
	// After a sender clock steps backward, the next forward sample must not
	// get a slew budget computed from the regressed timestamp (which would
	// let the estimate snap).
	ClockOffsetEstimator::Config cfg;
	cfg.window_s = 5.0;
	cfg.max_slew_rate = 0.1;
	ClockOffsetEstimator est(cfg);

	uint64_t tx = 20000000000ull;
	const int64_t kSkewUs = 100000;
	ClockOffsetEstimator::Sample steady;
	for (int i = 0; i < 50; i++)
	{
		steady = est.Observe(tx, (uint64_t)((int64_t)tx + kSkewUs));
		tx += 16000;
	}
	O3DS_CHECK_EQ(steady.offset_estimate_us, kSkewUs);

	// One sample 10 s in the past, same offset.
	const uint64_t back = tx - 10000000ull;
	est.Observe(back, (uint64_t)((int64_t)back + kSkewUs));

	// Next forward sample drops the offset by 300 ms. Only ~16 ms of real
	// time has passed since the newest sample, so movement is bounded by
	// ~1.6 us, not by the 10 s gap to the regressed sample.
	tx += 16000;
	auto after = est.Observe(tx, (uint64_t)((int64_t)tx + kSkewUs - 300000));
	const int64_t movedBy = kSkewUs - after.offset_estimate_us;
	O3DS_CHECK(movedBy >= 0);
	O3DS_CHECK(movedBy < 10000);
}

// ---------------------------------------------------------------------------
// CORE-17 (partial): SubjectList copy assignment double-delete
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_SubjectList_IsMoveOnly)
{
	// Checked at runtime (not static_assert) so this file still compiles
	// against the unfixed header and the failure shows up as a test result.
	O3DS_CHECK(!std::is_copy_constructible<SubjectList>::value);
	O3DS_CHECK(!std::is_copy_assignable<SubjectList>::value);
	O3DS_CHECK(std::is_move_constructible<SubjectList>::value);
	O3DS_CHECK(std::is_move_assignable<SubjectList>::value);
}

// Only compiled once SubjectList is actually move-only: exercises the move
// operations under ASan (a shallow copy would double-delete here).
template <typename T>
static typename std::enable_if<std::is_move_assignable<T>::value && !std::is_copy_assignable<T>::value>::type
ExerciseMoves()
{
	T a;
	a.addSubject("one")->addTransform("root", -1);
	a.mTime = 3.0;

	T b(std::move(a));
	O3DS_CHECK_EQ(b.size(), (size_t)1);
	O3DS_CHECK_EQ(a.size(), (size_t)0);
	O3DS_CHECK_EQ(b.mTime, 3.0);

	T c;
	c.addSubject("two");
	c.addSubject("three");
	c = std::move(b);
	O3DS_CHECK_EQ(c.size(), (size_t)1);
	O3DS_CHECK_EQ(c.mItems[0]->mName, std::string("one"));
	O3DS_CHECK_EQ(b.size(), (size_t)0);

	c = std::move(c); // self-move must not free the items
	O3DS_CHECK_EQ(c.size(), (size_t)1);
}

template <typename T>
static typename std::enable_if<!(std::is_move_assignable<T>::value && !std::is_copy_assignable<T>::value)>::type
ExerciseMoves()
{
	throw o3ds_test::TestFailure{ "SubjectList is still copy-assignable (shallow copy double-deletes)" };
}

O3DS_TEST(ParserHardening_SubjectList_MoveTransfersOwnership)
{
	ExerciseMoves<SubjectList>();
}

// ---------------------------------------------------------------------------
// Well-formed round trip still passes
// ---------------------------------------------------------------------------

O3DS_TEST(ParserHardening_WellFormedRoundTrip_StillParses)
{
	SubjectList sender;
	auto full = BuildWellFormedFull(sender);

	SubjectList list;
	O3DS_CHECK(ParseBuffer(list, full));
	O3DS_CHECK(list.mError.empty());
	auto* s = list.findSubject("rig");
	O3DS_CHECK(s != nullptr);
	O3DS_CHECK_EQ(s->size(), (size_t)3);
	O3DS_CHECK_EQ(s->mCurveValues[1], 0.75f);
	// World translation of b = root(1,0,0) + a(0,2,0) + b(0,0,3).
	O3DS_CHECK(s->mTransforms[2]->bWorldMatrix);
	auto world = s->mTransforms[2]->mWorldMatrix.GetTranslation();
	O3DS_CHECK_EQ(world.v[0], 1.0);
	O3DS_CHECK_EQ(world.v[1], 2.0);
	O3DS_CHECK_EQ(world.v[2], 3.0);

	// And a normal delta update on top of it.
	sender.mItems[0]->mTransforms[1]->translation.value = Vector3d(0.0, 4.0, 0.0);
	std::vector<char> update;
	size_t count = 0;
	sender.SerializeUpdate(update, count, 2.0);
	O3DS_CHECK(ParseBuffer(list, update, false));
	O3DS_CHECK_EQ(s->mTransforms[1]->translation.value.v[1], 4.0);
	O3DS_CHECK_EQ(s->mTransforms[2]->mWorldMatrix.GetTranslation().v[1], 4.0);
}
