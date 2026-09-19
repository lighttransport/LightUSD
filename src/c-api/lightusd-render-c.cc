// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD C API — tydra-next render-scene implementation.

#include "lightusd-render-c.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "c-internal.hh"
#include "tydra/next/render-converter.hh"
#include "safe-arithmetic.hh"
#include "tydra/next/render-data.hh"

namespace td = lightusd::tydra::next;
using lightusd_internal::EmptySV;
using lightusd_internal::Fail;
using lightusd_internal::SV;

struct lightusd_render_scene {
  td::RenderScene scene;
  std::vector<std::string> warnings;
  // Flatten-once cache for multi-chunk buffers: key -> contiguous bytes.
  // Guarded by mu (RenderScene itself is immutable after Convert).
  std::unordered_map<uint64_t, std::vector<uint8_t>> flat_cache;
  std::mutex mu;
};

namespace {
namespace safe = lightusd::safe;

uint64_t CacheKey(uint8_t domain, int32_t id, uint32_t sub, uint8_t which) {
  return (uint64_t(domain) << 56) | (uint64_t(uint32_t(id)) << 24) |
         (uint64_t(sub & 0xFFFF) << 8) | which;
}

bool ToSizeLimit(uint64_t value, size_t* out) {
  if (!out || value == 0) return false;
  if (value == LIGHTUSD_LIMIT_UNLIMITED) {
    *out = (std::numeric_limits<size_t>::max)();
    return true;
  }
#if SIZE_MAX < UINT64_MAX
  if (value > static_cast<uint64_t>(SIZE_MAX)) return false;
#endif
  *out = static_cast<size_t>(value);
  return true;
}

lightusd_status ToCStatus(::lightusd::next::OperationStatus status) {
  using Status = ::lightusd::next::OperationStatus;
  switch (status) {
    case Status::Ok: return LIGHTUSD_OK;
    case Status::InvalidArgument: return LIGHTUSD_ERR_INVALID_ARG;
    case Status::Unsupported: return LIGHTUSD_ERR_UNSUPPORTED;
    case Status::ResourceLimit: return LIGHTUSD_ERR_RESOURCE_LIMIT;
    case Status::IntegerOverflow: return LIGHTUSD_ERR_OVERFLOW;
    case Status::AllocationFailure: return LIGHTUSD_ERR_OUT_OF_MEMORY;
    case Status::Busy: return LIGHTUSD_ERR_BUSY;
    case Status::StaleRevision: return LIGHTUSD_ERR_STALE_REVISION;
    case Status::InvalidData:
    case Status::Cancelled:
    case Status::SinkRejected:
      return LIGHTUSD_ERR_INTERNAL;
  }
  return LIGHTUSD_ERR_INTERNAL;
}

// Fill a buffer view from a ChunkedArray: zero-copy when contiguous, else
// flatten once into the scene cache.
template <typename T, size_t ChunkBytes>
lightusd_status ViewFromChunked(lightusd_render_scene* scene,
                            const td::ChunkedArray<T, ChunkBytes>& arr,
                            uint64_t key, uint8_t comp_type,
                            uint8_t components, lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = comp_type;
  out->components = components;
  const size_t n = arr.size();
  if (components == 0) return Fail(LIGHTUSD_ERR_INTERNAL, "zero components");
  out->count = n / components;
  size_t nbytes;
  if (!safe::mul(n, sizeof(T), &nbytes)) {
    return Fail(LIGHTUSD_ERR_OVERFLOW, "buffer size overflow");
  }
  out->nbytes = nbytes;
  if (n == 0) return LIGHTUSD_OK;

  if (arr.is_contiguous()) {
    out->data = arr.chunk_data(0);
    return LIGHTUSD_OK;
  }
  std::lock_guard<std::mutex> lk(scene->mu);
  auto it = scene->flat_cache.find(key);
  if (it == scene->flat_cache.end()) {
    std::vector<uint8_t> flat(nbytes);
    arr.copy_to(reinterpret_cast<T*>(flat.data()));
    it = scene->flat_cache.emplace(key, std::move(flat)).first;
  }
  out->data = it->second.data();
  return LIGHTUSD_OK;
}

// Plain std::vector-backed view (always contiguous, zero-copy).
template <typename T>
lightusd_status ViewFromVector(const std::vector<T>& v, uint8_t comp_type,
                           uint8_t components, lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = comp_type;
  out->components = components;
  out->count = components ? v.size() / components : 0;
  out->data = v.empty() ? nullptr : v.data();
  if (!safe::mul(v.size(), sizeof(T), &out->nbytes)) {
    out->nbytes = 0;
    return Fail(LIGHTUSD_ERR_OVERFLOW, "buffer size overflow");
  }
  return LIGHTUSD_OK;
}

lightusd_status ViewFromMatrixVector(const std::vector<td::Matrix4>& v,
                                 lightusd_buffer_view* out) {
  std::memset(out, 0, sizeof(*out));
  out->component_type = LIGHTUSD_COMP_FLOAT32;
  out->components = 16;
  out->count = v.size();
  out->data = v.empty() ? nullptr : v.data();
  // Matrix4 is alignas(64) but sizeof is exactly 16 floats.
  if (!safe::mul(v.size(), sizeof(td::Matrix4), &out->nbytes)) {
    out->nbytes = 0;
    return Fail(LIGHTUSD_ERR_OVERFLOW, "matrix buffer size overflow");
  }
  return LIGHTUSD_OK;
}

const td::RenderMesh* MeshAt(const lightusd_render_scene* scene, int32_t id) {
  return scene ? scene->scene.get_mesh(id) : nullptr;
}

void CopyM4(float dst[16], const td::Matrix4& m) {
  std::memcpy(dst, m.m, 16 * sizeof(float));
}

const td::ShaderParam* FindParamPreview(const td::PreviewSurfaceShader& s,
                                        const std::string& n) {
  if (n == "diffuse_color") return &s.diffuse_color;
  if (n == "emissive_color") return &s.emissive_color;
  if (n == "specular_color") return &s.specular_color;
  if (n == "metallic") return &s.metallic;
  if (n == "roughness") return &s.roughness;
  if (n == "clearcoat") return &s.clearcoat;
  if (n == "clearcoat_roughness") return &s.clearcoat_roughness;
  if (n == "opacity") return &s.opacity;
  if (n == "opacity_threshold") return &s.opacity_threshold;
  if (n == "ior") return &s.ior;
  if (n == "normal") return &s.normal;
  if (n == "displacement") return &s.displacement;
  if (n == "occlusion") return &s.occlusion;
  return nullptr;
}

const td::ShaderParam* FindParamOpenPBR(const td::OpenPBRSurfaceShader& s,
                                        const std::string& n) {
  if (n == "base_weight") return &s.base_weight;
  if (n == "base_color") return &s.base_color;
  if (n == "base_roughness") return &s.base_roughness;
  if (n == "base_metalness") return &s.base_metalness;
  if (n == "specular_weight") return &s.specular_weight;
  if (n == "specular_color") return &s.specular_color;
  if (n == "specular_roughness") return &s.specular_roughness;
  if (n == "specular_ior") return &s.specular_ior;
  if (n == "transmission_weight") return &s.transmission_weight;
  if (n == "transmission_color") return &s.transmission_color;
  if (n == "subsurface_weight") return &s.subsurface_weight;
  if (n == "subsurface_color") return &s.subsurface_color;
  if (n == "coat_weight") return &s.coat_weight;
  if (n == "coat_color") return &s.coat_color;
  if (n == "coat_roughness") return &s.coat_roughness;
  if (n == "sheen_weight") return &s.sheen_weight;
  if (n == "sheen_color") return &s.sheen_color;
  if (n == "sheen_roughness") return &s.sheen_roughness;
  if (n == "emission_luminance") return &s.emission_luminance;
  if (n == "emission_color") return &s.emission_color;
  if (n == "opacity") return &s.opacity;
  if (n == "normal") return &s.normal;
  if (n == "tangent") return &s.tangent;
  return nullptr;
}

}  // namespace

