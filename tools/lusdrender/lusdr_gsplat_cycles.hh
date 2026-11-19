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
};

// Collects every ParticleField3DGaussianSplat (and ParticleField with a
// gaussianEllipsoid kernel) in world space and builds the splat BVH.
// Returns false when the stage contains no renderable splats.
bool BuildGSplatCyclesScene(const lightusd::Stage &stage, double time,
                            GSplatCyclesScene *scene);

// Samples the splats at `times` (shutter open .. close, ascending) into
// scene->motion_keys, like Cycles' motion steps: position, orientation, scale,
// opacity and the object transform are evaluated per step. Fields whose splat
// count changes across the shutter stay static. Returns false if no splat
// moves (scene left static).
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
bool AttachGSplatCyclesToDirect(const lightusd::Stage &stage, double time,
                                bool srgb, DirectScene *direct);

// True when the stage has renderable non-splat geometry (meshes, gprims,
// curves, points), i.e. the splat-only Cycles renderer is not sufficient.
bool StageHasNonSplatGeometry(const lightusd::Stage &stage);

}  // namespace lusdr
