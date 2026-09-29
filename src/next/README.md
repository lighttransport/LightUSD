# LightUSD Next - Architecture Redesign

## Overview

The `src/next/` directory contains a complete redesign of LightUSD's core architecture, focusing on:

- **Minimal template usage** - Replace 78+ `DEFINE_TYPE_TRAIT` macros with runtime type dispatch
- **Fast compilation times** - O(1) table lookup instead of 65+ if-else chains
- **Low header dependencies** - Minimal includes, forward declarations where possible
- **Clean separation of concerns** - Modular design with clear boundaries

API breakage and feature deletion are acceptable for this redesign. The goal is a clean, maintainable codebase that can be extended incrementally.

## Current Status

### Implemented Components

Status here means the current `next` subset is implemented. It does not mean
legacy typed-schema parity; render-facing legacy schema compatibility is handled
by extraction/downstream layers such as `tydra/next`.

| Component | Status | Files |
|-----------|--------|-------|
| **Core Type System** | | |
| Type System | ✅ Complete | `types/type-id.hh`, `types/type-info.{hh,cc}`, `types/value.{hh,cc}` |
| Time Interpolation | ✅ Complete | `types/interpolation.{hh,cc}` |
| Path | ✅ Complete | `prim/path.{hh,cc}` |
| **Layer/Stage** | | |
| Property Index | ✅ Complete | `layer/property-index.{hh,cc}` |
| PrimSpec | ✅ Complete | `layer/prim-spec.{hh,cc}` (with TimeSampleStorage) |
| Layer | ✅ Complete | `layer/layer.{hh,cc}` |
| Stage | ✅ Complete | `stage/stage.{hh,cc}` |
| **Parsing** | | |
| Lexer | ✅ Complete | `parser/lexer.{hh,cc}` |
| Value Parser | ✅ Complete | `parser/value-parser.{hh,cc}` |
| ASCII Parser | ✅ Complete | `parser/ascii-parser.{hh,cc}` |
| USDA Reader | ✅ Complete | `reader/usda-reader.{hh,cc}` |
| **Binary Format** | | |
| Crate Format | ✅ Complete | `crate/crate-format.{hh,cc}` |
| Crate Reader | ✅ Complete | `crate/crate-reader.{hh,cc}` |
| USDC Reader | ✅ Complete | `reader/usdc-reader.{hh,cc}` |
| Crate Writer | ✅ Basic | `crate/crate-writer.{hh,cc}` |
| USDC Writer | ✅ Basic | `writer/usdc-writer.{hh,cc}` |
| **Writers** | | |
| Value Printer | ✅ Complete | `writer/value-printer.{hh,cc}` |
| Prim Printer | ✅ Complete | `writer/prim-printer.{hh,cc}` |
| USDA Writer | ✅ Complete | `writer/usda-writer.{hh,cc}` |
| **Evaluation** | | |
| Attribute Eval | ✅ Complete | `eval/attribute-eval.{hh,cc}` |
| **Infrastructure** | | |
| Asset Resolver | ✅ Complete | `resolver/asset-resolver.{hh,cc}` |
| Composition | ✅ Basic | `composition/composition.{hh,cc}` |
| **Schema APIs** | | |
| UsdGeomMesh | ✅ Complete | `schema/geom-mesh.{hh,cc}` |
| UsdGeomXform | ✅ Complete | `schema/geom-xform.{hh,cc}` |
| UsdGeomCamera | ✅ Complete | `schema/usd-geom-camera.{hh,cc}` |
| UsdLux | ✅ Complete | `schema/usd-lux.{hh,cc}` |
| UsdShade | ✅ Complete | `schema/usd-shade.{hh,cc}` |
| **Integration** | | |
| Unified Header | ✅ Complete | `lightusd-next.{hh,cc}` |
| **Tydra/Next** | | |
| Render Data | ✅ Complete | `../tydra/next/render-data.{hh,cc}` |
| Scene Access | ✅ Complete | `../tydra/next/scene-access.{hh,cc}` |
| Render Converter | ✅ Complete | `../tydra/next/render-converter.{hh,cc}` |
| MaterialX | ✅ Complete | `../tydra/next/materialx.{hh,cc}` |

### Build Status

