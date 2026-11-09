// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-scene-nodes.hh"
#include "render-data-math.hh"
#include "render-data-points.hh"
#include "render-data-mesh.hh"
#include "render-data-texture.hh"
#include "render-data-light.hh"
#include "render-data-material.hh"
#include "render-data-volume.hh"
#include "render-data-particles.hh"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace lightusd { namespace tydra { namespace next {

struct ConversionDiagnostic {
  ConversionDisposition disposition = ConversionDisposition::Failed;
  std::string prim_path;
  std::string message;
};

//
// RenderScene - top-level container
//
class RenderScene {
 public:
  RenderScene();
  ~RenderScene();

  // Copies share chunk-backed geometry copy-on-write. Catalog vectors are
  // copied, while optional skin/shader records remain shared and immutable;
  // transactional updates replace whole affected resources.
  RenderScene(RenderScene&&) noexcept;
  RenderScene& operator=(RenderScene&&) noexcept;
  RenderScene(const RenderScene&);
  RenderScene& operator=(const RenderScene&);

  // Scene metadata
  std::string name;
  std::string default_prim;
  float meters_per_unit = 1.0f;
  enum class UpAxis : uint8_t { Y = 0, Z } up_axis = UpAxis::Y;

  // OpenUSD color-management state. Shader color constants in this scene are
  // expressed in working_color_space. The row-major matrix maps linear values
  // from that space into Three.js/native display-linear Rec.709.
  std::string render_settings_path;
  std::string working_color_space = "lin_rec709_scene";
  float working_to_display_linear[9] = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};

  // Time range
  double start_time = 0.0;
  double end_time = 0.0;
  double frames_per_second = 24.0;

  // Scene data
  std::vector<SceneNode> nodes;
  std::vector<RenderMesh> meshes;
  std::vector<RenderPoints> points;
  std::vector<RenderCurves> curves;
  std::vector<RenderPointInstancer> point_instancers;
  std::vector<RenderPointInstanceDraw> point_instance_draws;
  std::vector<RenderMaterial> materials;
  std::vector<RenderTexture> textures;
  std::vector<RenderUDIMTexture> udim_textures;
  std::vector<TextureImage> images;
  std::vector<RenderLight> lights;
  std::vector<RenderCamera> cameras;
  std::vector<AnimationClip> animations;
  std::vector<Skeleton> skeletons;
  // UsdVol Volumes and their resolved fields. Voxels are decoded on demand
  // through volume_cache (shared by copies of the scene).
  std::vector<RenderVolume> volumes;
  std::shared_ptr<vol::FieldCache> volume_cache;
  // UsdVol ParticleFields (3D Gaussian splats) with full per-particle data;
  // ParticleField3DGaussianSplat prims also appear in `points`.
  std::vector<RenderParticleField> particle_fields;
  std::vector<UnsupportedRenderable> unsupported_renderables;
  // Degradation is separate from success: unsupported data remains in the
  // source Stage, approximations have render output, and failures omit output.
  std::vector<ConversionDiagnostic> conversion_diagnostics;
  PhysicsAnnotations physics;

  // Root nodes
  std::vector<int32_t> root_nodes;

  // Lookup by path
  std::unordered_map<std::string, int32_t> node_by_path;
  std::unordered_map<std::string, int32_t> mesh_by_path;
  std::unordered_map<std::string, int32_t> points_by_path;
  std::unordered_map<std::string, int32_t> curves_by_path;
  std::unordered_map<std::string, int32_t> point_instancer_by_path;
  std::unordered_map<std::string, int32_t> material_by_path;
  std::unordered_map<std::string, int32_t> volume_by_path;

  // Memory usage
  size_t memory_usage() const;

  // Bounds-checked accessors
  const RenderMesh* get_mesh(int32_t mesh_id) const;
  const RenderPoints* get_points(int32_t points_id) const;
  const RenderCurves* get_curves(int32_t curves_id) const;
  const RenderMaterial* get_material(int32_t material_id) const;
  const RenderPointInstancer* get_point_instancer(int32_t instancer_id) const;
  const RenderPointInstanceDraw* get_point_instance_draw(size_t draw_id) const;
  RenderPointInstanceDrawView get_point_instance_draw_view(size_t draw_id) const;
  RenderPointInstanceDrawRange get_point_instancer_draws(int32_t instancer_id) const;
  const SceneNode* get_node(int32_t node_id) const;
  bool has_valid_point_instance_draw_ranges() const;

  // Stats
  struct Stats {
    size_t node_count;
    size_t mesh_count;
    size_t points_count;
    size_t point_cloud_point_count;
    size_t curves_count;                    // RenderCurves records
    size_t curve_count;                     // individual curves (all records)
    size_t curve_tessellated_point_count;   // total tessellated polyline points
    size_t point_instancer_count;
    size_t point_instance_count;
    size_t visible_point_instance_count;
    size_t point_instance_draw_count;
    size_t material_count;
    size_t texture_count;
    size_t image_count;
    size_t light_count;
    size_t camera_count;
    size_t animation_count;
    size_t skeleton_count;
    size_t total_vertices;
    size_t total_triangles;
    size_t memory_bytes;
  };
  Stats get_stats() const;
};

}}} // namespace lightusd::tydra::next
