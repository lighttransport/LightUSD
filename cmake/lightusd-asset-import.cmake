# Shared native frontends, independent of either Stage type. The OBJ-only core
# never creates or links the XML/robot frontend target.
get_filename_component(_lusd_import_root "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
function(lightusd_configure_import_target target root)
  if(COMMAND add_sanitizers)
    add_sanitizers(${target})
  endif()
  target_include_directories(${target} PRIVATE "${root}" "${root}/external")
  target_compile_features(${target} PRIVATE cxx_std_17 c_std_11)
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>")
  endif()
endfunction()
if(NOT TARGET lightusd_obj_import)
  add_library(lightusd_obj_import STATIC
    "${_lusd_import_root}/tydra/asset-import/reader.cc"
    "${_lusd_import_root}/tydra/asset-import/obj.cc"
    "${_lusd_import_root}/external/tinyobjloader-c/tiny_obj_c.c"
    "${_lusd_import_root}/external/tinyobjloader-c/tobj_tess.c")
  lightusd_configure_import_target(lightusd_obj_import "${_lusd_import_root}")
endif()
if(NOT LIGHTUSD_ASSET_IMPORT_OBJ_ONLY AND NOT TARGET lightusd_asset_import)
  add_library(lightusd_asset_import STATIC
    "${_lusd_import_root}/tydra/asset-import/mjcf.cc"
    "${_lusd_import_root}/tydra/asset-import/dae.cc"
    "${_lusd_import_root}/tydra/asset-import/builtin-texture.cc"
    "${_lusd_import_root}/tydra/asset-import/mjcf-sdf.cc"
    "${_lusd_import_root}/external/pugixml.cpp")
  include("${CMAKE_CURRENT_LIST_DIR}/lightusd-tinyvdb.cmake")
  target_link_libraries(lightusd_asset_import PUBLIC lightusd_obj_import lightusd_tinyvdb)
  lightusd_configure_import_target(lightusd_asset_import "${_lusd_import_root}")
endif()
