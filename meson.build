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
  'src/crate-path-utils/path_sort.cc',
  'src/crate-path-utils/tree_encode.cc',
  'src/arg-parser.cc',
  'src/asset-resolution.cc',
  'src/lightusd.cc',
  'src/xform.cc',
  'src/performance.cc',
  'src/ascii-parser.cc',
  'src/ascii-parser-props.cc',
  'src/ascii-parser-entry.cc',
  'src/ascii-parser-basetype.cc',
  'src/ascii-parser-basetype-inst.cc',
  'src/ascii-parser-timesamples.cc',
  'src/ascii-parser-timesamples-array.cc',
  'src/audio-loader.cc',
  'src/base122.cc',
  'src/usda-reader.cc',
  'src/usda-reader-reconstruct-1.cc',
  'src/usda-reader-reconstruct-2.cc',
  'src/usda-reader-reconstruct-3.cc',
  'src/usda-reader-reconstruct-4.cc',
  'src/usdc-reader.cc',
  'src/usdc-reader-property.cc',
  'src/usdc-reader-prim.cc',
  'src/usdc-reader-prim-parse.cc',
  'src/usdc-reader-prim-reconstruct-1.cc',
  'src/usdc-reader-prim-reconstruct-2.cc',
  'src/usdc-reader-prim-reconstruct-3.cc',
  'src/usdc-reader-prim-reconstruct-4.cc',
  'src/usdc-reader-reconstruct.cc',
  'src/usda-writer.cc',
  'src/usdc-writer.cc',
  'src/composition.cc',
  'src/composition-reconstruct.cc',
  'src/chunk-reader.cc',
  'src/crate-reader.cc',
  'src/crate-reader-arrays.cc',
  'src/crate-reader-values.cc',
  'src/crate-reader-paths.cc',
  'src/crate-reader-timesamples.cc',
  'src/crate-format.cc',
  'src/crate-writer.cc',
  'src/crate-writer-values.cc',
  'src/crate-writer-inline.cc',
  'src/stage-converter.cc',
  'src/sconv-geom.cc',
  'src/sconv-shader.cc',
  'src/sconv-light.cc',
  'src/sconv-skel.cc',
  'src/sconv-layer.cc',
  'src/crate-pprint.cc',
  'src/crate-dump.cc',
  'src/path-util.cc',
  'src/prim-reconstruct.cc',
  'src/prim-reconstruct2.cc',
  'src/prim-reconstruct3.cc',
  'src/prim-reconstruct-geom.cc',
  'src/prim-reconstruct-geom2.cc',
  'src/prim-reconstruct-geom3.cc',
  'src/prim-reconstruct-lightprim.cc',
  'src/prim-reconstruct-shader.cc',
  'src/prim-reconstruct-shader2.cc',
  'src/prim-reconstruct-shader3.cc',
  'src/prim-reconstruct-shader4.cc',
  'src/prim-composition.cc',
  'src/prim-types.cc',
  'src/prim-types-schema.cc',
  'src/core/prim-enums.cc',
  'src/enum-handlers.cc',
  'src/layer.cc',
  'src/primvar.cc',
  'src/str-util.cc',
  'src/usd-dump.cc',
  'src/value-pprint.cc',
  'src/value-pprint-dispatch.cc',
  'src/value-pprint-prim.cc',
  'src/value-types.cc',
  'src/value-types-typename.cc',
  'src/value-types-cast.cc',
  'src/value-types-matrix.cc',
  'src/value-types-lerp.cc',
  'src/color-space.cc',
  'src/tiny-format.cc',
  'src/tiny-string.cc',
  'src/io-util.cc',
  'src/image-loader.cc',
  'src/image-writer.cc',
  'src/image-util.cc',
  'src/linear-algebra.cc',
  'src/value-eval-util.cc',
  'src/usdGeom.cc',
  'src/usdGeom-accessors.cc',
  'src/usdGeom-primvar-inst-scalar-a.cc',
  'src/usdGeom-primvar-inst-scalar-b.cc',
  'src/usdGeom-primvar-inst-vec-a.cc',
  'src/usdGeom-primvar-inst-vec-b.cc',
  'src/usdGeom-primvar-inst-role-a.cc',
  'src/usdGeom-primvar-inst-role-b.cc',
  'src/usdSkel.cc',
  'src/usdShade.cc',
  'src/usdLux.cc',
  'src/mtlx-xml-tokenizer.cc',
  'src/mtlx-xml-parser.cc',
  'src/core/mtlx-dom.cc',
  'src/mtlx-simple-parser.cc',
  'src/usdMtlx.cc',
  'src/usdMtlx-write.cc',
  'src/usdObj.cc',
  'src/pprint-enum.cc',
  'src/pprint-meta.cc',
  'src/pprint-geom.cc',
  'src/pprint-shader.cc',
  'src/pprint-light.cc',
  'src/pprint-skel.cc',
  'src/pprinter.cc',
  'src/timesamples-pprint.cc',
  'src/timesamples.cc',
  'src/timesamples-eval.cc',
  'src/stage.cc',
  'src/uuid-gen.cc',
  'src/parser-timing.cc',
  'src/sha256.cc',
  'src/typed-array.cc',
  'src/task-queue.cc',
  'src/prim-pprint-parallel.cc',
)

