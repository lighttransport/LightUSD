# Vendored tinyvdb (src/external/tinyvdb): OpenVDB I/O plus the sparse
# level-set, meshing and isosurface-stuffing tools. One target, shared by the
# root library (usdVol) and lightusd_asset_import (SDF/volumetric import), so
# executables never link two copies of the tvdb_* symbols.
get_filename_component(_lusd_tinyvdb_root "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
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
    "${_lusd_tinyvdb_dir}/tinyvdb_thread.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_tree.c"
    "${_lusd_tinyvdb_dir}/tinyvdb_winding.c"
    "${_lusd_tinyvdb_dir}/tvdb_memory.c")
  set_target_properties(lightusd_tinyvdb PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
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
endif()
