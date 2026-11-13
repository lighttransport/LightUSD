# LightRT Vendor Import (maintained fork)

Upstream: https://github.com/syoyo/lightrt (local: `~/work/lightrt`)

This is a **copied vendor import that has diverged into a maintained fork**, not a
pristine snapshot and not a git submodule. Both sides have evolved: lightusd added
features/fixes here, and upstream advanced independently. Keep that in mind before
re-syncing — a blind overwrite would drop lightusd-only code that `lusdrender`
depends on.

## Last re-sync

3-way merged against upstream `main` @ `c5ca37b` on 2026-06-28. Per-file fork
bases (the upstream commit each file was originally imported from, used as the
merge base):

- `lightrt_c.{c,h}`, `lightrt_c_tri.{c,h}` — base `1759b1e`
- `lightrt_c_vk.{c,h}` — base `7024f46`
- `lightrt_vkew.{c,h}`, `vk/shaders/*.comp` — track upstream verbatim

To re-sync again: for each diverged file run
`git merge-file --diff3 ours <base> <upstream-main>`, resolve any conflicts
(they cluster in `rtx_scene_build_core`; keep the staging-buffer side — see
below), then rebuild + run `ctest -R tool-lusdrender-smoke`.

## lightusd-local patches (must survive a re-sync)

Core CPU kernel (`lightrt_c_tri.{c,h}`), not upstream:
- `lrt_tri_scene_build_indexed()` (indexed triangle build)
- hit-vertex recovery from BVH leaves (drops the resident vertex soup, −1.5 GB)
- memory-capped + parallelized 22M-instance TLAS build
- cache-coherent leaf-slot color reorder (opt-in)
- MinGW/MSVC Windows portability (`_aligned_malloc`, `<threads.h>` gating,
  `_BitScanReverse64`/`Forward` fallbacks; 32-bit MSVC scans the two halves
  with `_BitScanReverse`)

Vulkan (`lightrt_c_vk.c`):
- ray-query AS build inputs are **staged into device-local memory** (the AS build
  input must be device-local on some drivers — "NVIDIA requires it"). Upstream
  uploads them host-visible; on a re-sync, keep the lightusd staging path.

lightusd-only files (no upstream counterpart — never overwrite):
- `lightrt_c_d3d11.{cpp,h}`, `d3d/shaders/*` — the Direct3D 11 compute backend
- `vk/shaders/compile_shaders.sh` — relocated next to the shaders (upstream keeps
  it under `scripts/`); its `SH` path is adjusted accordingly
- `README.lightusd.md` (this file), `test_indexed_build.c`

## Imported / tracked files

`LICENSE`, `lightrt_c.{c,h}`, `lightrt_c_tri.{c,h}`, `lightrt_c_vk.{c,h}`,
`lightrt_vkew.{c,h}`, `vk/shaders/{trace_bvh,build_morton,trace_ray_query,shade_analytic}.{comp,spv.h}`.

The committed `*.spv.h` are regenerated from the `*.comp` with
`vk/shaders/compile_shaders.sh` (needs a `GL_EXT_ray_query`-capable glslang).

## MaterialX / OpenPBR snapshot

The `mtlxrender/` subtree and CUDA C entry files were copied from local LightRT
commit `13ec9814f2da84846d64828b0dd96daa33df3582`. The existing root-level
CPU/Vulkan/HIP files remain the lightusd-maintained fork described above.

- 2026-11-11: `mtlxrender/mtlx_doc.{c,h}`, `mtlx_eval.c` and the new
  `mtlx_stdlib_defs.inc` were re-synced from LightRT branch `mtlx-conformance`
  (`8dd2a78`, on top of `formal-lean` `83471f4`): standard-library nodedef
  defaults at load, nearest-scope `fileprefix`, node fixes found by the
  Lean vectors, and a V-axis correction for `ramptb`, `ramp4` and `splittb`. `mtlx_stdlib_defs.inc`
  is generated (`lake exe mtlxspec-gen`); do not edit it by hand.
