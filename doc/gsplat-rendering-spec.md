# Gaussian splats: schema and rendering specification

This document pins down what LightUSD reads from a Gaussian splat
`ParticleField`, which authored values it diagnoses, and how its Cycles-mode
renderers (lusdrender `--gsplat-kernel cycles`, lusdview, LightRT
`lightrt_c_gsplat.h`) shade it. Each rendering rule is tagged **[Cycles]**
when it matches Blender 5.3 Cycles (the reference, Apache-2.0,
`intern/cycles/kernel/geom/gsplat_intersect.h`) or **[Deviation]** when it
knowingly differs.

## 1. Schema (OpenUSD 26.08 `usdVol`)

| Property | Type (float / half) | Default when missing |
|---|---|---|
| `positions` / `positionsh` | `point3f[]` / `point3h[]` | required; defines N |
| `orientations` / `orientationsh` | `quatf[]` / `quath[]`, (w, x, y, z) | identity |
| `scales` / `scalesh` | `float3[]` / `half3[]`, linear std. deviations | (1, 1, 1) |
| `opacities` / `opacitiesh` | `float[]` / `half[]` | 1 |
| `radiance:sphericalHarmonicsDegree` | `int` | 3 |
| `radiance:sphericalHarmonicsCoefficients[h]` | `float3[]` / `half3[]` | none (white) |
| `projectionModeHint` | `perspective` \| `tangential` | perspective |
| `sortingModeHint` | `zDepth` \| `cameraDistance` \| `rayHitDistance` | zDepth |

- `ParticleField3DGaussianSplat` implies the position, orientation, scale,
  opacity, Gaussian-ellipsoid kernel and SH APIs. A plain `ParticleField` must
  apply a position API and one kernel API (`...KernelGaussianEllipsoidAPI`,
  `...KernelGaussianSurfletAPI`, `...KernelConstantSurfletAPI`).
- When both a float and a half property are authored, the float one wins.
- SH layout: per particle, `(degree+1)^2` RGB triples, band-major
  (l = 0..degree, m = -l..l), reference 3DGS real-SH basis. Colour =
  `SH(dir) + 0.5`.
- Projection and sorting hints are rasterizer hints. Ray tracers ignore them.
- Arrays shorter than N are discarded (the default is used) and longer ones are
  truncated, as OpenUSD does.

## 2. Validation rules (`src/validation/usd-validation-geom.cc`)

Policy: **error** = no meaningful value exists (the particle cannot be
rendered as authored). **Warning** = a renderer has a defined fallback
(clamping, normalizing, discarding, culling). Value rules run on the default
value and on every time sample, and report once per property and sample.

| Rule | Severity | Condition |
|---|---|---|
| `vol.particleField.projectionModeHint` / `sortingModeHint` | error | token outside the allowed set |
| `vol.particleField.position` / `kernel` | warning | plain ParticleField without a position / kernel API |
| `vol.particleField.arraySize` | warning | auxiliary array length != N |
| `vol.particleField.shCount` | warning | SH coefficient count != N·(degree+1)² |
| `vol.particleField.shDegree` | warning | degree < 0 or > 3 |
| `vol.particleField.nonFinite` | error | NaN/Inf component in positions, orientations, scales or opacities |
| `vol.particleField.orientation` | error | zero-length quaternion |
| `vol.particleField.orientation` | warning | \|q\| deviates from 1 by > 1e-3 (renderers normalize) |
| `vol.particleField.scale` | warning | any scale component <= 0 (particle is culled) |
| `vol.particleField.opacity` | warning | opacity outside [0, 1] (renderers clamp) |
| `vol.particleField.sampleCount` | warning | time samples of one property differ in length (no motion blur across them) |

Tests: `test_gsplat_value_rules` in `tests/core/test_validation.cc`.
Round-trip fixtures: `tests/usda/gsplat-schema-sh-degrees.usda`,
`tests/usda/gsplat-schema-motion.usda`, `tests/usda/lusdview-gaussian-half.usda`.

## 3. Rendering semantics (Cycles mode)

### 3.1 Primitive
- **[Cycles]** Bounding sphere radius `r = sqrt(2 ln(opacity / 0.01)) · max(scale)`
  (scaled by the object transform). Splats with opacity < 0.01 are culled.
