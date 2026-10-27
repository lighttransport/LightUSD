# USD Physics, MuJoCo mjcPhysics, and Newton Schema Reference

This document describes the USD Physics schema, MuJoCo's **mjcPhysics** custom
schema extension, and Newton's USD physics schema as implemented in LightUSD.

## Overview

LightUSD supports three physics schema families:

- **UsdPhysics** (standard) — rigid bodies, collisions, joints, materials, scenes
- **mjcPhysics** (MuJoCo extension) — simulation options, solver params, actuators,
  tendons, keyframes
- **Newton physics** (Newton extension) — solver settings, collision/material
  parameters, mimic joints, and `NewtonActuator`

The custom schemas extend standard `UsdPhysics` (RigidBodyAPI, CollisionAPI,
etc.) with engine-specific attributes. Standard and custom APIs are usually
applied together on the same prims.

### Source Reference

- MuJoCo mjcPhysics schema: `schema.usda` from MuJoCo's `src/experimental/usd/mjcPhysics/`
- Newton schema: newton-usd-schemas 0.6.0 `newton_usd_schemas/generatedSchema.usda`
  (versioned descriptors in `doc/generated/engine-physics-schemas.json`)
- USD Physics schema: OpenUSD `pxr/usd/usdPhysics/`

### PhysicsScene Attributes

Per the official USD Physics schema (`pxr.UsdPhysics.Scene`):

| Attribute | USD Type | LightUSD C++ Type | Description |
|---|---|---|---|
| `physics:gravityDirection` | `vector3f` | `TypedAttribute<value::vector3f>` | Gravity direction unit vector |
| `physics:gravityMagnitude` | `float` | `TypedAttribute<float>` | Gravity magnitude (m/s^2) |

---

## Output profiles

The native OBJ/URDF/MJCF importers (C++, C, Python and both CLIs) author USD
in one of three profiles, selected with `profile=` (Python), `"profile"` in the C
`settings_json`, or `--profile` (CLI). JSON payloads given to
`ConvertURDFJsonToUSDStage` without a `profile` member keep `legacy`.