- 2026-11-12: `mtlxrender/bsdf.c` is synchronized with LightRT
  `mtlx-conformance` commit `87f8a75`: the weighted Schlick response now
  attenuates diffuse/subsurface energy, with a standalone white-furnace test
  in LightRT and a CTest integration in `tools/lusdrender`.
- 2026-11-13: `mtlxrender/{bsdf.c,bsdf.h,mtlx_eval.c,mtlx_eval.h}` matches
  LightRT `ae93885` for bounded sheen mixtures and the Zeltner mode.
- 2026-11-13: `mtlxrender/mtlx_eval.c` matches LightRT `398db9a`, including
  the zero default for BSDF, EDF and VDF mix weights.
- 2026-11-13: `mtlxrender/mtlx_eval.c` matches LightRT `1daffcb` for
  angle-independent opaque-diffuse layer throughput.
- 2026-11-13: `mtlxrender/mtlx_eval.c` matches LightRT `e700f11` for
  view-dependent sheen layer throughput.
- 2026-11-13: `mtlxrender/{bsdf.c,bsdf.h}` matches LightRT `78d407a` for
  an independently tested isotropic MaterialX conductor response building block.
- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `8967e2a` for bounded independent conductor closures and diffuse layering.
- 2026-11-13: `mtlxrender/{bsdf.c,bsdf.h,mtlx_eval.c,mtlx_eval.h}` matches
  LightRT `7dc1d03` for anisotropic conductor response and tangent routing.
- 2026-11-13: `mtlxrender/{bsdf.c,bsdf.h}` matches LightRT `99e5a9e` for
  anisotropic conductor proposals and normalized mixture sampling.
- 2026-11-13: `mtlxrender/bsdf.c` matches LightRT `ac60f55` for the
  aggregate nonfinite roughness guard.
- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `4271ac7` for channel-preserving diffuse closure multiplication.
- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `a46887c` for preserving bounded diffuse closure mixtures.
- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `853120a` for qualitative Oren–Nayar and Burley diffuse selection/response.
- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `5eaf175` for energy-compensated rough diffuse shading and its selection.
- 2026-11-13: `mtlxrender/bsdf.c` matches LightRT `5827793` for isotropic
  height-correlated Smith visibility in the base and coat GGX lobes.
- 2026-11-12: `mtlxrender/bsdf.{c,h}` is synchronized with LightRT
  `mtlx-conformance` commit `2839b5c`: `bsdf_lobe_weights()` exposes the four
  importance-sampling probabilities for validation AOVs. LightRT's
  `test-white-furnace` still passes after the API addition.
- 2026-11-12: `mtlxrender/mtlx_eval.c`'s checkerboard evaluator matches
  `NG_checkerboard_color3`: it subtracts `uvoffset` before flooring and maps odd
  cell parity to `color1`. Four appended Lean vectors exposed the previous phase
  inversion and offset-sign mismatch; LightRT commit `622d6ee` carries the same
  fix and the canonical vector rows.
- 2026-11-12: the procedural grid evaluator uses MaterialX floor-based modulo
  (`x - y*floor(x/y)`) instead of C `fmod`, matching `NG_grid_color3` for
  negative UVs. The appended grid vectors cover negative coordinates,
  subtractive offsets, staggered rows and the strict thickness threshold.
  LightRT commit `348082f` carries the same implementation and vectors; its
  1,296-vector replay and 67 independent node-truth checks pass.
- 2026-11-12: the Tydra MaterialX constant folder evaluates
  `NG_crosshatch_color3` as the union of its two rounded diagonal line tests.
  The rational vectors cover diagonal hits, the gap between diagonals, negative
  UVs, subtractive offset and stagger selection; LightRT's existing evaluator
  replays the same cases with the floor-modulo correction from `348082f`.
