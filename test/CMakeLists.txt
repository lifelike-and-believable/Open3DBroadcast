cmake_minimum_required(VERSION 3.13)

# One executable, one CTest entry per test file: `o3ds_core_tests --suite
# <file>` runs only the O3DS_TESTs defined in <file>.cpp (see main.cpp), so
# a failure names the file in the ctest summary, while tests are still
# written exactly as before (see test_framework.h). A new *_tests.cpp only
# needs adding to this list.
set(O3DS_CORE_TEST_SOURCES
	model_tests.cpp
	sequencing_tests.cpp
	reorder_gate_tests.cpp
	capture_tests.cpp
	channel_model_tests.cpp
	replay_tests.cpp
	clock_offset_tests.cpp
	pose_predictor_tests.cpp
	concealment_tests.cpp
	udp_fragment_tests.cpp
	udp_reassembly_hardening_tests.cpp
	residual_codec_tests.cpp
	model_residual_tests.cpp
	channel_quant_tests.cpp
	channel_quant_wire_tests.cpp
	parser_hardening_tests.cpp
	receiver_correctness_tests.cpp
	sender_wire_tests.cpp
	tcp_stream_parser_tests.cpp
	control_codec_tests.cpp
	control_state_tests.cpp
	crc32_tests.cpp
	parse_reuse_tests.cpp
	wire_format_tests.cpp
	stream_writer_tests.cpp
	resync_contract_tests.cpp
	scale_update_tests.cpp
	residual_rotation_tests.cpp
)

add_executable(o3ds_core_tests
	test_framework.h
	main.cpp
	${O3DS_CORE_TEST_SOURCES}
)

target_link_libraries(o3ds_core_tests PRIVATE open3dstreamstatic)
o3ds_enable_warnings(o3ds_core_tests)

foreach(source ${O3DS_CORE_TEST_SOURCES})
	get_filename_component(suite ${source} NAME_WE)
	add_test(NAME core.${suite} COMMAND o3ds_core_tests --suite ${suite})
endforeach()

# Serialize/Parse benchmark (ADR 0008 item 11, CORE-7, CORE-18). It checks
# correctness only and prints the timings; CTest runs it with few iterations
# under the "bench" label. Run build/test/o3ds_core_bench by hand for numbers.
add_executable(o3ds_core_bench bench/serialize_bench.cpp)
target_link_libraries(o3ds_core_bench PRIVATE open3dstreamstatic)
o3ds_enable_warnings(o3ds_core_bench)
add_test(NAME core.bench.serialize COMMAND o3ds_core_bench --iterations 50)
set_tests_properties(core.bench.serialize PROPERTIES LABELS bench)