```bash
# Configure, build, and run the standalone next smoke tests.
scripts/run-next-checks.sh
scripts/run-next-checks.sh --help

# Optional broader gates after the smoke tests.
scripts/build-openusd-usdcat.sh --prepare-only  # clone/fetch v26.05 to ref/openusd
scripts/build-openusd-usdcat.sh   # installs OpenUSD v26.05 usdcat to ref/dist/bin/usdcat
scripts/build-openusd-usdcat.sh --full  # full OpenUSD release build to ref/dist
OPENUSD_RETRY_NO_PYSIDE=0 scripts/build-openusd-usdcat.sh --full  # abort on first full-build failure
OPENUSD_FETCH=0 scripts/build-openusd-usdcat.sh --prepare-only  # local-only refresh when v26.05 already checked out
USDCAT_PATH=ref/dist/bin/usdcat scripts/run-next-checks.sh
USD_WG_ASSETS_DIR=/path/to/usd-wg/assets RUN_CORPUS=1 scripts/run-next-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release scripts/run-next-checks.sh
RUN_BENCH=1 BUILD_TYPE=Release BENCH_LAZY_VERTS=4000000 BENCH_LAZY_CLONES=32 scripts/run-next-checks.sh

# Manual labeled test selection after configuring with LIGHTUSD_NEXT_BUILD_TESTS=ON.
ctest --test-dir build-next --output-on-failure -L next -LE 'benchmark|corpus'
```

## Architecture

### Type System (`types/`)

```
TypeId (enum) ──► TypeInfo (runtime metadata) ──► Value (type-erased storage)
```

- **TypeId** - Simple enum with ~50 USD types (no templates)
- **TypeInfo** - Runtime type information with function pointers for operations
- **Value** - 136-byte inline storage with Small Buffer Optimization (SBO)

Key design: All type dispatch happens at runtime via table lookup, not compile-time templates.

### Layer System (`layer/`)

```
Layer
├── prims_: vector<PrimSpec>     (flat storage, cache-friendly)
├── root_indices_: vector<uint32_t>
├── path_to_index_: unordered_map
└── meta_: LayerMeta

PrimSpec
├── name_, type_id_, specifier_, path_
├── props_: PropIndex           (O(1) property lookup)
├── child_indices_: vector<uint32_t>
├── relationships_: unordered_map
└── meta_: PrimSpecMeta
```

Key design: Flat prim storage with indices instead of pointers. No separate Prim class needed - PrimSpec serves both roles.

### Stage System (`stage/`)

```
Stage
├── root_layer_: unique_ptr<Layer>
├── sub_layers_: vector<unique_ptr<Layer>>
└── meta_: StageMeta

UsdPrim (lightweight handle)
├── spec_: const PrimSpec*
├── layer_: const Layer*
└── index_: uint32_t
```

Key design: Stage wraps Layer(s) and provides UsdPrim handles for traversal. Non-copyable due to unique_ptr members.

### Parser System (`parser/`)

```
Lexer ──► ValueParser ──► AsciiParser ──► LayerBuilder ──► Stage
```

- **Lexer** - Simple tokenizer with line/column tracking
- **ValueParser** - Function pointer table for type-specific parsing (no templates)
- **AsciiParser** - PIMPL pattern, uses LayerBuilder internally

### Crate System (`crate/`)

```
CrateReader                          CrateWriter
├── Read sections                    ├── Build token table
├── Decode ValueRep                  ├── Build path table
├── Build prims via LayerBuilder     ├── Build fields/specs
└── Return Stage                     └── Write sections + TOC
```

### Writer System (`writer/`)

```
Stage/Layer ──► PrintLayer() ──► USDA string
Stage/Layer ──► WriteUSDA()  ──► USDA file
Stage/Layer ──► WriteUSDC()  ──► USDC file (via CrateWriter)
```

## File Structure

