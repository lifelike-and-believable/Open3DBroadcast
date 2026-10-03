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

#include "o3ds_export.h"
#include "wire_format.h"
#include <vector>
#include <string>
#include <utility>

#include "math.h"
#include "context.h"
#include "transform_component.h"
#include "predict/residual_codec.h"
#include "quant/channel_quant.h"
#include "o3ds_generated.h"


namespace O3DS
{
	/*! \class Transform model.h o3ds/model.h */
	//! Defines a single transform with name and parent id reference
	class O3DS_API Transform
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

		// D1 (roadmap doc §6/D1): LOCAL translation anchor for legacy-mode
		// quantization (Subject::SerializeUpdate/SubjectList::ParseUpdate's
		// Byte/Half tiers - see quant/channel_quant.h). It is the value sent
		// in the most recent full sync, not the last-SENT update value (a
		// moving reference would desync after one dropped update).
		// Re-captured at EVERY full sync on both sides (ADR 0005 (vii)):
		// Subject::Serialize() stores the float32-rounded translation it
		// writes, and ParseSubject() stores the translation it parses, so the
		// two always match after the same full sync, including for a
		// receiver that joins mid-stream. Following the pose also keeps
		// deltas small between periodic full syncs. Until
		// SubjectUpdate.ref_seq exists (WP-A4a), a receiver that misses a
		// full sync decodes quantized deltas against its older anchor until
		// the next full sync reaches it. Large local deviations (e.g. IK
		// stretching, or a root bone whose local space is effectively world
		// space) fall back to the Full tier.
		Vector3d mQuantAnchorTranslation = Vector3d(0.0, 0.0, 0.0);
		bool     mQuantAnchorSet = false;

		// D1 hysteresis: the tier ChooseScalarTierWithHysteresis last chose
		// for this channel (see quant/channel_quant.h) - carried per-Transform,
		// per-channel so the next call can require clearing the *opposite*
		// side of a boundary before switching tiers again, instead of
		// flapping every frame a value hovers near byteRange/halfRange (each
		// tier reconstructs on a different rounding grid, so every flap is a
		// visible jump on the receiver). Sender-only state. Defaults to Full,
		// the always-correct starting point - a fresh Transform has no prior
		// tier to be biased toward.
		QuantTier mLastTranslationTier = QuantTier::Full;
		QuantTier mLastRotationTier = QuantTier::Full;
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
		TransformList(const TransformList &)
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
			for (size_t i = 0; i < mItems.size(); i++)
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
	class O3DS_API Subject
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

		//! Builds every transform's local matrix (mMatrix) from its
		//! transformOrder, validates the hierarchy (exactly one root, parent
		//! ids in range, no self-parenting, no parent cycles) and, when
		//! computeWorldMatrices is true, solves mWorldMatrix in one O(N) pass.
		//! Returns false with mError set on a matrix component that has no
		//! matching entry in `matrices`, a non-finite local matrix, or an
		//! invalid hierarchy. With computeWorldMatrices false, every
		//! transform's bWorldMatrix is left false.
		bool CalcMatrices(bool computeWorldMatrices = true);

		flatbuffers::Offset<O3DS::Data::Subject> Serialize(flatbuffers::FlatBufferBuilder& builder);

		//! `quantRanges` (D1, roadmap doc §6/D1): when non-null, translation/
		//! rotation channels that clear deltaThreshold are additionally
		//! considered for adaptive quantization (Byte/Half tiers) instead of
		//! always being sent at full float32 precision - see quant/
		//! channel_quant.h. Translation quantizes a delta from this
		//! Transform's last-full-sync anchor (see Transform::
		//! mQuantAnchorTranslation's own doc comment for why NOT a moving
		//! last-sent reference); a Transform with no anchor yet (never gone
		//! through a full Serialize()/ParseSubject()) always falls back to
		//! Full for this call. Rotation quantizes the absolute value
		//! (smallest-three, no anchor needed) and reuses the same
		//! byteRange/halfRange thresholds against its own quaternion-space
		//! delta() as a simple, classical tier-selection rule. Default
		//! nullptr (disabled) leaves the wire byte-for-byte identical to
		//! before D1 existed.
		flatbuffers::Offset<O3DS::Data::SubjectUpdate> SerializeUpdate(flatbuffers::FlatBufferBuilder& builder, size_t& count, double deltaThreshold, const QuantRanges* quantRanges = nullptr);

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

