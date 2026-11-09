// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "c-api/lightusd-render-c.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace lightusd {
namespace web_next {
int RenderStream::nodePathCopy(int node_id, uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const std::string& path = render_scene_.nodes[static_cast<size_t>(node_id)].prim_path;
    return CopyOutputString(path, out, cap);
}

int RenderStream::nativeInstanceNodeIdsCopy(uint8_t* out, uint32_t cap) const {
  if (!render_scene_valid_) return -1;
  size_t count = 0;
  for (const tr::SceneNode& node : render_scene_.nodes) {
    if (node.is_instance) ++count;
  }
  if (count > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                  sizeof(uint32_t)) return -1;
  const size_t bytes = count * sizeof(uint32_t);
  if (!out || cap < bytes) return static_cast<int>(bytes);
  size_t written = 0;
  for (size_t i = 0; i < render_scene_.nodes.size(); ++i) {
    if (!render_scene_.nodes[i].is_instance) continue;
    const uint32_t id = static_cast<uint32_t>(i);
    std::memcpy(out + written * sizeof(id), &id, sizeof(id));
    ++written;
  }
  return static_cast<int>(bytes);
}

int RenderStream::nodePrototypePathCopy(int node_id, uint8_t* out,
                                        uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size()) return -1;
    const std::string& path =
        render_scene_.nodes[static_cast<size_t>(node_id)].prototype_path;
    return CopyOutputString(path, out, cap);
}

