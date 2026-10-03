// Tests for object reuse in SubjectList::Parse() (CORE-18, the Parse half;
// WP-A2 follow-up). A full sync used to delete every subject (clearInactive)
// and every transform and build them again; it now reuses the objects. The
// oracle: whatever a list held before, parsing a buffer into it must leave
// exactly the state that parsing the same buffer into a new list leaves
// (with clearInactive, the old code deleted everything first, so that was its
// behaviour), and Parse() must return and report the same.
#include "test_framework.h"

#include "o3ds/model.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace O3DS;

namespace
{
	using NodeOffsets = std::vector<flatbuffers::Offset<O3DS::Data::Transform>>;
	using SubjectOffsets = std::vector<flatbuffers::Offset<O3DS::Data::Subject>>;
	using UpdateOffsets = std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>>;
	using CurveOffsets = std::vector<flatbuffers::Offset<O3DS::Data::Curve>>;

	struct NodeSpec
	{
		const char* name; // nullptr: a nameless node (RCV-14 placeholder)
		int parent;
		float tx;
		bool withMatrix;
	};

	struct SubjectSpec
	{
		const char* name;
		std::vector<NodeSpec> nodes;
		bool withCurves;
		std::vector<std::pair<const char*, float>> curves;
		const char* format;
	};

	flatbuffers::Offset<O3DS::Data::Subject> AddSubject(flatbuffers::FlatBufferBuilder& b, const SubjectSpec& spec)
	{
		NodeOffsets nodes;
		for (const NodeSpec& node : spec.nodes)
		{
			const O3DS::Data::Translation t(node.tx, 1.0f, 2.0f);
			const O3DS::Data::Rotation r(0.0f, 0.0f, 0.0f, 1.0f);
			const O3DS::Data::Scale s(1.0f, 1.0f, 1.0f);
			std::vector<int8_t> comps{ O3DS::Data::Component_Translation, O3DS::Data::Component_Rotation, O3DS::Data::Component_Scale };
			std::vector<O3DS::Data::Matrix> matrices;
			if (node.withMatrix)
			{
				comps.push_back(O3DS::Data::Component_Matrix);
				O3DS::Data::Matrix m;
				matrices.push_back(m);
			}
			nodes.push_back(O3DS::Data::CreateTransformDirect(b, node.parent, node.name, &t, &r, &s,
				node.withMatrix ? &matrices : nullptr, &comps));
		}
		CurveOffsets curves;
		for (const auto& curve : spec.curves)
		{
			curves.push_back(O3DS::Data::CreateCurveDirect(b, curve.first, curve.second));
		}
		return O3DS::Data::CreateSubjectDirect(b, &nodes, spec.name,
			O3DS::Data::Direction_Right, O3DS::Data::Direction_Up, O3DS::Data::Direction_Back,
			spec.format, spec.withCurves ? &curves : nullptr);
	}

	std::vector<char> Buffer(const std::vector<SubjectSpec>& subjects, const char* updateFor = nullptr)
	{
		flatbuffers::FlatBufferBuilder b;
		SubjectOffsets offsets;
		for (const SubjectSpec& spec : subjects)
		{
			offsets.push_back(AddSubject(b, spec));
		}
		UpdateOffsets updates;
		std::vector<O3DS::Data::TranslationUpdate> translations{ O3DS::Data::TranslationUpdate(9.0f, 9.0f, 9.0f, 0) };
		if (updateFor != nullptr)
		{
			updates.push_back(O3DS::Data::CreateSubjectUpdateDirect(b, updateFor, &translations));
		}
		b.Finish(O3DS::Data::CreateSubjectListDirect(b, &offsets, updateFor ? &updates : nullptr, 1.0));
		std::vector<char> out;
		finalize(b, out, 1);
		return out;
	}

	void Append(std::string& out, const char* format, double value)
	{
		char text[64];
		std::snprintf(text, sizeof(text), format, value);
		out += text;
	}

	std::string DumpTransform(Transform& t)
	{
		std::string out = t.mName + "|" + std::to_string(t.mParentId) + "|ref=" + (t.mReference ? "set" : "null") + "|order=";
		for (ComponentType c : t.transformOrder)
		{
			out += std::to_string(static_cast<int>(c)) + ",";
		}
		for (int i = 0; i < 3; ++i) Append(out, " t%.17g", t.translation.value.v[i]);
		for (int i = 0; i < 4; ++i) Append(out, " r%.17g", t.rotation.value.v[i]);
		for (int i = 0; i < 3; ++i) Append(out, " s%.17g", t.scale.value.v[i]);
		out += " matrices=" + std::to_string(t.matrices.size());
		for (int r = 0; r < 4; ++r)
		{
			for (int c = 0; c < 4; ++c)
			{
				Append(out, " m%.17g", t.mMatrix.m[r][c]);
				Append(out, " w%.17g", t.mWorldMatrix.m[r][c]);
			}
		}
		out += t.bWorldMatrix ? " world" : " noworld";
		out += t.mQuantAnchorSet ? " anchor" : " noanchor";
		for (int i = 0; i < 3; ++i) Append(out, " a%.17g", t.mQuantAnchorTranslation.v[i]);
		out += " tiers=" + std::to_string(static_cast<int>(t.mLastTranslationTier)) + std::to_string(static_cast<int>(t.mLastRotationTier));
		return out;
	}

