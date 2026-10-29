// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

#include "next/eval/attribute-eval.hh"
#include "next/layer/layer.hh"
#include "next/pcp/batched-compose.hh"
#include "render-converter.hh"
#include "scene-preview.hh"
#include "texture-cache.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace fs = std::filesystem;
namespace usd = ::lightusd::next;
struct Sample {
  double p[3] = {0, 0, 0};
  float n[3] = {0, 0, 1};
  float color[3] = {.5f, .5f, .5f};
  float opacity = 1;
  float radius = 1;
};
struct Key {
  int64_t x = 0, y = 0, z = 0;
  int direction = 0;
  bool operator<(const Key& b) const {
    return std::tie(x, y, z, direction) < std::tie(b.x, b.y, b.z, b.direction);
  }
  bool operator==(const Key& b) const {
    return x == b.x && y == b.y && z == b.z && direction == b.direction;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = static_cast<uint64_t>(k.x) * 0x9e3779b185ebca87ull;
    h ^= static_cast<uint64_t>(k.y) * 0xc2b2ae3d27d4eb4full;
    h ^= static_cast<uint64_t>(k.z) * 0x165667b19e3779f9ull;
    return static_cast<size_t>(h ^ static_cast<unsigned>(k.direction));
  }
};
struct Accum {
  Sample representative;
  uint64_t rank = UINT64_MAX;
};
struct Material {
  RenderScene scene;
  RenderMaterial value;
};
struct Build {
  const ScenePreviewOptions& opts;
  ScenePreviewResult& result;
  fs::path directory;
  RenderSceneConverter converter;
  TextureDecoder decoder;
  std::map<std::string, Material> materials;
  std::map<std::string, usd::PrimSpec> material_specs;
  std::map<std::string, std::shared_ptr<DecodedImage>> images;
  std::set<std::string> failed_images;
  std::set<std::string> dependencies;
  std::set<std::string> prototypes;
  std::set<std::string> point_prototypes;
  std::map<std::string, std::string> native_prototypes;
  std::map<std::string, std::array<double, 16>> point_prototype_local;
  std::string resolve_prototype(std::string) const;
  std::map<std::string, uint64_t> pending_prototype_placements;
  std::ofstream placements;
  std::unordered_map<std::string, std::array<double, 16>> prototype_inverse;
  std::map<std::string, std::vector<Sample>> prototype_buffers;
  std::unordered_map<Key, Accum, KeyHash> cells;
  std::vector<std::array<double, 6>> extent_reservoir;
  std::vector<usd::PrimSpec> extras;
  usd::LayerMeta meta;
  double cell_size = 0;
  size_t max_cells = 0;
  uint64_t seen_extents = 0;
  uint64_t triangle_count = 0;
  Build(const ScenePreviewOptions&, ScenePreviewResult&, fs::path);
  bool check(const std::string&, uint64_t);
  bool plan(usd::pcp::ComposedBatch&);
  bool finish_plan();
  bool consume(usd::pcp::ComposedBatch&);
  bool insert(const Sample&);
  bool mesh(const usd::Stage&, const usd::UsdPrim&, const double*,
            const std::vector<std::pair<std::string, std::array<double, 16>>>&);
  bool emit(const Sample&,
            const std::vector<std::pair<std::string, std::array<double, 16>>>&);
  bool replay(
      const std::string&, const double*,
      const std::vector<std::pair<std::string, std::array<double, 16>>>&);
  std::string prototype_file(const std::string&) const;
  bool flush_prototype(const std::string&);
  bool queue_placement(
      const std::string&, const double*,
      const std::vector<std::pair<std::string, std::array<double, 16>>>&,
      bool point = false);
  bool finish_placements();
  void prepare_material(const usd::Stage&, const usd::UsdPrim&);
  std::string binding(usd::UsdPrim) const;
  std::array<float, 4> shade(const std::string&, const RenderMesh&, size_t,
                             const float*);
};
uint64_t Hash(const std::string&);
std::array<double, 16> Identity();
bool Inverse(const double*, double*);
void Multiply(const double*, const double*, double*);
Sample Transform(const Sample&, const double*);
std::vector<Sample> ReadPreviewSamples(const usd::UsdPrim&, ScenePreviewKind);
void MakePreviewParticles(usd::Layer&, const std::string&, const Sample*,
                          size_t, double, ScenePreviewKind);
bool WriteProxy(Build&);
void AppendPreviewExtras(const Build&, usd::Layer&);
bool WriteVolume(Build&, usd::Layer&, const std::vector<Sample>&);
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
