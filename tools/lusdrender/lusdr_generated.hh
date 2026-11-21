// SPDX-License-Identifier: Apache-2.0
// Bounded per-prim tessellation shared by CPU and GPU next-loader paths.
#pragma once
#include <cstdint>
#include <vector>

namespace lightusd { namespace next {
class Stage;
class UsdPrim;
}}

namespace lusdr {

struct GeneratedMeshNext {
  std::vector<float> points;
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  std::vector<float> normals;
  std::vector<float> uvs;
  bool normals_facevarying{false};
  bool uvs_facevarying{false};
};

// GPU vertex attributes cannot share a position across distinct face corners.
void ExpandGeneratedMeshCornersNext(const GeneratedMeshNext &mesh,
                                    std::vector<float> *points,
                                    std::vector<float> *normals,
                                    std::vector<float> *uvs,
                                    std::vector<uint32_t> *indices);

bool IsTriangleMeshNext(const lightusd::next::UsdPrim &prim);
bool GenerateAnalyticMeshNext(const lightusd::next::Stage &stage,
                              const lightusd::next::UsdPrim &prim, double time,
                              GeneratedMeshNext *out);

}  // namespace lusdr
