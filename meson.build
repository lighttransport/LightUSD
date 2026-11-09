# LightUSD Meson build
# Source list synchronized with CMakeLists.txt

project('lightusd', ['c', 'cpp'],
  version: '1.0.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
  ],
  meson_version: '>= 0.56.0',
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

# ============================================================================
# Options
# ============================================================================

opt_tydra = get_option('tydra')
opt_builtin_image_loader = get_option('builtin_image_loader')
opt_usdmtlx = get_option('usdmtlx')
opt_json = get_option('json')
opt_usdobj = get_option('usdobj')
opt_usdvox = get_option('usdvox')
opt_exr = get_option('exr')
opt_colorio = get_option('colorio')
opt_audio = get_option('audio')
opt_zstd = get_option('zstd')
opt_pxr_compat_api = get_option('pxr_compat_api')
opt_usdfbx = get_option('usdfbx')
opt_meshopt = get_option('meshopt')
opt_alac_audio = get_option('alac_audio')
opt_mcp_server = get_option('mcp_server')
opt_qjs = get_option('qjs')
opt_wamr = get_option('wamr')
opt_geogram = get_option('geogram')
opt_remotery = get_option('remotery')
opt_production_build = get_option('production_build')
opt_debug_print = get_option('debug_print')
opt_shared_lib = get_option('shared_lib')
opt_system_zlib = get_option('system_zlib')
opt_system_zstd = get_option('system_zstd')

# MCP server requires JSON
if opt_mcp_server
  opt_json = true
endif

# ============================================================================
# Compile definitions
# ============================================================================

lightusd_defines = []

if opt_json
  lightusd_defines += '-DLIGHTUSD_WITH_JSON'
endif
if opt_usdmtlx
  lightusd_defines += '-DLIGHTUSD_USE_USDMTLX'
endif
if opt_usdobj
  lightusd_defines += '-DLIGHTUSD_USE_USDOBJ'
endif
if opt_usdvox
  lightusd_defines += '-DLIGHTUSD_USE_USDVOX'
endif
if opt_usdfbx
  lightusd_defines += '-DLIGHTUSD_USE_USDFBX'
endif
if opt_exr
  lightusd_defines += '-DLIGHTUSD_WITH_EXR'
endif
if opt_colorio
  lightusd_defines += '-DLIGHTUSD_WITH_COLORIO'
endif
if opt_audio
  lightusd_defines += '-DLIGHTUSD_WITH_AUDIO'
endif
if opt_alac_audio
  lightusd_defines += '-DLIGHTUSD_WITH_ALAC_AUDIO'
endif
if opt_tydra
  lightusd_defines += '-DLIGHTUSD_WITH_TYDRA'
endif
if opt_zstd
  lightusd_defines += '-DLIGHTUSD_WITH_ZSTD_COMPRESSION'
endif
if opt_meshopt
  lightusd_defines += '-DLIGHTUSD_WITH_MESHOPT'
endif
if opt_geogram
  lightusd_defines += ['-DLIGHTUSD_WITH_GEOGRAM', '-DGEO_STATIC_LIBS', '-DGEOGRAM_WITH_LEGACY_NUMERICS']
endif
if opt_wamr
  lightusd_defines += '-DLIGHTUSD_WITH_WAMR'
  if host_machine.system() == 'windows'
    lightusd_defines += '-DBH_PLATFORM_WINDOWS=1'
  else
    lightusd_defines += '-DBH_PLATFORM_LINUX=1'
  endif
endif
if opt_mcp_server
  lightusd_defines += ['-DLIGHTUSD_WITH_MCP_SERVER', '-DOPENSSL_API_3_0']
endif
if opt_qjs
  lightusd_defines += '-DLIGHTUSD_WITH_QJS'
endif
if opt_remotery
  lightusd_defines += '-DRMT_ENABLED=1'
  if host_machine.system() == 'windows'
    lightusd_defines += '-DRMT_USE_D3D11=0'
  elif host_machine.system() == 'darwin'
    lightusd_defines += '-DRMT_USE_METAL=0'
  elif host_machine.system() == 'linux'
    lightusd_defines += '-DRMT_USE_OPENGL=0'
  endif
endif
if opt_production_build
  lightusd_defines += '-DLIGHTUSD_PRODUCTION_BUILD'
endif
if opt_debug_print
  lightusd_defines += '-DLIGHTUSD_DEBUG_PRINT'