	std::string DumpSubject(Subject& s)
	{
		std::string out = "subject " + s.mName + " ref=" + (s.mReference ? "set" : "null")
			+ " error=" + s.mError + " decoder=" + (s.GetResidualDecoder() ? "set" : "null")
			+ " encoder=" + (s.GetResidualEncoder() ? "set" : "null") + " joints=";
		for (const std::string& j : s.mJoints) out += j + ",";
		out += " axes=" + std::to_string(static_cast<int>(s.mContext.mX)) + std::to_string(static_cast<int>(s.mContext.mY))
			+ std::to_string(static_cast<int>(s.mContext.mZ)) + " format=" + s.mContext.mFormat + " curves=";
		for (size_t i = 0; i < s.mCurveNames.size(); ++i) out += s.mCurveNames[i] + ",";
		for (float v : s.mCurveValues) Append(out, "%.9g,", v);
		out += "\n";
		for (Transform* t : s.mTransforms.mItems)
		{
			out += "  " + DumpTransform(*t) + "\n";
		}
		return out;
	}

	std::string Dump(SubjectList& list)
	{
		std::string out;
		for (Subject* s : list.mItems)
		{
			out += DumpSubject(*s);
		}
		return out;
	}

	std::string DumpTouched(const std::vector<ParsedSubjectInfo>& touched)
	{
		std::string out;
		for (const ParsedSubjectInfo& info : touched)
		{
			out += info.name + (info.fullDescriptor ? "(full)," : "(update),");
		}
		return out;
	}

	//! Parses `buffer` into `reused` and into a new list; checks that both
	//! return the same, report the same subjects and hold the same state.
	void CheckMatchesFresh(SubjectList& reused, const std::vector<char>& buffer, bool expectOk, const char* step)
	{
		std::vector<ParsedSubjectInfo> touchedReused;
		std::vector<ParsedSubjectInfo> touchedFresh;
		SubjectList fresh;
		const bool okReused = reused.Parse(buffer.data(), buffer.size(), nullptr, true, &touchedReused);
		const bool okFresh = fresh.Parse(buffer.data(), buffer.size(), nullptr, true, &touchedFresh);
		if (okReused != expectOk || okFresh != expectOk || reused.mError != fresh.mError
			|| DumpTouched(touchedReused) != DumpTouched(touchedFresh) || Dump(reused) != Dump(fresh))
		{
			std::printf("step %s: ok %d/%d error '%s'/'%s'\ntouched %s / %s\nreused:\n%s\nfresh:\n%s\n", step,
				okReused ? 1 : 0, okFresh ? 1 : 0, reused.mError.c_str(), fresh.mError.c_str(),
				DumpTouched(touchedReused).c_str(), DumpTouched(touchedFresh).c_str(),
				Dump(reused).c_str(), Dump(fresh).c_str());
		}
		O3DS_CHECK(okReused == expectOk);
		O3DS_CHECK(okFresh == expectOk);
		O3DS_CHECK(reused.mError == fresh.mError);
		O3DS_CHECK(DumpTouched(touchedReused) == DumpTouched(touchedFresh));
		O3DS_CHECK(Dump(reused) == Dump(fresh));
	}

	const SubjectSpec kS1{ "s1", { { "root", -1, 1.0f, false }, { "a", 0, 2.0f, false }, { "b", 1, 3.0f, false } }, true, { { "c0", 0.25f }, { "c1", 0.75f } }, "fmtA" };
	const SubjectSpec kS2{ "s2", { { "hip", -1, 4.0f, false }, { "leg", 0, 5.0f, false } }, true, { { "blink_long_curve_name_over_sso", 0.5f } }, nullptr };
}

