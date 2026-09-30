/*
Open 3D Stream

Copyright 2026 Open3DStream Contributors

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

#include "receiver_streams.h"
#include "parse_limits.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace O3DS
{
	namespace
	{
		constexpr uint64_t kFnvOffset = 14695981039346656037ull;
		constexpr uint64_t kFnvPrime = 1099511628211ull;

		void FnvBytes(uint64_t& hash, const void* data, size_t len)
		{
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < len; ++i)
			{
				hash ^= bytes[i];
				hash *= kFnvPrime;
			}
		}

		// Fixed-width little-endian encoding, so the hash does not depend
		// on the host's byte order or on sizeof(size_t).
		void FnvU64(uint64_t& hash, uint64_t value)
		{
			unsigned char bytes[8];
			for (int i = 0; i < 8; ++i)
				bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
			FnvBytes(hash, bytes, sizeof(bytes));
		}

		void FnvString(uint64_t& hash, const std::string& s)
		{
			FnvU64(hash, static_cast<uint64_t>(s.size()));
			FnvBytes(hash, s.data(), s.size());
		}
	}

	bool PeekPacketMeta(const char* data, size_t len, PacketMeta& out)
	{
		out = PacketMeta();

		if (data == nullptr || len < 8)
			return false;

		flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data + 8), len - 8);
		if (!O3DS::Data::VerifySubjectListBuffer(verifier))
			return false;

		auto root = O3DS::Data::GetSubjectList(data + 8);
		if (root == nullptr || !std::isfinite(root->time()))
			return false;

		auto subjects = root->subjects();
		auto updates = root->updates();
		if ((subjects && subjects->size() > ParseLimits::kMaxSubjects)
			|| (updates && updates->size() > ParseLimits::kMaxSubjects))
			return false;

		std::vector<std::string> names;
		names.reserve((subjects ? subjects->size() : 0) + (updates ? updates->size() : 0));
		if (subjects)
		{
			for (auto s : *subjects)
			{
				if (s != nullptr && s->name() != nullptr)
					names.push_back(s->name()->str());
			}
		}
		if (updates)
		{
			for (auto u : *updates)
			{
				if (u != nullptr && u->name() != nullptr)
					names.push_back(u->name()->str());
			}
		}

		out.tx_seq = root->tx_seq();
		out.tx_wallclock_us = root->tx_wallclock_us();
		out.frame_epoch = root->frame_epoch();
		out.time = root->time();
		out.stream_key = StreamKeyForNames(names);
		out.subject_names = std::move(names);
		return true;
	}

	uint64_t StreamKeyForNames(std::vector<std::string> names)
	{
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		if (names.empty())
			return 0;

		uint64_t hash = kFnvOffset;
		FnvU64(hash, static_cast<uint64_t>(names.size()));
		for (const std::string& name : names)
			FnvString(hash, name);

		// 0 is reserved for "no named subjects".
		return hash == 0 ? 1 : hash;
	}

	uint64_t SkeletonFingerprint(const Subject& subject)
	{
		const std::vector<Transform*>& items = subject.mTransforms.mItems;

		uint64_t hash = kFnvOffset;
		FnvU64(hash, static_cast<uint64_t>(items.size()));
		for (const Transform* transform : items)
		{
			if (transform == nullptr)
			{
				FnvU64(hash, ~0ull);
				continue;
			}
			FnvString(hash, transform->mName);
			FnvU64(hash, static_cast<uint64_t>(static_cast<int64_t>(transform->mParentId)));
		}
		return hash;
	}

	LegacyOrdering::Decision LegacyOrdering::Check(double subjectListTime, double nowS, const LegacyOrderingConfig& config)
	{
		mLastCheckReset = false;

		if (mLastApplied >= 0.0)
		{
			const bool silence = config.silenceResetSeconds > 0.0
				&& mLastSeenS >= 0.0
				&& (nowS - mLastSeenS) > config.silenceResetSeconds;
			const bool jumpBack = config.timestampJumpResetSeconds > 0.0
				&& (mLastApplied - subjectListTime) > config.timestampJumpResetSeconds;
			if (silence || jumpBack)
			{
				mLastApplied = -1.0;
				mLastCheckReset = true;
			}
		}

		mLastSeenS = nowS;

		Decision decision = Decision::Apply;
		if (mLastApplied >= 0.0)
		{
			if (subjectListTime == mLastApplied)
				decision = Decision::Duplicate;
			else if (config.dropOutOfOrder && subjectListTime < mLastApplied)
				decision = Decision::OutOfOrder;
		}

		if (decision == Decision::Apply)
			mLastApplied = subjectListTime;

		return decision;
	}

	void LegacyOrdering::Reset()
	{
		mLastApplied = -1.0;
		mLastSeenS = -1.0;
		mLastCheckReset = false;
	}

	ReceiverStreamTable::ReceiverStreamTable(size_t maxStreams, bool computeWorldMatrices)
		: mMaxStreams(maxStreams > 0 ? maxStreams : 1)
		, mComputeWorldMatrices(computeWorldMatrices)
	{
	}

	uint64_t ReceiverStreamTable::ResolveKey(const std::vector<std::string>& subjectNames) const
	{
		for (const std::string& name : subjectNames)
		{
			auto owner = mSubjectOwner.find(name);
			if (owner != mSubjectOwner.end())
				return owner->second;
		}
		return StreamKeyForNames(subjectNames);
	}

	ReceiverStream& ReceiverStreamTable::Acquire(uint64_t key, double nowS, const std::vector<std::string>* subjectNames)
	{
		auto found = mStreams.find(key);
		if (found == mStreams.end())
		{
			if (mStreams.size() >= mMaxStreams)
			{
				auto oldest = mStreams.begin();
				for (auto it = mStreams.begin(); it != mStreams.end(); ++it)
				{
					if (it->second->lastSeenS < oldest->second->lastSeenS)
						oldest = it;
				}
				Erase(oldest);
			}

			std::unique_ptr<ReceiverStream> stream(new ReceiverStream());
			stream->subjects.mComputeWorldMatrices = mComputeWorldMatrices;
			found = mStreams.emplace(key, std::move(stream)).first;
		}

		found->second->lastSeenS = nowS;

		if (subjectNames != nullptr)
		{
			// Bounded like the streams themselves: at most kMaxSubjects names
			// per stream slot.
			const size_t maxOwned = mMaxStreams * ParseLimits::kMaxSubjects;
			for (const std::string& name : *subjectNames)
			{
				if (mSubjectOwner.find(name) != mSubjectOwner.end())
					continue;
				if (mSubjectOwner.size() >= maxOwned)
					break;
				mSubjectOwner.emplace(name, key);
			}
		}

		return *found->second;
	}

	void ReceiverStreamTable::Erase(std::map<uint64_t, std::unique_ptr<ReceiverStream>>::iterator it)
	{
		const uint64_t key = it->first;
		for (auto owner = mSubjectOwner.begin(); owner != mSubjectOwner.end();)
		{
			if (owner->second == key)
				owner = mSubjectOwner.erase(owner);
			else
				++owner;
		}
		mStreams.erase(it);
	}

	ReceiverStream* ReceiverStreamTable::Find(uint64_t key)
	{
		auto found = mStreams.find(key);
		return found != mStreams.end() ? found->second.get() : nullptr;
	}

	size_t ReceiverStreamTable::PruneIdle(double nowS, double idleSeconds)
	{
		size_t removed = 0;
		for (auto it = mStreams.begin(); it != mStreams.end();)
		{
			if ((nowS - it->second->lastSeenS) > idleSeconds)
			{
				auto next = std::next(it);
				Erase(it);
				it = next;
				++removed;
			}
			else
			{
				++it;
			}
		}
		return removed;
	}

	void ReceiverStreamTable::ForEach(const std::function<void(uint64_t key, ReceiverStream& stream)>& fn)
	{
		for (auto& entry : mStreams)
			fn(entry.first, *entry.second);
	}
}
