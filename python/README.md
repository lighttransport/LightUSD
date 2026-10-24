# lightusd

Lightweight, dependency-free **USD** (Universal Scene Description) library for
Python: load and author USDA / USDC / USDZ, resolve composition, and extract
GPU-ready render data — with **zero-copy NumPy interop**.

Built on the LightUSD *next* core + *tydra-next* (no pxrUSD install required,
no legacy core in the wheel; native `cmake -S .` and WASM still
provide the legacy path via `web/binding.cc`). **v1.0.0**, npm `preview`
dist-tag. Wheels:

- `cp311-abi3` — one stable-ABI wheel covers CPython **3.11+**
- `cp314t` — native **free-threaded** (GIL-less) CPython 3.14 wheel

```bash
pip install lightusd
```

## Reading

```python
import numpy as np
import lightusd

stage = lightusd.load("scene.usdz")          # usda / usdc / usdz, composed
for prim in stage:                            # depth-first traversal
    print(prim.path, prim.type_name)

mesh = stage.prim_at("/World/Mesh")
points = np.asarray(mesh["points"])           # zero-copy (N, 3) float32
attr = mesh.attribute("xformOp:translate")
if attr.has_timesamples:
    value = attr.get(time=12.0)               # linear interpolation
print(mesh.relationship("material:binding").targets)
```

Large USDC crates (district/full-LOD scenes) can exceed the secure default
structural-table caps before the memory budget is reached. Raise them
explicitly for trusted data with `max_crate_elements`:

```python
stage = lightusd.load("district.usdc", max_memory=4 << 30,
                      max_crate_elements=100_000_000)
```

Zero (the default) keeps the secure library limits; the memory budget still
bounds actual allocations.

## Authoring

```python
st = lightusd.Stage.create()
st.up_axis = "Y"
grid = st.define_prim("/World/Grid", "Mesh")
grid.set("points", np.zeros((64, 3), np.float32), type="point3f[]")
grid.set("purpose", "render", uniform=True)
grid.set("xformOp:translate", (0.0, 1.0, 0.0), time=0.0)
st.set_default_prim("World")
st.save("out.usdc")                           # or .usda / .usdz
```

## Render extraction (tydra)

```python
from lightusd import tydra

scene = tydra.to_render_scene(stage)          # triangulated, GPU-friendly
for mesh in scene.meshes:
    vertices = np.asarray(mesh.points)                # (N, 3) float32
    indices = np.asarray(mesh.triangulated_indices)   # (T,) uint32
    material = scene.materials[mesh.material_id]
    base_color = material.param("diffuse_color")      # tuple or RenderTexture
```

## Asset resolution

```python
from lightusd import AssetResolver

resolver = AssetResolver()
path, exists, is_package = resolver.resolve_new("sub/new.usda", "/proj/look/layer.usda")

with resolver.bind_context("/proj/assets"):
    path, exists, is_package = resolver.resolve("tex.png")

with resolver.cache_scope():
    resolver.resolve("tex.png")                # memoized for the scope

resolver.register_package(".pkg", lambda package, entry: f"virtual:{package}|{entry}")
path, exists, is_package = resolver.resolve("bundle.pkg[dir/entry.usda]")

resolver.register_scheme(
    "studio",
    lambda identifier, anchor, scheme_context: identifier,
    lambda identifier: b"#usda 1.0\n",
)
assert resolver.read("studio:layer.usda").startswith(b"#usda")

resolver.register_memory("usd-anon:generated.usda", b"#usda 1.0\n")
data = resolver.read("usd-anon:generated.usda")
info = resolver.asset_info("usd-anon:generated.usda")   # name/repository/exists
stamp = resolver.modification_timestamp("tex.png")      # (seconds, ns) or None
key = resolver.create_identifier("future.usda", "/proj/look/layer.usda")
report, ok = resolver.dependency_report("usd-anon:generated.usda")
count, bytes_used, peak = resolver.memory_stats

with resolver.bind_context("/proj/assets", scheme_contexts={"https": "/cache"}):
    path, exists, is_package = resolver.resolve("https://example.com/tex.png")

stage = lightusd.load("root.usda", resolver=resolver)
```

Resolver and package callbacks are retained until replaced, unregistered, or
the resolver is destroyed. Composition may invoke them from worker threads;
exceptions raised by callbacks are re-raised by `resolve`, `read`, or `load`.

## Notes

- Arrays implement Python's read-only buffer protocol. Use `memoryview(array)`
  or `np.asarray(array)`; `Array.memoryview()` and `__array_interface__` have
  been removed. A view retains its C11 storage after attribute replacement,
  stage closure, or render-scene deletion. Later writes detach shared storage.
- Numeric authoring accepts sequences and typed buffers, including strided and
  big-endian buffers. Shared native descriptors validate and convert components;
  integer overflow rejects the write without changing the attribute.
- NumPy is optional (interop only, not a dependency).
- `lightusd.flatten_file(src, dst)` runs a low-memory compose+flatten
  pipeline that streams large arrays through without decoding them.
- Free-threaded CPython: concurrent reads of one `Stage` are safe; do not
  author to a stage while other threads read it.
- Source builds can set `LIGHTUSD_PY_ENABLE_THREAD=0` to omit optional
  parser/composition/render worker threads while retaining API-level
  synchronization and free-threaded-CPython safety.