# ============================================================================
# Dependency sources (always compiled)
# ============================================================================

lightusd_dep_sources = files(
  'src/integerCoding.cpp',
  'src/lz4-compression.cc',
  'src/lz4/lz4.c',
)

# ============================================================================
# Conditional sources
# ============================================================================

# -- Tydra --
if opt_tydra
  lightusd_sources += files(
    'src/tydra/facial.cc',
    'src/tydra/prim-apply.cc',
    'src/tydra/scene-access.cc',
    'src/tydra/scene-access-listprims-inst.cc',
    'src/tydra/scene-access-listshaders-inst.cc',
    'src/tydra/scene-analysis.cc',
    'src/tydra/attribute-eval.cc',
    'src/tydra/attribute-eval-typed-all.cc',
    'src/tydra/attribute-eval-typed-inst-scalar.cc',
    'src/tydra/attribute-eval-typed-inst-array.cc',
    'src/tydra/command-and-history.cc',
    'src/tydra/obj-export.cc',
    'src/tydra/usd-export.cc',
    'src/tydra/shader-network.cc',
    'src/tydra/render-data.cc',
    'src/tydra/render-data-mesh.cc',
    'src/tydra/render-data-mesh-tangent.cc',
    'src/tydra/render-data-material.cc',
    'src/tydra/render-data-material-mtlx.cc',
    'src/tydra/render-data-anim.cc',
    'src/tydra/render-data-pprint.cc',
    'src/tydra/raytracing-data.cc',
    'src/tydra/raytracing-scene-converter.cc',
    'src/tydra/material-serializer.cc',
    'src/tydra/materialx-to-json.cc',
    'src/tydra/render-scene-dump.cc',
    'src/tydra/bone-util.cc',
    'src/tydra/layer-to-renderscene.cc',
    'src/tydra/texture-util.cc',
    'src/tydra/variant-support.cc',
    'src/tydra/variant-converter.cc',
    'src/tydra/variant-applier.cc',
    'src/tydra/mcp.cc',
    'src/tydra/mcp-tools.cc',
    'src/tydra/mcp-resources.cc',
    'src/tydra/mcp-server.cc',
    'src/tydra/diff-and-compare.cc',
    'src/tydra/js-script.cc',
    'src/tydra/threejs-exporter.cc',
  )
  lightusd_dep_sources += files('src/external/mikktspace/mikktspace.c')

  if opt_wamr
    lightusd_sources += files('src/tydra/wasm-runtime.cc')
  endif
endif

# -- pxr compat API --
if opt_pxr_compat_api
  lightusd_sources += files('src/pxr-compat.cc')
endif

# -- usdMtlx (pugixml) --
if opt_usdmtlx
  lightusd_dep_sources += files('src/external/pugixml.cpp')
endif

# -- JSON --
if opt_json
  lightusd_sources += files(
    'src/json-to-usd.cc',
    'src/usd-to-json.cc',
    'src/json-writer.cc',
    'src/json-util.cc',
  )
  lightusd_dep_sources += files('src/external/yyjson.c')
endif

# -- usdFbx --
if opt_usdfbx
  lightusd_sources += files('src/usdFbx.cc')
  lightusd_dep_sources += files('src/external/OpenFBX/src/ofbx.cpp')
endif

# -- usdObj --
if opt_usdobj
  lightusd_dep_sources += files('src/external/tiny_obj_loader.cc')
endif

# -- usdVox --
if opt_usdvox
  lightusd_sources += files('src/usdVox.cc')
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
  lightusd_dep_sources += files('src/zstd-compression.cc')
elif opt_zstd and opt_system_zstd
  lightusd_dep_sources += files('src/zstd-compression.cc')
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
  'src/tsd/tsd-lightusd.cc',
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
