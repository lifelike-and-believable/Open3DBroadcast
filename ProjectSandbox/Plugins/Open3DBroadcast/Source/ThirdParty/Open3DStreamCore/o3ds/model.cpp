/*
Open 3D Stream

Copyright 2020 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "model.h"
#include "crc32.h"
#include "getTime.h"
#include "parse_limits.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>

using namespace O3DS::Data;

namespace
{
	//! One FlatBufferBuilder per thread, cleared and reused by every
	//! Serialize* call that writes a whole buffer (CORE-18). Clear() keeps the
	//! builder's allocation, so a steady stream of frames stops allocating
	//! once the largest frame has been built. The calls never nest, and each
	//! finishes with the builder before it returns.
	flatbuffers::FlatBufferBuilder& ReusableBuilder()
	{
		thread_local flatbuffers::FlatBufferBuilder builder(16 * 1024);
		builder.Clear();
		return builder;
	}
}

void operator >>(const O3DS::TransformTranslation& src, O3DS::Data::Translation &dst)
{
	dst = O3DS::Data::Translation(
		(float)src.value.v[0],
		(float)src.value.v[1],
		(float)src.value.v[2]);
}

void operator >>(const O3DS::Data::Translation& src, O3DS::TransformTranslation &dst)
{
	dst = O3DS::TransformTranslation(src.x(), src.y(), src.z());
}

void operator >>(const O3DS::Data::TranslationUpdate& src, O3DS::TransformTranslation &dst)
{
	dst = O3DS::TransformTranslation(src.x(), src.y(), src.z());
}

void operator >>(const O3DS::TransformRotation& src, O3DS::Data::Rotation &dst)
{
	dst = O3DS::Data::Rotation(
		(float)src.value.v[0],
		(float)src.value.v[1],
		(float)src.value.v[2],
		(float)src.value.v[3]);
}

void operator >>(const O3DS::Data::Rotation& src, O3DS::TransformRotation &dst)
{
	dst = O3DS::TransformRotation(src.x(), src.y(), src.z(), src.w());
}

void operator >>(const O3DS::Data::RotationUpdate& src, O3DS::TransformRotation &dst)
{
	dst = O3DS::TransformRotation(src.x(), src.y(), src.z(), src.w());
}


void operator >>(const O3DS::TransformScale& src, O3DS::Data::Scale &dst)
{
	dst = O3DS::Data::Scale(
		(float)src.value.v[0],
		(float)src.value.v[1],
		(float)src.value.v[2]);
}

void operator >>(const O3DS::Data::Scale& src, O3DS::TransformScale &dst)
{
	dst = O3DS::TransformScale(src.x(), src.y(), src.z());
}

void operator >>(const O3DS::Data::ScaleUpdate& src, O3DS::TransformScale &dst)
{
	dst = O3DS::TransformScale(src.x(), src.y(), src.z());
}

void operator >>(const O3DS::TransformMatrix& src, O3DS::Data::Matrix &dst)
{
	dst = O3DS::Data::Matrix(
		(float)src.value.m[0][0],
		(float)src.value.m[0][1],
		(float)src.value.m[0][2],
		(float)src.value.m[0][3],
		(float)src.value.m[1][0],
		(float)src.value.m[1][1],
		(float)src.value.m[1][2],
		(float)src.value.m[1][3],
		(float)src.value.m[2][0],
		(float)src.value.m[2][1],
		(float)src.value.m[2][2],
		(float)src.value.m[2][3],
		(float)src.value.m[3][0],
		(float)src.value.m[3][1],
		(float)src.value.m[3][2],
		(float)src.value.m[3][3]);
}

void operator >>(const O3DS::Data::Matrix& src, O3DS::TransformMatrix &dst)
{
	dst.value.m[0][0] = src.m00();
	dst.value.m[0][1] = src.m01();
	dst.value.m[0][2] = src.m02();
	dst.value.m[0][3] = src.m03();
	dst.value.m[1][0] = src.m10();
	dst.value.m[1][1] = src.m11();
	dst.value.m[1][2] = src.m12();
	dst.value.m[1][3] = src.m13();
	dst.value.m[2][0] = src.m20();
	dst.value.m[2][1] = src.m21();
	dst.value.m[2][2] = src.m22();
	dst.value.m[2][3] = src.m23();
	dst.value.m[3][0] = src.m30();
	dst.value.m[3][1] = src.m31();
	dst.value.m[3][2] = src.m32();
	dst.value.m[3][3] = src.m33();
}

O3DS::Data::Direction dir(enum O3DS::Direction d)
{
	switch (d)
	{
	case O3DS::Direction::Up: return O3DS::Data::Direction_Up;
	case O3DS::Direction::Down: return O3DS::Data::Direction_Down;
	case O3DS::Direction::Left: return O3DS::Data::Direction_Left;
	case O3DS::Direction::Right: return O3DS::Data::Direction_Right;
	case O3DS::Direction::Forward: return O3DS::Data::Direction_Forward;
	case O3DS::Direction::Back: return O3DS::Data::Direction_Back;
	}
	return O3DS::Data::Direction_None;
}

enum O3DS::Direction dir(O3DS::Data::Direction d)
{
	switch (d)
	{
	case O3DS::Data::Direction_Up: return O3DS::Direction::Up;
	case O3DS::Data::Direction_Down: return O3DS::Direction::Down;
	case O3DS::Data::Direction_Left: return O3DS::Direction::Left;
	case O3DS::Data::Direction_Right: return O3DS::Direction::Right;
	case O3DS::Data::Direction_Forward: return O3DS::Direction::Forward;
	case O3DS::Data::Direction_Back: return O3DS::Direction::Back;
	}
	return O3DS::Direction::None;
}

// Wire-value validation helpers (WP-S1, CORE-9). Everything read from a
// buffer is untrusted: the CRC and the FlatBuffers Verifier only prove the
// bytes are intact and structurally sound, not that the floats are finite.
namespace
{
	bool Finite(double v) { return std::isfinite(v); }

	template <typename T>
	bool Finite3(const T& v) { return Finite(v.x()) && Finite(v.y()) && Finite(v.z()); }

	template <typename T>
	bool Finite4(const T& v) { return Finite3(v) && Finite(v.w()); }

	bool MatrixIsFinite(const O3DS::Matrixd& m)
	{
		for (int u = 0; u < 4; u++)
			for (int v = 0; v < 4; v++)
				if (!Finite(m.m[u][v])) return false;
		return true;
	}

	bool MatrixIsFinite(const O3DS::Data::Matrix& m)
	{
		return Finite(m.m00()) && Finite(m.m01()) && Finite(m.m02()) && Finite(m.m03())
			&& Finite(m.m10()) && Finite(m.m11()) && Finite(m.m12()) && Finite(m.m13())
			&& Finite(m.m20()) && Finite(m.m21()) && Finite(m.m22()) && Finite(m.m23())
			&& Finite(m.m30()) && Finite(m.m31()) && Finite(m.m32()) && Finite(m.m33());
	}

	template <typename VectorT, typename Pred>
	bool AllOf(const VectorT* vec, Pred pred)
	{
		if (vec == nullptr) return true;
		for (auto item : *vec)
			if (!pred(*item)) return false;
		return true;
	}

	//! Returns false (with `error` set) if any float an update carries is
	//! NaN or Inf. Covers the legacy and residual paths (both read the same
	//! vectors) and the quantization ranges, which scale every Q8/Q16 delta.
	bool ValidateUpdateFloats(const O3DS::Data::SubjectUpdate* inUpdate, std::string& error)
	{
		const bool ok =
			AllOf(inUpdate->translations(), [](const O3DS::Data::TranslationUpdate& t) { return Finite3(t); })
			&& AllOf(inUpdate->rotation(), [](const O3DS::Data::RotationUpdate& r) { return Finite4(r); })
			&& AllOf(inUpdate->scale(), [](const O3DS::Data::ScaleUpdate& s) { return Finite3(s); })
			&& AllOf(inUpdate->curves(), [](const O3DS::Data::CurveUpdate& c) { return Finite(c.value()); })
			&& Finite(inUpdate->quant_byte_range())
			&& Finite(inUpdate->quant_half_range());
		if (!ok)
			error = "Non-finite value in update";
		return ok;
	}

	//! Checks one wire subject against ParseLimits, CORE-1 (matrix
	//! components need matching matrices) and CORE-9 (finite values) before
	//! ParseSubject() touches any existing state. Returns false with `error`
	//! set on the first problem.
	bool ValidateSubject(const O3DS::Data::Subject* inSubject, std::string& error)
	{
		const std::string subjectName = inSubject->name()->str();

		auto inCurves = inSubject->curves();
		if (inCurves != nullptr)
		{
			if (inCurves->size() > O3DS::ParseLimits::kMaxCurvesPerSubject)
			{
				error = "Too many curves in subject " + subjectName;
				return false;
			}
			for (auto each : *inCurves)
			{
				if (each != nullptr && !Finite(each->value()))
				{
					error = "Non-finite curve value in subject " + subjectName;
					return false;
				}
			}
		}

		auto ovNodes = inSubject->nodes();
		if (ovNodes == nullptr)
			return true;

		if (ovNodes->size() > O3DS::ParseLimits::kMaxTransformsPerSubject)
		{
			error = "Too many transforms in subject " + subjectName;
			return false;
		}

		for (auto inNode : *ovNodes)
		{
			// RCV-14: nodes are addressed by index (parent ids), so a node
			// cannot be dropped without shifting every later parent id.
			if (inNode == nullptr)
			{
				error = "Null transform in subject " + subjectName;
				return false;
			}

			auto inComponents = inNode->components();
			auto inMatrix = inNode->matrix();
			const size_t componentCount = inComponents ? inComponents->size() : 0;
			const size_t matrixCount = inMatrix ? inMatrix->size() : 0;
			if (componentCount > O3DS::ParseLimits::kMaxComponentsPerTransform
				|| matrixCount > O3DS::ParseLimits::kMaxComponentsPerTransform)
			{
				error = "Too many components on a transform in subject " + subjectName;
				return false;
			}

			// Only values ParseSubject actually applies are checked: TRS
			// are copied only when their component is listed, matrices are
			// always copied.
			size_t matrixComponents = 0;
			bool finite = true;
			for (size_t c = 0; c < componentCount; c++)
			{
				const int8_t componentId = inComponents->Get((flatbuffers::uoffset_t)c);
				if (componentId == O3DS::Data::Component::Component_Translation && inNode->translation())
					finite = finite && Finite3(*inNode->translation());
				if (componentId == O3DS::Data::Component::Component_Rotation && inNode->rotation())
					finite = finite && Finite4(*inNode->rotation());
				if (componentId == O3DS::Data::Component::Component_Scale && inNode->scale())
					finite = finite && Finite3(*inNode->scale());
				if (componentId == O3DS::Data::Component::Component_Matrix)
					matrixComponents++;
			}
			finite = finite && AllOf(inMatrix, [](const O3DS::Data::Matrix& m) { return MatrixIsFinite(m); });
			if (!finite)
			{
				error = "Non-finite transform value in subject " + subjectName;
				return false;
			}

			// CORE-1: every matrix component must have a matrix to read.
			if (matrixComponents > matrixCount)
			{
				error = "Matrix component without matching matrix in subject " + subjectName;
				return false;
			}
		}

		return true;
	}

	//! RCV-14: placeholder name for a node the wire sent without a name.
	std::string UnnamedTransformName(size_t index)
	{
		return std::string(O3DS::kUnnamedTransformPrefix) + std::to_string(index);
	}

	//! Wire index -> container index, or false if it is out of range.
	//! Takes the wire's signed int as-is, so a negative index is rejected
	//! rather than wrapping to a huge size_t (CORE-23).
	bool ValidIndex(int id, size_t size)
	{
		return id >= 0 && (size_t)id < size;
	}
}

namespace O3DS
{

	// Transform 

	Transform::Transform(const std::string& name, int parentId, void *ref)
		: bWorldMatrix(false)
		, mName(name)
		, mParentId(parentId)
		, mReference(ref)	
	{}

	Transform::Transform(int parentId)
		: bWorldMatrix(false)
		, mName()
		, mParentId(parentId)
		, mReference(nullptr)
	{}

	Transform::Transform()
		: bWorldMatrix(false)
		, mName()
		, mParentId(-1)
		, mReference(nullptr)
	{}

	Transform::~Transform()
	{};

	bool Transform::nan()
	{
		if (mMatrix.HasNan()) return true;
		if (mWorldMatrix.HasNan()) return true;
		if (translation.value.v[0] != translation.value.v[0]) return true;
		if (translation.value.v[1] != translation.value.v[1]) return true;
		if (translation.value.v[2] != translation.value.v[2]) return true;
		if (rotation.value.v[0] != rotation.value.v[0]) return true;
		if (rotation.value.v[1] != rotation.value.v[1]) return true;
		if (rotation.value.v[2] != rotation.value.v[2]) return true;
		if (rotation.value.v[3] != rotation.value.v[3]) return true;
		if (scale.value.v[0] != scale.value.v[0]) return true;
		if (scale.value.v[1] != scale.value.v[1]) return true;
		if (scale.value.v[2] != scale.value.v[2]) return true;

		for (const auto& i : matrices) {
			if(i.value.HasNan()) { return true; }
		}
		return false;
	}

	bool Subject::CalcMatrices(bool computeWorldMatrices)
	{
		const size_t count = this->mTransforms.size();

		// Local matrices
		for (auto& transform : this->mTransforms)
		{
			transform->bWorldMatrix = false;
			auto &m = transform->mMatrix;
			m = Matrixd();

			size_t matrixId = 0;

			for (auto op : transform->transformOrder)
			{
				if (op == O3DS::TTranslation)
				{
					m = Matrixd::TranslateXYZ(transform->translation.value) * m;
				}
				if (op == O3DS::TRotation)
				{
					m = transform->rotation.asMatrix() * m;
				}
				if (op == O3DS::TScale)
				{
					m = m.Scale(transform->scale.value) * m;
				}
				if (op == O3DS::TMatrix)
				{
					// CORE-1: a matrix component with no matching entry in
					// `matrices` used to read past the end of the vector.
					if (matrixId >= transform->matrices.size())
					{
						mError = "Matrix component of " + transform->mName + " has no matching matrix";
						return false;
					}
					m = transform->matrices[matrixId++].value * m;
				}
			}

			// CORE-8: checked after the matrix is built (it used to run on
			// the identity, so it could never fire), and for Inf as well as
			// NaN.
			if (!MatrixIsFinite(m))
			{
				mError = "Matrix NAN";
				return false;
			}
		}

		// Validate the hierarchy and solve world matrices in one pass
		// (CORE-8). The previous iterate-until-stable loop was O(N * depth),
		// which a reverse-ordered chain turns into O(N^2), and it silently
		// returned true for a parent cycle that did not include the root.

		int rootCount = 0;
		for (size_t transformId = 0; transformId < count; transformId++)
		{
			const auto transform = this->mTransforms[transformId];
			const int parentId = transform->mParentId;
			if (parentId == -1)
			{
				rootCount++;
				continue;
			}

			if (parentId >= 0 && (size_t)parentId == transformId)
			{
				std::ostringstream oss;
				oss << "ParentId of " << transform->mName << " points to self (" << transformId << ")";
				mError = oss.str();
				return false;
			}

			if (parentId < 0 || (size_t)parentId >= count)
			{
				mError = "Invalid Parent Id";
				return false;
			}
		}

		if(rootCount == 0)
		{
			mError = "Could not find a root";
			return false;
		}
		if(rootCount > 1)
		{
			mError = "More than one root found";
			return false;
		}

		// Every parent id is now in range and there is exactly one root.
		// Walk each unresolved transform up to the nearest resolved ancestor
		// (or the root), then resolve the walked path top-down. Each
		// transform is resolved exactly once, so this is O(N) overall. A
		// walk that reaches a transform already on the current path is a
		// cycle.
		enum : uint8_t { Unvisited = 0, OnPath = 1, Resolved = 2 };
		std::vector<uint8_t> state(count, Unvisited);
		std::vector<size_t> path;

		for (size_t start = 0; start < count; start++)
		{
			if (state[start] == Resolved)
				continue;

			path.clear();
			size_t current = start;
			while (true)
			{
				if (state[current] == Resolved)
					break;
				if (state[current] == OnPath)
				{
					mError = "Parent cycle found at " + this->mTransforms[current]->mName;
					return false;
				}
				state[current] = OnPath;
				path.push_back(current);

				const int parentId = this->mTransforms[current]->mParentId;
				if (parentId == -1)
					break;
				current = (size_t)parentId;
			}

			// Resolve from the top of the walked path downward, so each
			// transform's parent is resolved before the transform itself.
			for (auto it = path.rbegin(); it != path.rend(); ++it)
			{
				auto transform = this->mTransforms[*it];
				if (computeWorldMatrices)
				{
					if (transform->mParentId == -1)
					{
						// No Parent - matrix is world matrix
						transform->mWorldMatrix = transform->mMatrix;
					}
					else
					{
						auto parentTransform = this->mTransforms[(size_t)transform->mParentId];
						transform->mWorldMatrix = transform->mMatrix * parentTransform->mWorldMatrix;
					}
					transform->bWorldMatrix = true;
				}
				state[*it] = Resolved;
			}
		}

		return true;
	}



	// Subject

	flatbuffers::Offset<O3DS::Data::Subject> Subject::Serialize(flatbuffers::FlatBufferBuilder& builder)
	{
		
		auto oSubjectName = builder.CreateString(this->mName);
		auto oFormat = builder.CreateString(this->mContext.mFormat);
		std::vector<flatbuffers::Offset<O3DS::Data::Transform>> ovSkeleton;

		O3DS::Data::Translation translation;
		O3DS::Data::Rotation rotation;
		O3DS::Data::Scale scale;

		for (auto& t : this->mTransforms) {
			int matrixId = 0;

			std::vector<O3DS::Data::Matrix> matrices;
			std::vector<int8_t> components;

			t->translation >> translation;
			t->rotation >> rotation;
			t->scale >> scale;

			// D1 quantization anchor, re-captured at EVERY full sync (ADR 0005
			// (vii), WP-S3/SND-2). The receiver's ParseSubject() anchors each
			// transform to the translation it parses from this same full sync,
			// so both sides agree after every full sync, including a receiver
			// that joined late. The anchor is the float32 value written on the
			// wire (not the double held here), so sender and receiver compute
			// quantized deltas against identical references even at large
			// coordinates.
			// Until SubjectUpdate.ref_seq lands (WP-A4a), a receiver that loses
			// a full sync decodes quantized translation deltas against the
			// previous anchor until the next full sync arrives; periodic full
			// syncs bound that window.
			t->mQuantAnchorTranslation = Vector3d(
				(double)(float)t->translation.value.v[0],
				(double)(float)t->translation.value.v[1],
				(double)(float)t->translation.value.v[2]);
			t->mQuantAnchorSet = true;

			for (const auto component : t->transformOrder) {
				if (component == O3DS::TTranslation) {
					components.push_back(O3DS::Data::Component::Component_Translation);
				}

				if (component == O3DS::TRotation) {
					components.push_back(O3DS::Data::Component::Component_Rotation);
				}

				if (component == O3DS::TScale) {
					components.push_back(O3DS::Data::Component::Component_Scale);
				}

				if (component == O3DS::TMatrix) {
					components.push_back(O3DS::Data::Component::Component_Matrix);
				}
			}

			for (auto& m : t->matrices) {
				// Copy all matrices.  This allows embedding other data (offsets)
				O3DS::Data::Matrix matrix;
				t->matrices[matrixId++] >> matrix;
				matrices.push_back(matrix);
			}

			auto oTransformName = builder.CreateString(t->mName);

			// flatbuffers::Offset<flatbuffers::Vector<const O3DS::Data::Matrix *>> oatrices;
			auto ovMatrices = builder.CreateVectorOfStructs(matrices);

			auto ovComponents = builder.CreateVector(components);

			ovSkeleton.push_back(CreateTransform(builder, t->mParentId, oTransformName,
											&translation, &rotation, &scale,
											ovMatrices, ovComponents));
		}

		auto transforms = builder.CreateVector(ovSkeleton);
			// Serialize curves if present
				auto curves = SerializeCurves(builder);
				return CreateSubject(builder, transforms, oSubjectName,
						dir(this->mContext.mX), dir(this->mContext.mY),
						dir(this->mContext.mZ), oFormat, curves);
	}


flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<O3DS::Data::Curve>>> Subject::SerializeCurves(flatbuffers::FlatBufferBuilder& builder)
{
	std::vector<flatbuffers::Offset<O3DS::Data::Curve>> out;
	for (size_t i = 0; i < mCurveNames.size(); ++i) {
		auto name = builder.CreateString(mCurveNames[i]);
		out.push_back(CreateCurve(builder, name, mCurveValues[i]));
	}
	return builder.CreateVector(out);
}

flatbuffers::Offset<flatbuffers::Vector<const O3DS::Data::CurveUpdate *>> Subject::SerializeCurveUpdates(flatbuffers::FlatBufferBuilder& builder, size_t &count)
{
	std::vector<O3DS::Data::CurveUpdate> out;
	for (size_t i = 0; i < mCurveNames.size(); ++i) {
		// Always send curves for now; delta optimization can be added later
		out.push_back(O3DS::Data::CurveUpdate(mCurveValues[i], (int)i));
		count++;
	}
	return builder.CreateVectorOfStructs(out);
}

	PoseSample Subject::ToPoseSample(double t, uint64_t seq) const
	{
		PoseSample s;
		s.t = t;
		s.seq = seq;

		s.translations.reserve(mTransforms.mItems.size());
		s.rotations.reserve(mTransforms.mItems.size());
		s.scales.reserve(mTransforms.mItems.size());
		for (Transform* const& transform : mTransforms.mItems)
		{
			s.translations.push_back(transform->translation.value);
			s.rotations.push_back(transform->rotation.value);
			s.scales.push_back(transform->scale.value);
		}

		s.curves = mCurveValues;

		return s;
	}

	flatbuffers::Offset<O3DS::Data::SubjectUpdate> Subject::SerializeUpdate(flatbuffers::FlatBufferBuilder& builder, size_t &count, double deltaThreshold, const QuantRanges* quantRanges)
	{
		auto oSubjectName = builder.CreateString(this->mName);

		std::vector<O3DS::Data::TranslationUpdate> translations;
		std::vector<O3DS::Data::TranslationUpdateQ8> translationsQ8;
		std::vector<O3DS::Data::TranslationUpdateQ16> translationsQ16;
		std::vector<O3DS::Data::RotationUpdate> rotations;
		std::vector<O3DS::Data::RotationUpdateQ8> rotationsQ8;
		std::vector<O3DS::Data::RotationUpdateQ16> rotationsQ16;
		std::vector<O3DS::Data::ScaleUpdate> scales;
		std::vector<flatbuffers::Offset<O3DS::Data::CurveUpdate>> curveUpdates;

		int transformId = 0;

		for (const auto& t : this->mTransforms)
		{
			if (t->nan())
			{
				continue;
			}
			if (t->translation.delta() > deltaThreshold)
			{
				bool quantized = false;
				// D1 (roadmap doc §6/D1): quantize a delta from this
				// Transform's fixed rest-pose anchor, never from
				// lastSentValue - see Transform::mQuantAnchorTranslation's
				// doc comment for why a moving reference would reintroduce
				// loss-sensitivity D1 is meant to avoid. No anchor yet
				// (never seen a full Serialize()) means no safe delta to
				// quantize - fall back to Full for this call only.
				if (quantRanges != nullptr && t->mQuantAnchorSet)
				{
					double dx = t->translation.value.v[0] - t->mQuantAnchorTranslation.v[0];
					double dy = t->translation.value.v[1] - t->mQuantAnchorTranslation.v[1];
					double dz = t->translation.value.v[2] - t->mQuantAnchorTranslation.v[2];
					double maxAbs = std::max(std::fabs(dx), std::max(std::fabs(dy), std::fabs(dz)));
					// Hysteresis-aware (see quant/channel_quant.h): re-evaluating
					// against a stateless boundary test every frame flaps tiers
					// (and thus reconstruction precision) whenever maxAbs hovers
					// near byteRange/halfRange - exactly what continuous,
					// small-amplitude motion (e.g. idle-animation sway around
					// the rest-pose anchor) does.
					QuantTier tier = ChooseScalarTierWithHysteresis(maxAbs, *quantRanges, t->mLastTranslationTier);
					t->mLastTranslationTier = tier;
					if (tier == QuantTier::Byte)
					{
						translationsQ8.push_back(O3DS::Data::TranslationUpdateQ8(
							QuantizeByte(dx, quantRanges->byteRange),
							QuantizeByte(dy, quantRanges->byteRange),
							QuantizeByte(dz, quantRanges->byteRange),
							transformId));
						quantized = true;
					}
					else if (tier == QuantTier::Half)
					{
						translationsQ16.push_back(O3DS::Data::TranslationUpdateQ16(
							QuantizeHalf(dx, quantRanges->halfRange),
							QuantizeHalf(dy, quantRanges->halfRange),
							QuantizeHalf(dz, quantRanges->halfRange),
							transformId));
						quantized = true;
					}
				}
				else if (quantRanges != nullptr)
				{
					// No anchor yet - the same "fall back to Full for this call
					// only" case the comment above describes. Reset the
					// hysteresis state too, so a real anchor arriving later
					// starts unbiased rather than inheriting a stale tier.
					t->mLastTranslationTier = QuantTier::Full;
				}
				if (!quantized)
				{
					translations.push_back(O3DS::Data::TranslationUpdate(
						(float)t->translation.value.v[0],
						(float)t->translation.value.v[1],
						(float)t->translation.value.v[2], transformId));
				}
				t->translation.sent();
				count++;
			}

			if (t->rotation.delta() > deltaThreshold)
			{
				bool quantized = false;
				// Rotation quantizes the ABSOLUTE value (smallest-three) -
				// unlike translation, unit-quaternion components are
				// already bounded, so no anchor is needed. Reuses
				// quantRanges' byteRange/halfRange as a generic "how much
				// did this channel move" threshold against the same
				// quaternion-space delta() already computed above - a
				// classical fixed-threshold choice, not dimensionally tied
				// to translation's own linear units.
				if (quantRanges != nullptr)
				{
					// Hysteresis-aware for the same reason as translation above -
					// t->rotation.delta() (frame-to-last-sent quaternion distance)
					// oscillates at a roughly constant magnitude during continuous
					// low-amplitude motion (idle sway/look-around), and re-picking
					// the tier from scratch every frame flaps it whenever that
					// magnitude hovers near byteRange/halfRange.
					QuantTier tier = ChooseScalarTierWithHysteresis(t->rotation.delta(), *quantRanges, t->mLastRotationTier);
					t->mLastRotationTier = tier;
					if (tier == QuantTier::Byte)
					{
						SmallestThreeQ8 q = QuantizeRotationByte(t->rotation.value);
						rotationsQ8.push_back(O3DS::Data::RotationUpdateQ8(q.droppedIndex, q.a, q.b, q.c, transformId));
						quantized = true;
					}
					else if (tier == QuantTier::Half)
					{
						SmallestThreeQ16 q = QuantizeRotationHalf(t->rotation.value);
						rotationsQ16.push_back(O3DS::Data::RotationUpdateQ16(q.droppedIndex, q.a, q.b, q.c, transformId));
						quantized = true;
					}
				}
				if (!quantized)
				{
					rotations.push_back(O3DS::Data::RotationUpdate(
						(float)t->rotation.value.v[0],
						(float)t->rotation.value.v[1],
						(float)t->rotation.value.v[2],
						(float)t->rotation.value.v[3], transformId));
				}
				t->rotation.sent();
				count++;
			}

			/*
		if (t->scale.delta() > 0.001)
		{
			scales.push_back(O3DS::Data::ScaleUpdate(
				(float)t->scale.value.v[0],
				(float)t->scale.value.v[1],
				(float)t->scale.value.v[2], transformId));
			t->scale.sent();
		}*/

			transformId++;
		}

		auto tr = builder.CreateVectorOfStructs(translations);
		auto ro = builder.CreateVectorOfStructs(rotations);
		auto sc = builder.CreateVectorOfStructs(scales);
		auto cu = SerializeCurveUpdates(builder, count);
		// CreateVectorOfStructs() always writes a (non-null-offset) vector,
		// even an empty one - it is NOT equivalent to passing 0. Since these
		// four are only ever populated when quantization is enabled AND a
		// channel actually chose that tier, an unconditional call here would
		// add real wire bytes for all four EVERY update regardless of
		// whether quantization is used at all - directly contradicting the
		// "byte-for-byte identical when disabled" guarantee this feature is
		// supposed to have (confirmed empirically: ~48 bytes of pure
		// overhead per update with quantization off before this guard).
		auto trQ8 = translationsQ8.empty() ? 0 : builder.CreateVectorOfStructs(translationsQ8);
		auto trQ16 = translationsQ16.empty() ? 0 : builder.CreateVectorOfStructs(translationsQ16);
		auto roQ8 = rotationsQ8.empty() ? 0 : builder.CreateVectorOfStructs(rotationsQ8);
		auto roQ16 = rotationsQ16.empty() ? 0 : builder.CreateVectorOfStructs(rotationsQ16);

		// Only non-zero when actually needed to decode something in THIS
		// update: rotation's smallest-three quantization needs no range at
		// all (see RotationUpdateQ8/16's own doc comment), so an update
		// with only quantized rotation channels (or none quantized at all)
		// must not carry a stale non-zero range - that would contradict the
		// schema's "0 == not used this update" contract and waste 4-8
		// bytes for nothing.
		const float byteRangeOut = translationsQ8.empty() ? 0.0f : (float)quantRanges->byteRange;
		const float halfRangeOut = translationsQ16.empty() ? 0.0f : (float)quantRanges->halfRange;

		return CreateSubjectUpdate(builder, oSubjectName, tr, ro, sc, cu,
			/*predictor_id*/0, /*is_keyframe*/false,
			byteRangeOut, halfRangeOut,
			trQ8, trQ16, roQ8, roQ16, /*curves_q8*/0, /*curves_q16*/0);
	}

	flatbuffers::Offset<O3DS::Data::SubjectUpdate> Subject::SerializeUpdateResidual(flatbuffers::FlatBufferBuilder& builder, size_t& count, double deltaThreshold, double t, uint64_t seq)
	{
		if (!mResidualEncoder)
		{
			// No encoder configured for this subject - behave exactly like
			// the legacy path (predictor_id defaults to 0/None on the wire).
			return SerializeUpdate(builder, count, deltaThreshold);
		}

		const PoseSample actual = ToPoseSample(t, seq);
		mResidualEncoder->BeginFrame(actual);

		const bool isKeyframe = mResidualEncoder->IsKeyframe();
		const PoseSample& reference = mResidualEncoder->Reference(); // empty when isKeyframe (see below)

		auto oSubjectName = builder.CreateString(this->mName);

		std::vector<O3DS::Data::TranslationUpdate> translations;
		std::vector<O3DS::Data::RotationUpdate> rotations;
		std::vector<O3DS::Data::ScaleUpdate> scales; // scale updates are unsent today (see legacy SerializeUpdate's commented-out block) - residual mode preserves that, nothing to generalize here

		// The exact pose a receiver will end up with, built alongside the
		// wire entries below: `actual`'s value for every channel this
		// frame sends (the residual round-trips it exactly), or
		// `reference`'s (the receiver's own prediction) for every channel
		// this frame omits. Fed to Commit() below instead of `actual`
		// itself - see ResidualEncoder's own doc comment for why
		// observing anything else would let the encoder's and decoder's
		// predictor history silently diverge.
		PoseSample reconstructed = actual;

		// Bounds-checked defensively: `reference` only matches mTransforms'
		// current topology if it hasn't changed since the last
		// ResidualEncoder::BeginFrame() - which itself now detects a
		// topology change and forces isKeyframe in that case, so this is
		// just defense in depth, not the primary safeguard.
		const size_t refTransCount = reference.translations.size();
		const size_t refRotCount = reference.rotations.size();

		int transformId = 0;
		for (const auto& tform : this->mTransforms)
		{
			const Vector3d refTrans = ((size_t)transformId < refTransCount) ? reference.translations[transformId] : Vector3d(0.0, 0.0, 0.0);
			const Quat refRot = ((size_t)transformId < refRotCount) ? reference.rotations[transformId] : Quat(0.0, 0.0, 0.0, 0.0);

			if (tform->nan())
			{
				// `reconstructed` started as a copy of `actual`, which
				// carries this channel's NaN straight from ToPoseSample()
				// (nan() isn't checked there). Committing a NaN into the
				// predictor's history would poison every later
				// Predict()/Observe() for this subject via ordinary
				// floating-point NaN propagation - fall back to the
				// reference value instead, exactly like a genuinely
				// omitted (unsent) channel.
				reconstructed.translations[transformId] = refTrans;
				reconstructed.rotations[transformId] = refRot;
				transformId++;
				continue;
			}

			// A keyframe must include every channel unconditionally (it
			// exists to fully resync a receiver joining mid-stream or
			// recovering from loss - omitting a channel just because it's
			// near the zero reference would defeat that), so only the
			// deltaThreshold gate applies to residual (non-keyframe) frames.
			if (isKeyframe || dist(tform->translation.value, refTrans) > deltaThreshold)
			{
				// Round-trip through float explicitly (not just at
				// serialization time) so `reconstructed` - what gets
				// Commit()'d into the predictor's history - matches
				// exactly what the receiver reconstructs (ref + the
				// float-precision residual actually on the wire), not the
				// sender's own full double-precision `actual`. Committing
				// anything more precise would desync the two sides'
				// history the moment this channel is next predicted from.
				const float residualX = (float)(tform->translation.value.v[0] - refTrans.v[0]);
				const float residualY = (float)(tform->translation.value.v[1] - refTrans.v[1]);
				const float residualZ = (float)(tform->translation.value.v[2] - refTrans.v[2]);
				translations.push_back(O3DS::Data::TranslationUpdate(residualX, residualY, residualZ, transformId));
				count++;
				reconstructed.translations[transformId] = Vector3d(
					refTrans.v[0] + (double)residualX,
					refTrans.v[1] + (double)residualY,
					refTrans.v[2] + (double)residualZ);
			}
			else
			{
				reconstructed.translations[transformId] = refTrans;
			}

			if (isKeyframe || dist(tform->rotation.value, refRot) > deltaThreshold)
			{
				const float residualX = (float)(tform->rotation.value.v[0] - refRot.v[0]);
				const float residualY = (float)(tform->rotation.value.v[1] - refRot.v[1]);
				const float residualZ = (float)(tform->rotation.value.v[2] - refRot.v[2]);
				const float residualW = (float)(tform->rotation.value.v[3] - refRot.v[3]);
				rotations.push_back(O3DS::Data::RotationUpdate(residualX, residualY, residualZ, residualW, transformId));
				count++;
				reconstructed.rotations[transformId] = Quat(
					refRot.v[0] + (double)residualX,
					refRot.v[1] + (double)residualY,
					refRot.v[2] + (double)residualZ,
					refRot.v[3] + (double)residualW);
			}
			else
			{
				reconstructed.rotations[transformId] = refRot;
			}

			transformId++;
		}

		// Curves: always sent unconditionally today (see
		// SerializeCurveUpdates) - never omitted, so `reconstructed.curves`
		// (== `actual.curves`, copied above) needs no adjustment here.
		std::vector<O3DS::Data::CurveUpdate> curveUpdates;
		const size_t refCurveCount = reference.curves.size();
		for (size_t i = 0; i < mCurveValues.size(); ++i)
		{
			const float refCurve = (i < refCurveCount) ? reference.curves[i] : 0.0f;
			curveUpdates.push_back(O3DS::Data::CurveUpdate(mCurveValues[i] - refCurve, (int)i));
			count++;
		}

		mResidualEncoder->Commit(reconstructed);

		auto tr = builder.CreateVectorOfStructs(translations);
		auto ro = builder.CreateVectorOfStructs(rotations);
		auto sc = builder.CreateVectorOfStructs(scales);
		auto cu = builder.CreateVectorOfStructs(curveUpdates);

		return CreateSubjectUpdate(builder, oSubjectName, tr, ro, sc, cu,
			static_cast<uint32_t>(mResidualEncoder->Id()), isKeyframe);
	}

	int Subject::Serialize(std::vector<char> &outbuf, double timestamp)
	{
		if (timestamp == 0.0) timestamp = GetTime();
		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::Subject> > subjects;
		
		flatbuffers::Offset<O3DS::Data::Subject> s = this->Serialize(builder);
		subjects.push_back(s);

		auto ovSubjects = builder.CreateVector(subjects);

		auto root = CreateSubjectList(builder, ovSubjects, 0, timestamp);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int Subject::SerializeUpdate(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp, const QuantRanges* quantRanges)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;
		outSubjectUpdates.push_back(this->SerializeUpdate(builder, count, deltaThreshold, quantRanges));

		auto ovSubjectUpdates = builder.CreateVector(outSubjectUpdates);

		auto root = CreateSubjectList(builder, 0, ovSubjectUpdates, timestamp);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int Subject::SerializeUpdateResidual(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp, uint64_t seq)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;
		outSubjectUpdates.push_back(this->SerializeUpdateResidual(builder, count, deltaThreshold, timestamp, seq));

		auto ovSubjectUpdates = builder.CreateVector(outSubjectUpdates);

		// `seq` must also reach the root SubjectList's own tx_seq field, not
		// just PoseSample::seq (fed to the predictor above) - a caller
		// passing a real A1 tx_seq expects it on the wire for the
		// receiver's ReorderGate, exactly like SubjectList::SerializeUpdateResidual
		auto root = CreateSubjectList(builder, 0, ovSubjectUpdates, timestamp, seq);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int SubjectList::Serialize(std::vector<char> &outbuf, double timestamp,
		uint64_t tx_seq, uint64_t tx_wallclock_us, uint32_t frame_epoch)
	{
		if(timestamp == 0.0) timestamp = GetTime();

		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::Subject> > subjects;

		for (O3DS::Subject* subject : this->mItems)
		{
			flatbuffers::Offset<O3DS::Data::Subject> s = subject->Serialize(builder);
			subjects.push_back(s);
		}

		auto ovSubjects = builder.CreateVector(subjects);

		auto root = CreateSubjectList(builder, ovSubjects, 0, timestamp, tx_seq, tx_wallclock_us, frame_epoch);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	void finalize(flatbuffers::FlatBufferBuilder& builder, std::vector<char>& outbuf, std::uint32_t flags)
	{
		const uint8_t* buf = builder.GetBufferPointer();
		const size_t size = builder.GetSize();

		// Header (flags, then CRC-32 of the payload, both in host byte order
		// as before), then the payload, written in place: one resize, which
		// keeps outbuf's capacity when the caller reuses it (CORE-18).
		const std::uint32_t crc = Crc32(buf, size);
		outbuf.resize(8 + size);
		std::memcpy(outbuf.data(), &flags, 4);
		std::memcpy(outbuf.data() + 4, &crc, 4);
		std::memcpy(outbuf.data() + 8, buf, size);
	}



	// Subject List

	int SubjectList::SerializeUpdate(std::vector<char> &outbuf, size_t& count, double timestamp,
		uint64_t tx_seq, uint64_t tx_wallclock_us, uint32_t frame_epoch)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;

		const QuantRanges* quantRanges = mQuantizationEnabled ? &mQuantRanges : nullptr;
		for (auto& subject : this->mItems)
		{
			outSubjectUpdates.push_back(subject->SerializeUpdate(builder, count, mDeltaThreshold, quantRanges));
		}

		auto ovSubjectUpdates = builder.CreateVector(outSubjectUpdates);

		auto root = CreateSubjectList(builder, 0, ovSubjectUpdates, timestamp, tx_seq, tx_wallclock_us, frame_epoch);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int SubjectList::SerializeUpdateResidual(std::vector<char> &outbuf, size_t& count, double timestamp,
		uint64_t tx_seq, uint64_t tx_wallclock_us, uint32_t frame_epoch)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder& builder = ReusableBuilder();

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;

		for (auto& subject : this->mItems)
		{
			outSubjectUpdates.push_back(subject->SerializeUpdateResidual(builder, count, mDeltaThreshold, timestamp, tx_seq));
		}

		auto ovSubjectUpdates = builder.CreateVector(outSubjectUpdates);

		auto root = CreateSubjectList(builder, 0, ovSubjectUpdates, timestamp, tx_seq, tx_wallclock_us, frame_epoch);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	bool SubjectList::PeekMeta(const char* data, size_t len,
		uint64_t& outTxSeq, uint64_t& outTxWallclockUs, uint32_t& outFrameEpoch)
	{
		outTxSeq = 0;
		outTxWallclockUs = 0;
		outFrameEpoch = 0;

		// Header is 8 bytes (flags + CRC) followed by the FlatBuffers payload;
		// reject anything too short before doing arithmetic on len or
		// dereferencing data (len - 8 would otherwise underflow).
		if (data == nullptr || len < 8)
			return false;

		flatbuffers::Verifier verifier(
			reinterpret_cast<const uint8_t*>(data + 8), len - 8);
		if (!O3DS::Data::VerifySubjectListBuffer(verifier))
			return false;

		auto root = GetSubjectList(data + 8);
		if (root == nullptr)
			return false;

		outTxSeq = root->tx_seq();
		outTxWallclockUs = root->tx_wallclock_us();
		outFrameEpoch = root->frame_epoch();
		return true;
	}

	bool SubjectList::Parse(const char *data, size_t len, TransformBuilder *builder, bool clearInactive,
		std::vector<ParsedSubjectInfo>* outTouched)
	{
		mError = "";
		if (outTouched)
			outTouched->clear();

		// Records a subject this packet applied data to (RCV-5). A full
		// descriptor wins over an update for the same subject in one packet.
		auto markTouched = [outTouched](const std::string& name, bool full)
		{
			if (outTouched == nullptr)
				return;
			for (ParsedSubjectInfo& info : *outTouched)
			{
				if (info.name == name)
				{
					info.fullDescriptor = info.fullDescriptor || full;
					return;
				}
			}
			outTouched->push_back(ParsedSubjectInfo{ name, full });
		};

		// Header is 8 bytes (flags + CRC) followed by the FlatBuffers payload;
		// reject anything too short before doing any arithmetic on len or
		// dereferencing data, since len - 8 would otherwise underflow.
		if (data == nullptr || len < 8)
		{
			mError = "Buffer too short";
			return false;
		}

		std::uint32_t crc = Crc32(data + 8, len - 8);

		std::uint32_t flags = *(std::uint32_t*)data;
		std::uint32_t check = *(std::uint32_t*)(data + 4);

		if (flags != 0x0001) {
			mError = "Invalid data structure";
			return false;
		}

		if (crc != check) {
			mError = "CRC Check failed";
			return false;
		}

		// The CRC only proves the payload wasn't corrupted in transit, not that
		// it is well-formed FlatBuffers data (this is untrusted network input).
		// Verify the buffer before trusting any offsets in it.
		flatbuffers::Verifier verifier(
			reinterpret_cast<const uint8_t*>(data + 8), len - 8);
		if (!O3DS::Data::VerifySubjectListBuffer(verifier))
		{
			mError = "FlatBuffers verification failed";
			return false;
		}

		auto root = GetSubjectList(data+8);

		// WP-S1 (CORE-9): reject a non-finite timestamp and over-limit
		// counts before anything in this list is touched.
		if (!Finite(root->time()))
		{
			mError = "Non-finite time";
			return false;
		}

		auto subjects_data = root->subjects();
		auto updates_data = root->updates();

		if ((subjects_data && subjects_data->size() > ParseLimits::kMaxSubjects)
			|| (updates_data && updates_data->size() > ParseLimits::kMaxSubjects))
		{
			mError = "Too many subjects in buffer";
			return false;
		}

		this->mTime = root->time();

		if (subjects_data)
		{
			// Clear the list before populating. mItems owns these Subject
			// pointers (~SubjectList deletes them the same way), so a bare
			// vector::clear() here would leak every previously-tracked
			// subject instead of dropping it - delete them first, matching
			// the pattern TransformList::clear()/Subject::clear() already
			// use for their own owned pointers.
			if (clearInactive) {
				for (Subject* s : this->mItems)
				{
					delete s;
				}
				this->mItems.clear();
			}
			for (uint32_t i = 0; i < subjects_data->size(); i++)
			{
				// For each subject. ParseSubject reports a rejected subject
				// through mError (its signature predates validation).
				auto inSubject = subjects_data->Get(i);
				this->ParseSubject(inSubject, builder);
				if (!mError.empty())
					return false;
				// ParseSubject skips a nameless subject without an error.
				if (inSubject->name() != nullptr)
					markTouched(inSubject->name()->str(), true);
			}
		}

		if (updates_data)
		{
			for (uint32_t i = 0; i < updates_data->size(); i++)
			{
				// For each update - dispatch on the wire's own predictor_id
				// (C2, roadmap doc §5/C2): 0/None is exactly today's legacy
				// last-pose-delta format (old senders never set this
				// field, so it defaults to 0), everything else is
				// residual-coded. Mixed legacy/residual subjects in the
				// same SubjectList are fine - this is a per-update, not
				// per-buffer, decision.
				auto inUpdate = updates_data->Get(i);
				const bool applied = (inUpdate->predictor_id() != 0)
					? this->ParseUpdateResidual(inUpdate, builder)
					: this->ParseUpdate(inUpdate, builder);
				if (!mError.empty())
					return false;
				// An update for an unknown subject, or one that could not
				// be decoded, is skipped and not reported.
				if (applied)
					markTouched(inUpdate->name()->str(), false);
			}
		}

		for (auto subject : this->mItems) {
			if(!subject->CalcMatrices(mComputeWorldMatrices)) {
				mError = subject->mError;
				return false;
			}
		}

		return true;
	}

	void SubjectList::ParseSubject(const O3DS::Data::Subject *inSubject,  TransformBuilder *builder )
	{
		// This buffer has passed FlatBuffers Verifier, so offsets are safe to
		// follow, but none of these table/string fields are marked `required`
		// in the schema, so a well-formed sender can still legitimately (or a
		// malicious one deliberately) omit them. Skip anything we can't parse
		// rather than dereferencing a null field.
		if (inSubject->name() == nullptr)
			return;

		std::string subjectName = inSubject->name()->str();

		// WP-S1: validate the whole subject before touching existing state,
		// so a rejected subject leaves the list as it was.
		if (!ValidateSubject(inSubject, mError))
			return;

		// Check to see if this subject already exists
		Subject *outSubject = this->findSubject(subjectName);
		if (outSubject == nullptr)
		{
			// Cap subjects held across buffers too (Parse() with
			// clearInactive=false keeps earlier ones).
			if (this->mItems.size() >= ParseLimits::kMaxSubjects)
			{
				mError = "Too many subjects";
				return;
			}

			// Add it
			outSubject = this->addSubject(subjectName);
		}

		outSubject->mContext.mX = dir(inSubject->x_axis());
		outSubject->mContext.mY = dir(inSubject->y_axis());
		outSubject->mContext.mZ = dir(inSubject->z_axis());
		outSubject->mContext.mFormat = (inSubject->format() != nullptr) ? inSubject->format()->str() : std::string();

		// Parse curves if present
		if (inSubject->curves()) {
			outSubject->mCurveNames.clear();
			outSubject->mCurveValues.clear();
			for (auto each : *inSubject->curves()) {
				if (each == nullptr || each->name() == nullptr)
					continue;
				outSubject->mCurveNames.push_back(each->name()->str());
				outSubject->mCurveValues.push_back(each->value());
			}
		}

		// Get the nodes (transforms) for this subject
		auto ovNodes = inSubject->nodes();

		// D1 quantization anchors are re-captured from this full sync (see
		// the Component_Translation branch below): the sender re-anchors at
		// every full sync too (Subject::Serialize, ADR 0005 (vii)), so no
		// anchor is carried over from the transforms deleted here.

		// Clear the subject and add the transforms
		outSubject->clear();

		// C2 (roadmap doc §5/C2): a full subject (re)sync means the
		// topology may have changed - a residual decoder's history is
		// indexed by the OLD transform/curve layout, and reusing it
		// against a possibly different one could silently misapply one
		// channel's prediction onto a different channel. Drop it;
		// ParseUpdateResidual constructs a fresh one on next use with no
		// history - the same "insufficient history -> keyframe" fallback
		// it already has to handle a subject's very first residual frame.
		// (The sender-side encoder doesn't need this: it detects a
		// topology change itself, from ToPoseSample()'s own channel
		// counts, every BeginFrame() - see ResidualEncoder::BeginFrame.)
		outSubject->SetResidualDecoder(nullptr);

		if (ovNodes == nullptr)
			return;

		for (int n = 0; n < (int)ovNodes->size(); n++)
		{
			// ValidateSubject() has rejected a null node.
			auto inNode = ovNodes->Get(n);

			// RCV-14: a nameless node used to be skipped, which shifted the
			// index of every later node, so their parent ids (indices into
			// the wire's node list) pointed at the wrong transform. Keep the
			// node and give it a placeholder name, so indices stay aligned.
			auto inName = inNode->name();

			auto inTranslation = inNode->translation();
			auto inRotation = inNode->rotation();
			auto inScale = inNode->scale();
			auto inMatrix = inNode->matrix();
			auto inComponents = inNode->components();

			std::string transformName = (inName != nullptr)
				? inName->str()
				: UnnamedTransformName(static_cast<size_t>(n));
			Transform *outTransform = outSubject->addTransform(transformName, inNode->parent());

			// Add the components to the transform stack in the order they are defined.
			if (inComponents != nullptr)
			{
				for (int8_t componentId : *inComponents)
				{
					if (componentId == O3DS::Data::Component::Component_Translation && inTranslation != nullptr)
					{
						*inTranslation >> outTransform->translation;
						outTransform->transformOrder.push_back(O3DS::TTranslation);

						// D1: anchor to the value this full sync carries,
						// exactly as the sender did when it wrote it.
						outTransform->mQuantAnchorTranslation = outTransform->translation.value;
						outTransform->mQuantAnchorSet = true;
					}
					if (componentId == O3DS::Data::Component::Component_Rotation && inRotation != nullptr)
					{
						*inRotation >> outTransform->rotation;
						outTransform->transformOrder.push_back(O3DS::TRotation);
					}
					if (componentId == O3DS::Data::Component::Component_Scale && inScale != nullptr)
					{
						*inScale >> outTransform->scale;
						outTransform->transformOrder.push_back(O3DS::TScale);
					}
					if (componentId == O3DS::Data::Component::Component_Matrix)
					{
						// ValidateSubject() above has already rejected a
						// node with more matrix components than matrices
						// (CORE-1), so CalcMatrices() has one for each.
						outTransform->transformOrder.push_back(O3DS::TMatrix);
					}
				}
			}

			// Copy all matrices, allows adding other matrix data to be used as offsets
			if (inMatrix != nullptr)
			{
				for (auto eachMatrix : *inMatrix) {
					auto transformMatrix = O3DS::TransformMatrix();
					*eachMatrix >> transformMatrix;
					outTransform->matrices.push_back(transformMatrix);
				}
			}
		}
	}

	bool SubjectList::ParseUpdate(
		const O3DS::Data::SubjectUpdate *inUpdate,
		TransformBuilder *builder)
	{
		// name/translations/rotation/scale are all optional (non-
		// `required`) fields in the schema, same as curves() below - a
		// well-formed sender can legitimately omit any of them, and this
		// is untrusted network input, so dereferencing them
		// unconditionally is a crash waiting to happen.
		if (inUpdate->name() == nullptr)
			return false;

		// WP-S1 (CORE-9): reject the update before applying any of it.
		if (!ValidateUpdateFloats(inUpdate, mError))
			return false;

		std::string name = inUpdate->name()->str();
		int id;

		// Find the subject to update, by name
		O3DS::Subject *outSubject = this->findSubject(name);
		if (!outSubject)
			return false;

		// Update TRS. CORE-23: every index is validated and an out-of-range
		// one (negative or too large) skips just that entry, the same rule
		// the quantized branches below always used.
		const size_t transformCount = outSubject->mTransforms.size();

		if (inUpdate->translations()) {
			for (auto inTranslation : *inUpdate->translations())
			{
				id = inTranslation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				*inTranslation >> outSubject->mTransforms[(size_t)id]->translation;
			}
		}

		// D1 (roadmap doc §6/D1): quantized translation deltas are relative
		// to this Transform's rest-pose anchor (see Transform::
		// mQuantAnchorTranslation's own doc comment) - NOT the value
		// currently sitting in translation.value, which the plain
		// translations() loop above may not even have touched this frame.
		// A Transform with no anchor yet (its first-ever full sync hasn't
		// happened) has nothing safe to reconstruct against and is skipped;
		// this shouldn't occur in practice with a matching sender - both
		// sides always anchor at the same full sync (see Subject::Serialize
		// and ParseSubject) - but a corrupt/adversarial buffer could still
		// carry these fields for a channel that doesn't have one.
		if (inUpdate->translations_q8()) {
			const double byteRange = inUpdate->quant_byte_range();
			for (auto inTranslation : *inUpdate->translations_q8())
			{
				id = inTranslation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				Transform* xf = outSubject->mTransforms[(size_t)id];
				if (!xf->mQuantAnchorSet)
					continue;
				xf->translation = O3DS::TransformTranslation(
					xf->mQuantAnchorTranslation.v[0] + DequantizeByte(inTranslation->dx(), byteRange),
					xf->mQuantAnchorTranslation.v[1] + DequantizeByte(inTranslation->dy(), byteRange),
					xf->mQuantAnchorTranslation.v[2] + DequantizeByte(inTranslation->dz(), byteRange));
			}
		}

		if (inUpdate->translations_q16()) {
			const double halfRange = inUpdate->quant_half_range();
			for (auto inTranslation : *inUpdate->translations_q16())
			{
				id = inTranslation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				Transform* xf = outSubject->mTransforms[(size_t)id];
				if (!xf->mQuantAnchorSet)
					continue;
				xf->translation = O3DS::TransformTranslation(
					xf->mQuantAnchorTranslation.v[0] + DequantizeHalf(inTranslation->dx(), halfRange),
					xf->mQuantAnchorTranslation.v[1] + DequantizeHalf(inTranslation->dy(), halfRange),
					xf->mQuantAnchorTranslation.v[2] + DequantizeHalf(inTranslation->dz(), halfRange));
			}
		}

		if (inUpdate->rotation()) {
			for (auto inRotation : *inUpdate->rotation())
			{
				id = inRotation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				*inRotation >> outSubject->mTransforms[(size_t)id]->rotation;
			}
		}

		// Quantized rotation is the ABSOLUTE value (smallest-three) - no
		// anchor needed, unlike translation above.
		if (inUpdate->rotations_q8()) {
			for (auto inRotation : *inUpdate->rotations_q8())
			{
				id = inRotation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				SmallestThreeQ8 q;
				q.droppedIndex = inRotation->dropped();
				q.a = inRotation->a();
				q.b = inRotation->b();
				q.c = inRotation->c();
				Quat decoded = DequantizeRotationByte(q);
				outSubject->mTransforms[(size_t)id]->rotation = O3DS::TransformRotation(
					decoded.v[0], decoded.v[1], decoded.v[2], decoded.v[3]);
			}
		}

		if (inUpdate->rotations_q16()) {
			for (auto inRotation : *inUpdate->rotations_q16())
			{
				id = inRotation->i();
				if (!ValidIndex(id, transformCount))
					continue;
				SmallestThreeQ16 q;
				q.droppedIndex = inRotation->dropped();
				q.a = inRotation->a();
				q.b = inRotation->b();
				q.c = inRotation->c();
				Quat decoded = DequantizeRotationHalf(q);
				outSubject->mTransforms[(size_t)id]->rotation = O3DS::TransformRotation(
					decoded.v[0], decoded.v[1], decoded.v[2], decoded.v[3]);
			}
		}

		if (inUpdate->scale()) {
			for (auto inScale : *inUpdate->scale())
			{
				id = inScale->i();
				if (!ValidIndex(id, transformCount))
					continue;
				*inScale >> outSubject->mTransforms[(size_t)id]->scale;
			}
		}

		// Curve updates
		if (inUpdate->curves()) {
			for (auto inCurve : *inUpdate->curves()) {
				id = inCurve->i();
				if (!ValidIndex(id, outSubject->mCurveValues.size()))
					continue;
				outSubject->mCurveValues[(size_t)id] = inCurve->value();
			}
		}
		return true;
	}

	bool SubjectList::ParseUpdateResidual(
		const O3DS::Data::SubjectUpdate *inUpdate,
		TransformBuilder *builder)
	{
		if (inUpdate->name() == nullptr)
			return false;

		// WP-S1 (CORE-9): reject before the decoder or any channel is touched.
		if (!ValidateUpdateFloats(inUpdate, mError))
			return false;

		std::string name = inUpdate->name()->str();

		O3DS::Subject *outSubject = this->findSubject(name);
		if (!outSubject)
			return false;

		const ResidualPredictorId wireId = static_cast<ResidualPredictorId>(inUpdate->predictor_id());

		// predictor_id is untrusted network input - Parse()'s dispatch
		// already guarantees it's nonzero here, but a corrupt or
		// forward-incompatible value (anything this build doesn't
		// recognize) would make MakePredictorForId() return nullptr, and
		// a ResidualDecoder built around a null predictor crashes on its
		// very first BeginFrame()/EndFrame() call. Drop the update rather
		// than construct one, matching ParseSubject's "skip what we can't
		// parse" posture for untrusted input.
		if (wireId != ResidualPredictorId::Hold && wireId != ResidualPredictorId::Linear && wireId != ResidualPredictorId::Quadratic)
			return false;

		O3DS::ResidualDecoder* decoder = outSubject->GetResidualDecoder();
		if (!decoder || decoder->Id() != wireId)
		{
			// First residual frame for this subject, or the sender
			// switched predictors mid-stream - (re)construct a matching
			// decoder. It has no history yet, so BeginFrame() below
			// safely falls back to a zero/identity reference regardless
			// of what is_keyframe says (see ResidualDecoder's own doc
			// comment), same fallback contract as a fresh encoder.
			auto newDecoder = std::make_unique<ResidualDecoder>(wireId);
			decoder = newDecoder.get();
			outSubject->SetResidualDecoder(std::move(newDecoder));
		}

		decoder->BeginFrame(inUpdate->is_keyframe(), this->mTime);
		const PoseSample& reference = decoder->Reference();

		const size_t refTransCount = reference.translations.size();
		const size_t refRotCount = reference.rotations.size();
		const size_t refCurveCount = reference.curves.size();

		// Baseline every channel to its own current prediction before
		// overlaying the sparse wire entries below. This is the crux of
		// why residual coding isn't just "sparse delta with extra math":
		// an OMITTED channel here means "residual ~= 0", i.e. "value ==
		// prediction" - NOT "value is unchanged since it was last sent"
		// the way an omission in the legacy delta scheme means. A moving
		// (but well-predicted) channel's value must still advance every
		// frame even when it never appears on the wire, or it would
		// visibly freeze the instant its residual first drops below
		// deltaThreshold. (On a keyframe, reference is empty/zero - see
		// ResidualDecoder::Reference() - and every real channel is
		// expected to be present on the wire per the encoder's own
		// zero-reference gating, so there is no meaningful baseline to
		// apply here beyond what Vector3d()/Quat()'s own zero defaults
		// already are.)
		for (size_t i = 0; i < refTransCount && i < outSubject->mTransforms.size(); ++i)
			outSubject->mTransforms[i]->translation.value = reference.translations[i];
		for (size_t i = 0; i < refRotCount && i < outSubject->mTransforms.size(); ++i)
			outSubject->mTransforms[i]->rotation.value = reference.rotations[i];
		for (size_t i = 0; i < refCurveCount && i < outSubject->mCurveValues.size(); ++i)
			outSubject->mCurveValues[i] = reference.curves[i];

		int id;
		// translations()/rotation() are optional (non-`required`) fields
		// in the schema, same as curves() below - a well-formed sender
		// can legitimately omit them (e.g. a frame with no translation
		// changes at all), and untrusted network input could omit them
		// deliberately, so dereferencing them unconditionally is a crash.
		if (inUpdate->translations()) {
			for (auto inTranslation : *inUpdate->translations())
			{
				id = inTranslation->i();
				if (id < 0 || (size_t)id >= outSubject->mTransforms.size())
					continue;
				const Vector3d refTrans = ((size_t)id < refTransCount) ? reference.translations[id] : Vector3d(0.0, 0.0, 0.0);
				outSubject->mTransforms[id]->translation = O3DS::TransformTranslation(
					refTrans.v[0] + inTranslation->x(),
					refTrans.v[1] + inTranslation->y(),
					refTrans.v[2] + inTranslation->z());
			}
		}

		if (inUpdate->rotation()) {
			for (auto inRotation : *inUpdate->rotation())
			{
				id = inRotation->i();
				if (id < 0 || (size_t)id >= outSubject->mTransforms.size())
					continue;
				const Quat refRot = ((size_t)id < refRotCount) ? reference.rotations[id] : Quat(0.0, 0.0, 0.0, 0.0);
				outSubject->mTransforms[id]->rotation = O3DS::TransformRotation(
					refRot.v[0] + inRotation->x(),
					refRot.v[1] + inRotation->y(),
					refRot.v[2] + inRotation->z(),
					refRot.v[3] + inRotation->w());
			}
		}

		// Scale: the legacy path doesn't send scale updates at all today
		// (see Subject::SerializeUpdate's commented-out block) - residual
		// mode preserves that, nothing to reconstruct here.

		if (inUpdate->curves()) {
			for (auto inCurve : *inUpdate->curves()) {
				id = inCurve->i();
				if (id < 0 || (size_t)id >= outSubject->mCurveValues.size())
					continue;
				const float refCurve = ((size_t)id < refCurveCount) ? reference.curves[id] : 0.0f;
				outSubject->mCurveValues[id] = refCurve + inCurve->value();
			}
		}

		// Advance the decoder's predictor with the subject's full current
		// pose (changed channels just applied above, plus any unchanged
		// carried-over ones) - same full-state Observe() contract
		// ResidualEncoder::BeginFrame() already relies on.
		decoder->EndFrame(outSubject->ToPoseSample(this->mTime, 0));
		return true;
	}



} // namespace O3DS

/*
void O3DS::Subject::update(bool useWorldMatrix)
{
	for (auto transform : mTransforms)
	{
		transform->update();
	}

	if (useWorldMatrix)
	{
		for (auto transform : mTransforms)
		{
			if (transform->mParentId >= 0)
			{
				transform->mParentInverseMatrix = mTransforms.mItems[transform->mParentId]->mMatrix.Inverse();
			}
		}

		for (auto i : mTransforms)
		{
			O3DS::Matrix<double> transformMatrix;
			if (i->mParentId >= 0)
			{
				transformMatrix = i->mMatrix * i->mParentInverseMatrix;
			}
			else
			{
				transformMatrix = i->mMatrix;
			}
			i->mTranslation = transformMatrix.GetTranslation();
			i->mOrientation = transformMatrix.GetQuaternion();
		}
	}
}*/

