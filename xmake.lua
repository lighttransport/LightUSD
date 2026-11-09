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
option("tydra",                {default = true,  description = "Build Tydra module"})
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

    -- Include directories
    add_includedirs("src", {public = true})

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
        "src/crate-path-utils/path_sort.cc",
        "src/crate-path-utils/tree_encode.cc",
        "src/arg-parser.cc",
        "src/asset-resolution.cc",
        "src/lightusd.cc",
        "src/xform.cc",
        "src/performance.cc",
        "src/ascii-parser.cc",
        "src/ascii-parser-props.cc",
        "src/ascii-parser-entry.cc",
        "src/ascii-parser-basetype.cc",
        "src/ascii-parser-basetype-inst.cc",
        "src/ascii-parser-timesamples.cc",
        "src/ascii-parser-timesamples-array.cc",
        "src/audio-loader.cc",
        "src/base122.cc",
        "src/usda-reader.cc",
        "src/usda-reader-reconstruct-1.cc",
        "src/usda-reader-reconstruct-2.cc",
        "src/usda-reader-reconstruct-3.cc",
        "src/usda-reader-reconstruct-4.cc",
        "src/usdc-reader.cc",
        "src/usdc-reader-property.cc",
        "src/usdc-reader-prim.cc",
        "src/usdc-reader-prim-parse.cc",
        "src/usdc-reader-prim-reconstruct-1.cc",
        "src/usdc-reader-prim-reconstruct-2.cc",
        "src/usdc-reader-prim-reconstruct-3.cc",
        "src/usdc-reader-prim-reconstruct-4.cc",
        "src/usdc-reader-reconstruct.cc",
        "src/usda-writer.cc",
        "src/usdc-writer.cc",
        "src/composition.cc",
        "src/composition-reconstruct.cc",
        "src/chunk-reader.cc",
        "src/crate-reader.cc",
        "src/crate-reader-arrays.cc",
        "src/crate-reader-values.cc",
        "src/crate-reader-paths.cc",
        "src/crate-reader-timesamples.cc",
        "src/crate-format.cc",
        "src/crate-writer.cc",
        "src/crate-writer-values.cc",
        "src/crate-writer-inline.cc",
        "src/stage-converter.cc",
        "src/sconv-geom.cc",
        "src/sconv-shader.cc",
        "src/sconv-light.cc",
        "src/sconv-skel.cc",
        "src/sconv-layer.cc",
        "src/crate-pprint.cc",
        "src/crate-dump.cc",
        "src/path-util.cc",
        "src/prim-reconstruct.cc",
        "src/prim-reconstruct2.cc",
        "src/prim-reconstruct3.cc",
        "src/prim-reconstruct-geom.cc",
        "src/prim-reconstruct-geom2.cc",
        "src/prim-reconstruct-geom3.cc",
        "src/prim-reconstruct-lightprim.cc",
        "src/prim-reconstruct-shader.cc",
        "src/prim-reconstruct-shader2.cc",
        "src/prim-reconstruct-shader3.cc",
        "src/prim-reconstruct-shader4.cc",
        "src/prim-composition.cc",
        "src/prim-types.cc",
        "src/prim-types-schema.cc",
        "src/core/prim-enums.cc",
        "src/enum-handlers.cc",
        "src/layer.cc",
        "src/primvar.cc",
        "src/str-util.cc",
        "src/usd-dump.cc",
        "src/value-pprint.cc",
        "src/value-pprint-dispatch.cc",
        "src/value-pprint-prim.cc",
        "src/value-types.cc",
        "src/value-types-typename.cc",
        "src/value-types-cast.cc",
        "src/value-types-matrix.cc",
        "src/value-types-lerp.cc",
        "src/color-space.cc",
        "src/tiny-format.cc",
        "src/tiny-string.cc",
        "src/io-util.cc",
        "src/image-loader.cc",
        "src/image-writer.cc",
        "src/image-util.cc",
        "src/linear-algebra.cc",
        "src/value-eval-util.cc",
        "src/usdGeom.cc",
        "src/usdGeom-accessors.cc",
        "src/usdGeom-primvar-inst-scalar-a.cc",
        "src/usdGeom-primvar-inst-scalar-b.cc",
        "src/usdGeom-primvar-inst-vec-a.cc",
        "src/usdGeom-primvar-inst-vec-b.cc",
        "src/usdGeom-primvar-inst-role-a.cc",
        "src/usdGeom-primvar-inst-role-b.cc",
        "src/usdSkel.cc",
        "src/usdShade.cc",
        "src/usdLux.cc",
        "src/mtlx-xml-tokenizer.cc",
        "src/mtlx-xml-parser.cc",
        "src/core/mtlx-dom.cc",
        "src/mtlx-simple-parser.cc",
        "src/usdMtlx.cc",
        "src/usdMtlx-write.cc",
        "src/usdObj.cc",
        "src/pprint-enum.cc",
        "src/pprint-meta.cc",
        "src/pprint-geom.cc",
        "src/pprint-shader.cc",
        "src/pprint-light.cc",
        "src/pprint-skel.cc",
        "src/pprinter.cc",
        "src/timesamples-pprint.cc",
        "src/timesamples.cc",
        "src/timesamples-eval.cc",
        "src/stage.cc",
        "src/uuid-gen.cc",
        "src/parser-timing.cc",
        "src/sha256.cc",
        "src/typed-array.cc",
        "src/task-queue.cc",
        "src/prim-pprint-parallel.cc"
    )

    -- ========================================================================
    -- Dependency sources (always compiled)
    -- ========================================================================

    add_files(
        "src/integerCoding.cpp",
        "src/lz4-compression.cc",
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
            "src/tydra/facial.cc",
            "src/tydra/prim-apply.cc",
            "src/tydra/scene-access.cc",
            "src/tydra/scene-access-listprims-inst.cc",
            "src/tydra/scene-access-listshaders-inst.cc",
            "src/tydra/scene-analysis.cc",
            "src/tydra/attribute-eval.cc",
            "src/tydra/attribute-eval-typed-all.cc",
            "src/tydra/attribute-eval-typed-inst-scalar.cc",
            "src/tydra/attribute-eval-typed-inst-array.cc",
            "src/tydra/command-and-history.cc",
            "src/tydra/obj-export.cc",
            "src/tydra/usd-export.cc",
            "src/tydra/shader-network.cc",
            "src/tydra/render-data.cc",
            "src/tydra/render-data-mesh.cc",
            "src/tydra/render-data-mesh-tangent.cc",
            "src/tydra/render-data-material.cc",
            "src/tydra/render-data-material-mtlx.cc",
            "src/tydra/render-data-anim.cc",
            "src/tydra/render-data-pprint.cc",
            "src/tydra/raytracing-data.cc",
            "src/tydra/raytracing-scene-converter.cc",
            "src/tydra/material-serializer.cc",
            "src/tydra/materialx-to-json.cc",
            "src/tydra/render-scene-dump.cc",
            "src/tydra/bone-util.cc",
            "src/tydra/layer-to-renderscene.cc",
            "src/tydra/texture-util.cc",
            "src/tydra/variant-support.cc",
            "src/tydra/variant-converter.cc",
            "src/tydra/variant-applier.cc",
            "src/tydra/mcp.cc",
            "src/tydra/mcp-tools.cc",
            "src/tydra/mcp-resources.cc",
            "src/tydra/mcp-server.cc",
            "src/tydra/diff-and-compare.cc",
            "src/tydra/js-script.cc",
            "src/tydra/threejs-exporter.cc"
        )
        add_files("src/external/mikktspace/mikktspace.c")

        if has_config("wamr") then
            add_files("src/tydra/wasm-runtime.cc")
        end
    end

    -- pxr compat API
    if has_config("pxr_compat_api") then
        add_files("src/pxr-compat.cc")
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
            "src/json-to-usd.cc",
            "src/usd-to-json.cc",
            "src/json-writer.cc",
            "src/json-util.cc"
        )
        add_files("src/external/yyjson.c")
    end

    -- usdFbx
    if has_config("usdfbx") then
        add_files("src/usdFbx.cc")
        add_files("src/external/OpenFBX/src/ofbx.cpp")
    end

    -- usdObj
    if has_config("usdobj") then
        add_files("src/external/tiny_obj_loader.cc")
    end

    -- usdVox
    if has_config("usdvox") then
        add_files("src/usdVox.cc")
    end

    -- EXR (tinyexr v3)
    if has_config("exr") then
        if has_config("system_zlib") then
            add_files("src/external/tinyexr_c_impl.c", {defines = {"TINYEXR_V3_HAS_DEFLATE=1", "TINYEXR_V3_NO_MINIZ=1"}})
            add_files("src/external/tinyexr.cc", {defines = "TINYEXR_USE_MINIZ=0"})
        else
            add_files("src/external/tinyexr_c_impl.c")
            add_files("src/external/tinyexr.cc")
            add_includedirs("src/external")
        end
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
        add_files("src/zstd-compression.cc")
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
