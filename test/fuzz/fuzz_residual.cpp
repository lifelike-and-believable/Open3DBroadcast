// Fuzz target: the C2 residual decoder (SubjectList::ParseUpdateResidual()
// and the per-subject ResidualDecoder it drives) across a *sequence* of
// frames. Residual decoding is stateful - each frame is reconstructed
// against the decoder's prediction from earlier frames - so a single-buffer
// target would never exercise history, predictor switches mid-stream or
// keyframe/non-keyframe interleaving.
//
// Input: `[u16 length][payload]...` (see SplitRecords()), at most
// kMaxFrames frames. Each payload gets a valid header from WrapPayload()
// and is parsed, in order, into one receiver that was first primed with the
// canonical keyframe.
#include "fuzz_support.h"

#include "o3ds/model.h"

namespace
{
	const size_t kMaxFrames = 64;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::vector<char>& keyframe = o3ds_fuzz::CanonicalKeyframe();

	O3DS::SubjectList list;
	O3DS_FUZZ_ASSERT(list.Parse(keyframe.data(), keyframe.size()));

	for (const auto& frame : o3ds_fuzz::SplitRecords(data, size, kMaxFrames))
	{
		const std::vector<char> wire = o3ds_fuzz::WrapPayload(frame.data, frame.size);
		list.Parse(wire.data(), wire.size(), nullptr, /*clearInactive*/ false);
	}

	return 0;
}
