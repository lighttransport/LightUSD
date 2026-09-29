// SPDX-License-Identifier: Apache-2.0
//
// lusdquicklook — the backend-neutral preview scene.
//
// Everything the renderers consume lives here: world-space triangle meshes,
// flat materials, small decoded textures and a handful of lights. Deliberately
// plain: no USD types, no tydra types, no GPU handles, so both the CPU tracer
// and the GL raster path read the same structs.
//
// Bulk arrays go through PoolAlloc so every byte lands in the shared MemBudget
// and an over-budget scene throws std::bad_alloc on the worker instead of
// growing until the kernel intervenes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "budget.hh"

namespace lusdql {

template <class T>
using QlVec = std::vector<T, PoolAlloc<T>>;

struct QlAabb {
  float lo[3] = {0, 0, 0};
  float hi[3] = {0, 0, 0};
  bool valid = false;

  void Expand(const float p[3]);
  void Expand(const QlAabb& other);
  void Center(float out[3]) const;
  float Radius() const;  // half the diagonal; 0 when invalid
};

// Decoded, downsampled, 8-bit RGBA. Preview textures are capped at
// kMaxTextureDim so a 4K map costs ~1 MB instead of 64 MB.
struct QlTexture {
  static constexpr uint32_t kMaxTextureDim = 512;
  // Environment maps are sampled by direction rather than by UV, and a small
  // one is plenty for a preview: 512x256 is ~512 KB, tracked like everything
  // else.
  static constexpr uint32_t kMaxEnvDim = 512;

  uint32_t width = 0;
  uint32_t height = 0;
  QlVec<uint8_t> rgba;  // width * height * 4
  bool wrap_repeat_s = true;
  bool wrap_repeat_t = true;
  bool srgb = true;  // decode to linear when sampling

  bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == size_t(width) * height * 4;
  }
};

// A flattened UsdPreviewSurface / OpenPBR surface. Only what the direct-lighting
// shader actually evaluates.
struct QlMaterial {
  // How a partially transparent surface is resolved. Blend is the expensive
  // one: it needs back-to-front ordering on the raster path and a layered
  // walk on the tracer, so it is only selected when the asset asks for it.
  enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

  float base_color[3] = {0.8f, 0.8f, 0.8f};
  float emissive[3] = {0.0f, 0.0f, 0.0f};
  float roughness = 0.5f;
  float metallic = 0.0f;
  float opacity = 1.0f;
  bool double_sided = false;

  // Indices into QlScene::textures; -1 = none. The data maps (normal,
  // roughness, metallic, opacity) are loaded with QlTexture::srgb false so
  // both backends sample them linearly.
  int base_color_tex = -1;
  int normal_tex = -1;
  int roughness_tex = -1;
  int metallic_tex = -1;
  int emissive_tex = -1;
  int opacity_tex = -1;

  // Which channel carries the scalar, for maps that pack several together
  // (the usual ORM layout is occlusion.r / roughness.g / metallic.b).
  uint8_t roughness_channel = 1;
  uint8_t metallic_channel = 2;
  uint8_t opacity_channel = 3;

  float normal_scale = 1.0f;
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;

  bool needs_tangents() const { return normal_tex >= 0; }
};

// World-space triangle mesh. Positions are pre-transformed by the node's world
// matrix, so the renderers need no hierarchy.
struct QlMesh {
  std::string name;
  std::string prim_path;

  QlVec<float> positions;      // 3 per vertex
  QlVec<float> normals;        // 3 per vertex; empty = use geometric normals
  QlVec<float> uvs;            // 2 per vertex; empty = no texturing
  QlVec<uint32_t> indices;     // 3 per triangle
  // 4 per vertex (xyz + bitangent sign); empty unless the bound material has a
  // normal map. At 16 B/vertex this is not carried speculatively.
  QlVec<float> tangents;

  int material_id = -1;
  bool is_proxy = false;  // extent box stand-in, not the authored geometry
  QlAabb bounds;

  size_t vertex_count() const { return positions.size() / 3; }
  size_t triangle_count() const { return indices.size() / 3; }
  uint64_t byte_size() const;
};

struct QlLight {
  enum class Type : uint8_t { Distant, Point, Sphere, Rect, Disk, Dome };

  Type type = Type::Distant;
  // Distant: direction light travels toward. Others: world position.
  float direction[3] = {0.0f, -1.0f, -0.3f};
  float position[3] = {0.0f, 0.0f, 0.0f};
  float color[3] = {1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float radius = 0.0f;  // sphere/disk falloff softening
  bool casts_shadow = true;
};

// Why a preview came out worse than the full-quality path. Reported in the
// status bar so a degraded image is never silently mistaken for the real thing.
struct QlDegradation {
  bool textures_dropped = false;
  bool proxy_geometry = false;
  bool uncomposed = false;
  bool geometry_skipped = false;
  bool triangle_cap_hit = false;
  std::string detail;

  bool any() const {
    return textures_dropped || proxy_geometry || uncomposed ||
           geometry_skipped || triangle_cap_hit;
  }
};

struct QlSceneStats {
  uint64_t prim_count = 0;
  uint64_t mesh_count = 0;
  uint64_t triangle_count = 0;
  uint64_t vertex_count = 0;
  uint64_t texture_count = 0;
  uint64_t material_count = 0;
  uint64_t light_count = 0;
  uint64_t geometry_bytes = 0;
  uint64_t texture_bytes = 0;
  double load_seconds = 0.0;
};

// An authored camera worth offering as a viewpoint.
struct QlCameraDesc {
  std::string name;
  float world_from_camera[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                 0, 0, 1, 0, 0, 0, 0, 1};
  float fov_y_radians = 0.7f;
  float near_clip = 0.1f;
  float far_clip = 10000.0f;
};

struct QlScene {
  std::vector<QlMesh> meshes;
  std::vector<QlMaterial> materials;
  std::vector<QlTexture> textures;
  std::vector<QlLight> lights;
  std::vector<QlCameraDesc> cameras;

  QlAabb bounds;
  bool y_up = true;  // false = Z-up stage
  float meters_per_unit = 1.0f;

  // Image-based lighting. `env_texture` indexes `textures` and holds a
  // latlong (equirectangular) environment, from an authored DomeLight or from
  // --env. The prefiltered chain is roughness-indexed and built once on the
  // CPU so both backends sample identical pixels.
  static constexpr int kEnvPrefilterLevels = 4;
  int env_texture = -1;
  int env_prefiltered[kEnvPrefilterLevels] = {-1, -1, -1, -1};
  float env_rotation = 0.0f;   // radians about the up axis
  float env_intensity = 1.0f;

  bool has_env() const { return env_texture >= 0; }

  QlSceneStats stats;
  QlDegradation degraded;

  void Clear();
  void RecomputeBounds();
  // Total tracked bytes held by the mesh arrays and textures.
  uint64_t ByteSize() const;
};

}  // namespace lusdql