extern "C" {

void lightusd_render_config_init(lightusd_render_config* cfg) {
  if (!cfg) return;
  std::memset(cfg, 0, sizeof(*cfg));
  cfg->struct_size = sizeof(*cfg);
  cfg->triangulate = 1;
  cfg->compute_normals = 1;
  cfg->compute_tangents = 0;
  cfg->build_vertex_indices = 1;
  cfg->load_textures = 1;
  cfg->allow_missing_textures = 1;
  cfg->target_color_space = 1; /* linear */
  cfg->duplicate_instance_meshes = 0;
  cfg->time_code = 0.0;
  cfg->max_threads = 0;
  cfg->max_resident_bytes = 1024ull * 1024ull * 1024ull;
  cfg->max_render_records = 1024ull * 1024ull;
  cfg->max_render_depth = 1024;
  cfg->max_value_clip_samples = 10000;
}

lightusd_status lightusd_render_convert(const lightusd_stage* stage,
                                const lightusd_render_config* cfg,
                                lightusd_render_scene** out) {
  if (!stage || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "stage/out is null");
  *out = nullptr;

  td::ConverterConfig config;
  config.asset_base_dir = stage->source_dir;
  if (cfg) {
    if (cfg->struct_size < sizeof(lightusd_render_config) ||
        cfg->max_threads < 0 ||
        !ToSizeLimit(cfg->max_resident_bytes,
                     &config.limits.max_resident_bytes) ||
        !ToSizeLimit(cfg->max_render_records,
                     &config.limits.max_render_records) ||
        !ToSizeLimit(cfg->max_render_depth,
                     &config.limits.max_namespace_depth) ||
        !ToSizeLimit(cfg->max_value_clip_samples,
                     &config.limits.max_value_clip_samples)) {
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid render config");
    }
    config.mesh.triangulate = cfg->triangulate != 0;
    config.mesh.compute_normals = cfg->compute_normals != 0;
    config.mesh.compute_tangents = cfg->compute_tangents != 0;
    config.mesh.build_vertex_indices = cfg->build_vertex_indices != 0;
    config.material.load_textures = cfg->load_textures != 0;
    config.material.allow_missing_textures = cfg->allow_missing_textures != 0;
    config.material.target_color_space =
        static_cast<td::ColorSpace>(cfg->target_color_space);
    config.point_instancer.duplicate_meshes =
        cfg->duplicate_instance_meshes != 0;
    config.time_code = cfg->time_code;
    config.execution.max_threads = cfg->max_threads;
  }

  td::RenderSceneConverter converter(config);
  td::ConvertResult result = converter.Convert(stage->stage);
  if (!result.success) {
    return Fail(ToCStatus(result.status),
                result.error.empty() ? "render conversion failed"
                                     : result.error);
  }
  lightusd_render_scene* scene = new (std::nothrow) lightusd_render_scene();
  if (!scene) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  scene->scene = std::move(result.scene);
  scene->warnings = std::move(result.warnings);
  *out = scene;
  return LIGHTUSD_OK;
}

void lightusd_render_scene_destroy(lightusd_render_scene* scene) { delete scene; }

lightusd_status lightusd_render_scene_warnings(const lightusd_render_scene* scene,
                                       lightusd_strlist** out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  lightusd_strlist* l = new (std::nothrow) lightusd_strlist();
  if (!l) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "alloc failed");
  l->items = scene->warnings;
  *out = l;
  return LIGHTUSD_OK;
}

