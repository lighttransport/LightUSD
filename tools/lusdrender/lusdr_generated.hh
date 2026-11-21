// SPDX-License-Identifier: Apache-2.0
// Bounded per-prim tessellation shared by CPU and GPU core-loader paths.
#pragma once
#include <cstdint>
#include <vector>

namespace lightusd {
class Stage;
class UsdPrim;
}

namespace lusdr {

struct GeneratedMeshCore {
  std::vector<float> points;
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  std::vector<float> normals;
  std::vector<float> uvs;
  // Optional MaterialX geometry values expanded per refined face corner.
  // Each request has four float lanes per corner plus one validity flag.
  std::vector<std::vector<float>> geomprops;
  std::vector<uint8_t> geomprop_valid;
  bool normals_facevarying{false};
  bool uvs_facevarying{false};
};

// GPU vertex attributes cannot share a position across distinct face corners.
void ExpandGeneratedMeshCornersCore(const GeneratedMeshCore &mesh,
                                    std::vector<float> *points,
                                    std::vector<float> *normals,
                                    std::vector<float> *uvs,
                                    std::vector<uint32_t> *indices);

bool IsTriangleMeshCore(const lightusd::UsdPrim &prim);
bool GenerateAnalyticMeshCore(const lightusd::Stage &stage,
                              const lightusd::UsdPrim &prim, double time,
                              GeneratedMeshCore *out);

}  // namespace lusdr