endif
if not get_option('builtin_image_loader')
  lightusd_defines += '-DLIGHTUSD_NO_BUILTIN_IMAGE_LOADER'
endif

# Inverted module flags
if not get_option('module_usda_reader')
  lightusd_defines += '-DLIGHTUSD_DISABLE_MODULE_USDA_READER'
endif
if not get_option('module_usda_writer')
  lightusd_defines += '-DLIGHTUSD_DISABLE_MODULE_USDA_WRITER'
endif
if not get_option('module_usdc_reader')
  lightusd_defines += '-DLIGHTUSD_DISABLE_MODULE_USDC_READER'
endif
if not get_option('module_usdc_writer')
  lightusd_defines += '-DLIGHTUSD_DISABLE_MODULE_USDC_WRITER'
endif

# ============================================================================
# Core sources (always compiled)
# ============================================================================

lightusd_sources = files(
  # Crate path utilities (path sorting / tree encoding for the USDC writer).
  # Formerly the sandbox/path-sort-and-encode-crate subproject; now part of the lib.
  'src/legacy/crate-path-utils/path_sort.cc',
  'src/legacy/crate-path-utils/tree_encode.cc',
  'src/legacy/arg-parser.cc',
  'src/legacy/asset-resolution.cc',
  'src/legacy/lightusd.cc',
  'src/legacy/xform.cc',
  'src/legacy/performance.cc',
  'src/legacy/ascii-parser.cc',
  'src/legacy/ascii-parser-props.cc',
  'src/legacy/ascii-parser-entry.cc',
  'src/legacy/ascii-parser-basetype.cc',
  'src/legacy/ascii-parser-basetype-inst.cc',
  'src/legacy/ascii-parser-timesamples.cc',
  'src/legacy/ascii-parser-timesamples-array.cc',
  'src/legacy/audio-loader.cc',
  'src/legacy/base122.cc',
  'src/legacy/usda-reader.cc',
  'src/legacy/usda-reader-reconstruct-1.cc',
  'src/legacy/usda-reader-reconstruct-2.cc',
  'src/legacy/usda-reader-reconstruct-3.cc',
  'src/legacy/usda-reader-reconstruct-4.cc',
  'src/legacy/usdc-reader.cc',
  'src/legacy/usdc-reader-property.cc',
  'src/legacy/usdc-reader-prim.cc',
  'src/legacy/usdc-reader-prim-parse.cc',
  'src/legacy/usdc-reader-prim-reconstruct-1.cc',
  'src/legacy/usdc-reader-prim-reconstruct-2.cc',
  'src/legacy/usdc-reader-prim-reconstruct-3.cc',
  'src/legacy/usdc-reader-prim-reconstruct-4.cc',
  'src/legacy/usdc-reader-reconstruct.cc',
  'src/legacy/usda-writer.cc',
  'src/legacy/usdc-writer.cc',
  'src/legacy/composition.cc',
  'src/legacy/composition-reconstruct.cc',
  'src/legacy/chunk-reader.cc',
  'src/legacy/crate-reader.cc',
  'src/legacy/crate-reader-arrays.cc',
  'src/legacy/crate-reader-values.cc',
  'src/legacy/crate-reader-paths.cc',
  'src/legacy/crate-reader-timesamples.cc',
  'src/legacy/crate-format.cc',
  'src/legacy/crate-writer.cc',
  'src/legacy/crate-writer-values.cc',
  'src/legacy/crate-writer-inline.cc',
  'src/legacy/stage-converter.cc',
  'src/legacy/sconv-geom.cc',
  'src/legacy/sconv-shader.cc',
  'src/legacy/sconv-light.cc',
  'src/legacy/sconv-skel.cc',
  'src/legacy/sconv-layer.cc',
  'src/legacy/crate-pprint.cc',
  'src/legacy/crate-dump.cc',
  'src/legacy/path-util.cc',
  'src/legacy/prim-reconstruct.cc',
  'src/legacy/prim-reconstruct2.cc',
  'src/legacy/prim-reconstruct3.cc',
  'src/legacy/prim-reconstruct-geom.cc',
  'src/legacy/prim-reconstruct-geom2.cc',
  'src/legacy/prim-reconstruct-geom3.cc',
  'src/legacy/prim-reconstruct-lightprim.cc',
  'src/legacy/prim-reconstruct-shader.cc',
  'src/legacy/prim-reconstruct-shader2.cc',
  'src/legacy/prim-reconstruct-shader3.cc',
  'src/legacy/prim-reconstruct-shader4.cc',
  'src/legacy/prim-composition.cc',
  'src/legacy/prim-types.cc',
  'src/legacy/prim-types-schema.cc',
  'src/legacy/core/prim-enums.cc',
  'src/legacy/enum-handlers.cc',
  'src/legacy/layer.cc',
  'src/legacy/primvar.cc',
  'src/legacy/str-util.cc',
  'src/legacy/usd-dump.cc',
  'src/legacy/value-pprint.cc',
  'src/legacy/value-pprint-dispatch.cc',
  'src/legacy/value-pprint-prim.cc',
  'src/legacy/value-types.cc',
  'src/legacy/value-types-typename.cc',
  'src/legacy/value-types-cast.cc',
  'src/legacy/value-types-matrix.cc',
  'src/legacy/value-types-lerp.cc',
  'src/legacy/color-space.cc',
  'src/legacy/tiny-format.cc',
  'src/legacy/tiny-string.cc',
  'src/io-util.cc',
  'src/image-loader.cc',
  'src/image-writer.cc',
  'src/legacy/image-util.cc',
  'src/legacy/linear-algebra.cc',
  'src/legacy/value-eval-util.cc',
  'src/legacy/usdGeom.cc',
  'src/legacy/usdGeom-accessors.cc',
  'src/legacy/usdGeom-primvar-inst-scalar-a.cc',
  'src/legacy/usdGeom-primvar-inst-scalar-b.cc',
  'src/legacy/usdGeom-primvar-inst-vec-a.cc',
  'src/legacy/usdGeom-primvar-inst-vec-b.cc',
  'src/legacy/usdGeom-primvar-inst-role-a.cc',
  'src/legacy/usdGeom-primvar-inst-role-b.cc',
  'src/legacy/usdSkel.cc',
  'src/legacy/usdShade.cc',
  'src/legacy/usdLux.cc',
  'src/mtlx-xml-tokenizer.cc',
  'src/mtlx-xml-parser.cc',
  'src/core/mtlx-dom.cc',
  'src/legacy/mtlx-simple-parser.cc',
  'src/legacy/usdMtlx.cc',
  'src/legacy/usdMtlx-write.cc',
  'src/legacy/usdObj.cc',
  'src/legacy/pprint-enum.cc',
  'src/legacy/pprint-meta.cc',
  'src/legacy/pprint-geom.cc',
  'src/legacy/pprint-shader.cc',
  'src/legacy/pprint-light.cc',
  'src/legacy/pprint-skel.cc',
  'src/legacy/pprinter.cc',
  'src/legacy/timesamples-pprint.cc',
  'src/legacy/timesamples.cc',
  'src/legacy/timesamples-eval.cc',
  'src/legacy/stage.cc',
  'src/legacy/uuid-gen.cc',
  'src/legacy/parser-timing.cc',
  'src/sha256.cc',
  'src/legacy/typed-array.cc',
  'src/legacy/task-queue.cc',
  'src/legacy/prim-pprint-parallel.cc',
)

