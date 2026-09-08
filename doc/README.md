# LightUSD Documentation

Category index for the docs in this directory. Completed or superseded
documents live in [archive/](archive/).

## API and Architecture

- [api-status.md](api-status.md) — OpenUSD schema coverage matrix (per-domain
  tables, implemented / stub / unsupported status, remaining gaps).
- [composition.md](composition.md) — Composition arcs (LIVRPS), instancing, and
  variants; LIVRPS correctness analysis vs OpenUSD PCP.
- [pcp.md](pcp.md) — Composition-graph (PCP / DAG engine) API reference,
  OpenUSD and AOUSD alignment.
- [instancing.md](instancing.md) — OpenUSD instancing model, the Moana Island
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
  LightUSD `next` and Tydra-next, with remediation status.

## Schema Domains

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

- [lusdview.md](lusdview.md) — `lusdview` build & GPU debugging notes.
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
- [benchmarks.md](benchmarks.md) — Renderer benchmarks: Moana Island
  per-element and mid-scale scenes (Kitchen_set, Moore Lane).
- [large-scene.md](large-scene.md) — Loading 10-20 GB production scenes (Island,
  ALab, Caldera) within a bounded RAM budget, incl. validated VRAM-fit configs.
- [sanitizers.md](sanitizers.md) — ASan/TSan build and run notes.
- [datarace.md](datarace.md) — Thread-safety and data-race notes.
- [refactor-next.md](refactor-next.md) — `src/next` optimization & hardening
  roadmap (fuzzers, corpus gate, phase results).
- [refactor-c-core.md](refactor-c-core.md) — C-style core consolidation,
  compile/object/WASM measurements, API migration, and remaining parity gates.
- [lightusd-next.md](lightusd-next.md) — `src/next`, Next IO, and Tydra Next
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
