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

#ifndef OPEN3D_STREAM_MODEL_H
#define OPEN3D_STREAM_MODEL_H

#include <vector>
#include <string>

#include "math.h"
#include "context.h"
#include "transform_component.h"
#include "predict/residual_codec.h"
#include "o3ds_generated.h"


namespace O3DS
{
	/*! \class Transform model.h o3ds/model.h */
	//! Defines a single transform with name and parent id reference
	class Transform
	{
	public:
		Transform(const std::string& name, int parentId, void *ref = nullptr);

		Transform(int parentId);

		Transform();

		virtual ~Transform();

		virtual void update() {}
		virtual std::string info() { return std::string(); }

		bool nan();

		TransformTranslation translation;
		TransformRotation    rotation;
		TransformScale       scale;

		Matrixd       mMatrix;
		Matrixd       mWorldMatrix;
		bool          bWorldMatrix;

		std::vector<TransformMatrix> matrices;
		std::vector<enum ComponentType> transformOrder;

		std::string mName;
		int mParentId;

		void *mReference;

	};

	//! Platform specific builder to make a transform object
	class TransformBuilder
	{
	public:
		virtual Transform* build(std::string name, int parentId) = 0;
	};


	/*! \class TransformList model.h o3ds/model.h */
	//! A list (std::vector) of Transform objects
	class TransformList
	{
	private:
		TransformList(const TransformList &other)
		{}

	public:
		TransformList() {}

		//! deletes the transform objects in the list
		virtual ~TransformList()
		{
			for (auto i : mItems)
				delete i;
		}

		//! Returns the number of transforms
		size_t size() { return mItems.size(); }

		//! Delete the transform objects and clear the least.
		void clear()
		{
			for (auto i : mItems)
				delete i;
			mItems.clear();
		}

		void update()
		{
			for (auto i : mItems)
				i->update();
		}

		std::vector <Transform*>::iterator begin() { return mItems.begin(); }
		std::vector <Transform*>::iterator end()   { return mItems.end(); }
		Transform *operator[](size_t id)           { return mItems[id]; }

		std::vector<Transform*> mItems;

		Transform* find(const std::string &name)
		{
			for (auto i = 0; i < mItems.size(); i++)
			{
				if (mItems[i]->mName == name)
					return mItems[i];
			}
			return nullptr;
		}
	};


	/*! \class Subject model.h o3ds/model.h
	 *  The subject can also have a SubjectInfo reference for implementation specific data */
	//! A collection of transforms, with a name
	class Subject
	{
	public:
		Subject(void *info = nullptr) 
			: mReference(info)
		{}

		Subject(std::string name, void *info = nullptr)
			: mName(name)
			, mReference(info) 
		{}

		std::string   mName;
		std::vector<std::string> mJoints;

		std::vector<std::string> mCurveNames;
		std::vector<float>       mCurveValues;

		TransformList mTransforms;
		void*         mReference;
		Context       mContext;
		std::string   mError;

		Transform* addTransform(const std::string& name, int parentId, TransformBuilder *builder = nullptr)
		{
			Transform *ret;
			if (builder) ret = builder->build(name, parentId);
			else         ret = new Transform(name, parentId);
			mTransforms.mItems.push_back(ret);
			return ret;
		}

		void addTransform(Transform* item)
		{
			mTransforms.mItems.push_back(item);
		}

		void clear()
		{
			mTransforms.clear();
		}

		void update()
		{
			mTransforms.update();
		}
		
		size_t size()
		{
			return mTransforms.mItems.size();
		}

		bool CalcMatrices();

		flatbuffers::Offset<O3DS::Data::Subject> Serialize(flatbuffers::FlatBufferBuilder& builder);

		flatbuffers::Offset<O3DS::Data::SubjectUpdate> SerializeUpdate(flatbuffers::FlatBufferBuilder& builder, size_t& count, double deltaThreshold);

