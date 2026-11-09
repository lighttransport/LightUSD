# Goal prompt: OBJ and physics support across native and Python

Continue improving LightUSD's OBJ, URDF and MJCF import, USD physics authoring,
and native C++/C/Python interoperability. Deliver working implementations and
tests in priority order. Use the shared native implementation for Python;
avoid implementing a second conversion or validation engine in Python.

## Constraints

- Preserve existing defaults, supported input behavior and stable C ABI.
- Keep the C++17 core dependency-free and exception-free. MuJoCo, OpenUSD,
  Newton and geometry-cooking integrations remain optional adapters or tests.
- Preserve authored data by default. Normalization, pruning, inferred physics,
  approximation and unit conversion must be explicit and reported.
- Respect another agent's edits. Use isolated Ninja trees under
  `build_ninja/obj-physics-*` or the existing private `obj-large-*` trees.
- Generate stress OBJ/MTL inputs in RAM; do not commit large assets or build
  artifacts. Leave changes uncommitted until the user requests a commit.
- Keep this prompt's progress and evidence current. Do not mark the overall
  goal complete while required roadmap work remains.

## Priority 1: shared import resources and physics API parity

1. **Deduplicate textures.** Resolve/cache texture requests within an import;
   retain one immutable byte payload per resolved identifier across material
   maps, imported layers, C/Python stages and USDZ packaging. Preserve source
   anchors, authoritative resolvers, error propagation and memory accounting.
   Deduplicate package entries without losing repeated references or aliases.
2. **Structured import reports and options.** Provide shared diagnostics with
   stable codes, severity, source/prim locations, imported/approximated/skipped
   features, dependencies, timings and memory estimates. Expose independent
   input/memory/depth limits and an unsupported-feature policy in Python while
   preserving existing calls and C options compatibility.
3. **Complete typed physics access and validation.** Expose authored UsdPhysics,
   `mjc:*` and `newton:*` data through a shared C++/C query layer. Use it for
   Python inspection and validation, including efficient owned array views.
   Generate Python schema names, property types and variability from native
   descriptors instead of maintaining independent hard-coded registries.

## Priority 2: fidelity and larger assets

4. **Mass/inertia policies.** Preserve authored values, optionally infer
   supported analytic properties, and support an optional MuJoCo compiled-value
   adapter. Report provenance and validate physical tensors and fallback values.
5. **Physics-aware unit conversion.** Convert into an existing stage's length,
   mass and axis conventions, including inertia, velocities, gravity, joint
   limits and drives. Expose the same helper through native and Python APIs.
6. **Compact OBJ topology.** Offer source-position preservation, indexed
   face-varying UVs and face-varying normals to avoid unnecessary position and
   skin-weight duplication. Retain the existing expanded representation.
7. **Supplied-skeleton binding.** Add native/Python helpers that validate `vw`
   indices against an existing skeleton, author relationships and bind
   transforms, and explicitly remap joints. Pruning/normalization is opt-in.
8. **Constraints and tendons.** Extend tendon equality preservation and graph
   validation; support evaluated sphere/cylinder wrap paths with clear
   approximation reporting and solver capability checks.
9. **Versioned Newton compatibility.** Audit current mass/joint/SDF/shell-mass
   schemas and add typed preservation, validation and optional downstream
   import verification. Distinguish schema preservation from solver execution.

## Priority 3: I/O and export

10. Add mapped/chunked large-file input, cancellation and progress callbacks;
    expose OBJ/MTL export with explicit transforms, material/topology policies
    and optional `vw` output. Treat deformables as a separately versioned,
    experimental extension after the rigid-body foundation is verified.

## Acceptance and verification

- Verify each delivered feature with meaningful generated-input regressions in
  native and Python paths, including failures, limits and ownership lifetimes.
- Run focused native CTest, ASan/UBSan and Python suites from isolated builds.
  Run the full native/standalone-next regression gates at integration milestones.
- Inventory the actual local Menagerie revision using `MENAGERIE_ROOT`; test
  tracked OBJ/MTL files and standalone MJCF entries, accounting for fragments.
- Extend engine comparisons beyond successful loading: compare body mass, COM,
  inertia tensors, joint frames/limits, filtering, actuators and initial state
  between direct imports and converted USD in the same engine. Keep optional
  short dynamics checks separate from ordinary dependency-free tests.