int RenderStream::nodeTransformCopy(int node_id, uint8_t kind, uint8_t* out,
                                    uint32_t cap) const {
    if (!render_scene_valid_ || node_id < 0 ||
        static_cast<size_t>(node_id) >= render_scene_.nodes.size() || kind > 1) return -1;
    const tr::SceneNode& node = render_scene_.nodes[static_cast<size_t>(node_id)];
    const tr::Matrix4& matrix = kind == 0 ? node.local_transform : node.world_transform;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, matrix.m, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::resourcePathCopy(uint8_t kind, int resource_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || resource_id < 0) return -1;
    const std::string* path = nullptr;
    const size_t id = static_cast<size_t>(resource_id);
    switch (kind) {
      case 0: if (id < render_scene_.nodes.size()) path = &render_scene_.nodes[id].prim_path; break;
      case 1: if (id < render_scene_.meshes.size()) path = &render_scene_.meshes[id].prim_path; break;
      case 2: if (const tr::RenderMaterial* m = outputRenderMaterial_(resource_id)) path = &m->prim_path; break;
      case 3: if (id < render_scene_.textures.size()) path = &render_scene_.textures[id].prim_path; break;
      case 4: if (id < render_scene_.images.size()) path = &render_scene_.images[id].resolved_path; break;
      case 5: if (id < render_scene_.lights.size()) path = &render_scene_.lights[id].prim_path; break;
      case 6: if (id < render_scene_.cameras.size()) path = &render_scene_.cameras[id].prim_path; break;
      case 7: if (id < render_scene_.skeletons.size()) path = &render_scene_.skeletons[id].prim_path; break;
      case 8: if (id < render_scene_.animations.size()) path = &render_scene_.animations[id].prim_path; break;
      case 9: if (id < render_scene_.unsupported_renderables.size()) path = &render_scene_.unsupported_renderables[id].prim_path; break;
      case 10: if (id < render_scene_.point_instancers.size()) path = &render_scene_.point_instancers[id].prim_path; break;
      case 11:
        if (id < render_scene_.root_nodes.size()) {
          const int32_t node = render_scene_.root_nodes[id];
          if (node >= 0 && static_cast<size_t>(node) < render_scene_.nodes.size())
            path = &render_scene_.nodes[static_cast<size_t>(node)].prim_path;
        }
        break;
      case 12: if (id < render_scene_.points.size()) path = &render_scene_.points[id].prim_path; break;
      case 13: if (id < render_scene_.curves.size()) path = &render_scene_.curves[id].prim_path; break;
      case 14:
        if (id < render_scene_.point_instance_draws.size()) {
          const int32_t instancer = render_scene_.point_instance_draws[id].point_instancer_id;
          if (instancer >= 0 && static_cast<size_t>(instancer) < render_scene_.point_instancers.size())
            path = &render_scene_.point_instancers[static_cast<size_t>(instancer)].prim_path;
        }
        break;
      default: return -1;
    }
    if (!path || path->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(path->size());
    if (!out || cap < path->size() || path->empty()) return required;
    std::memcpy(out, path->data(), path->size());
    return required;
}

int RenderStream::resourceNameCopy(uint8_t kind, int resource_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || resource_id < 0) return -1;
    const std::string* name = nullptr;
    const size_t id = static_cast<size_t>(resource_id);
    switch (kind) {
      case 0: if (id < render_scene_.nodes.size()) name = &render_scene_.nodes[id].name; break;
      case 1: if (id < render_scene_.meshes.size()) name = &render_scene_.meshes[id].name; break;
      case 2: if (const tr::RenderMaterial* m = outputRenderMaterial_(resource_id)) name = &m->name; break;
      case 3: if (id < render_scene_.textures.size()) name = &render_scene_.textures[id].name; break;
      case 4: if (id < render_scene_.images.size()) name = &render_scene_.images[id].name; break;
      case 5: if (id < render_scene_.lights.size()) name = &render_scene_.lights[id].name; break;
      case 6: if (id < render_scene_.cameras.size()) name = &render_scene_.cameras[id].name; break;
      case 7: if (id < render_scene_.skeletons.size()) name = &render_scene_.skeletons[id].name; break;
      case 8: if (id < render_scene_.animations.size()) name = &render_scene_.animations[id].name; break;
      case 10: if (id < render_scene_.point_instancers.size()) name = &render_scene_.point_instancers[id].name; break;
      case 11:
        if (id < render_scene_.root_nodes.size()) {
          const int32_t node = render_scene_.root_nodes[id];
          if (node >= 0 && static_cast<size_t>(node) < render_scene_.nodes.size())
            name = &render_scene_.nodes[static_cast<size_t>(node)].name;
        }
        break;
      case 12: if (id < render_scene_.points.size()) name = &render_scene_.points[id].name; break;
      case 13: if (id < render_scene_.curves.size()) name = &render_scene_.curves[id].name; break;
      case 15: if (id < render_scene_.skeletons.size()) name = &render_scene_.skeletons[id].display_name; break;
      default: return -1;
    }
    if (!name || name->size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(name->size());
    if (!out || cap < name->size() || name->empty()) return required;
    std::memcpy(out, name->data(), name->size());
    return required;
}

int RenderStream::recordPathCopy(uint8_t kind, int record_id, uint8_t* out,
                                 uint32_t cap) const {
    if (kind > LIGHTUSD_RENDER_POINT_INSTANCE_DRAW) return -1;
    return resourcePathCopy(kind, record_id, out, cap);
}

int RenderStream::lightField(int light_id, uint8_t field) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)()) ? (std::numeric_limits<int>::min)()
           : v >= static_cast<double>((std::numeric_limits<int>::max)()) ? (std::numeric_limits<int>::max)()
           : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return static_cast<int>(light.type);
      case 1: return scaled(light.intensity);
      case 2: return scaled(light.exposure);
      case 3: return light.normalize ? 1 : 0;
      case 4: return light.enable_shadow ? 1 : 0;
      default: return -1;
    }
}