# ============================================================================
# Dependency sources (always compiled)
# ============================================================================

lightusd_dep_sources = files(
  'src/legacy/integerCoding.cpp',
  'src/legacy/lz4-compression.cc',
  'src/lz4/lz4.c',
)

# ============================================================================
# Conditional sources
# ============================================================================

# -- Tydra --
if opt_tydra
  lightusd_sources += files(
    'src/legacy/tydra/facial.cc',
    'src/legacy/tydra/prim-apply.cc',
    'src/legacy/tydra/scene-access.cc',
    'src/legacy/tydra/scene-access-listprims-inst.cc',
    'src/legacy/tydra/scene-access-listshaders-inst.cc',
    'src/legacy/tydra/scene-analysis.cc',
    'src/legacy/tydra/attribute-eval.cc',
    'src/legacy/tydra/attribute-eval-typed-all.cc',
    'src/legacy/tydra/attribute-eval-typed-inst-scalar.cc',
    'src/legacy/tydra/attribute-eval-typed-inst-array.cc',
    'src/legacy/tydra/command-and-history.cc',
    'src/legacy/tydra/obj-export.cc',
    'src/legacy/tydra/usd-export.cc',
    'src/legacy/tydra/shader-network.cc',
    'src/legacy/tydra/render-data.cc',
    'src/legacy/tydra/render-data-mesh.cc',
    'src/legacy/tydra/render-data-mesh-tangent.cc',
    'src/legacy/tydra/render-data-material.cc',
    'src/legacy/tydra/render-data-material-mtlx.cc',
    'src/legacy/tydra/render-data-anim.cc',
    'src/legacy/tydra/render-data-pprint.cc',
    'src/legacy/tydra/raytracing-data.cc',
    'src/legacy/tydra/raytracing-scene-converter.cc',
    'src/legacy/tydra/material-serializer.cc',
    'src/legacy/tydra/materialx-to-json.cc',
    'src/legacy/tydra/render-scene-dump.cc',
    'src/legacy/tydra/bone-util.cc',
    'src/legacy/tydra/layer-to-renderscene.cc',
    'src/tydra/texture-util.cc',
    'src/legacy/tydra/variant-support.cc',
    'src/legacy/tydra/variant-converter.cc',
    'src/legacy/tydra/variant-applier.cc',
    'src/legacy/tydra/mcp.cc',
    'src/legacy/tydra/mcp-tools.cc',
    'src/legacy/tydra/mcp-resources.cc',
    'src/legacy/tydra/mcp-server.cc',
    'src/legacy/tydra/diff-and-compare.cc',
    'src/legacy/tydra/js-script.cc',
    'src/legacy/tydra/threejs-exporter.cc',
  )
  lightusd_dep_sources += files('src/external/mikktspace/mikktspace.c')

  if opt_wamr
    lightusd_sources += files('src/legacy/tydra/wasm-runtime.cc')
  endif