O3DS_TEST(ParseReuse_ResyncLeavesTheStateOfAFreshParse)
{
	SubjectList reused;
	CheckMatchesFresh(reused, Buffer({ kS1, kS2 }), true, "A: two subjects");

	// State the parser never sets on a subject or transform: a resync of the
	// same name must not carry it over, because a new object would not have it.
	int marker = 0;
	Subject* s2 = reused.findSubject("s2");
	O3DS_CHECK(s2 != nullptr);
	s2->mReference = &marker;
	s2->mJoints = { "j0" };
	s2->mError = "old error";
	s2->mTransforms.mItems[0]->mReference = &marker;
	s2->mTransforms.mItems[0]->mLastTranslationTier = QuantTier::Byte;
	s2->mTransforms.mItems[1]->matrices.resize(3);

	// B: s1 dropped; s2 first, more nodes, other names and parents, a matrix
	// component, no curves field at all; a new subject s3.
	const SubjectSpec s2b{ "s2", { { "pelvis", -1, 6.0f, true }, { "spine", 0, 7.0f, false }, { "neck", 1, 8.0f, false }, { "head", 2, 9.0f, false } }, false, {}, "fmtB" };
	const SubjectSpec s3{ "s3", { { "only", -1, 1.5f, false } }, true, {}, nullptr };
	CheckMatchesFresh(reused, Buffer({ s2b, s3 }), true, "B: reorder, grow, drop curves, drop s1");

	// C: s2 shrinks to one node with an empty curve list; s1 comes back with
	// nameless nodes; the buffer also updates s3, which this buffer drops, so
	// the update must not apply (and is not reported).
	const SubjectSpec s2c{ "s2", { { "pelvis", -1, 6.5f, false } }, true, {}, nullptr };
	const SubjectSpec s1c{ "s1", { { nullptr, -1, 1.0f, false }, { "x", 0, 2.0f, false }, { nullptr, 1, 3.0f, false } }, true, { { "c9", 0.1f } }, nullptr };
	CheckMatchesFresh(reused, Buffer({ s2c, s1c }, "s3"), true, "C: shrink, nameless nodes, update for a dropped subject");

	// D: the same name twice in one buffer (the second full subject wins).
	CheckMatchesFresh(reused, Buffer({ kS1, kS2, s1c }), true, "D: duplicate name");

	// E: an update for a subject the buffer keeps.
	CheckMatchesFresh(reused, Buffer({ kS1, kS2 }, "s1"), true, "E: update for a kept subject");

	// F: a rejected subject (two roots) after a valid one. Parse fails; the
	// list holds what was parsed before the rejection, as from a fresh list.
	const SubjectSpec bad{ "s1", { { "r0", -1, 0.0f, false }, { "r1", -1, 0.0f, false } }, true, {}, nullptr };
	CheckMatchesFresh(reused, Buffer({ kS2, bad }), false, "F: rejected subject");

	// G: recovery after the failure.
	CheckMatchesFresh(reused, Buffer({ kS1, kS2 }), true, "G: after a failure");

	// H: an empty full-subject list drops everything.
	CheckMatchesFresh(reused, Buffer({}), true, "H: no subjects");
	O3DS_CHECK(reused.size() == 0);
}

O3DS_TEST(ParseReuse_SameSubjectKeepsItsObjects)
{
	SubjectList list;
	const std::vector<char> a = Buffer({ kS1 });
	O3DS_CHECK(list.Parse(a.data(), a.size()));
	Subject* subject = list.findSubject("s1");
	O3DS_CHECK(subject != nullptr);
	const std::vector<Transform*> before = subject->mTransforms.mItems;

	// A resync with more nodes keeps the subject and its first transforms.
	SubjectSpec grown = kS1;
	grown.nodes.push_back({ "c", 2, 4.0f, false });
	const std::vector<char> b = Buffer({ grown });
	O3DS_CHECK(list.Parse(b.data(), b.size()));
	O3DS_CHECK(list.findSubject("s1") == subject);
	O3DS_CHECK(subject->mTransforms.mItems.size() == 4);
	for (size_t i = 0; i < before.size(); ++i)
	{
		O3DS_CHECK(subject->mTransforms.mItems[i] == before[i]);
	}
	O3DS_CHECK(subject->mTransforms.mItems[3]->mName == "c");
}

O3DS_TEST(ParseReuse_KeepInactiveReusesTransformsToo)
{
	// clearInactive = false keeps subjects the buffer does not mention and
	// updates the ones it does in place; their transforms must still end up
	// as a fresh parse leaves them.
	SubjectList list;
	const std::vector<char> a = Buffer({ kS1, kS2 });
	O3DS_CHECK(list.Parse(a.data(), a.size(), nullptr, false));

	const SubjectSpec s2b{ "s2", { { "pelvis", -1, 6.0f, true }, { "spine", 0, 7.0f, false }, { "neck", 1, 8.0f, false } }, true, { { "c", 1.0f } }, nullptr };
	const std::vector<char> b = Buffer({ s2b });
	O3DS_CHECK(list.Parse(b.data(), b.size(), nullptr, false));
	SubjectList fresh;
	O3DS_CHECK(fresh.Parse(b.data(), b.size()));

	O3DS_CHECK(list.findSubject("s1") != nullptr);
	O3DS_CHECK(list.findSubject("s2") != nullptr);
	O3DS_CHECK(DumpSubject(*list.findSubject("s2")) == DumpSubject(*fresh.findSubject("s2")));
}
