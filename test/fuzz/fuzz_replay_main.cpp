// Deterministic fallback driver for the O3DS fuzz targets, for compilers
// without libFuzzer (GCC, MSVC). Linked into `o3ds_fuzz_<target>_replay`
// in place of libFuzzer's main(); see test/fuzz/CMakeLists.txt.
//
//   o3ds_fuzz_<target>_replay [--iterations N] [--max-seconds S] [--seed X]
//       Runs every seed from SeedsFor(<target>) once, then N mutated inputs
//       (default 20000) derived from them by a fixed-seed PRNG, stopping
//       early (successfully) once S seconds (default 120) have elapsed.
//       The input sequence depends only on --seed and the seed corpus, so
//       a failure reproduces exactly on every run and every machine.
//
//   o3ds_fuzz_<target>_replay FILE...
//       Runs each file once as a single input: replays a libFuzzer crash
//       file, or the o3ds-fuzz-crash-<target>.bin this driver writes, on a
//       machine without libFuzzer.
//
// This is not a coverage-guided fuzzer and finds far less than libFuzzer
// does; it is a regression net that runs everywhere CTest does. A crash
// shows up as a sanitizer report (when built with O3DS_ENABLE_SANITIZERS)
// or an O3DS_FUZZ_ASSERT failure, and fails the CTest entry.
#include "fuzz_support.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// With ASan, save the failing input from its death callback too, so a
// sanitizer report (not just O3DS_FUZZ_ASSERT) leaves a replayable file.
#if defined(__SANITIZE_ADDRESS__)
#define O3DS_FUZZ_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define O3DS_FUZZ_ASAN 1
#endif
#endif
#if defined(O3DS_FUZZ_ASAN) && defined(__has_include)
#if __has_include(<sanitizer/common_interface_defs.h>)
#include <sanitizer/common_interface_defs.h>
#define O3DS_FUZZ_HAVE_DEATH_CALLBACK 1
#endif
#endif

#ifndef O3DS_FUZZ_TARGET_NAME
#error "O3DS_FUZZ_TARGET_NAME must be defined (see test/fuzz/CMakeLists.txt)"
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace
{
	const size_t kMaxInputSize = 64 * 1024;

	// The input currently being run, so a failure can save it for replay.
	const o3ds_fuzz::Bytes* gCurrentInput = nullptr;
	bool gSaved = false;

	void SaveCurrentInput()
	{
		if (gSaved || gCurrentInput == nullptr)
			return;
		gSaved = true;
		const std::string path = std::string("o3ds-fuzz-crash-") + O3DS_FUZZ_TARGET_NAME + ".bin";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(gCurrentInput->data()), (std::streamsize)gCurrentInput->size());
		std::fprintf(stderr, "Saved failing input (%zu bytes) to %s - replay it with: o3ds_fuzz_%s_replay %s\n",
			gCurrentInput->size(), path.c_str(), O3DS_FUZZ_TARGET_NAME, path.c_str());
	}

	// UBSan with halt_on_error and abort_on_error (set on the CTest entry)
	// ends in abort(), which doesn't go through ASan's death callback.
	void OnSignal(int sig)
	{
		SaveCurrentInput();
		std::signal(sig, SIG_DFL);
		std::raise(sig);
	}

	void Run(const o3ds_fuzz::Bytes& input)
	{
		gCurrentInput = &input;
		LLVMFuzzerTestOneInput(input.empty() ? nullptr : input.data(), input.size());
		gCurrentInput = nullptr;
	}

	// SplitMix64: tiny, fast, and identical output on every platform.
	struct Rng
	{
		uint64_t state;

		uint64_t Next()
		{
			uint64_t z = (state += 0x9e3779b97f4a7c15ull);
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}

		size_t Below(size_t n) { return n == 0 ? 0 : (size_t)(Next() % n); }
	};

	const uint32_t kInteresting[] = {
		0, 1, 2, 3, 4, 7, 8, 16, 32, 64, 100, 127, 128, 255, 256, 512, 1000, 1024,
		4096, 32767, 32768, 65535, 65536, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu,
	};

	void Mutate(o3ds_fuzz::Bytes& data, const std::vector<o3ds_fuzz::Bytes>& seeds, Rng& rng)
	{
		const size_t count = 1 + rng.Below(6);
		for (size_t m = 0; m < count; ++m)
		{
			switch (rng.Below(data.empty() ? 1 : 10))
			{
			case 0: // insert random bytes
			{
				const size_t n = 1 + rng.Below(8);
				const size_t at = rng.Below(data.size() + 1);
				for (size_t i = 0; i < n; ++i)
					data.insert(data.begin() + (std::ptrdiff_t)at, (uint8_t)rng.Next());
				break;
			}
			case 1: // flip one bit
				data[rng.Below(data.size())] ^= (uint8_t)(1u << rng.Below(8));
				break;
			case 2: // random byte
				data[rng.Below(data.size())] = (uint8_t)rng.Next();
				break;
			case 3: // interesting byte
				data[rng.Below(data.size())] = (uint8_t)kInteresting[rng.Below(sizeof(kInteresting) / sizeof(kInteresting[0]))];
				break;
			case 4: // interesting 16/32-bit little-endian value
			{
				const uint32_t v = kInteresting[rng.Below(sizeof(kInteresting) / sizeof(kInteresting[0]))];
				const size_t width = rng.Below(2) ? 4 : 2;
				const size_t at = rng.Below(data.size());
				for (size_t i = 0; i < width && at + i < data.size(); ++i)
					data[at + i] = (uint8_t)(v >> (8 * i));
				break;
			}
			case 5: // add a small delta to a 32-bit little-endian value
			{
				if (data.size() < 4) break;
				const size_t at = rng.Below(data.size() - 3);
				uint32_t v = 0;
				for (size_t i = 0; i < 4; ++i) v |= (uint32_t)data[at + i] << (8 * i);
				v += (uint32_t)(int32_t)((int)rng.Below(33) - 16);
				for (size_t i = 0; i < 4; ++i) data[at + i] = (uint8_t)(v >> (8 * i));
				break;
			}
			case 6: // erase a range
			{
				const size_t at = rng.Below(data.size());
				const size_t n = 1 + rng.Below(std::min<size_t>(16, data.size() - at));
				data.erase(data.begin() + (std::ptrdiff_t)at, data.begin() + (std::ptrdiff_t)(at + n));
				break;
			}
			case 7: // copy a chunk of the input over another position
			{
				const size_t from = rng.Below(data.size());
				const size_t to = rng.Below(data.size());
				const size_t n = 1 + rng.Below(std::min<size_t>(32, std::min(data.size() - from, data.size() - to)));
				std::memmove(data.data() + to, data.data() + from, n);
				break;
			}
			case 8: // splice in a chunk of another seed
			{
				const o3ds_fuzz::Bytes& other = seeds[rng.Below(seeds.size())];
				if (other.empty()) break;
				const size_t from = rng.Below(other.size());
				const size_t n = 1 + rng.Below(std::min<size_t>(64, other.size() - from));
				const size_t at = rng.Below(data.size() + 1);
				data.insert(data.begin() + (std::ptrdiff_t)at, other.begin() + (std::ptrdiff_t)from, other.begin() + (std::ptrdiff_t)(from + n));
				break;
			}
			default: // truncate
				data.resize(rng.Below(data.size()));
				break;
			}
		}

		if (data.size() > kMaxInputSize)
			data.resize(kMaxInputSize);
	}

	bool ReadFile(const char* path, o3ds_fuzz::Bytes& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		return true;
	}
}