		//! The tx_* fields stamp the frame (SubjectList.tx_seq etc.; 0 = unset).
		//! A sender stamps through O3DS::StreamWriter (ADR 0005 (iv)); calling
		//! these without a stamp is deprecated (CORE-29): the frame goes out
		//! unsequenced and a receiver cannot gate, order or conceal it.
		int Serialize(std::vector<char>& outbuf, double timestamp,
			uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

		int SerializeUpdate(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp, const QuantRanges* quantRanges = nullptr,
			uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

		//! Self-contained residual-coded variant of the vector<char> overload
		//! above, mirroring it exactly (builds its own FlatBufferBuilder and
		//! wraps this one subject's update in a single-subject SubjectList,
		//! rather than requiring a caller-owned builder/SubjectList like the
		//! offset-returning overload above does) - the natural entry point
		//! for a sender that serializes one subject's frame at a time (see
		//! UE glue in Open3DSender). `seq` is the frame's tx_seq (0 = unset);
		//! with tx_wallclock_us and frame_epoch it stamps the frame. A sender
		//! stamps through O3DS::StreamWriter (ADR 0005 (iv)); calling this
		//! without a stamp is deprecated (CORE-29).
		int SerializeUpdateResidual(std::vector<char>& outbuf, size_t& count, double deltaThreshold, double timestamp, uint64_t seq = 0,
			uint64_t tx_wallclock_us = 0, uint32_t frame_epoch = 0);

	private:
		std::unique_ptr<ResidualEncoder> mResidualEncoder;
		std::unique_ptr<ResidualDecoder> mResidualDecoder;
	};

	//! Prefix of the placeholder name SubjectList::Parse() gives a transform
	//! that arrived without a name, followed by its index in the subject
	//! (for example "o3ds_unnamed_3"). The transform is kept rather than
	//! dropped so that later parent ids stay aligned (RCV-14).
	constexpr const char* kUnnamedTransformPrefix = "o3ds_unnamed_";

	//! One subject that a SubjectList::Parse() call applied data to.
	//! fullDescriptor is true when the packet carried a full Subject for it
	//! (topology, names and curve list), false when it carried only an
	//! update for a subject that already existed.
	struct ParsedSubjectInfo
	{
		std::string name;
		bool fullDescriptor = false;
	};

	/*! \class SubjectList model.h o3ds/model.h */
	//!  A collection of subjects.
	class O3DS_API SubjectList
	{
	public:

		SubjectList()
			: mTime(0.0)
			, mDeltaThreshold(std::numeric_limits<double>::min())
		{}

		// mItems owns its Subject pointers, so a member-wise copy would
		// delete every subject twice (CORE-17). SubjectList is move-only.
		SubjectList(const SubjectList &other) = delete;
		SubjectList& operator=(const SubjectList &other) = delete;

		SubjectList(SubjectList &&other) noexcept
			: mItems(std::move(other.mItems))
			, mTime(other.mTime)
			, mDeltaThreshold(other.mDeltaThreshold)
			, mError(std::move(other.mError))
			, mQuantizationEnabled(other.mQuantizationEnabled)
			, mQuantRanges(other.mQuantRanges)
			, mComputeWorldMatrices(other.mComputeWorldMatrices)
		{
			other.mItems.clear();
		}

		SubjectList& operator=(SubjectList &&other) noexcept
		{
			if (this != &other)
			{
				for (auto i : mItems)
				{
					delete i;
				}
				mItems = std::move(other.mItems);
				other.mItems.clear();
				mTime = other.mTime;
				mDeltaThreshold = other.mDeltaThreshold;
				mError = std::move(other.mError);
				mQuantizationEnabled = other.mQuantizationEnabled;
				mQuantRanges = other.mQuantRanges;
				mComputeWorldMatrices = other.mComputeWorldMatrices;
			}
			return *this;
		}

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
		//! Why the last Parse() rejected its frame header (D8, ADR 0009), or
		//! Ok; with VersionTooNew, mLastFrameMinReaderVersion is the
		//! protocol the sender requires.
		Wire::FrameCheck mLastFrameCheck = Wire::FrameCheck::Ok;
		uint8_t mLastFrameMinReaderVersion = 0;

		// D1 (roadmap doc §6/D1): opt-in adaptive channel quantization for
		// SerializeUpdate() below, mirroring mDeltaThreshold's own
		// member-not-parameter pattern. Disabled (false) by default -
		// SerializeUpdate()'s wire output is then byte-for-byte identical
		// to before D1 existed. mQuantRanges is only consulted when
		// mQuantizationEnabled is true.
		bool mQuantizationEnabled = false;
		QuantRanges mQuantRanges;

		//! When true (the default, and the behaviour before this flag
		//! existed), Parse() also solves every transform's mWorldMatrix. The
		//! UE receiver only consumes local TRS, so it can turn this off and
		//! skip the per-transform matrix products. Hierarchy validation and
		//! local matrices (mMatrix) run either way.
		bool mComputeWorldMatrices = true;

		//! Encode all of the items in the subject list as binary data.
		//! tx_seq/tx_wallclock_us/frame_epoch are optional (0 == unset, the
		//! default); a receiver must treat 0 exactly like a sender that
		//! predates these fields. This function only writes what it is given;
		//! a sender stamps through O3DS::StreamWriter (ADR 0005 (iv)), and
		//! calling it without a stamp is deprecated (CORE-29).
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
		//!
		//! outTouched (WP-S4, RCV-5), when non-null, is cleared and then
		//! receives each subject this packet applied data to, in first-seen
		//! order and without duplicates. Subjects already in the list that
		//! the packet did not mention are not reported, so a receiver can
		//! publish only what was actually sent. An update naming a subject
		//! that does not exist is ignored and not reported. On failure the
		//! contents of outTouched are unspecified.
		bool Parse(const char *data, size_t len, TransformBuilder* = nullptr, bool clearInactive = true,
			std::vector<ParsedSubjectInfo>* outTouched = nullptr);

		//! Extract just the transmit-sequencing metadata (tx_seq/
		//! tx_wallclock_us/frame_epoch) from a wire buffer, without doing a
		//! full parse of its subjects/updates. Used by ReorderGate to make
		//! ordering decisions before paying the cost of a full Parse().
		//! Runs the same frame checks as Parse() (CheckFrame: frame word,
		//! CRC, FlatBuffers Verifier; CORE-15, ADR 0009) on these untrusted
		//! network bytes. Returns false (outputs left at 0, their "unset"
		//! value) if any check fails.
		static bool PeekMeta(const char* data, size_t len,
			uint64_t& outTxSeq, uint64_t& outTxWallclockUs, uint32_t& outFrameEpoch);

		void ParseSubject(const O3DS::Data::Subject*, TransformBuilder* = nullptr);

		//! Returns true when the update was applied to an existing subject;
		//! false when it was skipped (no name, unknown subject) or rejected
		//! (mError set).
		bool ParseUpdate(const O3DS::Data::SubjectUpdate*, TransformBuilder* = nullptr);

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
		//! Same return contract as ParseUpdate().
		bool ParseUpdateResidual(const O3DS::Data::SubjectUpdate*, TransformBuilder* = nullptr);

		//! Change distance threshold below which O3DS skips transmitting a transform update.
		void SetDeltaThreshold(double newThreshold) { mDeltaThreshold = newThreshold; }

	};

	//! Writes the 8-byte frame header (frameWord, then the CRC-32 of the
	//! payload, both little-endian) and the finished FlatBuffer to outbuf.
	//! Low level: the serializers use FinishSubjectListFrame, which picks
	//! the frame word; tests use this to build frames with a chosen word.
	O3DS_API void finalize(flatbuffers::FlatBufferBuilder& builder, std::vector<char>& outbuf, std::uint32_t frameWord);

	//! Finishes a SubjectList with the "O3DS" file identifier and writes the
	//! frame, stamping min_reader_version from what it contains (D8, ADR
	//! 0009 item 2): 2 when any update is residual or quantized, else 1.
	O3DS_API void FinishSubjectListFrame(flatbuffers::FlatBufferBuilder& builder,
		flatbuffers::Offset<O3DS::Data::SubjectList> root, std::vector<char>& outbuf);

	//! The lowest protocol a reader needs to apply this SubjectList:
	//! kMinReaderResidualOrQuantized when an update has predictor_id != 0
	//! or any *_q8 / *_q16 vector, kMinReaderPlain otherwise.
	O3DS_API uint8_t RequiredReaderVersion(const O3DS::Data::SubjectList& list);

	//! Checks a framed wire buffer before anything trusts it (ADR 0009):
	//! the frame word (min_reader_version 1..O3DS_PROTOCOL_VERSION, zero
	//! flag and reserved bytes), the payload CRC-32, the FlatBuffers
	//! Verifier (with the "O3DS" identifier on version-2 frames; optional on
	//! version 1). outMinReaderVersion is set
	//! once the frame word has been read, also when the version is too new.
	O3DS_API Wire::FrameCheck CheckFrame(const char* data, size_t len, uint8_t& outMinReaderVersion);


} // O3DS


#endif
