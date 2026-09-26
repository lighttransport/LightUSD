# Mini USD (Python) — experimental

A pure-Python reader and writer for USD **USDA** (text), **USDC** (binary
"crate") and **USDZ** (package) files. It needs only the Python standard
library (3.10+; the tests also pass on 3.9). numpy is used when it is
installed. The goal is easy USD import and export for apps, scripts and
LLM/coding agents, for example building a 3D model in code and exporting it
as USD. The aim is small, readable code rather than speed.

The format reference is the AOUSD core spec, plus the lightusd C++
implementation in `src/next/crate` of this repository.

## Quick start

```python
import sys; sys.path.insert(0, "sandbox/miniusd/python")   # no install needed
import miniusd
from miniusd import geom

stage = miniusd.Stage(up_axis="Y", meters_per_unit=1.0)
geom.add_xform(stage, "/World")
box = geom.add_cube(stage, "/World/Box", size=1.0, display_color=(0.2, 0.6, 0.2))
geom.set_transform(box, translate=(0, 0.5, 0), rotate=(0, 30, 0))

mat = geom.add_preview_material(stage, "/World/Looks/Wood",
                                diffuse_texture="textures/wood.png", roughness=0.7)
geom.bind_material(box, mat)

stage.save("scene.usda")                  # text
stage.save("scene.usdc")                  # binary crate (.usd also writes crate)
stage.save("scene.usdz", assets={"textures/wood.png": open("wood.png", "rb").read()})
```

Reading:

```python
stage = miniusd.open("scene.usdz")        # format detected from content
for prim in stage.traverse():
    print(prim.path, prim.type_name)
mesh = stage.prim_at("/World/Ball")
points = mesh.get("points")               # numpy (N,3) float32, or list of tuples
print(mesh.attribute("normals").metadata.get("interpolation"))
print(stage.assets.keys())                # other files in the usdz (e.g. textures)
```

`examples/build_scene.py` builds a textured scene with animation, a camera
and a light, and writes all three formats.

## Data model

Mini USD works on **one layer** and does no composition. References,
payloads, inherits, specializes, sublayers and variant sets are read and
written exactly as authored, but they are not composed. `Stage` is an alias
of `Layer`.

| Object | Main fields |
| --- | --- |
| `Layer` / `Stage` | `metadata`, `root` (pseudo-root `PrimSpec`), `assets`, `define(path, type)`, `prim_at`, `traverse`, `save`, `to_usda/usdc/usdz` |
| `PrimSpec` | `name`, `path`, `specifier` (`def`/`over`/`class`), `type_name`, `metadata`, `properties`, `children`, `variant_sets`; `set/get`, `create_attribute`, `create_relationship`, `add_reference`, `add_payload`, `apply_api`, `variant(set, name)` |
| `AttributeSpec` | `type_name` (e.g. `"point3f[]"`), `default`, `time_samples` (dict), `connections` (`ListOp`), `uniform`, `custom`, `metadata`; `set(v, time=None)`, `get(time=None)`, `connect(path)` |
| `RelationshipSpec` | `targets` (`ListOp`), `custom`, `varying`, `metadata` |

pxr-style aliases (`DefinePrim`, `GetPrimAtPath`, `GetAttribute`, ...) exist
for the most common calls.

Metadata keys use the Sdf field names (`documentation`, `variantSelection`,
`variantSetNames`, `references`, `payload`, `inherits`, `apiSchemas`,
`customData`, ...). List-op fields hold `ListOp(explicit=..., prepended=...,
appended=..., deleted=..., added=..., ordered=...)`. Metadata the schema
registry does not know is kept as `UnregisteredValue(text)`, holding the
authored USDA source text, the same way OpenUSD does it.

### Python value mapping

| USD | Python |
| --- | --- |
| `bool`, `int`, `uint`, `int64`, `uint64`, `uchar` | `bool` / `int` |
| `half`, `float`, `double`, `timecode` | `float` (half and float are rounded to their precision) |
| `string`, `token` | `str` (`Token` marks a token inside dictionaries) |
| `asset` | `AssetPath` (a `str` subclass) |
| `float3`, `color3f`, `quatf`, ... | tuple; quaternions are `(w, x, y, z)` as in USDA |
| `matrix4d` | tuple of row tuples |
| `T[]` (numeric) | `numpy.ndarray` of shape `(N,)`, `(N,k)` or `(N,k,k)`, or a list of scalars / tuples without numpy |
| `string[]`, `token[]`, `asset[]` | list of `str` |
| `dictionary` | `dict`; values whose type cannot be inferred are wrapped as `TypedValue(type, value)` |
| ValueBlock (`None` in USDA) | `miniusd.BLOCK`; a Python `None` means "not authored" |

