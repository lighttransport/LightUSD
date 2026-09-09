# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# LightUSD Next - CMake build configuration
# New modular architecture with minimal template usage

cmake_minimum_required(VERSION 3.10)
project(lightusd_next C CXX)

# Define source files
include("${CMAKE_CURRENT_LIST_DIR}/pipeline-sources.cmake")
set(LIGHTUSD_NEXT_SOURCES
    ${LIGHTUSD_NEXT_PIPELINE_SOURCES}
    execution.cc
    # Shared bounded JSON parser used by the next-generation URDF bridge.
    ../minijson.cc
    ../color-transform.cc
    # Shared SdfPathExpression parser/matcher. Kept stage-neutral so next
    # collection consumers do not need the legacy Stage representation.
    ../core/path-expression.cc
    ../core/path-expression-eval.cc
    # Type system
    types/value-view.cc

    # Layer (unified PrimSpec storage)
    layer/asset-anchor.cc

    # Writers
    writer/prim-printer.cc
    writer/usda-writer.cc
    writer/usdc-writer.cc
    writer/usdz-writer.cc

    # Schema convenience APIs
    schema/geom-point-instancer.cc
    schema/color-space.cc
    schema/usd-lux.cc
    schema/usd-geom-camera.cc
    schema/usd-geom-model.cc
    schema/usd-skel.cc
    schema/physics-scene.cc
    schema/physics-api.cc
    schema/physics-joint.cc
    schema/physics-collision.cc
    schema/usd-ar.cc
    schema/usd-media.cc
    schema/usd-mtlx.cc
    schema/usd-render.cc
    schema/usd-semantics.cc
    schema/usd-vol.cc

    # Composition
    composition/expression-variables.cc

    # Layer / PrimSpec diff
    diff/layer-diff.cc

    # PCP-style lazy composition cache (native to next)
    # MaterialX XML stack (dependency-free): layer-registry synthesizes
    # /MaterialX layers from referenced .mtlx documents.
    ../mtlx-xml-tokenizer.cc
    ../mtlx-xml-parser.cc
    ../mtlx-dom.cc
    pcp/load-rules.cc
    pcp/cache.cc
    pcp/cache-arcs.cc
    pcp/cache-relocates.cc
    pcp/cache-layers.cc
    pcp/cache-opinions.cc
    pcp/cache-storage.cc

    # AOUSD Core validation
    validation/usd-validation.cc
    validation/usd-validation-report.cc
    validation/usd-validation-physics.cc

    # High-level API
    lightusd-next.cc
)

