# FBX import (ufbx)

`.fbx` files load as USD layers wherever a layer can be loaded: as the root
layer, through references and payloads, and inside packages. The reader
(`src/reader/fbx-reader.cc`) uses the vendored [ufbx](https://github.com/ufbx/ufbx)
(`src/external/ufbx`, MIT / public domain) and is built with
`LIGHTUSD_WITH_UFBX` (default ON). It replaces the retired OpenFBX stub
(`LIGHTUSD_WITH_USDFBX` is ignored with a warning).

```usda
#usda 1.0
def SkelRoot "Character" (
    references = @rig.fbx@
)
{
}
```

## Mapping

ufbx loads the file Y-up, in meters, with axis and unit conversion applied to
transforms. The layer has `upAxis = "Y"`, `metersPerUnit = 1` and
`defaultPrim = "World"`.

| FBX | USD |
|---|---|
| mesh node | `/World/Geometry/<name>` Mesh: world-space points (bind pose), polygons as written, face-varying normals and UVs (indexed primvars), `subdivisionScheme = "none"` |
| skin deformer | SkelBindingAPI, `skel:skeleton`, `primvars:skel:jointIndices` / `jointWeights` (normalized per point; FBX files often store them unnormalized) |
| bones (skin clusters and bone nodes) | `/World/Skel` Skeleton: joints in parent-first order (paths from the node hierarchy, names made into identifiers), `bindTransforms` from the clusters, `restTransforms` from the node hierarchy |
| animation stack | `/World/Skel/Anim` SkelAnimation: translations, rotations and scales sampled at every frame of the first stack (`timeCodesPerSecond` = the file's frame rate; frame f = FBX time f / fps) |
| blend deformer channels | a `BlendShape` per channel under its mesh. The full-weight keyframe is `offsets` / `pointIndices` (world space, over all the channel's shapes' points); other keyframes are `inbetweens:<name>` with their `weight`. The mesh gets `skel:blendShapes` / `skel:blendShapeTargets`; the SkelAnimation gets `blendShapes` / `blendShapeWeights` (sampled, or the static channel weights) |

`/World` is a SkelRoot when there are bones or blend shapes. Meshes that only
have blend shapes are bound to a Skeleton without joints, as UsdSkel requires.

Not imported yet: materials, cameras, lights, non-skeletal node animation and
NURBS.

OBJ files reuse this reader for their `skel_lib` / `skel_anim` sidecars; see
[usdobj.md](usdobj.md#skeletons-skel_-lines).

## Verification

`tests/obj-skel` (ctest labels `fbx`, `skel`). `skin_compare` evaluates
LightUSD's deformation (UsdSkel blend shapes, then Tydra
`EvaluateSkinnedPoints`) and compares it with ufbx's own evaluation of the same
FBX node (blend offsets, then cluster skinning) at several frames:

- `tool-fbx-skin-ufbx`: a three-joint bend (`rig.fbx`);
- `tool-fbx-blend-ufbx-body` / `-box`: blend shapes (animated and static) on
  the skinned column, and an animated shape on an unskinned cube
  (`rig_blend.fbx`).

`tool-skel-skin-pxr` (with `OPENUSD_ROOT` and `OPENUSD_PYTHON`) checks the same
scenes against OpenUSD: `UsdSkel` skinning and `UsdSkel.BakeSkinning` (blend
shapes, plus a hand-written in-between and extrapolation fixture). The assets
come from `tests/obj-skel/make_assets.py`, which runs in Blender 5.x.
