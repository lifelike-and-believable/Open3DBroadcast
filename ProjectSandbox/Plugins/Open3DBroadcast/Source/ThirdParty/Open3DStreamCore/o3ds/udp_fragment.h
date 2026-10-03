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

#ifndef O3DS_UDP_FRAGMENT_H
#define O3DS_UDP_FRAGMENT_H

#include "o3ds_export.h"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <map>
#include <utility>

// UDP fragment wire header v2 (ADR 0009 item 5, TRB-17), 24 bytes,
// little-endian:
//   0-3   magic 'O','3','D','F' (a byte string)
//   4     version, 2
//   5     flags, 0
//   6-7   reserved, 0
//   8-11  message id
//   12-15 fragment index
//   16-19 total reassembled size
//   20-23 fragment payload size
// The magic makes a datagram's kind certain from its first 4 bytes
// (udpClassifyDatagram). The legacy 16-byte header (the four u32 without a
// magic) is not accepted: its first word, the message id, could look like a
// frame word.
static const size_t kUdpFragmentHeaderSize = 24;
static const unsigned char kUdpFragmentMagic[4] = { 'O', '3', 'D', 'F' };
static const unsigned char kUdpFragmentVersion = 2;

// Largest UDP payload over IPv4 (65535 - 8 byte UDP header - 20 byte IP header).
static const size_t kUdpMaxDatagramSize = 65507;

struct UdpFragmentHeader
{
	uint32_t id;
	uint32_t seq;
	uint32_t totalSize;
	uint32_t fragSize;
};

// Writes the 24-byte v2 header to out (no alignment requirement).
O3DS_API void writeUdpFragmentHeader(const UdpFragmentHeader& header, char* out);

// Reads the 24-byte v2 header from data (no alignment requirement). Returns
// false if sz < kUdpFragmentHeaderSize, or the magic, version, flags or
// reserved bytes are not those of a v2 header.
O3DS_API bool readUdpFragmentHeader(const char* data, size_t sz, UdpFragmentHeader& out);

// What a UDP datagram is, from its first 4 bytes (ADR 0009 item 5).
enum class UdpDatagramKind
{
	Fragment, // 'O3DF': a fragment, reassembled before anything else sees it
	Envelope, // 'O3DU' (the unified envelope): audio, control
	Frame,    // a frame word: byte 0 non-zero, bytes 1-3 zero (any version, so a
	          // newer one still reaches the parser and is reported as such)
	Unknown,  // anything else, including a legacy magic-less fragment whose id
	          // does not look like a frame word; drop it and count it
};
O3DS_API UdpDatagramKind udpClassifyDatagram(const char* data, size_t sz);

// Serial-number comparison (RFC 1982 style) for wrapping 32-bit message ids:
// true if a is "before" b, treating the id space as a circle.
O3DS_API bool udpMessageIdLess(uint32_t a, uint32_t b);

class O3DS_API UdpFragmenter
{
public:

	// inFragSize is the payload bytes per fragment. A zero fragment size, or
	// a size that cannot be represented in the 32-bit header, yields
	// mFrames == 0 (nothing to send).
	UdpFragmenter(const char* data, size_t sz, size_t inFragSize);

	// Writes fragment inSeq of the message into out (header + payload).
	// Returns false (and leaves out empty) if inSeq >= mFrames.
	bool makeFragment(uint32_t id, uint32_t seq, std::vector<char> &out) const;

	size_t mFragmentSize;
	std::vector<char> mBuffer;
	size_t mBufferSize;
	size_t mFrames;
};


// Default reassembly limits. See UdpReassemblyConfig.
static const size_t   kUdpDefaultMaxMessageSize   = 4u * 1024u * 1024u;  // 4 MiB
static const size_t   kUdpDefaultMaxInFlight      = 8;
static const size_t   kUdpDefaultMaxTotalBytes    = 16u * 1024u * 1024u; // 16 MiB
static const uint64_t kUdpDefaultMessageTimeoutMs = 100;

