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
`usdcat` when it can find it (`MINIUSD_USDCAT`, `PATH`, or
`~/local/USD/dist/bin/usdcat`). For `tests/data/features.usda` and every
`models/*.usda` file, pxr must print identical text for the source, for
Mini USD's `.usdc`, and for Mini USD's `.usda`. Mini USD must also read
pxr-written `.usdc` back to the same layer.

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
  cli.py           python -m miniusd
```
