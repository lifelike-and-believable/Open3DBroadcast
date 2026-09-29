cmake_minimum_required(VERSION 3.13)

# Adversarial-input harness for the core library's wire-facing entry points
# (WP-T1 in docs/roadmap/plugin-hardening-and-fab-readiness.md). One
# LLVMFuzzerTestOneInput() per target in fuzz_<target>.cpp, built two ways:
#
# - o3ds_fuzz_<target>_replay (always, with any compiler): the target linked
#   against fuzz_replay_main.cpp, a deterministic fixed-seed mutation runner.
#   Registered in CTest as fuzz.<target> under the `fuzz` label. Run with
#   `ctest -L fuzz`; `ctest -LE fuzz` runs the unit tests without them.
#
# - o3ds_fuzz_<target> (only with -DO3DS_BUILD_FUZZERS=ON, Clang): the same
#   target linked against libFuzzer. The top-level CMakeLists.txt adds the
#   coverage and sanitizer flags for the whole build in that mode, so the
#   core library is instrumented too. Build `o3ds_fuzz_corpus` for seed
#   corpora, then e.g.
#     test/fuzz/o3ds_fuzz_parse test/fuzz/corpus/parse -max_total_time=60

set(O3DS_FUZZ_TARGETS
	parse
	parse_update
	residual
	peek_meta
	udp_reassembly
	reorder_gate
)

add_library(o3ds_fuzz_support STATIC fuzz_support.h fuzz_support.cpp)
target_link_libraries(o3ds_fuzz_support PUBLIC open3dstreamstatic)
o3ds_enable_warnings(o3ds_fuzz_support)

foreach(target ${O3DS_FUZZ_TARGETS})
	add_executable(o3ds_fuzz_${target}_replay fuzz_${target}.cpp fuzz_replay_main.cpp)
	target_compile_definitions(o3ds_fuzz_${target}_replay PRIVATE O3DS_FUZZ_TARGET_NAME="${target}")
	target_link_libraries(o3ds_fuzz_${target}_replay PRIVATE o3ds_fuzz_support)
	o3ds_enable_warnings(o3ds_fuzz_${target}_replay)

	# 20000 mutated inputs per target, capped at 120 s (the runner stops
	# early and passes if it runs out of time, e.g. on a slow sanitizer
	# build). halt_on_error makes UBSan findings fail the test instead of
	# only printing, which is UBSan's default; abort_on_error lets the runner
	# save the failing input (see fuzz_replay_main.cpp).
	add_test(NAME fuzz.${target}
		COMMAND o3ds_fuzz_${target}_replay --iterations 20000 --max-seconds 120)
	set_tests_properties(fuzz.${target} PROPERTIES
		LABELS fuzz
		TIMEOUT 300
		ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1")

	if(O3DS_BUILD_FUZZERS)
		add_executable(o3ds_fuzz_${target} fuzz_${target}.cpp)
		target_link_libraries(o3ds_fuzz_${target} PRIVATE o3ds_fuzz_support)
		target_link_options(o3ds_fuzz_${target} PRIVATE -fsanitize=fuzzer)
		o3ds_enable_warnings(o3ds_fuzz_${target})
	endif()
endforeach()

add_executable(o3ds_fuzz_make_seeds make_seeds.cpp)
target_link_libraries(o3ds_fuzz_make_seeds PRIVATE o3ds_fuzz_support)
o3ds_enable_warnings(o3ds_fuzz_make_seeds)

add_custom_target(o3ds_fuzz_corpus
	COMMAND o3ds_fuzz_make_seeds ${CMAKE_CURRENT_BINARY_DIR}/corpus
	DEPENDS o3ds_fuzz_make_seeds
	COMMENT "Generating fuzz seed corpora under ${CMAKE_CURRENT_BINARY_DIR}/corpus"
	VERBATIM)