When `PrimSpec.set(name, value)` is called without a type, it looks the type
up in `geom.ATTR_TYPES` (points → `point3f[]`, extent → `float3[]`,
xformOpOrder → `uniform token[]`, ...), then falls back to inferring it from
the value (floats become `float`).

## Schema helpers: UsdSkel, UsdMtlx, UsdPhysics

`examples/build_features.py` uses all three. It builds a skinned, blend-shaped,
animated arm with an OpenPBR MaterialX material, plus rigid bodies and a
driven hinge.

### `miniusd.skel` (skinning, blend shapes, animation)

```python
from miniusd import skel
skel.add_skel_root(stage, "/Char")
sk = skel.add_skeleton(stage, "/Char/Skel", ["Hips", "Hips/Spine"],
                       bind_transforms=[skel.IDENTITY, skel.translation_matrix((0, 1, 0))])
anim = skel.add_animation(stage, "/Char/Skel/Anim", ["Hips", "Hips/Spine"],
                          rotations={1: [q_rest, q_rest], 24: [q_rest, q_bent]},   # (w, x, y, z)
                          blend_shapes=["Smile"], blend_shape_weights={1: [0], 24: [1]})
skel.bind_animation(sk, anim)
skel.bind_skin(mesh, sk, joint_indices, joint_weights, element_size=2)
skel.add_blendshape(mesh, "Smile", offsets, point_indices)
pts = skel.evaluate_points(stage, "/Char/Body", time=12)    # CPU blend shapes + LBS
```

* The authoring follows Blender's layout: `SkelRoot` / `Skeleton` /
  `SkelAnimation` / `BlendShape` with `SkelBindingAPI`. Rest transforms are
  derived from bind transforms (or the other way round) when only one is
  given. `skin_weights_from_nearest()` gives quick automatic weights.
* `evaluate_points` applies blend shapes, then linear blend skinning with
  `geomBindTransform` and per-mesh `skel:joints` remapping, and returns points
  in skeleton space. It matches OpenUSD's `UsdSkelSkinningQuery` to within
  1e-6 on every skinning/blend-shape model in the repo. Inbetween shapes are
  authored but not evaluated.
* `AttributeSpec.get(time)` interpolates time samples the way USD does:
  linearly, and with slerp for quaternions.

### `miniusd.mtlx` (MaterialX, Blender-compatible)

```python
from miniusd import mtlx
mat = mtlx.add_openpbr_material(stage, "/Looks/Skin", base_color=(0.8, 0.5, 0.4), roughness=0.45,
                                base_color_texture="textures/skin.png", normal_texture="textures/n.png")
info = mtlx.read_material(stage, mat)                 # surface node, inputs, textures
xml = mtlx.material_to_mtlx(stage, mat)               # standalone .mtlx document
mtlx.mtlx_file_to_material(stage, "/Looks", "brass.mtlx")   # .mtlx -> embedded USD network
```

* Materials carry `MaterialXConfigAPI` / `config:mtlx:version` and
  `outputs:mtlx:surface` pointing at an `ND_*` shader, with texture nodes in a
  `NodeGraph`. An optional UsdPreviewSurface fallback goes on
  `outputs:surface`. This is the layout Blender 4.x exports and OpenUSD's
  usdMtlx reads.
* `add_standard_surface_material`, the generic `add_node` / `set_input` /
  `add_nodegraph_output`, and `reference_mtlx_file` (usdMtlx file format
  reference) are also provided.
* Conversion between USD and `.mtlx` handles nodegraph interfaces,
  `<include>` / `xi:include`, and inferring the `ND_*` name for nodes without
  an explicit `nodedef`. Every exported document (from the repo's Blender
  files, the MaterialX example library, and freshly generated materials)
  passes `MaterialX.Document.validate()` (1.39.5). A `.mtlx` → USD → `.mtlx`
  round trip keeps every node, value and connection.

### `miniusd.physics` (UsdPhysics)

```python
from miniusd import physics
physics.add_scene(stage, "/PhysicsScene", gravity_direction=(0, -1, 0), gravity_magnitude=9.81)
physics.add_rigid_body(box, mass=2.0, velocity=(0, 0, 1))
physics.add_collider(mesh, approximation="convexHull")
physics.set_material(box, static_friction=0.6, dynamic_friction=0.6, restitution=0.2)  # Blender style
j = physics.add_joint(stage, "/World/Hinge", "revolute", body0=a, body1=b, axis="Z",
                      lower_limit=-60, upper_limit=60)
physics.add_drive(j, "angular", "force", stiffness=50, damping=5, target_position=30)
print(physics.describe(stage))                        # plain-data summary
```

