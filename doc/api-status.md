# USD API Implementation Status

Coverage of OpenUSD schema domains in lightusd.

> This page's historical `done/stub` counts describe the legacy typed-prim
> surface only. They are not renderer or cross-pipeline percentages. The pinned
> machine-readable baseline is
> [`generated/openusd-schema-26.08.json`](generated/openusd-schema-26.08.json),
> and the capability tiers and refresh procedure are documented in
> [`openusd-schema-compatibility.md`](openusd-schema-compatibility.md).

**Legend**: done = full pipeline (parse + pprint + USDC write), stub = struct exists but not wired into pipeline, -- = not implemented

---

## UsdGeom -- Geometry

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| Scope | concrete | done | In `core/model-scope.hh` |
| Xform | concrete | done | |
| Mesh | concrete | done | |
| BasisCurves | concrete | done | |
| NurbsCurves | concrete | done | |
| Points | concrete | done | |
| PointInstancer | concrete | done | |
| GeomSubset | concrete | done | |
| Sphere | concrete | done | |
| Cube | concrete | done | |
| Cone | concrete | done | |
| Cylinder | concrete | done | |
| Capsule | concrete | done | |
| Camera | concrete | done | |
| Plane | concrete | done | |
| NurbsPatch | concrete | done | |
| HermiteCurves | concrete | done | |
| TetMesh | concrete | done | |
| Cylinder_1 | concrete | done | Updated cylinder with separate top/bottom radii |
| Capsule_1 | concrete | done | Updated capsule with separate top/bottom radii |
| VisibilityAPI | API (single) | done | |
| GeomModelAPI | API (single) | done | |
| BackPlateAPI | API (multiple) | next schema | OpenUSD 26.08 registry/validation, typed extraction, render-camera propagation, lusdrender color/alpha/depth compositing, and camera-space depth-tested GL/Vulkan raster display for every authored instance |
| MotionAPI | API (single) | done | |
| PrimvarsAPI | API (non-applied) | done | |
| XformCommonAPI | API (non-applied) | done | |

**Coverage: 20/20 concrete, 5/5 API -- 100%**

---

## UsdLux -- Lighting

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| SphereLight | concrete | done | |
| DiskLight | concrete | done | |
| RectLight | concrete | done | |
| CylinderLight | concrete | done | |
| DistantLight | concrete | done | |
| DomeLight | concrete | done | |
| DomeLight_1 | concrete | done | Adds poleAxis for flexible orientation |
| GeometryLight | concrete | done | |
| PortalLight | concrete | done | |
| PluginLight | concrete | done | |
| LightFilter | concrete | done | |
| PluginLightFilter | concrete | done | |
| LightAPI | API (single) | done | |
| MeshLightAPI | API (single) | done | |
| VolumeLightAPI | API (single) | done | |
| LightListAPI | API (single) | done | |
| ListAPI | API (single) | done | |
| ShapingAPI | API (single) | done | |
| ShadowAPI | API (single) | done | |

**Coverage: 12/12 concrete, 7/7 API -- 100%**

---

## UsdShade -- Materials & Shaders

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| Material | concrete | done | |
| Shader | concrete | done | |
| NodeGraph | concrete | done | |
| MaterialBindingAPI | API (single) | done | |
| NodeDefAPI | API (single) | done | |
| ConnectableAPI | API (non-applied) | done | |
| CoordSysAPI | API (multi) | done | |

**Shader node implementations:**

| Shader | Status |
|--------|--------|
| UsdPreviewSurface | done |
| UsdUVTexture | done |
| UsdTransform2d | done |
| UsdPrimvarReader_float | done |
| UsdPrimvarReader_float2 | done |
| UsdPrimvarReader_float3 | done |
| UsdPrimvarReader_float4 | done |
| UsdPrimvarReader_int | done |
| UsdPrimvarReader_string | done |
| UsdPrimvarReader_normal | done |
| UsdPrimvarReader_point | done |
| UsdPrimvarReader_vector | done |
| UsdPrimvarReader_matrix | done |
| OpenPBRSurface | done |
| MtlxUsdPreviewSurface | done |
| MtlxAutodeskStandardSurface | done |
| MtlxOpenPBRSurface | done |
| MtlxUniformEdf | done |
| MtlxConicalEdf | done |
| MtlxMeasuredEdf | done |
| MtlxLight | done |

