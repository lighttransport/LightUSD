# LightUSD Blender bridge

This Blender 5.2+ add-on provides one import/export UI for Blender OpenUSD,
LightUSD, and the native OpenUSD hook path. The LightUSD backend does not need
`pxr`; set **Preferences > Add-ons > LightUSD USD Bridge > LightUSD Python path**
to a directory containing the `lightusd` package and its abi3 extension.

The LightUSD path covers composed stages, meshes, transforms, cameras, lights,
hair curves, MaterialX/OpenPBR-compatible graphs, Blender rigid bodies, and
preservation of `mjc:*` and `newton:*` physics attributes. Native OpenUSD keeps
Blender's importer/exporter and installs a hook class for future scene and
material extensions.

For development from this repository, use `LIGHTUSD_PYTHON_PATH` or point the
preference at `python/` plus the matching `build/lib.*` extension directory.

The checked-in curve regression can be run headlessly with Blender 5.2:

```sh
/home/syoyo/local/blender-5.2.0-linux-x64/blender \
  --background --python dcc/blender/tests/roundtrip.py
```

It imports and re-exports a NURBS curve and an animated BasisCurves groom,
checking topology and both point and width time samples.

## UE skeleton and Rigify interchange

`converter.py` maps USD `SkelRoot`/`Skeleton`/skinned `Mesh` prims to a Blender
armature, vertex groups, and an Armature modifier. Export writes the UE joint
paths, rest and bind transforms, four normalized influences per vertex, and
`unreal:*`/`rigify:*` metadata into the USD layer. The UE source skeleton is
canonical, so Blender bone display names may be changed without breaking the
mapping. `SkelAnimation` translation, quaternion rotation, and scale samples
are exchanged as Blender Actions using the same joint order. USD `BlendShape`
targets and `blendShapeWeights` are exchanged as Blender shape keys and their
animation data, which provides the facial-expression path needed by
MetaHuman-style rigs.

The optional Epic `ue2rigify` addon can be installed separately when a Rigify
control rig or retarget is needed:

<https://github.com/EpicGames/BlenderTools/tree/main/ue2rigify>

LightUSD does not vendor or require that addon. `dcc/blender/rigify.py`
records the canonical UE mannequin map and the stable
`LightUSD.UE.Rigify.v1` retarget profile as USD metadata, and detects the
addon when present; users can run
the addon’s own retarget workflow, then exchange the resulting deformation
animation through the same UsdSkel layer. The headless regression is:

```sh
LIGHTUSD_PYTHON_PATH="$PWD/python" \
  /home/syoyo/local/blender-5.2.0-linux-x64/blender \
  --background --python dcc/blender/tests/usdskel_roundtrip.py
```

Exported bound meshes are nested below their `SkelRoot` and carry
`SkelBindingAPI`, so the generated USDA can be imported by UE as a skeletal
mesh rather than a static mesh. The UE-side smoke test confirmed one skeleton,
one physics asset, and one morph target from the Blender-generated layer.
