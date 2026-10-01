/*
Open 3D Stream

Copyright 2026 Alastair Macleod

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

#ifndef OPEN3D_STREAM_CONTROL_H
#define OPEN3D_STREAM_CONTROL_H

#include "o3ds_export.h"
#include "parse_limits.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

//! Control channel: one-way events and keyed values from a sender to its
//! receivers, alongside mocap and audio (docs/adr/0011-control-channel.md).
//!
//! This header is transport- and engine-neutral. It provides:
//!   - Value, Message and the codec (SerializeMessage / ParseMessage, with
//!     Validate() enforcing ControlLimits on both sides);
//!   - ControlPublisher, the sender state: value table, coalescing, per-key
//!     rate limit, event redundancy, snapshots and retry;
//!   - ControlReceiver, the receiver state per source: last-writer-wins with
//!     tombstones, snapshot reconciliation, event de-duplication and TTL,
//!     rate and key limits;
//!   - ControlAligner, the optional hold queue that releases changes when
//!     the matching mocap stream reaches their sender time.
//!
//! Nothing here reads a clock: every time is passed in, so tests are
//! deterministic. Nothing here throws or uses RTTI (the UE plugin compiles
//! this file with both off, docs/adr/0003).
namespace O3DS
{
	namespace Control
	{
		//! Control protocol version written by this code and the highest one
		//! it reads (ControlMessage.protocol_version).
		constexpr uint16_t kProtocolVersion = 1;

		enum class ValueType : uint8_t
		{
			None = 0,
			Bool,
			Int,
			Double,
			String,
			Name,
			Vector3,
			Quat,
			Transform,
			Color,
			Bytes
		};

		struct Vec3
		{
			double x = 0.0, y = 0.0, z = 0.0;
		};

		struct Quat
		{
			double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
		};

		struct Color
		{
			float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
		};

		struct TransformValue
		{
			Vec3 translation;
			Quat rotation;
			Vec3 scale{ 1.0, 1.0, 1.0 };
		};

		//! A typed control value. Only the member matching `type` is
		//! meaningful; String and Name both use `text`.
		struct O3DS_API Value
		{
			ValueType type = ValueType::None;
			bool b = false;
			int64_t i = 0;
			double d = 0.0;
			std::string text;
			Vec3 vec;
			Quat quat;
			TransformValue transform;
			Color color;
			std::vector<uint8_t> bytes;

			static Value MakeNone() { return Value(); }
			static Value MakeBool(bool v);
			static Value MakeInt(int64_t v);
			static Value MakeDouble(double v);
			static Value MakeString(std::string v);
			static Value MakeName(std::string v);
			static Value MakeVector3(const Vec3& v);
			static Value MakeQuat(const Quat& v);
			static Value MakeTransform(const TransformValue& v);
			static Value MakeColor(const Color& v);
			static Value MakeBytes(std::vector<uint8_t> v);

			//! Compares the member selected by `type` (exact, bitwise-equal
			//! for doubles that are equal as values).
			bool operator==(const Value& other) const;
			bool operator!=(const Value& other) const { return !(*this == other); }
		};

		struct Entry
		{
			std::string key;
			std::string target;
			Value value;
			uint64_t version = 0;
			//! false when the value's union type is newer than this reader.
			//! The reader keeps the key (a snapshot still counts it as
			//! present) but does not apply the value. Writers never send one.
			bool supported = true;
		};

		struct Clear
		{
			std::string key;
			std::string target;
			uint64_t version = 0;
		};

		struct Event
		{
			uint64_t event_id = 0;
			std::string name;
			std::string target;
			Value value;
			uint32_t ttl_ms = 2000;
			uint64_t time_us = 0; //!< fire time, sender clock; 0 = the message's sender_time_us
			bool supported = true; //!< as Entry::supported; an unsupported event is skipped
		};

		//! In-memory form of ControlData::ControlMessage (src/o3ds_control.fbs).
		struct Message
		{
			uint16_t protocol_version = kProtocolVersion;
			std::string source_id;
			std::string source_name;
			uint32_t epoch = 0;
			uint64_t seq = 0;
			uint64_t sender_time_us = 0;
			uint64_t tx_wallclock_us = 0;
			std::vector<std::string> mocap_subjects;
			std::vector<Entry> set;
			std::vector<Clear> clear;
			std::vector<Event> events;
			uint32_t snapshot_id = 0;     //!< 0 = not part of a snapshot
			uint16_t snapshot_part = 0;
			uint16_t snapshot_parts = 0;
			uint64_t snapshot_seq = 0;

			bool IsSnapshot() const { return snapshot_id != 0; }
			size_t ItemCount() const { return set.size() + clear.size() + events.size(); }
		};

		enum class ParseError : uint8_t
		{
			None = 0,
			Empty,              //!< null or zero-length buffer
			TooLarge,           //!< over ControlLimits::kMaxPayloadBytes
			BadIdentifier,      //!< not an "O3DC" buffer
			VerifyFailed,       //!< FlatBuffers verifier rejected it
			UnsupportedVersion, //!< protocol_version > kProtocolVersion, or 0
			MissingField,       //!< a required string is empty, or seq/epoch is 0
			TooManyItems,
			StringTooLong,
			BadUtf8,
			NonFinite,          //!< NaN or infinity in a numeric value
			BadValue,           //!< unknown union member or malformed value
			BadSnapshot,        //!< inconsistent snapshot fields
			BadVersion,         //!< entry/clear version 0 or above seq
			BadTtl
		};

		O3DS_API const char* ToString(ParseError error);

		//! Semantic checks shared by reader and writer: required fields,
		//! counts, string lengths and UTF-8, finite numbers, snapshot and
		//! version consistency. A snapshot part's entries must be in strictly
		//! increasing (key, target) order, and only a single-part snapshot may
		//! be empty. Does not check the encoded size.
		O3DS_API ParseError Validate(const Message& message);

		//! Validates and encodes `message` as a ControlMessage FlatBuffer with
		//! the "O3DC" identifier. Returns false (out cleared) when Validate()
		//! fails, an item is unsupported, or the result exceeds
		//! ControlLimits::kMaxPayloadBytes.
		O3DS_API bool SerializeMessage(const Message& message, std::vector<uint8_t>& out);

		//! Decodes an untrusted buffer: size, identifier, verifier, then
		//! Validate(). On failure `out` is left default-constructed.
		O3DS_API ParseError ParseMessage(const uint8_t* data, size_t len, Message& out);

		//! True when `text` is well-formed UTF-8 with no NUL byte.
		O3DS_API bool IsValidUtf8(const std::string& text);

		//! Identity of a keyed value: key plus optional target subject.
		using SlotKey = std::pair<std::string, std::string>;

		// ------------------------------------------------------------------
		// Sender
		// ------------------------------------------------------------------

		struct PublisherConfig
		{
			//! Seconds between complete snapshots (ADR 0011 item 8; the sender
			//! component defaults it to FullSyncIntervalSeconds).
			double snapshot_interval_s = 1.0;

			//! Copies of each event, on consecutive ticks. 1 on reliable
			//! transports, 3 by default on unreliable ones.
			int event_redundancy = 1;

			//! Max times per second one key is re-sent; the latest value wins.
			double max_value_rate_hz = 30.0;

			//! Event TTL when FireEvent() is given 0.
			uint32_t default_event_ttl_ms = 2000;

			//! Max keys in the table (at most ControlLimits::kMaxKeysPerSource).
			size_t max_keys = ControlLimits::kMaxKeysPerSource;

			//! Output caps (at most the ControlLimits publisher caps, which a
			//! default receiver always accepts). Clears and events go before
			//! value changes; values over budget wait, coalesced.
			double max_live_bytes_per_s = ControlLimits::kPublisherMaxLiveBytesPerS;
			double max_snapshot_bytes_per_s = ControlLimits::kPublisherMaxSnapshotBytesPerS;

			//! Seconds a refused message is retried for.
			double retry_window_s = 2.0;

			//! Max refused messages held for retry; the oldest go first.
			size_t max_retry_messages = 64;
		};

		//! One encoded message ready for the transport. The caller wraps it in
		//! a control envelope and sends it; if the transport refuses it, pass
		//! it back to ControlPublisher::OnSendRefused().
		struct OutgoingMessage
		{
			std::vector<uint8_t> bytes;
			uint64_t seq = 0;
			bool snapshot = false;
		};

		enum class PublishResult : uint8_t
		{
			Ok = 0,
			NotRunning,  //!< events need a running publisher
			Invalid,     //!< empty or over-long name, bad UTF-8, non-finite value
			TooLarge,    //!< the item alone would not fit one message
			TooManyKeys  //!< the table is at max_keys
		};

		O3DS_API const char* ToString(PublishResult result);

		//! Sender-side state for one control stream (one sender component).
		//! Not thread-safe: call every method from one thread (the game
		//! thread in the UE plugin).
		class O3DS_API ControlPublisher
		{
		public:
			ControlPublisher(std::string sourceId, std::string sourceName, const PublisherConfig& config = PublisherConfig());

			void SetConfig(const PublisherConfig& config);
			const PublisherConfig& GetConfig() const { return mConfig; }

			//! Subjects this sender streams mocap under, so receivers can align
			//! control to that stream. Truncated to ControlLimits.
			void SetMocapSubjects(std::vector<std::string> subjects);

			//! Begins a session. The epoch becomes max(sessionEpoch,
			//! previous + 1) (ADR 0005 iv), and a snapshot is due at once.
			//! Pass O3DS::NewSessionEpoch().
			void Start(uint32_t sessionEpoch);

			//! Ends the session. Pending events and retries are dropped; the
			//! value table is kept for the next Start().
			void Stop();

			bool IsRunning() const { return mRunning; }
			uint32_t GetEpoch() const { return mEpoch; }

			//! Sets a value. Allowed while stopped (it goes out with the
			//! snapshot at the next Start). Setting the current value again is
			//! a no-op. Several sets of one key before the next Tick() send
			//! only the last.
			PublishResult SetValue(const std::string& key, const std::string& target, const Value& value);

			//! Removes a value. Removing an absent key is a no-op.
			PublishResult ClearValue(const std::string& key, const std::string& target);

			void ClearAll();

			//! Queues an event, sent on the next Tick() (and redundancy - 1
			//! further ticks). ttlMs 0 uses default_event_ttl_ms.
			PublishResult FireEvent(const std::string& name, const std::string& target, const Value& value,
				uint64_t senderTimeUs, uint32_t ttlMs = 0);

			//! The current value of a key, or nullptr.
			const Value* FindValue(const std::string& key, const std::string& target) const;
			size_t NumValues() const { return mTable.size(); }

			//! Asks for a complete snapshot on the next Tick() (new-peer
			//! trigger, ADR 0005 vi).
			void RequestSnapshot() { mSnapshotRequested = true; }

			//! Produces the messages due now: pending clears, coalesced value
			//! changes (rate-limited per key), event copies, snapshot parts
			//! (paced over half the interval) and retries. Appends to `out`.
			//! Does nothing while stopped.
			void Tick(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs, std::vector<OutgoingMessage>& out);

			//! The transport refused `message`: it is resent on later ticks
			//! until retry_window_s has passed. Receivers discard anything a
			//! newer message superseded.
			void OnSendRefused(const OutgoingMessage& message, double nowS);

		private:
			struct Slot
			{
				Value value;
				uint64_t version = 0;    //!< 0 = never sent
				bool dirty = false;
				double last_sent_s = -1.0e300;
			};

			struct PendingEvent
			{
				Event event;
				int copies_left = 0;
			};

			struct PendingSnapshot
			{
				std::vector<OutgoingMessage> parts;
				std::vector<double> due_s;
				size_t next = 0;
			};

			struct Retry
			{
				OutgoingMessage message;
				double first_refused_s = 0.0;
			};

			Message MakeHeader(uint64_t senderTimeUs, uint64_t wallclockUs) const;
			bool FitsAlone(const Message& withOneItem) const;
			void BuildSnapshot(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs);
			void EmitLive(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs, std::vector<OutgoingMessage>& out);

			std::string mSourceId;
			std::string mSourceName;
			PublisherConfig mConfig;
			std::vector<std::string> mMocapSubjects;

			bool mRunning = false;
			uint32_t mEpoch = 0;
			uint64_t mLastSeq = 0;
			uint64_t mNextEventId = 1;
			uint32_t mNextSnapshotId = 1;

			std::map<SlotKey, Slot> mTable;
			std::map<SlotKey, bool> mPendingClears; // value unused; ordered for determinism
			std::deque<PendingEvent> mPendingEvents;
			std::deque<Retry> mRetries;

			bool mSnapshotRequested = false;
			bool mHadKeysThisEpoch = false;
			bool mValidSource = false;
			double mLastSnapshotS = 0.0;
			PendingSnapshot mSnapshot;

			struct Budget
			{
				double tokens = 0.0;
				double last_s = 0.0;
				bool primed = false;
			};
			Budget mLiveBudget;
			Budget mSnapshotBudget;
			bool Spend(Budget& budget, double ratePerS, size_t bytes, double nowS, bool commit);
		};

		// ------------------------------------------------------------------
		// Receiver
		// ------------------------------------------------------------------

		struct ReceiverConfig
		{
			//! Max keys (values plus tombstones) per source.
			size_t max_keys_per_source = ControlLimits::kMaxKeysPerSource;

			//! Byte-rate budget per source for live messages, and a separate
			//! one for snapshot parts, so live traffic cannot starve
			//! snapshots. Each bucket holds one second of budget.
			double max_live_bytes_per_s = ControlLimits::kReceiverDefaultLiveBytesPerS;
			double max_snapshot_bytes_per_s = ControlLimits::kReceiverDefaultSnapshotBytesPerS;

			//! Event ids remembered per source for de-duplication.
			size_t dedupe_window = 512;

			//! Seconds without a new part before an incomplete snapshot is
			//! discarded. Counted from the last part, not the first: a large
			//! table paced at the publisher's snapshot byte cap can take
			//! several seconds to send.
			double incomplete_snapshot_timeout_s = 2.0;

			//! Max sources tracked; messages from further sources are dropped.
			size_t max_sources = 64;

			//! Seconds without a message before a source is dropped (its
			//! values are reported as cleared). A source with values sends a
			//! snapshot at least every 10 s, so only a dead one goes quiet.
			double source_idle_timeout_s = 30.0;

			//! Key and event-name prefixes accepted; empty accepts all.
			std::vector<std::string> allow_prefixes;
		};

		struct Change
		{
			enum class Kind : uint8_t { ValueChanged, ValueCleared, Event };

			Kind kind = Kind::ValueChanged;
			std::string source_id;
			std::string source_name;
			std::string name;   //!< key, or event name
			std::string target;
			Value value;        //!< empty for ValueCleared
			uint32_t epoch = 0;
			uint64_t seq = 0;
			uint64_t sender_time_us = 0; //!< when the change happened, sender clock
			uint64_t event_id = 0;
			//! ValueChanged: the entry's version. ValueCleared: the clear's
			//! version, or the snapshot capture point that removed the key
			//! (0 when a pruned source is reported cleared).
			uint64_t version = 0;
		};

		struct ReceiverStats
		{
			uint64_t messages_accepted = 0;
			uint64_t rejected_parse = 0;
			uint64_t dropped_rate = 0;
			uint64_t dropped_old_epoch = 0;
			uint64_t dropped_source_cap = 0;
			uint64_t items_stale = 0;
			uint64_t items_not_allowed = 0;
			uint64_t items_key_cap = 0;
			uint64_t items_unsupported = 0; //!< value types newer than this reader
			uint64_t events_duplicate = 0;
			uint64_t events_expired = 0;
			uint64_t snapshots_completed = 0;
			uint64_t snapshots_discarded = 0;
			uint64_t sources_pruned = 0;
		};

		//! Receiver-side state for every control source on one receiver. Not
		//! thread-safe: call every method from one thread.
		class O3DS_API ControlReceiver
		{
		public:
			explicit ControlReceiver(const ReceiverConfig& config = ReceiverConfig());

			void SetConfig(const ReceiverConfig& config);
			const ReceiverConfig& GetConfig() const { return mConfig; }

			//! Parses and applies one payload (the bytes inside the envelope).
			//! Appends the resulting changes to `out`. Returns the parse
			//! error, or None when the message was well-formed (it may still
			//! have been dropped by a limit; see GetStats()).
			ParseError Submit(const uint8_t* data, size_t len, double nowS, std::vector<Change>& out);

			//! Applies a message that already passed ParseMessage() (or
			//! Validate()); `wireBytes` is its encoded size, for the rate limit.
			void Apply(const Message& message, size_t wireBytes, double nowS, std::vector<Change>& out);

			//! Expires incomplete snapshots and idle sources. Pruned sources
			//! report their values as cleared.
			void Tick(double nowS, std::vector<Change>& out);

			//! The current value from one source, or nullptr.
			const Value* FindValue(const std::string& sourceId, const std::string& key, const std::string& target) const;

			//! Every current value from one source, ordered by key and target.
			std::vector<std::pair<SlotKey, Value>> GetValues(const std::string& sourceId) const;

			//! Mocap subjects the source last reported (for alignment).
			const std::vector<std::string>* FindMocapSubjects(const std::string& sourceId) const;

			size_t NumSources() const { return mSources.size(); }

			//! Forgets everything without reporting changes.
			void Reset();

			const ReceiverStats& GetStats() const { return mStats; }

		private:
			struct Slot
			{
				Value value;
				uint32_t epoch = 0;
				uint64_t version = 0;
			};

			struct Tombstone
			{
				uint32_t epoch = 0;
				uint64_t version = 0;
			};

			struct Bucket
			{
				double tokens = 0.0;
				double last_s = 0.0;
				bool primed = false;
			};

			struct Assembly
			{
				uint32_t id = 0;
				uint32_t epoch = 0;
				uint16_t parts = 0;
				uint64_t snapshot_seq = 0;
				std::vector<bool> have;
				//! First and last key of each received part. Parts cover
				//! contiguous ranges of the sorted table, so together with
				//! their neighbours they say which keys were absent at capture.
				std::vector<SlotKey> part_first;
				std::vector<SlotKey> part_last;
				size_t received = 0;
				double last_part_s = 0.0;
			};

			struct Source
			{
				std::string name;
				uint32_t epoch = 0;
				bool has_epoch = false;
				std::map<SlotKey, Slot> values;
				std::map<SlotKey, Tombstone> tombstones;
				//! Capture point of the last complete snapshot. A key with no
				//! slot and no tombstone was absent then, so a set at or below
				//! this point is stale (it lost to a clear the snapshot already
				//! reflects).
				uint32_t floor_epoch = 0;
				uint64_t floor_seq = 0;
				std::set<uint64_t> seen_events;
				std::deque<uint64_t> seen_order;
				uint64_t newest_sender_time_us = 0;
				std::vector<std::string> mocap_subjects;
				Assembly assembly;
				bool assembling = false;
				Bucket live;
				Bucket snapshot;
				double last_seen_s = 0.0;
			};

			bool Allowed(const std::string& name) const;
			bool TakeTokens(Bucket& bucket, double ratePerS, size_t bytes, double nowS);
			void ApplyEntry(Source& source, const std::string& sourceId, const Message& message, const Entry& entry, std::vector<Change>& out);
			void ApplyClear(Source& source, const std::string& sourceId, const Message& message, const Clear& clear, std::vector<Change>& out);
			void ApplyEvent(Source& source, const std::string& sourceId, const Message& message, const Event& event, std::vector<Change>& out);
			void TrackSnapshot(Source& source, const std::string& sourceId, const Message& message, double nowS, std::vector<Change>& out);
			void CompleteSnapshot(Source& source);
			//! Removes values in the given key range that `present` (null:
			//! none) lacks and that are at or below the assembly's capture
			//! point. Bounds are null for unbounded.
			void RemoveAbsent(Source& source, const std::string& sourceId, const SlotKey* low, bool lowInclusive,
				const SlotKey* high, bool highInclusive, const std::set<SlotKey>* present, std::vector<Change>& out);
			static void ReportCleared(const Source& source, const std::string& sourceId, std::vector<Change>& out);

			ReceiverConfig mConfig;
			std::map<std::string, Source> mSources;
			ReceiverStats mStats;
		};

		// ------------------------------------------------------------------
		// Alignment to the mocap timeline (ADR 0011 item 9, Timing)
		// ------------------------------------------------------------------

		struct AlignerStats
		{
			uint64_t released_aligned = 0;   //!< released when the mocap time reached them
			uint64_t released_unaligned = 0; //!< no mocap time available: released at once
			uint64_t released_late = 0;      //!< held past max_hold_s
		};

		//! Holds changes until the mocap stream they belong to is presenting
		//! the sender time they carry. Order within a source is preserved:
		//! a change is never released before an earlier one from that source.
		class O3DS_API ControlAligner
		{
		public:
			explicit ControlAligner(double maxHoldS = 0.5) : mMaxHoldS(maxHoldS) {}

			void SetMaxHold(double maxHoldS) { mMaxHoldS = maxHoldS; }

			void Push(Change&& change, double nowS);

			//! Releases what is due. `presentationTimeUs(sourceId, outUs)`
			//! returns false when that source has no aligned mocap stream
			//! (its changes are then released at once); otherwise it sets
			//! the sender time the receiver is presenting for that stream.
			template <typename PresentationFn>
			void Release(double nowS, PresentationFn&& presentationTimeUs, std::vector<Change>& out)
			{
				for (auto& queue : mQueues)
				{
					uint64_t presentingUs = 0;
					const bool aligned = presentationTimeUs(queue.first, presentingUs);
					ReleaseQueue(queue.second, aligned, presentingUs, nowS, out);
				}
				Prune();
			}

			//! Releases everything at once (alignment turned off, shutdown).
			void Flush(std::vector<Change>& out);

			size_t NumHeld() const;
			const AlignerStats& GetStats() const { return mStats; }

		private:
			struct Held
			{
				Change change;
				double pushed_s = 0.0;
			};

			void ReleaseQueue(std::deque<Held>& queue, bool aligned, uint64_t presentingUs, double nowS, std::vector<Change>& out);
			void Prune();

			double mMaxHoldS;
			std::map<std::string, std::deque<Held>> mQueues;
			AlignerStats mStats;
		};
	}
}

#endif
