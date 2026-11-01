// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
#include <algorithm>
#include <array>
#include <locale>
#include <sstream>
#include <limits>
#include <numeric>
#include <unordered_set>

#include "obj-export.hh"
#include "common-macros.inc"
#include "tiny-format.hh"

namespace lightusd {
namespace tydra {

#define PushError(msg) do { \
  if (err) { \
    (*err) += msg + "\n"; \
  } \
} while (0)

bool export_to_obj(const RenderScene &scene, const int mesh_id,
  std::string &obj_str, std::string &mtl_str, std::string *warn, std::string *err) {

  //
  // NOTE:
  //
  // - Export GeomSubset(per-face material) as group(g) + usemtl
  // - Export skin weight as tinyobjloader's 'vw' extension
  //

  (void)obj_str;
  (void)mtl_str;
  (void)warn;

  std::stringstream ss;
  // OBJ/MTL numbers must not follow the process locale's decimal separator.
  ss.imbue(std::locale::classic());
  ss.precision(std::numeric_limits<float>::max_digits10);

  if (mesh_id < 0) {
    PUSH_ERROR_AND_RETURN("Invalid mesh_id");
  } else if (size_t(mesh_id) >= scene.meshes.size()) {
    PUSH_ERROR_AND_RETURN(fmt::format("mesh_id {} is out-of-range. scene.meshes.size {}", mesh_id, scene.meshes.size()));
  }

  const RenderMesh &mesh = scene.meshes[size_t(mesh_id)];

  ss << "# exported from LightUSD Tydra.\n";
  ss << "mtllib " << mesh_id << mesh.prim_name + ".mtl";
  ss << "\n";

  for (size_t i = 0; i < mesh.points.size(); i++) {
    ss << "v " << mesh.points[i][0] << " " << mesh.points[i][1] << " " << mesh.points[i][2] << "\n";
  }
  ss << "# " << mesh.points.size() << " vertices\n";

  if (mesh.joint_and_weights.elementSize > 0 &&
      mesh.joint_and_weights.jointIndices.size() == mesh.joint_and_weights.jointWeights.size() &&
      mesh.joint_and_weights.jointWeights.size() / size_t(mesh.joint_and_weights.elementSize) == mesh.points.size() &&
      mesh.joint_and_weights.jointWeights.size() % size_t(mesh.joint_and_weights.elementSize) == 0) {

    size_t elementSize = size_t(mesh.joint_and_weights.elementSize); // # of weights per vertex.
    for (size_t i = 0; i < mesh.points.size(); i++) {
      ss << "vw " << i << " ";
      for (size_t w = 0; w < elementSize; w++) {
        if (w > 0) {
          ss << " ";
        }
        ss << mesh.joint_and_weights.jointIndices[i * elementSize + w] << " " << mesh.joint_and_weights.jointWeights[i * elementSize + w];
      }
      ss << "\n";
    }
  }

  bool has_texcoord = false;
  bool is_facevarying_texcoord = false;
  size_t texcoord_count = 0;
  bool has_normal = false;
  bool is_facevarying_normal = false;
  size_t normal_count = 0;

  // primary texcoord only
  if (mesh.texcoords.count(0)) {
    const VertexAttribute &texcoord = mesh.texcoords.at(0);
    if (texcoord.variability == VertexVariability::FaceVarying) {
      is_facevarying_texcoord = true;
    } else if (texcoord.variability == VertexVariability::Vertex) {
      is_facevarying_texcoord = false;
    } else {
      PUSH_ERROR_AND_RETURN("Vertex variability must be either 'vertex' or 'facevarying' for texcoord0");
    }
    if (texcoord.format == VertexAttributeFormat::Vec2) {
      const float *ptr = reinterpret_cast<const float *>(texcoord.buffer());
      texcoord_count = texcoord.vertex_count();
      for (size_t i = 0; i < texcoord_count; i++) {
        ss << "vt " << ptr[2 * i + 0] << " " << ptr[2 * i + 1] << "\n";
      }
      has_texcoord = true;
    }
  }

  if (!mesh.normals.empty()) {
    if (mesh.normals.variability == VertexVariability::FaceVarying) {
      is_facevarying_normal = true;
    } else if (mesh.normals.variability == VertexVariability::Vertex) {
      is_facevarying_normal = false;
    } else {
      PUSH_ERROR_AND_RETURN("Vertex variability must be either 'vertex' or 'facevarying' for normals");
    }
    if (mesh.normals.format == VertexAttributeFormat::Vec3) {
      const float *ptr = reinterpret_cast<const float *>(mesh.normals.buffer());
      normal_count = mesh.normals.vertex_count();
      for (size_t i = 0; i < normal_count; i++) {
        ss << "vn " << ptr[3 * i + 0] << " " << ptr[3 * i + 1] << " " << ptr[3 * i + 2] << "\n";
      }
      has_normal = true;
    }
  }

  const auto &counts = mesh.faceVertexCounts();
  const auto &indices = mesh.faceVertexIndices();
  const size_t num_faces = counts.size();

  // Corner offset of every face into faceVertexIndices (and face-varying
  // attributes), validated against the index array.
  std::vector<size_t> offsets(num_faces);
  {
    size_t offset = 0;
    for (size_t i = 0; i < num_faces; i++) {
      offsets[i] = offset;
      if (counts[i] > indices.size() - std::min(offset, indices.size())) {
        PUSH_ERROR_AND_RETURN(fmt::format("faceVertexCounts exceed faceVertexIndices at face {}", i));
      }
      offset += counts[i];
    }
  }

  // Groups in deterministic order: faces outside any subset first (so they
  // never inherit a later group's usemtl), then subsets by their first face.
  struct FaceGroup {
    std::string name;
    int material_id;
    std::vector<uint32_t> faces;
  };
  std::vector<FaceGroup> groups;
  {
    std::vector<bool> in_subset(num_faces, false);
    for (const auto &subset : mesh.material_subsetMap) {
      if (subset.first.empty()) {
        PUSH_ERROR_AND_RETURN("Empty material_subset name is not allowed.");
      }
      FaceGroup group{subset.first, subset.second.material_id, {}};
      for (const int32_t face : subset.second.indices()) {
        if (face < 0 || size_t(face) >= num_faces) {
          PUSH_ERROR_AND_RETURN(fmt::format("GeomSubset `{}` face index {} is out of range", subset.first, face));
        }
        group.faces.push_back(uint32_t(face));
        in_subset[size_t(face)] = true;
      }
      groups.push_back(std::move(group));
    }
    std::sort(groups.begin(), groups.end(), [](const FaceGroup &a, const FaceGroup &b) {
      const uint32_t fa = a.faces.empty() ? UINT32_MAX : *std::min_element(a.faces.begin(), a.faces.end());
      const uint32_t fb = b.faces.empty() ? UINT32_MAX : *std::min_element(b.faces.begin(), b.faces.end());
      return fa != fb ? fa < fb : a.name < b.name;
    });
    FaceGroup rest{"", mesh.material_id, {}};
    for (size_t i = 0; i < num_faces; i++) {
      if (!in_subset[i]) rest.faces.push_back(uint32_t(i));
    }
    if (!rest.faces.empty() || groups.empty()) groups.insert(groups.begin(), std::move(rest));
  }
  for (const auto &group : groups) {
    if (group.material_id >= 0 && size_t(group.material_id) >= scene.materials.size()) {
      PUSH_ERROR_AND_RETURN(fmt::format("Material id {} is out of range. scene.materials.size = {}",
                                        group.material_id, scene.materials.size()));
    }
  }

  for (const auto &group : groups) {
    if (!group.name.empty()) {
      ss << "g " << group.name << "\n";
    }
    if (group.material_id >= 0) {
      ss << "usemtl " << scene.materials[size_t(group.material_id)].name << "\n";
    }
    for (const uint32_t face : group.faces) {
      ss << "f";
      for (size_t f = 0; f < counts[face]; f++) {
        const size_t corner = offsets[face] + f;
        const uint32_t vertex = indices[corner];
        if (vertex >= mesh.points.size()) {
          PUSH_ERROR_AND_RETURN(fmt::format("faceVertexIndex {} is out of range", vertex));
        }
        // OBJ indices are 1-based; face-varying attributes index corners.
        const size_t t_idx = is_facevarying_texcoord ? corner : vertex;
        const size_t n_idx = is_facevarying_normal ? corner : vertex;
        const bool t = has_texcoord && t_idx < texcoord_count;
        const bool n = has_normal && n_idx < normal_count;
        if ((has_texcoord && !t) || (has_normal && !n)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Attribute index out of range at face {}", face));
        }
        ss << " " << (vertex + 1);
        if (t && n) {
          ss << "/" << (t_idx + 1) << "/" << (n_idx + 1);
        } else if (t) {
          ss << "/" << (t_idx + 1);
        } else if (n) {
          ss << "//" << (n_idx + 1);
        }
      }
      ss << "\n";
    }
    ss << "\n";
  }

  obj_str = ss.str();

  ss.str("");
  ss << "# exported from LightUSD Tydra.\n";

  // Original MTL spec: https://paulbourke.net/dataformats/mtl/
  // PBR extension: https://github.com/tinyobjloader/tinyobjloader/blob/release/pbr-mtl.md
  auto texture_name = [&](int32_t tex_id, std::string *name) -> bool {
    if (tex_id < 0 || size_t(tex_id) >= scene.textures.size()) {
      PushError(fmt::format("Invalid texture id {}. scene.textures.size = {}", tex_id, scene.textures.size()));
      return false;
    }
    const int64_t image_id = scene.textures[size_t(tex_id)].texture_image_id;
    if (image_id < 0 || image_id >= int64_t(scene.images.size())) {
      PushError(fmt::format("Invalid image id {}. scene.images.size = {}", image_id, scene.images.size()));
      return false;
    }
    *name = scene.images[size_t(image_id)].asset_identifier;
    if (name->empty()) {
      PushError(fmt::format("Filename for image id {} is empty.", image_id));
      return false;
    }
    return true;
  };
  // Emit `map_<key> file` for a texture or `<key> value` otherwise.
  auto emit = [&](const char *map_key, const char *key, int32_t tex_id,
                  const std::string &value) -> bool {
    if (tex_id >= 0) {
      std::string name;
      if (!texture_name(tex_id, &name)) return false;
      ss << map_key << " " << name << "\n";
    } else {
      ss << key << " " << value << "\n";
    }
    return true;
  };
  auto scalar = [&](float v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(std::numeric_limits<float>::max_digits10);
    os << v;
    return os.str();
  };
  auto color = [&](const std::array<float, 3> &c) {
    return scalar(c[0]) + " " + scalar(c[1]) + " " + scalar(c[2]);
  };

  std::vector<bool> written(scene.materials.size(), false);
  size_t material_count = 0;
  for (const auto &group : groups) {
    if (group.material_id < 0 || written[size_t(group.material_id)]) {
      continue;
    }
    written[size_t(group.material_id)] = true;
    ++material_count;
    const RenderMaterial &material = scene.materials[size_t(group.material_id)];
    ss << "newmtl " << material.name << "\n";
    const auto &shader = material.surfaceShader;
    if (!shader) {
      ss << "\n";
      continue;
    }
    bool ok = emit("map_Kd", "Kd", shader->diffuseColor.texture_id, color(shader->diffuseColor.value));
    if (shader->useSpecularWorkflow) {
      ok = ok && emit("map_Ks", "Ks", shader->specularColor.texture_id, color(shader->specularColor.value));
    } else {
      ok = ok && emit("map_Pm", "Pm", shader->metallic.texture_id, scalar(shader->metallic.value));
    }
    ok = ok && emit("map_Pr", "Pr", shader->roughness.texture_id, scalar(shader->roughness.value));
    ok = ok && emit("map_Ke", "Ke", shader->emissiveColor.texture_id, color(shader->emissiveColor.value));
    ok = ok && emit("map_d", "d", shader->opacity.texture_id, scalar(shader->opacity.value));
    // Clearcoat thickness/roughness (PBR extension).
    ok = ok && emit("map_Pc", "Pc", shader->clearcoat.texture_id, scalar(shader->clearcoat.value));
    ok = ok && emit("map_Pcr", "Pcr", shader->clearcoatRoughness.texture_id,
                    scalar(shader->clearcoatRoughness.value));
    // map_Ni is not in the original MTL definition.
    ok = ok && emit("map_Ni", "Ni", shader->ior.texture_id, scalar(shader->ior.value));
    ok = ok && emit("map_Ka", "Ka", shader->occlusion.texture_id, scalar(shader->occlusion.value));
    if (!ok) {
      return false;
    }
    // TODO: opacityThreshold
    ss << "\n";
  }
  ss << "# " << material_count << " materials.\n";

  mtl_str = ss.str();

  return true;
}


} // namespace tydra
} // namespace lightusd

