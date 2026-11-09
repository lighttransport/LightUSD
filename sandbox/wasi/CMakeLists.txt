cmake_minimum_required(VERSION 3.19)

project(lightusd_wasi C CXX)
set(PROJECT_NAME lightusd_wasi)

set(MAIN_SOURCE main.cc)

option(WASI_SDK_PATH "wasi-sdk path")

set(CMAKE_SYSROOT "${WASI_SDK_PATH}/share/wasi-sysroot")

# C++ exceptions is not availble.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")

# wasm specific settings
# https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/doc/build_wasm_app.md
# - Stack size: increased to 64KB.
#   Default is 16KB, which is not enough for some USDZ files with deep hierarchies.
#   TODO: Rewrite recursive function call with for-loop to save stack size.
#   (e.g. ReconstructPrimRecursively))
# - Initial memory: 128KB(Must be larger than stack size
set(CMAKE_LINK_FLAGS "${CMAKE_LINK_FLAGS} -z stack-size=65536 -Wl,--initial-memory=131072")

list(
  APPEND
  LIGHTUSD_SOURCES
  ../../src/legacy/lightusd.cc
  ../../src/io-util.cc
  ../../src/legacy/prim-types.cc
  ../../src/legacy/pprinter.cc
  ../../src/legacy/path-util.cc
  ../../src/legacy/str-util.cc
  ../../src/legacy/value-pprint.cc
  ../../src/legacy/value-types.cc
  ../../src/legacy/primvar.cc
  ../../src/legacy/prim-types.cc
  ../../src/legacy/crate-format.cc
  ../../src/legacy/crate-reader.cc
  ../../src/legacy/crate-reader-arrays.cc
  ../../src/legacy/crate-reader-values.cc
  ../../src/legacy/crate-reader-paths.cc
  ../../src/legacy/crate-writer.cc
  ../../src/legacy/crate-pprint.cc
  ../../src/legacy/ascii-parser.cc
  ../../src/legacy/ascii-parser-props.cc
  ../../src/legacy/ascii-parser-entry.cc
  ../../src/legacy/ascii-parser-basetype.cc
  ../../src/legacy/ascii-parser-timesamples.cc
  ../../src/legacy/ascii-parser-timesamples-array.cc
  ../../src/legacy/usda-reader.cc
  ../../src/legacy/usda-writer.cc
  ../../src/legacy/usdc-reader.cc
  ../../src/legacy/usdc-reader-property.cc
  ../../src/legacy/usdc-reader-prim.cc
  ../../src/legacy/usdc-reader-reconstruct.cc
  ../../src/legacy/usdc-writer.cc
  ../../src/image-loader.cc
  ../../src/legacy/prim-reconstruct.cc
  ../../src/legacy/prim-reconstruct-shader.cc
  ../../src/legacy/prim-composition.cc
  ../../src/legacy/tiny-format.cc
  ../../src/legacy/xform.cc
  ../../src/legacy/usdGeom.cc
  ../../src/legacy/usdLux.cc
  ../../src/legacy/usdShade.cc
  ../../src/legacy/usdSkel.cc
  ../../src/legacy/usdMtlx.cc
  ../../src/legacy/usdObj.cc
  ../../src/legacy/tydra/render-data.cc
  ../../src/legacy/tydra/render-data-mesh.cc
  ../../src/legacy/tydra/render-data-material.cc
  ../../src/legacy/tydra/render-data-anim.cc
  ../../src/legacy/tydra/scene-access.cc
  ../../src/legacy/tydra/shader-network.cc
  ../../src/legacy/stage.cc
)

set(LIGHTUSD_DEP_SOURCES
  ../../src/legacy/integerCoding.cpp
  ../../src/legacy/lz4-compression.cc
  ../../src/lz4/lz4.c
  ../../src/external/fpng.cpp
  #../../src/external/string_id/string_id.cpp
  #../../src/external/string_id/error.cpp
  #../../src/external/string_id/database.cpp
  #../../src/external/staticstruct.cc
  #../../src/external/tinyxml2/tinyxml2.cpp
  )
  #../../src/external/ryu/ryu/s2d.c
  #../../src/external/ryu/ryu/s2f.c)



add_executable(${PROJECT_NAME} ${MAIN_SOURCE} ${LIGHTUSD_SOURCES} ${LIGHTUSD_DEP_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ../../src/external/ryu)
target_include_directories(${PROJECT_NAME} PRIVATE ../../src/external/fast_float/include)
target_include_directories(${PROJECT_NAME} PRIVATE ../../src/)

#set_target_properties(${PROJECT_NAME} PROPERTIES CXX_EXCEPTIONS OFF)