- Test USDA/USDC/USDZ round trips and downstream OpenUSD/Newton behavior where
  applicable. Record tool versions and unsupported features without claiming
  simulation equivalence from schema preservation alone.
- Generate a tested capability matrix and correct stale physics documentation.

## Current progress

- Completed before this goal: bounded large OBJ parsing, `vw` arrays, normal
  generation fixes, strict MTL numeric validation, bounded custom-property
  sorting, indexed material/library lookup and duplicate-name source tracking.
- Current verification baseline: 103 focused Python tests; native and sanitizer
  coverage; 100,000-name lookup stress; 1,184 Menagerie OBJ and 24 MTL files.
  These changes remain uncommitted.
- Delivered Priority 1.1: per-import request/resolved-identifier texture caching,
  shared immutable ownership through layers/C/Python stages, separate resolver
  open/read callbacks, borrowed USDZ asset views and one entry per retained
  texture. Fixed export path rewriting so packaged shader paths are self-contained
  without mutating the source stage. Separate composed foreign layers retain
  independent read caches.
- Priority 1.1 verification: 116 focused Python tests; all 68 registered standalone
  next tests passed (two optional AOUSD checks skipped); five focused ASan/UBSan
  tests passed. Native CLI build and legacy CLI syntax check passed.
- Delivered Priority 1.2: bounded versioned native reports, owned C report queries,
  Python stage/error dictionaries, independent input/memory/depth limits and
  explicit warn/error/preserve policies for known unsupported OBJ/MJCF data.
  Preserved records survive USDA/USDC/USDZ as non-executable layer metadata;
  reports remain runtime telemetry. Included/skipped XML subtrees and expanded
  documents enforce depth limits. Both native CLIs expose the new policies and
  independent limits. See [the report contract](import-report.md).
- Priority 1.2 verification: 135 focused Python tests (including OpenUSD 23.08
  preservation/skin oracles), full Python suite 269 passed / 10 optional skips,
  all 68 registered next tests passed (two optional AOUSD checks skipped), six
  focused ASan/UBSan tests passed. Both native CLIs passed preservation exports
  and policy/limit failures. At Menagerie revision
  `cda5236f98383f6fbaa4820c6b18dba892cd087f`, all 1,184 OBJ files passed and all
  223 standalone MJCF entries passed the existing MuJoCo 3.16.0 rest-frame,
  joint-anchor/limit and record-count comparisons. These checks do not establish
  dynamics, inferred-inertia or surface-wrap equivalence.
- Priority 1.3 delivered first slice: shared native C++/C typed snapshots now back
  Python inspection, including owned array views, source property types,
  variability, blocks, unknown engine attributes and collision groups. Existing
  Python tuple output remains the default. Native C++/C ownership/time/budget
  tests and 46 focused Python cases passed; the full Python suite passed 280
  tests with 10 optional skips. See [typed snapshots](physics-query.md).
- Snapshot sanitizer and core-only (Tydra/OBJ disabled) checks passed. All 223
  standalone Menagerie entries also passed through the native-backed inspector.
- Standard UsdPhysics authoring now reads types, variability, API names and
  multiple-apply namespaces from the generated native C11 descriptors. Python
  exposes immutable definitions and checks drive/limit instances. Versioned
  engine extension descriptors are still pending.
- Snapshot/descriptor milestone verification: full Python suite 284 passed /
  10 optional skips, plus
  150 final focused cases including explicit-auto dispatch; all 69 registered
  native next tests passed (two optional AOUSD checks skipped). Snapshot
  ASan/UBSan, core-only query and pure-C11 descriptor-link checks passed. The
  instance-group timing gate passed in the final full run after its earlier
  contention-related threshold failure during concurrent build/dataset work.
- Delivered structured native validation through owned C/C++ reports and
  immutable Python `physics.validate()` results. Reports retain native rule IDs,
  locations, sample times and coverage. Streaming issue/byte limits and bounded
  sample counts expose omissions and cannot turn incomplete reporting into a
  passing result. The validation component remains optional for ordinary native
  core/query/render consumers. See [validation](physics-validation.md).
- The new semantic Menagerie gate exposed and drove corrections for inherited
  partial MJCF solver arrays, canonical option tokens, zero solver tolerances,
  `jointactuatorfrc` and independent UsdPhysics inertia/orientation fallbacks.
  Solver completion and enum spelling changes have structured diagnostics.
  Nonuniform analytic ellipsoid approximations now respect warn/error/preserve
  policies while retaining the existing default representation.