```
src/next/
├── CMakeLists.txt
├── README.md                    # This file
├── lightusd-next.hh            # Unified header (includes all components)
├── lightusd-next.cc            # High-level API implementation
│
├── types/                       # Core type system
│   ├── type-id.hh              # TypeId enum (~200 lines)
│   ├── type-info.hh            # TypeInfo struct
│   ├── type-info.cc            # Type registry implementation
│   ├── value.hh                # Value class with SBO (136 bytes)
│   ├── value.cc                # Value implementation
│   ├── interpolation.hh        # TimeInterpolation, SampleResult
│   └── interpolation.cc        # Linear/Held interpolation, quaternion slerp
│
├── prim/                        # USD primitives
│   ├── path.hh                 # Path class
│   ├── path.cc
│   └── identifier.hh           # Identifier validation
│
├── layer/                       # Layer system
│   ├── property-index.hh       # PropNameTable, PropSlot, PropIndex
│   ├── property-index.cc
│   ├── prim-spec.hh            # PrimSpec, TimeSampleStorage, ValueStorage
│   ├── prim-spec.cc            # Includes time sample deduplication
│   ├── layer.hh                # Layer, LayerBuilder
│   └── layer.cc
│
├── stage/                       # Stage system
│   ├── stage.hh                # Stage, UsdPrim, StageBuilder
│   └── stage.cc
│
├── parser/                      # ASCII parsing
│   ├── lexer.hh
│   ├── lexer.cc
│   ├── value-parser.hh
│   ├── value-parser.cc
│   ├── ascii-parser.hh
│   ├── ascii-parser.cc            # Parser entry/body
│   ├── ascii-parser-internal.hh   # Private Impl declaration
│   ├── ascii-parser-metadata.cc   # Stage/layer metadata
│   ├── ascii-parser-prim.cc       # Prim/property/relationship bodies
│   ├── ascii-parser-timesamples.cc # timeSamples blocks
│   ├── ascii-parser-utils.cc      # Helpers/property metadata/diagnostics
│   └── ascii-parser-variants.cc   # Variant sets/options
│
├── crate/                       # Binary format
│   ├── crate-format.hh         # Crate structures, ValueRep
│   ├── crate-format.cc
│   ├── stream-reader.hh        # Binary stream helper
│   ├── crate-reader.hh
│   ├── crate-reader.cc          # Scalar value dispatch
│   ├── crate-reader-api.cc      # Public API/source selection
│   ├── crate-reader-arrays.cc   # Array decode/lazy arrays
│   ├── crate-reader-stage.cc    # CrateData -> Stage reconstruction
│   ├── crate-reader-vectors.cc  # Token/String/DoubleVector decode
│   ├── crate-writer.hh
│   └── crate-writer.cc
│
├── reader/                      # High-level readers
│   ├── usda-reader.hh
│   ├── usda-reader.cc
│   ├── usdc-reader.hh
│   └── usdc-reader.cc
│
├── writer/                      # Writers
│   ├── value-printer.hh
│   ├── value-printer.cc
│   ├── prim-printer.hh
│   ├── prim-printer.cc
│   ├── usda-writer.hh
│   ├── usda-writer.cc
│   ├── usdc-writer.hh
│   └── usdc-writer.cc
│
├── eval/                        # Attribute evaluation
│   ├── attribute-eval.hh       # AttributeEval class
│   └── attribute-eval.cc       # Time interpolation, connection following
│
├── resolver/                    # Asset resolution
│   ├── asset-resolver.hh       # AssetResolver, ResolvedAsset
│   └── asset-resolver.cc       # Search paths, package support
│
├── composition/                 # Composition arcs
│   ├── composition.hh          # Compositor, CompositionArc
│   └── composition.cc          # LIVRPS ordering, cycle detection
│
└── schema/                      # Schema convenience APIs
    ├── geom-mesh.hh            # UsdGeomMesh wrapper
    ├── geom-mesh.cc
    ├── geom-xform.hh           # UsdGeomXform wrapper
    ├── geom-xform.cc
    ├── usd-geom-camera.hh      # Camera data, projection matrix
    ├── usd-geom-camera.cc
    ├── usd-lux.hh              # Light types (Distant, Dome, Rect, etc.)
    ├── usd-lux.cc
    ├── usd-shade.hh            # Material, Shader, PreviewSurface
    └── usd-shade.cc

src/tydra/next/                  # Tydra render data conversion
├── chunked-array.hh            # Memory-efficient arrays
├── render-data.hh              # RenderScene, RenderMesh, RenderMaterial
├── render-data.cc
├── scene-access.hh             # Scene query utilities
├── scene-access.cc
├── render-converter.hh         # Stage to RenderScene converter
├── render-converter.cc
├── materialx.hh                # MaterialX conversion
└── materialx.cc
```

