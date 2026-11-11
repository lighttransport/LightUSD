# LightUSD Documentation

Category index for the docs in this directory. Completed plans and superseded
notes are removed rather than archived; find them in Git history.

## API and Architecture

- [api-status.md](api-status.md) — OpenUSD schema coverage matrix (per-domain
  tables, implemented / stub / unsupported status, remaining gaps).
- [composition.md](composition.md) — Composition arcs (LIVRPS), instancing, and
  variants; LIVRPS correctness analysis vs OpenUSD PCP.
- [pcp.md](pcp.md) — Composition-graph (PCP / DAG engine) API reference,
  OpenUSD and AOUSD alignment.
- [instancing.md](instancing.md) — OpenUSD instancing model, the Moana reference scene
  instancing structure, and LightUSD's instancing state + scaling plan.
- [timesamples.md](timesamples.md) — Time-sampled attribute evaluation,
  interpolation, and value deduplication.
- [crate-writer.md](crate-writer.md) — USDC Crate writer internals (ValueRep,
  indices, dedup, time samples).
- [mmap.md](mmap.md) — MMap-based USD loading and the mmap array fast path.
- [unregistered-value.md](unregistered-value.md) — UnregisteredValue handling
  (type inference rules) in OpenUSD vs LightUSD.
- [usd-json.md](usd-json.md) — minijson-backed USD layer/stage JSON conversion,
  arrays, buffers, and round-trip behavior.
- [signed-zero.md](signed-zero.md) — IEEE-754 signed-zero handling in OpenUSD,
  AOUSD Core requirements and ambiguities, and bit-exact round-trip guidance.
- [ousd-vs-lightusd.md](ousd-vs-lightusd.md) — AOUSD Core 1.0.1 / OpenUSD review of
  LightUSD core and Tydra, with remediation status.

## Schema Domains

- [openusd-schema-compatibility.md](openusd-schema-compatibility.md) — OpenUSD
  schema compatibility baseline.
- [usdobj.md](usdobj.md) — Wavefront OBJ support (`usdObj`).
- [import-report.md](import-report.md) — Native import reports and policies.
- [physics-query.md](physics-query.md) — Typed physics snapshots.
- [physics-validation.md](physics-validation.md) — Structured physics validation.
- [usdLux.md](usdLux.md) — Lighting schemas and Tydra light conversion.
- [usd-physics.md](usd-physics.md) — UsdPhysics, MuJoCo mjcPhysics, Newton.
- [usd-physics-upAxis.md](usd-physics-upAxis.md) — `upAxis` handling in the
  physics API.
- [materialx.md](materialx.md) — MaterialX and OpenPBR material support
  (uses `blender_shader_nodes.json` / `blender_to_materialx_node_mapping.json`).
- [skinning.md](skinning.md) — UsdSkel skeletal animation and skinning.
- [mjcf-mjx-support.md](mjcf-mjx-support.md) — MJCF / MuJoCo-MJX → USD support
  matrix.
- [lte_spectral_api.md](lte_spectral_api.md) — LTE SpectralAPI extension
  proposal.
- [how-to-implement-feature.md](how-to-implement-feature.md) — Step-by-step
  procedure for adding a new USD schema / prim type.

## Rendering, GPU, and Assets

- [color-management.md](color-management.md) — Color-space resolution and the
  rendering color regression.
- [lusdview-texture.md](lusdview-texture.md) — Compressed texture formats for
  OpenGL/Vulkan in lusdview.
- [ptex.md](ptex.md) — Ptex support.
- [workflow-tools.md](workflow-tools.md) — Dependency reports, property traces, checker baselines/SARIF, recursive diff, static GLB and CPU capture workflows.

- [lusdview.md](lusdview.md) — `lusdview` build & GPU debugging notes.
- [lmetalview](../examples/lmetalview/README.md) — native macOS Swift + Metal
  Quick Look-style viewer (core product only).
