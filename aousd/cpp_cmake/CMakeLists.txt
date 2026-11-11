cmake_minimum_required(VERSION 3.16)
project(usd_comparison CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set compiler
set(CMAKE_C_COMPILER clang-20)
set(CMAKE_CXX_COMPILER clang++-20)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Paths to OpenUSD and LightUSD
set(OPENUSD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../dist" CACHE PATH "Path to OpenUSD installation")
set(LIGHTUSD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to LightUSD source")
set(LIGHTUSD_BUILD "${LIGHTUSD_ROOT}/build" CACHE PATH "Path to LightUSD build directory")

# Find TBB (required by OpenUSD)
find_package(TBB REQUIRED)

# Add OpenUSD include directories
include_directories(
    ${OPENUSD_ROOT}/include
    ${OPENUSD_ROOT}/include/boost-1_82
)

# Add LightUSD include directories
include_directories(
    ${LIGHTUSD_ROOT}/src
    ${LIGHTUSD_ROOT}/src/external
)

# Link directories
link_directories(
    ${OPENUSD_ROOT}/lib
    ${LIGHTUSD_BUILD}
)

# Create executable
add_executable(usd_comparison main.cpp)

# Compiler flags
target_compile_options(usd_comparison PRIVATE
    -Wall
    -Wno-deprecated
    -Wno-deprecated-declarations
    -O2
    -g
)

# Preprocessor definitions
target_compile_definitions(usd_comparison PRIVATE
    PXR_PYTHON_ENABLED
)

# Link OpenUSD libraries (order matters!)
set(USD_LIBS
    usd
    usdGeom
    usdShade
    usdLux
    usdSkel
    usdVol
    usdProc
    usdRender
    usdHydra
    usdRi
    sdf
    pcp
    kind
    usdUtils
    gf
    tf
    js
    ts
    work
    plug
    trace
    arch
    vt
    ar
)

# Link libraries
target_link_libraries(usd_comparison
    ${USD_LIBS}
    lightusd_legacy_static  # LightUSD static library
    TBB::tbb
    pthread
    dl
    m
)

# Set RPATH for runtime
set_target_properties(usd_comparison PROPERTIES
    INSTALL_RPATH "${OPENUSD_ROOT}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Install target
install(TARGETS usd_comparison
    RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin
)

# Custom target to run the application
add_custom_target(run
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/usd_comparison
    DEPENDS usd_comparison
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running USD comparison tool..."
)

# Print configuration
message(STATUS "")
message(STATUS "USD Comparison Tool Configuration:")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  OpenUSD Root: ${OPENUSD_ROOT}")
message(STATUS "  LightUSD Root: ${LIGHTUSD_ROOT}")
message(STATUS "  LightUSD Build: ${LIGHTUSD_BUILD}")
message(STATUS "")