## Active Backlog

The original high-priority TODO list has mostly landed. Keep this section as the
current cleanup/refactor queue rather than a historical checklist.

### Cleanup / Refactor

- Split the largest implementation files into focused translation units:
  `crate-reader`, `crate-writer`, `pcp/cache`, `value-parser`, and
  `ascii-parser`. The reader API/source-selection layer, PCP cache helper include
  units/opinion-fill/merge/list-op passes, value-parser helper include units,
  crate stage/array/vector units, writer pass-through/property helpers, and
  ASCII parser metadata/prim/helper/timeSamples/variant units have landed. The
  remaining split work is opportunistic cleanup around smaller EncodeValue
  subhelpers or future sections being edited anyway.
- Keep `doc/refactor-next.md`, `doc/memory-and-performance.md`, and this README
  synchronized with landed behavior and benchmark deltas. Update
  `doc/testing-cpp.md` when next test coverage changes.
- Keep `scripts/run-next-checks.sh` as the canonical standalone smoke test for
  the next module. The main regression build still excludes `next` by design;
  next tests are run explicitly through the standalone build and web/WASM
  next-product gates.
- Keep the public C render-session test covering full publication, incremental
  mesh updates, revision rejection, and ownership of earlier scene snapshots.

### Memory / Performance

- Make benchmark output diff-friendly: struct sizes, layer/stage memory stats,
  RSS, lazy/eager clone RSS, crate pass-through/reencode counts, and mmap-vs-heap
  attribution. `benchmark_next memstats` and `benchmark_next memstats-file`
  provide the current stable key/value output.
- Audit remaining large temporary buffers in the crate reader/writer and prefer
  reusable scratch storage or direct streaming where behavior stays identical.
- Preserve lazy crate arrays and copy-on-write materialized arrays through
  composition, flatten, and write paths.

### Format Parity

- Keep next USDC writer compatibility checks covering memory/file byte parity
  and write -> read -> USDA text semantics; add optional pxrUSD `usdcat`
  comparison when available.
- Keep explicit tests for unusual paths: compressed bool-array rejection,
  bool-array roundtrip, arc metadata dictionaries beside references/payloads,
  unknown crate layouts, lazy TimeSamples, malformed TOC/table/payload edge
  cases, and the dense generated USDC fixture in `test_usdc_roundtrip`.
- Keep USDA backend parity tests covering string, file, `std::ostream`, and
  `StreamWriter` output paths for both Stage and Layer writers.

### Remaining Features

- Keep PointInstancer direct-geometry duplication opt-in. Typed
  `UsdGeomPointInstancer` accessors, `ReadPointInstancerData()`,
  `RenderScene::point_instancers`, prototype mesh bindings, and lightweight
  `point_instance_draws` are present, including direct/inherited mesh material
  IDs, prototype-relative mesh transforms, and unresolved-prototype diagnostics
  on draw refs. `RenderScene::get_point_instancer_draws()` provides O(1)
  per-instancer draw ranges, `get_point_instance_draw_view()` provides
  bounds-checked draw/instancer/mesh/material resolution, and validators check
  draw ranges and prototype mesh bindings. `ConverterConfig::point_instancer
  .duplicate_meshes` can materialize transformed mesh copies when a consumer
  cannot use draw refs directly.
- Keep broadening low-memory flatten coverage for mixed USDA/USDC/USDZ
  dependency graphs. Basic filesystem USDA sublayers and references are
  supported and covered by `test_lazy_array`.
- Decide whether schema convenience APIs should keep growing manually or move to
  generation once the shape stabilizes.

## Design Decisions

### Why no templates?

The original LightUSD used heavy template metaprogramming (78+ `DEFINE_TYPE_TRAIT` macros) which caused:
- Slow compilation (template instantiation in every TU)
- Large object files (3.5-4.4MB for parser)
- Hard to understand error messages
- Difficult to extend

The new design uses runtime type dispatch via function pointer tables, which:
- Compiles faster (no template instantiation)
- Produces smaller object files
- Has predictable performance (O(1) lookup)
- Is easier to debug and extend

### Why flat prim storage?

Storing prims in a flat `vector<PrimSpec>` instead of a tree:
- Better cache locality (sequential memory access)
- Simpler memory management (no tree node allocation)
- Easier serialization (indices instead of pointers)
- Children accessed via indices (still O(1))

