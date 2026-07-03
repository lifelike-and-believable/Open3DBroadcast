cmake_minimum_required(VERSION 3.13)

# Single executable = single CTest entry for now; the binary reports each
# O3DS_TEST's pass/fail on stdout (see main.cpp), and `ctest --output-on-failure`
# surfaces that output when the aggregate exits non-zero. Splitting into
# per-test CTest entries can be added later without changing how tests are
# written (see test_framework.h).
add_executable(o3ds_core_tests
	test_framework.h
	main.cpp
	model_tests.cpp
	sequencing_tests.cpp
	reorder_gate_tests.cpp
)

target_link_libraries(o3ds_core_tests PRIVATE open3dstreamstatic)

add_test(NAME o3ds_core_tests COMMAND o3ds_core_tests)
