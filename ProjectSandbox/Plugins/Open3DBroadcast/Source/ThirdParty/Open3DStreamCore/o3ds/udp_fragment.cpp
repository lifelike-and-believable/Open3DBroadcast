/*
Open 3D Stream

Copyright 2022 Alastair Macleod

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

#include "udp_fragment.h"

#include <cstring>
#include <limits>
#include <vector>

namespace
{
	// Explicit little-endian, alignment-safe scalar access (CORE-22). The
	// bytes are copied with memcpy to/from a local byte array and assembled
	// with shifts, so there is no type-punning and no dependence on host
	// endianness or on the alignment of the network buffer.
	void writeLE32(char* out, uint32_t v)
	{
		unsigned char b[4];
		b[0] = (unsigned char)(v & 0xffu);
		b[1] = (unsigned char)((v >> 8) & 0xffu);
		b[2] = (unsigned char)((v >> 16) & 0xffu);
		b[3] = (unsigned char)((v >> 24) & 0xffu);
		memcpy(out, b, 4);
	}

	uint32_t readLE32(const char* in)
	{
		unsigned char b[4];
		memcpy(b, in, 4);
		return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	}
}

void writeUdpFragmentHeader(const UdpFragmentHeader& header, char* out)
{
	const unsigned char prefix[8] = {
		kUdpFragmentMagic[0], kUdpFragmentMagic[1], kUdpFragmentMagic[2], kUdpFragmentMagic[3],
		kUdpFragmentVersion, 0, 0, 0 };
	memcpy(out, prefix, sizeof(prefix));
	writeLE32(out + 8, header.id);
	writeLE32(out + 12, header.seq);
	writeLE32(out + 16, header.totalSize);
	writeLE32(out + 20, header.fragSize);
}

bool readUdpFragmentHeader(const char* data, size_t sz, UdpFragmentHeader& out)
{
	if (data == nullptr || sz < kUdpFragmentHeaderSize) return false;
	unsigned char prefix[8];
	memcpy(prefix, data, sizeof(prefix));
	if (memcmp(prefix, kUdpFragmentMagic, 4) != 0 || prefix[4] != kUdpFragmentVersion
		|| prefix[5] != 0 || prefix[6] != 0 || prefix[7] != 0)
	{
		return false;
	}
	out.id        = readLE32(data + 8);
	out.seq       = readLE32(data + 12);
	out.totalSize = readLE32(data + 16);
	out.fragSize  = readLE32(data + 20);
	return true;
}

UdpDatagramKind udpClassifyDatagram(const char* data, size_t sz)
{
	if (data == nullptr || sz < 4) return UdpDatagramKind::Unknown;
	unsigned char b[4];
	memcpy(b, data, 4);
	if (memcmp(b, kUdpFragmentMagic, 4) == 0) return UdpDatagramKind::Fragment;
	if (b[0] == 'O' && b[1] == '3' && b[2] == 'D' && (b[3] == 'A' || b[3] == 'U')) return UdpDatagramKind::Envelope;
	if (b[0] != 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) return UdpDatagramKind::Frame;
	return UdpDatagramKind::Unknown;
}

bool udpMessageIdLess(uint32_t a, uint32_t b)
{
	// a precedes b if the forward distance from a to b is in (0, 2^31).
	const uint32_t diff = b - a;
	return diff != 0 && diff < 0x80000000u;
}


UdpFragmenter::UdpFragmenter(const char* data, size_t sz, size_t inFragSize)
	: mFragmentSize(inFragSize)
	, mBufferSize(0)
	, mFrames(0)
{
	// The header carries both sizes as uint32; refuse anything that would
	// be silently truncated on the wire, and a zero fragment size.
	const size_t kMax32 = (size_t)std::numeric_limits<uint32_t>::max();
	if (data == nullptr || sz == 0 || inFragSize == 0 || sz > kMax32 || inFragSize > kMax32)
		return;

	mBuffer.assign(data, data + sz);
	mBufferSize = sz;
	mFrames = (sz + inFragSize - 1) / inFragSize;
}

bool UdpFragmenter::makeFragment(
	uint32_t inId,
	uint32_t inSeq,
	std::vector<char>& out
) const
{
	out.clear();

	if ((size_t)inSeq >= mFrames)
		return false;

	UdpFragmentHeader header;
	header.id        = inId;
	header.seq       = inSeq;
	header.totalSize = (uint32_t)mBufferSize;
	header.fragSize  = (uint32_t)mFragmentSize;

	const size_t start = (size_t)inSeq * mFragmentSize;
	const size_t len = ((size_t)inSeq == mFrames - 1) ? (mBufferSize - start) : mFragmentSize;

	out.resize(kUdpFragmentHeaderSize + len);
	writeUdpFragmentHeader(header, out.data());
	memcpy(out.data() + kUdpFragmentHeaderSize, mBuffer.data() + start, len);
	return true;
}

UdpCombiner::UdpCombiner(size_t maxMessageSize)
	: mMaxMessageSize(maxMessageSize)
	, mFrameSize(0)
	, mBufferSize(0)
	, mFrames(0)
	, mReceived(0)
{
}

bool UdpCombiner::validateFragment(const UdpFragmentHeader& header, size_t payloadLen, size_t maxMessageSize)
{
	const uint64_t bufSz    = header.totalSize;
	const uint64_t fragSize = header.fragSize;
	const uint64_t seq      = header.seq;

	// Degenerate or oversized parameters from the wire are rejected before
	// any arithmetic with them (fragSize == 0 would divide by zero).
	if (fragSize == 0) return false;
	if (bufSz == 0 || bufSz > (uint64_t)maxMessageSize) return false;
	if (payloadLen > kUdpMaxDatagramSize - kUdpFragmentHeaderSize) return false;

	const uint64_t frames = (bufSz + fragSize - 1) / fragSize;
	if (seq >= frames) return false;

	// Every fragment must fill its slot exactly: non-last fragments carry a
	// full fragSize payload, the last one exactly the remaining tail.
	const uint64_t sliceStart = seq * fragSize;
	const uint64_t expected = (seq == frames - 1) ? (bufSz - sliceStart) : fragSize;
	return (uint64_t)payloadLen == expected;
}

bool UdpCombiner::addFragment(const char* data, size_t sz)
{
	UdpFragmentHeader header;
	if (!readUdpFragmentHeader(data, sz, header)) return false;

	const size_t payloadLen = sz - kUdpFragmentHeaderSize;
	if (!validateFragment(header, payloadLen, mMaxMessageSize)) return false;

	if (mBufferSize == 0)
	{
		// First fragment: lock the message geometry (CORE-2). Capacity is
		// reserved for the claimed size; bytes are only written as fragments
		// arrive, so a lone spoofed first fragment does not touch the whole
		// reservation.
		mBufferSize = header.totalSize;
		mFrameSize  = header.fragSize;
		mFrames     = (mBufferSize + mFrameSize - 1) / mFrameSize;
		mBuffer.clear();
		mBuffer.reserve(mBufferSize);
		mFound.assign(mFrames, false);
		mReceived = 0;
	}
	else if (header.totalSize != mBufferSize || header.fragSize != mFrameSize)
	{
		// A later fragment disagreeing with the locked geometry is rejected;
		// its own (seq < frames) check was against a different frame count.
		return false;
	}

	const size_t seq = header.seq;
	if (seq >= mFound.size()) return false; // redundant with validateFragment, kept as a hard bound

	const size_t start = seq * mFrameSize;
	const size_t end = start + payloadLen;
	if (end > mBufferSize) return false;

	if (mBuffer.size() < end)
		mBuffer.resize(end);
	memcpy(mBuffer.data() + start, data + kUdpFragmentHeaderSize, payloadLen);

	if (!mFound[seq])
	{
		mFound[seq] = true;
		mReceived++;
	}

	return true;
}

bool UdpCombiner::isComplete() const
{
	return mFrames != 0 && mReceived == mFrames;
}


UdpMapper::UdpMapper()
	: UdpMapper(UdpReassemblyConfig())
{
}

UdpMapper::UdpMapper(const UdpReassemblyConfig& config)
	: mConfig(config)
{
	if (mConfig.maxInFlightMessages == 0) mConfig.maxInFlightMessages = 1;
	// A single message must always fit in the total budget.
	if (mConfig.maxTotalBytes < mConfig.maxMessageSize) mConfig.maxTotalBytes = mConfig.maxMessageSize;
}

void UdpMapper::erase(std::map<Key, Entry>::iterator it)
{
	mBytesInUse -= it->second.combiner.reservedBytes();
	mItems.erase(it);
}

bool UdpMapper::evictOldest()
{
	auto oldest = mItems.end();
	for (auto it = mItems.begin(); it != mItems.end(); ++it)
	{
		if (oldest == mItems.end() || it->second.order < oldest->second.order)
			oldest = it;
	}
	if (oldest == mItems.end()) return false;
	erase(oldest);
	mStats.messagesEvicted++;
	return true;
}

void UdpMapper::expire(uint64_t nowMs)
{
	for (auto it = mItems.begin(); it != mItems.end();)
	{
		const Entry& e = it->second;
		const uint64_t age = nowMs > e.firstSeenMs ? nowMs - e.firstSeenMs : 0;
		if (!e.combiner.isComplete() && age >= mConfig.messageTimeoutMs)
		{
			auto victim = it++;
			erase(victim);
			mStats.messagesExpired++;
		}
		else
		{
			++it;
		}
	}
}

bool UdpMapper::addFragment(uint64_t sourceKey, const char* data, size_t sz, uint64_t nowMs)
{
	expire(nowMs);

	UdpFragmentHeader header;
	if (!readUdpFragmentHeader(data, sz, header)
		|| !UdpCombiner::validateFragment(header, sz - kUdpFragmentHeaderSize, mConfig.maxMessageSize))
	{
		mStats.fragmentsRejected++;
		return false;
	}

	const Key key(sourceKey, header.id);
	auto it = mItems.find(key);
	if (it == mItems.end())
	{
		// New message: only now that the fragment is known to be well formed
		// do we make room for it, so a malformed datagram cannot evict
		// legitimate in-progress messages.
		while (!mItems.empty()
			&& (mItems.size() >= mConfig.maxInFlightMessages
				|| mBytesInUse + header.totalSize > mConfig.maxTotalBytes))
		{
			evictOldest();
		}

		Entry entry;
		entry.combiner = UdpCombiner(mConfig.maxMessageSize);
		if (!entry.combiner.addFragment(data, sz))
		{
			mStats.fragmentsRejected++;
			return false;
		}
		entry.firstSeenMs = nowMs;
		entry.order = mNextOrder++;
		mBytesInUse += entry.combiner.reservedBytes();
		it = mItems.emplace(key, std::move(entry)).first;
	}
	else if (!it->second.combiner.addFragment(data, sz))
	{
		mStats.fragmentsRejected++;
		return false;
	}

	mStats.fragmentsAccepted++;

	if (it->second.combiner.isComplete())
	{
		// Latest-wins per source: incomplete messages from the same source
		// with an older id (wrapping comparison) can no longer be useful.
		for (auto j = mItems.begin(); j != mItems.end();)
		{
			if (j != it
				&& j->first.first == sourceKey
				&& !j->second.combiner.isComplete()
				&& udpMessageIdLess(j->first.second, header.id))
			{
				auto victim = j++;
				erase(victim);
				mStats.messagesSuperseded++;
			}
			else
			{
				++j;
			}
		}
	}

	return true;
}

bool UdpMapper::getFrame(std::vector<char> &out)
{
	auto ready = mItems.end();
	for (auto it = mItems.begin(); it != mItems.end(); ++it)
	{
		if (it->second.combiner.isComplete()
			&& (ready == mItems.end() || it->second.order < ready->second.order))
		{
			ready = it;
		}
	}

	if (ready == mItems.end())
		return false;

	out = std::move(ready->second.combiner.mBuffer);
	erase(ready);
	mStats.messagesCompleted++;
	return true;
}

void UdpMapper::clear()
{
	mItems.clear();
	mBytesInUse = 0;
}
