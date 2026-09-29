// Shared helpers for the O3DS core fuzz targets (test/fuzz/fuzz_*.cpp).
//
// Every target is a plain LLVMFuzzerTestOneInput(), so the same source file
// builds two ways (see test/fuzz/CMakeLists.txt):
//   - with Clang + O3DS_BUILD_FUZZERS=ON, linked against libFuzzer
//     (`o3ds_fuzz_<target>`), for coverage-guided fuzzing;
//   - with any compiler, linked against fuzz_replay_main.cpp
//     (`o3ds_fuzz_<target>_replay`), a deterministic fixed-seed mutation
//     runner registered in CTest under the `fuzz` label, for machines
//     without libFuzzer (GCC, MSVC).
//
// Seeds are generated from real SubjectList serializations (SeedsFor()
// below), never committed as binaries: `o3ds_fuzz_make_seeds <dir>` writes
// them out as a libFuzzer corpus, and the replay runner builds them in
// memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace o3ds_fuzz
{
	using Bytes = std::vector<uint8_t>;

	//! Names of every fuzz target, in the order CMake registers them. Each
	//! name matches a fuzz_<name>.cpp and a SeedsFor(<name>) corpus.
	const std::vector<std::string>& TargetNames();

	//! Deterministic seed corpus for one target (empty for an unknown name).
	std::vector<Bytes> SeedsFor(const std::string& target);

	//! The full wire buffer (8-byte header + FlatBuffers payload) of the
	//! canonical keyframe every update/residual seed was generated against.
	//! Update-parsing targets Parse() this first, so fuzzed updates land on
	//! a receiver that already knows the subjects they name.
	const std::vector<char>& CanonicalKeyframe();

	//! Prepends the 8-byte O3DS wire header (flags = 1, CRC32 of the
	//! payload). Parse() rejects any buffer whose CRC doesn't match before
	//! it looks at the payload, so the parse targets fuzz the payload and
	//! let this fix the checksum up - otherwise almost every mutation would
	//! stop at the CRC check.
	std::vector<char> WrapPayload(const uint8_t* data, size_t size);

	//! Splits `[u16 little-endian length][bytes]...` into at most
	//! `maxRecords` records. A truncated final record gets whatever bytes
	//! remain, so every input decodes to something.
	struct Record
	{
		const uint8_t* data;
		size_t size;
	};
	std::vector<Record> SplitRecords(const uint8_t* data, size_t size, size_t maxRecords);

	//! Appends one `[u16 length][bytes]` record (the inverse of SplitRecords).
	void AppendRecord(Bytes& out, const uint8_t* data, size_t size);

	//! Called by O3DS_FUZZ_ASSERT before aborting. The replay runner points
	//! it at a function that saves the failing input to disk; under
	//! libFuzzer it stays null because libFuzzer saves crash inputs itself.
	extern void (*gOnAssertFailure)();
}

//! Invariant check that fails the fuzz run (libFuzzer or replay) the same
//! way a sanitizer report does. Works in Release builds, unlike assert().
#define O3DS_FUZZ_ASSERT(cond) \
	do { \
		if (!(cond)) { \
			std::fprintf(stderr, "O3DS_FUZZ_ASSERT failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
			if (o3ds_fuzz::gOnAssertFailure) o3ds_fuzz::gOnAssertFailure(); \
			std::abort(); \
		} \
	} while (0)
