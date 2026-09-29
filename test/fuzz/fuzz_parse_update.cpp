// Fuzz target: the update half of SubjectList::Parse() - ParseUpdate()
// (legacy and D1-quantized) and ParseUpdateResidual(), which Parse()
// dispatches to per SubjectUpdate on its predictor_id.
//
// ParseUpdate() is not a byte-level entry point of its own: it takes an
// already-verified SubjectUpdate and indexes into subjects the receiver
// learned from an earlier keyframe. So each run first parses the canonical
// keyframe (fuzz_support.cpp), then parses the fuzzed payload into the same
// list with clearInactive = false, exactly like a live receiver.
//
// Input: the FlatBuffers payload of one update buffer; the header is added
// by WrapPayload().
#include "fuzz_support.h"

#include "o3ds/model.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::vector<char>& keyframe = o3ds_fuzz::CanonicalKeyframe();

	O3DS::SubjectList list;
	O3DS_FUZZ_ASSERT(list.Parse(keyframe.data(), keyframe.size()));

	const std::vector<char> wire = o3ds_fuzz::WrapPayload(data, size);
	list.Parse(wire.data(), wire.size(), nullptr, /*clearInactive*/ false);

	// Sender-side code run over whatever state the update left behind
	// (a relay re-serializes what it received).
	size_t count = 0;
	std::vector<char> out;
	list.SerializeUpdate(out, count, 1.0);

	return 0;
}
