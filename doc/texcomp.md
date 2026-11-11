# GPU Texture Compression (KTX2 / BC / ASTC / ETC2)

lightusd can load, decode, and produce GPU block-compressed textures to cut VRAM
and bandwidth in the viewers (lusdview / lusdrender) and on the web — while
staying **DCC-transparent** and **legacy-compatible**: a USD/USDZ asset that uses
compression still opens in stock USD tools and Apple Quick Look.

The compression codecs come from the vendored, pure-C11 **tinyexr texture tool
stack** at `src/external/textools/` (built as `lightusd_textools` when
`LIGHTUSD_WITH_TEXTOOLS=ON`, the default):

- **texcomp** — block encoders **and decoders** for BC1/3/5/6H/7, ETC2/EAC and
  ASTC LDR+HDR, plus the `uni` transcodable intermediate.
- **texpipe** — mip-chain + DDS/KTX2 container writer **and reader**
  (`tp_ktx2_read` / `tp_ktx2_decode_level_rgba8` / `tp_ktx2_write_uni`).
- **tir** — content-aware image resize. **envmap** — IBL/prefiltered cubemaps.

See `src/external/textools/README.lightusd.md` for the vendor provenance and the
re-sync procedure (fix upstream in the tinyexr repo, then re-copy — keep the
vendored tree pristine).

## Background: why transcoding

No single GPU compressed format is available everywhere:

| Platform / API | BC1–7 (S3TC/RGTC/BPTC) | ETC2/EAC | ASTC |
|---|---|---|---|
| Desktop (Win/Linux, NVIDIA/AMD/Intel) | ✅ | rarely HW | generally none |
| macOS (pre-Apple9) | ✅ | ❌ | ❌ |
| iOS / Apple Silicon Apple9+ | ✅ (Apple9+) | ✅ | ✅ |
| Android (GLES3) | device-dependent | ✅ guaranteed | common |
| WebGL2 / WebGPU | s3tc/bptc common on desktop | common on mobile | common on mobile |

Only **BC6H** and **ASTC-HDR** carry HDR; everything else is LDR. Because formats
don't overlap, the portable strategy is: ship one *transcodable* asset and
convert it to the device's native format at load. lightusd uses its own
private **Basis-free `uni`** intermediate. Its blocks are valid ASTC 4x4 and
convert to BC7 / ASTC / ETC2 or RGBA8, but they are not the Basis UASTC wire
representation.

## USD authoring: the legacy-safe KTX2 hint

USD has **no standard** for compressed textures, and **USDZ forbids KTX2** (only
png/jpg/exr/avif are permitted). So lightusd keeps `inputs:file` pointing at a
conventional image and names the compressed companion in **attribute
`customData`**:

```usda
def Shader "diffuseTex" {
    uniform token info:id = "UsdUVTexture"
    asset inputs:file = @textures/diffuse.png@ (
        customData = { asset ktx2 = @textures/diffuse.ktx2@ }
    )
    float3 outputs:rgb
}
```

- lightusd-aware loaders **search the `.ktx2` first**; if it resolves, they load
  and transcode/decode it. Otherwise they fall back to `inputs:file`.
- Stock USD tools, Quick Look, and RealityKit ignore `customData` and see only
  the png — so the asset is fully valid and USDZ-legal. `usdz-convert` does not
  package `customData`-referenced assets, so a hinted stage is already
  spec-clean; the hint is inert to unaware consumers.
- Resolution happens in `RenderSceneConverter::ConvertUVTexture`
  (`src/tydra/render-data-material.cc`), gated by `LIGHTUSD_WITH_TEXTOOLS`.
  Non-UDIM textures only.

A **direct** reference (`inputs:file = @tex.ktx2@`) also works: the core image
loader decodes it, and `usdz-convert` transcodes it to png/jpg when a
legacy/ARKit target is requested (`IsAllowedTextureExt` allows `ktx2` as an
*input*; `IsAllowedARKitTextureExt` still excludes it from USDZ output).

### Authoring the companions: `usd-texcomp`

`examples/usd-texcomp` is a core example built with
`LIGHTUSD_BUILD_EXAMPLES=ON`, Tydra and textools enabled; it does not require
`LIGHTUSD_BUILD_LEGACY`. Both USDA and USDC input are supported. For every `UsdUVTexture.inputs:file`
image it writes a `.ktx2` (private `uni`, full mip chain) next to the output and adds
the `customData ktx2` hint to the attribute — leaving `inputs:file` untouched:

```sh
usd-texcomp scene.usda -o scene_ktx2.usda [--mips on|off] [--zstd on|off]
#   diffuse.png -> diffuse.ktx2 (1024x1024, 11 level(s), ...)
# then:
lusdview scene_ktx2.usda --texture-keep-compressed on
```

The output opens unchanged in stock USD tools (they see only the png and ignore
`customData`), while lightusd-aware consumers pick up the compressed companion.

