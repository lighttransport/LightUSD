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
- Still pending: geometry/material library layers, raw
  `<flex>`, interpolated (trilinear/quadratic) dofs,
  cube/skybox builtin textures, parallel per-mesh conversion and mapped input
  for URDF/MJCF mesh assets. Import progress/cancellation callbacks (C
  `progress`, Python `progress=`) and memory-mapped root OBJ files are in. Composed-stage physics
  validation needs no separate mode: stages loaded with composition (the
  default) validate their composed prims and targets (covered by a test).

Reference specifications: [UsdPhysics](https://openusd.org/dev/api/usd_physics_page_front.html),
[MJCF](https://mujoco.readthedocs.io/en/stable/XMLreference.html), and
[Newton USD import](https://newton-physics.github.io/newton/latest/concepts/usd_parsing.html).
