cmake_minimum_required(VERSION 3.10)
project(mtlx-parser)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directories
include_directories(include)

# Source files
set(SOURCES
    src/mtlx-xml-tokenizer.cc
    src/mtlx-xml-parser.cc
    src/mtlx-dom.cc
    src/legacy/mtlx-simple-parser.cc
)

# Header files
set(HEADERS
    include/mtlx-xml-tokenizer.hh
    include/mtlx-xml-parser.hh
    include/mtlx-dom.hh
)

# Create static library
add_library(mtlx-parser STATIC ${SOURCES})

# Test executable
add_executable(test_parser tests/test_parser.cc)
target_link_libraries(test_parser mtlx-parser)

# Example program to parse MaterialX files
add_executable(parse_mtlx examples/parse_mtlx.cc)
target_link_libraries(parse_mtlx mtlx-parser)

# Test adapter
add_executable(test_adapter tests/test_adapter.cc)
target_link_libraries(test_adapter mtlx-parser)

# Enable warnings
if(MSVC)
    target_compile_options(mtlx-parser PRIVATE /W4)
    target_compile_options(test_parser PRIVATE /W4)
    target_compile_options(parse_mtlx PRIVATE /W4)
else()
    target_compile_options(mtlx-parser PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(test_parser PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(parse_mtlx PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Add security flags
if(NOT MSVC)
    target_compile_options(mtlx-parser PRIVATE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    target_compile_options(test_parser PRIVATE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    target_compile_options(parse_mtlx PRIVATE -fstack-protector-strong -D_FORTIFY_SOURCE=2)
endif()