**Coverage: 3/3 concrete, 4/4 API, 21/21 shader nodes -- 100%**

---

## UsdSkel -- Skeletal Animation

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| SkelRoot | concrete | done | |
| Skeleton | concrete | done | |
| SkelAnimation | concrete | done | |
| BlendShape | concrete | done | |
| SkelBindingAPI | API (single) | done | |

**Coverage: 4/4 concrete, 1/1 API -- 100%**

---

## UsdPhysics -- Physics Simulation

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| PhysicsScene | concrete | done | |
| PhysicsCollisionGroup | concrete | done | |
| PhysicsJoint | concrete | done | Generic D6 joint (all DOFs free) |
| PhysicsRevoluteJoint | concrete | done | |
| PhysicsPrismaticJoint | concrete | done | |
| PhysicsSphericalJoint | concrete | done | |
| PhysicsDistanceJoint | concrete | done | |
| PhysicsFixedJoint | concrete | done | |
| PhysicsRigidBodyAPI | API (single) | done | |
| PhysicsCollisionAPI | API (single) | done | |
| PhysicsMaterialAPI | API (single) | done | |
| PhysicsMeshCollisionAPI | API (single) | done | |
| PhysicsMassAPI | API (single) | done | Struct + enum registered |
| PhysicsFilteredPairsAPI | API (single) | done | Struct + enum registered |
| PhysicsArticulationRootAPI | API (single) | done | Marker schema, struct + enum registered |
| PhysicsDriveAPI | API (multi) | done | |
| PhysicsLimitAPI | API (multi) | done | |

**Also: MuJoCo extensions** (lightusd-specific, not in OpenUSD):
MjcActuator, MjcTendon, MjcKeyframe, MjcSensor (concrete); MjcSceneAPI, MjcJointAPI, MjcCollisionAPI, MjcMeshCollisionAPI, MjcMaterialAPI, MjcSiteAPI, MjcImageableAPI, MjcEqualityAPI and variants (API schemas).

**Coverage: 8/8 concrete, 9/9 API -- 100%**

---

## UsdMedia -- Media

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| SpatialAudio | concrete | done | |
| AssetPreviewsAPI | API (single) | done | Marker schema, struct + enum registered |

**Coverage: 1/1 concrete, 1/1 API -- 100%**

---

## Apple Preliminary USDZ AR Schemas

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| Preliminary_PhysicsGravitationalForce | concrete | done | |
| Preliminary_InfiniteColliderPlane | concrete | done | |
| Preliminary_ReferenceImage | concrete | done | |
| Preliminary_Behavior | concrete | done | |
| Preliminary_Trigger | concrete | done | |
| Preliminary_Action | concrete | done | |
| Preliminary_Text | concrete | done | |
| Preliminary_AnchoringAPI | API (single) | done | Via host prim props |
| Preliminary_PhysicsMaterialAPI | API (single) | done | Via host prim props |
| Preliminary_PhysicsRigidBodyAPI | API (single) | done | Via host prim props |
| Preliminary_PhysicsColliderAPI | API (single) | done | Via host prim props |

**Coverage: 7/7 concrete, 4/4 API -- 100%**

---

## UsdVol -- Volumes

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| Volume | concrete | done | Typed GPrim-derived (`src/usdGeom.hh`), schema attrs modeled, reconstructed in `src/prim-reconstruct-vol.cc` |
| FieldAsset | concrete | done | `GPrim`-derived base for field assets; pxr 25.x renamed it to `VolumeFieldAsset` (both names handled, see `src/next/schema/schema-registry.cc`) |
| OpenVDBAsset | concrete | done | `FieldAsset`-derived |
| Field3DAsset | concrete | done | `FieldAsset`-derived |
| VolumeFieldBase | abstract | -- | pxr 25.x name for the former `FieldBase` abstract base |
| VolumeFieldAsset | abstract | -- | pxr 25.x name for the former `FieldAsset` abstract base |
| ParticleField | concrete | next schema | Registry ancestry and validation; legacy preserves generic data |
| ParticleField3DGaussianSplat | concrete | rendered | Next Vulkan/RT Gaussian carrier plus 26.08 fallbacks and validation |
| 11 ParticleField API schemas | API (single) | next schema | Official position/orientation/scale/opacity/kernel/radiance declarations; typed accessors remain |

