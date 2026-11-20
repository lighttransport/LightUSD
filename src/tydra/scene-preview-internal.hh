// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "eval/attribute-eval.hh"
#include "layer/layer.hh"
#include "pcp/batched-compose.hh"
#include "render-converter.hh"
#include "scene-preview-compute.hh"
#include "scene-preview.hh"
#include "texture-cache.hh"

namespace lightusd {
namespace tydra {
namespace preview_detail {
namespace fs = std::filesystem;
namespace usd = ::lightusd;
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
  uint64_t order = 0;  // First insertion of this cell, preserved across coarsening.
};
// Open-addressing Key -> Accum map for the sampling grid (hot path: one
// lookup per emitted sample). Entries are contiguous; emplace may relocate
// them, so iterators are invalidated by emplace/reserve like std::vector.
class CellMap {
 public:
  using value_type = std::pair<Key, Accum>;
  using iterator = value_type*;
  using const_iterator = const value_type*;
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  iterator begin() { return entries_.data(); }
  iterator end() { return entries_.data() + entries_.size(); }
  const_iterator begin() const { return entries_.data(); }
  const_iterator end() const { return entries_.data() + entries_.size(); }
  iterator find(const Key& k) {
    if (slots_.empty()) return end();
    const size_t mask = slots_.size() - 1;
    for (size_t i = Slot(k) & mask;; i = (i + 1) & mask) {
      const uint32_t e = slots_[i];
      if (!e) return end();
      if (entries_[e - 1].first == k) return &entries_[e - 1];
    }
  }
  std::pair<iterator, bool> emplace(const Key& k, const Accum& a) {
    iterator it = find(k);
    if (it != end()) return {it, false};
    if ((entries_.size() + 1) * 4 > slots_.size() * 3)
      Rebuild(std::max<size_t>(64, slots_.size() * 2));
    entries_.emplace_back(k, a);
    Place(entries_.size() - 1);
    return {&entries_.back(), true};
  }
  void reserve(size_t n) {
    entries_.reserve(n);
    size_t want = 64;
    while (want * 3 < n * 4) want *= 2;
    if (want > slots_.size()) Rebuild(want);
  }
  void clear() {
    entries_.clear();
    slots_.clear();
  }
  void rehash(size_t) {
    entries_.shrink_to_fit();
    slots_.shrink_to_fit();
  }
  void swap(CellMap& o) {
    entries_.swap(o.entries_);
    slots_.swap(o.slots_);
  }

 private:
  static size_t Slot(const Key& k) {
    uint64_t h = KeyHash()(k);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<size_t>(h);
  }
  void Place(size_t index) {
    const size_t mask = slots_.size() - 1;
    size_t i = Slot(entries_[index].first) & mask;
    while (slots_[i]) i = (i + 1) & mask;
    slots_[i] = static_cast<uint32_t>(index + 1);
  }
  void Rebuild(size_t n) {
    slots_.assign(n, 0);
    for (size_t e = 0; e < entries_.size(); ++e) Place(e);
  }
  std::vector<value_type> entries_;
  std::vector<uint32_t> slots_;  // entry index + 1; 0 = empty
};
struct CellGrid {
  static constexpr size_t kShards = 16;
  std::array<CellMap, kShards> shards;
  size_t count = 0;
  uint64_t next_order = 0;

