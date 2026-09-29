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
//
// Each test also records its source file; `o3ds_core_tests --suite <name>`
// runs only the tests from <name>.cpp, which is how test/CMakeLists.txt
// registers one CTest entry per test file.
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
		std::string suite; // source file name without directory or extension
	};

	inline std::string SuiteFromPath(const char* path)
	{
		std::string s(path);
		const size_t slash = s.find_last_of("/\\");
		if (slash != std::string::npos)
			s = s.substr(slash + 1);
		const size_t dot = s.rfind('.');
		if (dot != std::string::npos)
			s = s.substr(0, dot);
		return s;
	}

	inline std::vector<TestCase>& registry()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	struct Registrar
	{
		Registrar(const char* name, std::function<void()> fn, const char* file)
		{
			registry().push_back(TestCase{ name, std::move(fn), SuiteFromPath(file) });
		}
	};
}

#define O3DS_TEST_CONCAT_INNER(a, b) a##b
#define O3DS_TEST_CONCAT(a, b) O3DS_TEST_CONCAT_INNER(a, b)

#define O3DS_TEST(name) \
	static void O3DS_TEST_CONCAT(o3ds_test_fn_, name)(); \
	static o3ds_test::Registrar O3DS_TEST_CONCAT(o3ds_test_reg_, name)(#name, O3DS_TEST_CONCAT(o3ds_test_fn_, name), __FILE__); \
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
