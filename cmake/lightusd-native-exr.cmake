# SPDX-License-Identifier: Apache-2.0
# Optional, vendored pure-C11 EXR codec for native preview/conversion images.
option(LIGHTUSD_WITH_EXR "Build native EXR/HDR image support" ON)
if(LIGHTUSD_WITH_EXR AND NOT TARGET lightusd_exr_codec)
  get_filename_component(_lightusd_exr_root "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
  file(GLOB _lightusd_exr_sources CONFIGURE_DEPENDS
    "${_lightusd_exr_root}/external/tinyexr/src/*.c")
  add_library(lightusd_exr_codec STATIC
    "${_lightusd_exr_root}/imageio/exr-codec.c" ${_lightusd_exr_sources})
  target_include_directories(lightusd_exr_codec PRIVATE
    "${_lightusd_exr_root}/external/tinyexr/include"
    "${_lightusd_exr_root}/external/tinyexr/src")
  target_compile_definitions(lightusd_exr_codec PRIVATE EXR_NO_JPH EXR_NO_ZSTD)
  set_target_properties(lightusd_exr_codec PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
  if(NOT MSVC)
    target_compile_options(lightusd_exr_codec PRIVATE -w)
  endif()
  if(UNIX)
    target_link_libraries(lightusd_exr_codec PUBLIC m)
  endif()
endif()