int main(int argc, char** argv)
{
	o3ds_fuzz::gOnAssertFailure = SaveCurrentInput;
#ifdef O3DS_FUZZ_HAVE_DEATH_CALLBACK
	__sanitizer_set_death_callback(SaveCurrentInput);
#endif
	std::signal(SIGABRT, OnSignal);
#ifndef O3DS_FUZZ_ASAN
	// Under ASan its own SEGV handler reports the crash (and then runs the
	// death callback above); replacing it would lose the report.
	std::signal(SIGSEGV, OnSignal);
#endif

	long long iterations = 20000;
	double maxSeconds = 120.0;
	uint64_t seed = 0x03d5f0220260929ull;
	std::vector<const char*> files;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--iterations" && i + 1 < argc) iterations = std::atoll(argv[++i]);
		else if (arg == "--max-seconds" && i + 1 < argc) maxSeconds = std::atof(argv[++i]);
		else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
		else if (arg.size() > 1 && arg[0] == '-')
		{
			std::fprintf(stderr, "usage: %s [--iterations N] [--max-seconds S] [--seed X] [FILE...]\n", argv[0]);
			return 2;
		}
		else files.push_back(argv[i]);
	}

	if (!files.empty())
	{
		for (const char* path : files)
		{
			o3ds_fuzz::Bytes input;
			if (!ReadFile(path, input))
			{
				std::fprintf(stderr, "cannot read %s\n", path);
				return 2;
			}
			std::printf("[%s] replaying %s (%zu bytes)\n", O3DS_FUZZ_TARGET_NAME, path, input.size());
			Run(input);
		}
		std::printf("[%s] replayed %zu file(s) without a crash\n", O3DS_FUZZ_TARGET_NAME, files.size());
		return 0;
	}

	const std::vector<o3ds_fuzz::Bytes> seeds = o3ds_fuzz::SeedsFor(O3DS_FUZZ_TARGET_NAME);
	if (seeds.empty())
	{
		std::fprintf(stderr, "no seeds for target '%s'\n", O3DS_FUZZ_TARGET_NAME);
		return 2;
	}

	for (const auto& s : seeds)
		Run(s);

	const auto start = std::chrono::steady_clock::now();
	Rng rng{ seed };
	long long done = 0;
	for (; done < iterations; ++done)
	{
		if ((done & 255) == 0)
		{
			const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			if (elapsed > maxSeconds)
			{
				std::printf("[%s] time budget of %.0f s reached\n", O3DS_FUZZ_TARGET_NAME, maxSeconds);
				break;
			}
		}

		o3ds_fuzz::Bytes input = seeds[rng.Below(seeds.size())];
		Mutate(input, seeds, rng);
		Run(input);
	}

	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::printf("[%s] %zu seed(s) + %lld mutated input(s) in %.1f s, seed 0x%llx: no crash\n",
		O3DS_FUZZ_TARGET_NAME, seeds.size(), done, elapsed, (unsigned long long)seed);
	return 0;
}