**Coverage: 4/6 concrete, 0/11 API** (Volume, FieldAsset, OpenVDBAsset,
Field3DAsset implemented; ParticleField / ParticleField3DGaussianSplat
remain; VolumeFieldBase / VolumeFieldAsset are abstract bases)

> Note: `core/model-scope.hh` carries a corrective comment — the UsdVol prims
> (Volume, FieldAsset, OpenVDBAsset, Field3DAsset) are GPrim-derived and defined
> in `usdGeom.hh`, not placeholder structs.

The runtime OpenVDB adapter (`src/usdVol.hh`) is backed by the vendored
TinyVDB C API:
- **Value types:** bool, mask, int32, int64, half, float and double grids from
  OpenVDB 1.x--13.x files.
- **Vector grids:** vec3i/f/d, as magnitude or, with
  `VDBReadOptions::vector_components`, as three channels.
- **Transforms:** any linear map, reported as an index-to-world matrix.
- **Field reads:** `ReadVDBGrid` decodes only the grid a field selects
  (`fieldName`, then `fieldIndex` among equal names), optionally downsampled to
  a voxel budget. `ReadVDBGridInfo` lists grids, class and bounds from headers
  alone.
- **Point grids:** PointDataGrid is diagnosed and skipped.
- **Agreement with OpenVDB:** dense values equal OpenVDB's on all 20 float
  grids of the tinyvdbio corpus.
  - Active tiles now carry their values; they used to read the background, as
    in `suzanne.vdb`'s interior.
  - Half-stored subnormals decode exactly; `smoke.vdb`'s faint densities used
    to come out at half their value.
- **Corpus regression:** `tests/run-tinyvdbio-corpus.sh`, also the
  `usdvol-vdb-corpus` ctest. Set `LIGHTUSD_TINYVDBIO_DATA_DIR` or
  `$TINYVDBIO_DATA_DIR` to run it; it is skipped otherwise.

**Rendering (tydra/next).** `RenderScene::volumes` holds one `RenderVolume`
per Volume.
- **Fields:** each `field:<name>` relationship with exactly one (forwarded)
  target, resolved by `src/tydra/vol` at the scene time. Placement is the
  field prim's transform times the grid transform.
- **Extent:** authored, or else the union of the fields' world boxes.
- **Material:** volume shader constants.
- **Voxels:** decoded on demand through `RenderScene::volume_cache`.
- **NanoVDB and sampling:** the same cache converts a sparse grid to one
  NanoVDB buffer (`ReadVDBGridNanoVDB`) and samples world-space points through
  tinyvdb's sparse sampler (nearest or trilinear, scalar or vec3), per scene
  time.
- **ParticleFields:** `RenderScene::particle_fields` applies the schema's
  array rules.
- **Access:** C API `lightusd_render_volume_*` (including
  `..._field_nanovdb` and `..._field_sample`) and
  `lightusd_render_particle_data_*`, Python `RenderScene.volumes()` /
  `volume_dense()` / `volume_nanovdb()` / `volume_sample()` /
  `particle_fields()`, and next wasm `getVolumes` / `getVolumeDense` /
  `getVolumeNanovdb` / `getVolumeSample` / `getParticleField`.

**Authoring.** `lightusd.volume` provides:
- `inspect_vdb`;
- `create_volume_from_vdb`, which authors an OpenVDBAsset per grid with
  `fieldIndex` for repeated names, `fieldDataType`, `fieldClass` and
  `extent`;
- `compute_extent`;
- `field_paths` / `set_field` / `block_field`;
- `python -m lightusd.volume inspect|create`.

The C entry point is `lightusd_vdb_inspect`.

**Validation.** On top of the existing checks:
- `vol.volume.fieldTargetCount`: exactly one target;
- `vol.volume.fieldTargetType`: same-layer targets must be VolumeFieldBase;
- `vol.particleField.shCount` / `shDegree`.