* Also provided: mass properties, kinematic bodies, physics materials bound
  with `material:binding:physics`, fixed / revolute / prismatic / spherical /
  distance joints, `PhysicsLimitAPI`, `PhysicsDriveAPI`, collision groups,
  filtered pairs and the articulation root.
* A pxr schema-conformance check confirms that every authored
  Skel/Physics/Shade/Geom/Lux attribute has its schema's type and
  variability.

## CLI

```
python -m miniusd cat in.usdc                 # print as USDA
python -m miniusd convert in.usda out.usdz    # convert by extension
python -m miniusd info in.usdz                # prim tree + stats
```

## Format support

* **USDA**: the full layer grammar: layer, prim and property metadata; list
  ops (`prepend`, `append`, `delete`, `add`, `reorder`); references and
  payloads with layer offsets; variant sets; `.timeSamples`; `.connect`;
  relationships (relative target paths are made absolute); dictionaries;
  `reorder nameChildren/properties/rootPrims`; triple-quoted strings;
  `@@@` asset paths; `#`, `//` and `/* */` comments. Output closely follows
  `usdcat`.
* **USDC**: reads crate versions 0.4 to 0.12 and writes 0.8.0. The structural
  sections use pxr's integer coding plus LZ4, implemented in pure Python.
  Compressed int and float arrays are read; arrays are written uncompressed.
  Inline encodings match pxr (`half2` raw bytes, int8-packed vectors and
  matrices, double-as-float). TimeSamples, dictionaries, list ops,
  `UnregisteredValue` and the legacy single-`SdfPayload` field are handled.
* **USDZ**: stored (uncompressed) zip with every file 64-byte aligned and the
  root layer first (`usdc` by default, `root_format="usda"` also works).
  Other files are copied through as bytes; image processing is left to the
  app.

### Known limitations (v0)

* No composition or value resolution (the layer is the stage).
* Unsupported value types: `pathExpression`, `opaque`, splines, `VtArrayEdit`,
  and non-standard types such as `uint2`. The declaration is kept but the
  value is dropped, with a warning in `layer.warnings`.
* List ops on unregistered metadata keys are dropped.
* The crate writer does not compress numeric arrays, so files are somewhat
  larger than pxr's.

## Tests

```
cd sandbox/miniusd/python
python -m unittest discover -s tests                     # with numpy (if installed)
MINIUSD_NO_NUMPY=1 python -m unittest discover -s tests  # pure stdlib path
```

`tests/test_pxr_interop.py` runs differential checks against OpenUSD's
`usdcat` when it can find it (`MINIUSD_USDCAT` or `PATH`). For `tests/data/features.usda` and every
`models/*.usda` file, pxr must print identical text for the source, for
Mini USD's `.usdc`, and for Mini USD's `.usda`. Mini USD must also read
pxr-written `.usdc` back to the same layer.

`tests/test_schemas.py` can also run optional external oracles when these
variables are set:

* `MINIUSD_SKEL_ORACLE`: a built `tests/tools/skel_oracle.cpp`, which
  evaluates UsdSkel with OpenUSD.
* `MINIUSD_PXR_PYTHON`: a Python with `pxr` importable, for the schema
  conformance check.
* `MINIUSD_MTLX_PYTHON`: a Python with the `MaterialX` package, for `.mtlx`
  validation.

On the whole repo corpus (848 `.usda` and 250 `.usdc`/`.usdz` files under
`tests/` and `models/`), all but about 5 files that pxr can open round-trip
identically in every direction. The exceptions are the unsupported types
listed above.

## Layout

```
miniusd/
  values.py        value types, type table, coercion / inference, ListOp, Reference, ...
  model.py         Layer / PrimSpec / AttributeSpec / RelationshipSpec, path helpers
  usda_reader.py   tokenizer + recursive-descent parser
  usda_writer.py   usdcat-style text writer (shortest float32/half formatting)
  crate_format.py  crate constants (type ids, spec types, ValueRep bits)
  lz4.py           LZ4 block codec + TfFastCompression framing
  intcodec.py      Usd_IntegerCompression (delta + 2-bit codes)
  usdc_reader.py   crate reader
  usdc_writer.py   crate writer
  usdz.py          USDZ pack/unpack
  geom.py          builders: xform, mesh, cube/sphere/cylinder, preview material,
                   binding, camera, lights
  skel.py          UsdSkel authoring + CPU skinning / blend shape evaluation
  mtlx.py          UsdMtlx (MaterialXConfigAPI) networks, .mtlx XML import/export
  physics.py       UsdPhysics authoring + describe()
  cli.py           python -m miniusd
```
