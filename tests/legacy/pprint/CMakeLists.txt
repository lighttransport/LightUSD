# Pretty-print benchmark
cmake_minimum_required(VERSION 3.10)

project(pprint_benchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find threading library
find_package(Threads REQUIRED)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/../../../src)

# Add executable
add_executable(pprint_benchmark
  pprint_benchmark.cc
)

# Link against lightusd library
target_link_libraries(pprint_benchmark
  lightusd_static
  Threads::Threads
)

# Enable threading support
target_compile_definitions(pprint_benchmark PRIVATE
  LIGHTUSD_ENABLE_THREAD=1
)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  target_compile_options(pprint_benchmark PRIVATE
    -Wall
    -Wextra
  )
endif()

# Install target
install(TARGETS pprint_benchmark
  RUNTIME DESTINATION bin
)
