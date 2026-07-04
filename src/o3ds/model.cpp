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
#include "getTime.h"
#include "CRC.h"
#include <algorithm>
#include <iterator>
#include <sstream>

using namespace O3DS::Data;

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

	bool Subject::CalcMatrices()
	{
		for (auto& transform : this->mTransforms)
		{
			transform->bWorldMatrix = false;
			auto &m = transform->mMatrix;
			m = Matrixd();

			if(m.HasNan())
			{
				mError = "Matrix NAN";
				return false;
			}

			int matrixId = 0;

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
					m = transform->matrices[matrixId++].value * m;
				}
			}
		}

		// Calculate world matrix

		// Find the root first
		int rootCount = 0;	
		for(auto transform : this->mTransforms) {
			if (transform->mParentId == -1)
			{
				// No Parent - matrix is world matrix
				transform->mWorldMatrix = transform->mMatrix;
				transform->bWorldMatrix = true;
				rootCount++;
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

		bool done = false;
		while (!done)
		{
			// Assume we are done, and flag as not done when we do work
			done = true;
			for (int transformId = 0; transformId < this->mTransforms.size(); transformId++)
			{
				auto transform = this->mTransforms[transformId];
				if (transform->bWorldMatrix) {
					 continue;
				}

				if (transformId == transform->mParentId)
				{
					std::ostringstream oss;
					oss << "ParentId of " << transform->mName << " points to self (" << transformId << ")";
					mError = oss.str();
					return false;
				}

				if (transform->mParentId < 0 || (size_t)transform->mParentId >= this->mTransforms.size())
				{
					mError = "Invalid Parent Id";
					return false;
				}

				auto& parentTransform = this->mTransforms.mItems[transform->mParentId];
				if (!parentTransform->bWorldMatrix)
				{
					// Parent has not been calculated yet
					continue;
				}

				transform->mWorldMatrix = transform->mMatrix * parentTransform->mWorldMatrix;
				transform->bWorldMatrix = true;
				done = false;
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

	flatbuffers::Offset<O3DS::Data::SubjectUpdate> Subject::SerializeUpdate(flatbuffers::FlatBufferBuilder& builder, size_t &count, double deltaThreshold)
	{
		auto oSubjectName = builder.CreateString(this->mName);

		std::vector<O3DS::Data::TranslationUpdate> translations;
		std::vector<O3DS::Data::RotationUpdate> rotations;
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
				translations.push_back(O3DS::Data::TranslationUpdate(
					(float)t->translation.value.v[0],
					(float)t->translation.value.v[1],
					(float)t->translation.value.v[2], transformId));
				t->translation.sent();
				count++;
			}

			if (t->rotation.delta() > deltaThreshold)
			{
				rotations.push_back(O3DS::Data::RotationUpdate(
					(float)t->rotation.value.v[0],
					(float)t->rotation.value.v[1],
					(float)t->rotation.value.v[2],
					(float)t->rotation.value.v[3], transformId));
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
		return CreateSubjectUpdate(builder, oSubjectName, tr, ro, sc, cu);
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
		flatbuffers::FlatBufferBuilder builder;

		std::vector<flatbuffers::Offset<O3DS::Data::Subject> > subjects;
		
		flatbuffers::Offset<O3DS::Data::Subject> s = this->Serialize(builder);
		subjects.push_back(s);

		auto ovSubjects = builder.CreateVector(subjects);

		auto root = CreateSubjectList(builder, ovSubjects, 0, timestamp);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int Subject::SerializeUpdate(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder builder;

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;
		outSubjectUpdates.push_back(this->SerializeUpdate(builder, count, deltaThreshold));

		auto ovSubjectUpdates = builder.CreateVector(outSubjectUpdates);

		auto root = CreateSubjectList(builder, 0, ovSubjectUpdates, timestamp);

		builder.Finish(root);

		finalize(builder, outbuf, 1);

		return static_cast<int>(outbuf.size());
	}

	int SubjectList::Serialize(std::vector<char> &outbuf, double timestamp,
		uint64_t tx_seq, uint64_t tx_wallclock_us, uint32_t frame_epoch)
	{
		if(timestamp == 0.0) timestamp = GetTime();

		flatbuffers::FlatBufferBuilder builder;

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
		outbuf.resize(0);

		uint8_t* buf = builder.GetBufferPointer();
		int size = builder.GetSize();

		// Flags
		//std::uint32_t flags = 0x0001;
		const char* flagptr = (const char*)&flags;
		std::copy(flagptr, flagptr + 4, back_inserter(outbuf));

		// Checksum
		std::uint32_t crc = CRCPP::CRC::Calculate(buf, size, CRCPP::CRC::CRC_32());
		const char* crcptr = (const char*)&crc;
		std::copy(crcptr, crcptr + 4, back_inserter(outbuf));

		// Data
		std::copy(buf, buf + size, back_inserter(outbuf));
	}



	// Subject List

	int SubjectList::SerializeUpdate(std::vector<char> &outbuf, size_t& count, double timestamp,
		uint64_t tx_seq, uint64_t tx_wallclock_us, uint32_t frame_epoch)
	{
		if (timestamp == 0.0)
		{
			timestamp = GetTime();
		}

		flatbuffers::FlatBufferBuilder builder;

		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> outSubjectUpdates;

		for (auto& subject : this->mItems)
		{
			outSubjectUpdates.push_back(subject->SerializeUpdate(builder, count, mDeltaThreshold));
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

		flatbuffers::FlatBufferBuilder builder;

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

	bool SubjectList::Parse(const char *data, size_t len, TransformBuilder *builder, bool clearInactive)
	{
		mError = "";

		// Header is 8 bytes (flags + CRC) followed by the FlatBuffers payload;
		// reject anything too short before doing any arithmetic on len or
		// dereferencing data, since len - 8 would otherwise underflow.
		if (data == nullptr || len < 8)
		{
			mError = "Buffer too short";
			return false;
		}

		std::uint32_t crc = CRCPP::CRC::Calculate(data + 8, len - 8, CRCPP::CRC::CRC_32());

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

		this->mTime = root->time();

		auto subjects_data = root->subjects();
		auto updates_data = root->updates();

		auto ovSubjects = root->subjects();

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
				// For each subject
				this->ParseSubject(subjects_data->Get(i), builder);
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
				if (inUpdate->predictor_id() != 0)
					this->ParseUpdateResidual(inUpdate, builder);
				else
					this->ParseUpdate(inUpdate, builder);
			}
		}

		for (auto subject : this->mItems) {
			if(!subject->CalcMatrices()) {
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

		// Check to see if this subject already exists
		Subject *outSubject = this->findSubject(subjectName);
		if (outSubject == nullptr)
		{
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
			auto inNode = ovNodes->Get(n);
			if (inNode == nullptr)
				continue;

			auto inName = inNode->name();
			if (inName == nullptr)
				continue;

			auto inTranslation = inNode->translation();
			auto inRotation = inNode->rotation();
			auto inScale = inNode->scale();
			auto inMatrix = inNode->matrix();
			auto inComponents = inNode->components();

			std::string transformName = inName->str();
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

	void SubjectList::ParseUpdate(
		const O3DS::Data::SubjectUpdate *inUpdate,
		TransformBuilder *builder)
	{
		// name/translations/rotation/scale are all optional (non-
		// `required`) fields in the schema, same as curves() below - a
		// well-formed sender can legitimately omit any of them, and this
		// is untrusted network input, so dereferencing them
		// unconditionally is a crash waiting to happen.
		if (inUpdate->name() == nullptr)
			return;
		std::string name = inUpdate->name()->str();
		int id;

		// Find the subject to update, by name
		O3DS::Subject *outSubject = this->findSubject(name);
		if (!outSubject)
			return;

		// Update TRS

		if (inUpdate->translations()) {
			for (auto inTranslation : *inUpdate->translations())
			{
				id = inTranslation->i();
				if (id < outSubject->mTransforms.size())
					*inTranslation >> outSubject->mTransforms[id]->translation;
				else
					break;
			}
		}

		if (inUpdate->rotation()) {
			for (auto inRotation : *inUpdate->rotation())
			{
				id = inRotation->i();
				if (id < outSubject->mTransforms.size())
					*inRotation >> outSubject->mTransforms[id]->rotation;
				else
					break;
			}
		}

		if (inUpdate->scale()) {
			for (auto inScale : *inUpdate->scale())
			{
				id = inScale->i();
				if (id < outSubject->mTransforms.size())
					*inScale >> outSubject->mTransforms[id]->scale;
				else
					break;
			}
		}

		// Curve updates
		if (inUpdate->curves()) {
			for (auto inCurve : *inUpdate->curves()) {
				id = inCurve->i();
				if (id < outSubject->mCurveValues.size()) {
					outSubject->mCurveValues[id] = inCurve->value();
				} else {
					// ignore out-of-range
				}
			}
		}
	}

	void SubjectList::ParseUpdateResidual(
		const O3DS::Data::SubjectUpdate *inUpdate,
		TransformBuilder *builder)
	{
		if (inUpdate->name() == nullptr)
			return;
		std::string name = inUpdate->name()->str();

		O3DS::Subject *outSubject = this->findSubject(name);
		if (!outSubject)
			return;

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
			return;

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

