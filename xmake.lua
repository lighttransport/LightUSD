-- LightUSD xmake build
-- Source list synchronized with CMakeLists.txt

set_xmakever("2.7.0")
set_project("lightusd")
set_version("1.0.0")

set_languages("c11", "c++17")

-- ============================================================================
-- Options
-- ============================================================================

-- Core features (default ON)
option("tydra",                {default = false, description = "Build legacy Tydra"})
option("builtin_image_loader", {default = true,  description = "Build with built-in image loader (stb_image and fpng)"})
option("usdmtlx",             {default = true,  description = "Build with MaterialX support"})
option("json",                 {default = true,  description = "Build with JSON support"})
option("usdobj",               {default = true,  description = "Build with usdObj support (import Wavefront .obj)"})
option("usdvox",               {default = true,  description = "Build with usdVox support (import MagicaVoxel .vox)"})
option("exr",                  {default = true,  description = "Build with EXR HDR texture support"})
option("colorio",              {default = true,  description = "Build with Color IO baked LUT support"})
option("audio",                {default = true,  description = "Build with Audio support (MP3 and WAV)"})
option("zstd",                 {default = true,  description = "Enable zstd compression for USD files"})
option("pxr_compat_api",       {default = true,  description = "Build with pxr compatible API"})

-- Module control (default ON)
option("module_usda_reader",   {default = true,  description = "Build with USDA reader feature"})
option("module_usda_writer",   {default = true,  description = "Build with USDA writer feature"})
option("module_usdc_reader",   {default = true,  description = "Build with USDC reader feature"})
option("module_usdc_writer",   {default = true,  description = "Build with USDC writer feature"})

-- Optional features (default OFF)
option("usdfbx",               {default = false, description = "Build with usdFbx support (import FBX)"})
option("opensubdiv",           {default = false, description = "Build with OpenSubdiv support"})
option("meshopt",              {default = false, description = "Build with meshoptimizer support"})
option("alac_audio",           {default = false, description = "Build with ALAC (M4A) audio support"})
option("mcp_server",           {default = false, description = "Build with C++ MCP server (HTTP) support"})
option("qjs",                  {default = false, description = "Build with QuickJS (JavaScript) support"})
option("wamr",                 {default = false, description = "Build with WAMR (WebAssembly Micro Runtime) support"})
option("geogram",              {default = false, description = "Build with Geogram library"})
option("remotery",             {default = false, description = "Build with Remotery profiling support"})
option("production_build",     {default = false, description = "Build for production"})
option("debug_print",          {default = false, description = "Enable debug print"})
option("shared_lib",           {default = false, description = "Also build shared library"})
option("system_zlib",          {default = false, description = "Use system zlib instead of bundled miniz"})
option("system_zstd",          {default = false, description = "Use system zstd instead of bundled version"})

-- ============================================================================
-- (crate path utilities — path sorting / tree encoding for the USDC writer — are
--  now compiled directly into the main library from src/crate-path-utils/; the
--  former sandbox/path-sort-and-encode-crate "crate-encoding" target is deprecated.)
-- ============================================================================

-- ============================================================================
-- Main library
-- ============================================================================