		// C2 (roadmap doc §5/C2): opt-in per-subject residual coding. Both
		// members are null by default (legacy mode, unaffected). Whichever
		// role this Subject instance plays, set up the matching one -
		// SetResidualEncoder() before calling SerializeUpdateResidual()
		// (sender), SetResidualDecoder() before a receiver's Parse() sees
		// a residual-coded update for this subject's name (receiver, see
		// SubjectList::ParseUpdateResidual). Nothing stops both being set
		// on the same instance since Subject is shared code for both
		// roles today, but only one is meaningful in practice.
		void SetResidualEncoder(std::unique_ptr<ResidualEncoder> encoder) { mResidualEncoder = std::move(encoder); }
		void SetResidualDecoder(std::unique_ptr<ResidualDecoder> decoder) { mResidualDecoder = std::move(decoder); }
		ResidualEncoder* GetResidualEncoder() const { return mResidualEncoder.get(); }
		ResidualDecoder* GetResidualDecoder() const { return mResidualDecoder.get(); }

		//! Builds a flat PoseSample snapshot of this subject's current
		//! translations/rotations/scales/curves, in mTransforms/
		//! mCurveValues index order - the same order TranslationUpdate::i()
		//! etc. already index into, so residual channel alignment needs no
		//! remapping.
		PoseSample ToPoseSample(double t, uint64_t seq) const;

		//! Residual-coded variant of SerializeUpdate(). Requires
		//! GetResidualEncoder() != nullptr (call SetResidualEncoder()
		//! first) - falls back to the legacy SerializeUpdate() overload
		//! otherwise (predictor_id defaults to 0 on the wire either way,
		//! so this is safe to call unconditionally once an encoder is
		//! wired up). `t`/`seq` become the PoseSample fed to the encoder
		//! and, on Predict() success, the wire's implicit reference time -
		//! callers should pass the same `t` as the enclosing
		//! SubjectList::SerializeUpdateResidual()'s timestamp.
		flatbuffers::Offset<O3DS::Data::SubjectUpdate> SerializeUpdateResidual(flatbuffers::FlatBufferBuilder& builder, size_t& count, double deltaThreshold, double t, uint64_t seq);

		// Curves
		flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<O3DS::Data::Curve>>> SerializeCurves(flatbuffers::FlatBufferBuilder& builder);
		flatbuffers::Offset<flatbuffers::Vector<const O3DS::Data::CurveUpdate *>> SerializeCurveUpdates(flatbuffers::FlatBufferBuilder& builder, size_t &count);

		int Serialize(std::vector<char>& outbuf, double timestamp);

		int SerializeUpdate(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp);

		//! Self-contained residual-coded variant of the vector<char> overload
		//! above, mirroring it exactly (builds its own FlatBufferBuilder and
		//! wraps this one subject's update in a single-subject SubjectList,
		//! rather than requiring a caller-owned builder/SubjectList like the
		//! offset-returning overload above does) - the natural entry point
		//! for a sender that serializes one subject's frame at a time (see
		//! UE glue in Open3DSender). `seq` defaults to 0 (unset); pass a real
		//! tx_seq if this subject's frames flow through A1 sequencing.
		int SerializeUpdateResidual(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp, uint64_t seq = 0);

	private:
		std::unique_ptr<ResidualEncoder> mResidualEncoder;
		std::unique_ptr<ResidualDecoder> mResidualDecoder;
	};

	/*! \class SubjectList model.h o3ds/model.h */
	//!  A collection of subjects.
	class SubjectList
	{
	public:

		SubjectList()
			: mTime(0.0)
			, mDeltaThreshold(std::numeric_limits<double>::min())
		{}

		SubjectList(const SubjectList &other)
			: mTime(0.0)
			, mDeltaThreshold(std::numeric_limits<double>::min())
		{}

		virtual ~SubjectList()
		{
			for (auto i : mItems)
			{
				delete i;
			}
		}

		Subject* addSubject(std::string name, void* ref=nullptr)
		{
			auto s = new Subject(name, ref);
			mItems.push_back(s);
			return s;
		}

