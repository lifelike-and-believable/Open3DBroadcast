// Writes the seed corpus of every fuzz target to <dir>/<target>/seed-NN,
// the layout libFuzzer takes as a corpus directory:
//
//   o3ds_fuzz_make_seeds build/test/fuzz/corpus
//   build/test/fuzz/o3ds_fuzz_parse build/test/fuzz/corpus/parse -max_total_time=60
//
// The `o3ds_fuzz_corpus` CMake target runs this into the build tree. Seeds
// are generated rather than committed so they always match the current
// schema and serializer.
#include "fuzz_support.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
		return 2;
	}

	namespace fs = std::filesystem;
	const fs::path root(argv[1]);
	size_t total = 0;
	size_t bytes = 0;

	for (const std::string& target : o3ds_fuzz::TargetNames())
	{
		const fs::path dir = root / target;
		fs::create_directories(dir);

		const auto seeds = o3ds_fuzz::SeedsFor(target);
		for (size_t i = 0; i < seeds.size(); ++i)
		{
			char name[32];
			std::snprintf(name, sizeof(name), "seed-%02zu", i);
			std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(seeds[i].data()), (std::streamsize)seeds[i].size());
			if (!out)
			{
				std::fprintf(stderr, "failed to write %s\n", (dir / name).string().c_str());
				return 1;
			}
			bytes += seeds[i].size();
		}
		total += seeds.size();
		std::printf("%s: %zu seed(s)\n", target.c_str(), seeds.size());
	}

	std::printf("wrote %zu seed(s), %zu bytes, under %s\n", total, bytes, root.string().c_str());
	return 0;
}