### Why unified PrimSpec?

The old design had both `PrimSpec` and `Prim` classes with conversion between them. The new design uses `PrimSpec` for both roles:
- Less code duplication
- No conversion overhead
- Simpler mental model
- `UsdPrim` is just a lightweight handle

### Why move-only Stage/Layer?

Using `unique_ptr` for layer ownership makes:
- Ownership semantics explicit
- No accidental copying of large data
- Clear lifetime management
- Efficient transfer of ownership

## Metrics

### Compile Time Comparison

Measured with GCC 13.3.0 on Linux.

| File | src/next/ | Original | Speedup |
|------|-----------|----------|---------|
| value.cc vs value-types.cc | 0.31 sec | 5.85 sec | **19x** |
| ascii-parser.cc | 0.70 sec | 5.47 sec | **8x** |
| crate-reader.cc | 1.22 sec | 6.18 sec | **5x** |
| prim.cc vs prim-types.cc | 0.70 sec | 3.38 sec | **5x** |

### Build Time Summary

| Metric | Value |
|--------|-------|
| Total files | 23 |
| Serial build (-j1) | 12.3 sec |
| Parallel build (-j16) | 3.0 sec |
| Average per file | 0.54 sec |
| Slowest file | 1.22 sec (crate-reader.cc) |
| Fastest file | 0.14 sec (type-info.cc) |

### Object File Sizes

| File | src/next/ | Original | Reduction |
|------|-----------|----------|-----------|
| ascii-parser.cc.o | 247 KB | 6.4 MB | **26x** |
| value.cc.o vs value-types.cc.o | 94 KB | 3.2 MB | **34x** |
| crate-reader.cc.o | 593 KB | 4.7 MB | **8x** |
| prim-spec.cc.o vs prim-types.cc.o | 578 KB | 4.3 MB | **7x** |
| **Total library .o** | **4.3 MB** | **37.6 MB** | **~9x** |

### Per-Component Breakdown

| Component | Files | Compile Time | Avg/File |
|-----------|-------|--------------|----------|
| types/ | 2 | 0.45 sec | 0.23 sec |
| prim/ | 3 | 1.28 sec | 0.43 sec |
| layer/ | 3 | 2.05 sec | 0.68 sec |
| stage/ | 1 | 0.71 sec | 0.71 sec |
| parser/ | 3 | 1.40 sec | 0.47 sec |
| crate/ | 3 | 2.42 sec | 0.81 sec |
| reader/ | 2 | 1.01 sec | 0.51 sec |
| writer/ | 4 | 2.01 sec | 0.50 sec |
| schema/ | 2 | 1.00 sec | 0.50 sec |

### All Files by Compile Time

| File | Time | .o Size |
|------|------|---------|
| crate-reader.cc | 1.22 sec | 593 KB |
| prim-spec.cc | 0.95 sec | 578 KB |
| crate-writer.cc | 0.95 sec | 423 KB |
| stage.cc | 0.71 sec | 275 KB |
| ascii-parser.cc | 0.70 sec | 247 KB |
| prim.cc | 0.70 sec | 443 KB |
| usda-writer.cc | 0.69 sec | 171 KB |
| layer.cc | 0.62 sec | 275 KB |
| usda-reader.cc | 0.54 sec | 123 KB |
| geom-mesh.cc | 0.54 sec | 157 KB |
| value-parser.cc | 0.49 sec | 223 KB |
| property-index.cc | 0.48 sec | 205 KB |
| prim-printer.cc | 0.47 sec | 65 KB |
| usdc-reader.cc | 0.47 sec | 73 KB |
| geom-xform.cc | 0.46 sec | 74 KB |
| usdc-writer.cc | 0.43 sec | 33 KB |
| value-printer.cc | 0.42 sec | 78 KB |
| attribute.cc | 0.33 sec | 96 KB |
| value.cc | 0.31 sec | 94 KB |
| path.cc | 0.25 sec | 59 KB |
| crate-format.cc | 0.25 sec | 53 KB |
| lexer.cc | 0.21 sec | 49 KB |
| type-info.cc | 0.14 sec | 53 KB |

### Summary