**HDR sources** (`.exr` / `.hdr`) are routed to **BC6H** instead of `uni` (which is
LDR-only): they are encoded as a mipped BC6H `.ktx2` that uploads as-is wherever
BPTC is supported, and decodes to float (or tone-maps) elsewhere. A 64x64 HDR test
texture: 64 KiB RGBA32F -> 5 KiB BC6H.

The `.ktx2` is **Zstd-supercompressed** by default (`supercompressionScheme = 2`,
the form real KTX2/UASTC assets ship in) — typically an order of magnitude smaller
on disk than the raw block payload (a 64x64 test texture: 5792 -> 501 bytes).
`--zstd off` writes the uncompressed (scheme 0) form. Zstd needs
`LIGHTUSD_WITH_ZSTD_COMPRESSION` (on by default).

## Core / tydra: loading a KTX2

`.ktx2` is a first-class loadable image. `lightusd::image::LoadImageFromMemory`
(`src/image-loader.cc`) detects the KTX2 identifier and decodes level 0 to
RGBA8 via the texpipe reader:

- **Every** block format decodes: the LDR codecs (uni, BC1/3/5/7, ETC2/EAC,
  ASTC LDR) to RGBA8, and the HDR ones (**BC6H**, **ASTC-HDR**) to **float RGBA**
  (`bpp = 32`, `PixelFormat::Float`) — HDR has no meaningful RGBA8 form, so an HDR
  `.ktx2` loads as a float image just like an EXR.
- The reader validates the level index against the file size and each level's
  declared byte length against its block geometry (rejects crafted/truncated
  files).
- `supercompressionScheme = 0` (uncompressed) and `2` (Zstd) are supported —
  real-world KTX2 (esp. UASTC) is usually Zstd-supercompressed. Zstd support
  requires `LIGHTUSD_WITH_ZSTD_COMPRESSION` (on by default); the reader
  decompresses each level via the vendored ZSTD. BasisLZ (scheme 1) is not
  handled.

Because the loader decodes to a normal RGBA8 `Image`, every existing consumer
works unchanged: **lusdrender** (a software path tracer) consumes `.ktx2` for
free, and `usdz-convert` can re-encode it to png/jpg/exr for a legacy asset.

`tydra::TextureImage` (`src/tydra/render-data.hh`) also carries a
`TextureBlockFormat blockFormat` (+ `blockWidth`/`blockHeight`), which the
keep-compressed passthrough (below) populates instead of decoding.

## lusdview: compress-on-load

lusdview converts decoded textures to a GPU block format at load to save VRAM,
selectable on the CLI:

```
lusdview <scene.usd[z]> --texture-compress off|bc|bc7|astc|etc2|auto
lusdview <scene.usd[z]> --texture-mips on|off
```

- `bc` = BC1/BC3 by opacity; `bc7` = BC7; `astc` = ASTC 4x4; `etc2` = ETC2_RGBA;
  `auto` = the best format for the device.
- **Cap-gated fallback:** the requested format is remapped to one the GPU
  actually supports before encoding, so `--texture-compress astc` on a desktop
  BC-only GPU transparently falls back to BC7 (and to uncompressed if nothing is
  available). Device capabilities are queried from the renderer
  (`RendererCaps`: GL extension strings / `VkPhysicalDeviceFeatures`) and passed
  into the texture build (`ChooseCompressedFormat` in `mesh_build.cc`).
- Both the OpenGL (`glCompressedTexImage2D`, S3TC/RGTC/BPTC/ETC2/ASTC enums) and
  Vulkan (`VK_FORMAT_*_BLOCK`) backends upload the compressed blocks and their
  precomputed content-aware mip chains. The Vulkan backend enables the
  `textureCompression{BC,ETC2,ASTC_LDR}` device features at creation.
- `maxTextureSize` / `textureBudgetMB` cap decoded texture memory before
  compression.

BC6H (HDR) is not reachable from this RGBA8 *compress-on-load* path — an HDR
source must be authored to a BC6H `.ktx2` (`usd-texcomp`, above) and consumed via
the kept-compressed passthrough, which uploads its blocks directly.

### Kept-compressed KTX2 passthrough

When the texture asset is already a `.ktx2`, lusdview can upload its GPU blocks
directly instead of decoding to RGBA8 and re-encoding:

```
lusdview <scene> --texture-keep-compressed on
# runnable example (a 64x64 uni KTX2 with a full mip chain):
lusdview models/ktx2-uni-plane.usda --texture-keep-compressed on
```

- Core/tydra loads the `.ktx2` block payload without decoding
  (`RenderSceneConverterConfig::keep_compressed_textures`), tagging the tydra
  `TextureImage` with its `TextureBlockFormat`.
- lusdview adapts the blocks to the device (`TexToolsAdaptCompressed`): the `uni`
  intermediate is a byte-copy to ASTC where supported, a cheap transcode to BC7
  or ETC2 otherwise, or a decode to RGBA8 as a last resort; a stored BC7/ASTC/BCn
  payload is uploaded as-is when the device supports it. This skips the expensive
  re-encode (and its quality loss).
