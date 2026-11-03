// ld3d12view - CPU-side lighting setup shared with lmetalview's behaviour:
// authored USD lights (or a fixed three-point studio rig when none exist), the
// dome-light environment image with its irradiance SH9, and the directional
// shadow matrix fitted to the scene bounds.
#pragma once

#include "lmv.h"

#include <vector>

namespace ld3d12 {

// Mirrors `Light` in the mesh shader (64 bytes).
struct LightGPU {
  float position_type[4] = {0, 0, 0, 1};  // xyz position, w = lmv_light_type
  float color_intensity[4] = {1, 1, 1, 1};  // rgb * intensity, w = cos outer (spot)
  float direction[4] = {0, 1, 0, 0};  // xyz towards the light
  float params[4] = {0, 0, 0, 0};     // y = cos inner angle (spot)
};
static_assert(sizeof(LightGPU) == 64, "LightGPU must match the shader");

// Authored lights (dome lights excluded) or the studio rig when none are
// authored. `*authored` reports which one was returned.
std::vector<LightGPU> BuildLights(const lmv_scene* scene, bool* authored);

// First dome light's environment image id, or -1.
int32_t FindDomeImage(const lmv_scene* scene);

// Cosine-convolved SH9 irradiance of the equirect image, already moved into
// display-linear Rec.709 with `color_matrix` (row-major 3x3). Returns false when
// the image has no readable pixels.
bool ComputeEnvironmentSH(const lmv_scene* scene, int32_t image_id,
                          const float color_matrix[9], float sh[9][4]);

// GGX-prefiltered specular environment for image-based lighting: an equirect
// mip chain where mip i holds radiance convolved for roughness i/(levels-1)
// (mip 0 = mirror, last = fully rough). Stored as RGBA16F in the working colour
// space (the shader applies the working -> display matrix). `width`/`height`
// are the size of mip 0; each further mip halves them.
struct PrefilteredEnv {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<std::vector<uint16_t>> levels;  // RGBA half floats per mip
};
bool BuildPrefilteredEnvironment(const lmv_scene* scene, int32_t image_id,
                                 PrefilteredEnv* out);

// Orthographic shadow view-projection (column-major, 0..1 depth) for a
// directional light towards `light_dir`, fitted to the scene bounds.
void DirectionalShadowMatrix(const float light_dir[3], const float bmin[3],
                             const float bmax[3], float out[16]);

}  // namespace ld3d12