endif

# -- pxr compat API --
if opt_pxr_compat_api
  lightusd_sources += files('src/legacy/pxr-compat.cc')
endif

# -- usdMtlx (pugixml) --
if opt_usdmtlx
  lightusd_dep_sources += files('src/external/pugixml.cpp')
endif

# -- JSON --
if opt_json
  lightusd_sources += files(
    'src/legacy/json-to-usd.cc',
    'src/legacy/usd-to-json.cc',
    'src/legacy/json-writer.cc',
    'src/legacy/json-util.cc',
  )
  lightusd_dep_sources += files('src/external/yyjson.c')
endif

# -- usdFbx --
if opt_usdfbx
  lightusd_sources += files('src/legacy/usdFbx.cc')
  lightusd_dep_sources += files('src/external/OpenFBX/src/ofbx.cpp')
endif

# -- usdObj --
if opt_usdobj
  lightusd_dep_sources += files('src/external/tiny_obj_loader.cc')
endif

# -- usdVox --
if opt_usdvox
  lightusd_sources += files('src/legacy/usdVox.cc')
endif

# -- ALAC audio --
if opt_alac_audio
  lightusd_dep_sources += files(
    'src/external/alac/codec/EndianPortable.c',
    'src/external/alac/codec/ALACBitUtilities.c',
    'src/external/alac/codec/ALACDecoder.cpp',
    'src/external/alac/codec/ALACEncoder.cpp',
    'src/external/alac/codec/ag_dec.c',
    'src/external/alac/codec/ag_enc.c',
    'src/external/alac/codec/dp_dec.c',
    'src/external/alac/codec/dp_enc.c',
    'src/external/alac/codec/matrix_dec.c',
    'src/external/alac/codec/matrix_enc.c',
  )
endif

# -- MCP server --
if opt_mcp_server
  lightusd_dep_sources += files('src/external/civetweb/civetweb.c')
endif

# -- QuickJS --
if opt_qjs
  lightusd_dep_sources += files(
    'src/external/quickjs-ng/cutils.c',
    'src/external/quickjs-ng/libregexp.c',
    'src/external/quickjs-ng/libunicode.c',
    'src/external/quickjs-ng/quickjs.c',
    'src/external/quickjs-ng/xsum.c',
  )
endif

# -- meshoptimizer --
if opt_meshopt
  lightusd_dep_sources += files(
    'src/external/meshoptimizer/allocator.cpp',
    'src/external/meshoptimizer/clusterizer.cpp',
    'src/external/meshoptimizer/indexanalyzer.cpp',
    'src/external/meshoptimizer/indexcodec.cpp',
    'src/external/meshoptimizer/indexgenerator.cpp',
    'src/external/meshoptimizer/overdrawoptimizer.cpp',
    'src/external/meshoptimizer/partition.cpp',
    'src/external/meshoptimizer/quantization.cpp',
    'src/external/meshoptimizer/rasterizer.cpp',
    'src/external/meshoptimizer/simplifier.cpp',
    'src/external/meshoptimizer/spatialorder.cpp',
    'src/external/meshoptimizer/stripifier.cpp',
    'src/external/meshoptimizer/vcacheoptimizer.cpp',
    'src/external/meshoptimizer/vertexcodec.cpp',
    'src/external/meshoptimizer/vertexfilter.cpp',
    'src/external/meshoptimizer/vfetchoptimizer.cpp',
  )
