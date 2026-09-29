// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Render Data Implementation

#include "render-data.hh"
#include <cmath>

#include "safe-arithmetic.hh"

namespace lightusd {
namespace tydra {
namespace next {

RenderMesh::RenderMesh() = default;
RenderMesh::~RenderMesh() = default;
RenderMesh::RenderMesh(const RenderMesh&) = default;
RenderMesh& RenderMesh::operator=(const RenderMesh&) = default;
RenderMesh::RenderMesh(RenderMesh&&) noexcept = default;
RenderMesh& RenderMesh::operator=(RenderMesh&&) noexcept = default;

RenderMaterial::RenderMaterial() = default;
RenderMaterial::~RenderMaterial() = default;
RenderMaterial::RenderMaterial(const RenderMaterial&) = default;
RenderMaterial& RenderMaterial::operator=(const RenderMaterial&) = default;
RenderMaterial::RenderMaterial(RenderMaterial&&) noexcept = default;
RenderMaterial& RenderMaterial::operator=(RenderMaterial&&) noexcept = default;

RenderScene::RenderScene() = default;
RenderScene::~RenderScene() = default;
RenderScene::RenderScene(RenderScene&&) noexcept = default;
RenderScene& RenderScene::operator=(RenderScene&&) noexcept = default;
RenderScene::RenderScene(const RenderScene&) = default;
RenderScene& RenderScene::operator=(const RenderScene&) = default;
namespace {

template <typename T>
size_t VectorBytes(const std::vector<T>& v) {
  return safe::saturating_mul(v.capacity(), sizeof(T));
}

template <typename T>
size_t VectorSlackBytes(const std::vector<T>& v) {
  return safe::saturating_mul(v.capacity() - v.size(), sizeof(T));
}

void AddBytes(size_t* total, size_t bytes) {
  *total = safe::saturating_add(*total, bytes);
}

size_t StringVectorBytes(const std::vector<std::string>& v) {
  size_t total = safe::saturating_mul(v.capacity(), sizeof(std::string));
  for (const std::string& s : v) {
    AddBytes(&total, s.capacity());
  }
  return total;
}

size_t StringMapBytes(const std::unordered_map<std::string, int32_t>& m) {
  size_t total = safe::saturating_mul(m.bucket_count(), sizeof(void*));
  for (const auto& entry : m) {
    AddBytes(&total, sizeof(entry));
    AddBytes(&total, entry.first.capacity());
  }
  return total;
}

}  // namespace

//
// RenderMesh
//

void RenderMesh::compact() {
  face_vertex_counts.shrink_to_fit();
  face_vertex_indices.shrink_to_fit();
  points.shrink_to_fit();
  normals.shrink_to_fit();
  tangents.shrink_to_fit();
  texcoords_0.shrink_to_fit();
  texcoords_1.shrink_to_fit();
  colors.shrink_to_fit();
  opacities.shrink_to_fit();
  triangulated_indices.shrink_to_fit();
  triangulated_face_vertex_indices.shrink_to_fit();

  for (auto& pv : primvars) {
    pv.float_data.shrink_to_fit();
    pv.int_data.shrink_to_fit();
    pv.uint_data.shrink_to_fit();
    pv.indices.shrink_to_fit();
  }

  if (skin) {
    skin->joint_indices.shrink_to_fit();
    skin->joint_weights.shrink_to_fit();
  }

  for (auto& bs : blend_shapes) {
    bs.point_offsets.shrink_to_fit();
    bs.normal_offsets.shrink_to_fit();
    for (auto& inbetween : bs.inbetweens) {
      inbetween.point_offsets.shrink_to_fit();
    }
  }
}

bool RenderMesh::has_alloc_failure() const {
  if (face_vertex_counts.alloc_failed() || face_vertex_indices.alloc_failed() ||
      points.alloc_failed() || normals.alloc_failed() ||
      tangents.alloc_failed() || texcoords_0.alloc_failed() ||
      texcoords_1.alloc_failed() || colors.alloc_failed() ||
      opacities.alloc_failed() ||
      triangulated_indices.alloc_failed() ||
      triangulated_face_vertex_indices.alloc_failed()) {
    return true;
  }
  for (const auto& pv : primvars) {
    if (pv.float_data.alloc_failed() || pv.int_data.alloc_failed() ||
        pv.uint_data.alloc_failed() || pv.indices.alloc_failed()) {
      return true;
    }
  }
  if (skin && (skin->joint_indices.alloc_failed() ||
               skin->joint_weights.alloc_failed())) {
    return true;
  }
  for (const auto& bs : blend_shapes) {
    if (bs.point_offsets.alloc_failed() || bs.normal_offsets.alloc_failed()) {
      return true;
    }
    for (const auto& inbetween : bs.inbetweens) {
      if (inbetween.point_offsets.alloc_failed()) return true;
    }
  }
  return false;
}

size_t RenderMesh::memory_usage() const {
  size_t total = sizeof(*this);
  AddBytes(&total, name.capacity());
  AddBytes(&total, prim_path.capacity());
  AddBytes(&total, texcoords_0_name.capacity());
  AddBytes(&total, texcoords_1_name.capacity());
  AddBytes(&total, VectorBytes(subdivision_face_source));
  AddBytes(&total, face_vertex_counts.memory_usage());
  AddBytes(&total, face_vertex_indices.memory_usage());
  AddBytes(&total, points.memory_usage());
  AddBytes(&total, normals.memory_usage());
  AddBytes(&total, tangents.memory_usage());
  AddBytes(&total, texcoords_0.memory_usage());
  AddBytes(&total, texcoords_1.memory_usage());
  AddBytes(&total, colors.memory_usage());
  AddBytes(&total, opacities.memory_usage());
  AddBytes(&total, triangulated_indices.memory_usage());
  AddBytes(&total, triangulated_face_vertex_indices.memory_usage());

  AddBytes(&total, VectorBytes(primvars));
  for (const auto& pv : primvars) {
    AddBytes(&total, pv.name.capacity());
    AddBytes(&total, pv.memory_usage());
  }
  AddBytes(&total, VectorBytes(material_subsets));
  AddBytes(&total, VectorBytes(face_triangle_offsets));
  AddBytes(&total, VectorBytes(sanitize_face_remap));
  AddBytes(&total, VectorBytes(hole_faces));

  if (skin) {
    AddBytes(&total, sizeof(SkinBinding));
    AddBytes(&total, skin->skeleton_path.capacity());
    AddBytes(&total, StringVectorBytes(skin->mesh_joint_order));
    AddBytes(&total, skin->joint_indices.memory_usage());
    AddBytes(&total, skin->joint_weights.memory_usage());
  }

  AddBytes(&total, VectorBytes(blend_shapes));
  for (const auto& bs : blend_shapes) {
    AddBytes(&total, bs.name.capacity());
    AddBytes(&total, bs.point_offsets.memory_usage());
    AddBytes(&total, bs.normal_offsets.memory_usage());
    AddBytes(&total, VectorBytes(bs.point_indices));
    AddBytes(&total, VectorBytes(bs.inbetweens));
    for (const auto& inbetween : bs.inbetweens) {
      AddBytes(&total, inbetween.name.capacity());
      AddBytes(&total, inbetween.point_offsets.memory_usage());
    }
  }

  return total;
}

//
// RenderPoints
//

void RenderPoints::compact() {
  points.shrink_to_fit();
  normals.shrink_to_fit();
  widths.shrink_to_fit();
  colors.shrink_to_fit();
  opacities.shrink_to_fit();
}

bool RenderPoints::has_alloc_failure() const {
  return points.alloc_failed() || normals.alloc_failed() || widths.alloc_failed() ||
         colors.alloc_failed() || opacities.alloc_failed();
}

size_t RenderPoints::memory_usage() const {
  size_t total = sizeof(*this);
  AddBytes(&total, name.capacity());
  AddBytes(&total, prim_path.capacity());
  AddBytes(&total, points.memory_usage());
  AddBytes(&total, normals.memory_usage());
  AddBytes(&total, widths.memory_usage());
  AddBytes(&total, colors.memory_usage());
  AddBytes(&total, opacities.memory_usage());
  return total;
}

//
// RenderCurves
//

void RenderCurves::compact() {
  points.shrink_to_fit();
  widths.shrink_to_fit();
  colors.shrink_to_fit();
  opacities.shrink_to_fit();
  tessellated_points.shrink_to_fit();
  tessellated_widths.shrink_to_fit();
  tessellated_colors.shrink_to_fit();
  tessellated_opacities.shrink_to_fit();
}

bool RenderCurves::has_alloc_failure() const {
  return points.alloc_failed() || widths.alloc_failed() ||
         colors.alloc_failed() || opacities.alloc_failed() ||
         tessellated_points.alloc_failed() ||
         tessellated_widths.alloc_failed() ||
         tessellated_colors.alloc_failed() ||
         tessellated_opacities.alloc_failed();
}

size_t RenderCurves::memory_usage() const {
  size_t total = sizeof(*this);
  AddBytes(&total, name.capacity());
  AddBytes(&total, prim_path.capacity());
  AddBytes(&total, VectorBytes(curve_vertex_counts));
  AddBytes(&total, points.memory_usage());
  AddBytes(&total, widths.memory_usage());
  AddBytes(&total, colors.memory_usage());
  AddBytes(&total, opacities.memory_usage());
  AddBytes(&total, VectorBytes(tessellated_vertex_counts));
  AddBytes(&total, tessellated_points.memory_usage());
  AddBytes(&total, tessellated_widths.memory_usage());
  AddBytes(&total, tessellated_colors.memory_usage());
  AddBytes(&total, tessellated_opacities.memory_usage());
  return total;
}

//
// RenderPointInstancer
//

size_t RenderPointInstancer::memory_usage() const {
  size_t total = sizeof(*this);
  AddBytes(&total, StringVectorBytes(prototype_paths));
  AddBytes(&total, VectorBytes(prototype_node_ids));
  AddBytes(&total, VectorBytes(prototype_mesh_offsets));
  AddBytes(&total, VectorBytes(prototype_mesh_ids));
  AddBytes(&total, VectorBytes(prototype_mesh_transforms));
  AddBytes(&total, VectorBytes(proto_indices));
  AddBytes(&total, VectorBytes(positions));
  AddBytes(&total, VectorBytes(orientations));
  AddBytes(&total, VectorBytes(scales));
  AddBytes(&total, VectorBytes(velocities));
  AddBytes(&total, VectorBytes(angular_velocities));
  AddBytes(&total, VectorBytes(ids));
  AddBytes(&total, VectorBytes(invisible_ids));
  AddBytes(&total, VectorBytes(inactive_ids));
  AddBytes(&total, VectorBytes(transforms));
  AddBytes(&total, VectorBytes(instance_visible));
  AddBytes(&total, VectorBytes(compact_instances));
  AddBytes(&total, name.capacity());
  AddBytes(&total, prim_path.capacity());
  AddBytes(&total, validation_error.capacity());
  return total;
}

size_t RenderPointInstancer::prototype_mesh_count(size_t prototype_index) const {
  if (prototype_index + 1 >= prototype_mesh_offsets.size()) return 0;
  const uint32_t begin = prototype_mesh_offsets[prototype_index];
  const uint32_t end = prototype_mesh_offsets[prototype_index + 1];
  return end >= begin ? static_cast<size_t>(end - begin) : 0;
}

bool RenderPointInstancer::has_valid_prototype_mesh_bindings() const {
  if (prototype_mesh_offsets.size() != prototype_paths.size() + 1) return false;
  if (prototype_node_ids.size() != prototype_paths.size()) return false;
  if (prototype_mesh_transforms.size() != prototype_mesh_ids.size()) return false;
  if (prototype_mesh_offsets.empty() || prototype_mesh_offsets[0] != 0) return false;
  uint32_t prev = 0;
  for (uint32_t offset : prototype_mesh_offsets) {
    if (offset < prev) return false;
    prev = offset;
  }
  return prototype_mesh_offsets.back() == prototype_mesh_ids.size();
}

size_t RenderPointInstancer::visible_instance_count() const {
  if (instance_visible.empty() && !compact_instances.empty()) {
    size_t total = 0;
    for (const CompactInstance& instance : compact_instances) {
      if ((instance.flags & 1u) != 0) ++total;
    }
    return total;
  }
  if (instance_visible.empty()) return instance_count();
  size_t total = 0;
  for (uint8_t visible : instance_visible) {
    if (visible) ++total;
  }
  return total;
}

bool RenderPointInstancer::has_valid_draw_range(size_t total_draw_count) const {
  const size_t start = draw_start;
  const size_t count = draw_count;
  return start <= total_draw_count && count <= total_draw_count - start;
}

//
// RenderCamera
//

float RenderCamera::fov_y() const {
  if (type == CameraType::Orthographic || focal_length <= 0.0f ||
      vertical_aperture <= 0.0f) {
    return 0.0f;
  }
  // fov_y = 2 * atan(vertical_aperture / (2 * focal_length))
  return 2.0f * std::atan(vertical_aperture / (2.0f * focal_length));
}

float RenderCamera::fov_x() const {
  if (type == CameraType::Orthographic || focal_length <= 0.0f ||
      horizontal_aperture <= 0.0f) {
    return 0.0f;
  }
  return 2.0f * std::atan(horizontal_aperture / (2.0f * focal_length));
}

float RenderCamera::aspect_ratio() const {
  if (vertical_aperture <= 0.0f) return 1.0f;
  return horizontal_aperture / vertical_aperture;
}

//
// RenderScene
//

size_t RenderScene::memory_usage() const {
  size_t total = sizeof(*this);

  /* Include catalog capacities and owned metadata strings.  The previous
   * estimate counted only object sizes, which made the public memory budget
   * unusable for scenes whose records were mostly paths and lookup keys. */
  AddBytes(&total, name.capacity());
  AddBytes(&total, default_prim.capacity());
  AddBytes(&total, render_settings_path.capacity());
  AddBytes(&total, working_color_space.capacity());
  AddBytes(&total, VectorBytes(root_nodes));
  AddBytes(&total, StringMapBytes(node_by_path));
  AddBytes(&total, StringMapBytes(mesh_by_path));
  AddBytes(&total, StringMapBytes(points_by_path));
  AddBytes(&total, StringMapBytes(curves_by_path));
  AddBytes(&total, StringMapBytes(point_instancer_by_path));
  AddBytes(&total, StringMapBytes(material_by_path));

  // Per-record memory_usage() includes sizeof(record); account only for
  // reserved, unused slots here. TextureImage::memory_usage() covers its
  // payload alone, so its full catalog allocation is counted below.
  AddBytes(&total, VectorSlackBytes(meshes));
  AddBytes(&total, VectorSlackBytes(points));
  AddBytes(&total, VectorSlackBytes(curves));
  AddBytes(&total, VectorSlackBytes(point_instancers));
  AddBytes(&total, VectorBytes(images));
  for (const auto& mesh : meshes) {
    AddBytes(&total, mesh.memory_usage());
  }

  for (const auto& point_cloud : points) {
    AddBytes(&total, point_cloud.memory_usage());
  }

  for (const auto& curve : curves) {
    AddBytes(&total, curve.memory_usage());
  }

  for (const auto& instancer : point_instancers) {
    AddBytes(&total, instancer.memory_usage());
  }

  for (const auto& img : images) {
    AddBytes(&total, img.memory_usage());
    AddBytes(&total, img.name.capacity());
    AddBytes(&total, img.resolved_path.capacity());
    AddBytes(&total, img.asset_identifier.capacity());
  }

  // Estimate for other containers (saturating on overflow)
  auto add_size = [&total](size_t count, size_t elem_size) {
    AddBytes(&total, safe::saturating_mul(count, elem_size));
  };
  add_size(nodes.capacity(), sizeof(SceneNode));
  for (const auto& node : nodes) {
    AddBytes(&total, node.name.capacity());
    AddBytes(&total, node.prim_path.capacity());
    AddBytes(&total, node.prototype_path.capacity());
    AddBytes(&total, VectorBytes(node.children));
  }
  add_size(point_instance_draws.capacity(), sizeof(RenderPointInstanceDraw));
  add_size(materials.capacity(), sizeof(RenderMaterial));
  for (const auto& material : materials) {
    AddBytes(&total, material.name.capacity());
    AddBytes(&total, material.prim_path.capacity());
    AddBytes(&total, VectorBytes(material.diagnostics));
    for (const auto& diagnostic : material.diagnostics) {
      AddBytes(&total, diagnostic.material_path.capacity());
      AddBytes(&total, diagnostic.node_path.capacity());
      AddBytes(&total, diagnostic.shader_id.capacity());
      AddBytes(&total, diagnostic.message.capacity());
    }
    AddBytes(&total, VectorBytes(material.retained_params));
    for (const auto& parameter : material.retained_params) {
      AddBytes(&total, parameter.shader.capacity());
      AddBytes(&total, parameter.name.capacity());
    }
    AddBytes(&total, material.displacement_shader_path.capacity());
    AddBytes(&total, material.volume_shader_path.capacity());
    AddBytes(&total, material.volume_nodegraph_json.capacity());
    AddBytes(&total, material.preview_surface_nodegraph_json.capacity());
    AddBytes(&total, material.mtlx_config.version.capacity());
    AddBytes(&total, material.mtlx_config.name_space.capacity());
    AddBytes(&total, material.mtlx_config.colorspace.capacity());
    AddBytes(&total, material.mtlx_config.source_uri.capacity());
    if (material.preview_surface) {
      AddBytes(&total, sizeof(PreviewSurfaceShader));
    }
    if (material.openpbr) {
      AddBytes(&total, sizeof(OpenPBRSurfaceShader));
      AddBytes(&total, material.openpbr->nodegraph_json.capacity());
    }
  }
  add_size(textures.capacity(), sizeof(RenderTexture));
  for (const auto& texture : textures) {
    AddBytes(&total, texture.name.capacity());
    AddBytes(&total, texture.prim_path.capacity());
    AddBytes(&total, texture.asset_path.capacity());
    AddBytes(&total, texture.ktx2_hint.capacity());
    AddBytes(&total, texture.uv_primvar.capacity());
    AddBytes(&total, texture.source_color_space.capacity());
    AddBytes(&total, texture.target_color_space.capacity());
  }
  add_size(udim_textures.capacity(), sizeof(RenderUDIMTexture));
  for (const auto& udim : udim_textures) {
    AddBytes(&total, udim.prim_name.capacity());
    AddBytes(&total, udim.abs_path.capacity());
    AddBytes(&total, udim.display_name.capacity());
    AddBytes(&total, udim.asset_identifier.capacity());
    AddBytes(&total, VectorBytes(udim.tiles));
  }
  add_size(lights.capacity(), sizeof(RenderLight));
  for (const auto& light : lights) {
    AddBytes(&total, light.name.capacity());
    AddBytes(&total, light.prim_path.capacity());
    AddBytes(&total, light.shaping_ies_file.capacity());
    AddBytes(&total, light.texture_file.capacity());
    AddBytes(&total, StringVectorBytes(light.light_link_targets));
    AddBytes(&total, StringVectorBytes(light.shadow_link_targets));
    AddBytes(&total, StringVectorBytes(light.filter_targets));
  }
  add_size(cameras.capacity(), sizeof(RenderCamera));
  for (const auto& camera : cameras) {
    AddBytes(&total, camera.name.capacity());
    AddBytes(&total, camera.prim_path.capacity());
  }
  add_size(animations.capacity(), sizeof(AnimationClip));
  for (const auto& animation : animations) {
    AddBytes(&total, animation.name.capacity());
    AddBytes(&total, animation.prim_path.capacity());
    AddBytes(&total, StringVectorBytes(animation.clip_asset_paths));
    AddBytes(&total, VectorBytes(animation.channels));
    for (const auto& channel : animation.channels) {
      AddBytes(&total, channel.target_prim_path.capacity());
      AddBytes(&total, channel.property_name.capacity());
      AddBytes(&total, channel.target_skeleton_path.capacity());
      AddBytes(&total, StringVectorBytes(channel.joint_order));
      AddBytes(&total, StringVectorBytes(channel.blend_shape_order));
      AddBytes(&total, VectorBytes(channel.joint_remap));
      AddBytes(&total, VectorBytes(channel.keyframes));
      AddBytes(&total, VectorBytes(channel.array_values));
    }
  }
  add_size(skeletons.capacity(), sizeof(Skeleton));
  for (const auto& skeleton : skeletons) {
    AddBytes(&total, skeleton.name.capacity());
    AddBytes(&total, skeleton.display_name.capacity());
    AddBytes(&total, skeleton.prim_path.capacity());
    AddBytes(&total, skeleton.animation_source_path.capacity());
    AddBytes(&total, VectorBytes(skeleton.joints));
    for (const auto& joint : skeleton.joints) {
      AddBytes(&total, joint.name.capacity());
      AddBytes(&total, joint.path.capacity());
      AddBytes(&total, VectorBytes(joint.children));
    }
  }
  add_size(unsupported_renderables.capacity(), sizeof(UnsupportedRenderable));
  for (const auto& unsupported : unsupported_renderables) {
    AddBytes(&total, unsupported.prim_path.capacity());
    AddBytes(&total, unsupported.type_name.capacity());
    AddBytes(&total, unsupported.reason.capacity());
  }

  // Physics annotations are part of the retained render snapshot too. Keep
  // their string and extension-property storage in the public memory budget;
  // otherwise physics-heavy scenes under-report usage substantially.
  auto add_physics_properties = [&total](
      const std::vector<PhysicsProperty>& properties) {
    AddBytes(&total, VectorBytes(properties));
    for (const auto& property : properties) {
      AddBytes(&total, property.name.capacity());
      AddBytes(&total, property.value.capacity());
    }
  };
  AddBytes(&total, VectorBytes(physics.scenes));
  for (const auto& value : physics.scenes) {
    AddBytes(&total, value.prim_path.capacity());
    add_physics_properties(value.extension_properties);
  }
  AddBytes(&total, VectorBytes(physics.rigid_bodies));
  for (const auto& value : physics.rigid_bodies) {
    AddBytes(&total, value.prim_path.capacity());
    AddBytes(&total, value.simulation_owner.capacity());
    add_physics_properties(value.extension_properties);
  }
  AddBytes(&total, VectorBytes(physics.colliders));
  for (const auto& value : physics.colliders) {
    AddBytes(&total, value.prim_path.capacity());
    AddBytes(&total, value.simulation_owner.capacity());
    AddBytes(&total, value.approximation.capacity());
    add_physics_properties(value.extension_properties);
  }
  AddBytes(&total, VectorBytes(physics.joints));
  for (const auto& value : physics.joints) {
    AddBytes(&total, value.prim_path.capacity());
    AddBytes(&total, value.type_name.capacity());
    AddBytes(&total, value.body0.capacity());
    AddBytes(&total, value.body1.capacity());
    add_physics_properties(value.extension_properties);
  }
  AddBytes(&total, VectorBytes(physics.materials));
  for (const auto& value : physics.materials) {
    AddBytes(&total, value.prim_path.capacity());
    add_physics_properties(value.extension_properties);
  }
  AddBytes(&total, VectorBytes(physics.filtered_pairs));
  for (const auto& value : physics.filtered_pairs) {
    AddBytes(&total, value.prim_path.capacity());
    AddBytes(&total, StringVectorBytes(value.filtered_pair_paths));
  }
  AddBytes(&total, StringVectorBytes(physics.articulation_roots));

  return total;
}

const RenderMesh* RenderScene::get_mesh(int32_t mesh_id) const {
  if (mesh_id < 0 || static_cast<size_t>(mesh_id) >= meshes.size()) return nullptr;
  return &meshes[static_cast<size_t>(mesh_id)];
}

const RenderPoints* RenderScene::get_points(int32_t points_id) const {
  if (points_id < 0 || static_cast<size_t>(points_id) >= points.size()) return nullptr;
  return &points[static_cast<size_t>(points_id)];
}

const RenderCurves* RenderScene::get_curves(int32_t curves_id) const {
  if (curves_id < 0 || static_cast<size_t>(curves_id) >= curves.size()) {
    return nullptr;
  }
  return &curves[static_cast<size_t>(curves_id)];
}

const RenderMaterial* RenderScene::get_material(int32_t material_id) const {
  if (material_id < 0 ||
      static_cast<size_t>(material_id) >= materials.size()) {
    return nullptr;
  }
  return &materials[static_cast<size_t>(material_id)];
}

const RenderPointInstancer* RenderScene::get_point_instancer(
    int32_t instancer_id) const {
  if (instancer_id < 0 ||
      static_cast<size_t>(instancer_id) >= point_instancers.size()) {
    return nullptr;
  }
  return &point_instancers[static_cast<size_t>(instancer_id)];
}

const RenderPointInstanceDraw* RenderScene::get_point_instance_draw(
    size_t draw_id) const {
  if (draw_id >= point_instance_draws.size()) return nullptr;
  return &point_instance_draws[draw_id];
}

RenderPointInstanceDrawView RenderScene::get_point_instance_draw_view(
    size_t draw_id) const {
  RenderPointInstanceDrawView view;
  view.draw = get_point_instance_draw(draw_id);
  if (!view.draw) return view;
  view.instancer = get_point_instancer(view.draw->point_instancer_id);
  view.mesh = get_mesh(view.draw->mesh_id);
  view.expanded_mesh = get_mesh(view.draw->expanded_mesh_id);
  view.material = get_material(view.draw->material_id);
  return view;
}

RenderPointInstanceDrawRange RenderScene::get_point_instancer_draws(
    int32_t instancer_id) const {
  RenderPointInstanceDrawRange range;
  const RenderPointInstancer* instancer = get_point_instancer(instancer_id);
  if (!instancer || instancer->draw_count == 0) return range;
  const size_t start = instancer->draw_start;
  const size_t count = instancer->draw_count;
  if (start > point_instance_draws.size() ||
      count > point_instance_draws.size() - start) {
    return range;
  }
  range.data = point_instance_draws.data() + start;
  range.size = count;
  return range;
}

const SceneNode* RenderScene::get_node(int32_t node_id) const {
  if (node_id < 0 || static_cast<size_t>(node_id) >= nodes.size()) return nullptr;
  return &nodes[static_cast<size_t>(node_id)];
}

bool RenderScene::has_valid_point_instance_draw_ranges() const {
  for (size_t instancer_id = 0; instancer_id < point_instancers.size();
       ++instancer_id) {
    const RenderPointInstancer& instancer = point_instancers[instancer_id];
    if (!instancer.has_valid_draw_range(point_instance_draws.size())) {
      return false;
    }
    for (size_t draw_i = 0; draw_i < instancer.draw_count; ++draw_i) {
      const size_t draw_id = static_cast<size_t>(instancer.draw_start) + draw_i;
      if (draw_id >= point_instance_draws.size()) return false;
      if (point_instance_draws[draw_id].point_instancer_id !=
          static_cast<int32_t>(instancer_id)) {
        return false;
      }
    }
  }
  return true;
}

RenderScene::Stats RenderScene::get_stats() const {
  Stats s = {};
  s.node_count = nodes.size();
  s.mesh_count = meshes.size();
  s.points_count = points.size();
  s.point_instancer_count = point_instancers.size();
  s.point_instance_draw_count = point_instance_draws.size();
  s.material_count = materials.size();
  s.texture_count = textures.size();
  s.image_count = images.size();
  s.light_count = lights.size();
  s.camera_count = cameras.size();
  s.animation_count = animations.size();
  s.skeleton_count = skeletons.size();

  for (const auto& mesh : meshes) {
    s.total_vertices += mesh.point_count();

    // Count triangles
    if (mesh.is_triangulated) {
      s.total_triangles += mesh.triangulated_indices.size() / 3;
    } else {
      // Estimate from face counts
      for (size_t i = 0; i < mesh.face_vertex_counts.size(); ++i) {
        uint32_t nverts = mesh.face_vertex_counts[i];
        if (nverts >= 3) {
          s.total_triangles += nverts - 2;  // Triangle fan decomposition
        }
      }
    }
  }

  for (const auto& instancer : point_instancers) {
    s.point_instance_count += instancer.instance_count();
    s.visible_point_instance_count += instancer.visible_instance_count();
  }

  for (const auto& point_cloud : points) {
    s.point_cloud_point_count += point_cloud.point_count();
  }

  s.curves_count = curves.size();
  for (const auto& curve : curves) {
    s.curve_count += curve.curve_count();
    s.curve_tessellated_point_count += curve.tessellated_point_count();
  }

  s.memory_bytes = memory_usage();
  return s;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
