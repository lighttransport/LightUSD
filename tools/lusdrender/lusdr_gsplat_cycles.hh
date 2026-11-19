// SPDX-License-Identifier: Apache-2.0
// Gaussian splat renderer using the Blender Cycles kernel
// (--gsplat-kernel cycles). Rays are traced with the LightRT splat primitive
// (lightrt_c_gsplat.h) on the CPU, Vulkan or HIP; SH emission is shaded on
// the CPU with src/tydra/gsplat-kernel.hh.
#pragma once

#include <vector>

#include "lightrt_c_gsplat.h"
#include "lusdr_context.hh"
#include "tydra/gsplat-tracer.hh"

namespace lusdr {

struct GSplatCyclesScene {
  lightusd::gsplat::GSplatTracer tracer;  // splats at the render time + SH
  Bounds bounds;
  size_t skipped_prims = 0;  // non-Gaussian kernels / unreadable prims
  // Motion blur: motion_nkeys key-major arrays of tracer.size() world splats
  // across the shutter (empty / 1 = static).
  std::vector<lrt_gsplat> motion_keys;
  uint32_t motion_nkeys = 1;
  // Animated colour: DC (SH band 0) RGB per key, key-major
  // (motion_nkeys * tracer.size() * 3); empty = render-time colour.
  std::vector<float> motion_dc;
  // Camera motion blur: camera frames at the motion_nkeys shutter times
  // (empty = static camera).
  std::vector<CameraFrame> camera_keys;
};

// DC colour of splat i at shutter time tau (motion_dc must be non-empty).
void GSplatDcAt(const std::vector<float> &dc, uint32_t nkeys, size_t n,
                size_t i, float tau, float out[3]);

// Camera at shutter time tau: origin interpolated linearly, basis by
// normalized linear interpolation (keys at even spacing over [0,1]).
CameraFrame LerpCameraFrame(const std::vector<CameraFrame> &keys, float tau);

// Samples the camera at `times`; fills scene->camera_keys when it moves.
bool BuildCameraMotion(const lightusd::Stage &stage, const std::string &camera,
                       const std::vector<double> &times, GSplatCyclesScene *scene);

// Collects every ParticleField3DGaussianSplat (and ParticleField with a
// gaussianEllipsoid kernel) in world space and builds the splat BVH.
// Returns false when the stage contains no renderable splats.
bool BuildGSplatCyclesScene(const lightusd::Stage &stage, double time,
                            GSplatCyclesScene *scene);

// Samples the splats at `times` (shutter open .. close, ascending) into
// scene->motion_keys, like Cycles' motion steps: position, orientation, scale,
// opacity, DC colour and the object transform are evaluated per step. Fields
// whose splat count changes across the shutter stay static. Returns false if
// no splat moves or changes colour (scene left static).
bool BuildGSplatCyclesMotion(const lightusd::Stage &stage,
                             const std::vector<double> &times,
                             GSplatCyclesScene *scene);

// Renders emission-only splats (Cycles' default splat shader) with jittered
// primary rays and writes opt.output (8-bit sRGB-encoded, like Blender's
// Standard view transform, or scene-linear for .exr). -vk/-vkr or -hip select
// the GPU backend, which keeps device memory under -maxVram (default 4 GiB).
bool RenderGSplatCycles(const Options &opt, const GSplatCyclesScene &scene,
                        const CameraFrame &camera, int height);

// Mixed scenes (meshes + splats): builds the splats into direct->gsplat_* so
// the regular CPU integrator traces them with the Cycles kernel on primary,
// bounce and shadow rays. Returns false when the stage has no splats.
// motion_steps > 1 with an open camera shutter (opt.camera) samples the
// splats across the shutter; the integrator then traces each camera sample
// and its shadow/bounce rays at that sample's shutter time.
bool AttachGSplatCyclesToDirect(const lightusd::Stage &stage, double time,
                                bool srgb, DirectScene *direct,
                                const std::string &camera = std::string(),
                                int motion_steps = 1);

// True when the stage has renderable non-splat geometry (meshes, gprims,
// curves, points), i.e. the splat-only Cycles renderer is not sufficient.
bool StageHasNonSplatGeometry(const lightusd::Stage &stage);

}  // namespace lusdr