endif

# -- Remotery --
if opt_remotery
  lightusd_dep_sources += files('src/external/Remotery/Remotery.c')
endif

# -- Geogram --
if opt_geogram
  lightusd_dep_sources += files(
    'src/external/geogram/geogram/third_party/triangle/triangle.c',
  )
endif

# ============================================================================
# Per-file compile flags (built as separate static libs)
# ============================================================================

extra_link_libs = []
lightusd_inc = include_directories('src')

# -- fpng (needs FPNG_NO_SSE=1) --
if opt_builtin_image_loader
  fpng_lib = static_library('lightusd_fpng',
    'src/external/fpng.cpp',
    include_directories: lightusd_inc,
    cpp_args: ['-DFPNG_NO_SSE=1'],
    pic: true,
  )
  extra_link_libs += fpng_lib
endif

# -- zstd (needs ZSTD_DISABLE_ASM=1, suppress warnings) --
if opt_zstd and not opt_system_zstd
  zstd_lib = static_library('lightusd_zstd',
    'src/external/zstd.c',
    include_directories: lightusd_inc,
    c_args: ['-DZSTD_DISABLE_ASM=1', '-w'],
    pic: true,
  )
  extra_link_libs += zstd_lib
  lightusd_dep_sources += files('src/legacy/zstd-compression.cc')
elif opt_zstd and opt_system_zstd
  lightusd_dep_sources += files('src/legacy/zstd-compression.cc')
endif

# -- EXR (tinyexr) --
if opt_exr
  # Use TinyEXR v3 API by default
  exr_c_args = []
  exr_cpp_args = []
  if opt_system_zlib
    exr_c_args += ['-DTINYEXR_V3_HAS_DEFLATE=1', '-DTINYEXR_V3_NO_MINIZ=1']
    exr_cpp_args += ['-DTINYEXR_USE_MINIZ=0']
  endif
  exr_inc = [lightusd_inc]
  if not opt_system_zlib
    exr_inc += include_directories('src/external')
  endif

  exr_lib = static_library('lightusd_exr',
    'src/external/tinyexr_c_impl.c',
    'src/external/tinyexr.cc',
    include_directories: exr_inc,
    c_args: exr_c_args,
    cpp_args: exr_cpp_args,
    pic: true,
  )
  extra_link_libs += exr_lib
endif

# -- miniz (for EXR/TIFF when not using system zlib) --
if (opt_exr or get_option('usdmtlx')) and not opt_system_zlib
  miniz_lib = static_library('lightusd_miniz',
    'src/external/miniz.c',
    include_directories: lightusd_inc,
    c_args: ['-D_LARGEFILE64_SOURCE=1'],
    pic: true,
  )
  extra_link_libs += miniz_lib
endif

# ============================================================================
# tinysubdiv (tsd) sources — dependency-free subdivision surfaces, always built
# ============================================================================

lightusd_sources += files(
  'src/tsd/tsd-validate.cc',
  'src/tsd/tsd-topology.cc',
  'src/tsd/tsd-bilinear.cc',
  'src/tsd/tsd-catmark.cc',
  'src/tsd/tsd-loop.cc',
  'src/tsd/tsd-fvar.cc',
  'src/tsd/tsd-refine.cc',
  'src/tsd/tsd-limit.cc',
  'src/legacy/tsd/tsd-lightusd.cc',
)

# ============================================================================
# WAMR sources
# ============================================================================