# Define header files (for IDE project generation)
set(LIGHTUSD_NEXT_HEADERS
    execution.hh
    # Type system
    types/type-id.hh
    types/type-info.hh
    types/value.hh
    types/value-view.hh
    types/interpolation.hh

    # Prim types
    prim/path.hh
    prim/identifier.hh
    prim/attribute.hh
    prim/prim.hh

    # Composition policy
    composition/expression-variables.hh

    # Layer (unified PrimSpec storage)
    layer/property-index.hh
    layer/prim-spec.hh
    layer/layer.hh

    # Stage (composed scene)
    stage/stage.hh
    stage/change-set.hh

    # Color management shared with render consumers
    ../color-transform.hh
    schema/color-space.hh

    # ASCII parser
    parser/lexer.hh
    parser/value-parser.hh
    parser/value-parser-arrays.inc
    parser/value-parser-dict.inc
    parser/value-parser-numeric.hh
    parser/value-parser-scalars.inc
    parser/ascii-parser-internal.hh
    parser/ascii-parser.hh

    # Crate (binary) reader/writer
    crate/crate-format.hh
    crate/crate-reader-internal.hh
    crate/crate-reader.hh
    crate/crate-writer-fields.inc
    crate/crate-writer-impl.inc
    crate/crate-writer-sections.inc
    crate/crate-writer-tables.inc
    crate/crate-writer-values.inc
    crate/crate-writer-write.inc
    crate/crate-writer-types.hh
    crate/crate-writer.hh
    crate/stream-reader.hh
    crate/stream-writer.hh

    # High-level readers
    reader/usda-reader.hh
    reader/usdc-reader.hh

    # Writers
    writer/value-printer.hh
    writer/prim-printer.hh
    writer/usda-writer.hh
    writer/usdc-writer.hh
    writer/usdz-writer.hh

    # Schema convenience APIs
    schema/geom-mesh.hh
    schema/schema-registry.hh
    schema/geom-point-instancer.hh
    schema/geom-xform.hh
    schema/usd-lux.hh
    schema/usd-geom-camera.hh
    schema/usd-shade.hh
    schema/usd-skel.hh
    schema/physics-scene.hh
    schema/physics-api.hh
    schema/physics-joint.hh
    schema/physics-collision.hh
    schema/usd-ar.hh
    schema/usd-media.hh
    schema/usd-mtlx.hh
    schema/usdPhysics.hh

    # Attribute evaluation
    eval/attribute-eval.hh
    eval/value-clip.hh

    # Asset resolution
    resolver/asset-resolver.hh

    # Composition
    composition/composition.hh

    # Layer / PrimSpec diff
    diff/layer-diff.hh

    # PCP-style lazy composition cache (native to next)
    pcp/arc-types.hh
    pcp/namespace-mapping.hh
    pcp/prim-index.hh
    pcp/layer-registry.hh

    # AOUSD Core validation
    validation/usd-validation.hh
    pcp/cache-arc-expansion.inc
    pcp/cache.hh
    pcp/cache-compose.inc
    pcp/cache-invalidation.inc
    pcp/cache-layer-stack.inc
    pcp/cache-lock.hh
    pcp/cache-parallel-warm.inc
    pcp/cache-specs-instances.inc
    pcp/cache-stage-build.inc
    pcp/cache-utils.hh

    # High-level API
    lightusd-next.hh
    compat.hh
)

# LZ4 source (from existing LightUSD)
set(LZ4_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../lz4/lz4.c
)