Apache 2.0. Part of [LightUSD](https://github.com/lighttransport/LightUSD).


### Native OBJ, URDF and MuJoCo imports

`lightusd.load(path)` also accepts `.obj`, `.urdf`, `.mjcf` and robot/MuJoCo
`.xml` files. XML is identified by its root element. These imports run inside
the native extension; Node, MuJoCo and Newton are not required.

```python
import lightusd
from lightusd import physics, tydra

stage = lightusd.import_urdf("robot.urdf", packages={"robot_assets": "assets"})
# Equivalent automatic dispatch: lightusd.load("robot.urdf")
for joint in physics.inspect(stage).joints:
    print(joint.prim_path, joint.attributes, joint.relationships)

actuator = physics.define_prim(
    stage, "/World/Drive", "NewtonActuator",
    api_schemas=["NewtonPDControlAPI"],
    attributes={"newton:kp": 100.0, "newton:kd": 5.0},
    relationships={"newton:targets": ["/World/Joints/hinge"]},
)
physics.apply_schema(actuator, "NewtonMaxEffortClampingAPI",
                     attributes={"newton:maxEffort": 10.0})
print(tydra.to_render_scene(stage).physics["joints"])
stage.save_usdz("robot.usdz")  # includes imported textures
```

`import_obj`, `import_urdf` and `import_mjcf` return ordinary editable `Stage`
objects. Import options are `up_axis="Z"`, `asset_dirs=()`, `packages=None`,
`allow_missing=False`, `tessellate_collision_shapes=False`, `max_memory=0`,
and `resolver=None`. Positive `max_memory` bounds input and aggregate import
estimates; zero selects finite defaults. Dependencies resolve relative to the
file that declares them. Supply asset directories or package mappings for
external asset roots. Resolver callbacks also work for memory-backed inputs.

For bytes, pass an explicit import format and a source anchor when needed:
`lightusd.load_bytes(data, format="mjcf", source="scene.xml", resolver=resolver)`.
`loads()` remains a USDA-text parser, and `is_usd()` remains USD-specific.

OBJ imports preserve polygon meshes, normals, smoothing groups, UVs, object/group
names and MTL material assignments. Common MTL colors, PBR scalars and texture
maps become USD PreviewSurface networks. Missing MTL/textures warn; missing
meshes/includes fail unless `allow_missing=True`. Bump-to-normal conversion,
free-form OBJ surfaces and unsupported MTL shading features are not synthesized.
Loose USD exports refer to source textures; USDZ exports retain and package them.
Repeated maps and aliases resolving to the same texture share retained bytes
within an import and one USDZ entry. A stage can still save those bytes after
its resolver is unregistered. USDZ path rewriting leaves the source stage intact.

`stage.import_report()` returns native conversion counts, coded diagnostics,
dependency reads, timing and memory estimates without draining warnings.
Failed-import exceptions expose `error.import_report`. Native imports also
accept independent `max_input` (bytes), `max_depth` and
`unsupported_policy="warn"|"error"|"preserve"` options. Preservation retains
known unsupported source records as non-executable USD layer metadata.
See [import reports and policies](../doc/import-report.md) for scope and limits.

`physics.inspect(stage, array_views=True)` uses the shared native physics query
and retains read-only numeric buffers for large state arrays. Snapshots include
property types, variability, blocks and collision groups and survive stage
closure. See [typed physics snapshots](../doc/physics-query.md).

`physics.validate(stage, samples=True)` returns an immutable native validation
report with rule IDs, locations, sample times and coverage. Findings do not
raise exceptions; inspect `report.valid` and `report.complete`. Diagnostic
limits report omissions and prevent a truncated result from passing. See
[physics validation](../doc/physics-validation.md) for coverage and C/C++ APIs.
OBJ layers also work in references, payloads and sublayers. Direct imports emit
linear BasisCurves for `l` and Points for `p`. The zero-based `vw` extension
becomes vertex SkelBindingAPI primvars, preserving every signed influence without
normalizing; the containing USD supplies the skeleton and joint order. See
[OBJ mapping and skin binding](../doc/usdobj.md) for details.

Physics inspection snapshots authored values, arrays and relationship targets,
including `mjc:*` and `newton:*`; it does not run a simulator. Authoring helpers
use canonical USD property names, stage length units, degrees for joint angles,
and real-first quaternions. For a property without a built-in type, pass
`attributes={"mjc:custom:example": ("double[]", [1.0, 2.0])}`.
`RenderScene.physics` exposes the existing C annotation records and textual
extension summaries; use `physics.inspect(stage)` for complete typed engine data.

MJCF flex/deformables and unsupported equality types remain unsupported;
tendon surface wrapping remains approximate. Import warnings are available
through `stage.warnings()`.

## Performance and memory

- `lightusd.load` runs the same next core as the CLI; the binding does not add
  a second copy of geometry. Numeric attributes are zero-copy buffer views, and
  `Stage.flattened()` is the one call that deliberately builds a second stage.
- Live memory is released on `Stage.close()`. RSS may stay high afterwards
  because glibc retains freed composition arenas; `malloc_trim` (or process
  exit) reclaims them.
- For large scenes, preload jemalloc — the allocator the native CLI links:

  ```sh
  LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 python your_app.py
  ```

  On the 10 GB Caldera scene an eager load drops from 1.63 s / 17.3 s CPU to
  1.14 s / 10.9 s CPU at the same ~1.9 GiB peak. Do not link an allocator into
  the wheel; the choice belongs to the process embedding the extension.
- Measurements, A/B method and binding-vs-CLI parity:
  [large-scene profile §2.6.9](../doc/large-scene.md#269-python-binding-abi3-loadcompose-profile-2026-10).
