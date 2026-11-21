# Preview lighting completion audit

This records the current lighting goal and the evidence required to finish it.

## Development constraint

Do not use or probe physical GPUs. Use CPU and Mesa llvmpipe/lavapipe only for
development, testing and qualification. Do not run CUDA, NVML, `nvidia-smi`, or
hardware GPU tests. Use `scripts/with-software-rendering.sh` for builds and
compute checks, and `check-renderers.py --software` for application checks.
Software results must not be represented as hardware performance/VRAM evidence.

After an OOM-related session crash, continue with one build/test workload at a
time and `-j1` builds. Wrap commands with `tools/lusdpreview/memory_guard.py`
(8 GiB group RSS, 4 GiB available-memory reserve by default). The qualification
runner shares this guard and uses a 6 GiB converter host budget. Guard stops
are failed/incomplete checks; keep the reserve instead of retrying unguarded.
See `tools/lusdpreview/README.md` for options and sampling limitations.

## Required work

- Angular/mirrored-ball dome importance sampling: implemented. Verify energy
  against numerical spherical integrals, including partial texels at disk edges.
- Progressive accumulation: implemented. Verify image-noise reduction and
  resets for camera, viewport, settings and scene changes in both applications
  and the experimental render-thread path.
- Better light integration: implemented as solid-angle sampling for rect, disk,
  sphere and cylinder emitters plus cone sampling for spot lights, with
  importance sampling for textured emitters; CPU and Mesa lavapipe parity is
  recorded. Shadow integration still uses binary visibility with explicit
  step/transmittance limits; a separate shadow convergence tolerance and
  exhaustion reporting remain open.
- Light features: partially implemented (2026-11-01). Portable `intensity` and
  `color` light filters are parsed from `lightusd:filter:*`, bound through
  `rel light:filters`, folded into the render-ready light for every backend and
  baked into cached previews; `next_test_light_filter` covers binding,
  parameters, clamped values, unknown/missing targets, custom registration and
  the cache fold. Receiver-dependent shaper/blocker kinds remain open. IES
  profiles parse, export and render in the applications; cached previews bake
  them to portable `light-<hash>.ies` sidecars, and the preview volume kernel
  resamples each profile once into a 32x16 candela table in the light's local
  frame and applies it to emission, so CPU, Vulkan and CUDA agree. Preview-volume
  light/shadow linking is implemented: dense receiver ids, per-light light and
  shadow mask words, independent kernel tests, cached link relationships, and
  CPU/lavapipe coverage (section 10.123).
- Higher-quality media: implemented and re-verified on current code on CPU and
  Mesa lavapipe. The compute test covers independent partial overlaps with
  extinction-weighted emission, fine-halo replacement at its exact boundary,
  thin density peaks with adaptive refinement, diagonal cubic-density optical
  depth, rays beyond 4096 and 16384 steps, primary/surface/shadow exhaustion
  reporting, conservative-medium radiance, and (added 2026-11-01) twelve-bounce
  scattering bounded, non-negative and matching CPU.
- Broader qualification: software qualification is recorded in section 10.124
  (Caldera bounded capture, fog volume matrix, source refinement, threaded GL,
  compute parity). Larger-resolution ALab/Island runs and any hardware
  qualification remain open; hardware runs are historical evidence only and must
  not be restarted.

The whole goal remains incomplete until every item above has current evidence.