int RenderStream::lightTransformCopy(int light_id, uint8_t* out,
                                     uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, render_scene_.lights[static_cast<size_t>(light_id)].transform.m,
                kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::lightColorCopy(int light_id, uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    constexpr size_t kBytes = 3 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    const tr::Float3& color =
        render_scene_.lights[static_cast<size_t>(light_id)].color;
    std::memcpy(out, &color.x, kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::lightInfo(int light_id, lightusd_next_light_info* out) const {
    static_assert(sizeof(lightusd_next_light_info) == 192,
                  "light info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_light_info) ||
        !render_scene_valid_ || light_id < 0 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const size_t limit = static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    const size_t counts[] = {light.light_link_targets.size(),
        light.shadow_link_targets.size(), light.filter_targets.size(),
        light.light_link_mesh_indices.size(), light.shadow_link_mesh_indices.size()};
    for (size_t count : counts) if (count > limit) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->type = static_cast<int32_t>(light.type);
    out->flags = (light.normalize ? 1u : 0u) |
        (light.enable_color_temperature ? 2u : 0u) |
        (light.shaping_ies_normalize ? 4u : 0u) |
        (light.light_links_all ? 8u : 0u) |
        (light.shadow_links_all ? 16u : 0u) |
        (light.enable_shadow ? 32u : 0u);
    for (size_t i = 0; i < 5; ++i) out->counts[i] = static_cast<int32_t>(counts[i]);
    out->dome_texture_id = -1;
    float* v = out->values;
    v[0] = light.intensity;
    v[1] = light.exposure;
    v[2] = light.color_temperature;
    v[3] = light.diffuse;
    v[4] = light.specular;
    v[5] = light.shaping_focus;
    v[6] = light.shaping_focus_tint.x;
    v[7] = light.shaping_focus_tint.y;
    v[8] = light.shaping_focus_tint.z;
    v[9] = light.shaping_cone_softness;
    v[10] = light.shaping_ies_angle_scale;
    v[11] = light.shadow_distance;
    v[12] = light.shadow_falloff;
    v[13] = light.shadow_falloff_gamma;
    v[14] = light.color.x; v[15] = light.color.y; v[16] = light.color.z;
    v[17] = light.shadow_color.x;
    v[18] = light.shadow_color.y;
    v[19] = light.shadow_color.z;
    std::memcpy(out->transform, light.transform.m, sizeof(out->transform));
    switch (light.type) {
      case tr::LightType::Sphere: v[20] = light.params.sphere.radius; break;
      case tr::LightType::Rect:
        v[20] = light.params.rect.width; v[21] = light.params.rect.height; break;
      case tr::LightType::Disk: v[20] = light.params.disk.radius; break;
      case tr::LightType::Spot: v[20] = light.params.spot.angle; break;
      case tr::LightType::Cylinder:
        v[20] = light.params.cylinder.radius;
        v[21] = light.params.cylinder.length; break;
      case tr::LightType::Directional: v[20] = light.params.distant.angle; break;
      case tr::LightType::Dome:
        out->dome_texture_id = light.params.dome.texture_id;
        out->dome_texture_format = static_cast<int32_t>(light.params.dome.texture_format);
        if (out->dome_texture_id >= 0 &&
            static_cast<size_t>(out->dome_texture_id) < render_scene_.images.size())
          out->flags |= 64u;
        break;
      default: break;
    }
    return 0;
}

int RenderStream::lightStringCopy(int light_id, uint8_t kind, int item_id,
                                  uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 || kind > 4 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const std::string* value = nullptr;
    if (kind == 0) value = &light.shaping_ies_file;
    else if (kind == 4) {
      if (light.type != tr::LightType::Dome || light.params.dome.texture_id < 0 ||
          static_cast<size_t>(light.params.dome.texture_id) >= render_scene_.images.size())
        return -1;
      const auto& image = render_scene_.images[static_cast<size_t>(light.params.dome.texture_id)];
      value = image.name.empty() ? &image.resolved_path : &image.name;
    } else {
      const auto& strings = kind == 1 ? light.light_link_targets
          : kind == 2 ? light.shadow_link_targets : light.filter_targets;
      if (item_id < 0 || static_cast<size_t>(item_id) >= strings.size()) return -1;
      value = &strings[static_cast<size_t>(item_id)];
    }
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap < value->size() || value->empty()) return required;
    std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::lightMeshIdsCopy(int light_id, uint8_t kind, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || light_id < 0 || kind > 1 ||
        static_cast<size_t>(light_id) >= render_scene_.lights.size()) return -1;
    const tr::RenderLight& light = render_scene_.lights[static_cast<size_t>(light_id)];
    const auto& ids = kind == 0 ? light.light_link_mesh_indices
                                 : light.shadow_link_mesh_indices;
    if (ids.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                         sizeof(int32_t)) return -1;
    const int required = static_cast<int>(ids.size() * sizeof(int32_t));
    if (!out || cap < static_cast<uint32_t>(required) || ids.empty()) return required;
    std::memcpy(out, ids.data(), static_cast<size_t>(required));
    return required;
}

int RenderStream::cameraField(int camera_id, uint8_t field) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const tr::RenderCamera& camera = render_scene_.cameras[static_cast<size_t>(camera_id)];
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)()) ? (std::numeric_limits<int>::min)()
           : v >= static_cast<double>((std::numeric_limits<int>::max)()) ? (std::numeric_limits<int>::max)()
           : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return static_cast<int>(camera.type);
      case 1: return scaled(camera.focal_length);
      case 2: return scaled(camera.ortho_width);
      case 3: return scaled(camera.near_clip);
      case 4: return scaled(camera.far_clip);
      case 5: return scaled(camera.fov_x());
      case 6: return scaled(camera.fov_y());
      default: return -1;
    }
}