// Reassembles one message. The first accepted fragment locks the message's
// total size, fragment size and fragment count; any later fragment that
// disagrees is rejected. Owns its buffer via std::vector (rule of zero).
class O3DS_API UdpCombiner
{
public:
	explicit UdpCombiner(size_t maxMessageSize = kUdpDefaultMaxMessageSize);

	// Checks a fragment for internal consistency (non-zero sizes, total size
	// within maxMessageSize, index in range, payload length exactly filling
	// its slot) without any per-message state. Never allocates.
	static bool validateFragment(const UdpFragmentHeader& header, size_t payloadLen, size_t maxMessageSize);

	bool addFragment(const char* data, size_t sz);

	// True once every fragment has arrived. False for a combiner that has
	// accepted nothing yet (so an empty combiner never yields an empty frame).
	bool isComplete() const;

	// Bytes reserved for this message (0 until the first fragment is accepted).
	size_t reservedBytes() const { return mBufferSize; }

	size_t mMaxMessageSize;
	size_t mFrameSize;          // locked fragment payload size
	std::vector<char> mBuffer;  // reassembled bytes, mBufferSize long once complete
	size_t mBufferSize;         // locked total size
	size_t mFrames;             // locked fragment count
	size_t mReceived;           // distinct fragments received

	std::vector<bool> mFound;
};

struct UdpReassemblyConfig
{
	// Max reassembled message size; larger claimed sizes are rejected.
	size_t maxMessageSize = kUdpDefaultMaxMessageSize;
	// Max concurrently tracked messages (all sources); the oldest is evicted.
	size_t maxInFlightMessages = kUdpDefaultMaxInFlight;
	// Max bytes reserved across all tracked messages; the oldest are evicted.
	size_t maxTotalBytes = kUdpDefaultMaxTotalBytes;
	// Incomplete messages older than this (from their first fragment) expire.
	uint64_t messageTimeoutMs = kUdpDefaultMessageTimeoutMs;
};

struct UdpReassemblyStats
{
	uint64_t fragmentsAccepted = 0;
	uint64_t fragmentsRejected = 0;
	uint64_t messagesCompleted = 0;
	uint64_t messagesEvicted = 0;   // dropped to respect count/bytes limits
	uint64_t messagesExpired = 0;   // dropped after messageTimeoutMs
	uint64_t messagesSuperseded = 0; // dropped because a newer id from the same source completed
};

// Reassembles messages from many sources. Keyed on (sourceKey, message id);
// sourceKey is an opaque caller-supplied value (e.g. a hash of the sender
// address). Time is passed in explicitly (monotonic milliseconds) so the
// behaviour is deterministic and testable; no clock is read internally.
// Not thread-safe.
class O3DS_API UdpMapper
{
public:
	UdpMapper();
	explicit UdpMapper(const UdpReassemblyConfig& config);

	// Returns true if the fragment was accepted. A rejected fragment never
	// creates state and never disturbs other in-progress messages.
	bool addFragment(uint64_t sourceKey, const char* data, size_t sz, uint64_t nowMs);

	// Pops the oldest completed message into out (replacing its contents).
	// Never yields an empty frame. Completing a message drops incomplete
	// messages from the same source with an older id (serial comparison).
	bool getFrame(std::vector<char> &out);

	// Drops incomplete messages whose first fragment is older than the timeout.
	void expire(uint64_t nowMs);

	void clear();

	size_t inFlightMessages() const { return mItems.size(); }
	size_t bytesInUse() const { return mBytesInUse; }
	const UdpReassemblyStats& stats() const { return mStats; }
	const UdpReassemblyConfig& config() const { return mConfig; }

private:
	typedef std::pair<uint64_t, uint32_t> Key; // (sourceKey, message id)

	struct Entry
	{
		UdpCombiner combiner;
		uint64_t firstSeenMs = 0;
		uint64_t order = 0; // creation order, for deterministic oldest-first eviction
	};

	void erase(std::map<Key, Entry>::iterator it);
	bool evictOldest();

	UdpReassemblyConfig mConfig;
	std::map<Key, Entry> mItems;
	size_t mBytesInUse = 0;
	uint64_t mNextOrder = 0;
	UdpReassemblyStats mStats;
};

#endif