  static size_t shardIndex(const Key& key) {
    const uint64_t hash = KeyHash()(key);
    return (hash ^ (hash >> 32)) % kShards;
  }
  size_t size() const { return count; }
  void place(const Key& key, const Sample& sample, uint64_t rank) {
    count += placeInShard(shards[shardIndex(key)], key, sample, rank, next_order++);
  }
  static bool placeInShard(CellMap& shard, const Key& key, const Sample& sample,
                           uint64_t rank, uint64_t order) {
    auto entry = shard.emplace(key, Accum{sample, rank, order});
    if (!entry.second && rank < entry.first->second.rank) {
      entry.first->second.representative = sample;
      entry.first->second.rank = rank;
    }
    return entry.second;
  }
};
struct Material {
  RenderScene scene;
  RenderMaterial value;
};
struct GeometryBatch;
// Output of one mesh sampled off the main thread (see Build::drain_meshes).
struct MeshJobOut {
  std::vector<Sample> samples;
  uint64_t meshes = 0;
  std::string error;
};
struct Build {
  const ScenePreviewOptions& opts;
  ScenePreviewResult& result;
  fs::path directory;
  TextureDecoder decoder;
  std::map<std::string, Material> materials;
  std::map<std::string, usd::PrimSpec> material_specs;
  std::map<std::string, std::shared_ptr<DecodedImage>> images;
  std::set<std::pair<std::string, bool>> failed_images;
  void appendTextureWarnings();
  std::set<std::string> dependencies;
  std::set<std::string> prototypes;
  std::set<std::string> point_prototypes;
  std::map<std::string, std::string> native_prototypes;
  std::map<std::string, std::array<double, 16>> point_prototype_local;
  // Placement targets never traversed as prototypes (pruned, inactive or
  // absent): they render nothing, as in USD; reported in the warning.
  std::set<std::string> missing_prototypes;
  std::string resolve_prototype(std::string) const;
  std::map<std::string, uint64_t> pending_prototype_placements;
  std::ofstream placements;
  std::unordered_map<std::string, std::array<double, 16>> prototype_inverse;
  std::map<std::string, std::vector<Sample>> prototype_buffers;
  CellGrid cells;
  std::map<std::string, uint32_t> source_material_ids;
  std::vector<std::string> source_material_paths;
  std::map<uint64_t, std::array<double, 6>> source_bounds;
  fs::path source_directory;
  bool source_capture = false;
  bool source_save_catalog(usd::Layer&);
  bool bake_lights();
  bool source_save_index();
  bool source_load_index(const fs::path&);
  bool source_mesh(
      const usd::Stage&, const usd::UsdPrim&, const RenderMesh&, const double*,
      const std::vector<std::pair<std::string, std::array<double, 16>>>&);
  bool source_placement(
      const std::string&, const double*,
      const std::vector<std::pair<std::string, std::array<double, 16>>>&);
  bool source_replay(uint64_t, const double*, size_t depth = 0);
  std::vector<std::array<double, 6>> extent_reservoir;
  std::vector<usd::PrimSpec> extras;
  usd::LayerMeta meta;
  std::unique_ptr<PreviewCompute> compute;
  std::unique_ptr<GeometryBatch> geometry;
  bool geometry_backend();
  bool queue_geometry(
      const Sample*, const float*, size_t, float, const std::string&, size_t,
      const std::vector<std::pair<std::string, std::array<double, 16>>>&,
      const double*);
  bool flush_geometry();
  double cell_size = 0;
  size_t max_cells = 0;
  uint64_t seen_extents = 0;
  uint64_t triangle_count = 0;
  Build(const ScenePreviewOptions&, ScenePreviewResult&, fs::path);
  bool check(const std::string&, uint64_t);
  bool plan(usd::pcp::ComposedBatch&);
  bool finish_plan();
  bool consume(usd::pcp::ComposedBatch&);
  bool insert(const Sample&, bool filter_region = true);
  // Insert with a precomputed grid key and rank (valid for cell_size).
  bool place(const Sample&, const Key&, uint64_t rank);
  // Parallel replay of top-level placements (see finish_placements).
  struct TopPlacement {
    std::string key;
    std::array<double, 16> world;
  };
  bool replay_top_batch(std::vector<TopPlacement>* batch);
  std::map<std::string, std::vector<Sample>> prototype_sample_cache;
  size_t prototype_sample_cache_bytes = 0;
  std::map<std::string, size_t> prototype_sample_counts;
  // out is set only by drain_meshes worker threads: samples are
  // buffered in *out instead of emitted, and shared result fields untouched.
  bool mesh(const usd::Stage&, const usd::UsdPrim&, const double*,
            const std::vector<std::pair<std::string, std::array<double, 16>>>&,
            MeshJobOut* out = nullptr);
  // Deferred renderable prims of the current batch, sampled in parallel and
  // emitted in traversal order by drain_meshes().
  struct MeshJob {
    uint32_t prim = 0;
    std::array<double, 16> world{};
    std::vector<std::pair<std::string, std::array<double, 16>>> ancestors;
  };
  std::vector<MeshJob> mesh_jobs;
  std::vector<std::string> pending_flushes;  // popped prototype ancestors
  std::mutex shade_mutex;
  bool parallel_meshes();
  bool drain_meshes(const usd::Stage&);
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
struct GeometryBatch {
  struct Record {
    std::string material;
    size_t face = 0, count = 0;
    std::vector<std::pair<std::string, std::array<double, 16>>> ancestors;
    std::array<double, 3> origin{};
  };
  Build& build;
  std::vector<uint32_t> input = std::vector<uint32_t>(16), weights;
  std::map<size_t, uint32_t> weight_offsets;
  std::vector<Record> records;
  size_t samples = 0;
  explicit GeometryBatch(Build& b) : build(b) {}
  bool add(const Sample*, const float*, size_t, float, const std::string&,
           size_t,
           const std::vector<std::pair<std::string, std::array<double, 16>>>&,
           const double*);
  bool flush();
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
}  // namespace tydra
}  // namespace lightusd
