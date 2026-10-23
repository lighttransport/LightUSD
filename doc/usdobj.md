# Wavefront OBJ support (`usdObj`)

LightUSD imports Wavefront OBJ geometry, MTL materials and the `vw` skin-weight
extension into standard USD schemas. OBJ and MTL parsing use the vendored C11
tinyobjloader and its tessellator, shared by standalone imports, Python and
URDF/MuJoCo mesh loading. The pinned revision, MIT license and local patches are
recorded in [`UPSTREAM.txt`](../src/external/tinyobjloader-c/UPSTREAM.txt).

`usdObj` describes an import facility, **not an Obj prim schema**. Output uses
Mesh, BasisCurves, Points, Material, Shader and SkelBindingAPI. OpenUSD also has
an [example read-only OBJ file-format plugin](https://github.com/PixarAnimationStudios/OpenUSD/tree/2095fafafd033fa23386d7ec6d58c7cc33974518/extras/usd/examples/usdObj).
That inspected implementation imports grouped polygon meshes and indexed
face-varying `primvars:uv`, but does not translate normals or MTL materials.
LightUSD's hierarchy, vertex splitting and `primvars:st` mapping differ from
that example.

## Loading and converting

```python
import lightusd

stage = lightusd.import_obj("mesh.obj")
print(stage.warnings())
stage.save("mesh.usda")
stage.save("mesh.usdc")
stage.save_usdz("mesh.usdz")  # Includes successfully loaded texture assets.

# Equivalent filename-based dispatch:
stage = lightusd.load("mesh.obj")

# Bytes require an explicit format. The source anchors relative MTL paths.
with open("mesh.obj", "rb") as source:
    stage = lightusd.load_bytes(source.read(), format="obj", source="mesh.obj")
```

`import_obj` accepts `up_axis="Z"`, `asset_dirs=()`, `max_memory=0` and
`resolver=None`. `up_axis` may be `"Y"` or `"Z"`; it declares the stage axis and
does not rotate or rescale OBJ coordinates. A positive `max_memory` is an import
budget in bytes; zero keeps finite library defaults. `loads()` parses USDA text,
and `is_usd()` remains a USD-only check. See the [Python API](../python/README.md).

The native `urdf-to-usd` CLI also accepts OBJ:

```sh
urdf-to-usd mesh.obj --format all -o mesh.usda
```

This writes `mesh.usda`, `mesh.usdc` and `mesh.usdz`. Use `--input-format obj` to
override filename detection, `--asset-dir DIR` for additional asset roots, and
`--max-memory BYTES` for a custom budget.

## Geometry mapping

| OBJ input | USD output |
|---|---|
| `o`, `g` partitions | Separate geometry prims with sanitized, unique names and the original label as `displayName` |
| `v`, `f` | Mesh points, `faceVertexCounts` and `faceVertexIndices`; polygon arity is preserved |
| Positive and negative face indices | Resolved OBJ indices, then zero-based USD indices |
| `vn` | Vertex-interpolated normals; explicit source normals are retained |
| Missing normals, `s` smoothing groups | Computed geometric normals, smoothed within matching source position/group pairs |
| `vt` | Vertex-interpolated `texCoord2f[] primvars:st` using the first two components |
| RGB on `v` records | Vertex-interpolated `color3f[] primvars:displayColor` when the color array is complete |
| `l` | Linear, nonperiodic BasisCurves with `curveVertexCounts` |
| `p` | Points |
| `usemtl` | MaterialBindingAPI bindings on face GeomSubsets in the `materialBind` family |
| `vw` | Vertex joint-index/weight primvars with SkelBindingAPI; see below |

Meshes have `subdivisionScheme = "none"`; point-based prims receive a computed
extent. UV/normal seams and flat-shaded corners can split one source position
into several USD points. Point count and index order therefore need not match
the source OBJ position array. A mesh with incomplete corner UVs omits
`primvars:st` and warns instead of inventing UVs for the missing corners.

`obj_topology="indexed"` (Python `import_obj(..., obj_topology="indexed")`, C
`settings_json` `"obj_topology"`) keeps one point per source position used by
each mesh, in first-use order, and writes normals and UVs as indexed
face-varying primvars (`primvars:normals` / `primvars:st` with `:indices`).
Positions, vertex colors and `vw` skin weights are then not duplicated across
seams. The default `expanded` keeps the per-corner representation above. URDF
and MJCF mesh assets are always expanded.

Standalone imports use `/World` as the default prim, with geometry under
`/World/Links/Geometry` and materials under `/World/Materials`. Names are made
valid and collision-safe. Reference composition can replace the `/World` prefix;
select geometry by type or `displayName` when paths are not known in advance.

OBJ meshes used by URDF/MJCF are triangulated. Their line and point primitives
are omitted with a warning. Standalone OBJ import preserves polygons and emits
line/point prims. Bare `v` records without faces, lines or points do not create a
Points prim. Free-form curves/surfaces (`cstype`, `curv`, `surf`) are unsupported
and produce a warning; there is no NURBS evaluation.

OBJ supplies no physical units or rigid-body description. Standalone import
does not invent `metersPerUnit`, colliders, masses or physics joints. Physics
for an OBJ mesh used in a robot asset comes from the URDF/MJCF description.

## MTL materials and textures

Materials become UsdPreviewSurface networks. Supported translations include:

| MTL input | PreviewSurface input or behavior |
|---|---|
| `Kd`, `Ks`, `Ke` | `diffuseColor`, `specularColor`, `emissiveColor` |
| `d` / `Tr` | `opacity` |
| `Ni` | `ior` |
| `Pr`, `Pm` | `roughness`, `metallic` |
| `Ns` without explicit `Pr` | Roughness approximation: `sqrt(2 / (Ns + 2))` |
| Explicit `Pm` | Metallic workflow; otherwise the specular workflow is selected |
| `map_Kd`, `map_Ks`, `map_Ke` | Diffuse, specular and emissive textures |
| `map_d`, `map_Pr`, `map_Pm` | Opacity, roughness and metallic textures |
| `norm` | Normal texture with raw sampling and the `[0, 1]` to `[-1, 1]` remap |
| Texture `-s`, `-o`, `-clamp` | Two-dimensional UV scale/translation and clamp/repeat wrapping |

Maps use UsdUVTexture, a `st` primvar reader and UV transforms. Color maps use
`sRGB`; scalar and normal maps use `raw`. Missing MTL files or textures warn and
leave the geometry usable. Bump maps require height-to-normal conversion and
are not connected. Displacement and unsupported shading features such as sheen
or anisotropy are not reconstructed; this is not a lossless MTL shader archive.

Loaded MTL files validate recognized numeric fields as complete tokens with
finite values in the loader's precision. Malformed numbers, overflowed integer
options, negative `Ns`, invalid on/off options and missing texture filenames
fail the import. A malformed MTL dependency is an error; an unavailable optional
MTL file still produces a warning. Dependency byte/memory limits remain fatal
for MTL files and textures, including resolver-provided bytes. Fatal material
resolution or parsing errors stop subsequent material-library reads.
Omitted trailing color components retain
their defaults. Texture `-s`, `-o` and `-t` accept one to three components, and
filenames beginning with digits (such as `123.png`) are preserved intact.

The C parser retains custom MTL properties in sorted arrays. Finalization uses
an in-place sort with bounded `O(n log n)` work across the library, followed by
linear grouping by material. Equal keys retain their original order. These
custom properties are not translated into USD shader inputs.

Material names and previously requested library names use balanced indexes;
lookup and insertion take logarithmic comparisons instead of scanning every
definition. Repeated identical authored `mtllib` names are resolved once per
load, including unavailable libraries. Different authored names are independent
requests even if a resolver maps them to the same file.

For duplicate material names, `usemtl` binds the first definition. Later
definitions retain their own generated USD material names, source files and
explicit `Pr`/`Pm` flags. A later duplicate cannot redirect an earlier
definition's textures or change its shading workflow. MTL source tracking
handles UTF-8 BOMs and LF, CRLF and CR line endings.

Relative MTL paths resolve from the OBJ file. Each material's texture paths
resolve from the MTL file that defines it. Additional filesystem roots can be
provided through `asset_dirs`; dependencies outside the source/allowed roots
are rejected. An injected resolver is authoritative: a failed callback is not
followed by a direct filesystem read. Memory inputs with external dependencies
need a source anchor or a resolver capable of resolving those dependencies.

Successfully loaded texture bytes are retained for later USDZ packaging,
including after USD composition. Loose USDA/USDC exports keep source texture
identifiers; they do not copy the textures alongside the output.

Within one import, texture requests are cached by source anchor and name, then
by resolved identifier. Material maps and aliases that resolve to the same
identifier share one immutable byte buffer and one USDZ entry. Layers, their
clones and C/Python stages retain shared ownership, so saving remains valid
after a resolver is unregistered or an input package is removed. Each foreign
layer import has its own read cache. USDZ export rewrites shader paths on an
export copy; the original stage keeps its source identifiers.

The shared C++ import frontend offers `Options::open` to separate resolution
from reading, allowing aliases to hit the cache before I/O. The built-in
filesystem, C/Python resolver and package paths use this interface. Existing
combined `Options::read` callbacks remain supported: repeated requests are
cached, while a distinct alias may need one read to discover its identifier.
Both interfaces preserve authoritative resolution and resource limits.

Import reports, independent input/depth limits and `unsupported_policy` are
described in [native import reports and policies](import-report.md). In Python,
`stage.import_report()` exposes conversion counts, dependency reads, diagnostic
codes and memory estimates. `unsupported_policy="preserve"` retains unconverted
MTL bump/displacement/sheen/anisotropy data as non-executable layer metadata.

## Skin weights: the `vw` extension

`vw` is a tinyobjloader extension, not a standard Wavefront OBJ statement. It
provides joint indices and weights while the containing USD supplies the rig:

```text
vw <vertex_id> [<joint_id> <weight>]...
```

Both `vertex_id` and `joint_id` are **zero-based**. The vertex ID addresses the
original `v` position array, not a face corner, UV, normal or emitted USD point.
This differs from ordinary OBJ face indices, which are one-based or negative
relative indices. `vw` does not support negative relative indexing.

For example, save the following as `skin.obj`:

```obj
v 0 0 0
v 1 0 0
v 0 1 0
vw 0 0 0.25 1 0.75
vw 1 1 1.0
vw 2 0 1.0
f 1 2 3
```

The first point has two influences, the second uses joint 1, and the third uses
joint 0. These are joint-order indices; OBJ contains no joint names.

### USD representation and preservation rules

For this triangle the Mesh receives SkelBindingAPI and the following arrays:

```usda
int[] primvars:skel:jointIndices = [0, 1, 1, 0, 0, 0] (
    interpolation = "vertex"
    elementSize = 2
)
float[] primvars:skel:jointWeights = [0.25, 0.75, 1, 0, 1, 0] (
    interpolation = "vertex"
    elementSize = 2
)
```

`elementSize` is the largest influence count in any source `vw` row. For each
emitted point the arrays contain that many consecutive indices and weights.
The same mapping applies to imported Mesh, BasisCurves and Points prims.

- Every influence is retained in source order, including repeated joint IDs.
  There is no four-influence limit, sorting, merging, pruning or normalization.
- Finite signed weights, zero weights, weights above one and non-unit sums are
  accepted. This preserves source data; the consuming skinning implementation
  determines whether those weights are appropriate for deformation.
- Short rows are padded with `(joint 0, weight 0)`. A missing or empty row for an
  emitted point receives all-zero weights and a warning; no root-joint weight
  is guessed.
- With no `vw` records, or with only empty rows, no skin binding is authored.
  All-empty rows produce a warning.
- Original position IDs are tracked through seam splitting, object/group
  partitioning, triangulation and line/point expansion. Every copy of a source
  position receives the same influences.

There may be at most one row per source vertex. Negative, fractional or
out-of-range vertex IDs, negative/fractional joint IDs, duplicate vertex rows,
unpaired joint/weight tokens and non-finite weights fail the import. Joint IDs
must fit a nonnegative signed 32-bit integer. USD import uses float weights and
rejects values outside finite float range. Expanded arrays are checked against
the import memory budget.

The C11 loader populates its arena-owned skin arrays in both the float (`_f`)
and double (`_d`) APIs. The USD frontend uses the float API and authors
`float[]` weights. It cannot check an index against a skeleton's joint count
during OBJ parsing because the skeleton is supplied separately.

### Binding to a skeleton supplied in USD

Importing `vw` does not create a Skeleton, SkelRoot, joint-name order, bind/rest
transforms, animation or `skel:skeleton` relationship. Supply these in the
containing USD. The following complete `character.usda` binds `skin.obj` to a
two-joint skeleton; identity transforms make this a minimal bind-space example:

```usda
#usda 1.0
def SkelRoot "Rig" (
    prepend references = @skin.obj@
    prepend apiSchemas = ["SkelBindingAPI"]
) {
    rel skel:skeleton = </Rig/Skeleton>

    def Skeleton "Skeleton" {
        uniform token[] joints = ["root", "root/child"]
        uniform matrix4d[] bindTransforms = [
            ((1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1)),
            ((1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1))
        ]
        uniform matrix4d[] restTransforms = [
            ((1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1)),
            ((1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1))
        ]
    }
}
```

The referenced geometry inherits the skeleton relationship from `/Rig`.
Joint IDs index the effective `skel:joints` order when one is supplied through
SkelBindingAPI, otherwise the bound Skeleton's `joints` order. Match this order
to the IDs used by the OBJ; an index must be valid for that order. For an
existing rig, use its actual joint order and bind/rest transforms.

OBJ coordinates must agree with the rig's bind space. The importer does not
author `primvars:skel:geomBindTransform`, so USD's identity fallback applies
unless the containing asset authors a suitable transform.

`lightusd.skel.bind_skin` authors this binding on an imported mesh after
checking every joint index against the supplied skeleton:

```python
import lightusd
from lightusd import skel

stage = lightusd.load("character.usda")
mesh = next(p for p in stage if p.type_name == "Mesh")
skel.bind_skin(stage, mesh.path, "/Rig/Skeleton",
               joint_remap={0: "root", 1: "root/child"},   # OBJ joint id -> joint
               geom_bind_transform=[[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
```

It applies SkelBindingAPI, `skel:skeleton`, an optional `skel:joints` subset
(`joints=[...]`, which the indices then address) and
`primvars:skel:geomBindTransform`. `joint_remap` maps source ids to indices or
joint names; an id without a mapping or an index outside the joint order raises
`ValueError` before anything is authored. Influences are kept exactly unless
`max_influences` (keep the strongest per point) or `normalize` (unit weight sums)
is requested. OpenUSD's UsdSkel resolves the result as a skinning target.

Compose with LightUSD and export standard USD for downstream tools:

```python
import lightusd

stage = lightusd.load("character.usda")
stage.save("character.usdc")
```

The exported USD contains geometry and skin primvars without requiring an OBJ
reader in the consuming application. Loading skin data does not itself evaluate
skeletal animation or run a deformable-body simulation.

## Exporting USD meshes to OBJ/MTL

`lightusd.objexport.export_obj(stage, "out.obj")` writes every Mesh (or the
`prims` given) into one OBJ plus `out.mtl`:

- points in world space (default) or each mesh's local space
  (`world_space=False`); normals use the inverse-transpose;
- normals and UVs honor USD interpolation (vertex, faceVarying, uniform,
  constant) and `:indices`; identical values are shared;
- polygons are kept unless `triangulate=True` (fan);
- mesh and `materialBind` GeomSubset bindings become `usemtl` groups, and
  UsdPreviewSurface inputs become `Kd`, `d`, `Ke`, `Ni`, `Pr`, `Pm` and
  `map_Kd` (`materials=False` skips the MTL);
- `primvars:skel:jointIndices/jointWeights` become zero-based `vw` rows
  (`skin_weights=False` omits them).

Importing the result reproduces per-corner positions, UVs and skin weights.
The helper is pure Python (numpy speeds it up when installed; a 400k-vertex
Menagerie robot exports in about 2 s).

## Composition and native APIs

The next-core layer loader used by Python supports OBJ references, payloads and
sublayers. An OBJ layer has default prim `/World`, so a reference without an
explicit prim path imports that subtree, as in the binding example above. An
OBJ sublayer contributes its `/World` hierarchy directly. OBJ, MTL and textures
can also be resolved within the same USDZ package; package dependencies cannot
escape to arbitrary filesystem files.

Unflattened USD assets that refer to OBJ require OBJ loading support in the
consumer. OpenUSD's example plugin does not implement LightUSD's material and
`vw` mapping. Export the composed stage as USDA, USDC or USDZ when those features
must be portable to other USD readers.

| API | Scope |
|---|---|
| Python `import_obj`, `load`, `load_bytes` | Full stage import, including materials, curves, points and skin primvars |
| C `lightusd_import_file`, `lightusd_import_memory` | Full import with `LIGHTUSD_IMPORT_OBJ`; initialize options with `lightusd_import_options_init` |
| C++ `usdObj::ReadObjStageFromFile`, `ReadObjStageFromMemory`, `ReadObjLayerFromMemory` | Legacy full-stage/layer adapters; memory APIs accept source and import options |
| C++ `usdObj::ReadObjFromFile`, `ReadObjFromString` | Legacy geometry-only adapter: combines polygon meshes, requires faces and does not provide the full material/curve/point scene |
| Next-core `pcp::LoadLayerFromFile`, `LoadLayerFromMemory` | Layer loading and composition; use a `.obj` source identifier for format dispatch |

See [`lightusd-import-c.h`](../src/c-api/lightusd-import-c.h),
[`usdObj.hh`](../src/usdObj.hh) and the
[next-core layer loader](../src/next/pcp/layer-registry.hh) for signatures and
ownership. The legacy composition loader also accepts loose OBJ layers; this
does not make every USD-only memory loader an OBJ reader.

OBJ export is a separate Tydra mesh exporter (`tydra::export_to_obj`), not a
general USD-stage-to-OBJ file-format writer. It can write the same zero-based
`vw` extension from render-mesh skin data.

## Build options, diagnostics and verification

`LIGHTUSD_WITH_USDOBJ=ON` enables the OBJ layer adapter. In the standalone next
build it works with `LIGHTUSD_WITH_TYDRA=OFF`, without the XML robot frontend.
The legacy full-stage adapter requires Tydra as well. Disabling USDOBJ removes
the layer adapter; robot import frontends have their own shared OBJ dependency.

Import budgets default to 1 GiB of tracked memory estimates and 512 MiB per
input. They cover parser allocations, dependency reads and expanded data, not
the process's total resident memory. Invalid geometry, malformed skin data,
exceeded parser/expanded-array limits and inputs with no supported primitives
fail the import. Optional missing MTL/maps,
incomplete UVs, missing weights and unsupported features are reported through
`stage.warnings()` in Python or the native warning output.

### Large inputs and generated stress tests

OBJ memory inputs are parsed as bounded byte views; they need no NUL terminator
and the next-core/Python path does not copy the entire input into a string.
File imports without a custom resolver memory-map the OBJ on POSIX systems
(other platforms and resolver-backed reads fall back to reading it), so the
source is not duplicated in anonymous memory; it still counts against
`max_input` and the memory budget. The file must not shrink while it is being
imported.
Serial OBJ, MTL and callback parsing visit lines incrementally rather than
allocating a line table proportional to the input. The next-core layer loader
and C/Python OBJ import APIs pass compact numeric geometry to USD without a
JSON node for every coordinate. The legacy/robot JSON bridge remains available
and checks its additional allocation estimates against the import budget.

Large inputs require explicit budgets above the 512 MiB input/1 GiB memory
defaults. Python's positive `max_memory` also sets the per-input byte limit; the
native import C API exposes `max_input_bytes` and `max_memory_bytes` separately.
Byte offsets use `size_t`, so input larger than 4 GiB is supported on 64-bit
systems when the configured limits and available memory permit it. Geometry
indices remain signed 32-bit; oversized counts are rejected before narrowing.
The shared frontend also limits a line to 16 MiB and face arity to 65,536.

Index and face budgets apply across object/group partitions, not independently
per object. Lines and points consume the shared geometry-index budget;
triangulated polygons consume the number of triangles actually requested.
Malformed/overflowing index tokens and explicit position, UV or normal indices
outside their arrays fail. Missing UV fields can still omit UVs with a warning.
Vertex, normal and UV records are checked in the bounded scanner, including
finite ranges; computed normals use double intermediate arithmetic for large
and subnormal float coordinates. Nonzero generated normals are normalized;
zero-area faces and exactly cancelling smoothing contributions retain zero
normals. Explicit normals are preserved without normalization. Smoothing
accumulators are allocated only for source position/group pairs with missing
normals. All incident faces still contribute their area, including faces with
explicit normals in another object. Flat normals are computed on demand without
a full per-face scratch array.

Repeated object names use advancing suffix counters, so resolving
name collisions does not restart a search through all prior objects. Warning
storage is capped even when a single diagnostic is very large.

[`test_obj_large.cc`](../tests/next/test_obj_large.cc) generates OBJ source in
memory. Its normal CTest case checks geometry, UV/color/skin arrays, allocation
failure cleanup, expansion budgets, tiny decimals, scale-independent and mixed
explicit/generated normals, smoothing memory use, large coordinates, blank-line
memory use and non-terminated buffers ending at a guard page. No large fixture
assets are checked in. Its optional `--triangles` and `--comment-mib` arguments
exercise dense geometry and byte offsets beyond 32 bits independently:

```sh
cmake -S src/next -B build_ninja/obj-large-next -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLIGHTUSD_NEXT_BUILD_TESTS=ON
cmake --build build_ninja/obj-large-next --target test_obj_large -j8
ctest --test-dir build_ninja/obj-large-next -R next_test_obj_large --output-on-failure

# One million triangles with normals, UVs, colors and vw records.
build_ninja/obj-large-next/test_obj_large --triangles 1000000 --comment-mib 0

# Explicit normals with a smoothing group must avoid unused smoothing maps.
build_ninja/obj-large-next/test_obj_large --triangles 1000000 --comment-mib 0 --smooth

# More than 4 GiB of input, with real geometry after the generated comments.
build_ninja/obj-large-next/test_obj_large --triangles 100 --comment-mib 4097
```

These stress sizes are opt-in and allocate the source in RAM. `--parser-only`
runs the C11 parse without the USD conversion. Python-generated buffer and
expansion-limit tests are in
[`test_obj_large.py`](../python/tests/test_obj_large.py).
Generated MTL validation and allocation-failure tests are in
[`test_obj_materials.cc`](../tests/next/test_obj_materials.cc). They cover both
precision families, standalone/OBJ/callback entry points, 2,001 materials,
30,000 reverse-ordered custom fields and duplicate-key order. Python resolver
coverage is in [`test_obj_materials.py`](../python/tests/test_obj_materials.py).
The material test also generates ascending, descending and alternating name
orders, repeated/missing library requests and prefix/duplicate name lookups for
scene and callback loading in both precisions. For a larger optional run:

```sh
cmake --build build_ninja/obj-large-next --target test_obj_materials -j8
build_ninja/obj-large-next/test_obj_materials --lookup-materials 100000
```

Regression coverage also includes:

- [C11/next OBJ reader tests](../tests/next/test_obj_reader.cc): float/double
  skin arrays, invalid records, mixed primitive types and influence metadata.
- [Python OBJ layer/skin tests](../python/tests/test_obj_layers_skin.py): seam
  copies, signed weights, more than four influences, composition, packaged
  dependencies, retained textures and an optional OpenUSD UsdSkel deformation
  oracle.
- [Native import tests](../python/tests/test_native_import.py): normals, UVs,
  MTL maps, concave tessellation, texture packaging and robot mesh imports.

See [regression instructions](testing-cpp.md#native-objrobot-imports-and-python-physics)
for isolated builds, the OpenUSD oracle and the full MuJoCo Menagerie sweep.