- [ld3d12view](../examples/ld3d12view/README.md) — native Windows Direct3D 12
  viewer on the lmetalview core and lightui (core product only).
- [lusdview-tasks.md](lusdview-tasks.md) — `lusdview`/`lusdrender` open tasks
  and working notes (incl. the external usd-assets run log).
- [lusdrender.md](lusdrender.md) — `lusdrender` GPU backend testing.
- [tydra-tangent.md](tydra-tangent.md) — Tangent/normal computation and
  quantization.
- [threejs.md](threejs.md) — Three.js integration notes.
- [texcomp.md](texcomp.md) — GPU texture compression (KTX2 / BC / ASTC / ETC2).
- [udim.md](udim.md) — UDIM texture support.
- [image-formats.md](image-formats.md) — Image writer/loader formats, bit depths,
  and TIFF/DNG limitations.
- [openusd-usdz.md](openusd-usdz.md) — USDZ creation and `usdchecker --arkit`
  validation.
- [lusdzconvert.md](lusdzconvert.md) — `lusdzconvert` native USD → USDZ
  converter.

## Performance and Memory

- [memory-and-performance.md](memory-and-performance.md) — Memory profile and
  performance notes (parsing stages, Tydra conversion, config options).
- [benchmarks.md](benchmarks.md) — Renderer benchmarks: Moana reference scene
  per-element and mid-scale scenes (Kitchen_set, Moore Lane).
- [large-scene.md](large-scene.md) — Loading 10-20 GB production scenes (reference scene,
  ALab, benchmark scene) within a bounded RAM budget, incl. validated VRAM-fit configs.
- [large-scene-vs-openusd.md](large-scene-vs-openusd.md) — benchmark scene compose/write
  benchmark against OpenUSD v26.11.1 (build recipe, phase tables, USDA
  newline-after-prim-spec finding).
- [sanitizers.md](sanitizers.md) — ASan/TSan build and run notes.
- [datarace.md](datarace.md) — Thread-safety and data-race notes.
- [refactor-next.md](refactor-next.md) — historical core (`src/`) optimization & hardening log
  roadmap (fuzzers, corpus gate, phase results).
- [refactor-c-core.md](refactor-c-core.md) — C-style core consolidation,
  compile/object/WASM measurements, API migration, and remaining parity gates.
- [core.md](core.md) — `src/`, core IO, and Tydra
  overview.

## Build, Test, Release

- [build-and-examples.md](build-and-examples.md) — Build commands, CMake
  options, examples and tools.
- [developer.md](developer.md) — PyPI and npm build/publish workflow details
  (wheels.yml / wasmPublish.yml, local verification, release checklist).
- [testing-cpp.md](testing-cpp.md) — C++ test infrastructure and regression
  procedure.
- [testing-reproducibility.md](testing-reproducibility.md) — clean-cache,
  pinned-input verification and focused test targets.
- [python_binding.md](python_binding.md) — Python binding build and maintenance.
- [wine_cl.md](wine_cl.md) — WINE + clang-cl cross-build (pure Win32/Win64).
- [ci.md](ci.md) — Release / publish procedure (version bump, tags, PyPI/npm).
- [mcp.md](mcp.md) — MCP (Model Context Protocol) server interface.

## Tydra Animation

- [tydra-animation-spec-en.md](tydra-animation-spec-en.md) — Tydra animation
  specification.

## Renderer validation reports

Dataset validation reports and captures (Island, ALab, Caldera, USD-WG
assets, Menagerie, Lean conformance runs) are kept machine-local under
`doc/validation/`, which is gitignored; they reference external datasets and
are not part of the repository.

## Active work briefs

- [obj-physics-goal.md](obj-physics-goal.md) — OBJ / URDF / MJCF import and
  physics interoperability work brief.
- [preview-lighting-goal.md](preview-lighting-goal.md) — Preview lighting
  completion audit and the no-hardware-GPU testing constraint.