| Metric | Old | New |
|--------|-----|-----|
| Header size | 76 KB | ~8 KB |
| Template instantiations | 233+ | 0 |
| Type dispatch | 65+ if-else | O(1) table lookup |
| Largest .o file | 6.4 MB | 593 KB |
| Total parser/type .o | 37.6 MB | 4.3 MB |
| Per-file compile avg | 5.2 sec | 0.54 sec |
| Static library | — | 5.3 MB |

## Usage Example

### Simple Usage (Unified Header)

```cpp
#include "next/lightusd-next.hh"

using namespace lightusd::next;

int main() {
  Stage stage;
  std::string warn, err;

  // Auto-detect format and load
  if (!LoadUSD("model.usd", &stage, &warn, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  // Traverse all prims
  stage.Traverse([](const UsdPrim& prim) {
    std::cout << prim.GetPath().str() << " : " << prim.GetTypeName() << "\n";
    return true;  // continue traversal
  });

  // Write to different formats
  WriteUSDA(stage, "output.usda");
  WriteUSDC(stage, "output.usdc");

  return 0;
}
```

### C and C++ application boundary

Use `lightusd-c.h` for the C API and `lightusd-cpp.hh` for the C++17 ownership
facade. Link the installed CMake target `lightusd::c`; rendering additionally
uses `lightusd-render-cpp.hh` and `lightusd::render_c`. Configure
`LIGHTUSD_WITH_TYDRA=OFF` for a parser/composition/writer library without Tydra.
The old `compat.hh` architecture-switching facade was removed. The internal
scene model is `Layer`/`PrimSpec`/`Stage`; the unused recursive `Prim` and
`Attribute` model was removed.

### Detailed Reader/Writer API

```cpp
#include "next/reader/usda-reader.hh"
#include "next/writer/usda-writer.hh"
#include "next/stage/stage.hh"

using namespace lightusd::next;

// Load USDA with options
LoadOptions opts;
opts.resolve_assets = true;
opts.base_dir = "/path/to/assets";

LoadResult result = LoadUSDAFromFile("model.usda", opts);
if (!result.success) {
  std::cerr << "Error: " << result.error_summary << "\n";
  for (const auto& e : result.errors) {
    std::cerr << "  Line " << e.line << ": " << e.message << "\n";
  }
  return 1;
}

Stage stage = std::move(result.stage);

// Write with options
USDAWriteOptions write_opts;
write_opts.compact = false;
write_opts.float_precision = 6;
WriteUSDAToFile("output.usda", stage, write_opts);
```

## Using Schema APIs

### Geometry (Mesh, Xform)

```cpp
#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-xform.hh"

using namespace lightusd::next;

// Get all meshes from stage
auto meshes = GetAllMeshes(stage);

for (const UsdGeomMesh& mesh : meshes) {
  // Get topology
  auto faceVertexCounts = mesh.GetFaceVertexCounts();
  auto faceVertexIndices = mesh.GetFaceVertexIndices();

  // Get geometry
  auto points = mesh.GetPoints();
  size_t numPoints = mesh.GetPointCount();

  // Get UVs if present
  if (mesh.HasUVs()) {
    auto uvs = mesh.GetUVs();
  }

  // Check for animation
  if (mesh.HasAnimatedPoints()) {
    auto times = mesh.GetPointsTimeSamples();
    for (double t : times) {
      auto pointsAtTime = mesh.GetPointsAtTime(t);
    }
  }
}

// Work with transforms
UsdPrim xformPrim = stage.GetPrimAtPath("/World/Character");
UsdGeomXform xform(xformPrim);

if (xform.IsValid()) {
  float tx, ty, tz;
  if (xform.GetTranslation(&tx, &ty, &tz)) {
    // Use translation
  }

  float matrix[16];
  xform.ComputeLocalTransform(matrix);
}
```

### Camera

```cpp
#include "next/schema/usd-geom-camera.hh"

using namespace lightusd::next;

UsdPrim camPrim = stage.GetPrimAtPath("/World/Camera");
if (IsCamera(camPrim)) {
  CameraData cam;
  GetCameraData(stage, camPrim, &cam);

  // Lens properties
  float focalLength = cam.focal_length;       // mm
  float hAperture = cam.horizontal_aperture;  // mm

  // Computed values
  float hFov = cam.fov_horizontal;  // radians
  float aspect = cam.aspect_ratio;

  // Projection matrix (OpenGL column-major)
  float proj[16];
  ComputeProjectionMatrix(cam, proj);
}
```

