// Acceptance tests for src/o3ds/replay.h, per the roadmap doc's B2 spec
// (docs/roadmap/resilient-streaming-and-motion-prediction.md, section
// "Phase B2").
#include "test_framework.h"

#include "o3ds/replay.h"
#include "o3ds/model.h"

#include <sstream>

using namespace O3DS;

namespace
{
	// Builds a real, valid on-the-wire O3DS frame carrying the given tx_seq
	// - not a synthetic/fake buffer, so PeekMeta inside ReplayCapture
	// exercises the same code path a live receiver would.
	std::vector<char> MakeWireFrame(uint64_t seq)
	{
		SubjectList subjects;
		subjects.addSubject("Performer")->addTransform("Root", -1);
		std::vector<char> buffer;
		subjects.Serialize(buffer, /*timestamp*/ 1.0, seq, /*tx_wallclock_us*/ 0, /*frame_epoch*/ 0);
		return buffer;
	}

	// Writes a header + N sequential records (tx_seq 1..count) to `out`.
	void WriteTestCapture(std::ostream& out, int count, uint64_t startRecvUs = 1000000, uint64_t stepUs = 16667)
	{
		WriteCaptureHeader(out, CaptureHeaderInfo());
		for (int i = 1; i <= count; i++)
		{
			CaptureRecord r;
			r.recv_wallclock_us = startRecvUs + (uint64_t)(i - 1) * stepUs;
			r.wire_bytes = MakeWireFrame((uint64_t)i);
			WriteCaptureRecord(out, r);
		}
	}
}

O3DS_TEST(Replay_EmitsFramesWithCorrectSeqExtractedFromWireBytes)
{
	std::ostringstream out;
	WriteTestCapture(out, 10);

	std::istringstream in(out.str());
	ReplayConfig config;
	config.timing = ReplayTiming::AsFastAsPossible;

	std::vector<uint64_t> seqLog;
	bool ok = ReplayCapture(in, config, [&](Frame&& f, double /*t*/) {
		seqLog.push_back(f.seq);
		return true;
	});

	O3DS_CHECK(ok);
	O3DS_CHECK_EQ(seqLog.size(), (size_t)10);
	for (int i = 0; i < 10; i++)
		O3DS_CHECK_EQ(seqLog[(size_t)i], (uint64_t)(i + 1));
}

O3DS_TEST(Replay_CaptureTimeMatchesRecvWallclock)
{
	std::ostringstream out;
	WriteTestCapture(out, 3, /*startRecvUs*/ 5000000, /*stepUs*/ 1000000); // 5.0s, 6.0s, 7.0s

	std::istringstream in(out.str());
	ReplayConfig config;

	std::vector<double> times;
	ReplayCapture(in, config, [&](Frame&&, double t) {
		times.push_back(t);
		return true;
	});

	O3DS_CHECK_EQ(times.size(), (size_t)3);
	O3DS_CHECK(times[0] > 4.999 && times[0] < 5.001);
	O3DS_CHECK(times[1] > 5.999 && times[1] < 6.001);
	O3DS_CHECK(times[2] > 6.999 && times[2] < 7.001);
}

O3DS_TEST(Replay_InvalidWireBytesTreatedAsLegacyNotDropped)
{
	std::ostringstream out;
	WriteCaptureHeader(out, CaptureHeaderInfo());
	CaptureRecord garbage;
	garbage.recv_wallclock_us = 1;
	garbage.wire_bytes = { 'n', 'o', 't', ' ', 'a', ' ', 'f', 'r', 'a', 'm', 'e' };
	WriteCaptureRecord(out, garbage);

	std::istringstream in(out.str());
	ReplayConfig config;

	int emitted = 0;
	uint64_t seq = 123; // pre-seed non-zero to prove it gets reset
	ReplayCapture(in, config, [&](Frame&& f, double) {
		emitted++;
		seq = f.seq;
		return true;
	});

	O3DS_CHECK_EQ(emitted, 1); // not silently dropped
	O3DS_CHECK_EQ(seq, (uint64_t)0); // treated as legacy/unset, matching ReorderGate's own bypass
}

O3DS_TEST(Replay_SinkReturningFalse_StopsEarly)
{
	std::ostringstream out;
	WriteTestCapture(out, 20);

	std::istringstream in(out.str());
	ReplayConfig config;

	int emitted = 0;
	bool ok = ReplayCapture(in, config, [&](Frame&&, double) {
		emitted++;
		return emitted < 5; // stop after 5
	});

	O3DS_CHECK(ok);
	O3DS_CHECK_EQ(emitted, 5);
}

O3DS_TEST(Replay_Loop_RewindsAndContinuesUntilSinkStops)
{
	std::ostringstream out;
	WriteTestCapture(out, 4); // small capture, so looping is easy to observe

	std::istringstream in(out.str());
	ReplayConfig config;
	config.loop = true;

	std::vector<uint64_t> seqLog;
	bool ok = ReplayCapture(in, config, [&](Frame&& f, double) {
		seqLog.push_back(f.seq);
		return seqLog.size() < 10; // stop partway through the third pass
	});

	O3DS_CHECK(ok);
	O3DS_CHECK_EQ(seqLog.size(), (size_t)10);
	// Confirms an actual rewind happened: without it, only 4 frames would
	// ever be emitted and the sink's "stop at 10" condition would never
	// naturally be reached this way (replay would just end at 4).
	O3DS_CHECK_EQ(seqLog[0], (uint64_t)1);
	O3DS_CHECK_EQ(seqLog[4], (uint64_t)1); // start of the second pass
	O3DS_CHECK_EQ(seqLog[8], (uint64_t)1); // start of the third pass
}

O3DS_TEST(Replay_TruncatedCapture_StillEmitsWhatWasReadThenStopsCleanly)
{
	std::ostringstream out;
	WriteTestCapture(out, 5);
	std::string full = out.str();
	std::string truncated = full.substr(0, full.size() - 3); // cut mid-last-record

	std::istringstream in(truncated);
	ReplayConfig config;

	int emitted = 0;
	bool ok = ReplayCapture(in, config, [&](Frame&&, double) {
		emitted++;
		return true;
	});

	O3DS_CHECK(ok); // truncation is not an error, per B1's own tolerance
	O3DS_CHECK_EQ(emitted, 4); // the 5th record was the one cut short
}

O3DS_TEST(Replay_InvalidHeader_ReturnsFalse)
{
	std::istringstream in(std::string("not a capture at all"));
	ReplayConfig config;

	bool sinkCalled = false;
	bool ok = ReplayCapture(in, config, [&](Frame&&, double) {
		sinkCalled = true;
		return true;
	});

	O3DS_CHECK(ok == false);
	O3DS_CHECK(sinkCalled == false);
}

O3DS_TEST(Replay_HeaderOutParam_Populated)
{
	std::ostringstream out;
	CaptureHeaderInfo info;
	info.source_desc = "replay test capture";
	WriteCaptureHeader(out, info);

	std::istringstream in(out.str());
	ReplayConfig config;
	CaptureHeaderInfo readHeader;

	ReplayCapture(in, config, [](Frame&&, double) { return true; }, &readHeader);

	O3DS_CHECK_EQ(readHeader.source_desc, std::string("replay test capture"));
}