| | `newton` (default) | `openusd` | `legacy` |
|---|---|---|---|
| Target consumers | Newton 1.8, MuJoCo 3.16 USD decoder, OpenUSD 26.08 | any UsdPhysics consumer | historical output |
| Engine schemas | UsdPhysics + MjcPhysics + Newton (newton-usd-schemas 0.6.0) | UsdPhysics/UsdGeom/UsdShade only; engine-only records omitted with a warning | as before |
| Variability | from generated descriptors | from generated descriptors | historical (several `uniform` mismatches) |
| Revolute/prismatic limits | `physics:lower/upperLimit` | same | also non-standard `physics:limit:<dof>:low/high` |
| World link | static Xform; joints to the world omit `physics:body0` | same | disabled rigid body |
| Sites | Gprims under their body, `MjcSiteAPI` + `NewtonSiteAPI` | under their body | `/World/Sites` |
| Friction | `Material` + `PhysicsMaterialAPI` (static = dynamic = mu0), `newton:torsional/rollingFriction`, `newton:contactStiffness/contactDamping` from geom `solref` (Newton's `solref_to_stiffness_damping`), deduplicated by friction and solref, `material:binding:physics` | `PhysicsMaterialAPI` only | `mjc:geomFriction` |
| Joint dynamics | `NewtonJointAPI` (`newton:armature`, `newton:friction`, `newton:damping` per degree for angular joints, `newton:velocityLimit`); `MjcJointAPI` `ref/springref/margin/stiffness/sol*` | omitted | `mjc:*` + `physxJoint/physxLimit` aliases |
| Inertia | `diagonalInertia` + `principalAxes` + `NewtonMassAPI` `newton:inertia`; MJCF bodies without `<inertial>` get MuJoCo's `inertiafromgeom` result | diagonal + axes | as authored only |
| Multi-joint bodies | stacked joints between the body pair (`newton:jointsAddMobility`) | stacked | massless intermediate links |
| MJCF actuators | `MjcActuator` with MuJoCo's compiled gain/bias/dynamics expansion (Newton maps position/velocity shapes to joint targets) | omitted | `NewtonActuator` PD for joint motors/position/velocity |
| MJCF equalities | connect → `PhysicsSphericalJoint`, weld → `PhysicsFixedJoint` (excluded from articulation, `active` → `jointEnabled`, site forms supported); joint → `NewtonMimicAPI` + `MjcEqualityJointAPI` on the follower (`mimicCoef0` in degrees) | spherical/fixed joints only | `Xform` + `MjcEquality*API` |
| MJCF collision | collides iff `contype|conaffinity != 0`; a visible collider is one prim (collision APIs, look material, `displayColor`; as mujoco-usd-converter, so MuJoCo's decoder reads one geom); colliders in groups 3+ are `purpose = guide`; group 3+ non-colliders become guide visuals | same | group-based split, separate visual and guide collider |
| Mocap / gravcomp | `physics:kinematicEnabled`, `mjc:gravcomp` | kinematic only | `mjc:mocap` |
| URDF `planar` | D6 `PhysicsJoint`, `transX/rotY/rotZ` locked (`low > high`), `transY/transZ/rotX` unlimited (`-inf`/`inf`; Newton creates a D6 DOF only for an axis with a LimitAPI) | same | error |
| URDF effort | `urdf:limit:effort` + `mjc:actuatorfrcrange` (±effort) with `mjc:actuatorfrclimited` | `urdf:limit:effort` | `urdf:limit:effort` |
| MJCF `ref` | `mjc:ref` only (Newton measures joint coordinates from it) | omitted | also `state:*:physics:position` |
| MJCF stacked joints with a ball | intermediate-link chain (Newton merges only hinges/slides into a D6) | stacked | massless intermediate links |
| MJCF `<mesh>` assets with vertices but no faces (MuJoCo's `plugin/sdf/asset/die.obj`) | the convex hull of the OBJ's vertices, as MuJoCo's `mjCMesh::Process` does (faces, volume and inertia from the hull; reported as `mjcf.mesh_convex_hull`); degenerate point sets (colocated / collinear / coplanar) are rejected | same | same |
| MJCF `<flexcomp>`, raw `<flex>` | UsdPhysics deformables (below) | unsupported (reported) | unsupported (reported) |
| MJCF primitive visuals | native `Sphere`/`Cube`/`Cylinder`/`Capsule`/`Plane` (as mujoco-usd-converter), material bound, `displayColor`/`displayOpacity` from geom `rgba` | same | tessellated meshes |
| MJCF ellipsoids (non-uniform) | mesh with the radii baked in (convex hull as a collider): a scaled `Sphere` is read as a unit sphere by Newton and rejected by OpenUSD | same | scaled `Sphere` (reported) |
| URDF ghost / dummy-inertia links | urdf-usd-converter rules: inertia-, visual- and collision-less links on fixed joints (or the root) lose `RigidBodyAPI`, their fixed joint is dropped and child joints re-anchor on the nearest rigid ancestor (or the world); an inertial-only childless child on a fixed joint under a geometry-only parent is merged into it | same | as authored |
| MJCF mesh colliders | `newton:maxHullVertices` only for `<mesh maxhullvert>` (unlimited otherwise); `<mesh inertia="shell">` and `shellinertia` geoms → `NewtonMassAPI` `newton:massModel = "shell"`, `legacy`/`exact` → `mjc:inertia`, `convex` implied (mujoco-usd-converter 0.8.0) | approximation only | `newton:maxHullVertices = 64`, `mjc:inertia = "legacy"` |
| MJCF `type="sdf"` geoms | mesh collider with `NewtonSDFCollisionAPI` (Newton builds the SDF from the mesh; not `NewtonMeshCollisionAPI`) and `physics:approximation = "none"`, `newton:sdfMaxResolution`/`sdfNarrowBand*` matching a precomputed level-set sidecar (see [SDF sidecars and volumetric meshes](#sdf-sidecars-and-volumetric-meshes)) | same collider without Newton APIs or sidecar | mesh collider (convex hull) |
| MJCF SDF plugin meshes (`<mesh><plugin instance>` of `mujoco.sdf.torus/bowl/gear/nut/bolt`) | generated as MuJoCo's compiler does (`mjCMesh::LoadSDF`: the plugin's distance on a ~300-cell grid over its AABB, MarchingCubeCpp), vertex/face counts and masses identical to MuJoCo 3.16; other plugins (`sdflib` on a file mesh needs none) are `mjcf.sdf_plugin_unsupported` | same | same |
| MJCF collision bitmasks | `mjc:contype`/`mjc:conaffinity` (Newton) | `PhysicsCollisionGroup`s under `/World/CollisionGroups`, one per distinct (contype, conaffinity) pair, each filtering the groups MuJoCo's `(ct_a & ca_b) \|\| (ct_b & ca_a)` rule keeps apart; none when every pair collides | `mjc:geomContype/geomConaffinity` |
| MJCF contact margin/gap | `newton:contactMargin`/`newton:contactGap` on every collider, MuJoCo's 0 when unauthored (mujoco-usd-converter) | omitted | only when authored |
| MJCF OBJ meshes | polygons triangulated as MuJoCo 3.16's OBJ decoder (tinyobjloader 2945a96: quads on the shorter diagonal, larger polygons ear-clipped in float; a work limit falls back to fans, `mjcf.obj_polygon_fan`; polygons of more than 100 vertices are dropped as the MuJoCo 3.16 PyPI decoder drops them, `mjcf.obj_polygon_dropped`), so colliders and inferred inertia follow the compiled model | same | tinyobjloader-c triangulation |
| MJCF actuator defaults | MuJoCo 3.16's class slot rules: one actuator slot per class (type = last element written), shortcut parameters from the nearest slot written with the same shortcut or a matching `<general>` (converted back to kp/kv/...), a `<general>` taking a shortcut slot's kind | same | inlined by element tag |
| MJCF default gravity | MuJoCo's `0 0 -9.81` when `<option gravity>` is absent | same | 9.80665 |
| Shell visual meshes | a visual geom of a `<mesh inertia="shell">` / `shellinertia` carries `NewtonMassAPI` `newton:massModel = "shell"` (MuJoCo weighs visual geoms too) | omitted | none |
| Explicit zero mass/inertia | UsdPhysics reads 0 as unspecified (consumers derive it from colliders; Newton's `add_usd` replaces a zero inertia), so an MJCF `<inertial>` zero tensor with a positive mass is authored as `1e-12` principal moments (`diagonalInertia` and `newton:inertia`), and `mass="0"` on a body with colliders as `physics:mass = 1e-12` (above MuJoCo's 1e-15 minimum) | unauthored | unauthored |
| Solver iterations | MJCF `<option iterations>` → `newton:maxSolverIterations` | omitted | `-1` |
| Articulation self-collision | `newton:selfCollisionEnabled` only when the source sets it (an authored value overrides `add_usd(enable_self_collisions=...)`) | omitted | always `true` |
| URDF `<transmission>` | `urdf:transmission:name/type/actuator/hardwareInterface/mechanicalReduction` on the joint; no actuator (as urdf-usd-converter; Newton's `add_usd` rejects actuators on joints that are not single-DOF) | same | zero-gain `NewtonActuator` PD |
| MJCF `<flag>` | `newton:gravityEnabled` follows `gravity`; `filterparent="disable"` sets `physics:collisionEnabled` on joints; `spring`/`damper` flags authored | flags only | flags only |
| Deprecated `mjc:*` aliases | not written: `mjc:option:timestep` (only when `newton:timeStepsPerSecond` cannot hold the timestep), `mjc:option:iterations`, `mjc:flag:gravity`, `mjc:margin/gap`, `mjc:maxhullvert`, `mjc:target` on mimic followers, `mjc:timestep` (MuJoCo 3.16 reads the `newton:*` forms) | omitted | written |
| MJCF materials | `diffuseColor` = linearized `rgba` (default white), `roughness = 1 - shininess` unless `roughness` is set | same | raw `rgba` |
| Source names | `displayName` keeps a link/joint/geom/material/tendon/actuator name that sanitizing or uniquifying changed | same | none |
| URDF visuals | native `Cube`/`Sphere`/`Cylinder`; non-standard `<capsule radius length|height>` → `Capsule` | same | tessellated meshes; capsule unsupported |
| URDF extensions | unknown attributes and elements on the robot (`/World/custom`), links, joints, visuals and collisions → `urdf:<attribute>` strings, `urdf:text` and child `Scope`s (urdf-usd-converter's undefined-element layout) | same | dropped |
| URDF without joints | no articulation root (a lone rigid body) | same | articulation root |
| URDF inputs Newton's `add_urdf` accepts | zero-size primitives and unsupported mesh formats are skipped (`urdf.geometry_unsupported`); negative MTL `Ns` is clamped to 0; mimic chains resolve to the leading joint; explicit zero `<mass>`/`<inertia>` use the 1e-12 stand-ins; ghost links are judged by their `<visual>`/`<collision>` elements (urdf-usd-converter) | same | errors / as authored |
| OBJ stage | `metersPerUnit = 1`, root `kind = component` | same | no units |

MJCF spatial tendons follow MuJoCo's USD decoder and mujoco-usd-converter:
ordered sites, wrap geoms and pulleys become `mjc:path` + `mjc:path:indices`,
`mjc:path:segments` (a pulley, including a leading one, starts a new segment)
and `mjc:path:divisors` (segment 0 has divisor 1), with `mjc:sideSites` +
`mjc:sideSites:indices` (-1 without a side site). Wrap geoms stay native
`Sphere`/`Cylinder` prims under their body (visual-only wraps without
`PhysicsCollisionAPI`); other wrap types are reported and the tendon skipped,
as is any actuator whose target was not exported. A one-value `springlength`
is written as two values. Through Newton's `add_usd` + `SolverMuJoCo`, tendon
counts, wrap counts and `tendon_length0` match MuJoCo compiling the MJCF.

`<replicate>` is expanded as MuJoCo's reader does (accumulated offset in the
previous copy's frame, `i * euler`, suffix `sep` + zero-padded index, inner
suffixes first) and elements referencing replicated names in `<tendon>`,
`<actuator>`, `<sensor>`, `<equality>` and `<contact>` are replicated with
them. `<composite type="cable">` becomes MuJoCo's body chain (capsule/
cylinder/box segments on ball joints in a parallel-transported frame,
`initial` free/ball/none, `B_first`/`B_<i>`/`B_last`, `S_first`/`S_last`,
neighbour excludes; at most 512 vertices so body traversal stays
stack-safe); the cable elasticity plugin becomes ball-joint stiffness E·I/L toward the initial shape (exact for bending, twist approximated; reported).
Both match MuJoCo's compiled body poses on MuJoCo's sample models.

In the `newton` profile, MJCF `<flexcomp>` of every type (`grid`, `circle`,
`square`, `disc`, `direct`, `box`, `cylinder`, `ellipsoid`, `mesh` from OBJ
or STL, `gmsh` 4.1/2.2 ASCII or binary (MuJoCo 3.16's GMSH decoder layouts,
including fTetWild's binary 2.2); MuJoCo's point/element generation including
its vertex order, pins, pose, scaling and unused-point removal) becomes one
soft body under
`/World/Deformables` in the AOUSD UsdPhysics deformable layout Newton reads:

| flex dim | Prim | Simulation / material API | Notes |
|---|---|---|---|
| 1 | `BasisCurves` (`linear`, `periodic` for circles) | `PhysicsCurvesDeformableSimAPI` / `PhysicsCurvesDeformableMaterialAPI` | edge stiffness k and damping c → `curvesStretchStiffness = k L`, `newton:curvesStretchDamping = c L`; no bend/twist |
| 2 | `Mesh` (triangles) | `PhysicsSurfaceDeformableSimAPI` / `PhysicsSurfaceDeformableMaterialAPI` | `surfaceStretchStiffness = E h / (1 - nu^2)` and `surfaceBendStiffness = E h^3 / (12 (1 - nu^2))` only for the `elastic2d` modes MuJoCo applies |
| 3 | `TetMesh` (right-handed tets + boundary `surfaceFaceVertexIndices`) | `PhysicsVolumeDeformableSimAPI` / `PhysicsVolumeDeformableMaterialAPI` | Young's modulus and Poisson ratio |

Masses are `physics:masses` per point (flexcomp `mass / npnt`), thickness is
`physics:thicknesses` (2D elastic thickness, otherwise the contact diameter),
the material is bound with `material:binding:physics`, and
`contype|conaffinity != 0` applies `PhysicsCollisionAPI`. Pinned vertices
become one `PhysicsAttachment` (`type0 = point`, `type1 = xform`, `coords1` in
the owning body frame, no `src1` for the world); Newton imports attachments
only for cables. Non-`full` `dof`, plugins, 2D/3D damping and edge equality
are reported as approximated or dropped. Shells carry `physics:restBendAnglesDefault =
restShape` (MuJoCo bends relative to the initial shape). On MuJoCo's flex sample models the vertices
match MuJoCo's compiled `flexvert_xpos` and the element counts match;
Newton imports them as particles, triangles, tetrahedra and cable rods.

MJCF `<skin>` (in `<asset>` or `<deformable>`, inline `vertex`/`face`/
`texcoord` + `<bone>`s or a binary `.skn` file) becomes UsdSkel in the
`newton` and `openusd` profiles: a `SkelRoot` `/World/Skins/<name>` with a
`Skeleton` (one root joint per bone body; `bindTransforms` from
`bindpos`/`bindquat`, `restTransforms` the bodies' initial world poses, a
`world` bone at the identity) and the skinned `Mesh` (`SkelBindingAPI`,
`skel:skeleton`, per-vertex `jointIndices`/`jointWeights` normalized as
MuJoCo's compiler does, `displayColor` from `rgba`, its material bound).
MuJoCo's linear blend skinning is UsdSkel's, so the rest pose equals
MuJoCo's skinned vertices at qpos0 (`mjv_updateScene`, to float precision on
MuJoCo's flag and cube_skin models). `inflate` (an offset along the skinned
normals UsdSkel cannot express) is baked into the bind points so the rest
pose stays exact. LightUSD extension (`newton` only; `openusd` omits it):
`rel lightusd:mjcSkin:bodies` on the Skeleton targets the bone bodies' prims
in joint order, for consumers that drive the joints from the simulation,
plus `lightusd:mjcSkin:group` and `lightusd:mjcSkin:inflate` (already baked).
`legacy` keeps reporting `<skin>`.

Mesh assets also read MuJoCo's legacy binary `.msh` meshes and GMSH `.gmsh`
files (a 2D file's triangles, a 3D file's tetrahedral boundary, as MuJoCo
3.16's GMSH decoder extracts it).

Raw `<deformable><flex body=... vertex=... element=...>` (what `mj_saveXML`
writes for a flexcomp) converts the same way. A vertex body that is only a
point mass (three independent slide joints or a free joint, an inertial,
nothing else, and not named by any other element) is absorbed: its mass
becomes the vertex's `physics:masses` entry and its link and joints are not
written. `world` and joint-less world children pin their vertex to the
world; any other body (for example one carrying a tendon site) stays a rigid
body and pins the vertex to it, with one `PhysicsAttachment` per body
(`<flex>_pins`, `<flex>_pins_1`, ...). Pinned vertices get the mean free
vertex mass, so engines that derive a density see the uniform body flexcomp
would give. Vertex joint damping is dropped with a warning. Flexes with
`node`/`dof` interpolation, models with `qpos` keyframes (absorbing joints
would shift them), invalid references and flexes without a free vertex are
reported as unsupported. MuJoCo-saved grid flexes (2D cloth on a holder body,
3D jelly, 1D rope pinned to the world) match `flexvert_xpos`, and convert to
the same points, elements, masses, pins and links as the original flexcomp;
Newton imports both alike.

MJCF visuals: capsule meshes have hemispherical caps (previously plain
cylinders), cameras default to MuJoCo's 45 degree `fovy` and take
`clippingRange` from `<visual><map znear zfar>` times an authored
`<statistic extent>`, lights carry `intensity` and spot `exponent`
(`inputs:shaping:focus`), and material `texrepeat` becomes a
`UsdTransform2d` scale on the diffuse texture.

Builtin `type="2d"` textures (`checker`, `gradient`, `flat`, with `edge`,
`cross` or `random` marks) that a material uses are generated as PNG,
pixel-identical to MuJoCo's `tex_data`, at most 4096x4096 and charged to the
import budget. The stage authors `textures/builtin_<name>.png`;
`Stage.save` (C `lightusd_stage_save`) and the `urdf-to-usd` CLI write the
file beside a USDA/USDC layer, USDZ packages it, and
`lightusd.asset.save_component` copies it into `Payload/Textures`.
`Stage.generated_assets()` (C `lightusd_stage_generated_asset*`) returns the
bytes. The first builtin `type="skybox"` texture (the one MuJoCo renders)
becomes a `DomeLight` under `/World/Lights` with a latitude-longitude
`inputs:texture:file` (`inputs:texture:format = "latlong"`): the six faces
of MuJoCo's `BuiltinCube` (pixel-identical to `tex_data`) are resampled at
4w x 2w (at most 4096 x 2048) with nearest-texel cube-map addressing; row 0
is the pole and u = 0.5 faces +Z, as Storm reads a dome texture. The light
is rotated so its +Y pole is the stage's +Z, which is where MuJoCo puts the
cube's +Y. A 512-pixel Menagerie gradient sky gives a 348 KB PNG in about
70 ms. Note that a DomeLight also lights the scene; MuJoCo's skybox is only
a background. Cube textures on materials (there is no UsdPreviewSurface cube
projection), file-based skyboxes, invalid sizes and the `legacy` profile are
reported as `mjcf.texture_builtin_unsupported`; unused builtin textures are
skipped.

Profile-independent fixes also changed `legacy` output for affected inputs:
MuJoCo mesh-fit primitives (`mesh=` on a primitive type) are sized from the
mesh's equivalent inertia box, `limited="false"`/`autolimits="false"` are
honored, a downward `zaxis` rotates about X as in MuJoCo, joint axes and light
directions inside `<frame>` are rotated, class orientations no longer override
an element's own orientation, the root `<default>` is class `main`, and
`UsdUVTexture.inputs:st` is `float2`.

Inferred MJCF inertia ports MuJoCo's compiler: geoms in `inertiagrouprange`
(visual or not) with `density` (default 1000) or `mass`; analytic primitives
(including shell inertia and `fromto`); meshes by MuJoCo's tetrahedral
integration in `legacy` (default), `exact` or `shell` mode (`convex` uses the
exact volume and is reported); MuJoCo only reads the first object of an OBJ
mesh asset. Across Menagerie revision `cda5236f`, 1,226 of 1,237 bodies match
MuJoCo 3.16's compiled mass and principal inertia (rtol 1e-3/2e-3); the
remainder are thin open `ms_human_700` rib surfaces within 1.2%.

Engine schema descriptors are generated from newton-usd-schemas 0.6.0 and
MuJoCo 3.16.0 (`scripts/generate-engine-schema-manifest.py` →
`doc/generated/engine-physics-schemas.json` →
`scripts/generate-checker-definitions.py`). The validator checks authored
type and variability against them (`core.schema.attributeType`,
`core.schema.variability`) and flags non-standard multiple-apply names
(`physics.property.nonStandardName`).

Downstream checks (optional, see [testing](testing-cpp.md)):
`tests/next/run-newton-compare.py` compares Newton's `add_mjcf` with Newton's
`add_usd` of the converted file (MuJoCo arbitrates mass/COM/inertia/collider
differences); `tests/next/run-openusd-physics-check.py` runs every OpenUSD
`UsdValidation` validator and the UsdPhysics parser on each profile.

## SDF sidecars and volumetric meshes

LightUSD computes signed distance fields and tetrahedral meshes with the
vendored tinyvdb (`src/external/tinyvdb`, see its `UPSTREAM.txt`), on the CPU.

**Level sets.** `tvdb_mesh_to_level_set` builds a sparse narrow-band level
set (float 5/4/3 tree, class `level set`, negative inside):
- Distances are exact closest-face distances.
- Signs come from generalized winding numbers (|w| > 1/2), so open,
  inside-out and inconsistently oriented meshes still get a consistent
  inside.
- The interior is flood-filled; output is identical for any thread count.
- It matches brute-force distances to 1e-7. OpenVDB's `meshToLevelSet` agrees
  in sign and to within 0.3 voxel (OpenVDB propagates and renormalizes
  distances away from the polygons), and libopenvdb reads the files back.

**MJCF `type="sdf"` geoms** (newton profile, `sdf="auto"`; `openusd` and
`legacy` author none). Each collider mesh gets a level set in its own frame:
- **Size:** `sdf_resolution` voxels (default 64) across the mesh's largest
  extent and `sdf_band` voxels (default 3) on each side. Geoms sharing a mesh
  asset share one file.
- **Sidecar:** written as the generated asset `sdf/<mesh>.vdb`. USDA/USDC saves
  write it beside the layer, and USDZ saves package it.
- **Collider:** `newton:sdfMaxResolution` and, in an unscaled frame,
  `newton:sdfNarrowBandInner/Outer`, so Newton's own (CUDA) SDF build matches
  the sidecar.
- **Volume:** a `Volume` `<collider>_sdf` beside the collider, with the same
  transform and `purpose = "guide"`. Its `OpenVDBAsset` field `sdf` has
  `fieldName = "sdf"`, `fieldClass = "levelSet"` and `filePath` the sidecar.
- **LightUSD extension:** `rel lightusd:sdf` from the collider to that Volume.

Newton reads the parameters (it cannot load a precomputed SDF from USD); the
Volume is for consumers that can.

**Volumetric soft bodies.** Isosurface stuffing (Labelle and Shewchuk 2007)
tetrahedralizes a level set's interior:
- It uses a BCC lattice, warps lattice vertices near the surface, and applies
  sign stencils with minimum-key diagonals.
- Tets are positively oriented and the boundary is watertight and outward.
- A unit sphere at spacing 0.1 gives dihedral angles of 23 to 133 degrees.
- **Flexcomp opt-in:** `flex_tetrahedralize="stuffing"` (spacing
  `flex_tet_spacing`, default extent / 12) replaces MuJoCo's fan of every
  surface triangle to `origin` for `dim=3 type=mesh` flexcomps
  (`mjcf.flex_tetrahedralized`). Pins given by vertex id are dropped
  (`mjcf.flex_pin_unmapped`).
- **Experimental grading:** set `flex_tet_coarse_spacing` to a value at least
  `flex_tet_spacing` to grade stuffing for a mesh flexcomp or a VDB flexcomp.
  This path is serial and CPU-only; small features can be missed and element
  quality is not guaranteed. The importer limits the background by its memory
  budget. For non-VDB meshes, also select `flex_tetrahedralize="stuffing"`;
  the generated level-set band is widened to support the requested grading.
  A preexisting VDB with a band too narrow to reach the requested coarse size
  produces `mjcf.flex_graded_narrow_band` and retains its stored distances.
  `flex_tet_surface_guard=True` also refines cells that may contain a surface;
  it can increase memory and runtime substantially. Its conservative bound
  assumes a 1-Lipschitz SDF, which interpolated grids may not satisfy, and it
  cannot guarantee recovery of features below the fine spacing.
- The vendored TinyVDB C API can measure a conservative one-sided boundary
  distance for an exact signed-distance sampler. It subdivides each output
  triangle once, samples the four subtriangle centroids, and adds each
  centroid's maximum distance to its subtriangle vertices. This bounds output
  boundary-to-zero-set distance only; it does not bound missing components or
  the reverse surface-to-mesh distance, and it is not exposed by the Python
  volumetric API. Interpolated VDB samples may not meet the exact SDF assumption.
- **Bunny example:** the fan overlaps on non-star-shaped meshes. On MuJoCo's
  bunny its tets sum to 125% of the mesh volume, while stuffing converges from
  below: 94% at spacing extent/12, 97% at extent/24.
- **Newton:** imports the result as particles and tets.

**`.vdb` inputs** (LightUSD extension, `mjcf.vdb_mesh_extension`; MuJoCo has
no VDB meshes):
- MJCF `<mesh file="x.vdb">` takes the first level-set grid, meshed at its zero
  isovalue.
- A flexcomp `type="mesh" file="x.vdb" dim="3"` is stuffed directly (spacing
  default 4 voxels).
- MJCF VDB assets are decoded with the remaining import memory budget, and
  their decoder peak is charged before meshing or tetrahedralization. Voxel,
  background-tet and output-tet counts are also derived from the remaining
  budget without minimum floors. These count estimates do not account exactly
  for every downstream temporary or generated level set.

**APIs.**
- **C** (`lightusd-import-c.h`): `lightusd_mesh_to_level_set_vdb`,
  `lightusd_level_set_vdb_info` / `_sample` / `_to_mesh` / `_to_tet_mesh`, and
  `lightusd_stage_add_generated_asset`.
- **Python** (`lightusd.volumetric`): `mesh_to_level_set`,
  `LevelSet.sample/to_mesh/to_tet_mesh/save`, `read_level_set`,
  `add_mesh_sdf(stage, mesh_path)` (the importer's layout on any Mesh prim)
  and `make_volume_deformable(stage, mesh_or_volume)` (a `TetMesh` with
  `PhysicsVolumeDeformableSimAPI`, per-point masses and a bound
  `PhysicsVolumeDeformableMaterialAPI` material).
- **CLI:** `python -m lightusd.volumetric IN -o OUT --sdf MESH --deformable
  PRIM`, and `urdf-to-usd --sdf auto|off --sdf-resolution N --sdf-band V
  --flex-tetrahedralize mujoco|stuffing --flex-tet-spacing H`.

**Devices.** Level sets and stuffing run on the CPU by default. A build with
`-DLIGHTUSD_TINYVDB_GPU=ON` adds tinyvdb's `tinyvdb_gpu_compute`. It loads the
CUDA driver + NVRTC or HIP + hiprtc at runtime, so building needs no GPU SDK.
For pip builds, set `LIGHTUSD_CMAKE_ARGS=-DLIGHTUSD_TINYVDB_GPU=ON`.
- **What runs on the device:** the per-voxel distances and winding numbers,
  and the stuffing lattice samples. The host keeps the sparse bookkeeping.
- **Results:** level sets have the same active voxels and distances as the
  CPU's, with signs that can differ only on voxels on the surface itself
  (|d| about 1e-8). Stuffing meshes are identical.
- **Selecting a device:**
  - `volumetric_device` (import option, Python keyword, `--volumetric-device`)
    and `device` (C settings, `lightusd.volumetric` keywords) take `cpu`,
    `cuda`, `hip` or `auto`. `auto` means CUDA, then HIP, then the CPU.
  - An unavailable device falls back to the CPU in importers, with the
    warning `asset.volumetric_device_unavailable`.
  - In the APIs an unavailable device is an error: `LIGHTUSD_ERR_UNSUPPORTED`,
    or `UsdError` in Python. `lightusd_volumetric_device_available` and
    `lightusd.volumetric.device_available` report where a device name runs.
- **Speed:** on an RX 9070 XT (HIP) against 8 CPU threads, level sets of 1M to
  5M candidate voxels are 24 to 32 times faster (8.2 s to 0.34 s for a
  24k-face mug at 160 voxels across). Stuffing is 3.5 times faster.

## Newton Physics Implementation

LightUSD recognizes the Newton API schemas listed in
`newton_usd_schemas/generatedSchema.usda`:

- Scene APIs: `NewtonSceneAPI`, `NewtonXpbdSceneAPI`, `NewtonKaminoSceneAPI`
- Rigid/collision/material APIs: `NewtonArticulationRootAPI`,
  `NewtonCollisionAPI`, `NewtonMeshCollisionAPI`, `NewtonMaterialAPI`
- Joint API: `NewtonMimicAPI`
- Actuator APIs: `NewtonActuatorDelayAPI`, `NewtonPDControlAPI`,
  `NewtonPIDControlAPI`, `NewtonNeuralControlAPI`,
  `NewtonMaxEffortClampingAPI`, `NewtonDCMotorClampingAPI`,
  `NewtonPositionBasedClampingAPI`, plus their base marker APIs

### Typed vs. Generic Storage

LightUSD has typed C++ storage for the Newton data that maps cleanly onto the
existing physics prim model:

| Schema | LightUSD representation |
|---|---|
| `NewtonSceneAPI` | `PhysicsScene::newtonScene` |
| `NewtonXpbdSceneAPI` | `PhysicsScene::newtonXpbdScene` |
| `NewtonKaminoSceneAPI` | `PhysicsScene::newtonKaminoScene` |
| `NewtonMimicAPI` | `PhysicsJointBase::newtonMimic` |
| `NewtonActuator` | Concrete `NewtonActuator` prim |

Newton collision, mesh-collision, material, and articulation-root APIs are
recognized in `apiSchemas`; their `newton:*` attributes are preserved in each
host prim's generic `props` map. This avoids a broad applied-API storage
redesign while still round-tripping authored Newton assets.

### Round-Trip Behavior

USDA and USDC parsing preserve Newton `apiSchemas` as known schemas, not as
unknown-schema fallbacks. The reconstruct path consumes typed Newton scene,
mimic, and actuator properties into their C++ structs; `sconv-physics.cc`
re-emits those fields during USDC writing so they do not disappear when the
generic property map is empty.

### Newton USDA Examples

```usda
def PhysicsScene "PhysicsScene" (
    prepend apiSchemas = ["NewtonSceneAPI"]
)
{
    vector3f physics:gravityDirection = (0, 0, -1)
    float physics:gravityMagnitude = 9.81
    uniform int newton:maxSolverIterations = 100
    uniform int newton:timeStepsPerSecond = 500
    bool newton:gravityEnabled = true
}
```

```usda
def Mesh "Collider" (
    prepend apiSchemas = [
        "PhysicsCollisionAPI",
        "PhysicsMeshCollisionAPI",
        "NewtonCollisionAPI",
        "NewtonMeshCollisionAPI"
    ]
)
{
    uniform token physics:approximation = "convexHull"
    float newton:contactMargin = 0.005
    float newton:contactGap = 0
    uniform int newton:maxHullVertices = -1
}
```

```usda
def PhysicsRevoluteJoint "Follower" (
    prepend apiSchemas = ["NewtonMimicAPI"]
)
{
    rel newton:mimicJoint = </Leader>
    float newton:mimicCoef0 = 0
    float newton:mimicCoef1 = -1
}
```

```usda
def NewtonActuator "FingerDrive" (
    prepend apiSchemas = ["NewtonPDControlAPI", "NewtonMaxEffortClampingAPI"]
)
{
    rel newton:targets = </World/Joint1>
    float newton:kp = 120
    float newton:kd = 4
    float newton:maxEffort = 30
}
```

---

> **MuJoCo 3.16 deprecations.** MuJoCo's USD decoder (3.16) warns on these
> MjcPhysics attributes and prefers the Newton forms; the native importers
> write the Newton form in the `newton` profile and keep the MjcPhysics alias
> only in `legacy`: `mjc:option:timestep` → `newton:timeStepsPerSecond`
> (kept when the integer cannot hold the timestep), `mjc:option:iterations` →
> `newton:maxSolverIterations`, `mjc:flag:gravity` → `newton:gravityEnabled`,
> `mjc:margin`/`mjc:gap` → `newton:contactMargin`/`newton:contactGap`,
> `mjc:maxhullvert` → `newton:maxHullVertices`, `mjc:shellinertia` and shell
> `mjc:inertia` → `newton:massModel`, `mjc:armature`/`damping`/`frictionloss`
> → `NewtonJointAPI`, `mjc:torsionalfriction`/`rollingfriction` →
> `newton:torsionalFriction`/`rollingFriction`, equality `mjc:target`/`coef0`/
> `coef1` → `newton:mimicJoint`/`mimicCoef0`/`mimicCoef1`. `mjc:inertia` is
> still written for `legacy`/`exact` meshes (MuJoCo would otherwise infer
> `convex`).

## 1. MjcSceneAPI — Global Simulation Options

Applied to `UsdPhysicsScene` prims. Maps to MJCF `<option>`, `<option/flag>`, and `<compiler>`.

### 1.1 Simulation Options (`mjc:option:*`)

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:option:timestep` | double | 0.002 | Simulation timestep in seconds |
| `mjc:option:impratio` | double | 1.0 | Ratio of frictional-to-normal constraint impedance |
| `mjc:option:wind` | double3 | (0,0,0) | Velocity vector of medium (wind) |
| `mjc:option:magnetic` | double3 | (0,-0.5,0) | Global magnetic flux |
| `mjc:option:density` | double | 0.0 | Density of medium |
| `mjc:option:viscosity` | double | 0.0 | Viscosity of medium |
| `mjc:option:o_margin` | double | 0.0 | Contact override margin |
| `mjc:option:o_solref` | double[] | [0.02, 1.0] | Contact override solver reference |
| `mjc:option:o_solimp` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Contact override solver impedance |
| `mjc:option:o_friction` | double[] | [1.0, 1.0, 0.005, 0.0001, 0.0001] | Contact override friction |
| `mjc:option:integrator` | token | `euler` | Numerical integrator (`euler`, `rk4`, `implicit`, `implicitfast`) |
| `mjc:option:cone` | token | `pyramidal` | Contact friction cone type (`pyramidal`, `elliptic`) |
| `mjc:option:jacobian` | token | `auto` | Constraint Jacobian type (`auto`, `dense`, `sparse`) |
| `mjc:option:solver` | token | `newton` | Constraint solver algorithm (`pgs`, `cg`, `newton`) |
| `mjc:option:iterations` | int | 100 | Max solver iterations |
| `mjc:option:tolerance` | double | 1e-8 | Solver early-termination tolerance |
| `mjc:option:ls_iterations` | int | 50 | Max linesearch iterations (CG/Newton) |
| `mjc:option:ls_tolerance` | double | 0.01 | Linesearch tolerance |
| `mjc:option:noslip_iterations` | int | 0 | Max Noslip solver iterations |
| `mjc:option:noslip_tolerance` | double | 1e-6 | Noslip solver tolerance |
| `mjc:option:ccd_iterations` | int | 35 | Max convex collision iterations |
| `mjc:option:ccd_tolerance` | double | 1e-6 | Convex collision tolerance |
| `mjc:option:sdf_iterations` | int | 10 | SDF collision iterations per initial point |
| `mjc:option:sdf_initpoints` | int | 40 | Number of SDF starting points |
| `mjc:option:actuatorgroupdisable` | int[] | [] | List of actuator groups to disable |

### 1.2 Simulation Flags (`mjc:flag:*`)

All are `bool` type. Default is `True` unless noted.

| USD Attribute | Default | Description |
|---|---|---|
| `mjc:flag:constraint` | True | Enable constraint solver |
| `mjc:flag:equality` | True | Enable equality constraints |
| `mjc:flag:frictionloss` | True | Enable friction loss constraints |
| `mjc:flag:limit` | True | Enable joint/tendon limit constraints |
| `mjc:flag:contact` | True | Enable collision detection and contact constraints |
| `mjc:flag:spring` | True | Enable joint and tendon springs |
| `mjc:flag:damper` | True | Enable joint and tendon dampers |
| `mjc:flag:gravity` | True | Enable gravity |
| `mjc:flag:clampctrl` | True | Clamp control inputs to actuator ranges |
| `mjc:flag:warmstart` | True | Warm-start constraint solver from previous step |
| `mjc:flag:filterparent` | True | Filter parent-child contact pairs |
| `mjc:flag:actuation` | True | Enable actuator forces |
| `mjc:flag:refsafe` | True | Prevent instabilities from small solref[0] |
| `mjc:flag:sensor` | True | Enable sensor computations |
| `mjc:flag:midphase` | True | Enable mid-phase collision filtering (AABB BVH) |
| `mjc:flag:nativeccd` | True | Use native convex collision pipeline |
| `mjc:flag:eulerdamp` | True | Implicit damping in Euler integrator |
| `mjc:flag:autoreset` | True | Auto-reset on numerical issues |
| `mjc:flag:island` | True | Enable constraint island discovery |
| `mjc:flag:override` | **False** | Enable contact override mechanism |
| `mjc:flag:energy` | **False** | Compute potential/kinetic energy |
| `mjc:flag:fwdinv` | **False** | Compare forward/inverse dynamics |
| `mjc:flag:invdiscrete` | **False** | Discrete-time inverse dynamics |
| `mjc:flag:multiccd` | **False** | Multi-contact CCD for convex pairs |

### 1.3 Compiler Settings (`mjc:compiler:*`)

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:compiler:autoLimits` | bool | True | Infer limit flags from range attributes |
| `mjc:compiler:boundMass` | double | 0.0 | Lower bound on body mass |
| `mjc:compiler:boundInertia` | double | 0.0 | Lower bound on diagonal inertia |
| `mjc:compiler:setTotalMass` | double | -1.0 | Scale all masses so total equals this value (disabled if negative) |
| `mjc:compiler:useThread` | bool | True | Multi-threaded compilation |
| `mjc:compiler:balanceInertia` | bool | False | Auto-fix invalid diagonal inertia |
| `mjc:compiler:angle` | token | `degree` | Angle unit (`degree`, `radian`) |
| `mjc:compiler:fitAABB` | bool | False | Use AABB (vs equivalent-inertia box) for mesh-to-primitive fitting |
| `mjc:compiler:fuseStatic` | bool | False | Fuse static bodies with parent |
| `mjc:compiler:inertiaFromGeom` | token | `auto` | Infer body mass/inertia from geoms (`false`, `true`, `auto`) |
| `mjc:compiler:alignFree` | bool | False | Align free-joint body frame with inertial frame |
| `mjc:compiler:inertiaGroupRange:min` | int | 0 | Min geom group for inertia inference |
| `mjc:compiler:inertiaGroupRange:max` | int | 5 | Max geom group for inertia inference |
| `mjc:compiler:saveInertial` | bool | False | Save explicit inertial clauses for all bodies |

---

## 2. MjcJointAPI — Joint Physics Properties

Applied to `UsdPhysicsJoint` prims alongside standard joint APIs.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:group` | int | 0 | Joint group |
| `mjc:stiffness` | double | 0 | Spring stiffness (equilibrium at `springref`) |
| `mjc:damping` | double | 0 | Velocity-linear damping force |
| `mjc:armature` | double | 0 | Additional rotor/transmission inertia |
| `mjc:frictionloss` | double | 0 | Dry friction loss |
| `mjc:springdamper` | double[] | [0, 0] | Auto-set stiffness/damping from [time_constant, damping_ratio] |
| `mjc:springref` | double | 0 | Spring equilibrium position/angle |
| `mjc:ref` | double | 0 | Reference position/angle for initial config |
| `mjc:margin` | double | 0 | Distance threshold for limit activation |
| `mjc:solreflimit` | double[] | [0.02, 1.0] | Solver reference for joint limits |
| `mjc:solimplimit` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance for joint limits |
| `mjc:solreffriction` | double[] | [0.02, 1.0] | Solver reference for dry friction |
| `mjc:solimpfriction` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance for dry friction |
| `mjc:actuatorfrcrange:min` | double | 0 | Min actuator force clamp (hinge/slide only) |
| `mjc:actuatorfrcrange:max` | double | 0 | Max actuator force clamp (hinge/slide only) |
| `mjc:actuatorfrclimited` | token | `auto` | Enable actuator force clamping (`false`, `true`, `auto`) |
| `mjc:actuatorgravcomp` | bool | false | Add gravity compensation to actuator forces |

---

## 3. MjcCollisionAPI — Collision Geometry Properties

Applied to prims **alongside** `UsdPhysicsCollisionAPI`. Both APIs must be present.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:group` | int | 0 | Collision group |
| `mjc:priority` | int | 0 | Determines how two colliders' properties are combined |
| `mjc:condim` | int | 3 | Contact space dimensionality (max of pair) |
| `mjc:solmix` | double | 1.0 | Weight for averaging contact parameters |
| `mjc:solref` | double[] | [0.02, 1.0] | Solver reference (time constant, damping) |
| `mjc:solimp` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance |
| `mjc:margin` | double | 0.0 | Contact detection distance threshold |
| `mjc:gap` | double | 0.0 | Distance for inactive contacts (for custom computations) |
| `mjc:shellinertia` | bool | False | Compute inertia assuming mass on surface |

### Collision Setup in USD

To make a prim collidable in MuJoCo:

```usda
def Mesh "table" (
    prepend apiSchemas = ["PhysicsCollisionAPI", "MjcCollisionAPI"]
)
{
    # Standard UsdPhysics collision
    bool physics:collisionEnabled = true

    # MuJoCo-specific collision parameters
    int mjc:condim = 3
    double mjc:margin = 0.001
    double[] mjc:solref = [0.02, 1.0]
    double[] mjc:solimp = [0.9, 0.95, 0.001, 0.5, 2.0]
    int mjc:priority = 0
}
```

The contact dimensionality (`condim`) values:
- **1**: Frictionless contact (normal force only)
- **3**: Regular frictional contact (default — normal + 2 tangent)
- **4**: + torsional friction
- **6**: + rolling friction (full)

---

## 4. MjcMeshCollisionAPI — Mesh Collision Properties

Applied to mesh collision prims alongside `UsdPhysicsMeshCollisionAPI`.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:inertia` | token | `legacy` | Mesh inertia computation mode (`legacy`, `convex`, `exact`, `shell`) |
| `mjc:maxhullvert` | int | -1 | Max vertices in convex hull (-1 = unlimited) |

---

## 5. MjcMaterialAPI — Physical Material Properties

Applied alongside `UsdPhysicsMaterialAPI`. Standard friction and restitution come from
`UsdPhysicsMaterialAPI`; these add MuJoCo-specific friction types.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:torsionalfriction` | double | 0.005 | Friction around contact normal |
| `mjc:rollingfriction` | double | 0.0001 | Friction on contact tangent plane (both axes) |

---

## 6. MjcActuator — Force Transmission

Concrete typed prim representing actuators. Applies force to joints, bodies, or sites via relationships.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:group` | int | 0 | Actuator group |
| `mjc:target` | rel | — | Transmission target (joint, body, or site) |
| `mjc:gear` | double[] | [1,0,0,0,0,0] | Length/force scaling vector |
| `mjc:crankLength` | double | 0.0 | Connecting rod length (slider-crank only) |
| `mjc:jointInParent` | bool | False | Define rotation axis in parent frame |
| `mjc:refSite` | rel | — | Reference site for relative measurement |
| `mjc:sliderSite` | rel | — | Slider-crank pin site |
| `mjc:dynType` | token | `none` | Activation dynamics (`none`, `integrator`, `filter`, `filterexact`, `muscle`, `user`) |
| `mjc:gainType` | token | `fixed` | Gain type (`fixed`, `affine`, `muscle`, `user`) |
| `mjc:biasType` | token | `none` | Bias type (`none`, `affine`, `muscle`, `user`) |
| `mjc:dynPrm` | double[] | [1,0,...] | Dynamics parameters (10 elements) |
| `mjc:gainPrm` | double[] | [1,0,...] | Gain parameters (10 elements) |
| `mjc:biasPrm` | double[] | [0,0,...] | Bias parameters (10 elements) |
| `mjc:actDim` | int | -1 | Activation state dimension (-1 = auto) |
| `mjc:actEarly` | bool | False | Use next activation value for force |
| `mjc:ctrlLimited` | token | `auto` | Clamp control input (`false`, `true`, `auto`) |
| `mjc:forceLimited` | token | `auto` | Clamp force output (`false`, `true`, `auto`) |
| `mjc:actLimited` | token | `auto` | Clamp activation state (`false`, `true`, `auto`) |
| `mjc:ctrlRange:min/max` | double | 0 | Control input clamp range |
| `mjc:forceRange:min/max` | double | 0 | Force output clamp range |
| `mjc:actRange:min/max` | double | 0 | Activation clamp range |
| `mjc:lengthRange:min/max` | double | 0 | Feasible transmission length range |
| `mjc:inheritRange` | double | 0 | Auto-set ctrlrange from target range (0 = disabled) |

---

## 7. MjcTendon — Fixed and Spatial Tendons

Concrete typed prim for tendons.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:type` | token | `spatial` | Tendon type (`spatial`, `fixed`) |
| `mjc:path` | rel | — | Sites/geoms (spatial) or joints (fixed) the tendon wraps |
| `mjc:path:indices` | int[] | [] | Ordered indices into unique path targets; repeated visits allowed |
| `mjc:path:coef` | double[] | [] | Joint coefficients (fixed tendons) |
| `mjc:path:segments` | int[] | [] | Segment indices for path points |
| `mjc:path:divisors` | double[] | [] | Segment length divisors |
| `mjc:sideSites` | rel | — | Side sites for geom wrapping direction |
| `mjc:sideSites:indices` | int[] | [] | Index mapping for side sites |
| `mjc:group` | int | 0 | Tendon group |
| `mjc:stiffness` | double | 0.0 | Spring stiffness along tendon |
| `mjc:damping` | double | 0.0 | Velocity-linear damping along tendon |
| `mjc:armature` | double | 0.0 | Inertia for tendon length changes |
| `mjc:frictionloss` | double | 0.0 | Dry friction loss |
| `mjc:springlength` | double[] | [-1, -1] | Spring rest length or dead-band range (-1 = auto) |
| `mjc:limited` | token | `auto` | Enable length limits (`auto`, `true`, `false`) |
| `mjc:range:min/max` | double | 0 | Allowed tendon length range |
| `mjc:margin` | double | 0.0 | Limit activation distance |
| `mjc:solreflimit` | double[] | [0.02, 1.0] | Solver reference for limits |
| `mjc:solimplimit` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance for limits |
| `mjc:solreffriction` | double[] | [0.02, 1.0] | Solver reference for friction |
| `mjc:solimpfriction` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance for friction |
| `mjc:actuatorfrclimited` | token | `auto` | Clamp actuator forces on tendon |
| `mjc:actuatorfrcrange:min/max` | double | 0 | Actuator force clamp range |
| `mjc:width` | double | 0.003 | Rendering radius |
| `mjc:rgba` | color4f | (0.5, 0.5, 0.5, 1.0) | Rendering color |

---

## 8. MjcEqualityAPI — Equality Constraints

Base API applied to constraint prims. Specialized by Connect, Weld, and Joint sub-APIs.

### 8.1 Base (MjcEqualityAPI)

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:target` | rel | — | Secondary constraint target |
| `mjc:solref` | double[] | [0.02, 1.0] | Solver reference |
| `mjc:solimp` | double[] | [0.9, 0.95, 0.001, 0.5, 2.0] | Solver impedance |

### 8.2 MjcEqualityWeldAPI

Inherits MjcEqualityAPI, adds:

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:torqueScale` | float | 1.0 | Scale factor for angular residual (units: torque/force = length) |

### 8.3 MjcEqualityConnectAPI

Inherits MjcEqualityAPI. No additional attributes.

### 8.4 MjcEqualityJointAPI

Inherits MjcEqualityAPI. Constrains one joint as a quartic polynomial of another.

| USD Attribute | Type | Default | Description |
|---|---|---|---|
| `mjc:coef0` | double | 0 | Constant coefficient a0 |
| `mjc:coef1` | double | 1 | Linear coefficient a1 |
| `mjc:coef2` | double | 0 | Quadratic coefficient a2 |
| `mjc:coef3` | double | 0 | Cubic coefficient a3 |
| `mjc:coef4` | double | 0 | Quartic coefficient a4 |

Constraint: `y = y0 + a0 + a1*(x-x0) + a2*(x-x0)^2 + a3*(x-x0)^3 + a4*(x-x0)^4`

---

## 9. MjcSiteAPI / MjcImageableAPI

| Schema | USD Attribute | Type | Default | Description |
|---|---|---|---|---|
| MjcSiteAPI | `mjc:group` | int | 0 | Site group |
| MjcImageableAPI | `mjc:group` | int | 0 | Visual-only entity group |

MjcSiteAPI is applied to UsdGeomSphere, UsdGeomCapsule, UsdGeomCylinder, or UsdGeomCube.
MjcImageableAPI marks entities as strictly visual (contype = conaffinity = 0).

---

## 10. MjcKeyframe — Simulation State Snapshots

| USD Attribute | Type | Description |
|---|---|---|
| `mjc:qpos` | double[] | Joint positions → `mjData.qpos` |
| `mjc:qvel` | double[] | Joint velocities → `mjData.qvel` |
| `mjc:act` | double[] | Actuator activations → `mjData.act` |
| `mjc:ctrl` | double[] | Controls → `mjData.ctrl` |
| `mjc:mpos` | double[] | Mocap body positions → `mjData.mocap_pos` |
| `mjc:mquat` | double[] | Mocap body quaternions → `mjData.mocap_quat` |

---

## URDF / MJCF → USD Physics Conversion

LightUSD converts URDF and MuJoCo MJCF robots into a UsdPhysics + mjcPhysics
(+ Newton) stage along two paths:

- **Native importers** (C++, C ABI, Python `lightusd.import_urdf/import_mjcf`,
  CLI `examples/urdf-to-usd`): `src/tydra/asset-import/` parses the source and
  `src/tydra/next/urdf-to-usd*.cc` writes the stage in the selected
  [output profile](#output-profiles). This is the path described by the rest
  of this document.
- **JSON bridge** (web demo / node CLI): `lightusd::tydra::ConvertURDFJsonToUSDStage`
  (`src/tydra/urdf-to-usd.cc`) consumes a JSON payload produced by the web
  demo (`web/js/urdf.js`) or the node CLI (`web/js/cli/urdf-to-usd.js`), both of
  which parse the source robot and emit per-joint frames (`localPos0/1`,
  `localRot0/1`) and per-link inertials. A payload without a `profile` member
  keeps the `legacy` output. The WASM entry point is
`createURDFPhysicsScene` in `web/binding.cc`; re-importing the stage for the
preview goes back through `extractPhysicsSceneJSON`.

The node CLI treats URDF/MJCF XML as untrusted by default: mesh assets and MJCF
`<include>` files are read only from the input directory, `--asset-dir`, or
`--package-root`. Use `--allow-unsafe-paths` only for trusted legacy XML that
intentionally references files outside those roots.

### Tydra MuJoCo JSON shape

`ConvertURDFJsonToUSDStage` accepts the compact MJCF-derived payload generated by
the web/Node parsers and the richer JSON shape emitted by
`ConvertPhysicsToJson`.

- `MjcActuator` entries may use source-name targets (`targetTendon`,
  `targetJoint`, `targetSite`, `targetBody`) or a direct USD path in `target`.
  The JSON form preserves scalar ranges (`ctrlRange_min/max`,
  `forceRange_min/max`, `actRange_min/max`, `lengthRange_min/max`), transmission
  arrays (`gear`, `dynPrm`, `gainPrm`, `biasPrm`), limit tokens, plugin fields,
  and ref/slider site paths.
- `MjcTendon` entries support fixed-tendon `joints:[{joint,coef}]`, spatial
  `path:[{site|sidesite}]`, or direct exported routing arrays:
  `route`, `sideSites`, `routeIndices`, `sideSiteIndices`, `routeSegments`,
  `routeDivisors`, and `routeCoef`. `route` and `sideSites` accept string
  entries or object entries with `site`, `sidesite`, `target`, or `path` keys;
  partial explicit spatial routes are preserved with a warning instead of being
  dropped.
- `MjcSensor` entries are exported under `sensors` with `type`, `objtype`,
  `objname`, `reftype`, `refname`, `group`, `cutoff`, `noise`, and `user`.
  Import also accepts common MJCF aliases such as `site`, `joint`, `body`,
  `geom`, `tendon`, `actuator`, `camera`, `light`, and `refSite`/`refBody`,
  normalizing them into `objtype`/`objname` and `reftype`/`refname`.
- `PhysicsJsonExportOptions::include_defaults=false` omits unauthored fallback
  fields for MuJoCo joints, actuators, tendons, and sensors; set it to `true`
  when consumers need schema fallback values materialized in JSON.

### Joint type mapping

| Source joint | USD prim | Notes |
|---|---|---|
| URDF `revolute`, `continuous` / MJCF `hinge` | `PhysicsRevoluteJoint` | `continuous` = revolute with no limits |
| URDF `prismatic` / MJCF `slide` | `PhysicsPrismaticJoint` | |
| MJCF `ball` | `PhysicsSphericalJoint` | 3-DOF; cone-angle limits not carried (free rotation) |
| URDF/MJCF `fixed` (or absent joint) | `PhysicsFixedJoint` | welded link |
| MJCF `free` / `floating` base | grounded (fixed root) | the floating DOFs are not emitted as a joint; a non-root `free` joint warns and exports as `PhysicsFixedJoint` |

The joint frame rotation is authored via `physics:localRot0` / `physics:localRot1`
(quaternion, USD `(w, x, y, z)` order). `localRot0` is authoritative; `originMatrix`
in the payload is consumed only for its translation. A joint that carries a
rotation in `originMatrix` but omits `localRot0` warns rather than dropping it
silently.

### Units

- **Revolute limits**: the payload carries radians; the converter writes USD
  `physics:lowerLimit` / `physics:upperLimit` in **degrees** (UsdPhysics
  convention, `× 180/π`). Re-import converts back to radians.
- **Prismatic limits**: meters, unchanged.
- MJCF `<compiler angle="degree|radian">` is honored when parsing source ranges.

### Link layout

`layout="nested"` (Python `import_urdf/import_mjcf(..., layout="nested")`, C
`settings_json` `"layout"`, CLI `--layout nested`) authors each dynamic link
as a child prim of its parent link, following the kinematic tree as
urdf-usd-converter and mujoco-usd-converter do, with parent-relative
transforms. Links attached to the world or to a static link stay directly
under `/World/Links`; joints, sites, tendons, equalities and filters target the
nested paths. World transforms equal the default `flat` layout; OpenUSD's
validators and UsdPhysics parser see the same bodies and shapes, and Newton's
comparison passes on sampled Menagerie models in both layouts.

### URDF root joint

URDF root links are welded to the world (`__world_to_<link>` fixed joint), as
urdf-usd-converter does and as Newton's `add_urdf` defaults. The weld has no
`physics:body0`, so Newton's `add_usd(floating=True)` (or `base_joint=`) still
replaces it with a free (or the chosen) base joint.

### Atomic component layout

`lightusd.asset.save_component(stage, directory, name="Robot")` writes a
converted stage in the layer layout urdf-usd-converter and
mujoco-usd-converter produce:

```
<directory>/Robot.usda              defaultPrim /Robot, kind=component,
                                    assetInfo, payload ./Payload/Contents.usda
<directory>/Payload/Contents.usda   sublayers Physics, Materials, Geometry
<directory>/Payload/Geometry.usda   prims, transforms, meshes and gprims
<directory>/Payload/Materials.usda  Material networks and material:binding
<directory>/Payload/Physics.usda    physics/newton/mjc/urdf API schemas and
                                    attributes as overs; joints, tendons,
                                    actuators, equalities, the PhysicsScene
<directory>/Payload/GeometryLibrary.usda  one Mesh per distinct shape under
                                    /Geometry, referenced by Geometry.usda
<directory>/Payload/Textures/       copied textures (textures="copy")
```

The stage's root prim (`/World`) is renamed to `/<name>` and every
relationship target and connection follows it. API schemas are split per
layer as `prepend` list ops, so the composed prims carry the original list.
Opened in OpenUSD, the interface file composes to the same prims, schemas,
values, connections and targets as the flat output (checked on Menagerie
panda, spot, g1, go2 and the shadow hand). It is a Python helper;
`format="usdc"` writes binary layers.

With `geometry_library=True` (the default) a mesh's shape attributes
(points, topology, normals, UVs, extent, subdivision data) go to one
`GeometryLibrary` prim per distinct shape, which the mesh references; its
transform, colors, purpose, bindings and physics stay in the Geometry and
Physics layers. Meshes shared by visuals and colliders or repeated links are
stored once: go2's 33 meshes need 16 library prims and its USDA component
shrinks from 72.6 to 36.1 MB (g1 94.7 to 60.8 MB). USDC output is about the
same size either way, since the crate format already shares identical arrays.
Materials are not duplicated by the importers, so there is no separate
material library.

Mesh normals and UVs are one value per (welded) point, so the newton and
openusd profiles author them with `vertex` interpolation, which is also what
Newton's `Mesh` stores. The legacy profile keeps its historical expansion to
`faceVarying` for indexed meshes (about six times the data for welded STL).

### Unit conversion

Native imports author SI units (`metersPerUnit = 1`, `kilogramsPerUnit = 1`,
which Newton requires). `lightusd.units.convert_units(stage,
meters_per_unit=..., kilograms_per_unit=..., up_axis=...)` converts a stage in
place, for example into a centimeter scene. Each attribute is scaled by its
length/mass dimension: gprim sizes, points, widths, extents and translations
(L); mass (M), density (M/L^3), diagonal and `newton:inertia` tensors (M L^2);
joint frames, linear limits and linear drive/joint-state targets (L);
linear drive stiffness/damping (M), linear force limits (M L); angular drive
gains and torque limits, NewtonJointAPI armature/damping/friction for revolute
joints (M L^2) with angles kept in degrees; gravity magnitude, Newton
contact distances and SDF narrow bands, and distance-joint limits (L); joint
break force (M L) and torque (M L^2); deformable masses and thicknesses.
OpenVDBAsset fields get an innermost `xformOp:scale:units`, since their
`.vdb` voxels stay in the file's units. Time samples are
converted with defaults. An up-axis change prepends (or accumulates and
removes) `xformOp:rotateX:upAxis` on root prims and rotates
`physics:gravityDirection`. `mjc:*` attributes stay in MuJoCo model units;
DOF quantities whose linear/angular kind is unknown are listed in
`report.skipped`. Converting Menagerie models to cm/g and back reproduces
every value to float precision.

The conversion is native: the C API exposes it as
`lightusd_stage_convert_units(stage, meters_per_unit, kilograms_per_unit,
up_axis, &report, &skipped)` in `lightusd-physics-c.h` (0 / NULL keep the
stage's value), and the Python function calls it. A time-sampled attribute is
scaled sample by sample without authoring a default, and each attribute keeps
its declared type (for example `half3[]`). Converting fruitfly to cm/g takes
0.03 s (the earlier pure-Python pass took 1.4 s). Attributes are rewritten
one at a time (no stage-sized copy), so an error leaves the stage partially
converted; discard it.

### Best-effort and unsupported features

These are converted on a best-effort basis and are reported (import report
diagnostics and the warning string) rather than dropped silently. Rows give the
default `newton` profile; [Output profiles](#output-profiles) lists the
`openusd` and `legacy` differences.

| Feature | Behavior |
|---|---|
| MJCF body with multiple `<joint>` | hinges/slides stack between the same body pair (`newton:jointsAddMobility`, merged by Newton into a D6); a stack with a ball joint becomes a chain through massless intermediate links (`<body>__mjcdof_k`) |
| MJCF `ball` joint | `PhysicsSphericalJoint`; a swing `range` becomes equal `physics:coneAngle0Limit`/`coneAngle1Limit` half-angles (`legacy` reports it) |
| MJCF non-diagonal `fullinertia` | `physics:diagonalInertia` + `physics:principalAxes` (eigen-decomposition) and the full tensor as `NewtonMassAPI` `newton:inertia` |
| URDF `<mimic>` / MJCF joint equality | `NewtonMimicAPI` on the follower (`newton:mimicCoef0` in degrees for angular joints); a single-joint equality keeps `mjc:coef0` and is reported |
| MJCF `<actuator>` | `MjcActuator` with MuJoCo's compiled gain/bias/dynamics expansion; pid/dcmotor/so3 types without an MjcPhysics token are reported and skipped, as is any actuator whose target was not exported |
| URDF `<transmission>` | `urdf:transmission:*` metadata on the joint (no actuator) |
| MJCF `<tendon>` (fixed, spatial) | `MjcTendon` under `/World/Tendons`; spatial routing through sites, native `Sphere`/`Cylinder` wrap geoms and pulleys (see [Output profiles](#output-profiles)); other wrap geom types skip the tendon (`mjcf.tendon_wrap_type`) |
| MJCF `<site>` | a gprim under its body with `MjcSiteAPI` + `NewtonSiteAPI` |
| MJCF `<equality>` connect/weld | `PhysicsSphericalJoint` / `PhysicsFixedJoint` excluded from the articulation (`active` → `physics:jointEnabled`); unsupported equality types are reported (`mjcf.equality_unsupported`) |
| MJCF `<contact><exclude>` / `<pair>` | `PhysicsFilteredPairsAPI` on the first body / an `Xform` under `/World/Contacts` with `mjc:geom1`/`mjc:geom2` tokens and the pair's `condim`/`margin`/`gap`/`friction`/`sol*` (no MjcPhysics schema covers pairs; `openusd` omits it) |
| MJCF `<flexcomp>`, raw `<flex>` | UsdPhysics deformables; reduced flexcomp DOFs are approximated as full-dof and reported, while interpolated raw flexes are skipped and reported |

For converted flexcomps, the importer recognizes MuJoCo's `full`, `radial`,
`trilinear`, `quadratic`, and `2d` DOF values and rejects unknown values. The
non-full modes are approximations; `unsupported_policy="preserve"` retains the
original XML as non-executable source data. Interpolated raw flexes with
`node`/`dof` are skipped under the normal unsupported policy; preserve mode
retains their original `<flex>` XML as a non-executable source record.
| MJCF `<skin>` | UsdSkel `SkelRoot`/`Skeleton`/skinned `Mesh` under `/World/Skins` + `lightusd:mjcSkin:*` extension data (see above) |
| MJCF `<frame scale/massscale>` | the frame's pose is applied, its scale is reported (`mjcf.frame_scale_unsupported`) |
| MJCF mesh paths that leave the model directory (MuJoCo's own `plugin/sdf/mug.xml` uses `../../mug/mug.obj`) | resolve relative to the model file, but only under an allowed asset root: pass `asset_dirs` (Python/C/CLI `--asset-dir`); otherwise reported as `Asset outside allowed roots` | same | same |
| MJCF cube textures on materials | reported (`mjcf.texture_builtin_unsupported`); builtin skyboxes become a latlong `DomeLight` |

---

## LightUSD Implementation Status

### Core (parsing + reconstruction)

| Feature | Status | Files |
|---|---|---|
| UsdPhysics type definitions | Done | `src/usdPhysics.hh` |
| mjcPhysics type definitions | Done | `src/mjcPhysics.hh` |
| Newton type definitions | Done | `src/newtonPhysics.hh` |
| USDA parsing (all concrete prims) | Done | `src/prim-reconstruct-physics*.cc`, `src/usda-reader.cc` |
| USDC parsing | Done | `src/usdc-reader-prim.cc` |
| Pretty-printing (USDA output) | Done | `src/pprint-physics.cc` |
| Tydra JSON export | Done | `src/tydra/physics-to-json.cc` |
| Native URDF/MJCF/OBJ import (payload) | Done | `src/tydra/asset-import/` (`mjcf*.cc` with `mjcf-internal.hh`, `urdf.cc`, `obj.cc`, `dae.cc`, `reader.cc`, `builtin-texture.cc`) |
| Profile-aware USD writer (next core) | Done | `src/tydra/next/urdf-to-usd*.cc` |
| C ABI / Python | Done | `src/c-api/lightusd-import-c.cc`, `lightusd-physics-c.cc`; `python/lightusd/` |
| Physics query / validation | Done | `lightusd_stage_query_physics`, `src/next/validation/usd-validation-physics.cc` |

### API schemas supported

Registered in the `APISchemas::APIName` enum (`src/core/composition-types.hh`):

- **UsdPhysics**: PhysicsRigidBodyAPI, PhysicsCollisionAPI, PhysicsMaterialAPI, PhysicsMeshCollisionAPI, PhysicsMassAPI, PhysicsFilteredPairsAPI, PhysicsArticulationRootAPI; multi-apply: PhysicsDriveAPI, PhysicsLimitAPI
- **mjcPhysics**: MjcSceneAPI, MjcJointAPI, MjcCollisionAPI, MjcMeshCollisionAPI, MjcMaterialAPI, MjcSiteAPI, MjcImageableAPI, MjcEqualityAPI (+ Connect, Weld, Joint variants)
- **Newton**: NewtonSceneAPI, NewtonXpbdSceneAPI, NewtonKaminoSceneAPI, NewtonArticulationRootAPI, NewtonCollisionAPI, NewtonMeshCollisionAPI, NewtonMaterialAPI, NewtonMimicAPI, and the actuator control/clamping APIs

### Concrete prim types

(`ReconstructPrim<T>` specializations exist for all of these)

- **UsdPhysics**: PhysicsScene, PhysicsJoint (generic D6), PhysicsRevoluteJoint, PhysicsPrismaticJoint, PhysicsSphericalJoint, PhysicsFixedJoint, PhysicsDistanceJoint, PhysicsCollisionGroup (8)
- **mjcPhysics**: MjcActuator, MjcTendon, MjcKeyframe, MjcSensor (4)
- **Newton**: NewtonActuator (1)

### Test coverage

- ~30 unit tests in `tests/unit/unit-physics.cc` covering the concrete prim types, UsdPhysics/mjcPhysics/Newton API schemas, pprint roundtrip, and Tydra JSON export
- `tests/usda/physics-*.usda` synthetic test files for USDA/USDC roundtrip
- Blender export roundtrip test asset: `models/blender-physics.usda`
- Native importers: `python/tests/` (import, profiles, soft bodies, tendons,
  textures, components), `tests/next/` CTest, the `fuzz_next_import`
  libFuzzer harness
- Downstream checks (optional): `tests/next/run-newton-compare.py` (Newton
  `add_mjcf`/`add_urdf` vs `add_usd`: Menagerie 69/69, 135 URDFs),
  `tests/next/run-mujoco-usd-compare.py` (MuJoCo's compile of the MJCF vs its
  USD decoder's compile of LightUSD's stage, Menagerie 68/69, needs a
  `MUJOCO_WITH_USD` build and `tests/next/mujoco-model-dump.cc`) and
  `tests/next/run-openusd-physics-check.py` (OpenUSD 26.08 UsdPhysics
  validators and parser)

---

## Blender Integration

`blender/usd_physics_hook.py` is a Blender addon (4.0+) that bridges Blender's
rigid-body simulation with the UsdPhysics and mjcPhysics schemas. It installs as
a normal addon (Edit > Preferences > Add-ons > Install from Disk) and runs
automatically on File > Export/Import > USD (`.usda`/`.usdc`/`.usdz`). Blender's
bundled `pxr` Python modules are required.

**Schema mode** (addon preference): `USD` (UsdPhysics only), `MJC` (MuJoCo `mjc:`
attributes only), or `BOTH` (default — both).

**Export mapping (Blender → USD):**

| Blender Feature | USD Schema | Key Attributes |
|---|---|---|
| Gravity | `PhysicsScene` | `physics:gravityDirection`, `physics:gravityMagnitude` |
| Rigid body (active) | `PhysicsRigidBodyAPI` + `PhysicsMassAPI` | `physics:rigidBodyEnabled`, `physics:mass` |
| Rigid body (passive) | `PhysicsRigidBodyAPI` | `physics:rigidBodyEnabled=false`, `physics:kinematicEnabled` |
| Collision shape | `PhysicsCollisionAPI` | applied to mesh prims |
| Collision collections | `PhysicsCollisionGroup` | Bodies with disjoint Blender masks are filtered; each exported body also stores its mask for Blender re-import |
| Friction / restitution | `PhysicsMaterialAPI` | `physics:staticFriction`, `physics:dynamicFriction`, `physics:restitution` |
| Hinge / Slider / Fixed / Point constraint | `PhysicsRevolute` / `Prismatic` / `Fixed` / `SphericalJoint` | `physics:axis`, limits, `physics:body0/1`, `physics:localPos0/1`, `physics:localRot0/1` |
| Generic spring | `PhysicsJoint` with per-axis `PhysicsDriveAPI` and `PhysicsLimitAPI` | Enabled translation and rotation drives and limits; angular limits use degrees |

When the mode includes MuJoCo, collision/world/constraint prims also get
`mjc:condim`, `mjc:friction`, `mjc:solmix`, `mjc:margin`, `mjc:option:timestep`,
`mjc:option:iterations`, `mjc:flag:gravity`, `mjc:damping`, `mjc:stiffness`,
`mjc:group`. Import reverses the joint/body/material mapping and matches USD prim
names to Blender objects (exact and `.`→`_` matches first, then child mesh
names when the importer merges parent Xforms).
Constraint frames are exported relative to each body's world transform and
restored from the first resolved body on import. Import also resolves body
prim names to merged child-mesh objects, which Blender's USD importer creates
when `merge_parent_xform` is enabled. It selects the target before invoking
Blender's rigid-body operators. Singular body transforms are skipped.

On import, `mjc:geomFriction` on collider prims maps its first value (MuJoCo's
sliding coefficient) to Blender's single rigid-body friction value. When a
rigid body has several colliders with authored MuJoCo friction, the hook uses
their arithmetic mean; a `PhysicsMaterialAPI` with authored static friction on
the rigid-body prim takes precedence. Blender does not represent MuJoCo's
torsional or rolling terms or its contact-friction mixing rules.

USD `PhysicsCollisionGroup` filters are read on import and mapped to Blender's
20 collision layers. Inference is bounded to 256 bodies and 256 groups. A
graph that needs more than 20 layers, or exceeds those bounds, disables
imported body collisions and logs a warning rather than allowing filtered
pairs to collide.

Run the Blender runtime smoke test with:

```sh
blender -b --factory-startup --python blender/tests/test_usd_physics_runtime.py
```

The test uses Blender's actual USD export/import operators in both parent-Xform
merge modes, checks a Bullet free-fall and floor-contact simulation, and runs
an inclined-contact fixture with MuJoCo friction attributes on child colliders.
The imported body and incline both receive Bullet friction 0.8. A CPU comparison
with MuJoCo 3.10.1 put the slider near x=0.020 m in MuJoCo and x=0.558 m in
Blender at 1.5 s. Friction mapping is verified, but those contact responses
are not equivalent. The smoke test was run with Blender 5.2.3 LTS. It also
preserves a non-uniform USD scale `(2.0, 0.5, 1.5)` in both import modes and
compares source/import Bullet drops at frames 1, 30, 60 and 120 within 0.02
Blender units. It also imports a 2 m capsule pendulum with a Y-axis revolute
joint, verifies that it swings under gravity, and checks that its pivot stays
within 0.1 mm of the anchor over two seconds. A matching MuJoCo 3.10.1 CPU run
sampled the capsule center at 0.5, 1.0, 1.5 and 2.0 s; the largest
Blender-versus-MuJoCo position difference was 0.082 m at 1.5 s. This is one
no-contact joint case, not a general claim of solver equivalence.
A second fixture imports two linked capsules with two Y-axis revolute joints.
It verifies that both joints resolve to the intended bodies and that each
pivot remains within 1 cm of its counterpart throughout the two-second run.
In a matching MuJoCo 3.10.1 CPU comparison, the second link's center position
differs by at most 0.397 m at the four sample times (largest at 2.0 s); coupled
joint trajectories still diverge between Bullet and MuJoCo.

The addon writes the MjcPhysics names above as they are; MuJoCo 3.16's decoder
reads several of them as deprecated aliases (see the deprecation note before
section 1).

**Limitations:** soft body and cloth are not exported. Export supports up to
512 distinct Blender collision masks; beyond that limit the addon logs a
warning and omits USD filters while retaining the per-body masks. Imported
collision groups are mapped to Blender layers as described above. Spring
behavior is approximate across physics solvers, and MuJoCo-only mode retains
the legacy X-axis scalar spring mapping. MuJoCo actuators and tendons have no
Blender equivalent (preserved only if authored directly in the USD). Bodies
with multiple separate child mesh prims are not combined into compound Blender
colliders. The runtime checks do not establish spring or cross-solver dynamics
equivalence.
