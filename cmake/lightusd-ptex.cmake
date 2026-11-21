# SPDX-License-Identifier: Apache-2.0
# Sources of the vendored tptex library (src/external/textools/ptex) that the
# renderers need: reader, half, utils, status, allocator, memory input, inflate
# (against the in-tree miniz) and the seam-aware sampler. The writer and the
# filters are not included. Include() this from the directory that builds the
# sources (source-file properties are directory scoped).
#
#   LIGHTUSD_PTEX_C_SOURCES       always-available core
#   LIGHTUSD_PTEX_C_FILE_SOURCES  tptex_file.c (filesystem input)
get_filename_component(_lightusd_tptex_dir
    "${CMAKE_CURRENT_LIST_DIR}/../src/external/textools/ptex" ABSOLUTE)
set(LIGHTUSD_PTEX_C_SOURCES
    "${_lightusd_tptex_dir}/src/tptex_alloc.c"
    "${_lightusd_tptex_dir}/src/tptex_half.c"
    "${_lightusd_tptex_dir}/src/tptex_inflate.c"
    "${_lightusd_tptex_dir}/src/tptex_io.c"
    "${_lightusd_tptex_dir}/src/tptex_reader.c"
    "${_lightusd_tptex_dir}/src/tptex_sample.c"
    "${_lightusd_tptex_dir}/src/tptex_status.c"
    "${_lightusd_tptex_dir}/src/tptex_utils.c")
set(LIGHTUSD_PTEX_C_FILE_SOURCES "${_lightusd_tptex_dir}/src/tptex_file.c")
# miniz is found through the src/ include root; float code must not be
# contracted into FMAs so results match across compilers and targets.
set_source_files_properties(${LIGHTUSD_PTEX_C_SOURCES} ${LIGHTUSD_PTEX_C_FILE_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS "TPTEX_MINIZ_HEADER=\"external/miniz.h\"")
if(NOT MSVC)
  set_property(SOURCE ${LIGHTUSD_PTEX_C_SOURCES} ${LIGHTUSD_PTEX_C_FILE_SOURCES}
      APPEND PROPERTY COMPILE_OPTIONS "-ffp-contract=off")
endif()
