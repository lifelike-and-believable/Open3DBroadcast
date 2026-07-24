cmake_minimum_required(VERSION 3.13)

# PredictorEval - measures C0 baseline prediction error to inform the
# roadmap's C3 go/no-go. Not shipped in the plugin; a developer tool.
add_executable(PredictorEval
	main.cpp
	motion_corpus.h
	motion_corpus.cpp
)

target_include_directories(PredictorEval PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(PredictorEval PRIVATE open3dstreamstatic)
