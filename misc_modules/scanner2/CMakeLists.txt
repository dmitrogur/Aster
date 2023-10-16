cmake_minimum_required(VERSION 3.13)
project(scanner)

file(GLOB SRC "src/*.cpp")

add_library(scanner2 SHARED ${SRC})
target_link_libraries(scanner2 PRIVATE sdrpp_core)
set_target_properties(scanner2 PROPERTIES PREFIX "")

target_include_directories(scanner2 PRIVATE "src/")

if (MSVC)
    target_compile_options(scanner2 PRIVATE /O2 /Ob2 /std:c++17 /EHsc -lcurl)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(scanner2 PRIVATE -O3 -std=c++17 -lcurl -Wno-unused-command-line-argument -undefined dynamic_lookup)
else ()
    target_compile_options(scanner2 PRIVATE -O3 -std=c++17 -lcurl )
endif ()

# Install directives
install(TARGETS scanner2 DESTINATION lib/sdrpp/plugins)