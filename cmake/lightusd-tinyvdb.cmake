# Vendored tinyvdb (src/external/tinyvdb): OpenVDB I/O, volume access
# (grid info, typed dense extraction, sampling, NanoVDB conversion) and the
# sparse level-set, meshing and isosurface-stuffing tools. One target, shared by the
# root library (usdVol) and lightusd_asset_import (SDF/volumetric import), so
# executables never link two copies of the tvdb_* symbols.
get_filename_component(_lusd_tinyvdb_root "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
option(LIGHTUSD_TINYVDB_GPU
  "Build tinyvdb's runtime-loaded CUDA/HIP level-set and stuffing kernels (opt-in device)" OFF)
if(NOT TARGET lightusd_tinyvdb)
  set(_lusd_tinyvdb_dir "${_lusd_tinyvdb_root}/external/tinyvdb")
  add_library(lightusd_tinyvdb STATIC
    "${_lusd_tinyvdb_dir}/tinyvdb_io.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_lifecycle.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_mesh.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_sdf_tree.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_sparse.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_sparse_tools.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_sparse_tree.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_tetmesh.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_tet_refine.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_thread.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_to_nanovdb.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_tree.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_volume.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_winding.c"
    "${_lusd_tinyvdb_dir}/tvdb_memory.c")
  # This target owns miniz (and, when LIGHTUSD_TINYVDB_ZSTD is set, the
  # bundled zstd) for every build: the legacy root drops them from its own
  # sources and gets them through lightusd_asset_import. Without zstd,
  # BLOSC-zstd .vdb files are rejected. LZ4 comes from lightusd_next / the
  # root sources.
  target_sources(lightusd_tinyvdb PRIVATE "${_lusd_tinyvdb_root}/external/miniz.c")
  set_source_files_properties("${_lusd_tinyvdb_root}/external/miniz.c"
    PROPERTIES COMPILE_DEFINITIONS "_LARGEFILE64_SOURCE=1")
  if(LIGHTUSD_TINYVDB_ZSTD)
    target_sources(lightusd_tinyvdb PRIVATE "${_lusd_tinyvdb_root}/external/zstd.c")
    set_source_files_properties("${_lusd_tinyvdb_root}/external/zstd.c"
      PROPERTIES COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM=1" COMPILE_FLAGS "-w")
    target_compile_definitions(lightusd_tinyvdb PRIVATE LIGHTUSD_TINYVDB_ZSTD=1)
  endif()
  set_target_properties(lightusd_tinyvdb PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    # The threaded paths use <stdatomic.h>, which MSVC gates behind this flag
    # (with /std:c11, from C_STANDARD 11).
    target_compile_options(lightusd_tinyvdb PRIVATE
      $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
  endif()
  target_include_directories(lightusd_tinyvdb PUBLIC "$<BUILD_INTERFACE:${_lusd_tinyvdb_dir}>")
  find_package(Threads)
  if(TARGET Threads::Threads)
    target_link_libraries(lightusd_tinyvdb PUBLIC Threads::Threads)
  endif()
  if(UNIX)
    target_link_libraries(lightusd_tinyvdb PUBLIC m)
  endif()
  if(EMSCRIPTEN)
    # No C11 thread runtime in single-threaded WASM builds: serial tasks.
    target_compile_definitions(lightusd_tinyvdb PUBLIC TINYVDB_THREAD_NONE=1)
  endif()
  # Optional CUDA/HIP level sets and stuffing (tinyvdb_gpu_compute): the
  # driver libraries are loaded at runtime, so this needs no GPU SDK and
  # still runs, on the CPU, where no device exists. The CPU stays the default
  # device either way (`volumetric_device`).
  if(LIGHTUSD_TINYVDB_GPU AND NOT EMSCRIPTEN)
    # The device side of the level-set sampler is a normal header; NVRTC and
    # hiprtc need its text, generated here as a C string.
    set(_lusd_tinyvdb_device_inc
        "${CMAKE_CURRENT_BINARY_DIR}/tinyvdb/generated/tinyvdb_nanovdb_device_src.inc")
    add_custom_command(
      OUTPUT "${_lusd_tinyvdb_device_inc}"
      COMMAND ${CMAKE_COMMAND}
              -DIN=${_lusd_tinyvdb_dir}/tinyvdb_nanovdb_device.h
              -DOUT=${_lusd_tinyvdb_device_inc}
              -P ${CMAKE_CURRENT_LIST_DIR}/embed-c-string.cmake
      DEPENDS "${_lusd_tinyvdb_dir}/tinyvdb_nanovdb_device.h"
              "${CMAKE_CURRENT_LIST_DIR}/embed-c-string.cmake"
      VERBATIM)
    target_sources(lightusd_tinyvdb PRIVATE
                   "${_lusd_tinyvdb_dir}/tinyvdb_gpu_compute.c"
                   "${_lusd_tinyvdb_device_inc}")
    target_include_directories(lightusd_tinyvdb PRIVATE
                               "${CMAKE_CURRENT_BINARY_DIR}/tinyvdb/generated")
    target_compile_definitions(lightusd_tinyvdb PUBLIC LIGHTUSD_HAVE_TINYVDB_GPU=1)
    target_link_libraries(lightusd_tinyvdb PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
