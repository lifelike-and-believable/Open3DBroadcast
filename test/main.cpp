// Entry point for the O3DS core test suite. Runs every test registered via
// O3DS_TEST across the linked translation units and reports a summary.
// Exits non-zero if any test failed or threw, which is what `ctest` needs.
#include "test_framework.h"

#include <cstdio>
#include <exception>

int main()
{
	auto& tests = o3ds_test::registry();
	int failed = 0;

	std::printf("Running %zu test(s)...\n", tests.size());

	for (auto& test : tests)
	{
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

	std::printf("---\n%zu passed, %d failed (of %zu)\n", tests.size() - failed, failed, tests.size());

	return failed == 0 ? 0 : 1;
}
