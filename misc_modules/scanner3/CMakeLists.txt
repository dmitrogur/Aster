cmake_minimum_required(VERSION 3.13)
project(scanner3)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(scanner3 PRIVATE "src/" "../../decoder_modules/radio/src")