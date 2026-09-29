// Entry point for the O3DS core test suite. Runs every test registered via
// O3DS_TEST across the linked translation units and reports a summary.
// Exits non-zero if any test failed or threw, which is what `ctest` needs.
//
//   o3ds_core_tests                 run every test
//   o3ds_core_tests --suite <name>  run only the tests defined in <name>.cpp
//                                   (fails if there are none, so a renamed
//                                   file can't silently register an empty
//                                   CTest entry)
#include "test_framework.h"

#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char** argv)
{
	const char* suite = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc)
		{
			suite = argv[++i];
		}
		else
		{
			std::printf("usage: %s [--suite <test-file-name>]\n", argv[0]);
			return 2;
		}
	}

	auto& tests = o3ds_test::registry();
	size_t run = 0;
	int failed = 0;

	for (auto& test : tests)
	{
		if (suite != nullptr && test.suite != suite)
			continue;

		run++;
		try
		{
			test.fn();
			std::printf("[PASS] %s\n", test.name.c_str());
		}
		catch (const o3ds_test::TestFailure& failure)
		{
			std::printf("[FAIL] %s: %s\n", test.name.c_str(), failure.message.c_str());
			failed++;
		}
		catch (const std::exception& ex)
		{
			std::printf("[FAIL] %s: unexpected exception: %s\n", test.name.c_str(), ex.what());
			failed++;
		}
		catch (...)
		{
			std::printf("[FAIL] %s: unknown exception\n", test.name.c_str());
			failed++;
		}
	}

	if (suite != nullptr && run == 0)
	{
		std::printf("No tests registered for suite '%s'\n", suite);
		return 1;
	}

	std::printf("---\n%zu passed, %d failed (of %zu)\n", run - failed, failed, run);

	return failed == 0 ? 0 : 1;
}
