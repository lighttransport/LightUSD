#pragma once
// UsdVol ParticleField (e.g. ParticleField3DGaussianSplat) render data with
// the schema's array rules applied: `positions` defines the particle count;
// other per-particle arrays are truncated when longer and replaced by the
// defaults when shorter (identity rotation, unit scale, opacity 1, SH DC
// (0.5, 0.5, 0.5) at degree 0); float arrays win over the `*h` half arrays
// when non-empty.
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

struct RenderParticleField {
  std::string prim_path;
  std::string type_name;               // "ParticleField3DGaussianSplat", ...
  double world_matrix[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  size_t count = 0;
  std::vector<float> positions;        // xyz * count (local space)
  std::vector<float> orientations;     // quaternion (real, i, j, k) * count
  std::vector<float> scales;           // linear xyz * count
  std::vector<float> opacities;        // [0, 1] * count
  int sh_degree = 0;                   // spherical-harmonics degree in use
  // (sh_degree + 1)^2 rgb coefficients per particle, grouped per particle in
  // the authored order.
  std::vector<float> sh_coefficients;
  std::string kernel = "none";         // gaussianEllipsoid, gaussianSurflet, constantSurflet
  std::string projection_mode_hint = "perspective";
  std::string sorting_mode_hint = "zDepth";
  std::vector<std::string> warnings;
};

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
