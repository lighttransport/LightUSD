// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "core/minijson.hh"
#include "tydra/render-data.hh"

namespace render_example {
namespace r = lightusd::tydra;
using Json = lightusd::minijson::Value;
struct PackedAttributes {
  std::vector<std::vector<int8_t>> normals, tangents;
  bool pack(r::RenderScene& scene) {
    normals.resize(scene.meshes.size());
    tangents.resize(scene.meshes.size());
    for (size_t i = 0; i < scene.meshes.size(); i++) {
      auto pack_one = [](r::FloatChunked& source, std::vector<int8_t>& target) {
        target.reserve(source.size());
        for (float value : source) {
          if (!std::isfinite(value)) return false;
          target.push_back(static_cast<int8_t>(
              std::lround(std::clamp(value, -1.f, 1.f) * 127)));
        }
        source = r::FloatChunked{};
        return true;
      };
      if (!pack_one(scene.meshes[i].normals, normals[i]) ||
          !pack_one(scene.meshes[i].tangents, tangents[i]))
        return false;
    }
    return true;
  }
  void restore(r::RenderScene& scene) const {
    for (size_t i = 0; i < normals.size(); i++) {
      for (int8_t value : normals[i])
        scene.meshes[i].normals.push_back(float(value) / 127.f);
      for (int8_t value : tangents[i])
        scene.meshes[i].tangents.push_back(float(value) / 127.f);
    }
  }
  size_t bytes() const {
    size_t n = 0;
    for (const auto& a : normals) n += a.capacity();
    for (const auto& a : tangents) n += a.capacity();
    return n;
  }
};
template <class Array>
Json preview(const Array& values, size_t limit = 64) {
  Json result = Json::array();
  size_t i = 0;
  for (const auto& value : values) {
    if (i++ >= limit) break;
    result.push_back(static_cast<double>(value));
  }
  return result;
}
inline Json matrix(const r::Matrix4& value) {
  Json j = Json::array();
  for (float x : value.m) j.push_back(x);
  return j;
}
inline Json matrix(const r::Matrix4d& value) {
  Json j = Json::array();
  for (double x : value.m) j.push_back(x);
  return j;
}
inline const char* interpolation(r::Interpolation value) {
  static const char* names[] = {"constant", "uniform", "vertex", "faceVarying",
                                "varying"};
  const auto i = static_cast<unsigned>(value);
  return i < 5 ? names[i] : "invalid";
}
inline const char* animation_path(r::AnimationChannel::TargetPath path) {
  static const char* names[] = {"Translation", "Rotation", "Scale", "Weights",
                                "CustomProperty"};
  const auto i = static_cast<unsigned>(path);
  return i < 5 ? names[i] : "Unknown";
}
inline Json parameter(const r::ShaderParam& value) {
  return Json{{"texture_id", value.texture_id},
              {"value", Json::array({value.value.x, value.value.y,
                                     value.value.z, value.value.w})}};
}
inline Json attribute(const r::FloatChunked& values, r::Interpolation interp,
                      unsigned components,
                      const std::vector<int8_t>* packed = nullptr) {
  const Json data = packed ? preview(*packed) : preview(values);
  const auto count = packed ? packed->size() : values.size() * sizeof(float);
  return Json{{"count", uint64_t(count)},
              {"format", std::string(packed ? "SNorm8x" : "Float") +
                             std::to_string(components)},
              {"variability", interpolation(interp)},
              {"elementSize", 1},
              {"value", data.dump()},
              {"values", data},
              {"truncated", (packed ? packed->size() : values.size()) > 64}};
}
inline Json build_diagnostics(const r::RenderScene& scene,
                              const std::string& source,
                              const Json& configuration,
                              const PackedAttributes* packed,
                              const Json* source_metadata = nullptr,
                              const Json* display_names = nullptr) {
  const auto display_name = [&](const std::string& path) {
    return display_names && (*display_names)[path].is_string()
               ? (*display_names)[path].get_string()
               : std::string();
  };
  Json result = Json::object();
  result["configuration"] = configuration;
  result["metadata"] = Json{
      {"format_version", "1.0"},
      {"generator", "LightUSD/Tydra"},
      {"source_file", source},
      {"scene",
       Json{{"upAxis", scene.up_axis == r::RenderScene::UpAxis::Z ? "Z" : "Y"},
            {"metersPerUnit", scene.meters_per_unit},
            {"framesPerSecond", scene.frames_per_second},
            {"startTimeCode", scene.start_time},
            {"endTimeCode", scene.end_time}}}};
  if (source_metadata) result["metadata"]["scene"] = *source_metadata;
  result["summary"] =
      Json{{"default_root_node",
            scene.root_nodes.empty() ? -1 : scene.root_nodes[0]},
           {"num_nodes", uint64_t(scene.root_nodes.size())},
           {"total_nodes", uint64_t(scene.nodes.size())},
           {"num_meshes", uint64_t(scene.meshes.size())},
           {"num_skeletons", uint64_t(scene.skeletons.size())},
           {"num_animations", uint64_t(scene.animations.size())},
           {"num_cameras", uint64_t(scene.cameras.size())},
           {"num_lights", uint64_t(scene.lights.size())},
           {"num_materials", uint64_t(scene.materials.size())},
           {"num_textures", uint64_t(scene.textures.size())},
           {"num_images", uint64_t(scene.images.size())},
           {"num_buffers", uint64_t(scene.images.size())},
           {"num_points", uint64_t(scene.points.size())},
           {"num_curves", uint64_t(scene.curves.size())}};
  for (const char* key :
       {"nodes", "meshes", "skeletons", "animations", "cameras", "lights",
        "materials", "textures", "images", "buffers"})
    result[key] = Json::array();
  // Flat catalog IDs are retained as well as recursive children. Bound nesting
  // prevents malformed caller-created render graphs from cycling indefinitely.
  const auto node_json = [&](size_t id) {
    const auto& n = scene.nodes[id];
    static const char* names[] = {
        "Xform",     "Mesh",       "Points",           "PointInstancer",
        "Camera",    "PointLight", "DirectionalLight", "SpotLight",
        "RectLight", "DiskLight",  "DomeLight",        "SphereLight",
        "Skeleton",  "Curves"};
    const auto type = static_cast<unsigned>(n.type);
    return Json{{"type", type < 14 ? names[type] : "Unknown"},
                {"id", uint64_t(id)},
                {"prim_name", n.name},
                {"display_name", display_name(n.prim_path)},
                {"abs_path", n.prim_path},
                {"data_id", n.data_id},
                {"local_matrix", matrix(n.local_transform).dump()},
                {"global_matrix", matrix(n.world_transform).dump()},
                {"visible", n.visible},
                {"children", Json::array()}};
  };
  struct NodeWork {
    int32_t id;
    Json* output;
    size_t depth;
  };
  std::vector<NodeWork> stack;
  for (int32_t id : scene.root_nodes) {
    if (id < 0 || static_cast<size_t>(id) >= scene.nodes.size()) continue;
    result["nodes"].push_back(node_json(static_cast<size_t>(id)));
  }
  size_t root = 0;
  for (int32_t id : scene.root_nodes)
    if (id >= 0 && static_cast<size_t>(id) < scene.nodes.size())
      stack.push_back({id, &result["nodes"][root++], 0});
  size_t visits = 0;
  while (!stack.empty()) {
    const auto work = stack.back();
    stack.pop_back();
    if (work.depth >= 120 || visits++ > scene.nodes.size() * 2) {
      (*work.output)["diagnostic"] = "node traversal limit exceeded";
      continue;
    }
    const auto& node = scene.nodes[static_cast<size_t>(work.id)];
    auto& children = (*work.output)["children"];
    std::vector<int32_t> ids;
    for (int32_t id : node.children)
      if (id >= 0 && static_cast<size_t>(id) < scene.nodes.size()) {
        ids.push_back(id);
        children.push_back(node_json(static_cast<size_t>(id)));
      }
    for (size_t i = 0; i < ids.size(); i++)
      stack.push_back({ids[i], &children[i], work.depth + 1});
  }
  for (size_t i = 0; i < scene.meshes.size(); i++) {
    const auto& m = scene.meshes[i];
    const auto& indices = m.triangulated_indices.empty()
                              ? m.face_vertex_indices
                              : m.triangulated_indices;
    Json mesh{
        {"prim_name", m.name},
        {"display_name", display_name(m.prim_path)},
        {"abs_path", m.prim_path},
        {"num_points", uint64_t(m.point_count())},
        {"points", preview(m.points).dump()},
        {"num_faceVertexCounts",
         uint64_t(m.is_triangulated ? indices.size() / 3 : m.face_count())},
        {"faceVertexCounts", preview(m.face_vertex_counts).dump()},
        {"num_faceVertexIndices", uint64_t(indices.size())},
        {"faceVertexIndices", preview(indices).dump()},
        {"materialId", m.material_id},
        {"normals", attribute(m.normals, m.normals_interp, 3,
                              packed ? &packed->normals[i] : nullptr)},
        {"tangents", attribute(m.tangents, m.tangents_interp, 4,
                               packed ? &packed->tangents[i] : nullptr)},
        {"num_texcoordSlots",
         int(!m.texcoords_0.empty()) + int(!m.texcoords_1.empty())},
        {"skel_id", m.skin ? m.skin->skeleton_id : -1},
        {"texcoords_0", attribute(m.texcoords_0, m.texcoords_0_interp, 2)},
        {"texcoords_1", attribute(m.texcoords_1, m.texcoords_1_interp, 2)},
        {"colors", attribute(m.colors, m.colors_interp, 3)},
        {"opacities", attribute(m.opacities, m.opacities_interp, 1)},
        {"material_subsets", Json::array()},
        {"blend_shapes", Json::array()}};
    for (const auto& subset : m.material_subsets)
      mesh["material_subsets"].push_back(
          Json{{"name", subset.name},
               {"start", subset.face_start},
               {"count", subset.face_count},
               {"material_id", subset.material_id}});
    for (const auto& shape : m.blend_shapes)
      mesh["blend_shapes"].push_back(
          Json{{"name", shape.name},
               {"weight", shape.weight},
               {"point_offsets", preview(shape.point_offsets)},
               {"point_indices", preview(shape.point_indices)},
               {"normal_offsets", preview(shape.normal_offsets)},
               {"inbetween_count", uint64_t(shape.inbetweens.size())}});
    if (m.skin)
      mesh["joint_and_weights"] =
          Json{{"elementSize", m.skin->influences_per_vertex},
               {"jointIndices", preview(m.skin->joint_indices)},
               {"jointWeights", preview(m.skin->joint_weights)},
               {"geomBindTransform", matrix(m.skin->geom_bind_transform)}};
    result["meshes"].push_back(std::move(mesh));
  }
  for (const auto& skeleton : scene.skeletons) {
    Json j{{"name", skeleton.name},
           {"abs_path", skeleton.prim_path},
           {"display_name", skeleton.display_name},
           {"anim_id", skeleton.animation_id},
           {"root_joint", skeleton.root_joint},
           {"joints", Json::array()}};
    for (size_t i = 0; i < skeleton.joints.size(); i++) {
      const auto& joint = skeleton.joints[i];
      j["joints"].push_back(
          Json{{"joint_name", joint.name},
               {"joint_path", joint.path},
               {"joint_id", uint64_t(i)},
               {"parent_id", joint.parent_id},
               {"bind_transform", matrix(joint.bind_transform)},
               {"rest_transform", matrix(joint.rest_transform)}});
    }
    result["skeletons"].push_back(std::move(j));
  }
  for (const auto& animation : scene.animations) {
    Json a{{"name", animation.name},
           {"prim_name", animation.name},
           {"display_name", display_name(animation.prim_path)},
           {"abs_path", animation.prim_path},
           {"duration", animation.end_time - animation.start_time},
           {"num_samplers", uint64_t(animation.channels.size())},
           {"num_channels", uint64_t(animation.channels.size())},
           {"channels", Json::array()}};
    for (size_t i = 0; i < animation.channels.size(); i++) {
      const auto& channel = animation.channels[i];
      Json keys = Json::array();
      for (size_t k = 0; k < std::min<size_t>(64, channel.keyframes.size());
           k++) {
        const auto& key = channel.keyframes[k];
        keys.push_back(
            Json{{"time", key.time},
                 {"value", Json::array({key.value.x, key.value.y, key.value.z,
                                        key.value.w})}});
      }
      a["channels"].push_back(
          Json{{"target_node", channel.target_node},
               {"target_skeleton", channel.target_skeleton},
               {"sampler", uint64_t(i)},
               {"path", animation_path(channel.target_path)},
               {"target_prim_path", channel.target_prim_path},
               {"property_name", channel.property_name},
               {"keyframes", keys},
               {"num_keyframes", uint64_t(channel.keyframes.size())},
               {"array_values", preview(channel.array_values)},
               {"element_count", channel.element_count},
               {"value_stride", channel.value_stride}});
    }
    result["animations"].push_back(std::move(a));
  }
  for (const auto& camera : scene.cameras)
    result["cameras"].push_back(
        Json{{"name", camera.name},
             {"display_name", display_name(camera.prim_path)},
             {"abs_path", camera.prim_path},
             {"focalLength", camera.focal_length},
             {"fovY", camera.fov_y()},
             {"shutterOpen", camera.shutter_open},
             {"shutterClose", camera.shutter_close}});
  for (const auto& light : scene.lights)
    result["lights"].push_back(Json{{"name", light.name},
                                    {"abs_path", light.prim_path},
                                    {"intensity", light.intensity},
                                    {"exposure", light.exposure}});
  for (const auto& material : scene.materials) {
    Json m{{"name", material.name},
           {"display_name", display_name(material.prim_path)},
           {"abs_path", material.prim_path},
           {"hasSurfaceShader", bool(material.preview_surface)},
           {"hasOpenPBRShader", bool(material.openpbr)}};
    if (material.preview_surface) {
      const auto& p = *material.preview_surface;
      Json shader = Json::object();
#define PARAM(name) shader[#name] = parameter(p.name)
      PARAM(diffuse_color);
      PARAM(specular_color);
      PARAM(emissive_color);
      PARAM(metallic);
      PARAM(roughness);
      PARAM(opacity);
      PARAM(opacity_threshold);
      PARAM(clearcoat);
      PARAM(clearcoat_roughness);
      PARAM(ior);
      PARAM(normal);
      PARAM(occlusion);
      PARAM(displacement);
#undef PARAM
      m["surfaceShader"] = std::move(shader);
    }
    if (material.openpbr) {
      const auto& p = *material.openpbr;
      Json shader = Json::object();
#define PARAM(name) shader[#name] = parameter(p.name)
      PARAM(base_weight);
      PARAM(base_color);
      PARAM(base_roughness);
      PARAM(base_diffuse_roughness);
      PARAM(base_metalness);
      PARAM(specular_weight);
      PARAM(specular_color);
      PARAM(specular_roughness);
      PARAM(specular_ior);
      PARAM(specular_anisotropy);
      PARAM(specular_roughness_anisotropy);
      PARAM(specular_rotation);
      PARAM(transmission_weight);
      PARAM(transmission_color);
      PARAM(transmission_depth);
      PARAM(transmission_scatter);
      PARAM(transmission_scatter_anisotropy);
      PARAM(transmission_dispersion);
      PARAM(transmission_dispersion_scale);
      PARAM(subsurface_weight);
      PARAM(subsurface_color);
      PARAM(subsurface_radius);
      PARAM(subsurface_scale);
      PARAM(subsurface_anisotropy);
      PARAM(coat_weight);
      PARAM(coat_color);
      PARAM(coat_roughness);
      PARAM(coat_rotation);
      PARAM(coat_ior);
      PARAM(coat_affect_color);
      PARAM(coat_affect_roughness);
      PARAM(coat_anisotropy);
      PARAM(coat_roughness_anisotropy);
      PARAM(coat_normal);
      PARAM(sheen_weight);
      PARAM(sheen_color);
      PARAM(sheen_roughness);
      PARAM(fuzz_weight);
      PARAM(fuzz_color);
      PARAM(fuzz_roughness);
      PARAM(thin_film_weight);
      PARAM(thin_film_thickness);
      PARAM(thin_film_ior);
      PARAM(emission_luminance);
      PARAM(emission_color);
      PARAM(opacity);
      PARAM(thin_walled);
      PARAM(normal);
      PARAM(tangent);
      PARAM(coat_tangent);
      PARAM(displacement);
#undef PARAM
      m["openPBRShader"] = std::move(shader);
    }
    result["materials"].push_back(std::move(m));
  }
  for (const auto& texture : scene.textures)
    result["textures"].push_back(Json{
        {"primvar_name", texture.uv_primvar},
        {"asset_path", texture.asset_path},
        {"connectedOutputChannel", int(texture.output_channel)},
        {"bias", Json::array({texture.bias.x, texture.bias.y, texture.bias.z,
                              texture.bias.w})},
        {"scale", Json::array({texture.scale_value.x, texture.scale_value.y,
                               texture.scale_value.z, texture.scale_value.w})},
        {"wrapS", int(texture.wrap_s)},
        {"wrapT", int(texture.wrap_t)},
        {"textureImageID", texture.image_id},
        {"has_transform2d", texture.has_transform2d}});
  static const char* component_names[] = {"UInt8",   "Int8",    "UInt16",
                                          "Int16",   "UInt32",  "Int32",
                                          "Float16", "Float32", "Float64"};
  for (size_t i = 0; i < scene.images.size(); i++) {
    const auto& image = scene.images[i];
    const int64_t buffer =
        image.is_loaded() ? static_cast<int64_t>(result["buffers"].size()) : -1;
    result["images"].push_back(
        Json{{"asset_identifier", image.asset_identifier},
             {"decoded", image.is_loaded()},
             {"channels", unsigned(image.channels)},
             {"width", image.width},
             {"height", image.height},
             {"miplevel", unsigned(image.mip_levels)},
             {"colorSpace",
              image.color_space == r::ColorSpace::sRGB ? "sRGB" : "linear"},
             {"bufferID", buffer}});
    if (image.is_loaded()) {
      const auto type = static_cast<unsigned>(image.component_type);
      result["buffers"].push_back(Json{
          {"bytes", uint64_t(image.data.size())},
          {"componentType", type < 9 ? component_names[type] : "invalid"}});
    }
  }
  result["summary"]["num_buffers"] = uint64_t(result["buffers"].size());
  return result;
}
inline void yaml(std::ostream& output, const Json& value, size_t indent = 0) {
  const std::string spaces(indent, ' ');
  if (const auto* object = value.object_items()) {
    if (object->empty()) {
      output << spaces << "{}\n";
      return;
    }
    for (const auto& member : *object) {
      output << spaces << member.key << ':';
      const auto& child = member.value();
      if ((child.is_object() || child.is_array()) && !child.empty()) {
        output << '\n';
        yaml(output, child, indent + 2);
      } else
        output << ' ' << child.dump() << '\n';
    }
  } else if (const auto* array = value.array_items()) {
    for (const auto& child : *array) {
      output << spaces << '-';
      if ((child.is_object() || child.is_array()) && !child.empty()) {
        output << '\n';
        yaml(output, child, indent + 2);
      } else
        output << ' ' << child.dump() << '\n';
    }
  } else
    output << spaces << value.dump() << '\n';
}
inline void dump_timesamples(const r::RenderScene& scene) {
  std::cout << "Animation Timesamples Dump\nTotal animations: "
            << scene.animations.size() << '\n';
  for (size_t a = 0; a < scene.animations.size(); a++) {
    const auto& animation = scene.animations[a];
    std::cout << "Animation [" << a << "]: " << animation.name
              << "\n  Abs path: " << animation.prim_path
              << "\n  Duration: " << animation.end_time - animation.start_time
              << " seconds\n";
    for (size_t c = 0; c < animation.channels.size(); c++) {
      const auto& channel = animation.channels[c];
      std::cout << "  Channel [" << c
                << "] Path: " << animation_path(channel.target_path)
                << "\n    Target node: " << channel.target_node
                << " Skeleton ID: " << channel.target_skeleton
                << "\n    Keyframes: " << channel.keyframes.size()
                << "\n    Timesamples:\n";
      for (size_t k = 0; k < channel.keyframes.size(); k++) {
        const auto& key = channel.keyframes[k];
        std::cout << "      [" << k << "] t=" << key.time << " : (";
        if (!channel.array_values.empty()) {
          const size_t count =
              size_t(channel.element_count) * channel.value_stride;
          if (count && k <= channel.array_values.size() / count &&
              count <= channel.array_values.size() - k * count) {
            for (size_t j = 0; j < count; j++) {
              if (j) std::cout << ", ";
              std::cout << channel.array_values[k * count + j];
            }
          } else
            std::cout << "invalid array payload";
        } else {
          const float values[] = {key.value.x, key.value.y, key.value.z,
                                  key.value.w};
          const size_t count =
              channel.target_path == r::AnimationChannel::TargetPath::Rotation
                  ? 4
              : channel.target_path ==
                          r::AnimationChannel::TargetPath::Translation ||
                      channel.target_path ==
                          r::AnimationChannel::TargetPath::Scale
                  ? 3
                  : 1;
          for (size_t j = 0; j < count; j++) {
            if (j) std::cout << ", ";
            std::cout << values[j];
          }
        }
        std::cout << ")\n";
      }
    }
  }
}
}  // namespace render_example
