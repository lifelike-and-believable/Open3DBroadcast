// Minimal, dependency-free test framework for the O3DS core library.
//
// No external test framework (gtest, catch2, ...) is pulled in on purpose:
// the core library already has zero third-party test dependencies, and
// pulling one in just for this would add a submodule + build step to every
// PR that touches src/o3ds. A single self-registering macro pair is enough
// for the assertions this suite needs.
//
// Usage:
//   O3DS_TEST(MyTestName)
//   {
//       O3DS_CHECK(1 + 1 == 2);
//   }
//
// All registered tests run from test/main.cpp. A failed O3DS_CHECK throws
// TestFailure, which main.cpp catches per-test so one failure doesn't abort
// the rest of the suite; the process exits non-zero if any test failed,
// which is all `ctest` needs to mark the test red.
#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace o3ds_test
{
	struct TestFailure
	{
		std::string message;
	};

	struct TestCase
	{
		std::string name;
		std::function<void()> fn;
	};

	inline std::vector<TestCase>& registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct Registrar
	{
		Registrar(const char* name, std::function<void()> fn)
		{
			registry().push_back(TestCase{ name, std::move(fn) });
		}
	};
}

#define O3DS_TEST_CONCAT_INNER(a, b) a##b
#define O3DS_TEST_CONCAT(a, b) O3DS_TEST_CONCAT_INNER(a, b)

#define O3DS_TEST(name) \
	static void O3DS_TEST_CONCAT(o3ds_test_fn_, name)(); \
	static o3ds_test::Registrar O3DS_TEST_CONCAT(o3ds_test_reg_, name)(#name, O3DS_TEST_CONCAT(o3ds_test_fn_, name)); \
	static void O3DS_TEST_CONCAT(o3ds_test_fn_, name)()

#define O3DS_CHECK(cond) \
	do { \
		if (!(cond)) { \
			std::ostringstream oss; \
			oss << "CHECK failed: " << #cond << " at " << __FILE__ << ":" << __LINE__; \
			throw o3ds_test::TestFailure{ oss.str() }; \
		} \
	} while (0)

#define O3DS_CHECK_EQ(a, b) \
	do { \
		if (!((a) == (b))) { \
			std::ostringstream oss; \
			oss << "CHECK_EQ failed: " << #a << " == " << #b \
				<< " (" << (a) << " != " << (b) << ") at " << __FILE__ << ":" << __LINE__; \
			throw o3ds_test::TestFailure{ oss.str() }; \
		} \
	} while (0)
