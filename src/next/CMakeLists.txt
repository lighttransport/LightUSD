# SPDX-License-Identifier: Apache-2.0
# Copyright 2024-Present Light Transport Entertainment Inc.
#
# LightUSD Next - CMake build configuration
# New modular architecture with minimal template usage

cmake_minimum_required(VERSION 3.10)
project(lightusd_next C CXX)

# ---------------------------------------------------------------------------
# Release hardening for the shipped native artifacts.
#
# This library parses untrusted files, so the standard exploit mitigations are
# applied to the library and tools it ships: stack canaries, fortify'd libc
# wrappers, control-flow protection, and full RELRO / non-executable stack.
#
# WASM and WASI are skipped on purpose -- the flags are unsupported there and
# the sandbox already provides most of the properties; the option reports this
# rather than silently dropping the hardening.
#
# This is defense in depth, not a substitute for the source-level bounds and
# budget checks in the parsers.
# ---------------------------------------------------------------------------
option(LIGHTUSD_HARDENING
       "Apply release hardening flags (stack protector, FORTIFY, RELRO, CFI) to shipped next targets"
       ON)

function(lightusd_next_harden tgt)
  if(NOT LIGHTUSD_HARDENING)
    return()
  endif()
  if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
    return()
  endif()
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /sdl /guard:cf)
    # /sdl turns the CRT deprecation warning (C4996: fopen, getenv, gmtime,
    # strncpy, ...) into an error. Keep /sdl's runtime checks but silence that
    # one, as the clang-cl toolchain files already do.
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_options(${tgt} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE -fstack-protector-strong)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86)$")
      target_compile_options(${tgt} PRIVATE -fcf-protection=full)
    endif()
    # _FORTIFY_SOURCE requires optimization; enable it only when optimizing so
    # the debug build does not emit the "requires -O1" diagnostic.
    if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo|MinSizeRel")
      target_compile_definitions(${tgt} PRIVATE _FORTIFY_SOURCE=2)
    endif()
    if(NOT APPLE)
      target_link_options(${tgt} PRIVATE
          -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    endif()
  endif()
endfunction()

# Define source files
include("${CMAKE_CURRENT_LIST_DIR}/pipeline-sources.cmake")
set(LIGHTUSD_NEXT_SOURCES
    ${LIGHTUSD_NEXT_PIPELINE_SOURCES}
    execution.cc
    resolver/dependency-report.cc
    eval/property-trace.cc
    stage/change-set.cc
    stage/change-set-diff.cc
    stage/stage-session.cc
    # Shared bounded JSON parser used by the next-generation URDF bridge.
    ../minijson.cc
    ../color-transform.cc
    # C11 core: SdfPathExpression parsing / serialization / matching.
    # next owns this module; the legacy src/core/path-expression*.cc pair is
    # deliberately NOT part of this product (it would drag the legacy
    # value-type layer, value-types.hh + typed-array-core.hh, into every next
    # translation unit).
    c/path-expr.c
    # C11 core: generated checker tables (schema inventory) and their lookups.
    # Type system
    types/value-view.cc

    # Layer (unified PrimSpec storage)
    layer/asset-anchor.cc

    # Writers
    writer/prim-printer.cc
    writer/usda-writer.cc
    writer/usda-writer-print.cc
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
    schema/physics-query.cc
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
    diff/layer-diff-value.cc
    diff/layer-diff-output.cc

    # PCP-style lazy composition cache (native to next)
    # MaterialX XML stack (dependency-free): layer-registry synthesizes
    # /MaterialX layers from referenced .mtlx documents.
    ../mtlx-xml-tokenizer.cc
    ../mtlx-xml-parser.cc
    ../mtlx-dom.cc
    pcp/load-rules.cc
    pcp/cache.cc

    pcp/cache-arcs.cc
    pcp/cache-arcs-process.cc
    pcp/cache-relocates.cc
    pcp/cache-layers.cc
    pcp/cache-opinions.cc
    pcp/cache-storage.cc

    # AOUSD Core validation

    # High-level API
    lightusd-next.cc
    format-detect.cc
)

# Define header files (for IDE project generation)
set(LIGHTUSD_NEXT_HEADERS
    execution.hh
    resource-limits.hh
    # Type system
    types/type-id.hh
    types/type-info.hh
    types/value.hh
    types/value-view.hh
    types/interpolation.hh

    # Prim types
    prim/path.hh
    prim/identifier.hh

    # Composition policy
    composition/expression-variables.hh

    # C11 core (C ABI consumed by next and by C consumers)
    c/lightusd-core-c.h
    c/generated-schema-tables.h

    # next-local SdfPathExpression adapter over the C11 core
    core/path-expression.hh
    core/path-expression-eval.hh

    # Layer (unified PrimSpec storage)
    layer/property-index.hh
    layer/prim-spec.hh
    layer/layer.hh

    # Stage (composed scene)
    stage/stage.hh
    stage/change-set.hh
    stage/stage-session.hh

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
    schema/physics-query.hh
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
    load-usd.hh
    lightusd-next.hh
    operation-status.hh
)

# LZ4 source (from existing LightUSD)
set(LZ4_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../lz4/lz4.c
)

# Prepend source directory to all files
list(TRANSFORM LIGHTUSD_NEXT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM LIGHTUSD_NEXT_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

# C-only primitives are independently linkable without a C++ runtime.
set(LIGHTUSD_CORE_C_SOURCES ${LIGHTUSD_NEXT_SOURCES})
list(FILTER LIGHTUSD_CORE_C_SOURCES INCLUDE REGEX "\\.c$")
list(FILTER LIGHTUSD_NEXT_SOURCES EXCLUDE REGEX "\\.c$")
add_library(lightusd_core_c STATIC ${LIGHTUSD_CORE_C_SOURCES} ${LZ4_SOURCES})
# The generated descriptor queries are public C11 core APIs. Keep their data
# and lookup code here so bindings do not need the C++ validation engine merely
# to enumerate schema names, property types and variability.
target_sources(lightusd_core_c PRIVATE c/validation-tables.c c/generated-schema-tables.c)
set_target_properties(lightusd_core_c PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(UNIX)
  target_link_libraries(lightusd_core_c PUBLIC m)
endif()
target_include_directories(lightusd_core_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
lightusd_next_harden(lightusd_core_c)
option(LIGHTUSD_NEXT_USE_PCH "Use a precompiled header for native next C++ sources" ON)
add_library(lightusd_next STATIC ${LIGHTUSD_NEXT_SOURCES} ${LIGHTUSD_NEXT_HEADERS})
option(LIGHTUSD_WITH_USDOBJ "Import OBJ layers using the C11 loader" ON)
if(LIGHTUSD_WITH_USDOBJ)
    set(LIGHTUSD_ASSET_IMPORT_OBJ_ONLY ON)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/lightusd-asset-import.cmake")
    unset(LIGHTUSD_ASSET_IMPORT_OBJ_ONLY)
    target_sources(lightusd_next PRIVATE reader/obj-reader.cc)
    target_link_libraries(lightusd_next PUBLIC lightusd_obj_import)
    target_compile_definitions(lightusd_next PUBLIC LIGHTUSD_WITH_USDOBJ)
endif()
if(LIGHTUSD_NEXT_USE_PCH)
  target_precompile_headers(lightusd_next PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_LIST_DIR}/pch.hh>")
  # Without this, clang embeds the header mtimes in the PCH and ccache cannot
  # cache any TU that uses it (CI sets the matching CCACHE_SLOPPINESS).
  target_compile_options(lightusd_next PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:SHELL:-Xclang -fno-pch-timestamp>")
endif()
target_link_libraries(lightusd_next PUBLIC lightusd_core_c)

# Validation is an explicit component: resolver/core consumers must not pull it in.
add_library(lightusd_validation STATIC
    validation/usd-validation.cc
    validation/usd-validation-props.cc
    validation/usd-validation-compose.cc
    validation/usd-validation-geom.cc
    validation/usd-validation-shade.cc
    validation/usd-validation-colorspace.cc
    validation/usd-validation-arkit.cc
    validation/usd-validation-report.cc
    validation/usd-validation-physics.cc
    validation/validation-context.cc
    validation/validation-stage-physics.cc
)
target_link_libraries(lightusd_validation PUBLIC lightusd_next)
target_compile_features(lightusd_validation PRIVATE cxx_std_17)
set_target_properties(lightusd_validation PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
lightusd_next_harden(lightusd_validation)
if(NOT MSVC)
  target_compile_options(lightusd_validation PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
endif()

# C++17 (matches the parent project). The USDA writer formats floats via the
# vendored dragonbox dtoa (writer/dtoa.cc), with a zmij fast path.
target_compile_features(lightusd_next PUBLIC cxx_std_17)

# The C11 core is compiled as C11 by the same target: strict standard, no
# extensions, so the header stays a valid strict-C11 boundary for consumers.
set_target_properties(lightusd_next PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
lightusd_next_harden(lightusd_next)

# Silence warnings from the vendored zmij translation unit (third-party code:
# __int128 pedantic, sign-compare, unused params -- not ours to fix).
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../external/zmij/zmij.cc
        PROPERTIES COMPILE_FLAGS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../external/zmij/zmij.cc
        PROPERTIES COMPILE_FLAGS "-w")
endif()

# pipeline/flatten.cc is shared with the combined WASM module, which does not
# link the pcp::Cache engine; only this library enables FlattenOptions::
# use_pcp_compose.
target_compile_definitions(lightusd_next PRIVATE LIGHTUSD_NEXT_PIPELINE_PCP_COMPOSE)

# Optional threading: enables std::thread parallel paths in next. Native builds
# default to ON (the parallel source warm, arc-layer prefetch, opinion fill and
# crate-writer build are the difference between minutes and hours on large
# scenes). WASM/browser builds default to OFF: they would need Emscripten
# pthreads / SharedArrayBuffer.
if(EMSCRIPTEN)
    set(LIGHTUSD_NEXT_THREAD_DEFAULT OFF)
else()
    set(LIGHTUSD_NEXT_THREAD_DEFAULT ON)
endif()
option(LIGHTUSD_NEXT_ENABLE_THREAD
    "Enable std::thread parallel paths in next (default ON for native, OFF for WASM)"
    ${LIGHTUSD_NEXT_THREAD_DEFAULT})
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

# ------------------------------------------------------------------
# Optional drop-in allocator for the next CLI tools (next_usdcat). The
# compose + stream path is allocation-heavy (millions of small PrimSpec/Src
# allocations and cross-thread frees); glibc ptmalloc is a bottleneck there
# (~20% of compose samples in malloc/free). Measured on Island with 32
# threads, jemalloc cut the parallel opinion fill ~2x (2.9 -> 1.35 s) and
# load+compose 14.3 -> 10.2 s; output is byte-identical (an allocator never
# changes it). Only the tool executables link it -- the library never forces
# an allocator on applications.
#
# Auto-detected SYSTEM library, never required: jemalloc (preferred), then
# tcmalloc_minimal, then mimalloc. Missing -> plain system malloc with a status
# message. Opt out with -DLIGHTUSD_NEXT_WITH_ALLOCATOR=OFF. Never used for
# Emscripten/WASI, Windows/macOS (untested there), or sanitizer/fuzzer builds
# (the sanitizer runtimes provide their own allocator).
# Equivalent at runtime to LD_PRELOAD=libjemalloc.so.2.
#
# jemalloc runs with its default decay. next-refactor baked a no-purge
# malloc_conf (dirty/muzzy_decay_ms:-1) into the tool; re-measured here
# (Island flatten, 3 interleaved runs) it gave the same wall time (~24 s vs
# glibc ~30 s) at +1.2 GB peak RSS (7.8 vs 6.6 GB), so it is not baked. Try it
# with MALLOC_CONF=dirty_decay_ms:-1,muzzy_decay_ms:-1.
# ------------------------------------------------------------------
option(LIGHTUSD_NEXT_WITH_ALLOCATOR
    "Link an installed jemalloc/tcmalloc/mimalloc into the next CLI tools when found (optional)" ON)
set(LIGHTUSD_NEXT_ALLOCATOR_LIB "")
set(_next_alloc_skip "")
if(EMSCRIPTEN OR WASI OR CMAKE_SYSTEM_NAME STREQUAL "WASI" OR
   CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(_next_alloc_skip "WebAssembly target")
elseif(NOT UNIX OR APPLE)
    set(_next_alloc_skip "only enabled on Linux/Unix")
elseif(LIGHTUSD_NEXT_BUILD_FUZZERS OR LIGHTUSD_NEXT_BUILD_TSAN_TESTS OR
       "${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}" MATCHES "-fsanitize")
    set(_next_alloc_skip "sanitizer build")
endif()
if(LIGHTUSD_NEXT_WITH_ALLOCATOR AND _next_alloc_skip)
    message(STATUS "next: drop-in allocator disabled (${_next_alloc_skip}); using system malloc")
elseif(LIGHTUSD_NEXT_WITH_ALLOCATOR)
    # Ranking (next-refactor measurements, doc/large-scene.md): jemalloc is
    # the fastest at the lowest RSS; tcmalloc_minimal is the zero-config
    # runner-up (often shipped only as the versioned runtime without a -dev
    # symlink, so also look for that); mimalloc is fast only with
    # MIMALLOC_PURGE_DELAY=-1 and costs more RSS.
    find_library(LIGHTUSD_NEXT_ALLOC_JEMALLOC NAMES jemalloc)
    if(LIGHTUSD_NEXT_ALLOC_JEMALLOC)
        set(LIGHTUSD_NEXT_ALLOCATOR_LIB "${LIGHTUSD_NEXT_ALLOC_JEMALLOC}")
    else()
        find_library(LIGHTUSD_NEXT_ALLOC_TCMALLOC NAMES tcmalloc_minimal tcmalloc)
        if(NOT LIGHTUSD_NEXT_ALLOC_TCMALLOC)
            find_file(LIGHTUSD_NEXT_ALLOC_TCMALLOC
                NAMES libtcmalloc_minimal.so.4 libtcmalloc.so.4
                PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
                      /lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib)
        endif()
        if(LIGHTUSD_NEXT_ALLOC_TCMALLOC)
            set(LIGHTUSD_NEXT_ALLOCATOR_LIB "${LIGHTUSD_NEXT_ALLOC_TCMALLOC}")
        else()
            find_library(LIGHTUSD_NEXT_ALLOC_MIMALLOC NAMES mimalloc)
            if(LIGHTUSD_NEXT_ALLOC_MIMALLOC)
                set(LIGHTUSD_NEXT_ALLOCATOR_LIB "${LIGHTUSD_NEXT_ALLOC_MIMALLOC}")
            endif()
        endif()
    endif()
    if(LIGHTUSD_NEXT_ALLOCATOR_LIB)
        message(STATUS "next: linking allocator ${LIGHTUSD_NEXT_ALLOCATOR_LIB} into CLI tools")
    else()
        message(STATUS "next: no jemalloc/tcmalloc/mimalloc found (install libjemalloc-dev for faster compose); using system malloc")
    endif()
endif()

# Optional: Create test executables.
#
# `next` is a stable standalone CMake project. It is not added to the top-level
# build by default, so full regression runs must build this project separately
# and run its ctests. Keep the tests in a Debug build: many test programs use
# assert(), which is compiled out under NDEBUG.
option(LIGHTUSD_NEXT_BUILD_TESTS "Build tests for stable LightUSD Next" OFF)
# `lusdcat` is the next-backed CLI (formerly `next_usdcat`). It is a tool, not
# a test, so the default `LIGHTUSD_NATIVE_PRODUCT=next` root build ships it
# without requiring test registration. Default ON for native standalone builds;
# WASM/WASI builds have no command line to drive so they skip it.
if(EMSCRIPTEN OR WASI OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
  set(_lightusd_next_tools_default OFF)
else()
  set(_lightusd_next_tools_default ON)
endif()
option(LIGHTUSD_NEXT_BUILD_TOOLS
    "Build next CLI tools (lusdcat)" ${_lightusd_next_tools_default})
unset(_lightusd_next_tools_default)
set(LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT "" CACHE PATH
    "External AOUSD Core supplemental release checkout")

if(LIGHTUSD_NEXT_BUILD_TESTS)
    enable_testing() # Needed before child projects register their tests.
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
    # This test inspects private cache storage; match the Impl lock layout.
    if(LIGHTUSD_NEXT_FINE_LOCKS)
        target_compile_definitions(test_pcp_parallel PRIVATE LIGHTUSD_NEXT_FINE_LOCKS)
    endif()

    add_executable(test_writer
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_writer.cc
    )
    target_link_libraries(test_writer PRIVATE lightusd_next)

    add_executable(test_dtoa
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_dtoa.cc
    )
    target_link_libraries(test_dtoa PRIVATE lightusd_next)

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

    add_executable(test_array_parser ../../tests/next/test_array_parser.cc)
    target_link_libraries(test_array_parser PRIVATE lightusd_next)
    add_executable(test_c11_leaves ../../tests/next/test_c11_leaves.c)
    target_link_libraries(test_c11_leaves PRIVATE lightusd_core_c)
    target_include_directories(test_c11_leaves PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(test_c11_leaves PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME next_c11_leaves COMMAND test_c11_leaves)
    add_executable(test_storage ../../tests/next/test_storage.c)
    target_link_libraries(test_storage PRIVATE lightusd_core_c)
    target_include_directories(test_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(test_storage PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
      target_compile_options(test_storage PRIVATE /UNDEBUG)
    else()
      target_compile_options(test_storage PRIVATE -UNDEBUG)
    endif()
    add_test(NAME next_c11_storage COMMAND test_storage)
    file(GLOB exr_test_sources ../external/tinyexr/src/*.c)
    add_executable(test_exr_codec ../../tests/next/test_exr_codec.c
      ../imageio/exr-codec.c ${exr_test_sources})
    target_include_directories(test_exr_codec PRIVATE ../external/tinyexr/include ../external/tinyexr/src)
    target_compile_definitions(test_exr_codec PRIVATE EXR_NO_JPH EXR_NO_ZSTD)
    set_target_properties(test_exr_codec PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(UNIX)
      target_link_libraries(test_exr_codec PRIVATE m)
    endif()
    add_test(NAME next_exr_codec COMMAND test_exr_codec)
    add_executable(test_array_parser_c ../../tests/next/test_array_parser_c.c)
    target_link_libraries(test_array_parser_c PRIVATE lightusd_core_c)
    target_include_directories(test_array_parser_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    set_target_properties(test_array_parser_c PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

    add_executable(test_path_expression_c_core
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_path_expression_c_core.cc
    )
    target_link_libraries(test_path_expression_c_core PRIVATE lightusd_next)

    add_executable(test_layer_diff
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_layer_diff.cc
    )
    target_link_libraries(test_layer_diff PRIVATE lightusd_next)

    add_executable(test_memory_flatten
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_memory_flatten.cc
    )
    target_link_libraries(test_memory_flatten PRIVATE lightusd_next)

    add_executable(test_resolver
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_resolver.cc
    )
    target_link_libraries(test_resolver PRIVATE lightusd_next)

    add_executable(test_validation_registry
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_validation_registry.cc)
    target_link_libraries(test_validation_registry PRIVATE lightusd_validation)
    add_test(NAME next_test_validation_registry COMMAND test_validation_registry)

    add_executable(test_validation
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_validation.cc
    )
    target_link_libraries(test_validation PRIVATE lightusd_validation)

    add_executable(test_aousd_conformance
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_aousd_conformance.cc
    )
    target_link_libraries(test_aousd_conformance PRIVATE lightusd_validation)

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

    add_executable(test_crate_limits
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_crate_limits.cc
    )
    target_link_libraries(test_crate_limits PRIVATE lightusd_next)
endif()

# ---------------------------------------------------------------------------
# lusdcat -- the unified LightUSD CLI for the next product.
#
# This is the next-backed `next_usdcat` promoted to the `lusdcat` name, so the
# default `LIGHTUSD_NATIVE_PRODUCT=next` build ships a `lusdcat` binary with no
# extra options. `next_usdcat` stays as an alias for regression harnesses and
# scripts that still use the old name. The target is also required by the tests
# registered above (they run `$<TARGET_FILE:next_usdcat>`), hence the OR.
# ---------------------------------------------------------------------------
if((LIGHTUSD_NEXT_BUILD_TOOLS OR LIGHTUSD_NEXT_BUILD_TESTS) AND NOT TARGET lusdcat)
    add_executable(lusdcat
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/lusdcat.cc
    )
    # Allocator (if found, see LIGHTUSD_NEXT_WITH_ALLOCATOR) first, so it wins
    # symbol resolution.
    target_link_libraries(lusdcat PRIVATE
        ${LIGHTUSD_NEXT_ALLOCATOR_LIB} lightusd_next)
    lightusd_next_harden(lusdcat)
    # Keep the CLI at the build root (`build_ninja/lusdcat`), matching the
    # legacy tool layout so scripts and AGENTS.md commands work unchanged.
    set_target_properties(lusdcat PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_executable(next_usdcat ALIAS lusdcat)
endif()

# ============================================================
# ThreadSanitizer regression tests (opt-in, default OFF)
# ============================================================
# Instruments the whole next build with -fsanitize=thread (every tool and
# test is a TSan program -- a TSan binary must be built whole) and registers
# `next_test_tsan_propname`, which hammers the PropNameTable frozen fast path
# the parallel opinion fill relies on; a
# detected race aborts nonzero, so ctest FAILS. With it on, every ctest
# (test_pcp_parallel, next_usdcat flattens, ...) also runs under TSan. Use a
# dedicated build dir:
#   cmake -S src/next -B build-tsan -DLIGHTUSD_NEXT_BUILD_TESTS=ON \
#         -DLIGHTUSD_NEXT_ENABLE_THREAD=ON -DLIGHTUSD_NEXT_BUILD_TSAN_TESTS=ON
option(LIGHTUSD_NEXT_BUILD_TSAN_TESTS
    "Instrument next with -fsanitize=thread and register the TSan regression tests" OFF)
if(LIGHTUSD_NEXT_BUILD_TSAN_TESTS)
    if(NOT LIGHTUSD_NEXT_ENABLE_THREAD)
        message(FATAL_ERROR "LIGHTUSD_NEXT_BUILD_TSAN_TESTS requires LIGHTUSD_NEXT_ENABLE_THREAD=ON")
    endif()
    if(NOT LIGHTUSD_NEXT_BUILD_TESTS)
        message(FATAL_ERROR "LIGHTUSD_NEXT_BUILD_TSAN_TESTS requires LIGHTUSD_NEXT_BUILD_TESTS=ON")
    endif()
    # Directory-wide flags (CMake uses their final value for every target in
    # this directory), so C test programs and the shared C API library are
    # instrumented too -- a thread started from an uninstrumented executable
    # crashes inside the TSan runtime.
    set(NEXT_TSAN_FLAGS "-fsanitize=thread -g -fno-omit-frame-pointer")
    string(APPEND CMAKE_C_FLAGS " ${NEXT_TSAN_FLAGS}")
    string(APPEND CMAKE_CXX_FLAGS " ${NEXT_TSAN_FLAGS}")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=thread")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=thread")
    add_executable(tsan_propname
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/tsan_propname.cc)
    target_link_libraries(tsan_propname PRIVATE lightusd_next)
    # Some kernels make TSan abort at startup with "unexpected memory mapping"
    # (high-entropy ASLR); run under `setarch -R` when available.
    find_program(NEXT_SETARCH setarch)
    if(NEXT_SETARCH)
        add_test(NAME next_test_tsan_propname
            COMMAND ${NEXT_SETARCH} -R $<TARGET_FILE:tsan_propname>)
    else()
        add_test(NAME next_test_tsan_propname COMMAND tsan_propname)
    endif()
    set_tests_properties(next_test_tsan_propname PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1:exitcode=66"
        LABELS "next;tsan")
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
    foreach(fz next_usdc next_usda next_compose next_roundtrip minijson next_dtoa)
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
option(LIGHTUSD_WITH_TYDRA "Build render conversion and its C API" ON)

# Declared BEFORE the tydra_next subdirectory: cmake/lightusd-render-support.cmake
# (included there) selects POSITION_INDEPENDENT_CODE for the shared subdivision
# objects from this variable. Declaring it afterwards left them non-PIC, which
# only surfaced with a compiler that emits PC32 relocations (clang) when linking
# the shared C ABI.
option(LIGHTUSD_NEXT_BUILD_SHARED_C_API
       "Build the portable shared C ABI in addition to static libraries"
       ON)
if(LIGHTUSD_WITH_TYDRA AND NOT TARGET tydra_next)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tydra/next
                     ${CMAKE_CURRENT_BINARY_DIR}/tydra_next_build)
    lightusd_next_harden(tydra_next)
endif()

# ============================================================
# C API (lightusd_c): FFI layer over lightusd_next + tydra_next.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(LIGHTUSD_NEXT_BUILD_EXAMPLES "Build next workflow examples" ON)
  if(LIGHTUSD_NEXT_BUILD_EXAMPLES AND LIGHTUSD_WITH_TYDRA)
    add_subdirectory(../../examples/next_workflows next_workflow_examples)
  endif()
endif()

if(LIGHTUSD_NEXT_BUILD_EXAMPLES)
  add_executable(asset_resolution_example
      ../../examples/asset_resolution/asset-resolution-next.cc)
  target_link_libraries(asset_resolution_example PRIVATE lightusd_next)
  set_target_properties(asset_resolution_example PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(LIGHTUSD_NEXT_BUILD_TESTS)
    add_test(NAME next_asset_resolution_example COMMAND asset_resolution_example
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(next_asset_resolution_example PROPERTIES
        LABELS "next;examples")
  endif()
endif()
# Used by the Python binding and any other language binding.
# ============================================================
option(LIGHTUSD_NEXT_BUILD_C_API_EXAMPLE
       "Build the public C API consumer example"
       OFF)

# Keep core C consumers independent of render headers and libraries.
set(LIGHTUSD_C_SOURCES
    ../c-api/lightusd-c.cc
    ../c-api/lightusd-c-format.cc
    ../c-api/lightusd-c-dependencies.cc
    ../c-api/lightusd-physics-c.cc
    ../c-api/lightusd-session-c.cc)
add_library(lightusd_c STATIC ${LIGHTUSD_C_SOURCES})
add_library(lightusd_validation_c STATIC
    ../c-api/lightusd-validation-c.cc)
target_link_libraries(lightusd_validation_c PUBLIC lightusd_c lightusd_validation)
target_include_directories(lightusd_validation_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(lightusd_validation_c PRIVATE cxx_std_17)
lightusd_next_harden(lightusd_validation_c)
lightusd_next_harden(lightusd_c)

# Fuzz the C ABI itself: the installed boundary used by JS/WASM, Python and
# engine bindings. The C++ internals already have their own harnesses; this one
# links lightusd_c so the FFI layer is instrumented and exercised.
if(LIGHTUSD_NEXT_BUILD_FUZZERS)
    target_compile_options(lightusd_c PRIVATE ${NEXT_FUZZ_LIB_FLAGS})
    add_executable(fuzz_next_cabi
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzzer/next_cabi_fuzzmain.cc)
    target_link_libraries(fuzz_next_cabi PRIVATE lightusd_c)
    target_compile_options(fuzz_next_cabi PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
    target_link_options(fuzz_next_cabi PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Static consumers can discard unused C entry points even while the
    # implementation is being split into smaller source files.
    target_compile_options(lightusd_c PRIVATE
        -ffunction-sections -fdata-sections)
endif()
# This target is installed for external C consumers. Propagate the static
# core dependency through the exported target so a find_package consumer gets
# a complete link interface instead of an unresolved lightusd_next archive.
target_link_libraries(lightusd_c PUBLIC lightusd_next)
set(LIGHTUSD_C_TARGETS lightusd_c lightusd_validation_c)
if(LIGHTUSD_NEXT_BUILD_EXAMPLES)
    add_executable(variant-lister
        ../../examples/variant-lister/variant-lister-next.cc)
    target_include_directories(variant-lister PRIVATE ../c-api)
    target_link_libraries(variant-lister PRIVATE lightusd_c)
    set_target_properties(variant-lister PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(LIGHTUSD_NEXT_BUILD_TESTS)
        add_test(NAME next_variant_lister_smoke COMMAND variant-lister --summary
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/variantSet-002.usda)
        set_tests_properties(next_variant_lister_smoke PROPERTIES
            LABELS "next;examples"
            PASS_REGULAR_EXPRESSION "Total variant options: 5")
    endif()
endif()
if(LIGHTUSD_NEXT_BUILD_C_API_EXAMPLE)
    add_executable(next_c_api_example
                   ../../examples/c_api_example/next-c-api-example.c)
    target_include_directories(next_c_api_example PRIVATE ../c-api)
    target_link_libraries(next_c_api_example PRIVATE lightusd_c)
    if(LIGHTUSD_NEXT_BUILD_TESTS)
        add_test(NAME next_c_api_example_smoke
                 COMMAND next_c_api_example
                         ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/anytype-001.usda)
        set_tests_properties(next_c_api_example_smoke PROPERTIES LABELS "next;c-api")
    endif()
endif()
if(LIGHTUSD_WITH_TYDRA)
    add_library(lightusd_render_c STATIC
        ../c-api/lightusd-render-c.cc
        ../c-api/lightusd-render-glb-c.cc
        ../c-api/lightusd-import-c.cc
        ../c-api/lightusd-volumetric-c.cc)
    target_link_libraries(lightusd_render_c PUBLIC lightusd_c tydra_next)
    lightusd_next_harden(lightusd_render_c)
    list(APPEND LIGHTUSD_C_TARGETS lightusd_render_c)
    # Native OBJ/URDF/MJCF/COLLADA import (parsers, MJCF macro expansion and
    # the USD writer) through the C import API.
    if(LIGHTUSD_NEXT_BUILD_FUZZERS AND TARGET lightusd_asset_import)
        foreach(_lib lightusd_render_c tydra_next lightusd_asset_import lightusd_obj_import)
            target_compile_options(${_lib} PRIVATE ${NEXT_FUZZ_LIB_FLAGS})
        endforeach()
        add_executable(fuzz_next_import
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzzer/next_import_fuzzmain.cc)
        target_include_directories(fuzz_next_import PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../external)
        target_link_libraries(fuzz_next_import PRIVATE lightusd_render_c)
        target_compile_options(fuzz_next_import PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
        target_link_options(fuzz_next_import PRIVATE ${NEXT_FUZZ_BIN_FLAGS})
    endif()
    if(LIGHTUSD_NEXT_BUILD_C_API_EXAMPLE)
        add_executable(next_render_session_example
                       ../../examples/c_api_example/next-render-session-example.c)
        target_include_directories(next_render_session_example PRIVATE ../c-api)
        target_link_libraries(next_render_session_example PRIVATE lightusd_render_c lightusd_c)
        if(LIGHTUSD_NEXT_BUILD_TESTS)
            add_test(NAME next_render_session_example_smoke
                     COMMAND next_render_session_example
                             ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/anytype-001.usda)
            set_tests_properties(next_render_session_example_smoke
                                 PROPERTIES LABELS "next;c-api;render")
        endif()
    endif()
endif()

# Native MJCF -> USD converter on the next converter and writers (the same
# source builds against legacy tydra in the legacy product).
if(LIGHTUSD_NEXT_BUILD_EXAMPLES AND LIGHTUSD_WITH_TYDRA AND
   NOT LIGHTUSD_NATIVE_PRODUCT STREQUAL "legacy")
    add_executable(urdf-to-usd
        ../../examples/urdf-to-usd/main.cc)
    target_include_directories(urdf-to-usd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../external)
    target_compile_definitions(urdf-to-usd PRIVATE LIGHTUSD_URDF_TO_USD_NEXT=1)
    target_link_libraries(urdf-to-usd PRIVATE tydra_next lightusd_next)
    set_target_properties(urdf-to-usd PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(urdf-to-usd PRIVATE -fexceptions)
    endif()
endif()

# ============================================================
# MCP server (next product): a public C API consumer that serves the MCP
# tools/resources surface (tool schemas shared with the legacy server).
option(LIGHTUSD_WITH_MCP_SERVER "Build the LightUSD MCP server library and stdio executable" OFF)
# Legacy product trees include src/next for their own tools and keep the
# legacy MCP server; the next server is built for the next product only.
if(LIGHTUSD_WITH_MCP_SERVER AND NOT LIGHTUSD_NATIVE_PRODUCT STREQUAL "legacy")
    add_library(lightusd_mcp STATIC
        ../mcp/mcp-server.cc
        ../mcp/mcp-tool-schemas.cc
        ../mcp/mcp-util.cc
        ../mcp/mcp-tools-assets.cc
        ../mcp/mcp-tools-composition.cc
        ../mcp/mcp-tools-diff.cc
        ../mcp/mcp-tools-query.cc
        ../mcp/mcp-tools-stage.cc
        ../mcp/mcp-tools-usdz.cc)
    target_include_directories(lightusd_mcp
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../mcp
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../c-api)
    target_link_libraries(lightusd_mcp PUBLIC lightusd_c lightusd_validation)
    # Tool arguments are untrusted JSON; nlohmann reports type errors by
    # throwing, and the server converts them into tool errors.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(lightusd_mcp PRIVATE -fexceptions)
    endif()
    add_executable(lightusd-mcp ../mcp/lightusd-mcp-main.cc)
    target_link_libraries(lightusd-mcp PRIVATE lightusd_mcp)
    set_target_properties(lightusd-mcp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    if(LIGHTUSD_NEXT_BUILD_TESTS)
        add_executable(test_mcp_next
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_mcp_next.cc)
        target_include_directories(test_mcp_next PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../c-api)
        target_link_libraries(test_mcp_next PRIVATE lightusd_mcp)
        add_test(NAME next_test_mcp COMMAND test_mcp_next
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(next_test_mcp PROPERTIES LABELS "next;mcp")
    endif()
endif()

# One shared FFI module owns all internal registries. Do not statically link
# separate copies of next into two DLLs: their interned property IDs would
# refer to different name tables. WITH_TYDRA=OFF produces a core-only DLL.
if(LIGHTUSD_NEXT_BUILD_SHARED_C_API)
    set_target_properties(lightusd_next lightusd_core_c lightusd_validation PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(lightusd_c_shared SHARED ${LIGHTUSD_C_SOURCES} ../c-api/lightusd-validation-c.cc)
    target_link_libraries(lightusd_c_shared PRIVATE lightusd_next lightusd_validation)
    lightusd_next_harden(lightusd_c_shared)
    if(LIGHTUSD_WITH_TYDRA)
        set_target_properties(tydra_next PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_sources(lightusd_c_shared PRIVATE
            ../c-api/lightusd-render-c.cc
            ../c-api/lightusd-render-glb-c.cc
            ../c-api/lightusd-import-c.cc
            ../c-api/lightusd-volumetric-c.cc)
        target_link_libraries(lightusd_c_shared PRIVATE tydra_next)
    endif()
    target_compile_definitions(lightusd_c_shared
        PRIVATE LIGHTUSD_C_BUILD_SHARED=1
        INTERFACE LIGHTUSD_C_USE_SHARED=1)
    set_target_properties(lightusd_c_shared PROPERTIES OUTPUT_NAME lightusd_c)
    if(WIN32)
        # On Windows the DLL's import library is lightusd_c.lib, the same file
        # the static lightusd_c archive would write; the import library (C
        # exports only) then replaces the archive and static consumers such as
        # lightusd_render_c lose the internal C++ helpers (LNK2019).
        set_target_properties(lightusd_c PROPERTIES OUTPUT_NAME lightusd_c_static)
    endif()
    list(APPEND LIGHTUSD_C_TARGETS lightusd_c_shared)
endif()

# Only the C boundary and thin ownership facades are installed for consumers.
if(LIGHTUSD_WITH_TYDRA)
  # Packaging must use actual target artifacts, including nested build dirs.
  # Searching by basename can select an obsolete archive after reconfiguration.
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/lightusd-python-libraries-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:lightusd_validation_c>\n$<TARGET_FILE:lightusd_render_c>\n$<TARGET_FILE:lightusd_c>\n$<TARGET_FILE:tydra_next>\n$<TARGET_FILE:lightusd_asset_import>\n$<TARGET_FILE:lightusd_validation>\n$<TARGET_FILE:lightusd_next>\n$<TARGET_FILE:lightusd_obj_import>\n$<TARGET_FILE:lightusd_tinyvdb>\n$<TARGET_FILE:lightusd_core_c>\n")
endif()
include(GNUInstallDirs)
set_target_properties(lightusd_c PROPERTIES EXPORT_NAME c)
set_target_properties(lightusd_validation_c PROPERTIES EXPORT_NAME validation_c)
set_target_properties(lightusd_validation PROPERTIES EXPORT_NAME validation)
install(TARGETS lightusd_core_c lightusd_next lightusd_c lightusd_validation lightusd_validation_c EXPORT LightUSDTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES c/lightusd-core-c.h c/storage.h c/value.h c/chunks.h c/descriptor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/next/c)
install(FILES ../c-api/lightusd-validation-c.h ../c-api/lightusd-validation-cpp.hh
              ../c-api/lightusd-c.h ../c-api/lightusd-cpp.hh
              ../c-api/lightusd-physics-c.h
              ../c-api/lightusd-session-c.h ../c-api/lightusd-session-cpp.hh
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(LIGHTUSD_WITH_TYDRA)
    set_target_properties(lightusd_render_c PROPERTIES EXPORT_NAME render_c)
    install(TARGETS tydra_next lightusd_render_c EXPORT LightUSDTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES ../c-api/lightusd-render-c.h ../c-api/lightusd-render-cpp.hh ../c-api/lightusd-import-c.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(LIGHTUSD_NEXT_BUILD_SHARED_C_API)
    set_target_properties(lightusd_c_shared PROPERTIES EXPORT_NAME c_shared)
    install(TARGETS lightusd_c_shared EXPORT LightUSDTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LightUSDConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LightUSDConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LightUSD")
if(TARGET lightusd_asset_import)
  install(TARGETS lightusd_asset_import EXPORT LightUSDTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(TARGET lightusd_obj_import)
  install(TARGETS lightusd_obj_import EXPORT LightUSDTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(TARGET lightusd_tinyvdb)
  install(TARGETS lightusd_tinyvdb EXPORT LightUSDTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT LightUSDTargets NAMESPACE lightusd::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LightUSD")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/LightUSDConfig.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/LightUSD")

foreach(c_target IN LISTS LIGHTUSD_C_TARGETS)
    target_compile_features(${c_target} PRIVATE cxx_std_17)
    target_include_directories(${c_target}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../c-api>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
               $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    if(MSVC)
        target_compile_options(${c_target} PRIVATE /W4)
        if(NOT LIGHTUSD_CXX_RTTI)
            target_compile_options(${c_target} PRIVATE /GR-)
        endif()
        if(NOT LIGHTUSD_CXX_EXCEPTIONS)
            target_compile_options(${c_target} PRIVATE /EHs-c-)
        endif()
    else()
        target_compile_options(${c_target} PRIVATE -Wall -Wextra -Wpedantic)
        if(NOT LIGHTUSD_CXX_RTTI)
            target_compile_options(${c_target} PRIVATE -fno-rtti)
        endif()
        if(NOT LIGHTUSD_CXX_EXCEPTIONS)
            target_compile_options(${c_target} PRIVATE -fno-exceptions)
        endif()
    endif()
endforeach()

# Tydra Next test
if(LIGHTUSD_NEXT_BUILD_TESTS)
    if(LIGHTUSD_WITH_USDOBJ)
        add_executable(test_obj_reader ../../tests/next/test_obj_reader.cc)
        target_link_libraries(test_obj_reader PRIVATE lightusd_next)
        add_test(NAME next_test_obj_reader COMMAND test_obj_reader)
        add_executable(test_obj_large ../../tests/next/test_obj_large.cc)
        target_link_libraries(test_obj_large PRIVATE lightusd_next)
        add_test(NAME next_test_obj_large COMMAND test_obj_large)
        add_executable(test_obj_materials ../../tests/next/test_obj_materials.cc)
        target_compile_features(test_obj_materials PRIVATE cxx_std_17)
        target_include_directories(test_obj_materials PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_obj_materials PRIVATE lightusd_obj_import)
        add_test(NAME next_test_obj_materials COMMAND test_obj_materials)
        add_executable(test_obj_textures ../../tests/next/test_obj_textures.cc)
        target_link_libraries(test_obj_textures PRIVATE lightusd_next)
        add_test(NAME next_test_obj_textures COMMAND test_obj_textures)

    endif()

  if(LIGHTUSD_WITH_TYDRA)
    add_executable(test_tydra_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/tydra/next/test_tydra_next.cc
    )
    target_link_libraries(test_tydra_next PRIVATE tydra_next lightusd_next)

    add_executable(test_asset_import ../../tests/next/test_asset_import.cc)
    target_link_libraries(test_asset_import PRIVATE lightusd_render_c)
    add_test(NAME next_test_asset_import COMMAND test_asset_import)

    add_executable(test_texture_alias ../../tests/next/test_texture_alias.cc)
    target_link_libraries(test_texture_alias PRIVATE tydra_next lightusd_next)
    add_test(NAME next_test_texture_alias COMMAND test_texture_alias)

    # URDF/MJCF JSON -> USD converter (next port of the legacy builders).
    add_executable(test_urdf_to_usd_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_urdf_to_usd_next.cc
    )
    target_link_libraries(test_urdf_to_usd_next PRIVATE tydra_next lightusd_next)

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

  endif()

    # Performance benchmark
    add_executable(benchmark_next
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/benchmark_next.cc
    )
    target_link_libraries(benchmark_next PRIVATE lightusd_next)

    # Register tests with ctest
    enable_testing()

    add_executable(test_physics_query ../../tests/next/test_physics_query.cc)
    target_link_libraries(test_physics_query PRIVATE lightusd_c)
    add_test(NAME next_test_physics_query COMMAND test_physics_query)

    add_executable(test_validation_report ../../tests/next/test_validation_report.cc)
    target_link_libraries(test_validation_report PRIVATE lightusd_validation_c)
    add_test(NAME next_test_validation_report COMMAND test_validation_report)

    add_executable(test_lightusd_cpp ../../tests/c-api/test_lightusd_cpp.cc)
    target_link_libraries(test_lightusd_cpp PRIVATE lightusd_c)
    target_compile_features(test_lightusd_cpp PRIVATE cxx_std_17)
    if(LIGHTUSD_WITH_TYDRA)
        target_link_libraries(test_lightusd_cpp PRIVATE lightusd_render_c)
        target_compile_definitions(test_lightusd_cpp PRIVATE LIGHTUSD_TEST_RENDER_API=1)
    endif()
    add_test(NAME next_test_cpp_facade COMMAND test_lightusd_cpp)
    if(LIGHTUSD_WITH_TYDRA)
        add_executable(test_lightusd_document_session
            ../../tests/c-api/test_lightusd_document_session.c)
        target_link_libraries(test_lightusd_document_session PRIVATE
            lightusd_render_c)
        set_target_properties(test_lightusd_document_session PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(NOT MSVC)
            target_compile_options(test_lightusd_document_session PRIVATE
                -Wall -Wextra -Werror -pedantic)
        endif()
        add_test(NAME next_test_document_session_c
            COMMAND test_lightusd_document_session
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/cube.usda
                    ${CMAKE_CURRENT_BINARY_DIR}/document-change-records.usda)
        set_tests_properties(next_test_document_session_c PROPERTIES LABELS "next;c-api;render")
        add_executable(test_lightusd_render_session
            ../../tests/c-api/test_lightusd_render_session.c)
        target_link_libraries(test_lightusd_render_session PRIVATE
            lightusd_render_c)
        set_target_properties(test_lightusd_render_session PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(NOT MSVC)
            target_compile_options(test_lightusd_render_session PRIVATE
                -Wall -Wextra -Werror -pedantic)
        endif()
        add_test(NAME next_test_render_session_c
            COMMAND test_lightusd_render_session)
        add_executable(test_lightusd_volume_c
            ../../tests/c-api/test_lightusd_volume_c.c)
        target_link_libraries(test_lightusd_volume_c PRIVATE
            lightusd_render_c)
        set_target_properties(test_lightusd_volume_c PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(NOT MSVC)
            target_compile_options(test_lightusd_volume_c PRIVATE
                -Wall -Wextra -Werror -pedantic)
        endif()
        # Needs the out-of-tree tinyvdbio reference corpus; skips without it.
        add_test(NAME next_test_volume_c COMMAND test_lightusd_volume_c)
        set_tests_properties(next_test_volume_c PROPERTIES
                             LABELS "next;c-api;render;volume")
        if(TARGET lightusd_tinyvdb)
            add_executable(test_next_level_set_allocator
                ../../tests/volumetric/test-level-set-allocator.c)
            target_link_libraries(test_next_level_set_allocator PRIVATE lightusd_tinyvdb lightusd_next)
            set_target_properties(test_next_level_set_allocator PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
            add_test(NAME next_test_level_set_allocator COMMAND test_next_level_set_allocator)
            set_tests_properties(next_test_level_set_allocator PROPERTIES LABELS "next;cpu;volume;security")
        endif()
        if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND TARGET lightusd_tinyvdb)
            add_executable(test_next_tet_overflow_32
                ../../tests/volumetric/tet-overflow-32.c)
            target_include_directories(test_next_tet_overflow_32 PRIVATE
                ../../src/external/tinyvdb)
            target_link_libraries(test_next_tet_overflow_32 PRIVATE lightusd_tinyvdb)
            add_test(NAME next_test_tet_overflow_32 COMMAND test_next_tet_overflow_32)
            set_tests_properties(next_test_tet_overflow_32 PROPERTIES
                                 LABELS "next;cpu;volume;security")
        endif()
        add_executable(test_lightusd_level_set_c
            ../../tests/c-api/test_lightusd_level_set_c.c)
        target_link_libraries(test_lightusd_level_set_c PRIVATE
            lightusd_render_c)
        set_target_properties(test_lightusd_level_set_c PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(NOT MSVC)
            target_compile_options(test_lightusd_level_set_c PRIVATE
                -Wall -Wextra -Werror -pedantic)
        endif()
        add_test(NAME next_test_level_set_c COMMAND test_lightusd_level_set_c)
        set_tests_properties(next_test_level_set_c PROPERTIES
                             LABELS "next;c-api;volume")
    endif()

    # C API smoke test (pure C11 translation unit)
    add_executable(test_lightusd_c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/c-api/test_lightusd_c.c
    )
    if(LIGHTUSD_NEXT_BUILD_SHARED_C_API)
        target_link_libraries(test_lightusd_c PRIVATE lightusd_c_shared)
        target_compile_definitions(test_lightusd_c PRIVATE LIGHTUSD_C_USE_SHARED=1)
    else()
        target_link_libraries(test_lightusd_c PRIVATE lightusd_validation_c)
    endif()
    # Sources stay pure C11, but link with the C++ driver: the C API library is
    # C++, and under -fsanitize=undefined its vptr checks need the C++ UBSan
    # runtime that only the C++ driver adds.
    set_target_properties(test_lightusd_c PROPERTIES C_STANDARD 11
                          LINKER_LANGUAGE CXX)
    if(NOT MSVC)
        target_compile_options(test_lightusd_c PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME next_test_c_api COMMAND test_lightusd_c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/cube.usda
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/instancing-001.usda
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/c-core-relationships.usda
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usda/clips-primmeta-001.usda
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/usdc/schema-blendshape-001.usdc
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
    add_test(NAME next_test_dtoa COMMAND test_dtoa
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # Opt-in: all 2^32 float bit patterns, fast path vs dragonbox reference
    # (~minutes; multithreaded with LIGHTUSD_NEXT_ENABLE_THREAD).
    option(LIGHTUSD_NEXT_DTOA_EXHAUSTIVE_TEST
        "Register the exhaustive all-float dtoa equivalence ctest" OFF)
    if(LIGHTUSD_NEXT_DTOA_EXHAUSTIVE_TEST)
        add_test(NAME next_test_dtoa_exhaustive COMMAND test_dtoa exhaustive
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(next_test_dtoa_exhaustive PROPERTIES
            LABELS "exhaustive" TIMEOUT 3600)
    endif()
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
    add_test(NAME next_test_crate_limits COMMAND test_crate_limits
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_load_rules COMMAND test_load_rules
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      add_test(NAME next_c11_syntax
          COMMAND ${CMAKE_COMMAND} -DC_COMPILER=${CMAKE_C_COMPILER}
              -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/../..
              -P ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/check-c11.cmake)
    endif()
    add_test(NAME next_test_array_parser COMMAND test_array_parser)
    add_test(NAME next_test_array_parser_c COMMAND test_array_parser_c)
    add_test(NAME next_test_path_expression_c_core COMMAND test_path_expression_c_core
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_layer_diff COMMAND test_layer_diff
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_memory_flatten COMMAND test_memory_flatten
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_resolver COMMAND test_resolver
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_validation COMMAND test_validation
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME next_test_aousd_conformance COMMAND test_aousd_conformance
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(LIGHTUSD_WITH_TYDRA)
        add_test(NAME next_test_tydra COMMAND test_tydra_next
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        # Convex hull for face-less MJCF mesh assets; lives in asset-import.
        if(TARGET lightusd_asset_import)
            add_executable(test_convex_hull
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test_convex_hull.cc
            )
            target_link_libraries(test_convex_hull PRIVATE lightusd_asset_import
                                                  lightusd_next)
            target_include_directories(test_convex_hull PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/..)
            target_compile_features(test_convex_hull PRIVATE cxx_std_17)
            add_test(NAME next_test_convex_hull COMMAND test_convex_hull
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        endif()
        add_test(NAME next_test_urdf_to_usd COMMAND test_urdf_to_usd_next
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    add_test(NAME next_benchmark COMMAND benchmark_next
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(LIGHTUSD_WITH_TYDRA)
    add_test(NAME next_bench_tydra_render COMMAND bench_tydra_render
        --iters 1 --prims 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(next_bench_tydra_render PROPERTIES LABELS "benchmark")
    endif()

    set(LIGHTUSD_NEXT_SMOKE_TESTS
        next_test_lightusd
        next_test_layer
        next_test_stage
        next_test_pcp
        next_test_pcp_parallel
        next_test_instance_group
        next_test_array_edit_write
        next_test_writer
        next_test_dtoa
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
         next_test_crate_limits
         next_test_load_rules
         next_test_path_expression_c_core
         next_test_layer_diff
        next_test_memory_flatten
        next_test_resolver
        next_test_validation
        next_test_aousd_conformance)
    if(LIGHTUSD_WITH_TYDRA)
        list(APPEND LIGHTUSD_NEXT_SMOKE_TESTS next_test_tydra)
    endif()
    set_tests_properties(${LIGHTUSD_NEXT_SMOKE_TESTS}
        PROPERTIES LABELS "next")
    set_tests_properties(next_benchmark
        PROPERTIES LABELS "next;benchmark")

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME next_usdcat_options
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test-usdcat-options.py
                    "$<TARGET_FILE:next_usdcat>")
        set_tests_properties(next_usdcat_options PROPERTIES LABELS "next" TIMEOUT 60)
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
    set(LIGHTUSD_NEXT_PXR_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/../../../OpenUSD/dist/lib/python"
        CACHE PATH "Pinned OpenUSD Python package for sampled-value differential tests")
    if(Python3_Interpreter_FOUND AND EXISTS "${LIGHTUSD_NEXT_PXR_PYTHON}/pxr/Usd/__init__.py")
        add_test(NAME next_aousd_sampled_value_pxrdiff
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/compare-aousd-sampled-values.py
                --test "$<TARGET_FILE:test_aousd_value_resolution>"
                --root "${LIGHTUSD_AOUSD_SUPPLEMENTAL_ROOT}"
                --pxr-python "${LIGHTUSD_NEXT_PXR_PYTHON}")
        set_tests_properties(next_aousd_sampled_value_pxrdiff PROPERTIES
            SKIP_RETURN_CODE 77 LABELS "next;aousd;pxrdiff" TIMEOUT 120)
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
        add_test(
            NAME next_compare_usda_booleans
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test-compare-usda-booleans.cjs)
        set_tests_properties(next_compare_usda_booleans
            PROPERTIES LABELS "next;pxrdiff")
        add_test(
            NAME next_compare_usda_listops
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test-compare-usda-listops.cjs)
        set_tests_properties(next_compare_usda_listops
            PROPERTIES LABELS "next;pxrdiff")
        add_test(
            NAME next_compare_usda_references
            COMMAND ${LIGHTUSD_NEXT_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/next/test-compare-usda-references.cjs)
        set_tests_properties(next_compare_usda_references
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
