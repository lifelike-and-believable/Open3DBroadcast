// Fuzz target: the control channel reader (src/o3ds/control.h):
// ParseMessage() on untrusted payloads, then ControlReceiver and
// ControlAligner fed whatever it accepts (docs/adr/0011-control-channel.md).
//
// Input:
//   byte 0: receiver limits selector: max_keys_per_source = 1 + (b % 16),
//           dedupe_window = 1 + (b >> 4)
//   then up to kMaxRecords [u16 length][bytes] records, each one control
//   payload (the bytes inside the envelope), delivered 10 ms apart.
//
// Invariants:
//   - an accepted message passes Validate(), and re-encoding it either
//     fails for size alone or parses back to the same message;
//   - the receiver never holds more values for a source than its key cap;
//   - every change the receiver reports names a source it was given;
//   - the aligner releases everything on Flush().
#include "fuzz_support.h"

#include "o3ds/control.h"

#include <set>
#include <string>

using namespace O3DS::Control;

namespace
{
	const size_t kMaxRecords = 64;

	bool SameMessage(const Message& a, const Message& b)
	{
		if (a.source_id != b.source_id || a.source_name != b.source_name || a.epoch != b.epoch || a.seq != b.seq
			|| a.sender_time_us != b.sender_time_us || a.tx_wallclock_us != b.tx_wallclock_us || a.mocap_subjects != b.mocap_subjects
			|| a.snapshot_id != b.snapshot_id || a.snapshot_part != b.snapshot_part || a.snapshot_parts != b.snapshot_parts
			|| a.snapshot_seq != b.snapshot_seq || a.set.size() != b.set.size() || a.clear.size() != b.clear.size()
			|| a.events.size() != b.events.size())
			return false;
		for (size_t k = 0; k < a.set.size(); ++k)
		{
			if (a.set[k].key != b.set[k].key || a.set[k].target != b.set[k].target || a.set[k].version != b.set[k].version
				|| a.set[k].value != b.set[k].value)
				return false;
		}
		for (size_t k = 0; k < a.clear.size(); ++k)
		{
			if (a.clear[k].key != b.clear[k].key || a.clear[k].target != b.clear[k].target || a.clear[k].version != b.clear[k].version)
				return false;
		}
		for (size_t k = 0; k < a.events.size(); ++k)
		{
			const Event& x = a.events[k];
			const Event& y = b.events[k];
			if (x.event_id != y.event_id || x.name != y.name || x.target != y.target || x.value != y.value || x.ttl_ms != y.ttl_ms
				|| x.time_us != y.time_us)
				return false;
		}
		return true;
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 1)
		return 0;

	ReceiverConfig config;
	config.max_keys_per_source = 1 + (data[0] % 16);
	config.dedupe_window = 1 + (data[0] >> 4);
	ControlReceiver receiver(config);
	ControlAligner aligner(0.05);

	std::set<std::string> sources;
	std::vector<Change> changes;
	double now = 0.0;

	for (const o3ds_fuzz::Record& record : o3ds_fuzz::SplitRecords(data + 1, size - 1, kMaxRecords))
	{
		now += 0.01;

		Message parsed;
		const ParseError error = ParseMessage(record.data, record.size, parsed);
		if (error == ParseError::None)
		{
			O3DS_FUZZ_ASSERT(Validate(parsed) == ParseError::None);
			sources.insert(parsed.source_id);

			std::vector<uint8_t> again;
			if (SerializeMessage(parsed, again))
			{
				Message reparsed;
				O3DS_FUZZ_ASSERT(ParseMessage(again.data(), again.size(), reparsed) == ParseError::None);
				O3DS_FUZZ_ASSERT(SameMessage(parsed, reparsed));
			}
		}

		changes.clear();
		receiver.Submit(record.data, record.size, now, changes);
		receiver.Tick(now, changes);
		for (Change& change : changes)
		{
			O3DS_FUZZ_ASSERT(sources.count(change.source_id) == 1);
			aligner.Push(std::move(change), now);
		}

		for (const std::string& source : sources)
			O3DS_FUZZ_ASSERT(receiver.GetValues(source).size() <= config.max_keys_per_source);

		std::vector<Change> released;
		aligner.Release(now, [&](const std::string& source, uint64_t& presentingUs)
		{
			presentingUs = static_cast<uint64_t>(now * 1.0e6);
			return (source.size() % 2) == 0;
		}, released);
	}

	std::vector<Change> rest;
	aligner.Flush(rest);
	O3DS_FUZZ_ASSERT(aligner.NumHeld() == 0);
	return 0;
}
