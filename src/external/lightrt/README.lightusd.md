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

- `mtlxrender/` contains the C11 MaterialX document parser, node evaluator,
  texture cache, environment light helpers, and OpenPBR BSDF support from
  `examples/mtlxrender`.
- `lightrt_c_cuda.{cu,h}` contains the upstream CUDA C entry point for the later
  lusdview CUDA RT material path.

The first lusdview integration uses these files as the reference layout and
bridges USD/Tydra material parameters into a LightRT/OpenPBR-compatible CPU
block. The vendored C sources are not linked into lusdview yet; GL/Vulkan raster
currently consume the existing preview material subset, and VK/CUDA RT can be
extended to sample the full LightRT/OpenPBR block.

## License

The top-level `LICENSE` is MIT. Some copied C sources carry
`SPDX-License-Identifier: Apache-2.0` comments; preserved unchanged to keep
upstream provenance intact.