- Current verification: full Python suite 308 passed / 10 optional skips;
  native next suite 68 passed / two optional AOUSD skips.
  Four focused ASan/UBSan tests, the core-only validation-report test and a C11
  header check passed. Both native CLIs passed solver/default-policy checks and
  USDA/USDC/USDZ mesh-collider validation round trips. Generated native C and
  Python cases cover inherited solver vectors and excess-entry failures.
- At Menagerie revision `cda5236f98383f6fbaa4820c6b18dba892cd087f`, all 223
  standalone entries passed import and existing MuJoCo 3.16.0 reference checks
  in both final profiles. With explicit mesh colliders, all 223 also passed
  native semantic validation. Default analytic colliders produced 50
  `physics.collider.nonUniformScale` findings across 10 entries with nonuniform
  ellipsoids. Meshes remain polygon approximations; these checks establish
  neither analytic collision nor simulation equivalence.
- 2026-10-23 review/parity round (uncommitted): reviewed d6a78ccfd/acd9725f3
  against newton-usd-schemas 0.6.0, mujoco-usd-converter 0.8.0,
  urdf-usd-converter 0.3.4, Newton 1.8 `import_usd`, MuJoCo 3.16's USD decoder
  and OpenUSD 26.08. Delivered:
  - Output profiles `newton` (native default) / `openusd` / `legacy` (JSON
    bridge default); see [USD physics](usd-physics.md#output-profiles).
    Legacy output is byte-identical for 59 of 69 Menagerie models; the other
    10 differ only through profile-independent fixes listed there.
  - Versioned engine descriptors (Priority 1.3 remainder): Newton/MuJoCo
    schemas in the native tables and Python `schema_definitions()`; writer
    variability from descriptors; validator `core.schema.variability` and
    standard `limit:`/`drive:` names; zero gravity direction accepted;
    `low > high` treated as a locked axis.
  - Security/hardening: dependency containment for bare source names (an
    empty root accepted every path), backslash texture paths, bounded
    ear-clipping work, checked MJCF integer conversion, int32-safe heightfield
    ids, iterative URDF chain transforms.
  - MJCF semantics: `limited`/`autolimits`, class orientation precedence,
    root class `main`, unknown classes, downward `zaxis`, frame joint axes and
    light directions, actuator shortcut expansion, collision by
    `contype|conaffinity`, stacked joints, equality joints (body and site
    forms), mesh-fit primitives, box/ellipsoid `fromto`, MuJoCo
    `inertiafromgeom` (Priority 2.4 inference path) and first-OBJ-object
    meshes.
  - URDF: planar D6, zero axis on fixed joints, missing/unknown geometry under
    the policy, ASCII STL, COLLADA meshes/materials (`dae.cc`),
    `urdf:safety_controller:*`/`calibration:*`/`limit:effort`.
  - OBJ: export rewritten (corner indices, deterministic groups, bounds,
    locale, one MTL entry per material); MTL `Ni` presence, `d` over `Tr`,
    clearcoat, `-imfchan`, `-bm`, `-colorspace`; `metersPerUnit`/`kind`.
- Verification of this round: full Python suite 353 passed / 11 optional
  skips; all 70 standalone next tests; legacy `unit-test-lightusd` all pass;
  ASan/UBSan clean on the import/OBJ/converter/validation/query tests (the
  known `-fno-rtti` vptr false positive excepted). Inferred inertia matches
  MuJoCo 3.16 for 1,226 of 1,237 Menagerie bodies. OpenUSD 26.08's 28
  validators report no issues for sampled Menagerie models in all profiles
  after the `inputs:st` fix. Newton comparison results are in
  [testing](testing-cpp.md#downstream-newton-and-openusd-checks).
- 2026-10-24 round (soft bodies, tendons, Newton contract):
  - Tendons: exact MuJoCo path encoding (native wrap geoms, side-site
    indices, pulley segments/divisors), tendon group/armature/sol*/
    actuatorfrc*, two-value springlength; actuators with unexported
    targets are skipped. Verified against MuJoCo through Newton.
  - `<replicate>` and `<composite type="cable">` expansion; `<flexcomp>`
    grid/circle/square/disc/direct → UsdPhysics deformables (newton
    profile) with pins as `PhysicsAttachment`.
  - Newton contract fixes: URDF planar free axes, URDF effort as
    `mjc:actuatorfrcrange`, no ref-mirroring joint state, gravity flag,
    `filterparent`, spring/damper flags, weld `relpose` body2 frame,
    ball-containing joint stacks as chains, URDF colliders keep Newton's
    gap/margin defaults, revolute-only mimic degree conversion, world
    `<exclude>` filters world colliders, mesh `maxhullvert`, static meshes
    collide as convex hulls (hfields stay exact).
  - Validator: `physics.extension.newton.inertia` (6 finite values, PSD,
    triangle inequality, agreement with `diagonalInertia`) and NewtonJointAPI
    ranges (armature/friction/damping >= 0, limit gains >= 0 or -inf).
- 2026-10-24 round 2: every flexcomp type (box/cylinder/ellipsoid, OBJ/STL
  mesh, ASCII GMSH) matching MuJoCo vertex for vertex; cable elasticity as
  ball-joint stiffness E*I/L (sag within ~4 mm of MuJoCo's plugin through
  Newton); native primitive visuals with rgba display colors, baked
  ellipsoid meshes, capsule caps, camera fovy/clipping, light intensity,
  texrepeat; URDF ghost-link collapse and dummy-inertia merge matching the
  urdf-usd-converter test corpus; `fuzz_next_import` libFuzzer harness
  (OBJ/URDF/MJCF via the C API, COLLADA directly) with seeds in
  `tests/fuzzer/next_import_seeds` - it found an unaligned pugixml read in
  the COLLADA path and deep-cable stack overflows (now an iterative flexcomp
  walk and a 512-vertex cable cap).
- Phase E (first pass): the C/Python import keeps mesh positions, normals,
  UVs and indices as typed buffers referenced by `meshRef`
  (`Options::typed_meshes`, set by the C API; the CLI's `--dump-json` keeps
  plain JSON), mesh/link/payload JSON is assembled by move instead of
  braced initializer lists (which deep-copied every array), and OBJ corner
  deduplication uses an open-addressed table instead of `std::map`. Output is
  byte-identical (typed vs JSON path on all 68 Menagerie first models and 56
  urdf-usd-converter corpus files). Import time: unitree_g1 1.52 -> 0.27 s,
  aloha 1.41 -> 0.30 s, ms_human_700 0.78 -> 0.24 s, stretch 2.54 -> 1.87 s,
  fruitfly 5.97 -> 2.32 s (134 MB of OBJ text; OBJ parsing now dominates).
- Compact OBJ topology (Priority 2.6): `obj_topology="indexed"` keeps source
  positions with indexed face-varying normals/UVs (OpenUSD flattens them to
  one value per corner; no validator issues).
- Supplied-skeleton binding (Priority 2.7): `lightusd.skel.bind_skin`
  validates `vw` joint ids against a USD Skeleton, remaps them and authors
  the UsdSkel binding; pruning/normalization are opt-in.
- Physics-aware unit conversion (Priority 2.5): `lightusd.units.convert_units`
  (lengths, masses, inertia, joint frames/limits/drives by DOF kind, gravity,
  up axis); native in `lightusd_stage_convert_units` (C) since 2026-10-24,
  ~50x faster than the former Python pass and byte-identical on 6 Menagerie
  models x 3 targets. The port fixed a double-scaled default authored for
  time-sample-only attributes and kept declared types (`half3[]`).
- Nested body hierarchy: `layout="nested"` (opt-in).
- OBJ parser throughput: tinyobjloader-c scanner speedups (31 MB OBJ 0.234 ->
  0.165 s, byte-identical); fruitfly import 2.32 -> 1.75 s.
- OBJ/MTL export (Priority 3): `lightusd.objexport.export_obj` with world or
  local space, polygon/triangle topology, MTL from UsdPreviewSurface and
  optional `vw`, verified by import round trips.
- Atomic component layout: `lightusd.asset.save_component` (interface +
  Payload/Contents + Geometry/Materials/Physics layers + copied textures),
  composing in OpenUSD to the flat output. Supporting API: `Prim.declare` /
  C `lightusd_attr_declare` (typed attribute without a value) and list-op
  qualified `apiSchemas` metadata (`"prepend apiSchemas"`).
- MJCF builtin 2D textures (checker/gradient/flat + marks) generated as PNG,
  pixel-identical to MuJoCo; written beside saved layers, packaged in USDZ.
- MJCF builtin skybox -> latlong DomeLight (BuiltinCube faces pixel-identical
  to MuJoCo, resampled with OpenGL cube-map addressing, pole on stage +Z).
- Raw MJCF `<flex>` -> the flexcomp deformable layout (point-mass vertex
  bodies absorbed, other vertex bodies pinned); verified against MuJoCo-saved
  flexcomps and Newton.
- Component `GeometryLibrary` layer (`save_component(geometry_library=True)`):
  one referenced prim per distinct mesh shape; OpenUSD compose-back exact.
- Not planned: interpolated (trilinear/quadratic) flex dofs (no UsdPhysics or
  Newton representation; converted as full-dof and reported as approximated)
  and cube-projected material textures (UsdPreviewSurface has no cube
  projection; reported).
- MJCF and URDF mesh assets parse on worker threads (`threads=`), replayed
  so output, warnings and reports equal a serial import (Menagerie 53.0 ->
  32.5 s; 63-mesh URDF 0.60 -> 0.15 s).
- Nothing from the original list is pending; see "Not planned" above. Import
  progress/cancellation callbacks (C `progress`, Python `progress=`) and
  memory-mapped OBJ files and URDF/MJCF mesh assets (`Reader::Map`) are in;
  binary STL vertex welding uses an open-addressed table (500k-triangle STL
  import 1.82 -> 0.39 s, op3 0.52 -> 0.24 s, g1 0.22 -> 0.14 s, USDC
  byte-identical on 8 models). Composed-stage physics
  validation needs no separate mode: stages loaded with composition (the
  default) validate their composed prims and targets (covered by a test).

Gap review against mujoco-usd-converter 0.8.0, urdf-usd-converter and Newton
f121272e (2026-10-24; Menagerie Newton compare still 65/69). Fixed in the
`newton` profile, `legacy` byte-identical on 347 Menagerie/corpus inputs:

- URDF `<transmission>` -> `urdf:transmission:*` joint metadata, not a
  zero-gain actuator (`add_usd` failed on fixed/planar targets).
- Unmapped `package://` roots discovered like Newton/urdf-usd-converter
  (`urdf.package_discovered`); the corpus `ros_packages.urdf` now converts.
- `newton:selfCollisionEnabled` only when authored; `newton:maxHullVertices`
  only for `maxhullvert`; `<option iterations>` -> `newton:maxSolverIterations`;
  mesh `inertia`/`shellinertia` -> `newton:massModel`/`mjc:inertia`; material
  `newton:contactStiffness/contactDamping` from `solref`.
- Second round: one prim per visible collider (1,953 fewer prims over 132
  Menagerie/corpus models), no deprecated `mjc:*` aliases, linear MJCF
  material colors and `roughness = 1 - shininess`, `displayName` for changed
  names, `mjcf.frame_scale_unsupported`, `actuatorgroupdisable`, URDF
  unknown elements as `urdf:*` scopes (the reference's `undefined.urdf`
  layout), URDF `<capsule>` and native primitive visuals, no articulation root
  on jointless URDFs. Legacy byte-identical; Newton compare 65/69; OpenUSD
  validators 0 issues.
- Third round: MJCF OBJ polygons follow MuJoCo's triangulation (ms_human's
  ribs now match MuJoCo's masses exactly), explicit zero MJCF inertia is
  written as zero `newton:inertia`, and the Newton compare arbitrates
  `limited="false"` joints with MuJoCo (Newton's add_mjcf keeps the range).
  Newton compare 66/69; the three left are not fixable in USD output:
  stretch's gripper (0.3%: MuJoCo 3.16's wheel drops most of one degenerate
  215-gon that tinyobjloader 2945a96 itself triangulates under every
  compiler/flag tried), rby1 `NECK_0` (explicit `<inertial mass="0">` with
  colliders: UsdPhysics reads mass 0 as unspecified, so add_usd derives it
  from colliders) and lite6 `link_base` (explicit zero inertia, authored as
  zero `newton:inertia`, but add_usd replaces zero inertia of a body with
  mass by a sphere default while add_mjcf keeps it). Also fixed: the
  supported-schema ledger (usdProc/usdUI in scope, 9bd1cc133) and the
  roundtrip fuzzer build.
- MJCF colliders now always carry `newton:contactMargin`/`contactGap`
  (MuJoCo's 0 when unauthored), as mujoco-usd-converter writes them; Newton
  would otherwise apply `rigid_gap = 0.1` m (its own `add_mjcf` does).
- Resolved by verification, no change (accepted by the user): the URDF root weld (as
  urdf-usd-converter and `add_urdf`; `add_usd(floating=True)` still floats it
  because the weld has no `physics:body0`); URDF effort limits (Newton
  schemas 0.6.0 have no joint effort attribute; Newton reads effort only from
  `mjc:actuatorfrcrange` via its MuJoCo resolver, which we author, or a
  drive's force limit, and a zero-gain drive would switch Newton to effort
  control). The stretch residual was traced to the MuJoCo 3.16 wheel's own
  decoder: called directly it returns 23,575 faces for
  `link_SG3_gripper_body.obj` with bit-identical vertices, while
  tinyobjloader 2945a96 (and every release, gcc/clang, float/double,
  fast-math, FMA, earcut) yields 23,776; our triangulation matches the
  source.

- Residual Newton-compare mismatches fixed (2026-10-24, user asked to keep
  working on them): the MuJoCo 3.16 PyPI OBJ decoder drops every polygon of
  more than 100 vertices (found by black-box tests against the wheel's
  decoder; 100 kept, 101 dropped), which we now match, so stretch's gripper
  equals MuJoCo's compiled mass; explicit MJCF zero inertia/mass are authored
  as 1e-12 (UsdPhysics cannot hold an exact 0), so lite6 `link_base` keeps
  ~zero inertia and rby1 `NECK_0` ~zero mass in Newton's `add_usd` (Newton's
  finalize then floors that body's inertia at 1e-6, which only an exact zero
  mass escapes; the compare records it as a note). Menagerie Newton compare
  69/69. Upstream issue drafts (Newton zero-inertia/zero-mass handling,
  MuJoCo wheel polygon limit) are in the session scratchpad for filing; the
  repo no longer depends on them.

- Review closed (2026-10-24, accepted by the user): the 22 gap-review fixes,
  the stretch/rby1/lite6 residual fixes (Newton compare 69/69), the URDF root
  weld and effort limit (verified, unchanged) and the two not-planned items
  (interpolated flex dofs, cube-projected material textures).

- URDF Newton compare (2026-10-24): `run-newton-compare.py --urdf DIR`
  compares `add_urdf` with `add_usd` of LightUSD's output over Newton's own
  assets (g1, h1, anymal, fr3, allegro), Newton examples, urdf-usd-converter's
  corpus, chrono and tiny-differentiable-simulator: 135 pass, the 3 invalid
  `error_*` fixtures are rejected. Fixed on the way: negative MTL `Ns`
  (Blender) rejected the OBJ, zero-size URDF primitives and unsupported mesh
  formats failed the import, `../meshes` in a ROS package was outside the
  allowed roots, mimic-of-mimic chains made `add_usd` fail, explicit URDF zero
  mass/inertia were derived from colliders, and ghost links were judged by
  converted rather than authored geometry.

- MuJoCo USD-decoder check (2026-10-24): `run-mujoco-usd-compare.py`
  compiles each Menagerie MJCF with MuJoCo 3.16 and compiles LightUSD's stage
  through MuJoCo's own USD decoder (OpenUSD 26.05 build): 68/69 pass (stretch:
  the wheel-only OBJ polygon limit). Fixed in LightUSD: default gravity was
  9.80665 (MuJoCo 9.81); actuator default classes were inlined by tag
  (ms_human's `<general class="muscle">` lost its muscle model, stretch's
  `<position class>` its `<general>` gains, rby1's velocity took a
  position's kv); shell meshes used as visual geoms lacked the shell mass
  model (stretch, adam_lite failed to compile); `newton:inertia` was written
  for diagonal tensors too. Decoder bugs/limits found (issues/, untracked):
  newton:inertia + diagonalInertia rejected, physics-purpose material
  bindings ignored, sites also decoded as geoms, unauthored limits decode as
  limited, no tree ball joints, no collision bitmasks, no body gravcomp.

- openusd collision filtering (2026-10-24): MJCF contype/conaffinity bitmasks
  become UsdPhysics collision groups (exact for any masks: one group per
  distinct pair, filtered pairs from MuJoCo's rule), parsed by OpenUSD's
  UsdPhysics (iit_softfoot 1 group, agility_cassie 3), validators clean.

- MJCF SDF geoms (2026-10-24): `<geom type="sdf">` becomes a mesh collider
  with NewtonSDFCollisionAPI (approximation none); MuJoCo's analytic SDF
  plugin meshes are generated as MuJoCo's compiler does (port of
  plugin/sdf distances + vendored MarchingCubeCpp `f03a1b3e`), identical
  vertex/face counts. MuJoCo's model/plugin/sdf examples: decoder compare 6/6
  (torus bowl gear nutbolt primitives cow), Newton compare 4/4 (no CUDA here:
  the harness compares SDF colliders as meshes, since Newton builds SDFs on
  CUDA only; Newton's add_mjcf drops these geoms, mass 0, while the USD
  matches MuJoCo's masses). The remaining open items are resolved:
  `mesh.xml`'s die.obj (a vertex-only point cloud) is now the convex hull of its
  vertices, as MuJoCo's `mjCMesh::Process` does (`mjcf.mesh_convex_hull`;
  degenerate point sets are reported and rejected), and `mug.xml`'s
  `../../mug/mug.obj` resolves when the model root is an allowed asset root --
  `run-mujoco-usd-compare.py` now derives that root for MJCF inputs whose text
  references `../`.

- MJCF `<skin>` and binary GMSH (2026-10-24): skins become UsdSkel
  (SkelRoot/Skeleton/skinned Mesh, weights normalized, inflate baked so the
  rest pose is exact) with a LightUSD extension relationship
  `lightusd:mjcSkin:bodies` to the bone bodies; rest pose equals MuJoCo's
  `mjv_updateScene` skin vertices to float precision (flag 7462 verts,
  cube_skin .skn with a world bone, a rotated two-bone blend). Binary GMSH
  4.1/2.2 (gmsh and fTetWild layouts) for flexcomp matches the ASCII files and
  MuJoCo's flex sizes on its 8 cube test models; `.gmsh` and legacy `.msh`
  mesh assets pass the MuJoCo-decoder compare. Newton's add_mjcf cannot load
  the binary volume GMSH or `.msh` references (its own limits).

- SDF and volumetric processing through tinyvdb (2026-10-25): tinyvdbio
  branch volumetric-sdf (mesh -> sparse level set with winding-number signs,
  winding numbers, isosurface stuffing, file init for writing; a quadratic
  sparse-map rehash fixed, OpenVDB oracle build fixed) vendored as one
  lightusd_tinyvdb target that also owns miniz/zstd. MJCF sdf geoms get
  level-set .vdb sidecars (generated assets) + Newton SDF parameters +
  guide Volume/OpenVDBAsset + lightusd:sdf; flexcomp opt-in stuffing;
  .vdb mesh/flexcomp inputs; C/Python/CLI APIs. Checks: OpenUSD validators
  clean, Newton resolves the SDF parameters and imports stuffed TetMeshes,
  vdb_print reads the sidecars, legacy byte-identical. Open: graded
  stuffing lattices.
- CUDA/HIP level sets and stuffing (2026-10-25): tinyvdb gained a HIP backend
  (HIP driver API in the CUDA loader table; all CUDA tests pass on HIP) and
  tinyvdb_gpu_compute (device distance + winding and lattice samples, host
  bookkeeping; CPU-equal results). Brick-based candidate bands and a
  bucketed stuffing boundary also made the CPU path faster with byte-identical
  output. LightUSD: opt-in LIGHTUSD_TINYVDB_GPU build, `volumetric_device`
  (cpu default, cuda, hip, auto) in importers, C, Python and CLI. RX 9070 XT:
  level sets 24-32x faster than 8 CPU threads; sidecars of the plugin SDF
  examples match the CPU's except for on-surface signs.

Reference specifications: [UsdPhysics](https://openusd.org/dev/api/usd_physics_page_front.html),
[MJCF](https://mujoco.readthedocs.io/en/stable/XMLreference.html), and
[Newton USD import](https://newton-physics.github.io/newton/latest/concepts/usd_parsing.html).