target("lightusd_static")
    set_kind("static")
    set_languages("c11", "c++17")

    -- Include directories: src/legacy first (mirrors legacy.cmake's
    -- include_directories(BEFORE)) so legacy `#include "tydra/render-data.hh"`
    -- resolves to the legacy converter header, not the core Tydra one.
    add_includedirs("src/legacy")
    add_includedirs("src", {public = true})

    -- Force-include the legacy namespace bridge into every legacy C++ TU
    -- (mirrors src/legacy/legacy.cmake / the root CMake build).
    if is_plat("windows") then
        add_cxxflags("/FI$(projectdir)/src/legacy/legacy-namespace.hh", {force = true})
    else
        add_cxxflags("-include $(projectdir)/src/legacy/legacy-namespace.hh", {force = true})
    end

    -- PIC
    if not is_plat("windows") then
        add_cxxflags("-fPIC")
        add_cflags("-fPIC")
    end

    -- System libs
    if is_plat("linux") then
        add_syslinks("dl")
    end

    -- ========================================================================
    -- Core sources (always compiled)
    -- ========================================================================

    add_files(
        -- Crate path utilities (path sorting / tree encoding for the USDC writer;
        -- formerly the sandbox/path-sort-and-encode-crate target).
        "src/legacy/crate-path-utils/path_sort.cc",
        "src/legacy/crate-path-utils/tree_encode.cc",
        "src/legacy/arg-parser.cc",
        "src/legacy/asset-resolution.cc",
        "src/legacy/lightusd.cc",
        "src/legacy/xform.cc",
        "src/legacy/performance.cc",
        "src/legacy/ascii-parser.cc",
        "src/legacy/ascii-parser-props.cc",
        "src/legacy/ascii-parser-entry.cc",
        "src/legacy/ascii-parser-basetype.cc",
        "src/legacy/ascii-parser-basetype-inst.cc",
        "src/legacy/ascii-parser-timesamples.cc",
        "src/legacy/ascii-parser-timesamples-array.cc",
        "src/legacy/audio-loader.cc",
        "src/legacy/base122.cc",
        "src/legacy/usda-reader.cc",
        "src/legacy/usda-reader-reconstruct-1.cc",
        "src/legacy/usda-reader-reconstruct-2.cc",
        "src/legacy/usda-reader-reconstruct-3.cc",
        "src/legacy/usda-reader-reconstruct-4.cc",
        "src/legacy/usdc-reader.cc",
        "src/legacy/usdc-reader-property.cc",
        "src/legacy/usdc-reader-prim.cc",
        "src/legacy/usdc-reader-prim-parse.cc",
        "src/legacy/usdc-reader-prim-reconstruct-1.cc",
        "src/legacy/usdc-reader-prim-reconstruct-2.cc",
        "src/legacy/usdc-reader-prim-reconstruct-3.cc",
        "src/legacy/usdc-reader-prim-reconstruct-4.cc",
        "src/legacy/usdc-reader-reconstruct.cc",
        "src/legacy/usda-writer.cc",
        "src/legacy/usdc-writer.cc",
        "src/legacy/composition.cc",
        "src/legacy/composition-reconstruct.cc",
        "src/legacy/chunk-reader.cc",
        "src/legacy/crate-reader.cc",
        "src/legacy/crate-reader-arrays.cc",
        "src/legacy/crate-reader-values.cc",
        "src/legacy/crate-reader-paths.cc",
        "src/legacy/crate-reader-timesamples.cc",
        "src/legacy/crate-format.cc",
        "src/legacy/crate-writer.cc",
        "src/legacy/crate-writer-values.cc",
        "src/legacy/crate-writer-inline.cc",
        "src/legacy/stage-converter.cc",
        "src/legacy/sconv-geom.cc",
        "src/legacy/sconv-shader.cc",
        "src/legacy/sconv-light.cc",
        "src/legacy/sconv-skel.cc",
        "src/legacy/sconv-layer.cc",
        "src/legacy/crate-pprint.cc",
        "src/legacy/crate-dump.cc",
        "src/legacy/path-util.cc",
        "src/legacy/prim-reconstruct.cc",
        "src/legacy/prim-reconstruct2.cc",
        "src/legacy/prim-reconstruct3.cc",
        "src/legacy/prim-reconstruct-geom.cc",
        "src/legacy/prim-reconstruct-geom2.cc",
        "src/legacy/prim-reconstruct-geom3.cc",
        "src/legacy/prim-reconstruct-lightprim.cc",
        "src/legacy/prim-reconstruct-shader.cc",
        "src/legacy/prim-reconstruct-shader2.cc",
        "src/legacy/prim-reconstruct-shader3.cc",
        "src/legacy/prim-reconstruct-shader4.cc",
        "src/legacy/prim-composition.cc",
        "src/legacy/prim-types.cc",
        "src/legacy/prim-types-schema.cc",
        "src/legacy/core/prim-enums.cc",
        "src/legacy/enum-handlers.cc",
        "src/legacy/layer.cc",
        "src/legacy/primvar.cc",
        "src/legacy/str-util.cc",
        "src/legacy/usd-dump.cc",
        "src/legacy/value-pprint.cc",
        "src/legacy/value-pprint-dispatch.cc",
        "src/legacy/value-pprint-prim.cc",
        "src/legacy/value-types.cc",
        "src/legacy/value-types-typename.cc",
        "src/legacy/value-types-cast.cc",
        "src/legacy/value-types-matrix.cc",
        "src/legacy/value-types-lerp.cc",
        "src/legacy/color-space.cc",
        "src/legacy/tiny-format.cc",
        "src/legacy/tiny-string.cc",
        "src/io-util.cc",
        "src/image-loader.cc",
        "src/image-writer.cc",
        "src/tydra/texture-util.cc",
        "src/legacy/image-util.cc",
        "src/legacy/linear-algebra.cc",
        "src/legacy/value-eval-util.cc",
        "src/legacy/usdGeom.cc",
        "src/legacy/usdGeom-accessors.cc",
        "src/legacy/usdGeom-primvar-inst-scalar-a.cc",
        "src/legacy/usdGeom-primvar-inst-scalar-b.cc",
        "src/legacy/usdGeom-primvar-inst-vec-a.cc",
        "src/legacy/usdGeom-primvar-inst-vec-b.cc",
        "src/legacy/usdGeom-primvar-inst-role-a.cc",
        "src/legacy/usdGeom-primvar-inst-role-b.cc",
        "src/legacy/usdSkel.cc",
        "src/legacy/usdShade.cc",
        "src/legacy/usdLux.cc",
        "src/mtlx-xml-tokenizer.cc",
        "src/mtlx-xml-parser.cc",
        "src/core/mtlx-dom.cc",
        "src/legacy/mtlx-simple-parser.cc",
        "src/legacy/usdMtlx.cc",
        "src/legacy/usdMtlx-write.cc",
        "src/legacy/usdObj.cc",
        "src/legacy/pprint-enum.cc",
        "src/legacy/pprint-meta.cc",
        "src/legacy/pprint-geom.cc",
        "src/legacy/pprint-shader.cc",
        "src/legacy/pprint-light.cc",
        "src/legacy/pprint-skel.cc",
        "src/legacy/pprinter.cc",
        "src/legacy/timesamples-pprint.cc",
        "src/legacy/timesamples.cc",
        "src/legacy/timesamples-eval.cc",
        "src/legacy/stage.cc",
        "src/legacy/uuid-gen.cc",
        "src/legacy/parser-timing.cc",
        "src/sha256.cc",
        "src/legacy/typed-array.cc",
        "src/legacy/task-queue.cc",
        "src/legacy/prim-pprint-parallel.cc"
    )

    -- ========================================================================
    -- Dependency sources (always compiled)
    -- ========================================================================

    add_files(
        "src/legacy/integerCoding.cpp",
        "src/legacy/lz4-compression.cc",
        "src/lz4/lz4.c"
    )

    -- ========================================================================
    -- Compile definitions
    -- ========================================================================

    if has_config("json") then
        add_defines("LIGHTUSD_WITH_JSON")
    end
    if has_config("usdmtlx") then
        add_defines("LIGHTUSD_USE_USDMTLX")
    end
    if has_config("usdobj") then
        add_defines("LIGHTUSD_USE_USDOBJ")
    end
    if has_config("usdvox") then
        add_defines("LIGHTUSD_USE_USDVOX")
    end
    if has_config("usdfbx") then
        add_defines("LIGHTUSD_USE_USDFBX")
    end
    if has_config("exr") then
        add_defines("LIGHTUSD_WITH_EXR")
        add_defines("LIGHTUSD_EXR_V3=1")
    end
    if has_config("colorio") then
        add_defines("LIGHTUSD_WITH_COLORIO")
    end
    if has_config("audio") then
        add_defines("LIGHTUSD_WITH_AUDIO")
    end
    if has_config("alac_audio") then
        add_defines("LIGHTUSD_WITH_ALAC_AUDIO")
    end
    if has_config("tydra") then
        add_defines("LIGHTUSD_WITH_TYDRA")
    end
    if has_config("zstd") then
        add_defines("LIGHTUSD_WITH_ZSTD_COMPRESSION")
    end
    if has_config("opensubdiv") then
        add_defines("LIGHTUSD_WITH_OPENSUBDIV")
    end
    if has_config("meshopt") then
        add_defines("LIGHTUSD_WITH_MESHOPT")
    end
    if has_config("geogram") then
        add_defines("LIGHTUSD_WITH_GEOGRAM", "GEO_STATIC_LIBS", "GEOGRAM_WITH_LEGACY_NUMERICS")
    end
    if has_config("wamr") then
        add_defines("LIGHTUSD_WITH_WAMR")
        if is_plat("windows") then
            add_defines("BH_PLATFORM_WINDOWS=1")
        else
            add_defines("BH_PLATFORM_LINUX=1")
        end
    end
    if has_config("mcp_server") then
        add_defines("LIGHTUSD_WITH_MCP_SERVER", "OPENSSL_API_3_0")
    end
    if has_config("qjs") then
        add_defines("LIGHTUSD_WITH_QJS")
    end
    if has_config("remotery") then
        add_defines("RMT_ENABLED=1")
        if is_plat("windows") then
            add_defines("RMT_USE_D3D11=0")
        elseif is_plat("macosx") then
            add_defines("RMT_USE_METAL=0")
        elseif is_plat("linux") then
            add_defines("RMT_USE_OPENGL=0")
        end
    end
    if has_config("production_build") then
        add_defines("LIGHTUSD_PRODUCTION_BUILD")
    end
    if has_config("debug_print") then
        add_defines("LIGHTUSD_DEBUG_PRINT")
    end
    if not has_config("builtin_image_loader") then
        add_defines("LIGHTUSD_NO_BUILTIN_IMAGE_LOADER")
    end

    -- Inverted module flags
    if not has_config("module_usda_reader") then
        add_defines("LIGHTUSD_DISABLE_MODULE_USDA_READER")
    end
    if not has_config("module_usda_writer") then
        add_defines("LIGHTUSD_DISABLE_MODULE_USDA_WRITER")
    end
    if not has_config("module_usdc_reader") then
        add_defines("LIGHTUSD_DISABLE_MODULE_USDC_READER")
    end
    if not has_config("module_usdc_writer") then
        add_defines("LIGHTUSD_DISABLE_MODULE_USDC_WRITER")
    end

    -- ========================================================================
    -- Conditional sources
    -- ========================================================================

    -- Tydra
    if has_config("tydra") then
        add_files(
            "src/legacy/tydra/facial.cc",
            "src/legacy/tydra/prim-apply.cc",
            "src/legacy/tydra/scene-access.cc",
            "src/legacy/tydra/scene-access-listprims-inst.cc",
            "src/legacy/tydra/scene-access-listshaders-inst.cc",
            "src/legacy/tydra/scene-analysis.cc",
            "src/legacy/tydra/attribute-eval.cc",
            "src/legacy/tydra/attribute-eval-typed-all.cc",
            "src/legacy/tydra/attribute-eval-typed-inst-scalar.cc",
            "src/legacy/tydra/attribute-eval-typed-inst-array.cc",
            "src/legacy/tydra/command-and-history.cc",
            "src/legacy/tydra/obj-export.cc",
            "src/legacy/tydra/usd-export.cc",
            "src/legacy/tydra/shader-network.cc",
            "src/legacy/tydra/render-data.cc",
            "src/legacy/tydra/render-data-mesh.cc",
            "src/legacy/tydra/render-data-mesh-tangent.cc",
            "src/legacy/tydra/render-data-material.cc",
            "src/legacy/tydra/render-data-material-mtlx.cc",
            "src/legacy/tydra/render-data-anim.cc",
            "src/legacy/tydra/render-data-pprint.cc",
            "src/legacy/tydra/raytracing-data.cc",
            "src/legacy/tydra/raytracing-scene-converter.cc",
            "src/legacy/tydra/material-serializer.cc",
            "src/legacy/tydra/materialx-to-json.cc",
            "src/legacy/tydra/render-scene-dump.cc",
            "src/legacy/tydra/bone-util.cc",
            "src/legacy/tydra/layer-to-renderscene.cc",
            "src/legacy/tydra/variant-support.cc",
            "src/legacy/tydra/variant-converter.cc",
            "src/legacy/tydra/variant-applier.cc",
            "src/legacy/tydra/mcp.cc",
            "src/legacy/tydra/mcp-tools.cc",
            "src/legacy/tydra/mcp-resources.cc",
            "src/legacy/tydra/mcp-server.cc",
            "src/legacy/tydra/diff-and-compare.cc",
            "src/legacy/tydra/js-script.cc",
            "src/legacy/tydra/threejs-exporter.cc"
        )
        add_files("src/external/mikktspace/mikktspace.c")

        if has_config("wamr") then
            add_files("src/legacy/tydra/wasm-runtime.cc")
        end
    end

    -- pxr compat API
    if has_config("pxr_compat_api") then
        add_files("src/legacy/pxr-compat.cc")
    end

    -- Built-in image loader (fpng with per-file flags)
    if has_config("builtin_image_loader") then
        add_files("src/external/fpng.cpp", {defines = "FPNG_NO_SSE=1"})
    end

    -- usdMtlx (pugixml)
    if has_config("usdmtlx") then
        add_files("src/external/pugixml.cpp")
    end

    -- JSON
    if has_config("json") then
        add_files(
            "src/legacy/json-to-usd.cc",
            "src/legacy/usd-to-json.cc",
            "src/legacy/json-writer.cc",
            "src/legacy/json-util.cc"
        )
        add_files("src/external/yyjson.c")
    end

    -- usdFbx
    if has_config("usdfbx") then
        add_files("src/legacy/usdFbx.cc")
        add_files("src/external/OpenFBX/src/ofbx.cpp")
    end

    -- usdObj
    if has_config("usdobj") then
        add_files("src/external/tiny_obj_loader.cc")
    end

    -- usdVox
    if has_config("usdvox") then
        add_files("src/legacy/usdVox.cc")
    end

    -- EXR (TinyEXR v3 C API, pure-C11; HTJ2K/ZSTD compiled out)
    -- Keep this list in sync with src/external/tinyexr/src/ and
    -- cmake/lightusd-native-exr.cmake. Selects the v3 backend via
    -- LIGHTUSD_EXR_V3 (image-loader.cc / image-writer.cc).
    if has_config("exr") then
        add_files("src/external/tinyexr/src/exr_attr.c",
                  "src/external/tinyexr/src/exr_b44.c",
                  "src/external/tinyexr/src/exr_codec.c",
                  "src/external/tinyexr/src/exr_color.c",
                  "src/external/tinyexr/src/exr_context.c",
                  "src/external/tinyexr/src/exr_convert.c",
                  "src/external/tinyexr/src/exr_core.c",
                  "src/external/tinyexr/src/exr_cpu.c",
                  "src/external/tinyexr/src/exr_deep.c",
                  "src/external/tinyexr/src/exr_deflate.c",
                  "src/external/tinyexr/src/exr_fpnge.c",
                  "src/external/tinyexr/src/exr_freestanding.c",
                  "src/external/tinyexr/src/exr_half.c",
                  "src/external/tinyexr/src/exr_mip.c",
                  "src/external/tinyexr/src/exr_piz.c",
                  "src/external/tinyexr/src/exr_pxr24.c",
                  "src/external/tinyexr/src/exr_reader.c",
                  "src/external/tinyexr/src/exr_resize.c",
                  "src/external/tinyexr/src/exr_rle.c",
                  "src/external/tinyexr/src/exr_simd_neon.c",
                  "src/external/tinyexr/src/exr_simd_x86.c",
                  "src/external/tinyexr/src/exr_source.c",
                  "src/external/tinyexr/src/exr_source_part.c",
                  "src/external/tinyexr/src/exr_spectral.c",
                  "src/external/tinyexr/src/exr_stdio.c",
                  "src/external/tinyexr/src/exr_thread.c",
                  "src/external/tinyexr/src/exr_tonemap.c",
                  "src/external/tinyexr/src/exr_util_simd_neon.c",
                  "src/external/tinyexr/src/exr_util_simd_x86.c",
                  "src/external/tinyexr/src/exr_writer.c",
                  "src/external/tinyexr/src/exr_zip.c",
                  {defines = {"EXR_NO_JPH", "EXR_NO_ZSTD"}, force = {cflags = "-w"}})
        add_files("src/imageio/exr-codec.c")
        add_includedirs("src/external/tinyexr/include", "src/external/tinyexr/src")
    end

    -- miniz (for EXR/TIFF when not using system zlib)
    if has_config("exr") and not has_config("system_zlib") then
        add_files("src/external/miniz.c", {defines = "_LARGEFILE64_SOURCE=1"})
    end

    -- Zstd compression
    if has_config("zstd") then
        if not has_config("system_zstd") then
            add_files("src/external/zstd.c", {defines = "ZSTD_DISABLE_ASM=1", force = {cflags = "-w"}})
        else
            add_packages("zstd")
        end
        add_files("src/legacy/zstd-compression.cc")
    end

    -- ALAC audio
    if has_config("alac_audio") then
        add_files(
            "src/external/alac/codec/EndianPortable.c",
            "src/external/alac/codec/ALACBitUtilities.c",
            "src/external/alac/codec/ALACDecoder.cpp",
            "src/external/alac/codec/ALACEncoder.cpp",
            "src/external/alac/codec/ag_dec.c",
            "src/external/alac/codec/ag_enc.c",
            "src/external/alac/codec/dp_dec.c",
            "src/external/alac/codec/dp_enc.c",
            "src/external/alac/codec/matrix_dec.c",
            "src/external/alac/codec/matrix_enc.c"
        )
    end

    -- MCP server
    if has_config("mcp_server") then
        add_files("src/external/civetweb/civetweb.c")
    end

    -- QuickJS
    if has_config("qjs") then
        add_files(
            "src/external/quickjs-ng/cutils.c",
            "src/external/quickjs-ng/libregexp.c",
            "src/external/quickjs-ng/libunicode.c",
            "src/external/quickjs-ng/quickjs.c",
            "src/external/quickjs-ng/xsum.c"
        )
    end

    -- meshoptimizer
    if has_config("meshopt") then
        add_files(
            "src/external/meshoptimizer/allocator.cpp",
            "src/external/meshoptimizer/clusterizer.cpp",
            "src/external/meshoptimizer/indexanalyzer.cpp",
            "src/external/meshoptimizer/indexcodec.cpp",
            "src/external/meshoptimizer/indexgenerator.cpp",
            "src/external/meshoptimizer/overdrawoptimizer.cpp",
            "src/external/meshoptimizer/partition.cpp",
            "src/external/meshoptimizer/quantization.cpp",
            "src/external/meshoptimizer/rasterizer.cpp",
            "src/external/meshoptimizer/simplifier.cpp",
            "src/external/meshoptimizer/spatialorder.cpp",
            "src/external/meshoptimizer/stripifier.cpp",
            "src/external/meshoptimizer/vcacheoptimizer.cpp",
            "src/external/meshoptimizer/vertexcodec.cpp",
            "src/external/meshoptimizer/vertexfilter.cpp",
            "src/external/meshoptimizer/vfetchoptimizer.cpp"
        )
    end

    -- Remotery
    if has_config("remotery") then
        add_files("src/external/Remotery/Remotery.c")
        add_includedirs("src/external/Remotery")
    end

    -- Geogram
    if has_config("geogram") then
        add_files("src/external/geogram/geogram/third_party/triangle/triangle.c")
        add_includedirs("src/external/geogram")
    end

    -- OpenSubdiv
    if has_config("opensubdiv") then
        add_files("src/subdiv.cc")
        add_includedirs("src/osd")
        add_files(
            "src/osd/opensubdiv/far/bilinearPatchBuilder.cpp",
            "src/osd/opensubdiv/far/catmarkPatchBuilder.cpp",
            "src/osd/opensubdiv/far/error.cpp",
            "src/osd/opensubdiv/far/loopPatchBuilder.cpp",
            "src/osd/opensubdiv/far/patchBasis.cpp",
            "src/osd/opensubdiv/far/patchBuilder.cpp",
            "src/osd/opensubdiv/far/patchDescriptor.cpp",
            "src/osd/opensubdiv/far/patchMap.cpp",
            "src/osd/opensubdiv/far/patchTable.cpp",
            "src/osd/opensubdiv/far/patchTableFactory.cpp",
            "src/osd/opensubdiv/far/ptexIndices.cpp",
            "src/osd/opensubdiv/far/stencilTable.cpp",
            "src/osd/opensubdiv/far/stencilTableFactory.cpp",
            "src/osd/opensubdiv/far/stencilBuilder.cpp",
            "src/osd/opensubdiv/far/topologyDescriptor.cpp",
            "src/osd/opensubdiv/far/topologyRefiner.cpp",
            "src/osd/opensubdiv/far/topologyRefinerFactory.cpp",
            "src/osd/opensubdiv/osd/cpuEvaluator.cpp",
            "src/osd/opensubdiv/osd/cpuKernel.cpp",
            "src/osd/opensubdiv/osd/cpuPatchTable.cpp",
            "src/osd/opensubdiv/osd/cpuVertexBuffer.cpp",
            "src/osd/opensubdiv/sdc/typeTraits.cpp",
            "src/osd/opensubdiv/sdc/crease.cpp",
            "src/osd/opensubdiv/vtr/fvarLevel.cpp",
            "src/osd/opensubdiv/vtr/fvarRefinement.cpp",
            "src/osd/opensubdiv/vtr/level.cpp",
            "src/osd/opensubdiv/vtr/quadRefinement.cpp",
            "src/osd/opensubdiv/vtr/refinement.cpp",
            "src/osd/opensubdiv/vtr/sparseSelector.cpp",
            "src/osd/opensubdiv/vtr/triRefinement.cpp"
        )
    end

    -- WAMR
    if has_config("wamr") then
        add_includedirs(
            "src/external/wamr/core/iwasm/include",
            "src/external/wamr/core/iwasm/interpreter",
            "src/external/wamr/core/iwasm/common",
            "src/external/wamr/core/shared/utils",
            "src/external/wamr/core/shared/mem-alloc",
            "src/external/wamr/core/shared/platform/include"
        )
        add_files(
            "src/external/wamr/core/iwasm/common/wasm_runtime_common.c",
            "src/external/wamr/core/iwasm/common/wasm_native.c",
            "src/external/wamr/core/iwasm/common/wasm_exec_env.c",
            "src/external/wamr/core/iwasm/common/wasm_memory.c",
            "src/external/wamr/core/iwasm/common/wasm_c_api.c",
            "src/external/wamr/core/iwasm/interpreter/wasm_loader.c",
            "src/external/wamr/core/iwasm/interpreter/wasm_runtime.c",
            "src/external/wamr/core/iwasm/interpreter/wasm_interp_classic.c",
            "src/external/wamr/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c",
            "src/external/wamr/core/shared/mem-alloc/mem_alloc.c",
            "src/external/wamr/core/shared/mem-alloc/ems/ems_alloc.c",
            "src/external/wamr/core/shared/mem-alloc/ems/ems_hmu.c",
            "src/external/wamr/core/shared/mem-alloc/ems/ems_kfc.c",
            "src/external/wamr/core/shared/utils/bh_assert.c",
            "src/external/wamr/core/shared/utils/bh_common.c",
            "src/external/wamr/core/shared/utils/bh_hashmap.c",
            "src/external/wamr/core/shared/utils/bh_list.c",
            "src/external/wamr/core/shared/utils/bh_log.c",
            "src/external/wamr/core/shared/utils/bh_queue.c",
            "src/external/wamr/core/shared/utils/bh_vector.c",
            "src/external/wamr/core/shared/utils/runtime_timer.c"
        )
        if is_plat("windows") then
            add_includedirs("src/external/wamr/core/shared/platform/windows")
            add_files("src/external/wamr/core/shared/platform/windows/platform_init.c")
        else
            add_includedirs("src/external/wamr/core/shared/platform/linux")
            add_files("src/external/wamr/core/shared/platform/linux/platform_init.c")
        end
    end

target_end()

-- ============================================================================
-- Optional shared library
-- ============================================================================

if has_config("shared_lib") then
    target("lightusd")
        set_kind("shared")
        set_languages("c11", "c++17")
        add_defines("LIGHTUSD_COMPILE_LIBRARY", "LIGHTUSD_SHARED_LIBRARY")
        -- Clone all settings from static target (xmake doesn't support target cloning,
        -- so for shared lib, users should use CMake or Meson which handle this better)
        -- This is a simplified version; for full shared lib support use CMake.
        add_includedirs("src", {public = true})
        if is_plat("linux") then
            add_syslinks("dl")
        end
    target_end()
end
