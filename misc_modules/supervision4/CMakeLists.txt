cmake_minimum_required(VERSION 3.13)
project(supervision4)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(supervision4 PRIVATE "src/")

target_include_directories(supervision4 PRIVATE "src/" "../../decoder_modules/radio/src")

target_include_directories(supervision4 PRIVATE "src/" "../../sink_modules/audio_sink/src")