### Lights

```cpp
#include "next/schema/usd-lux.hh"

using namespace lightusd::next;

stage.Traverse([&](const UsdPrim& prim) {
  if (IsLight(prim)) {
    LightType type = GetLightType(prim);

    switch (type) {
      case LightType::SphereLight: {
        SphereLightData data;
        GetSphereLightData(stage, prim, &data);
        // data.radius, data.intensity, data.color, etc.
        break;
      }
      case LightType::DomeLight: {
        DomeLightData data;
        GetDomeLightData(stage, prim, &data);
        // data.texture_file for HDRI
        break;
      }
      // ... other light types
    }
  }
  return true;
});
```

### Materials and Shaders

```cpp
#include "next/schema/usd-shade.hh"

using namespace lightusd::next;

UsdPrim matPrim = stage.GetPrimAtPath("/Materials/Metal");
if (IsMaterial(matPrim)) {
  // Get bound shader
  std::string shaderPath = GetSurfaceShader(stage, matPrim);
  UsdPrim shaderPrim = stage.GetPrimAtPath(shaderPath);

  if (IsPreviewSurface(shaderPrim)) {
    PreviewSurfaceData ps;
    GetPreviewSurfaceData(stage, shaderPrim, &ps);

    // Material properties
    float* diffuse = ps.diffuse_color;  // [3]
    float metallic = ps.metallic;
    float roughness = ps.roughness;

    // Texture connections
    if (!ps.diffuse_texture.empty()) {
      // Follow connection to get texture path
    }
  }
}
```

### Attribute Evaluation

```cpp
#include "next/eval/attribute-eval.hh"

using namespace lightusd::next;

AttributeEval eval(&stage);
eval.SetTime(1.0);  // Frame 1

UsdPrim prim = stage.GetPrimAtPath("/World/Cube");

// Type-safe evaluation with fallback
float opacity = eval.EvalOr(prim, "inputs:opacity", 1.0f);

// Vector evaluation
float color[3];
if (eval.EvalFloat3(prim, "inputs:diffuseColor", color)) {
  // Got color
}

// Full result with metadata
EvalResult result = eval.Eval(prim, "xformOp:translate");
if (result.success) {
  if (result.from_time_sample) {
    // Value came from time sample (possibly interpolated)
  }
  if (result.from_connection) {
    // Value resolved via connection
  }
}
```

### MaterialX Conversion (in tydra/next)

```cpp
#include "tydra/next/materialx.hh"

using namespace lightusd::tydra::next;

MtlxConverter converter;

// Convert MaterialX XML to render material
std::string mtlxContent = R"(
<?xml version="1.0"?>
<materialx version="1.38">
  <standard_surface name="SR_Metal" type="surfaceshader">
    <input name="base_color" type="color3" value="0.8, 0.8, 0.8"/>
    <input name="metalness" type="float" value="1.0"/>
    <input name="specular_roughness" type="float" value="0.2"/>
  </standard_surface>
  <surfacematerial name="Metal" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_Metal"/>
  </surfacematerial>
</materialx>
)";

RenderMaterial material;
if (converter.ConvertToRenderMaterial(mtlxContent, "Metal", &material)) {
  // material.preview_surface contains converted PBR data
}

// Convert USD material with MaterialX binding
UsdPrim matPrim = stage.GetPrimAtPath("/Materials/MyMaterial");
if (converter.ConvertUsdMtlxMaterial(stage, matPrim, &material)) {
  // Got converted material
}
```

## Building a Stage Programmatically

```cpp
#include "next/stage/stage.hh"

using namespace lightusd::next;

StageBuilder builder;
builder.SetDefaultPrim("World");
builder.SetUpAxis("Y");

LayerBuilder& layer = builder.GetLayerBuilder();

layer.begin_prim("World", "Xform");
layer.end_prim();

layer.begin_prim("Cube", "Mesh");
layer.add_property("points", Value::MakeFloat3Array({...}));
layer.add_property("faceVertexCounts", Value::MakeIntArray({...}));
layer.end_prim();

layer.finalize();

Stage stage = builder.Build();
```