- The KTX2's full precomputed mip chain is carried through (each level copied or
  transcoded to the chosen format), so minification stays correct without
  re-generating mips. Disabled by default; size-cap / budget resize falls back to
  the normal decode path.
- Both scene loaders take this path: the default (Tydra) loader resolves the
  `customData ktx2` hint through `RenderTexture::ktx2_hint` and reads the sibling
  asset itself, and the legacy loader (`--legacy-load`) goes through
  `RenderSceneConverterConfig::keep_compressed_textures`. `--texture-compress`
  applies on both as well.

## Web

`web/js/texcomp.{html,js}` demonstrates the same strategy in the browser,
using the Basis-free path by itself:

1. A sample RGBA8 texture is compressed once to `uni` in a small WebAssembly
   module (`web/js/texcomp/texcomp_web.c`, built by `build.sh` with emscripten —
   pure C11 over the textools `texcomp` sources, not linked against lightusd).
2. JS detects the browser's compressed-texture support (WebGL2
   `WEBGL_compressed_texture_*` / `EXT_texture_compression_bptc`).
3. It transcodes `uni` to the best available format (BC7 / ASTC / ETC2) and
   uploads a `THREE.CompressedTexture`; where no compressed format exists it
   decodes `uni` to RGBA8 and uploads a `THREE.DataTexture`.

Build and run:

```sh
source /path/to/emsdk/emsdk_env.sh
bash web/js/texcomp/build.sh          # -> texcomp_web.{mjs,wasm} (git-ignored)
cd web/js && npm run dev:texcomp      # or any static server, open /texcomp.html
```

The page reports the detected caps, the chosen GPU format, and the VRAM saving
(e.g. a 256×256 texture: 256 KiB RGBA8 → 64 KiB BC7 = 4×).

The main lightusd WASM module also links textools and exports
`compressTextureToUni` / `transcodeTextureUni`. `getTextureFromUSD` uses that ABI
for real scene textures: native-decoded RGBA8 images go directly through it;
ordinary images decoded by the browser are read back once and follow the same
path. The loader probes BC7, ASTC 4x4, then ETC2 RGBA, uploads a
`THREE.CompressedTexture`, and retains the existing `THREE.DataTexture` /
`THREE.Texture` path when no compressed format or WASM ABI is available. Color
maps set `THREE.SRGBColorSpace` while retaining Three.js's base compressed-format
constant (Three selects the sRGB GPU internal format); data maps stay linear.
The usual scene-texture Y flip is performed before block encoding because WebGL
cannot unpack-flip compressed uploads. Applications can opt out globally with
`LightUSDLoaderUtils.setSceneTextureCompressionEnabled(false)`.

Standard KTX2 is supported alongside that path. External URLs and undecoded
embedded streams are passed to Three.js `KTX2Loader`, which handles Basis
ETC1S/UASTC as well as defined VkFormats such as ASTC. Before dispatch, embedded
headers are classified: TinyEXR's private UNSPECIFIED-model `uni` carrier is
never sent to the Basis transcoder and receives a targeted error if the native
lightusd/textools layer did not already decode it. The loader and its worker are
initialized lazily; ordinary images do not pay that startup cost. By default a
short-lived WebGL capability probe configures the loader. Applications may
instead provide their renderer-configured instance with
`LightUSDLoaderUtils.setKTX2Loader(loader)`, or set a separately hosted
transcoder directory before first use with
`LightUSDLoaderUtils.setKTX2TranscoderPath(path)`. Passing `null` to
`setKTX2Loader` disables the Basis path.

Private `uni` KTX2 files written before this discriminator was introduced used
the real Basis UASTC DFD model by mistake. Regenerate those companions with the
current `usd-texcomp`; the reader now rejects the ambiguous legacy marker rather
than risking a silent misdecode of genuine Basis content.

## Building / gating

- `LIGHTUSD_WITH_TEXTOOLS` (default ON) builds `lightusd_textools` and enables
  the KTX2 image path in the core library and the compression paths in lusdview
  (`LUSDVIEW_WITH_TEXTOOLS`). When OFF, `.ktx2` is not decoded and lusdview falls
  back to its built-in BC1/BC3 encoder.
- textools tests: `ctest -R 'textools-' --output-on-failure` (KTX2
  write→read→decode round-trips live in `textools-texpipe`).
- Web scene-texture tests: `ctest --test-dir web/build_ninja -R
  'texture-compression|basis-ktx2' --output-on-failure` after the normal
  Emscripten build. The Chrome-gated
  `wasm-texture-compression-three-ktx2` test creates ASTC blocks with the main
  WASM ABI, checks both KTX2Loader and the real decoded-scene constructor,
  renders, and verifies both image gradients by GPU readback. The latter pins
  Three.js's base-format + `colorSpace` contract for compressed color maps.

## Follow-ups

- The native textools path intentionally remains Basis-free; Basis
  ETC1S/UASTC interoperability is supplied by Three.js in the web loader.