- **[Cycles]** Unit-Gaussian frame `A = S⁻¹ Rᵀ L⁻¹`, where L is the object's
  linear transform.
- **[Cycles]** Hit at the peak response along the ray:
  `t* = -dot(A(o-μ), A d) / dot(A d, A d)`.
  α = `opacity · exp(-½ |A(o + t* d - μ)|²)`.
- **[Cycles]** Stochastic opaque acceptance: a hit counts when
  `hash(o + t* d, seed) <= α`. The closest accepted hit wins, so a plain
  closest-hit traversal integrates transmittance by Monte Carlo. CPU, Vulkan
  and HIP share the same hash.

### 3.2 Shading
- **[Cycles]** Emission-only shader: a splat emits `SH(view dir) + 0.5` and
  neither reflects nor scatters light.
- **[Cycles]** Splats occlude shadow rays (shadow-all) and are visible to
  BSDF-sampled bounce rays, so their emission lights nearby surfaces.
- **[Deviation]** Colour space: radiance is linear per the USD schema.
  Blender 5.3 treats splat colour as sRGB. `--gsplat-colorspace srgb`
  reproduces Blender, and the comparison tests use it.
- **[Deviation]** lusdview preview (non-path-traced) modes add ambient and
  headlight fill and have no bounce rays. Only the path tracer is held to
  Cycles parity.
- **[Deviation]** The lusdrender `lightrt-bsdf` mesh diffuse BRDF is 2–5% darker
  than Cycles' Diffuse BSDF. This affects meshes in mixed scenes, not splats.

### 3.3 Motion blur
- **[Cycles]** Shutter = `[shutter:open, shutter:close]` of the camera,
  centred on the frame. Ray time τ is uniform in [0, 1] over the shutter
  (stratified per pixel).
- **[Cycles]** K motion keys are sampled evenly over the shutter
  (`--gsplat-motion-steps`). Centre, opacity, A and radius are interpolated
  linearly between the two keys around τ. Orientation uses nlerp.
- **[Cycles]** DC colour and opacity (`radiance_base`) are interpolated only
  for deforming splats (positions, orientations or scales animated, or an
  animated transform). Splats that only change colour/opacity use the
  frame-time value (Blender variant `colouronly`).
- **[Cycles]** Higher SH bands are not interpolated; they use the frame-time
  value.
- **[Cycles]** Camera motion blur: the camera transform is interpolated per ray
  time.
- **[Deviation]** In lusdview, camera and mesh blur come from shutter-segment
  rendering: when path tracing, 16 segments by default whenever the camera has an open shutter,
  with or without splats (`--pt-motion-segments` overrides). Splats follow
  each segment's time through their `--gsplat-motion-steps` keys.

## 4. Validation against Cycles
A render passes when its error against Blender 5.3 is within a small factor of
Blender's own seed-to-seed noise floor (1.1x for lusdrender, 1.3x for the
lusdview path tracer, 1.2x for mixed-scene visibility/emission), with mean
brightness within 1–2%.

- **Without Blender (default ctest):** references are checked in under
  `tests/gsplat/refs/`. Each case holds the Blender-exported scene
  (`scene.usdc`, or the `tests/usda/gsplat-mixed-*` fixtures), Blender's
  seed-0 render (`ref.npz`, float16, full resolution) and the noise floor
  (`meta.json`). Tests: `tool-lusdrender-gsplat-cycles-refs[-vk|-hip]`,
  `tool-lusdrender-gsplat-mixed-refs[-motion|-camera-motion]`,
  `lusdview-gsplat-refs-{vk,hip}`. `tests/gsplat/exr_np.py` reads the EXRs
  without OpenEXR.
- **With Blender:** set `LIGHTUSD_BLENDER` to a Blender 5.3 executable. The
  `*-blender-*` tests then render the references live. To regenerate the
  checked-in references, run:

  ```
  LIGHTUSD_BLENDER=... tests/gsplat/check_cycles_compare.py LUSDRENDER variants --write-refs --lusdcat LUSDCAT
  LIGHTUSD_BLENDER=... tests/gsplat/check_mixed_scene.py LUSDRENDER [--motion|--camera-motion] --write-refs
  ```
- lusdview mixed scenes are also checked against Blender block maps
  (`tests/gsplat/mixed_scene_blocks.json`).
