// SPDX-License-Identifier: Apache-2.0
// CPU reference renderer for Gaussian splats using the Blender Cycles kernel
// ported in src/tydra/gsplat-kernel.hh (--gsplat-kernel cycles).
#pragma once

#include "lusdr_context.hh"
#include "tydra/gsplat-tracer.hh"

namespace lusdr {

struct GSplatCyclesScene {
  lightusd::gsplat::GSplatTracer tracer;
  Bounds bounds;
  size_t skipped_prims = 0;  // non-Gaussian kernels / unreadable prims
};

// Collects every ParticleField3DGaussianSplat (and ParticleField with a
// gaussianEllipsoid kernel) in world space and builds the splat BVH.
// Returns false when the stage contains no renderable splats.
bool BuildGSplatCyclesScene(const lightusd::Stage &stage, double time,
                            GSplatCyclesScene *scene);

// Renders emission-only splats (Cycles' default splat shader) with jittered
// primary rays and writes opt.output (8-bit sRGB-encoded, like Blender's
// Standard view transform).
bool RenderGSplatCycles(const Options &opt, const GSplatCyclesScene &scene,
                        const CameraFrame &camera, int height);

}  // namespace lusdr
