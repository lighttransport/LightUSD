# lusdzconvert

Convert USD (`.usda` / `.usdc` / `.usdz`) into an ARKit-friendly USDZ package,
or flatten to a single USDC/USDA file.

Capabilities:

- **Read USD, pack to USDZ** — loads any USD format and writes a spec-compliant
  USDZ (AOUSD Core Spec §17: uncompressed zip, 64-byte aligned, root layer first),
  then validates the result with `ValidateUSDZ`.
- **Flat USDC / USDA output** — write a single flattened binary or ASCII file
  with `--outputFormat usdc` or `--outputFormat usda`. Textures retain their
  original bytes and format as external references; resolved filesystem paths
  are rebased to the output layer's directory.
- **Composition flatten** — resolves sublayers / references / payloads / variants
  (LIVRPS) into a single flattened stage before writing.
- **Texture resize** — caps each texture's longest edge.
- **Texture re-encode** — re-encodes PNG textures with [fpnge](https://github.com/veluca93/fpnge)
  (fast SIMD PNG encoder) when the library is built with `-DLIGHTUSD_WITH_FPNGE=ON`,
  otherwise falls back to the portable `fpng` encoder.
- **Texture repack** — standalone channel merging (e.g. `R=gloss, G=roughness`)
  via a generic channel-map spec.

## Build

```bash
cmake -S . -B build_ninja/legacy-convert -G Ninja \
      -DLIGHTUSD_NATIVE_PRODUCT=legacy \
      -DLIGHTUSD_BUILD_TOOLS=ON -DLIGHTUSD_WITH_TYDRA=ON \
      -DLIGHTUSD_WITH_FPNGE=ON -DLIGHTUSD_FPNGE_SIMD=avx2
cmake --build build_ninja/legacy-convert --target lusdzconvert -j16
# binary: build_ninja/legacy-convert/tools/lusdzconvert/lusdzconvert
```

The native converter currently uses the legacy API and requires the explicit
product selection above. The default next product supports the JavaScript
`usdzconvert` path through the next WASM module.

`LIGHTUSD_FPNGE_SIMD` selects the fpnge code path at compile time:
`avx2` (default), `sse41`, `sse2`, or `scalar`. fpnge upstream requires SSE4.1
minimum, so `sse2` and `scalar` do not compile fpnge and PNG encoding falls back
to `fpng`.

## Usage

```
lusdzconvert inputFileOrDirectory [outputFile] [options]
lusdzconvert -repack <outputImage> -packR <src> [-packG <src> ...] [options]
```

When the input is a directory, it must contain exactly one top-level `.usd`,
`.usda`, or `.usdc` root layer. Subdirectories are used for referenced assets,
but root-layer discovery itself is not recursive.

### Conversion options

| Flag | Meaning |
|------|---------|
| `-v`, `-verbose` | Verbose logging |
| `-noFlatten` | Do not compose/flatten (default: flatten) |
| `--outputFormat <fmt>` | Output format: `usdz` (default), `usdc`, or `usda`. `usdc`/`usda` produce flat files with external texture references |
| `--rootLayerFormat <fmt>` | Root layer format inside flattened USDZ: `usdc` (default) or `usda`; `-noFlatten` writes a USDA root to preserve composition arcs |
| `--pxr-usdcat <path>` | After conversion, also run pxrUSD `usdcat --flatten` to produce a reference file for comparison (e.g. `output.usdz.reference.usdc`) |
| `-arkitCompatible` | Apply ARKit USDZ policy: flatten for USDZ, force a USDC root, apply Y-up metadata, and use stricter texture checks |
| `-metersPerUnit <v>` | Override stage `metersPerUnit` |
| `-upAxis <X\|Y\|Z>` | Override stage up axis |
| `-url <s>` / `-copyright <s>` | Store in stage documentation |
| `-resizeTextures <N>` | Cap each texture's longest edge to N |
| `-textureFormat <keep\|png\|jpeg>` | Output texture format (default: keep) |
| `-pngEncoder <fpnge\|fpng>` | PNG encoder backend |
| `-jpegQuality <1-100>` | JPEG quality (default 90) |
| `-noReencode` | Copy unmodified textures through byte-for-byte |
| `-includeUnusedTextures` | Also convert/package image files in the input layer directories that are not referenced by `UsdUVTexture` |
| `-targetTextureSize <size>` | Shrink all textures so their **total** fits `<size>` (e.g. `100MB`, `50mb`, `1048576`) |
| `-fitStrategy <size\|quality>` | Lever to meet the budget: reduce dimensions (`size`) or transcode to JPEG + lower quality (`quality`) |
| `-fitMinTextureSize <N>` | Smallest longest-edge allowed by the size search (default 64) |
| `-fitMinQuality <1-100>` | Lowest JPEG quality allowed by the quality search (default 30) |

### Bake UDIMs into one texture

`--bake-udim grid` stitches tiles in their UDIM positions and inserts a
`UsdTransform2d` after the existing UV input. It preserves mesh topology and
supports non-flattened layers, including variant contents. `--bake-udim dense`
packs sorted tile IDs into a compact atlas with gutters and a reserved blank
cell, then authors separate face-varying UVs for each texture layout. Original
UVs and other texture consumers remain intact. Dense mode requires flattened
native output.

```sh
lusdzconvert scene.usda scene.usdz --bake-udim grid --udim-max-tiles 32
lusdzconvert scene.usda scene.usdz --bake-udim dense --udim-cross-tile split
lusdzconvert scene.usda baked.usda --outputFormat usda --bake-udim dense
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--bake-udim off\|grid\|dense` | `off` | Enable stitching or compact packing |
| `--udim-max-tiles N` | `100` | Maximum resolved tiles per texture layout; excess fails |
| `--udim-max-atlas-size N` | `8192` | Maximum atlas edge in pixels |
| `--udim-memory-budget SIZE` | `512MB` | Bake working-memory cap; accepts bytes, MB, or GB |
| `--udim-cross-tile reject\|split` | `reject` | Dense faces crossing tile edges fail or are clipped |
| `--udim-dense-padding N` | `2` | Dense gutter width in pixels |
| `--udim-subdivision-level N` | `2` | Subdivision levels before dense UV remapping |

Tile discovery accepts `<UDIM>`, `%04d`, and `%(UDIM)d` patterns and IDs
1001–9999. The tile limit applies to the union of IDs for an animated texture
file, so all frames share one layout. Missing cells are transparent. Tiles are
never silently omitted to meet a limit. Source resolution is retained unless
`-resizeTextures` requests a cap; an oversized atlas or insufficient working
memory fails explicitly. Integer tiles produce PNG8/PNG16; floating-point
tiles produce EXR. JPEG requires an explicit texture-format request and fails
when it would discard 16-bit or floating-point precision.

Dense splitting preserves winding, material subsets, compatible numeric and
string/token primvars, authored deformation samples, skin weights, and sparse
blendshape/in-between offsets. Subdivision is evaluated with its authored
scheme and boundary/crease rules. Internal prototype consumers retain their
instance transforms. Discrete primvars require equal contributors. Changing
topology, changing joint ownership, moving UVs that change tile ownership or
clipping topology, and animated UV-transform networks fail explicitly.

USDZ embeds generated textures. Flat USDA/USDC writes content-named sidecars in
`<output filename>_textures/` and publishes the root only after the sidecars
succeed; conflicting existing files fail without replacing the previous root.
The JS API and CLI expose the same controls; see
[JavaScript UDIM baking](../../web/js/docs/udim-baking.md).

### Fit textures to a size budget

`-targetTextureSize` searches a single lever to make the **sum of all texture
bytes** fit the given budget:

```bash
# Make all textures fit ~100 MB by reducing their dimensions (keeps PNG)
lusdzconvert model.usd model.usdz -targetTextureSize 100MB -fitStrategy size

# Fit ~25 MB by transcoding to JPEG and lowering quality (references rewritten)
lusdzconvert model.usd model.usdz -targetTextureSize 25MB -fitStrategy quality

# Combine: cap dimensions at 2048 first, then search JPEG quality to fit
lusdzconvert model.usd model.usdz -targetTextureSize 25MB -fitStrategy quality -resizeTextures 2048
```

If the chosen lever cannot reach the budget at its floor (`-fitMinTextureSize` /
`-fitMinQuality`), the floor is used (best effort) and a warning is printed.

### Repack mode

`-repack <outputImage>` writes a single image whose channels are sourced from
other images. Each `-packR/-packG/-packB/-packA <src>` takes either
`file.png:CH` (CH = channel index 0..3, default 0) or `const:VALUE` (0..255).

```bash
# glTF-style ORM: R=occlusion, G=roughness, B=metallic
lusdzconvert -repack orm.png \
  -packR ao.png:0 -packG rough.png:0 -packB metal.png:0 -packChannels 3

# pack a gloss map into R and a roughness map into G
lusdzconvert -repack packed.png -packR gloss.png:0 -packG rough.png:0 -packChannels 2
```

## Examples

```bash
# Flatten + ARKit + downscale textures to 1024 px
lusdzconvert model.usd model.usdz -arkitCompatible -resizeTextures 1024 -v

# Flat USDC (binary) without texture embed
lusdzconvert model.usd model.usdc --outputFormat usdc

# Flat USDA (ASCII) for human-readable diff
lusdzconvert model.usd model.usda --outputFormat usda

# Directory input, flattened USDZ with an ASCII root layer
lusdzconvert asset_dir asset.usdz --rootLayerFormat usda

# USDZ with pxrUSD reference file for comparison
lusdzconvert model.usd model.usdz --pxr-usdcat ../OpenUSD/dist/bin/usdcat

# Repack a USDZ, transcoding every texture to fpnge-encoded PNG
lusdzconvert in.usdz out.usdz -textureFormat png -pngEncoder fpnge
```

## Notes / limitations

- Automatic re-wiring of a material's shader graph to consume a repacked texture
  (e.g. detecting separate metallic/roughness textures in an existing material and
  merging them) is **not** performed in this version — repack is exposed as a
  standalone texture operation. Resize / re-encode / path-normalization in the
  conversion pipeline preserve the existing shader graph.
- Outside UDIM baking, EXR/16-bit textures are passed through unchanged (no resize/transcode), and
  are counted as fixed overhead against a `-targetTextureSize` budget.
- `-targetTextureSize` budgets the **sum of texture bytes**; the final `.usdz`
  is slightly larger (USDC layer + zip overhead).
