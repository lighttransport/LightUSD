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
| Friction | deduplicated `Material` + `PhysicsMaterialAPI` (static = dynamic = mu0), `newton:torsional/rollingFriction`, `material:binding:physics` | `PhysicsMaterialAPI` only | `mjc:geomFriction` |
| Joint dynamics | `NewtonJointAPI` (`newton:armature`, `newton:friction`, `newton:damping` per degree for angular joints, `newton:velocityLimit`); `MjcJointAPI` `ref/springref/margin/stiffness/sol*` | omitted | `mjc:*` + `physxJoint/physxLimit` aliases |
| Inertia | `diagonalInertia` + `principalAxes` + `NewtonMassAPI` `newton:inertia`; MJCF bodies without `<inertial>` get MuJoCo's `inertiafromgeom` result | diagonal + axes | as authored only |
| Multi-joint bodies | stacked joints between the body pair (`newton:jointsAddMobility`) | stacked | massless intermediate links |
| MJCF actuators | `MjcActuator` with MuJoCo's compiled gain/bias/dynamics expansion (Newton maps position/velocity shapes to joint targets) | omitted | `NewtonActuator` PD for joint motors/position/velocity |
| MJCF equalities | connect → `PhysicsSphericalJoint`, weld → `PhysicsFixedJoint` (excluded from articulation, `active` → `jointEnabled`, site forms supported); joint → `NewtonMimicAPI` + `MjcEqualityJointAPI` on the follower (`mimicCoef0` in degrees) | spherical/fixed joints only | `Xform` + `MjcEquality*API` |
| MJCF collision | collides iff `contype|conaffinity != 0`; group 3+ non-colliders become guide visuals | same | group-based split |
| Mocap / gravcomp | `physics:kinematicEnabled`, `mjc:gravcomp` | kinematic only | `mjc:mocap` |
| URDF `planar` | D6 `PhysicsJoint`, `transX/rotY/rotZ` locked (`low > high`), `transY/transZ/rotX` unlimited (`-inf`/`inf`; Newton creates a D6 DOF only for an axis with a LimitAPI) | same | error |
| URDF effort | `urdf:limit:effort` + `mjc:actuatorfrcrange` (±effort) with `mjc:actuatorfrclimited` | `urdf:limit:effort` | `urdf:limit:effort` |
| MJCF `ref` | `mjc:ref` only (Newton measures joint coordinates from it) | omitted | also `state:*:physics:position` |
| MJCF stacked joints with a ball | intermediate-link chain (Newton merges only hinges/slides into a D6) | stacked | massless intermediate links |
| MJCF `<flexcomp>` | UsdPhysics deformables (below) | unsupported (reported) | unsupported (reported) |
| MJCF primitive visuals | native `Sphere`/`Cube`/`Cylinder`/`Capsule`/`Plane` (as mujoco-usd-converter), material bound, `displayColor`/`displayOpacity` from geom `rgba` | same | tessellated meshes |
| MJCF ellipsoids (non-uniform) | mesh with the radii baked in (convex hull as a collider): a scaled `Sphere` is read as a unit sphere by Newton and rejected by OpenUSD | same | scaled `Sphere` (reported) |
| URDF ghost / dummy-inertia links | urdf-usd-converter rules: inertia-, visual- and collision-less links on fixed joints (or the root) lose `RigidBodyAPI`, their fixed joint is dropped and child joints re-anchor on the nearest rigid ancestor (or the world); an inertial-only childless child on a fixed joint under a geometry-only parent is merged into it | same | as authored |
| MJCF `<flag>` | `newton:gravityEnabled` follows `gravity`; `filterparent="disable"` sets `physics:collisionEnabled` on joints; `spring`/`damper` flags authored | flags only | flags only |
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
or STL, ASCII `gmsh` 4.1/2.2; MuJoCo's point/element generation including
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
are reported as approximated or dropped; binary GMSH, raw `<flex>` and
`<skin>` stay unsupported. Shells carry `physics:restBendAnglesDefault =
restShape` (MuJoCo bends relative to the initial shape). On MuJoCo's flex sample models the vertices
match MuJoCo's compiled `flexvert_xpos` and the element counts match;
Newton imports them as particles, triangles, tetrahedra and cable rods.