wamr_inc = []
if opt_wamr
  wamr_sources = files(
    'src/external/wamr/core/iwasm/common/wasm_runtime_common.c',
    'src/external/wamr/core/iwasm/common/wasm_native.c',
    'src/external/wamr/core/iwasm/common/wasm_exec_env.c',
    'src/external/wamr/core/iwasm/common/wasm_memory.c',
    'src/external/wamr/core/iwasm/common/wasm_c_api.c',
    'src/external/wamr/core/iwasm/interpreter/wasm_loader.c',
    'src/external/wamr/core/iwasm/interpreter/wasm_runtime.c',
    'src/external/wamr/core/iwasm/interpreter/wasm_interp_classic.c',
    'src/external/wamr/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c',
    'src/external/wamr/core/shared/mem-alloc/mem_alloc.c',
    'src/external/wamr/core/shared/mem-alloc/ems/ems_alloc.c',
    'src/external/wamr/core/shared/mem-alloc/ems/ems_hmu.c',
    'src/external/wamr/core/shared/mem-alloc/ems/ems_kfc.c',
    'src/external/wamr/core/shared/utils/bh_assert.c',
    'src/external/wamr/core/shared/utils/bh_common.c',
    'src/external/wamr/core/shared/utils/bh_hashmap.c',
    'src/external/wamr/core/shared/utils/bh_list.c',
    'src/external/wamr/core/shared/utils/bh_log.c',
    'src/external/wamr/core/shared/utils/bh_queue.c',
    'src/external/wamr/core/shared/utils/bh_vector.c',
    'src/external/wamr/core/shared/utils/runtime_timer.c',
  )
  if host_machine.system() == 'windows'
    wamr_sources += files('src/external/wamr/core/shared/platform/windows/platform_init.c')
  else
    wamr_sources += files('src/external/wamr/core/shared/platform/linux/platform_init.c')
  endif

  wamr_inc_dirs = [
    include_directories('src/external/wamr/core/iwasm/include'),
    include_directories('src/external/wamr/core/iwasm/interpreter'),
    include_directories('src/external/wamr/core/iwasm/common'),
    include_directories('src/external/wamr/core/shared/utils'),
    include_directories('src/external/wamr/core/shared/mem-alloc'),
    include_directories('src/external/wamr/core/shared/platform/include'),
  ]
  if host_machine.system() == 'windows'
    wamr_inc_dirs += include_directories('src/external/wamr/core/shared/platform/windows')
  else
    wamr_inc_dirs += include_directories('src/external/wamr/core/shared/platform/linux')
  endif

  wamr_c_args = []
  if host_machine.system() == 'windows'
    wamr_c_args += '-DBH_PLATFORM_WINDOWS=1'
  else
    wamr_c_args += '-DBH_PLATFORM_LINUX=1'
  endif

  wamr_lib = static_library('lightusd_wamr',
    wamr_sources,
    include_directories: wamr_inc_dirs,
    c_args: wamr_c_args,
    pic: true,
  )
  extra_link_libs += wamr_lib
  wamr_inc = wamr_inc_dirs
endif

# ============================================================================
# External dependencies
# ============================================================================

ext_deps = []

# dl library (Linux)
dl_dep = cc.find_library('dl', required: false)
if dl_dep.found()
  ext_deps += dl_dep
endif

# system zstd
if opt_zstd and opt_system_zstd
  ext_deps += dependency('libzstd')
endif

# system zlib
if opt_system_zlib
  ext_deps += dependency('zlib')
endif

# ============================================================================
# (crate path utilities are now compiled directly into the main library from
#  src/crate-path-utils/; the former sandbox/path-sort-and-encode-crate subproject
#  is deprecated.)
# ============================================================================

# ============================================================================
# Main static library
# ============================================================================

all_inc = [lightusd_inc] + wamr_inc
if opt_geogram
  all_inc += include_directories('src/external/geogram')
endif
if opt_remotery
  all_inc += include_directories('src/external/Remotery')
endif

lightusd_static = static_library('lightusd_static',
  lightusd_sources + lightusd_dep_sources,
  include_directories: all_inc,
  dependencies: ext_deps,
  cpp_args: lightusd_defines,
  c_args: lightusd_defines,
  link_with: extra_link_libs,
  pic: true,
)

# ============================================================================
# Optional shared library
# ============================================================================

if opt_shared_lib
  shared_defines = lightusd_defines + ['-DLIGHTUSD_COMPILE_LIBRARY', '-DLIGHTUSD_SHARED_LIBRARY']

  lightusd_shared = shared_library('lightusd',
    lightusd_sources + lightusd_dep_sources,
    include_directories: all_inc,
    dependencies: ext_deps,
    cpp_args: shared_defines,
    c_args: shared_defines,
    link_with: extra_link_libs,
  )
endif

# ============================================================================
# Declare dependency for subproject consumers
# ============================================================================

lightusd_dep = declare_dependency(
  link_with: lightusd_static,
  include_directories: lightusd_inc,
)