int RenderStream::cameraInfo(int camera_id, lightusd_next_camera_info* out) const {
    static_assert(sizeof(lightusd_next_camera_info) == 88,
                  "camera info POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_camera_info) ||
        !render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    const tr::RenderCamera& camera =
        render_scene_.cameras[static_cast<size_t>(camera_id)];
    out->type = static_cast<int32_t>(camera.type);
    out->focal_length = camera.focal_length;
    out->horizontal_aperture = camera.horizontal_aperture;
    out->vertical_aperture = camera.vertical_aperture;
    out->ortho_width = camera.ortho_width;
    out->near_clip = camera.near_clip;
    out->far_clip = camera.far_clip;
    out->focus_distance = camera.focus_distance;
    out->fstop = camera.fstop;
    out->fov_x = camera.fov_x();
    out->fov_y = camera.fov_y();
    out->aspect = camera.aspect_ratio();
    out->horizontal_aperture_offset = camera.horizontal_aperture_offset;
    out->vertical_aperture_offset = camera.vertical_aperture_offset;
    out->exposure = camera.exposure;
    out->stereo_role = static_cast<int32_t>(camera.stereo_role);
    out->shutter_open = camera.shutter_open;
    out->shutter_close = camera.shutter_close;
    return 0;
}

int RenderStream::cameraTransformCopy(int camera_id, uint8_t* out,
                                      uint32_t cap) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    constexpr size_t kBytes = 16 * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, render_scene_.cameras[static_cast<size_t>(camera_id)].transform.m,
                kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::cameraOpticsCopy(int camera_id, uint8_t* out,
                                   uint32_t cap) const {
    if (!render_scene_valid_ || camera_id < 0 ||
        static_cast<size_t>(camera_id) >= render_scene_.cameras.size()) return -1;
    const tr::RenderCamera& camera = render_scene_.cameras[static_cast<size_t>(camera_id)];
    const std::array<float, 8> values = {
        camera.focal_length, camera.horizontal_aperture,
        camera.vertical_aperture, camera.ortho_width,
        camera.near_clip, camera.far_clip, camera.fov_x(), camera.fov_y()};
    constexpr size_t kBytes = values.size() * sizeof(float);
    if (!out || cap < kBytes) return static_cast<int>(kBytes);
    std::memcpy(out, values.data(), kBytes);
    return static_cast<int>(kBytes);
}

int RenderStream::sceneField(uint8_t field) const {
    if (!render_scene_valid_) return -1;
    const auto scaled = [](double value) -> int {
      if (!std::isfinite(value)) return 0;
      const double v = value * 1000.0;
      return v <= static_cast<double>((std::numeric_limits<int>::min)())
                 ? (std::numeric_limits<int>::min)()
                 : v >= static_cast<double>((std::numeric_limits<int>::max)())
                       ? (std::numeric_limits<int>::max)()
                       : static_cast<int>(std::lround(v));
    };
    switch (field) {
      case 0: return scaled(static_cast<double>(render_scene_.meters_per_unit) * 1000.0);
      case 1: return static_cast<int>(render_scene_.up_axis);
      case 2: return scaled(render_scene_.start_time);
      case 3: return scaled(render_scene_.end_time);
      case 4: return scaled(render_scene_.frames_per_second);
      default: return -1;
    }
}

int RenderStream::sceneStringCopy(uint8_t kind, uint8_t* out,
                                  uint32_t cap) const {
    if (!loaded_ || kind > 6) return -1;
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &render_scene_.name; break;
      case 1: value = &render_scene_.default_prim; break;
      case 2: value = &render_scene_.render_settings_path; break;
      case 3: value = &render_scene_.working_color_space; break;
      case 4: value = &stage_.GetMeta().upAxis; break;
      case 5: value = &stage_.GetMeta().comment; break;
      case 6: value = &stage_.GetMeta().copyright; break;
    }
    if (!value || value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap == 0 || value->empty()) return required;
    const size_t copy = std::min<size_t>(value->size(), cap - 1);
    std::memcpy(out, value->data(), copy);
    out[copy] = 0;
    return required;
}