		Subject* findSubject(const std::string &name)
		{
			for (auto i : mItems)
			{
				if (i->mName == name)
					return i;
			}
			return nullptr;
		}

		void update()
		{
			for (auto i : mItems)
			{
				i->update();
			}
		}

		std::vector<Subject*> mItems;

		size_t size() { return mItems.size(); }
		std::vector <Subject*>::iterator begin() { return mItems.begin(); }
		std::vector <Subject*>::iterator end() { return mItems.end(); }
		Subject* operator [] (int ref) { return mItems.operator[](ref); }

		double mTime;
		double mDeltaThreshold;
		std::string mError;

		//! Encode all of the items in the subject list as binary data.
		//! tx_seq/tx_wallclock_us/frame_epoch are optional (0 == unset, the
		//! default); a receiver must treat 0 exactly like a sender that
		//! predates these fields. See src/o3ds/sequencing.h for generating
		//! them - callers own a SequenceCounter per outbound stream, this
		//! function does not generate them itself, it only writes what it's
		//! given onto the wire.
		int Serialize(std::vector<char> &outbuf, double timestamp = 0.0,
			uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

		int SerializeUpdate(std::vector<char>& outbuf, size_t& count, double timestamp = 0.0,
			uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

		//! Residual-coded variant of SerializeUpdate() (roadmap doc §5/C2).
		//! Subjects with a residual encoder configured (Subject::
		//! SetResidualEncoder()) are encoded via
		//! Subject::SerializeUpdateResidual(); subjects without one fall
		//! back to the legacy Subject::SerializeUpdate() path unchanged -
		//! safe to mix residual-coded and legacy subjects in the same list.
		int SerializeUpdateResidual(std::vector<char>& outbuf, size_t& count, double timestamp = 0.0,
			uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

		//! Populate or update the subject list with the binary data provided (created by Serialize)
		bool Parse(const char *data, size_t len, TransformBuilder* = nullptr, bool clearInactive = true);

		//! Extract just the transmit-sequencing metadata (tx_seq/
		//! tx_wallclock_us/frame_epoch) from a wire buffer, without doing a
		//! full parse of its subjects/updates. Used by ReorderGate to make
		//! ordering decisions before paying the cost of a full Parse().
		//! Validates buffer length and runs the FlatBuffers Verifier (this
		//! reads untrusted network bytes) but does NOT check the CRC - a
		//! corrupt-but-well-formed-enough buffer that fails CRC is still
		//! caught later, when it's actually delivered and Parse()'d.
		//! Returns false (outputs left at 0, their "unset" value) if the
		//! buffer is too short or fails FlatBuffers verification.
		static bool PeekMeta(const char* data, size_t len,
			uint64_t& outTxSeq, uint64_t& outTxWallclockUs, uint32_t& outFrameEpoch);

		void ParseSubject(const O3DS::Data::Subject*, TransformBuilder* = nullptr);

		void ParseUpdate(const O3DS::Data::SubjectUpdate*, TransformBuilder* = nullptr);

		//! Residual-coded counterpart to ParseUpdate() (roadmap doc §5/C2).
		//! Dispatched automatically from Parse() based on the wire's own
		//! predictor_id (0 -> ParseUpdate(), non-zero -> this) - callers
		//! never need to know in advance whether an incoming stream uses
		//! residual coding. (Re)constructs the named subject's
		//! ResidualDecoder on first use or on a predictor_id change
		//! mid-stream; a fresh decoder has no history, so its own
		//! BeginFrame() safely falls back to a zero/identity reference
		//! regardless of what is_keyframe says (see ResidualDecoder's own
		//! doc comment).
		void ParseUpdateResidual(const O3DS::Data::SubjectUpdate*, TransformBuilder* = nullptr);

		//! Change distance threshold below which O3DS skips transmitting a transform update.
		void SetDeltaThreshold(double newThreshold) { mDeltaThreshold = newThreshold; }

	};

	void finalize(flatbuffers::FlatBufferBuilder& builder, std::vector<char>& outbuf, std::uint32_t flags);


} // O3DS


#endif
