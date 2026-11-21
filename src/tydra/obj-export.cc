// SPDX-License-Identifier: Apache-2.0
#include "obj-export.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <ostream>
#include <streambuf>
#include <unordered_set>
#include <vector>

#include "export-text.hh"

namespace lightusd {
namespace tydra {
namespace {
using export_detail::TextBuffer;
std::string Token(std::string name) {
  for (char& c : name)
    if (static_cast<unsigned char>(c) <= 32 || c == '/' || c == '\\' ||
        c == '#')
      c = '_';
  return name.empty() ? "unnamed" : name;
}
bool OneLine(const std::string& value) {
  return value.find_first_of("\r\n") == std::string::npos &&
         value.find('\0') == std::string::npos;
}
}  // namespace
std::string OBJMeshBasename(const RenderMesh& mesh, int mesh_id) {
  return std::to_string(mesh_id) + Token(mesh.name);
}

bool export_to_obj(const RenderScene& scene, int mesh_id, std::string& obj,
                   std::string& mtl, std::string* warn, std::string* error,
                   size_t max_output_bytes) {
  const auto fail = [&](const char* message) {
    if (error) *error += std::string(message) + "\n";
    return false;
  };
  if (mesh_id < 0 || static_cast<size_t>(mesh_id) >= scene.meshes.size())
    return fail("Invalid mesh id");
  const auto& mesh = scene.meshes[static_cast<size_t>(mesh_id)];
  const size_t vertices = mesh.point_count();
  if (mesh.points.size() % 3)
    return fail("Points must contain complete xyz triples");
  const bool triangles = !mesh.triangulated_indices.empty();
  const auto& indices =
      triangles ? mesh.triangulated_indices : mesh.face_vertex_indices;
  const size_t faces =
      triangles ? indices.size() / 3 : mesh.face_vertex_counts.size();
  if ((triangles && indices.size() % 3) || faces > max_output_bytes / 2 ||
      vertices > max_output_bytes / 6)
    return fail("Invalid topology or output limit exceeded");
  std::vector<bool> hole_faces(mesh.face_vertex_counts.size(), false);
  for (uint32_t face : mesh.hole_faces) {
    if (face >= hole_faces.size()) return fail("Hole face index out of range");
    hole_faces[face] = true;
  }
  std::vector<size_t> offsets(faces + 1);
  std::vector<size_t> source_faces(faces);
  for (size_t f = 0; f < faces; ++f) {
    const size_t count = triangles ? 3 : mesh.face_vertex_counts[f];
    if (count < 3 || count > indices.size() - offsets[f])
      return fail("Invalid face vertex counts");
    offsets[f + 1] = offsets[f] + count;
    source_faces[f] = f;
  }
  if (offsets.back() != indices.size())
    return fail("Face counts do not consume the index array");
  if (triangles && !mesh.face_triangle_offsets.empty()) {
    const auto& ranges = mesh.face_triangle_offsets;
    if (ranges.front() != 0 || ranges.back() != faces ||
        ranges.size() != mesh.face_vertex_counts.size() + 1)
      return fail("Invalid source face mapping");
    for (size_t f = 0; f + 1 < ranges.size(); ++f) {
      if (ranges[f] > ranges[f + 1] || ranges[f + 1] > faces)
        return fail("Invalid source face mapping");
      for (size_t t = ranges[f]; t < ranges[f + 1]; ++t) source_faces[t] = f;
    }
  }
  if (triangles && !mesh.triangulated_face_vertex_indices.empty() &&
      mesh.triangulated_face_vertex_indices.size() != indices.size())
    return fail("Invalid source corner mapping");
  std::vector<int32_t> materials(faces, mesh.material_id);
  std::vector<int32_t> groups(faces, -1);
  for (size_t i = 0; i < mesh.material_subsets.size(); ++i) {
    const auto& subset = mesh.material_subsets[i];
    const bool triangle_ranges =
        triangles && !mesh.face_triangle_offsets.empty();
    const size_t range_limit =
        triangle_ranges ? faces : mesh.face_vertex_counts.size();
    if (subset.face_start > range_limit ||
        subset.face_count > range_limit - subset.face_start)
      return fail("Invalid material subset range");
    for (size_t f = 0; f < faces; ++f) {
      const size_t source = triangle_ranges ? f : source_faces[f];
      if (source >= subset.face_start &&
          source - subset.face_start < subset.face_count) {
        if (groups[f] >= 0) return fail("Overlapping material subsets");
        groups[f] = static_cast<int32_t>(i);
        materials[f] = subset.material_id;
      }
    }
  }
  for (const auto id : materials)
    if (id < -1 ||
        (id >= 0 && static_cast<size_t>(id) >= scene.materials.size()))
      return fail("Invalid material id");
  std::vector<std::string> material_names(scene.materials.size());
  std::unordered_set<std::string> used_names{"lightusd_unbound"};
  for (int32_t id : materials) {
    if (id < 0 || !material_names[static_cast<size_t>(id)].empty()) continue;
    std::string name = Token(scene.materials[static_cast<size_t>(id)].name);
    while (!used_names.insert(name).second) name += "_" + std::to_string(id);
    material_names[static_cast<size_t>(id)] = std::move(name);
  }
  const auto attribute_index = [&](Interpolation interpolation, size_t face,
                                   size_t corner, size_t vertex,
                                   size_t* result) {
    switch (interpolation) {
      case Interpolation::Constant:
        *result = 0;
        return true;
      case Interpolation::Uniform:
        *result = source_faces[face];
        return true;
      case Interpolation::Vertex:
      case Interpolation::Varying:
        *result = vertex;
        return true;
      case Interpolation::FaceVarying:
        if (triangles) {
          if (mesh.triangulated_face_vertex_indices.empty()) return false;
          *result = mesh.triangulated_face_vertex_indices[corner];
        } else
          *result = corner;
        return true;
    }
    return false;
  };
  TextBuffer object_buffer(max_output_bytes);
  std::ostream os(&object_buffer);
  os.imbue(std::locale::classic());
  os.precision(std::numeric_limits<float>::max_digits10);
  os << "# exported from LightUSD Tydra.\nmtllib "
     << OBJMeshBasename(mesh, mesh_id) << ".mtl\n";
  for (size_t i = 0; i < mesh.points.size(); i += 3) {
    if (!std::isfinite(mesh.points[i]) || !std::isfinite(mesh.points[i + 1]) ||
        !std::isfinite(mesh.points[i + 2]))
      return fail("Non-finite mesh point");
    os << "v " << mesh.points[i] << ' ' << mesh.points[i + 1] << ' '
       << mesh.points[i + 2] << '\n';
  }
  if (mesh.skin && mesh.skin->influences_per_vertex) {
    const auto& skin = *mesh.skin;
    if (skin.joint_indices.size() != skin.joint_weights.size() ||
        skin.joint_weights.size() / skin.influences_per_vertex != vertices ||
        skin.joint_weights.size() % skin.influences_per_vertex)
      return fail("Invalid skin weights");
    for (size_t v = 0; v < vertices; ++v) {
      os << "vw " << v;
      for (size_t j = 0; j < skin.influences_per_vertex; ++j) {
        const size_t index = v * skin.influences_per_vertex + j;
        if (!std::isfinite(skin.joint_weights[index]))
          return fail("Non-finite skin weight");
        os << ' ' << skin.joint_indices[index] << ' '
           << skin.joint_weights[index];
      }
      os << '\n';
    }
  }
  const auto write_attribute = [&](const FloatChunked& values,
                                   size_t components, const char* prefix) {
    if (values.size() % components) return false;
    for (size_t i = 0; i < values.size(); i += components) {
      os << prefix;
      for (size_t c = 0; c < components; ++c) {
        if (!std::isfinite(values[i + c])) return false;
        os << ' ' << values[i + c];
      }
      os << '\n';
    }
    return true;
  };
  if (!write_attribute(mesh.texcoords_0, 2, "vt") ||
      !write_attribute(mesh.normals, 3, "vn"))
    return fail("Invalid UV or normal payload");
  int32_t last_group = -2;
  for (size_t f = 0; f < faces; ++f) {
    if (!triangles && hole_faces[f]) continue;
    if (groups[f] != last_group) {
      const auto group = groups[f];
      const std::string name =
          group < 0 ? "default"
          : mesh.material_subsets[static_cast<size_t>(group)].name.empty()
              ? "subset_" + std::to_string(group)
              : Token(mesh.material_subsets[static_cast<size_t>(group)].name);
      os << "g " << name << '\n';
      os << "usemtl "
         << (materials[f] < 0
                 ? "lightusd_unbound"
                 : material_names[static_cast<size_t>(materials[f])])
         << '\n';
      last_group = groups[f];
    }
    os << 'f';
    for (size_t corner = offsets[f]; corner < offsets[f + 1]; ++corner) {
      const size_t vertex = indices[corner];
      if (vertex >= vertices) return fail("Mesh index out of range");
      size_t uv = 0, normal = 0;
      const bool has_uv = !mesh.texcoords_0.empty(),
                 has_normal = !mesh.normals.empty();
      if (has_uv &&
          (!attribute_index(mesh.texcoords_0_interp, f, corner, vertex, &uv) ||
           uv >= mesh.texcoords_0.size() / 2))
        return fail("UV index out of range");
      if (has_normal &&
          (!attribute_index(mesh.normals_interp, f, corner, vertex, &normal) ||
           normal >= mesh.normals.size() / 3))
        return fail("Normal index out of range");
      os << ' ' << vertex + 1;
      if (has_uv || has_normal) {
        os << '/';
        if (has_uv) os << uv + 1;
        if (has_normal) os << '/' << normal + 1;
      }
    }
    os << '\n';
    if (!os) return fail("OBJ output limit exceeded");
  }
  if (!os) return fail("OBJ output limit exceeded");
  TextBuffer material_buffer(max_output_bytes - object_buffer.text.size());
  std::ostream ms(&material_buffer);
  ms.imbue(std::locale::classic());
  ms.precision(std::numeric_limits<float>::max_digits10);
  std::vector<bool> written(scene.materials.size());
  if (std::find(materials.begin(), materials.end(), -1) != materials.end())
    ms << "newmtl lightusd_unbound\nKd 0.18 0.18 0.18\n";
  for (int32_t id : materials) {
    if (id < 0 || written[static_cast<size_t>(id)]) continue;
    written[static_cast<size_t>(id)] = true;
    const auto& material = scene.materials[static_cast<size_t>(id)];
    ms << "newmtl " << material_names[static_cast<size_t>(id)] << '\n';
    const auto emit = [&](const char* key, const ShaderParam& parameter,
                          bool color) {
      if (parameter.is_texture()) {
        if (static_cast<size_t>(parameter.texture_id) >= scene.textures.size())
          return false;
        const auto& texture =
            scene.textures[static_cast<size_t>(parameter.texture_id)];
        std::string filename = texture.asset_path;
        if (filename.empty() && texture.image_id >= 0 &&
            static_cast<size_t>(texture.image_id) < scene.images.size())
          filename = scene.images[static_cast<size_t>(texture.image_id)]
                         .asset_identifier;
        if (filename.empty() || !OneLine(filename)) return false;
        ms << "map_" << key << ' ' << filename << '\n';
      } else {
        if (!std::isfinite(parameter.value.x) ||
            (color && (!std::isfinite(parameter.value.y) ||
                       !std::isfinite(parameter.value.z))))
          return false;
        ms << key << ' ' << parameter.value.x;
        if (color) ms << ' ' << parameter.value.y << ' ' << parameter.value.z;
        ms << '\n';
      }
      return static_cast<bool>(ms);
    };
    PreviewSurfaceShader shader;
    if (material.preview_surface)
      shader = *material.preview_surface;
    else if (material.openpbr) {
      const auto& p = *material.openpbr;
      shader.diffuse_color = p.base_color;
      shader.metallic = p.base_metalness;
      shader.roughness = p.specular_roughness;
      shader.ior = p.specular_ior;
      shader.emissive_color = p.emission_color;
      if (!p.emission_luminance.is_texture()) {
        const float luminance = p.emission_luminance.as_float();
        if (luminance == 0)
          shader.emissive_color = ShaderParam{-1, {0, 0, 0, 1}};
        else if (!shader.emissive_color.is_texture()) {
          shader.emissive_color.value.x *= luminance;
          shader.emissive_color.value.y *= luminance;
          shader.emissive_color.value.z *= luminance;
        }
      }
      shader.opacity = p.opacity;
      shader.clearcoat = p.coat_weight;
      shader.clearcoat_roughness = p.coat_roughness;
      shader.normal = p.normal;
      if (warn)
        *warn += "OpenPBR material approximated as MTL PBR parameters\n";
    }
    if (!emit("Kd", shader.diffuse_color, true) ||
        !emit(shader.use_specular_workflow ? "Ks" : "Pm",
              shader.use_specular_workflow ? shader.specular_color
                                           : shader.metallic,
              shader.use_specular_workflow) ||
        !emit("Pr", shader.roughness, false) ||
        !emit("Ke", shader.emissive_color, true) ||
        !emit("d", shader.opacity, false) ||
        !emit("Pc", shader.clearcoat, false) ||
        !emit("Pcr", shader.clearcoat_roughness, false) ||
        !emit("Ni", shader.ior, false) || !emit("Ka", shader.occlusion, false))
      return fail("Invalid material or output limit exceeded");
    if (shader.normal.is_texture() && !emit("Bump", shader.normal, false))
      return fail("Invalid normal texture");
    ms << '\n';
  }
  if (!ms) return fail("MTL output limit exceeded");
  obj.swap(object_buffer.text);
  mtl.swap(material_buffer.text);
  return true;
}
}  // namespace tydra
}  // namespace lightusd
