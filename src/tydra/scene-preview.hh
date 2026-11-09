// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "pcp/prim-index.hh"
#include "core/host-services.hh"

namespace lightusd {
namespace next {
class AssetResolver;
}
}  // namespace lightusd
namespace lightusd {
namespace tydra {
namespace next {

enum class ScenePreviewKind { Gaussian, Points, Volume };
enum class PreviewComputeBackend { Auto, Cpu, Vulkan, Cuda };

// Per-instance compute selection and diagnostics; never reads process state.
struct PreviewComputeOptions {
  bool software_only = false;
  bool debug_validate = false;
  bool trace_pipeline = false;
  std::shared_ptr<const HostServices> host_services;
  // Exact library filenames are supplied by the caller. No loader search
  // occurs unless the supplied host provider explicitly permits it.
  std::string vulkan_library;
  std::string cuda_driver_library;
  std::string cuda_compiler_library;
};

struct ScenePreviewOptions {
  ScenePreviewKind kind = ScenePreviewKind::Gaussian;
  PreviewComputeBackend compute_backend = PreviewComputeBackend::Auto;
  bool volume_surface_carrier = true;
  // Total renderer ceiling, not just the serialized particle/voxel payload.
  uint64_t gpu_budget_bytes = uint64_t(2) << 30;
  uint64_t host_budget_bytes = uint64_t(16) << 30;
  uint64_t texture_budget_bytes = uint64_t(256) << 20;
  uint32_t texture_max_edge = 512;
  // Longest-axis sampling resolution. Zero automatically coarsens to budget.
  uint32_t resolution = 0;
  double time_code = 0;
  // Internal world-space sampling window for camera-driven source refinement.
  bool region_enabled = false;
  std::array<double, 3> region_min{}, region_max{};
  double sampling_cell_size = 0;
  std::string base_preview_path;
  size_t batch_prims = 100000;
  size_t batch_roots = 0;  // deterministic small-batch qualification
  std::string cache_directory;
  bool refresh = false;
  ::lightusd::next::pcp::CompositionOptions composition;
  // False cancels. Called at bounded composition/sampling/output boundaries.
  std::function<bool(const std::string&, uint64_t)> progress;
  PreviewComputeOptions compute_options;
  // Optional process-level memory observations. Zero/unavailable observations
  // do not replace the explicit payload and allocation budgets.
  std::shared_ptr<const HostServices> host_services;
  uint64_t min_available_host_bytes = 0;
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
  uint64_t refined_samples = 0;
  uint64_t estimated_gpu_bytes = 0;
  double voxel_size = 0;
  double elapsed_seconds = 0;
  bool refinement_region = false;
  std::array<double, 3> refinement_min{}, refinement_max{};
  double bounds_min[3] = {0, 0, 0};
  double bounds_max[3] = {0, 0, 0};
};

const char* ScenePreviewKindName(ScenePreviewKind kind);
bool ParseScenePreviewKind(const std::string& text, ScenePreviewKind* kind);
// Quarter of installed VRAM, reduced under memory pressure. Zero capacity
// (CPU-only conversion) selects the portable 2 GiB preview ceiling.
uint64_t ScenePreviewGpuBudget(uint64_t capacity, uint64_t available = 0);
bool QueryScenePreviewGpuMemory(
    uint64_t* capacity, uint64_t* available,
    const std::shared_ptr<const HostServices>& host_services = {},
    const std::string& library_path = {});
// Relative deterministic default; callers may set cache_directory explicitly.
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

struct ScenePreviewLodTile {
  std::string path;
  std::array<double, 3> minimum{}, maximum{};
  std::array<uint64_t, 4> bytes{};
  double cell_size = 0;
};
struct ScenePreviewLods {
  bool success = false;
  std::string root_path, error;
  double cell_size = 0;
  uint64_t gpu_budget_bytes = 0;
  std::vector<ScenePreviewLodTile> tiles;
};
struct ScenePreviewLodCamera {
  std::array<double, 3> eye{}, forward{0, 0, -1}, up{0, 1, 0};
  double focal_pixels = 500;
  double near_plane = .01;
  double far_plane = 1e30;
  double exposure = 0;
  bool orthographic = false;
};
using ScenePreviewLodSelections =
    std::map<std::string, std::map<std::string, std::string>>;
// Build portable USD variant levels from cached geometry, without recomposing
// the original scene. Only the selected levels enter the renderer working set.
ScenePreviewLods BuildScenePreviewLods(const ScenePreviewResult& result,
                                       const ScenePreviewOptions& options);
ScenePreviewLodSelections SelectScenePreviewLods(
    const ScenePreviewLods& lods, const ScenePreviewLodCamera& camera,
    double pixel_error = 2, const ScenePreviewLodSelections& previous = {});

ScenePreviewLodCamera InitialScenePreviewLodCamera(
    const ScenePreviewResult& result, const std::string& lod_root,
    const std::string& camera_name, int viewport_height, double fit_scale = 1.8,
    std::array<double, 3> direction = {1, .6, 1});

// Resample source geometry inside a camera-visible region and retain the
// existing samples outside it. A successful no-op returns the original cache.
ScenePreviewResult RefineScenePreview(const std::string& source,
                                      const ScenePreviewResult& base,
                                      const ScenePreviewOptions& options,
                                      const ScenePreviewLodCamera& camera,
                                      int viewport_width, int viewport_height,
                                      double pixel_error = 2);

struct PreviewVolumeSettings {
  bool medium = false;
  bool scene_lights = true;
  bool shadows = true;
  float density = 1;
  float scattering = 1;
  float emission = 0;
  float anisotropy = 0;
  uint32_t max_steps = 4096;
  uint32_t shadow_steps = 1024;
  float step_size = .5f;
  float integration_error = .001f;
  float transmittance_cutoff = 1e-7f;
  uint32_t scatter_bounces = 1;
  uint32_t scatter_samples = 2;
  uint32_t scatter_seed = 0;
  uint32_t light_samples = 8;
  uint32_t accumulation_frames = 64;
  bool operator==(const PreviewVolumeSettings& b) const {
    return medium == b.medium && scene_lights == b.scene_lights &&
           shadows == b.shadows && density == b.density &&
           scattering == b.scattering && emission == b.emission &&
           anisotropy == b.anisotropy && max_steps == b.max_steps &&
           shadow_steps == b.shadow_steps && step_size == b.step_size &&
           integration_error == b.integration_error &&
           transmittance_cutoff == b.transmittance_cutoff &&
           scatter_bounces == b.scatter_bounces &&
           scatter_samples == b.scatter_samples &&
           scatter_seed == b.scatter_seed && light_samples == b.light_samples &&
           accumulation_frames == b.accumulation_frames;
  }
};
struct ScenePreviewRequest {
  bool enabled = false;
  bool budget_explicit = false;
  bool camera_lod = true;
  bool source_refinement = true;
  bool direct_volume = true;
  bool native_volume = false;
  PreviewVolumeSettings volume;
  std::string source;
  double lod_pixel_error = 2;
  ScenePreviewOptions options;
  ScenePreviewResult result;
  ScenePreviewLods lods;
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
