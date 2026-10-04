cmake_minimum_required(VERSION 3.13)

# QuantEval - measures rotation error and jitter of the quantized encoding as a
# receiver sees it (CORE-12). Not shipped in the plugin; a developer tool.
add_executable(QuantEval
	main.cpp
	../PredictorEval/motion_corpus.h
	../PredictorEval/motion_corpus.cpp
)

target_include_directories(QuantEval PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(QuantEval PRIVATE open3dstreamstatic)