The capability ledger lists usdVol as a required domain.

---

## UsdRender -- Render Settings

| Schema | Type | Status | Notes |
|--------|------|--------|-------|
| RenderSettings | concrete | stub | Recognized placeholder prim (typed, in pipeline for parse + pprint + roundtrip); schema attrs stay generic (`props`) |
| RenderProduct | concrete | stub | Same placeholder treatment |
| RenderVar | concrete | stub | Same placeholder treatment |
| RenderPass | concrete | next schema | Registry fallbacks/declarations and relationship validation; render-pass execution remains |
| RenderSettingsBase | abstract | -- | |

**Coverage: 0/5 fully typed (3 placeholder-stub)**

---

## Built-in Mesh Import Plugins (non-USD formats)

These are lightusd-specific built-in importers (not OpenUSD schemas). A file
referenced via `references = @model.obj@` is decoded into a `GPrim`/mesh.
Import only -- no writer.

| Format | Source | Status | Notes |
|--------|--------|--------|-------|
| Wavefront OBJ (`usdObj`) | `src/usdObj.{hh,cc}` | partial (import) | Group/object hierarchy flattened to a single mesh; no materials; texcoords/normals expanded to face-varying. Built-in, gated by `LIGHTUSD_USE_USDOBJ`. |
| MagicaVoxel VOX (`usdVox`) | `src/usdVox.{hh,cc}` | partial (import) | Voxel asset import only. |

OBJ TODO (durable): indexed primvars for texcoords/normals, preserve shape
hierarchy, per-face material (GeomSubset), tinyobjloader skin weights, optional
load-time triangulation.

---

## Unsupported Domains (no plans to implement)

The following OpenUSD domains are renderer/DCC-specific and are out of scope for lightusd:

| Domain | Schemas | Reason |
|--------|---------|--------|
| **UsdRi** | StatementsAPI, RiMaterialAPI, RiSplineAPI | RenderMan-specific |
| **UsdHydra** | HydraGenerativeProceduralAPI | Hydra renderer-specific |
| **UsdUI** | Backdrop, NodeGraphNodeAPI, SceneGraphPrimAPI, AccessibilityAPI | DCC UI metadata |

Unsupported schemas are preserved as generic `Model` prims when encountered, so
data is not lost during roundtrip. (UsdProc's `GenerativeProcedural` is now a
recognized placeholder prim — see `core/model-scope.hh` — not a generic Model.)

---

## Summary

| Domain | Concrete | API | Overall |
|--------|----------|-----|---------|
| UsdGeom | 20/20 (100%) | 5/5 (100%) | 25/25 (100%) |
| UsdLux | 12/12 (100%) | 7/7 (100%) | 19/19 (100%) |
| UsdShade | 3/3 (100%) | 4/4 (100%) | 7/7 (100%) |
| UsdSkel | 4/4 (100%) | 1/1 (100%) | 5/5 (100%) |
| UsdPhysics | 8/8 (100%) | 9/9 (100%) | 17/17 (100%) |
| UsdMedia | 1/1 (100%) | 1/1 (100%) | 2/2 (100%) |
| Preliminary AR | 7/7 (100%) | 4/4 (100%) | 11/11 (100%) |
| UsdVol | 4/6 (67%) | 0/11 (0%) | 4/17 (24%) |
| UsdRender | 0/5 (0%; 3 placeholder-stub) | -- | 0/5 (0%) |
| **Total (supported)** | **59/66 (89%)** | **31/42 (74%)** | **90/108 (83%)** |
| Unsupported | -- | -- | 8 schemas (UsdRi, UsdHydra, UsdUI) |

### Remaining gaps

1. **UsdVol** -- further volume-rendering work is intentionally deferred.
2. **UsdRender** -- multi-pass dependency execution and non-color RenderVar AOV
   emission remain. External commands are intentionally never executed.
3. **UsdGeom draw modes** -- `model:cardVisibility` inheritance and face
   selection are implemented; generating complete bounds/origin/cards proxy
   geometry remains a separate draw-mode feature.