size_t lightusd_render_count(const lightusd_render_scene* scene, uint8_t kind) {
  if (!scene) return 0;
  const td::RenderScene& s = scene->scene;
  switch (kind) {
    case LIGHTUSD_RENDER_NODE:
      return s.nodes.size();
    case LIGHTUSD_RENDER_MESH:
      return s.meshes.size();
    case LIGHTUSD_RENDER_MATERIAL:
      return s.materials.size();
    case LIGHTUSD_RENDER_TEXTURE:
      return s.textures.size();
    case LIGHTUSD_RENDER_IMAGE:
      return s.images.size();
    case LIGHTUSD_RENDER_LIGHT:
      return s.lights.size();
    case LIGHTUSD_RENDER_CAMERA:
      return s.cameras.size();
    case LIGHTUSD_RENDER_SKELETON:
      return s.skeletons.size();
    case LIGHTUSD_RENDER_ANIMATION:
      return s.animations.size();
    case LIGHTUSD_RENDER_INSTANCER:
      return s.point_instancers.size();
    case LIGHTUSD_RENDER_ROOT_NODE:
      return s.root_nodes.size();
    case LIGHTUSD_RENDER_UNSUPPORTED:
      return s.unsupported_renderables.size();
    default:
      return 0;
  }
}

int32_t lightusd_render_lookup(const lightusd_render_scene* scene, uint8_t kind,
                           const char* prim_path) {
  if (!scene || !prim_path) return -1;
  const td::RenderScene& s = scene->scene;
  const std::unordered_map<std::string, int32_t>* map = nullptr;
  switch (kind) {
    case LIGHTUSD_RENDER_NODE:
      map = &s.node_by_path;
      break;
    case LIGHTUSD_RENDER_MESH:
      map = &s.mesh_by_path;
      break;
    case LIGHTUSD_RENDER_MATERIAL:
      map = &s.material_by_path;
      break;
    case LIGHTUSD_RENDER_INSTANCER:
      map = &s.point_instancer_by_path;
      break;
    default:
      return -1;
  }
  auto it = map->find(prim_path);
  return it == map->end() ? -1 : it->second;
}