MJCF visuals: capsule meshes have hemispherical caps (previously plain
cylinders), cameras default to MuJoCo's 45 degree `fovy` and take
`clippingRange` from `<visual><map znear zfar>` times an authored
`<statistic extent>`, lights carry `intensity` and spot `exponent`
(`inputs:shaping:focus`), and material `texrepeat` becomes a
`UsdTransform2d` scale on the diffuse texture. Builtin textures are reported
(`mjcf.texture_builtin_unsupported`).

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
stage. The C++ converter is `lightusd::tydra::ConvertURDFJsonToUSDStage`
(`src/tydra/urdf-to-usd.cc`); it consumes a JSON payload produced by the web
demo (`web/js/urdf.js`) or the node CLI (`web/js/cli/urdf-to-usd.js`), both of
which parse the source robot and emit per-joint frames (`localPos0/1`,
`localRot0/1`) and per-link inertials. The WASM entry point is
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
<directory>/Payload/Textures/       copied textures (textures="copy")
```

The stage's root prim (`/World`) is renamed to `/<name>` and every
relationship target and connection follows it. API schemas are split per
layer as `prepend` list ops, so the composed prims carry the original list.
Opened in OpenUSD, the interface file composes to the same prims, schemas,
values, connections and targets as the flat output (checked on Menagerie
panda, spot and g1). It is a Python helper; `format="usdc"` writes binary
layers. Mesh/material library layers (`GeometryLibrary.usdc` with
references) are not split out.

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
joints (M L^2) with angles kept in degrees; gravity magnitude and Newton
contact distances (L); deformable masses and thicknesses. Time samples are
converted with defaults. An up-axis change prepends (or accumulates and
removes) `xformOp:rotateX:upAxis` on root prims and rotates
`physics:gravityDirection`. `mjc:*` attributes stay in MuJoCo model units;
DOF quantities whose linear/angular kind is unknown are listed in
`report.skipped`. Converting Menagerie models to cm/g and back reproduces
every value to float precision.

### Best-effort and unsupported features

These are converted on a best-effort basis and **emit a warning** (console /
status line) instead of being dropped silently:

| Feature | Behavior |
|---|---|
| MJCF body with multiple `<joint>` | converted to a chain of single-DOF joints through `(N-1)` massless intermediate link Xforms (`<body>__mjcdof_k`), preserving every DOF; only the first joint carries the body offset |
| MJCF `ball` joint | mapped to `PhysicsSphericalJoint`; the single-slider preview leaves it at rest (3 DOF can't be driven by one scalar) |
| MJCF non-diagonal `fullinertia` | diagonalized into `physics:diagonalInertia` (principal moments) + `physics:principalAxes` (eigenvector quaternion), the lossless USD representation |
| URDF `<mimic>` | exported as `NewtonMimicAPI` when the target joint is also exported |
| MJCF joint-targeted `<actuator>` | exported as `NewtonActuator` best-effort (`kp`/`kv` or `gainprm`/`biasprm`, force/control range, delay); non-joint actuators are not converted |
| MJCF `<tendon><fixed>` | `MjcTendon` (type=fixed) under `/World/Tendons` — `mjc:path`→coupled joints, `mjc:path:coef`, stiffness/damping/range |
| MJCF `<tendon><spatial>` (muscles) | `MjcTendon` (type=spatial) routed through `<site>` markers, native `Sphere`/`Cylinder` wrap geoms with side sites, and pulley segments/divisors (see Output profiles). Visualized as polylines in the web demo source view |
| MJCF `<site>` | `GeomSphere` marker under `/World/Sites` + `MjcSiteAPI` (the routing points for spatial tendons) |
| MJCF `<actuator><muscle>` / `<general class="muscle">` | `MjcActuator` (`mjc:target`→tendon, `gainPrm`/`biasPrm`/`lengthRange`/`ctrlRange`) |
| MJCF `<equality>` (connect/weld/joint) | converted to an Xform host prim under `/World/Equalities` carrying the matching `MjcEquality{Connect,Weld,Joint}API` + `mjc:*` attributes |
| MJCF `<contact><exclude>` | converted to `PhysicsFilteredPairsAPI` (`physics:filteredPairs`) on the first body |

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
| Friction / restitution | `PhysicsMaterialAPI` | `physics:staticFriction`, `physics:dynamicFriction`, `physics:restitution` |
| Hinge / Slider / Fixed / Point / Spring constraint | `PhysicsRevolute` / `Prismatic` / `Fixed` / `Spherical` / `DistanceJoint` | `physics:axis`, limits, `physics:body0/1`, `physics:localPos0/1` |

When the mode includes MuJoCo, collision/world/constraint prims also get
`mjc:condim`, `mjc:friction`, `mjc:solmix`, `mjc:margin`, `mjc:option:timestep`,
`mjc:option:iterations`, `mjc:flag:gravity`, `mjc:damping`, `mjc:stiffness`,
`mjc:group`. Import reverses the joint/body/material mapping and matches USD prim
names to Blender objects (exact, then `.`→`_` fallback).

**Limitations:** soft body and cloth are not exported; collision groups are not
mapped; per-axis springs export only the X-axis spring/damping; MuJoCo actuators
and tendons have no Blender equivalent (preserved only if authored directly in
the USD).