# Prepend source directory to all files
list(TRANSFORM LIGHTUSD_NEXT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM LIGHTUSD_NEXT_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# Create static library
add_library(lightusd_next STATIC
    ${LIGHTUSD_NEXT_SOURCES}
    ${LIGHTUSD_NEXT_HEADERS}
    ${LZ4_SOURCES}
)

# C++17 (matches the parent project). The USDA writer formats floats via the
# vendored dragonbox dtoa (writer/dtoa.cc).
target_compile_features(lightusd_next PUBLIC cxx_std_17)

# Optional threading: enables std::thread parallel paths in next. Keep this OFF
# for WASM/browser builds so they do not require Emscripten pthreads /
# SharedArrayBuffer.
option(LIGHTUSD_NEXT_ENABLE_THREAD "Enable std::thread parallel paths in next (default OFF; not for default WASM)" OFF)
if(LIGHTUSD_NEXT_ENABLE_THREAD)
    find_package(Threads REQUIRED)
    target_compile_definitions(lightusd_next PUBLIC LIGHTUSD_ENABLE_THREAD)
    target_link_libraries(lightusd_next PUBLIC Threads::Threads)
endif()

# Optional fine-grained pcp::Cache locking: a std::shared_timed_mutex (pcp/
# cache-lock.hh) so pure reads (ComposedChildNames, GetMemoryStats, IsInstance,
# ...) take a shared lock instead of serializing behind the single exclusive
# mutex every Cache entry point otherwise takes. The READ_LOCK/WRITE_LOCK call
# sites in pcp/cache.cc and the cache-*.inc files were already written for
# this; only the switch to turn it on was missing. Requires
# LIGHTUSD_NEXT_ENABLE_THREAD (the lock macros are no-ops without it).
option(LIGHTUSD_NEXT_FINE_LOCKS "Use shared/exclusive locking for pcp::Cache instead of one exclusive mutex (requires LIGHTUSD_NEXT_ENABLE_THREAD)" OFF)
if(LIGHTUSD_NEXT_FINE_LOCKS)
    if(NOT LIGHTUSD_NEXT_ENABLE_THREAD)
        message(FATAL_ERROR "LIGHTUSD_NEXT_FINE_LOCKS requires LIGHTUSD_NEXT_ENABLE_THREAD=ON")
    endif()
    # PRIVATE, unlike LIGHTUSD_ENABLE_THREAD above: this only selects the
    # PcpMutex type used inside Cache::Impl (a pimpl, src/next/pcp/cache.cc),
    # never appears in a public header, and does not affect the layout of
    # anything a consumer links against -- so it does not need to match
    # between this library and its consumers the way LIGHTUSD_ENABLE_THREAD
    # (which changes PropNameTable's member layout) does.
    target_compile_definitions(lightusd_next PRIVATE LIGHTUSD_NEXT_FINE_LOCKS)
endif()

# Set include directories
target_include_directories(lightusd_next PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

# Include LZ4 header directory
target_include_directories(lightusd_next PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../lz4
)

# Compiler warnings
if(MSVC)
    target_compile_options(lightusd_next PRIVATE /W4)
    # Older MSVC toolset versions in some environments can trip STL/compiler
    # version checks; this keeps compatibility with CI/toolchain combinations that
    # still target this project.
    target_compile_definitions(lightusd_next PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    # Windows headers define max/min as macros; avoid collision with std::max/min
    add_compile_definitions(NOMINMAX)
else()
    target_compile_options(lightusd_next PRIVATE -Wall -Wextra -Wpedantic)
    # Disable some warnings for LZ4 C code
    set_source_files_properties(${LZ4_SOURCES} PROPERTIES COMPILE_FLAGS "-Wno-implicit-fallthrough")
endif()

# lightusd_next uses no C++ RTTI and no C++ exceptions; disable both (C++ sources
# only so the vendored C code does not warn). LIGHTUSD_CXX_RTTI / _EXCEPTIONS are
# inherited from the parent project when built as a subdirectory, and undefined
# (-> disabled) when built standalone.
if(NOT LIGHTUSD_CXX_RTTI)
    if(MSVC)
        target_compile_options(lightusd_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    else()
        target_compile_options(lightusd_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS)
    if(MSVC)
        target_compile_options(lightusd_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
        target_compile_options(lightusd_next PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
endif()

# Optional: Create test executables.
#
# `next` is a stable standalone CMake project. It is not added to the top-level
# build by default, so full regression runs must build this project separately
# and run its ctests. Keep the tests in a Debug build: many test programs use
# assert(), which is compiled out under NDEBUG.
option(LIGHTUSD_NEXT_BUILD_TESTS "Build tests for stable LightUSD Next" OFF)
set(LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT "" CACHE PATH
    "External AOUSD Core supplemental release checkout")

if(LIGHTUSD_NEXT_BUILD_TESTS)
    # The next tests validate behavior with assert(); a Release build (-DNDEBUG)
    # would compile those out and make the tests vacuous. Re-enable asserts for
    # everything defined below (directory-scoped; -UNDEBUG is appended after the
    # build-type flags so it wins). The library above keeps its NDEBUG/-O flags.
    add_compile_options(-UNDEBUG)

    add_executable(test_lightusd_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_next.cc
    )
    target_link_libraries(test_lightusd_next PRIVATE lightusd_next)

    add_executable(test_usdc_reader
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdc.cc
    )
    target_link_libraries(test_usdc_reader PRIVATE lightusd_next)
    # Fixtures are found by walking relative prefixes from the working
    # directory, which only resolves when the build dir sits directly under the
    # repo root. Hand the real root in so the test also runs from an
    # out-of-tree build dir (an ASan/threads/scratch build, or the second pass
    # in scripts/run-next-checks.sh).
    target_compile_definitions(test_usdc_reader PRIVATE
        LIGHTUSD_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")

    add_executable(test_layer
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_layer.cc
    )
    target_link_libraries(test_layer PRIVATE lightusd_next)

    add_executable(test_stage
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_stage.cc
    )
    target_link_libraries(test_stage PRIVATE lightusd_next)

    add_executable(test_pcp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_pcp.cc
    )
    target_link_libraries(test_pcp PRIVATE lightusd_next)
    target_compile_definitions(test_pcp PRIVATE
        LIGHTUSD_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")

    add_executable(test_instance_group
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_instance_group.cc
    )
    target_link_libraries(test_instance_group PRIVATE lightusd_next)

    add_executable(test_array_edit_write
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_array_edit_write.cc
    )
    target_link_libraries(test_array_edit_write PRIVATE lightusd_next)

    add_executable(test_pcp_parallel
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_pcp_parallel.cc
    )
    target_link_libraries(test_pcp_parallel PRIVATE lightusd_next)

    add_executable(test_writer
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_writer.cc
    )
    target_link_libraries(test_writer PRIVATE lightusd_next)

    add_executable(test_usdc_writer
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdc_writer.cc
    )
    target_link_libraries(test_usdc_writer PRIVATE lightusd_next)

    add_executable(test_usdc_roundtrip
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdc_roundtrip.cc
    )
    target_link_libraries(test_usdc_roundtrip PRIVATE lightusd_next)

    add_executable(test_complex_usda
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_complex_usda.cc
    )
    target_link_libraries(test_complex_usda PRIVATE lightusd_next)

    add_executable(test_roundtrip_fidelity
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_roundtrip_fidelity.cc
    )
    target_link_libraries(test_roundtrip_fidelity PRIVATE lightusd_next)

    add_executable(test_crate_dict
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_crate_dict.cc
    )
    target_link_libraries(test_crate_dict PRIVATE lightusd_next)

    add_executable(test_value_dict
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_value_dict.cc
    )
    target_link_libraries(test_value_dict PRIVATE lightusd_next)

    add_executable(test_crate_alloc_guard
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_crate_alloc_guard.cc
    )
    target_link_libraries(test_crate_alloc_guard PRIVATE lightusd_next)

    add_executable(test_lazy_array
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_lazy_array.cc
    )
    target_link_libraries(test_lazy_array PRIVATE lightusd_next)

    add_executable(bench_lazy_mem
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/bench_lazy_mem.cc
    )
    target_link_libraries(bench_lazy_mem PRIVATE lightusd_next)

    add_executable(bench_pcp_compose
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/bench_pcp_compose.cc
    )
    target_link_libraries(bench_pcp_compose PRIVATE lightusd_next)

    add_executable(bench_extref_compose
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/bench_extref_compose.cc
    )
    target_link_libraries(bench_extref_compose PRIVATE lightusd_next)

    add_executable(test_schemas
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_schemas.cc
    )
    target_link_libraries(test_schemas PRIVATE lightusd_next)

    add_executable(test_schemas_ext
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_schemas_ext.cc
    )
    target_link_libraries(test_schemas_ext PRIVATE lightusd_next)
    target_compile_definitions(test_schemas_ext PRIVATE
        LIGHTUSD_TEST_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")

    add_executable(test_usdcat_roundtrip
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdcat_roundtrip.cc
    )
    target_link_libraries(test_usdcat_roundtrip PRIVATE lightusd_next)

    add_executable(test_usdz_writer
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdz_writer.cc
    )
    target_link_libraries(test_usdz_writer PRIVATE lightusd_next)

    add_executable(test_composition
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_composition.cc
    )
    target_link_libraries(test_composition PRIVATE lightusd_next)

    add_executable(test_load_rules
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_load_rules.cc
    )
    target_link_libraries(test_load_rules PRIVATE lightusd_next)

    add_executable(test_layer_diff
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_layer_diff.cc
    )
    target_link_libraries(test_layer_diff PRIVATE lightusd_next)

    add_executable(test_memory_flatten
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_memory_flatten.cc
    )
    target_link_libraries(test_memory_flatten PRIVATE lightusd_next)

    add_executable(test_validation
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_validation.cc
    )
    target_link_libraries(test_validation PRIVATE lightusd_next)

    add_executable(test_aousd_conformance
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_aousd_conformance.cc
    )
    target_link_libraries(test_aousd_conformance PRIVATE lightusd_next)

    # Sampled-value oracle for the AOUSD supplemental value_resolution cases
    # (self-skips with exit 77 when the corpus is unavailable).
    add_executable(test_aousd_value_resolution
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_aousd_value_resolution.cc
    )
    target_link_libraries(test_aousd_value_resolution PRIVATE lightusd_next)

    # Self-contained value-resolution edge matrix (pxr-baked expectations in
    # tests/next/generated/vr-edge-expected.inc; never skips).
    add_executable(test_vr_edge_matrix
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_vr_edge_matrix.cc
    )
    target_link_libraries(test_vr_edge_matrix PRIVATE lightusd_next)
    target_include_directories(test_vr_edge_matrix PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next)

    # lusdcat-compatible CLI: drives the usd-wg asset-corpus regression gate
    # (tests/parse-asset-corpus.mjs --lusdcat $<TARGET_FILE:next_usdcat>).
    add_executable(next_usdcat
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/next_usdcat.cc
    )
    target_link_libraries(next_usdcat PRIVATE lightusd_next)

    # Replays minimized fuzzer-found crash inputs (run under ASAN/UBSAN).
    add_executable(test_crash_regressions
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_crash_regressions.cc
    )
    target_link_libraries(test_crash_regressions PRIVATE lightusd_next)
    target_compile_definitions(test_crash_regressions PRIVATE
        CRASH_REGRESSION_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/crash_regressions")

    add_executable(test_usdc_malformed
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_usdc_malformed.cc
    )
    target_link_libraries(test_usdc_malformed PRIVATE lightusd_next)
endif()

# ============================================================
# libFuzzer harnesses (clang only)
# ============================================================
option(LIGHTUSD_NEXT_BUILD_FUZZERS "Build libFuzzer harnesses for LightUSD Next (requires clang)" OFF)

if(LIGHTUSD_NEXT_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "LIGHTUSD_NEXT_BUILD_FUZZERS requires Clang for both C and C++ "
            "(-fsanitize=fuzzer)")
    endif()
    # The library must carry fuzzer coverage instrumentation too (no-link
    # variant), otherwise only the tiny harness body is explored.
    set(NEXT_FUZZ_LIB_FLAGS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all)
    set(NEXT_FUZZ_BIN_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_compile_options(lightusd_next PRIVATE ${NEXT_FUZZ_LIB_FLAGS})
    foreach(fz next_usdc next_usda next_compose next_roundtrip minijson)
        add_executable(fuzz_${fz}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzzer/${fz}_fuzzmain.cc
        )
        target_link_libraries(fuzz_${fz} PRIVATE lightusd_next)
        target_compile_options(fuzz_${fz} PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
        target_link_options(fuzz_${fz} PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
    endforeach()
endif()

# Tydra Next (render data conversion). Keep its target definition in its own
# directory so standalone next-core and top-level consumers cannot drift.
if(NOT TARGET tydra_next)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tydra/next
                     ${CMAKE_CURRENT_BINARY_DIR}/tydra_next_build)
endif()

# ============================================================
# C API (lightusd_c): FFI layer over lightusd_next + tydra_next.
# Used by the Python binding and any other language binding.
# ============================================================
add_library(lightusd_c STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-api/lightusd-c.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-api/lightusd-render-c.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-api/lightusd-c.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-api/lightusd-render-c.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../c-api/c-internal.hh
)
target_link_libraries(lightusd_c PUBLIC lightusd_next tydra_next)
target_compile_features(lightusd_c PUBLIC cxx_std_17)
target_include_directories(lightusd_c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../c-api>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)
if(MSVC)
    target_compile_options(lightusd_c PRIVATE /W4)
else()
    target_compile_options(lightusd_c PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOT LIGHTUSD_CXX_RTTI)
    if(MSVC)
        target_compile_options(lightusd_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR->)
    else()
        target_compile_options(lightusd_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
    endif()
endif()
if(NOT LIGHTUSD_CXX_EXCEPTIONS)
    if(MSVC)
        target_compile_options(lightusd_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    else()
        target_compile_options(lightusd_c PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
endif()

# Tydra Next test
if(LIGHTUSD_NEXT_BUILD_TESTS)
    add_executable(test_tydra_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/next/test_tydra_next.cc
    )
    target_link_libraries(test_tydra_next PRIVATE tydra_next lightusd_next)

    # Performance benchmark
    add_executable(benchmark_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/benchmark_next.cc
    )
    target_link_libraries(benchmark_next PRIVATE lightusd_next)

    # Render-conversion benchmark (tydra-next pipeline only; this standalone
    # tree cannot build the legacy tydra converter).
    # Report-only: prints timings and a deterministic scene checksum used to
    # verify byte-identical output across serial/parallel A/B builds.
    add_executable(bench_tydra_render
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/feat/render-convert/perf-render-convert.cc
    )
    target_link_libraries(bench_tydra_render PRIVATE tydra_next lightusd_next)
    target_include_directories(bench_tydra_render PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(bench_tydra_render PRIVATE
        PERFRC_ENABLE_NEXT=1 PERFRC_ENABLE_LEGACY=0)

    # Register tests with ctest
    enable_testing()

    # C API smoke test (pure C11 translation unit)
    add_executable(test_lightusd_c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/c-api/test_lightusd_c.c
    )
    target_link_libraries(test_lightusd_c PRIVATE lightusd_c)
    set_target_properties(test_lightusd_c PROPERTIES C_STANDARD 11)
    add_test(NAME next_test_c_api COMMAND test_lightusd_c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/cube.usda
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

    add_test(NAME next_test_lightusd COMMAND test_lightusd_next
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_layer COMMAND test_layer
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_stage COMMAND test_stage
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_pcp COMMAND test_pcp
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_pcp_parallel COMMAND test_pcp_parallel
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_instance_group COMMAND test_instance_group
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_array_edit_write COMMAND test_array_edit_write
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_writer COMMAND test_writer
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdc_writer COMMAND test_usdc_writer
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdc_reader COMMAND test_usdc_reader
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdc_roundtrip COMMAND test_usdc_roundtrip
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_complex_usda COMMAND test_complex_usda
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_roundtrip_fidelity COMMAND test_roundtrip_fidelity
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_crate_dict COMMAND test_crate_dict
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_value_dict COMMAND test_value_dict
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_crate_alloc_guard COMMAND test_crate_alloc_guard
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_lazy_array COMMAND test_lazy_array
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_schemas COMMAND test_schemas
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_schemas_ext COMMAND test_schemas_ext
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdcat_roundtrip COMMAND test_usdcat_roundtrip
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdz_writer COMMAND test_usdz_writer
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_composition COMMAND test_composition
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_crash_regressions COMMAND test_crash_regressions
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_usdc_malformed COMMAND test_usdc_malformed
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_load_rules COMMAND test_load_rules
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_layer_diff COMMAND test_layer_diff
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_memory_flatten COMMAND test_memory_flatten
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_validation COMMAND test_validation
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_aousd_conformance COMMAND test_aousd_conformance
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_tydra COMMAND test_tydra_next
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_benchmark COMMAND benchmark_next
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_bench_tydra_render COMMAND bench_tydra_render
        --iters 1 --prims 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(next_bench_tydra_render PROPERTIES LABELS "benchmark")

    set(LIGHTUSD_NEXT_SMOKE_TESTS
        next_test_lightusd
        next_test_layer
        next_test_stage
        next_test_pcp
        next_test_pcp_parallel
        next_test_instance_group
        next_test_array_edit_write
        next_test_writer
        next_test_usdc_writer
        next_test_usdc_reader
        next_test_usdc_roundtrip
        next_test_complex_usda
        next_test_roundtrip_fidelity
        next_test_crate_dict
        next_test_value_dict
        next_test_crate_alloc_guard
        next_test_lazy_array
        next_test_schemas
        next_test_schemas_ext
        next_test_usdcat_roundtrip
        next_test_usdz_writer
        next_test_composition
        next_test_crash_regressions
        next_test_usdc_malformed
        next_test_load_rules
        next_test_layer_diff
        next_test_memory_flatten
        next_test_validation
        next_test_aousd_conformance
        next_test_tydra)
    set_tests_properties(${LIGHTUSD_NEXT_SMOKE_TESTS}
        PROPERTIES LABELS "next")
    set_tests_properties(next_benchmark
        PROPERTIES LABELS "next;benchmark")

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME next_aousd_generated_tables
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/generate-next-aousd-tables.py
                    --root ${CMAKE_CURRENT_SOURCE_DIR}/../.. --check)
        set_tests_properties(next_aousd_generated_tables
            PROPERTIES LABELS "next;aousd;generated")
    endif()
    if(NOT LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT AND
       DEFINED ENV{AOUSD_CORE_SUPPLEMENTAL_ROOT})
        set(LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT
            "$ENV{AOUSD_CORE_SUPPLEMENTAL_ROOT}")
    endif()
    if(Python3_Interpreter_FOUND AND LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT AND
       EXISTS "${LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT}/composition")
        add_test(
            NAME next_aousd_supplemental
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/run-aousd-supplemental.py
                    --suite-root "${LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT}"
                    --next-usdcat "$<TARGET_FILE:next_usdcat>"
                    --aousd-test "$<TARGET_FILE:test_aousd_conformance>"
                    --aousd-value-test "$<TARGET_FILE:test_aousd_value_resolution>"
                    --max-composition-fail 0)
        set_tests_properties(next_aousd_supplemental
            PROPERTIES LABELS "next;aousd;supplemental" TIMEOUT 600)
    endif()
    # Sampled-value oracle also runs as its own ctest entry; the binary
    # self-skips (exit 77) when the corpus is missing.
    add_test(NAME next_test_aousd_value_resolution
        COMMAND test_aousd_value_resolution "${LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(next_test_aousd_value_resolution
        PROPERTIES SKIP_RETURN_CODE 77 LABELS "next;aousd")

    add_test(NAME next_test_vr_edge_matrix
        COMMAND test_vr_edge_matrix "${CMAKE_CURRENT_SOURCE_DIR}/../.."
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(next_test_vr_edge_matrix
        PROPERTIES LABELS "next;aousd;pxrdiff")

    # usd-wg asset-corpus regression gate (mirrors the top-level
    # corpus-parse-native test, but drives next_usdcat). Registered only when
    # node and the corpus checkout are present; set USD_WG_ASSETS_DIR to point
    # at a usd-wg/assets clone.
    find_program(LIGHTUSD_NEXT_NODE_EXECUTABLE NAMES node nodejs)
    if(LIGHTUSD_NEXT_NODE_EXECUTABLE)
        add_test(
            NAME next_compare_usda_asset_paths
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test-compare-usda-asset-paths.cjs)
        set_tests_properties(next_compare_usda_asset_paths
            PROPERTIES LABELS "next;pxrdiff")
    endif()
    # Pinned OpenUSD oracle for the pxr differential gates below. Set
    # LIGHTUSD_PXR_USDCAT to relocate it.
    if(DEFINED ENV{LIGHTUSD_PXR_USDCAT})
        set(NEXT_PXR_USDCAT "$ENV{LIGHTUSD_PXR_USDCAT}")
    else()
        set(NEXT_PXR_USDCAT "/mnt/nvme02/work/lightusd-repo/OpenUSD/dist/bin/usdcat")
    endif()
    if(DEFINED ENV{USD_WG_ASSETS_DIR})
        set(NEXT_CORPUS_ASSETS_DIR "$ENV{USD_WG_ASSETS_DIR}")
    else()
        set(NEXT_CORPUS_ASSETS_DIR "/mnt/nvme02/work/usd/assets")
    endif()
    if(LIGHTUSD_NEXT_NODE_EXECUTABLE AND EXISTS "${NEXT_CORPUS_ASSETS_DIR}")
        add_test(
            NAME next_corpus_parse
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/parse-asset-corpus.mjs
                    --assets "${NEXT_CORPUS_ASSETS_DIR}"
                    --mode load
                    --lusdcat "$<TARGET_FILE:next_usdcat>"
                    --out "${CMAKE_CURRENT_BINARY_DIR}/corpus-parse-results"
                    # Regression ratchet: 0 of 280 corpus files fail to parse
                    # today (some USDC files load with warnings on unsupported
                    # binary value types — see corpus-parse-results/summary.md).
                    # A tiny headroom guards against environment differences;
                    # raising it requires justification.
                    --max-fail 3
        )
        set_tests_properties(next_corpus_parse
            PROPERTIES LABELS "next;corpus")

        # Compose differential over the same corpus: flatten every asset and
        # cross-check against the pinned OpenUSD usdcat (needs the legacy
        # build's lusddiff for the layer re-serialization diff). Ratchets
        # (2026-07-15 baseline: MATCH 267, DIFFER 2 + DIFFERR 9 = 11; the
        # DIFFERR set is the LEGACY lusddiff parser failing on pxr output
        # forms, not next divergences): small headroom only — raising either
        # number requires justification.
        if(EXISTS "${NEXT_PXR_USDCAT}" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../build/lusddiff")
            add_test(
                NAME next_corpus_flatten_compare
                COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/parse-asset-corpus.mjs
                        --assets "${NEXT_CORPUS_ASSETS_DIR}"
                        --mode flatten
                        --lusdcat "$<TARGET_FILE:next_usdcat>"
                        --serialize-args "--rewrite-layer --openusd-compat"
                        --compare
                        --usdcat "${NEXT_PXR_USDCAT}"
                        --lusddiff "${CMAKE_CURRENT_SOURCE_DIR}/../../build/lusddiff"
                        --out "${CMAKE_CURRENT_BINARY_DIR}/corpus-flatten-results"
                        --max-fail 3
                        --max-differ 15
            )
            set_tests_properties(next_corpus_flatten_compare
                PROPERTIES LABELS "next;corpus;pxrdiff" TIMEOUT 1800)
        endif()
    endif()

    # next-vs-pxr flatten differential gate: next_usdcat -f vs the PINNED
    # OpenUSD 26.05 usdcat --flatten, compared semantically. Ratchet is 0:
    # any divergence not recorded (with reason) in
    # tests/next/next-pxr-flatten-xfail.txt fails; entries that start
    # passing are reported as XPASS for pruning.
    if(LIGHTUSD_NEXT_NODE_EXECUTABLE AND EXISTS "${NEXT_PXR_USDCAT}")
        set(NEXT_PXR_FLATTEN_ARGS
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/run-next-pxr-flatten-diff.mjs
            --next-usdcat "$<TARGET_FILE:next_usdcat>"
            --usdcat "${NEXT_PXR_USDCAT}")
        if(LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT)
            list(APPEND NEXT_PXR_FLATTEN_ARGS
                 --suite-root "${LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT}")
        endif()
        add_test(NAME next_pxr_flatten_diff
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE} ${NEXT_PXR_FLATTEN_ARGS})
        set_tests_properties(next_pxr_flatten_diff
            PROPERTIES LABELS "next;pxrdiff" TIMEOUT 900)
    endif()
endif()

# ============================================================
# Python extension module (optional)
# ============================================================

# (The Python binding is built by setup.py at the repo root; see
#  src/python/py-*.c and src/c-api/.)
