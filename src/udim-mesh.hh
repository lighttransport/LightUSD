// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "udim-bake.hh"

namespace lightusd {
namespace udim {
struct UVSet {
  std::string name;
  Layout layout;
  std::vector<double> values;         // expanded face-corner float2 data
  std::vector<uint8_t> active_faces;  // empty means all faces
};
struct Stencil {
  std::array<uint32_t, 3> points;
  std::array<uint32_t, 3> corners;
  std::array<double, 3> weights;
  uint32_t face;
};
struct MeshRemap {
  bool topology_changed{false};
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  std::vector<Stencil> vertices;  // one stencil per generated point/corner
  std::vector<uint32_t> face_sources;
  std::map<std::string, std::vector<double>> uv_values;
};
bool RemapDenseMesh(const std::vector<double>& points,
                    const std::vector<int32_t>& counts,
                    const std::vector<int32_t>& indices,
                    const std::vector<UVSet>& sets, const Options&, MeshRemap*,
                    std::string* error);
// Remap numeric primvars with source point/corner/face provenance. Integral
// values may only be interpolated when the contributors agree; joint indices
// and weights are handled together by the skin remapper.
bool RemapNumeric(const std::vector<double>& values, size_t components,
                  const std::string& interpolation, bool integral,
                  const MeshRemap&, size_t memory_budget, std::vector<double>*,
                  std::string* error);
}  // namespace udim
}  // namespace lightusd
