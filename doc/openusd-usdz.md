# OpenUSD USDZ Creation & `usdchecker --arkit` Validation

Reference notes on **how the upstream OpenUSD (pxr) reference implementation creates USDZ
packages** and **what `usdchecker --arkit` validates**, gathered by reading the OpenUSD source
tree (local checkout at `/mnt/nvme02/work/lightusd-repo/OpenUSD`, paths below are relative to that
root). Written to inform lightusd's own USDZ creation / conversion pipeline (MJCF/URDF → USD,
UE-export large-scene conversion).

Two questions motivated this:

1. **Does OpenUSD convert / resize / repack textures when building a USDZ?** → **No.** See
   [Texture handling](#3-texture-handling--no-convertresizerepack).
2. **What does `usdchecker --arkit` require?** → See
   [usdchecker validation](#4-usdchecker--arkit-validation).

---

## 1. USDZ creation paths

### `usdzip` CLI — `pxr/bin/usdzip/usdzip.cpp`

| Flag | Meaning |
| --- | --- |
| `-r`, `--recurse` | Recursively add files from sub-directories |
| `-a`, `--asset` | Resolvable asset path to the root layer; triggers dependency-aware packaging via `UsdUtilsCreateNewUsdzPackage` |
| `--arkitAsset` | Like `--asset` but uses the ARKit packaging path (`UsdUtilsCreateNewARKitUsdzPackage`) |
| `-c`, `--checkCompliance` | Run compliance checking before packaging |
| `-l`, `--list` | List contents of an existing `.usdz` (`-` ⇒ stdout) |
| `-d`, `--dump` | Dump per-file metadata (offset, compressed/uncompressed size) |
| `-v`, `--verbose` | Verbose output |

Two operating modes:

- **Direct file inclusion** — files added verbatim through
  `SdfZipFileWriter::CreateNew()` / `AddFile()`. No transformation.
- **Asset-based packaging** — `UsdUtilsCreateNewUsdzPackage()` (or the ARKit variant) resolves and
  localizes dependencies, then writes the zip.

### Packaging API — `pxr/usd/usdUtils/usdzPackage.cpp`

**`UsdUtilsCreateNewUsdzPackage()`**

```cpp
bool UsdUtilsCreateNewUsdzPackage(
    const SdfAssetPath& assetPath,
    const std::string& usdzFilePath,
    const std::string& firstLayerName = std::string(),
    bool editLayersInPlace = false);
```

- Collects the asset and **all external dependencies** (recursive traversal).
- **Localizes** asset paths: remaps references to package-relative paths so the archive is
  self-contained. Anonymous layers discovered during traversal are serialized into the package.
- USD *layers* are rewritten (path remapping); **non-USD assets such as textures are not touched**.

**`UsdUtilsCreateNewARKitUsdzPackage()`** — stricter, for ARKit/RealityKit:

- Forces the root layer to **`.usdc`** (binary crate), required by ARKit.
- **Flattens** the USD layer when external composition arcs (references/sublayers/payloads) exist.
  It explicitly warns this loses features:
  *"This will result in loss of features such as variantSets and all asset references to be
  absolutized."*
- Disables UDIM path resolution: `params.SetResolveUdimPaths(false);`
  (`usdzPackage.cpp:126`).

Supporting helpers: `UsdUtilsComputeAllDependencies()` (buckets deps into layers vs. assets),
`UsdUtilsExtractExternalReferences()`, and the localization machinery in
`pxr/usd/usdUtils/assetLocalization*.cpp` / `assetLocalizationPackage.cpp`.

---

## 2. Zip container rules — `pxr/usd/sdf/zipFile.cpp`

USDZ is a plain ZIP with two hard constraints from the USDZ spec, both enforced by
`SdfZipFileWriter`:

- **No compression.** Every entry is stored, not deflated:
  `h.f.compressionMethod = 0; // No compression` (`zipFile.cpp:1044`). `compressedSize` equals
  `uncompressedSize`.
- **64-byte alignment.** Each file's data must start on a 64-byte boundary. Padding is inserted in
  the ZIP local-header "extra" field:

  ```cpp
  // Per usdz specifications, file data must be aligned to 64 byte boundaries.
  constexpr size_t _DataAlignment = 64;                       // zipFile.cpp:442
  uint16_t _ComputeExtraFieldPaddingSize(size_t offset) {
      uint16_t requiredPadding = _DataAlignment - (offset % _DataAlignment);
      if (requiredPadding == _DataAlignment) requiredPadding = 0;
      else if (requiredPadding < _HeaderSize)  // too small for the 4-byte extra header
          requiredPadding += _DataAlignment;   // bump up while keeping alignment
      return requiredPadding;
  }
  ```

Source files are memory-mapped read-only (`ArchMapFileReadOnly`) and their bytes copied straight
into the archive; CRC32 is computed over the original bytes.

---

## 3. Texture handling — NO convert/resize/repack

**OpenUSD does not convert, resize, downsample, recompress, change the colorspace of, or otherwise
transform texture/image assets when building a USDZ.** Image files are copied **byte-for-byte**.

Evidence:

- The packaging path (`assetLocalizationPackage.cpp` → `SdfZipFileWriter::AddFile`) memory-maps the
  source file (`ArchMapFileReadOnly`) and writes the mapping's raw bytes; no image decode/encode is
  involved.
- There is **no image-processing code** anywhere in `pxr/usd/usdUtils/` — no resize, no format
  conversion, no colorspace handling, no UDIM tiling. The ARKit path even *disables* UDIM resolution
  (`SetResolveUdimPaths(false)`).
- Only USD **layers** are ever modified (path localization, and ARKit flattening).

**Implication for lightusd:** if we want texture conversion (e.g. TGA/BMP → PNG), resizing/mipmap
budget control, or atlas repacking, we must implement it ourselves — there is no upstream behavior
to match or rely on. The reference tool's contract is "copy what you reference." This is consistent
with our existing decision to embed texture bytes for USDZ (see `[[mjcf-texture-asset-paths]]`).

---

## 4. `usdchecker --arkit` validation

### CLI — `pxr/usdValidation/bin/usdchecker/usdchecker.cpp`

| Flag | Meaning |
| --- | --- |
| `-s`, `--skipVariants` | Only check the default variant selections |
| `-p`, `--rootPackageOnly` | Check only the root package (skip nested packages) |
| `-o`, `--out FILE` | Output file (default stdout; `stderr` suppresses coloring) |
| `--noAssetChecks` | Skip asset-level checks (stage metadata, defaultPrim) |
| `--arkit` | Enable ARKit/RealityKit USDZ compatibility rules |
| `-d`, `--dumpRules` | Print the enabled rule list |
| `-t`, `--strict` | Treat warnings as failures (non-zero exit) |
| `-v`, `--verbose` | Verbose |
| `--useNewValidationFramework` | Use C++ `UsdValidationRegistry` validators instead of the Python checker; `--variantSets`, `--variants`, `--disableVariantValidationLimit` apply to this path |

Rules live in `pxr/usd/usdUtils/complianceChecker.py` (each rule is a `BaseRuleChecker` subclass).
`GetBaseRules()` always run; `GetARKitRules()` add when `--arkit` is passed.

### Base rules (always)

| Checker | Rule |
| --- | --- |
| `ByteAlignmentChecker` | Files in the usdz are 64-byte aligned (`offset % 64 == 0`) |
| `CompressionChecker` | Files are stored uncompressed (`compressionMethod == 0`) |
| `MissingReferenceChecker` | No unresolvable asset dependencies in any checked variant |
| `StageMetadataChecker` | Stage declares `upAxis` and `metersPerUnit`; asset-level layers need a `defaultPrim`; consumer-level must be **Y-up** |
| `TextureChecker` | For a USDZ (or consumer-level checks) texture extensions must be in `exr, jpg, jpeg, png`. Decodable-but-non-portable formats `bmp, tga, hdr, tif, tx, zfile` get a specific "non-portable" error |
| `PrimEncapsulationChecker` | Boundables not nested under Gprims; Connectable prims only under container-like Connectables |
| `NormalMapTextureChecker` | 8-bit normal maps need `scale = (2,2,2,1)`, `bias = (-1,-1,-1,0)`, `sourceColorSpace = "raw"` |
| `MaterialBindingAPIAppliedChecker` | Prims with material bindings have `MaterialBindingAPI` applied |
| `SkelBindingAPIAppliedChecker` | Prims with UsdSkel binding properties have `SkelBindingAPI` and live under a `SkelRoot` |
| `ShaderPropertyTypeConformanceChecker` | Shader input types match the SDR shader definition |

### ARKit-only rules (`--arkit`)

| Checker | Rule |
| --- | --- |
| `ARKitLayerChecker` | Layer files restricted to `usd, usda, usdc, usdz` |
| `ARKitPrimTypeChecker` | Prim type must be in a whitelist (below) or start with `RealityKit`; custom schemas forbidden |
| `ARKitShaderChecker` | Shaders use `id` implementation source; ids in `UsdPreviewSurface, UsdUVTexture, UsdTransform2d, UsdPrimvarReader*, ND_*`; one connection per input |
| `ARKitMaterialBindingChecker` | Material bindings (direct or collection-based) resolve to valid targets |
| `ARKitFileExtensionChecker` | Package may only contain layers (`usd/usda/usdc/usdz`) + textures (`exr/jpg/jpeg/png`) |
| `ARKitPackageEncapsulationChecker` | If the root is a package it must be self-contained — no references outside the package |

**ARKit allowed prim types** (`_allowedPrimTypeNames`, `complianceChecker.py:792`):
`'' (untyped), Scope, Xform, Camera, Shader, Material, Mesh, Sphere, Cube, Cylinder, Cone, Capsule,
GeomSubset, Points, SkelRoot, Skeleton, SkelAnimation, BlendShape, SpatialAudio, PhysicsScene,
Preliminary_ReferenceImage, Preliminary_Text, Preliminary_Trigger` — plus any `RealityKit*` type.

**Allowed texture formats** (`_basicUSDZImageFormats`, `complianceChecker.py:232`):
`("exr", "jpg", "jpeg", "png")`. Note the doc-string says "only .jpg/.jpeg/.png for consumer-level"
but the enforced list also permits `exr`.

### No file-count / asset-count limit

`usdchecker --arkit` imposes **no limit on the number of texture/asset files** in a package. All
checks are per-file and type/format-based (`ARKitFileExtensionChecker` validates each file's
extension; `TextureChecker` validates each texture's format) — nothing counts files or enforces a
maximum. The only numeric cap in the checker is an unrelated **1000-variant** validation limit in
the new C++ framework (`--disableVariantValidationLimit`).

Caveat: Apple's AR Quick Look *runtime* has its own practical memory/size constraints (e.g. texture
resolution and total package size) that are **not** part of the open-source validator. If the
concern is on-device ARKit behavior rather than passing `usdchecker`, that is a separate,
runtime-side limit.

---

## 5. Status in lightusd — `lusdchecker --arkit`

The rules above are **implemented** in the dependency-free `core` validator
(`src/validation/usd-validation.{hh,cc}`) as an opt-in `arkit` rule group, and are exposed by
the `lusdchecker` CLI (`tools/lusdchecker/`):

```sh
cmake -S . -B build_ninja -G Ninja -DLIGHTUSD_BUILD_TOOLS=ON
cmake --build build_ninja --target lusdchecker

build_ninja/lusdchecker --arkit asset.usdz         # ARKit profile, exit 1 on any arkit.* error
build_ninja/lusdchecker --groups core,arkit a.usda # just the ARKit rules on a layer
build_ninja/lusdchecker --arkit --json asset.usdz  # "checkedGroups":[...,"arkit"]
```

`--arkit` enables the `arkit` group plus the base groups OpenUSD always runs with it
(`core,geom,shade,package`). The group is a **delivery profile, not a defect class**, so it is *not*
part of `--all` / `MakeValidateAllOptions()`: a Z-up layer with a `BasisCurves` prim is perfectly
valid USD, just not ARKit-deliverable.

### Rule mapping: OpenUSD checker → lusdchecker

| OpenUSD checker (complianceChecker.py) | lusdchecker rule id | Group |
| --- | --- | --- |
| `ByteAlignmentChecker` | `package.entry.alignment` | package |
| `CompressionChecker` | `package.entry.compression`, `package.entry.size` | package |
| `MissingReferenceChecker` | `package.dependency.missing`, `package.dependency.external`; `core.composition.error` under `--composed` | package / core |
| `StageMetadataChecker` | `core.layer.upAxis`, `core.layer.metersPerUnit`, `core.layer.defaultPrim`; ARKit-strength: `arkit.stage.upAxis` (must be `Y`), `arkit.stage.metersPerUnit`, `arkit.stage.defaultPrim` | core / **arkit** |
| `TextureChecker` | `arkit.texture.format` (exr/jpg/jpeg/png; bmp/tga/hdr/tif/tx/zfile get the "decodable but non-portable" message) | **arkit** |
| `PrimEncapsulationChecker` | `geom.encapsulation.nestedGprim`, `shade.encapsulation.shaderParent` | geom / shade |
| `NormalMapTextureChecker` | `arkit.normalMap.scaleBias` (scale `(2,2,2,1)`, bias `(-1,-1,-1,0)`, `sourceColorSpace = "raw"`) | **arkit** |
| `MaterialBindingAPIAppliedChecker` | `shade.material.bindingAPI` | shade |
| `SkelBindingAPIAppliedChecker` | `geom.skel.binding.api`, `geom.skel.binding.root` | geom |
| `ShaderPropertyTypeConformanceChecker` | `shade.preview.inputType`, `shade.preview.unknownInput`, `shade.uvTexture.*`, `shade.primvarReader.*` (partial — see below) | shade |
| `ARKitLayerChecker` | `arkit.layer.extension` (sublayer / reference / payload asset paths limited to usd/usda/usdc/usdz) | **arkit** |
| `ARKitPrimTypeChecker` | `arkit.prim.type` (§4 whitelist, plus any `RealityKit*` type; untyped allowed) | **arkit** |
| `ARKitShaderChecker` | `arkit.shader.implementationSource` (must be `id`), `arkit.shader.id` (`UsdPreviewSurface`, `UsdUVTexture`, `UsdTransform2d`, `UsdPrimvarReader*`, `ND_*`), `arkit.shader.connection` (≤ 1 connection per input) | **arkit** |
| `ARKitMaterialBindingChecker` | `arkit.material.binding` (direct and collection-based bindings must resolve to a Material prim) | **arkit** |
| `ARKitFileExtensionChecker` | `arkit.package.fileExtension` (error; replaces the non-ARKit `package.entry.extension` portability warning) + `arkit.package.rootLayer` (root entry must be `.usdc`) | **arkit** |
| `ARKitPackageEncapsulationChecker` | `package.dependency.external`, `package.dependency.missing` | package |

### Deliberately not implemented

- **Full SDR-driven `ShaderPropertyTypeConformance`.** lightusd has no shader-definition registry;
  input types are checked against the generated `UsdPreviewSurface` table and the `UsdUVTexture` /
  `UsdPrimvarReader` schemas only. Arbitrary `ND_*` MaterialX node inputs are not type-checked.
- **Composed-stage / per-variant evaluation.** The `arkit` rules run on the uncomposed `lightusd::Layer`
  (like every other group). Target-resolution rules (`arkit.material.binding`) only hard-fail when
  the layer is self-contained — no sublayers, no external reference/payload arcs — which is exactly
  what an ARKit `.usdz` root layer must be anyway. `--composed` can be combined with `--arkit` to
  flatten first.
- **Image bit-depth introspection.** `arkit.normalMap.scaleBias` decides "8-bit" from the file
  extension (png/jpg/jpeg/bmp/tga), not by decoding the image; a 16-bit PNG normal map is therefore
  still asked for the `(2,2,2,1)` / `(-1,-1,-1,0)` remap.
- **Texture conversion.** As established in §3, OpenUSD never rewrites textures, and neither does
  the checker: non-portable formats are *reported*, not fixed. Converting tga/bmp → png remains the
  converter's job.

Producer-side, the same list is the contract for our USDZ writer: `.usdc` root layer, uncompressed
64-byte-aligned zip entries, `upAxis = "Y"` + `metersPerUnit` + `defaultPrim`, id-based
`UsdPreviewSurface` / `UsdUVTexture` networks, `MaterialBindingAPI` on bound prims, and a
self-contained package.

See also `[[mjcf-texture-asset-paths]]` for our texture-embedding decision.

---

## 6. Source reference

| Component | Path (relative to OpenUSD root) |
| --- | --- |
| `usdzip` CLI | `pxr/bin/usdzip/usdzip.cpp` |
| Packaging API | `pxr/usd/usdUtils/usdzPackage.cpp` (`.h`) |
| Asset localization | `pxr/usd/usdUtils/assetLocalization*.cpp`, `assetLocalizationPackage.cpp` |
| Dependency computation | `pxr/usd/usdUtils/dependencies.cpp` |
| ZIP reader/writer | `pxr/usd/sdf/zipFile.cpp` (`.h`) |
| Compliance / ARKit rules | `pxr/usd/usdUtils/complianceChecker.py` |
| `usdchecker` CLI | `pxr/usdValidation/bin/usdchecker/usdchecker.cpp` |