int32_t lightusd_render_root_node(const lightusd_render_scene* scene, size_t index) {
  if (!scene || index >= scene->scene.root_nodes.size()) return -1;
  return scene->scene.root_nodes[index];
}

lightusd_status lightusd_render_scene_get_info(const lightusd_render_scene* scene,
                                       lightusd_render_scene_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderScene& s = scene->scene;
  out->name = SV(s.name);
  out->default_prim = SV(s.default_prim);
  out->meters_per_unit = s.meters_per_unit;
  out->up_axis = static_cast<uint8_t>(s.up_axis);
  out->start_time = s.start_time;
  out->end_time = s.end_time;
  out->frames_per_second = s.frames_per_second;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_node_get_info(const lightusd_render_scene* scene,
                                      int32_t id,
                                      lightusd_render_node_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::SceneNode* node = scene->scene.get_node(id);
  if (!node) return Fail(LIGHTUSD_ERR_NOT_FOUND, "node id out of range");
  out->name = SV(node->name);
  out->prim_path = SV(node->prim_path);
  out->type = static_cast<uint8_t>(node->type);
  out->visible = node->visible ? 1 : 0;
  out->data_id = node->data_id;
  out->parent_id = node->parent_id;
  out->child_count = static_cast<uint32_t>(node->children.size());
  CopyM4(out->local_transform, node->local_transform);
  CopyM4(out->world_transform, node->world_transform);
  return LIGHTUSD_OK;
}

size_t lightusd_render_node_children(const lightusd_render_scene* scene, int32_t id,
                                 int32_t* out, size_t cap) {
  if (!scene) return 0;
  const td::SceneNode* node = scene->scene.get_node(id);
  if (!node) return 0;
  if (out) {
    const size_t n = node->children.size() < cap ? node->children.size() : cap;
    for (size_t i = 0; i < n; ++i) out[i] = node->children[i];
  }
  return node->children.size();
}

lightusd_status lightusd_render_mesh_get_info(const lightusd_render_scene* scene,
                                      int32_t id,
                                      lightusd_render_mesh_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  std::memset(out, 0, sizeof(*out));
  out->name = SV(m->name);
  out->prim_path = SV(m->prim_path);
  out->point_count = m->point_count();
  out->face_count = m->face_count();
  out->material_id = m->material_id;
  out->is_triangulated = m->is_triangulated ? 1 : 0;
  out->has_normals = m->has_normals() ? 1 : 0;
  out->has_tangents = m->has_tangents() ? 1 : 0;
  out->has_texcoords0 = m->has_texcoords() ? 1 : 0;
  out->has_texcoords1 = !m->texcoords_1.empty() ? 1 : 0;
  out->has_colors = m->has_colors() ? 1 : 0;
  out->has_skin = m->has_skin() ? 1 : 0;
  out->has_bbox = m->has_bbox ? 1 : 0;
  out->normals_interp = static_cast<uint8_t>(m->normals_interp);
  out->texcoords0_interp = static_cast<uint8_t>(m->texcoords_0_interp);
  out->colors_interp = static_cast<uint8_t>(m->colors_interp);
  out->subset_count = static_cast<uint32_t>(m->material_subsets.size());
  out->primvar_count = static_cast<uint32_t>(m->primvars.size());
  out->blend_shape_count = static_cast<uint32_t>(m->blend_shapes.size());
  out->skeleton_id = m->skin ? m->skin->skeleton_id : -1;
  out->bbox_min[0] = m->bbox_min.x;
  out->bbox_min[1] = m->bbox_min.y;
  out->bbox_min[2] = m->bbox_min.z;
  out->bbox_max[0] = m->bbox_max.x;
  out->bbox_max[1] = m->bbox_max.y;
  out->bbox_max[2] = m->bbox_max.z;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_subset(const lightusd_render_scene* scene,
                                    int32_t mesh_id, size_t index,
                                    uint32_t* face_start, uint32_t* face_count,
                                    int32_t* material_id) {
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (index >= m->material_subsets.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "subset index out of range");
  }
  const auto& sub = m->material_subsets[index];
  if (face_start) *face_start = sub.face_start;
  if (face_count) *face_count = sub.face_count;
  if (material_id) *material_id = sub.material_id;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_primvar_info(const lightusd_render_scene* scene,
                                          int32_t mesh_id, size_t index,
                                          lightusd_render_primvar_info* out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (index >= m->primvars.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "primvar index out of range");
  }
  const td::VertexAttribute& pv = m->primvars[index];
  out->name = SV(pv.name);
  out->format = static_cast<uint8_t>(pv.format);
  out->interpolation = static_cast<uint8_t>(pv.interpolation);
  out->has_indices = pv.has_indices() ? 1 : 0;
  out->_pad = 0;
  out->element_count = pv.element_count();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_mesh_buffer(lightusd_render_scene* scene, int32_t mesh_id,
                                    uint8_t kind, lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  const uint64_t key = CacheKey(0, mesh_id, 0, kind);
  switch (kind) {
    case LIGHTUSD_MESH_BUF_POINTS:
      return ViewFromChunked(scene, m->points, key, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_MESH_BUF_FACE_COUNTS:
      return ViewFromChunked(scene, m->face_vertex_counts, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_FACE_INDICES:
      return ViewFromChunked(scene, m->face_vertex_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_TRI_INDICES:
      return ViewFromChunked(scene, m->triangulated_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES:
      return ViewFromChunked(scene, m->triangulated_face_vertex_indices, key,
                             LIGHTUSD_COMP_UINT32, 1, out);
    case LIGHTUSD_MESH_BUF_NORMALS:
      return ViewFromChunked(scene, m->normals, key, LIGHTUSD_COMP_FLOAT32, 3,
                             out);
    case LIGHTUSD_MESH_BUF_TANGENTS:
      return ViewFromChunked(scene, m->tangents, key, LIGHTUSD_COMP_FLOAT32, 4,
                             out);
    case LIGHTUSD_MESH_BUF_TEXCOORDS0:
      return ViewFromChunked(scene, m->texcoords_0, key, LIGHTUSD_COMP_FLOAT32, 2,
                             out);
    case LIGHTUSD_MESH_BUF_TEXCOORDS1:
      return ViewFromChunked(scene, m->texcoords_1, key, LIGHTUSD_COMP_FLOAT32, 2,
                             out);
    case LIGHTUSD_MESH_BUF_COLORS:
      return ViewFromChunked(scene, m->colors, key, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_MESH_BUF_JOINT_INDICES:
      if (!m->skin) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh has no skin");
      return ViewFromChunked(scene, m->skin->joint_indices, key,
                             LIGHTUSD_COMP_UINT16, 4, out);
    case LIGHTUSD_MESH_BUF_JOINT_WEIGHTS:
      if (!m->skin) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh has no skin");
      return ViewFromChunked(scene, m->skin->joint_weights, key,
                             LIGHTUSD_COMP_FLOAT32, 4, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown mesh buffer kind");
  }
}

lightusd_status lightusd_render_mesh_primvar_buffer(lightusd_render_scene* scene,
                                            int32_t mesh_id,
                                            size_t primvar_index,
                                            uint8_t which,
                                            lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (primvar_index >= m->primvars.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "primvar index out of range");
  }
  const td::VertexAttribute& pv = m->primvars[primvar_index];
  const uint64_t key =
      CacheKey(1, mesh_id, static_cast<uint32_t>(primvar_index), which);
  if (which == 1) {
    return ViewFromChunked(scene, pv.indices, key, LIGHTUSD_COMP_UINT32, 1, out);
  }
  uint8_t comps = 1;
  switch (pv.format) {
    case td::VertexFormat::Vec2:
    case td::VertexFormat::IVec2:
    case td::VertexFormat::UVec2:
      comps = 2;
      break;
    case td::VertexFormat::Vec3:
    case td::VertexFormat::IVec3:
    case td::VertexFormat::UVec3:
      comps = 3;
      break;
    case td::VertexFormat::Vec4:
    case td::VertexFormat::IVec4:
    case td::VertexFormat::UVec4:
      comps = 4;
      break;
    default:
      comps = 1;
      break;
  }
  switch (pv.format) {
    case td::VertexFormat::Int:
    case td::VertexFormat::IVec2:
    case td::VertexFormat::IVec3:
    case td::VertexFormat::IVec4:
      return ViewFromChunked(scene, pv.int_data, key, LIGHTUSD_COMP_INT32, comps,
                             out);
    case td::VertexFormat::UInt:
    case td::VertexFormat::UVec2:
    case td::VertexFormat::UVec3:
    case td::VertexFormat::UVec4:
      return ViewFromChunked(scene, pv.uint_data, key, LIGHTUSD_COMP_UINT32,
                             comps, out);
    default:
      return ViewFromChunked(scene, pv.float_data, key, LIGHTUSD_COMP_FLOAT32,
                             comps, out);
  }
}

lightusd_status lightusd_render_mesh_blendshape(lightusd_render_scene* scene,
                                        int32_t mesh_id, size_t bs_index,
                                        uint8_t which, lightusd_sv* name,
                                        float* weight,
                                        lightusd_buffer_view* out) {
  if (!scene) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene is null");
  const td::RenderMesh* m = MeshAt(scene, mesh_id);
  if (!m) return Fail(LIGHTUSD_ERR_NOT_FOUND, "mesh id out of range");
  if (bs_index >= m->blend_shapes.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "blend shape index out of range");
  }
  const auto& bs = m->blend_shapes[bs_index];
  if (name) *name = SV(bs.name);
  if (weight) *weight = bs.weight;
  if (out) {
    const uint64_t key =
        CacheKey(2, mesh_id, static_cast<uint32_t>(bs_index), which);
    if (which == 1) {
      return ViewFromChunked(scene, bs.normal_offsets, key, LIGHTUSD_COMP_FLOAT32,
                             3, out);
    }
    return ViewFromChunked(scene, bs.point_offsets, key, LIGHTUSD_COMP_FLOAT32, 3,
                           out);
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_get_info(const lightusd_render_scene* scene,
                                          int32_t id,
                                          lightusd_render_material_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* mat = scene->scene.get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  out->name = SV(mat->name);
  out->prim_path = SV(mat->prim_path);
  out->shader_type = static_cast<uint8_t>(mat->shader_type);
  out->double_sided = mat->double_sided ? 1 : 0;
  out->alpha_mode = static_cast<uint8_t>(mat->alpha_mode);
  out->_pad = 0;
  out->alpha_cutoff = mat->alpha_cutoff;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_mtlx_config(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_materialx_config_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderMaterial* mat = scene->scene.get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");
  std::memset(out, 0, sizeof(*out));
  out->authored = mat->mtlx_config.authored ? 1 : 0;
  out->version = SV(mat->mtlx_config.version);
  out->name_space = SV(mat->mtlx_config.name_space);
  out->colorspace = SV(mat->mtlx_config.colorspace);
  out->source_uri = SV(mat->mtlx_config.source_uri);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_material_param(const lightusd_render_scene* scene,
                                       int32_t id, const char* param,
                                       int32_t* texture_id, float value[4]) {
  if (!scene || !param) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/param null");
  const td::RenderMaterial* mat = scene->scene.get_material(id);
  if (!mat) return Fail(LIGHTUSD_ERR_NOT_FOUND, "material id out of range");

  const td::ShaderParam* p = nullptr;
  const std::string n(param);
  if (mat->shader_type == td::RenderMaterial::ShaderType::PreviewSurface &&
      mat->preview_surface) {
    p = FindParamPreview(*mat->preview_surface, n);
  } else if (mat->shader_type == td::RenderMaterial::ShaderType::OpenPBR &&
             mat->openpbr) {
    p = FindParamOpenPBR(*mat->openpbr, n);
  }
  if (!p) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, std::string("no shader param: ") + n);
  }
  if (texture_id) *texture_id = p->texture_id;
  if (value) {
    value[0] = p->value.x;
    value[1] = p->value.y;
    value[2] = p->value.z;
    value[3] = p->value.w;
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_texture_get_info(const lightusd_render_scene* scene,
                                         int32_t id,
                                         lightusd_render_texture_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.textures.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "texture id out of range");
  }
  const td::RenderTexture& t = scene->scene.textures[size_t(id)];
  out->name = SV(t.name);
  out->prim_path = SV(t.prim_path);
  out->asset_path = SV(t.asset_path);
  out->uv_offset[0] = t.offset.x;
  out->uv_offset[1] = t.offset.y;
  out->uv_scale[0] = t.scale.x;
  out->uv_scale[1] = t.scale.y;
  out->uv_rotation = t.rotation;
  out->wrap_s = static_cast<uint8_t>(t.wrap_s);
  out->wrap_t = static_cast<uint8_t>(t.wrap_t);
  out->output_channel = static_cast<uint8_t>(t.output_channel);
  out->_pad = 0;
  out->bias[0] = t.bias.x;
  out->bias[1] = t.bias.y;
  out->bias[2] = t.bias.z;
  out->bias[3] = t.bias.w;
  out->scale[0] = t.scale_value.x;
  out->scale[1] = t.scale_value.y;
  out->scale[2] = t.scale_value.z;
  out->scale[3] = t.scale_value.w;
  out->image_id = t.image_id;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_image_get_info(const lightusd_render_scene* scene,
                                       int32_t id,
                                       lightusd_render_image_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.images.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "image id out of range");
  }
  const td::TextureImage& img = scene->scene.images[size_t(id)];
  out->name = SV(img.name);
  out->resolved_path = SV(img.resolved_path);
  out->width = img.width;
  out->height = img.height;
  out->channels = img.channels;
  out->component_type = static_cast<uint8_t>(img.component_type);
  out->color_space = static_cast<uint8_t>(img.color_space);
  out->is_loaded = img.is_loaded() ? 1 : 0;
  out->nbytes = img.data.size();
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_image_buffer(lightusd_render_scene* scene, int32_t id,
                                     lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.images.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "image id out of range");
  }
  const td::TextureImage& img = scene->scene.images[size_t(id)];
  const uint64_t key = CacheKey(3, id, 0, 0);
  lightusd_status st = ViewFromChunked(scene, img.data, key, LIGHTUSD_COMP_UINT8,
                                   img.channels ? img.channels : 1, out);
  return st;
}

lightusd_status lightusd_render_light_get_info(const lightusd_render_scene* scene,
                                       int32_t id,
                                       lightusd_render_light_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.lights.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "light id out of range");
  }
  const td::RenderLight& l = scene->scene.lights[size_t(id)];
  out->name = SV(l.name);
  out->prim_path = SV(l.prim_path);
  out->type = static_cast<uint8_t>(l.type);
  out->normalize = l.normalize ? 1 : 0;
  out->enable_shadow = l.enable_shadow ? 1 : 0;
  out->_pad = 0;
  out->color[0] = l.color.x;
  out->color[1] = l.color.y;
  out->color[2] = l.color.z;
  out->intensity = l.intensity;
  out->exposure = l.exposure;
  CopyM4(out->transform, l.transform);
  out->param0 = 0.0f;
  out->param1 = 0.0f;
  switch (l.type) {
    case td::LightType::Sphere:
      out->param0 = l.params.sphere.radius;
      break;
    case td::LightType::Rect:
      out->param0 = l.params.rect.width;
      out->param1 = l.params.rect.height;
      break;
    case td::LightType::Disk:
      out->param0 = l.params.disk.radius;
      break;
    case td::LightType::Spot:
      out->param0 = l.params.spot.angle;
      break;
    default:
      break;
  }
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_camera_get_info(const lightusd_render_scene* scene,
                                        int32_t id,
                                        lightusd_render_camera_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.cameras.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "camera id out of range");
  }
  const td::RenderCamera& c = scene->scene.cameras[size_t(id)];
  out->name = SV(c.name);
  out->prim_path = SV(c.prim_path);
  out->type = static_cast<uint8_t>(c.type);
  out->focal_length = c.focal_length;
  out->horizontal_aperture = c.horizontal_aperture;
  out->vertical_aperture = c.vertical_aperture;
  out->ortho_width = c.ortho_width;
  out->near_clip = c.near_clip;
  out->far_clip = c.far_clip;
  out->fov_x = c.fov_x();
  out->fov_y = c.fov_y();
  CopyM4(out->transform, c.transform);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_get_info(const lightusd_render_scene* scene,
                                          int32_t id,
                                          lightusd_render_skeleton_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 || size_t(id) >= scene->scene.skeletons.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  }
  const td::Skeleton& s = scene->scene.skeletons[size_t(id)];
  out->name = SV(s.name);
  out->prim_path = SV(s.prim_path);
  out->joint_count = static_cast<uint32_t>(s.joints.size());
  out->root_joint = s.root_joint;
  out->animation_id = s.animation_id;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_skeleton_joint(const lightusd_render_scene* scene,
                                       int32_t skeleton_id, size_t joint_index,
                                       lightusd_render_joint_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (skeleton_id < 0 ||
      size_t(skeleton_id) >= scene->scene.skeletons.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "skeleton id out of range");
  }
  const td::Skeleton& s = scene->scene.skeletons[size_t(skeleton_id)];
  if (joint_index >= s.joints.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "joint index out of range");
  }
  const td::SkeletonJoint& j = s.joints[joint_index];
  out->name = SV(j.name);
  out->path = SV(j.path);
  out->parent_id = j.parent_id;
  CopyM4(out->bind_transform, j.bind_transform);
  CopyM4(out->rest_transform, j.rest_transform);
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_instancer_get_info(const lightusd_render_scene* scene,
                                           int32_t id,
                                           lightusd_render_instancer_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderPointInstancer* pi = scene->scene.get_point_instancer(id);
  if (!pi) return Fail(LIGHTUSD_ERR_NOT_FOUND, "instancer id out of range");
  out->name = SV(pi->name);
  out->prim_path = SV(pi->prim_path);
  out->instance_count = pi->instance_count();
  out->prototype_count = static_cast<uint32_t>(pi->prototype_count());
  out->valid = pi->valid ? 1 : 0;
  return LIGHTUSD_OK;
}

lightusd_status lightusd_render_instancer_buffer(lightusd_render_scene* scene, int32_t id,
                                         uint8_t kind,
                                         lightusd_buffer_view* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  const td::RenderPointInstancer* pi = scene->scene.get_point_instancer(id);
  if (!pi) return Fail(LIGHTUSD_ERR_NOT_FOUND, "instancer id out of range");
  switch (kind) {
    case LIGHTUSD_INST_BUF_PROTO_INDICES:
      return ViewFromVector(pi->proto_indices, LIGHTUSD_COMP_INT32, 1, out);
    case LIGHTUSD_INST_BUF_POSITIONS:
      return ViewFromVector(pi->positions, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_ORIENTATIONS:
      return ViewFromVector(pi->orientations, LIGHTUSD_COMP_FLOAT32, 4, out);
    case LIGHTUSD_INST_BUF_SCALES:
      return ViewFromVector(pi->scales, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_TRANSFORMS:
      return ViewFromMatrixVector(pi->transforms, out);
    case LIGHTUSD_INST_BUF_VISIBLE:
      return ViewFromVector(pi->instance_visible, LIGHTUSD_COMP_UINT8, 1, out);
    case LIGHTUSD_INST_BUF_VELOCITIES:
      return ViewFromVector(pi->velocities, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_ANGULAR_VELOCITIES:
      return ViewFromVector(pi->angular_velocities, LIGHTUSD_COMP_FLOAT32, 3, out);
    case LIGHTUSD_INST_BUF_IDS:
      return ViewFromVector(pi->ids, LIGHTUSD_COMP_INT64, 1, out);
    case LIGHTUSD_INST_BUF_INVISIBLE_IDS:
      return ViewFromVector(pi->invisible_ids, LIGHTUSD_COMP_INT64, 1, out);
    case LIGHTUSD_INST_BUF_INACTIVE_IDS:
      return ViewFromVector(pi->inactive_ids, LIGHTUSD_COMP_INT64, 1, out);
    default:
      return Fail(LIGHTUSD_ERR_INVALID_ARG, "unknown instancer buffer kind");
  }
}

lightusd_status lightusd_render_unsupported_get_info(
    const lightusd_render_scene* scene, int32_t id,
    lightusd_render_unsupported_info* out) {
  if (!scene || !out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "scene/out is null");
  if (id < 0 ||
      static_cast<size_t>(id) >= scene->scene.unsupported_renderables.size()) {
    return Fail(LIGHTUSD_ERR_NOT_FOUND, "unsupported renderable id out of range");
  }
  const td::UnsupportedRenderable& rec =
      scene->scene.unsupported_renderables[static_cast<size_t>(id)];
  out->prim_path = SV(rec.prim_path);
  out->type_name = SV(rec.type_name);
  out->reason = SV(rec.reason);
  return LIGHTUSD_OK;
}

}  // extern "C"
