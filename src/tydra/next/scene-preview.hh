// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "next/pcp/prim-index.hh"

namespace lightusd {
namespace next {
class AssetResolver;
}
}  // namespace lightusd
namespace lightusd {
namespace tydra {
namespace next {

enum class ScenePreviewKind { Gaussian, Points, Volume };

struct ScenePreviewOptions {
  ScenePreviewKind kind = ScenePreviewKind::Gaussian;
  // Total renderer ceiling, not just the serialized particle/voxel payload.
  uint64_t gpu_budget_bytes = uint64_t(2) << 30;
  uint64_t host_budget_bytes = uint64_t(16) << 30;
  uint64_t texture_budget_bytes = uint64_t(256) << 20;
  uint32_t texture_max_edge = 512;
  // Longest-axis sampling resolution. Zero automatically coarsens to budget.
  uint32_t resolution = 0;
  double time_code = 0;
  size_t batch_prims = 100000;
  size_t batch_roots = 0;  // deterministic small-batch qualification
  std::string cache_directory;
  bool refresh = false;
  ::lightusd::next::pcp::CompositionOptions composition;
  // False cancels. Called at bounded composition/sampling/output boundaries.
  std::function<bool(const std::string&, uint64_t)> progress;
};

struct ScenePreviewResult {
  bool success = false;
  bool cache_hit = false;
  std::string usd_path;
  // Volume proxies also cache a colored isosurface carrier for existing
  // triangle renderers. usd_path remains the portable UsdVol representation.
  std::string render_usd_path;
  std::string error;
  std::string warning;
  uint64_t source_prims = 0;
  uint64_t source_meshes = 0;
  uint64_t source_instances = 0;
  uint64_t samples = 0;
  uint64_t estimated_gpu_bytes = 0;
  double voxel_size = 0;
  double elapsed_seconds = 0;
  double bounds_min[3] = {0, 0, 0};
  double bounds_max[3] = {0, 0, 0};
};

const char* ScenePreviewKindName(ScenePreviewKind kind);
bool ParseScenePreviewKind(const std::string& text, ScenePreviewKind* kind);
// Quarter of installed VRAM, reduced under memory pressure. Zero capacity
// (CPU-only conversion) selects the portable 2 GiB preview ceiling.
uint64_t ScenePreviewGpuBudget(uint64_t capacity, uint64_t available = 0);
bool QueryScenePreviewGpuMemory(uint64_t* capacity, uint64_t* available);
std::string DefaultScenePreviewCacheDirectory();

// The returned USD is a portable cache root with relative USDC/VDB sidecars.
// A cache hit performs dependency checks, but never composes source geometry.
ScenePreviewResult ConvertScenePreview(
    const std::string& source, ::lightusd::next::AssetResolver& resolver,
    const ScenePreviewOptions& options = {});

// Export a cache entry as a portable directory (root.usdc plus sidecars).
// An existing destination is never overwritten.
bool ExportScenePreview(const ScenePreviewResult& result,
                        const std::string& directory, std::string* error);

struct ScenePreviewRequest {
  bool enabled = false;
  bool budget_explicit = false;
  ScenePreviewOptions options;
};
// Consume the same preview-specific switches before each app's normal parser.
// Other arguments, including renderer/camera/time/variant options, are
// retained.
bool ExtractScenePreviewArguments(int* argc, char** argv,
                                  ScenePreviewRequest* request,
                                  std::string* error);
bool PrepareScenePreview(std::string* source, ScenePreviewRequest& request,
                         uint64_t gpu_capacity, std::string* error);

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
