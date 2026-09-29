// Fuzz target: SubjectList::Parse() on an arbitrary FlatBuffers payload.
//
// Input: the payload only. WrapPayload() adds a valid flags/CRC header so
// mutations reach the FlatBuffers verifier and the model layer instead of
// stopping at the checksum. The raw input is also parsed as-is, to cover
// the header checks themselves.
//
// A successful parse is followed by a second parse of the same buffer into
// the same list (the "subjects already known" update path), then a
// re-serialize and re-parse of whatever state the hostile buffer produced,
// which is what a relay (apps/Repeater) does with it.
#include "fuzz_support.h"

#include "o3ds/model.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	{
		O3DS::SubjectList raw;
		raw.Parse(reinterpret_cast<const char*>(data), size);
	}

	const std::vector<char> wire = o3ds_fuzz::WrapPayload(data, size);

	O3DS::SubjectList list;
	if (!list.Parse(wire.data(), wire.size(), nullptr, /*clearInactive*/ true))
		return 0;

	list.Parse(wire.data(), wire.size(), nullptr, /*clearInactive*/ false);

	std::vector<char> again;
	if (list.Serialize(again, 1.0) > 0)
	{
		O3DS::SubjectList reparsed;
		reparsed.Parse(again.data(), again.size());
	}

	return 0;
}