int RenderStream::sceneMetadata(lightusd_next_scene_metadata* out) const {
    static_assert(sizeof(lightusd_next_scene_metadata) == 96,
                  "scene metadata POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_scene_metadata)) return -1;
    if (!loaded_) return 1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    const lightusd::next::StageMeta& meta = stage_.GetMeta();
    out->reserved = (meta.startTimeCode_set ? 1u : 0u) |
                    (meta.endTimeCode_set ? 2u : 0u) |
                    (meta.autoPlay_set ? 4u : 0u) |
                    (meta.autoPlay ? 8u : 0u);
    out->meters_per_unit = meta.metersPerUnit_set ? meta.metersPerUnit : 1.0;
    out->kilograms_per_unit = meta.kilogramsPerUnit;
    out->frames_per_second = meta.framesPerSecond;
    out->time_codes_per_second = meta.timeCodesPerSecond;
    out->start_time_code = meta.startTimeCode;
    out->end_time_code = meta.endTimeCode;
    std::memcpy(out->working_to_display_linear,
                render_scene_.working_to_display_linear,
                sizeof(out->working_to_display_linear));
    return 0;
}

int RenderStream::unsupportedStringCopy(int unsupported_id, uint8_t kind,
                                        uint8_t* out, uint32_t cap) const {
    if (!render_scene_valid_ || unsupported_id < 0 || kind > 2 ||
        static_cast<size_t>(unsupported_id) >=
            render_scene_.unsupported_renderables.size()) return -1;
    const auto& record =
        render_scene_.unsupported_renderables[static_cast<size_t>(unsupported_id)];
    const std::string* value = kind == 0 ? &record.prim_path
                              : kind == 1 ? &record.type_name : &record.reason;
    if (value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (!out || cap == 0 || value->empty()) return required;
    const size_t copy = std::min<size_t>(value->size(), cap - 1);
    std::memcpy(out, value->data(), copy);
    out[copy] = 0;
    return required;
}

int RenderStream::renderStats(lightusd_next_render_stats* out) const {
    if (!out || out->struct_size < sizeof(lightusd_next_render_stats)) return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->source_meshes = static_cast<int32_t>(stats_.source_mesh_count);
    out->source_materials = static_cast<int32_t>(
        std::max(stats_.source_material_count, source_material_keys_.size()));
    out->source_textures = static_cast<int32_t>(
        std::max(stats_.source_texture_count, source_texture_keys_.size()));
    out->optimized_meshes = meshCount();
    out->optimized_materials = static_cast<int32_t>(materials_.size());
    out->optimized_textures = static_cast<int32_t>(texture_keys_.size());
    out->merged_meshes = static_cast<int32_t>(stats_.merged_mesh_count);
    out->merge_groups = static_cast<int32_t>(stats_.merge_group_count);
    out->skipped_merge_meshes = static_cast<int32_t>(stats_.skipped_merge_count);
    out->stage_load_ms = stats_.stage_load_ms;
    out->input_copy_ms = stats_.input_copy_ms;
    out->input_bytes = static_cast<double>(stats_.input_bytes);
    out->stage_memory_bytes = static_cast<double>(stage_.GetMemoryUsage());
    out->geometry_borrowed_bytes = static_cast<double>(stats_.geometry_borrowed_bytes);
    out->geometry_materialized_bytes = static_cast<double>(stats_.geometry_materialized_bytes);
    if (render_scene_valid_) {
      out->render_scene_memory_bytes = static_cast<double>(render_scene_.memory_usage());
    }
    return 0;
}

int RenderStream::renderStatsDetail(lightusd_next_render_stats_detail* out) const {
    static_assert(sizeof(lightusd_next_render_stats_detail) == 200,
                  "render stats detail POD layout changed");
    if (!out || out->struct_size < sizeof(lightusd_next_render_stats_detail))
      return -1;
    const uint32_t size = out->struct_size;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = size;
    out->flags = (material_dedup_ ? 1u : 0u) |
                 (mesh_merge_ ? 2u : 0u) |
                 (mesh_merge_bake_transform_ ? 4u : 0u) |
                 (flatten_render_tree_ ? 8u : 0u) |
                 (render_scene_valid_ ? 16u : 0u);
    const double timings[] = {
        stats_.composition_ms, stats_.mesh_discovery_ms, stats_.optimize_ms,
        stats_.material_ms, stats_.material_identity_ms,
        stats_.material_conversion_ms, stats_.geometry_build_ms,
        stats_.merge_append_ms};
    std::memcpy(out->timings_ms, timings, sizeof(timings));
    out->cache_counts[0] = static_cast<int32_t>(stats_.material_identity_hits);
    out->cache_counts[1] = static_cast<int32_t>(stats_.material_identity_misses);
    out->cache_counts[2] = static_cast<int32_t>(stats_.material_graph_cache_hits);
    out->cache_counts[3] = static_cast<int32_t>(stats_.material_graph_cache_misses);
    size_t provided_asset_bytes = imported_asset_bytes_;
    for (const auto& asset : clip_assets_) provided_asset_bytes += asset.second.size();
    out->memory_bytes[0] = static_cast<double>(provided_asset_bytes);
    if (render_scene_valid_) {
      size_t mesh_points_bytes = 0;
      size_t mesh_normals_bytes = 0;
      size_t mesh_uv_bytes = 0;
      size_t mesh_topology_bytes = 0;
      size_t mesh_triangulation_bytes = 0;
      for (const tr::RenderMesh& mesh : render_scene_.meshes) {
        mesh_points_bytes += mesh.points.memory_usage();
        mesh_normals_bytes += mesh.normals.memory_usage();
        mesh_uv_bytes += mesh.texcoords_0.memory_usage() +
                         mesh.texcoords_1.memory_usage();
        mesh_topology_bytes += mesh.face_vertex_counts.memory_usage() +
                               mesh.face_vertex_indices.memory_usage();
        mesh_triangulation_bytes += mesh.triangulated_indices.memory_usage() +
                                    mesh.triangulated_face_vertex_indices.memory_usage();
      }
      out->memory_bytes[1] = static_cast<double>(mesh_points_bytes);
      out->memory_bytes[2] = static_cast<double>(mesh_normals_bytes);
      out->memory_bytes[3] = static_cast<double>(mesh_uv_bytes);
      out->memory_bytes[4] = static_cast<double>(mesh_topology_bytes);
      out->memory_bytes[5] = static_cast<double>(mesh_triangulation_bytes);
      const size_t counts[] = {
          render_scene_.nodes.size(), render_scene_.meshes.size(),
          render_scene_.points.size(), render_scene_.curves.size(),
          render_scene_.point_instancers.size(),
          render_scene_.point_instance_draws.size(),
          static_cast<size_t>(materialCount()), render_scene_.textures.size(),
          render_scene_.images.size(), render_scene_.lights.size(),
          render_scene_.cameras.size(), render_scene_.animations.size(),
          render_scene_.skeletons.size(),
          render_scene_.unsupported_renderables.size(),
          render_scene_warnings_.size()};
      for (size_t i = 0; i < 15; ++i)
        out->scene_counts[i] = static_cast<int32_t>(counts[i]);
    }
    return 0;
}

int RenderStream::variantSetCount() const {
    return variant_sets_.size() > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(variant_sets_.size());
  }

int RenderStream::variantNameCount(int set_id) const {
    if (set_id < 0 || static_cast<size_t>(set_id) >= variant_sets_.size()) return -1;
    const size_t count = variant_sets_[static_cast<size_t>(set_id)].variant_names.size();
    return count > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(count);
  }

int RenderStream::variantStringCopy(int set_id, int variant_id, uint8_t kind,
                                    uint8_t* out, uint32_t cap) const {
    if (set_id < 0 || kind > 3 ||
        static_cast<size_t>(set_id) >= variant_sets_.size()) return -1;
    const VariantSetInfo& info = variant_sets_[static_cast<size_t>(set_id)];
    const std::string* value = nullptr;
    switch (kind) {
      case 0: value = &info.prim_path; break;
      case 1: value = &info.set_name; break;
      case 2: value = &info.selected; break;
      case 3:
        if (variant_id < 0 ||
            static_cast<size_t>(variant_id) >= info.variant_names.size()) return -1;
        value = &info.variant_names[static_cast<size_t>(variant_id)];
        break;
    }
    if (!value || value->size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
      return -1;
    const int required = static_cast<int>(value->size());
    if (out && cap >= value->size() && !value->empty())
      std::memcpy(out, value->data(), value->size());
    return required;
}

int RenderStream::layerAssetPathCount(uint8_t kind) const {
    if (kind > 2) return -1;
    if (!loaded_) return 0;
    const size_t count = authored_arc_paths_[kind].size();
    return count > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? -1 : static_cast<int>(count);
}

int RenderStream::layerAssetPathCopy(uint8_t kind, int path_id,
                                     uint8_t* out, uint32_t cap) const {
    if (kind > 2 || path_id < 0 || !loaded_ ||
        static_cast<size_t>(path_id) >= authored_arc_paths_[kind].size()) return -1;
    const std::string& value = authored_arc_paths_[kind][static_cast<size_t>(path_id)];
    if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    if (out && cap >= value.size() && !value.empty())
      std::memcpy(out, value.data(), value.size());
    return static_cast<int>(value.size());
}

int RenderStream::layerArcPresent(uint8_t kind) const {
    if (kind > 4) return -1;
    if (kind == 3) return loaded_ && authored_has_inherits_ ? 1 : 0;
    if (kind == 4) return loaded_ && authored_has_specializes_ ? 1 : 0;
    // Authored arcs, snapshotted before in-place composition consumed them.
    return loaded_ && authored_arc_present_[kind] ? 1 : 0;
}

}  // namespace web_next
}  // namespace lightusd