- 2026-11-12: `ND_hexagon_float` is evaluated from its post-clamp residual sign,
  matching the selected LightRT signed-distance samples. Lean issue `LEAN-208`
  records that the reference graph sends negative values to `sqrt` and relies
  on NaN comparison behavior to select its inside branch. Four exact and four
  approximate vectors cover center, radius vertex, outside and translated center.
- 2026-11-13: `mtlxrender/texture.{c,h}` adds LightRT's optional round-up mip
  chain API (`5d28320`) to the vendored evaluator. LightUSD adds
  `texcache_preload_mips()` and a `buildMipmaps` option on
  `CreateMaterialXSurfaceRuntime`; derivative-aware image nodes use trilinear
  LOD when the cache has mip chains. The ordinary preload path is unchanged.
- 2026-11-13: LightUSD's `mtlx_eval_volume` rejects unsupported VDF and EDF
  closure categories, unknown connected value-node categories, and overly deep
  graphs and unresolved authored input connections. This is a local change to
  preserve on the next LightRT sync. Volume `measured_edf` is rejected until
  its IES profile is evaluated; the surface fallback is unchanged.
- 2026-11-13: Material surface and volume inputs can resolve a nodegraph output
  to the shader node; unresolved outputs and non-default suboutputs are rejected.
- 2026-11-13: Tydra embeds the evaluator for sampled volume coefficients,
  with a private stb decoder and an asset-reader callback in the texture cache.
  Tydra uses its asset resolver and a decoded-memory cap; volume graph traversal
  has a 100,000-visit work limit. The cache failure flag can be cleared between
  evaluations so a reusable evaluator can retry a failed texture read. Optional
  lazy mip chains are charged to the resolver-backed decoded pixel budget.
  The cache can also borrow shared-decoder mip pixels and bound native base
  image loading; keep borrowed data alive until the cache is destroyed.
  Preserve these local changes during sync.
- 2026-11-13: VDF/EDF multiplication uses the declared closure input and
  supports connected color weights; VDF coefficients retain signed RGB
  weights under a coefficient-wise interpretation of the PBR specification's
  distribution multiply equation. The MDL implementation instead uses
  `1 - weight * (1 - absorption)` for its absorption coefficient. The spec
  describes a normalized phase function and separate coefficients, so this
  interpretation and cross-target parity remain to be established.
- 2026-11-13: Volume evaluation retains up to 64 distinct scattering phase
  lobes while preserving aggregate coefficients and the legacy single-phase
  field. Identical anisotropies are combined; evaluation fails on overflow.
  This changes `MtlxVolumeParams` and all consumers must be rebuilt together.
- `mtlxrender/` contains the C11 MaterialX document parser, node evaluator,
  texture cache, environment light helpers, and OpenPBR BSDF support from
  `examples/mtlxrender`.
- `lightrt_c_cuda.{cu,h}` contains the upstream CUDA C entry point for the later
  lusdview CUDA RT material path.

The lusdview material bridge links the vendored MaterialX parser, evaluator,
texture cache and BSDF, and adapts USD/Tydra material parameters into the
LightRT/OpenPBR CPU block. `lusdrender --mtlx` also retains a graph runtime for
per-hit CPU evaluation. GL/Vulkan raster still consume the existing preview
material subset; VK/CUDA RT can be extended to sample the full LightRT/OpenPBR
block.

## License

The top-level `LICENSE` is MIT. Some copied C sources carry
`SPDX-License-Identifier: Apache-2.0` comments; preserved unchanged to keep
upstream provenance intact.

- 2026-11-13: `mtlxrender/bsdf.c` matches LightRT `862e1f6` for
  anisotropic conductor visible-normal proposals.

- 2026-11-13: `mtlxrender/{bsdf.c,bsdf.h,mtlx_eval.c,mtlx_eval.h}`
  matches LightRT `786c4ad` for independent conductor thin films.

- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `c3339f4` for per-conductor normals and matching sampling.

- 2026-11-13: `mtlxrender/{bsdf.c,mtlx_eval.c,mtlx_eval.h}` matches LightRT
  `123cb89` for per-conductor retroreflection and its sampling PDF.
