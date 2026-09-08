// SPDX-License-Identifier: Apache-2.0
#include "render-converter.hh"
#include "next/schema/usd-shade.hh"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::Stage; using ::lightusd::next::UsdPrim;
namespace {
std::string FirstValidBoundMaterialPath(const Stage& stage,
                                        const ::lightusd::next::UsdPrim& prim,
                                        const std::string& purpose) {
  static const char* kPreviewOrder[] = {"material:binding:preview",
                                        "material:binding",
                                        "material:binding:full"};
  static const char* kFullOrder[] = {"material:binding:full",
                                     "material:binding",
                                     "material:binding:preview"};
  const char* const* bindingOrder = purpose == "full" ? kFullOrder
                                                       : kPreviewOrder;
  for (size_t i = 0; i < 3; ++i) {
    const char* rel = bindingOrder[i];
    const std::vector<::lightusd::next::Path>* targets =
        prim.GetRelationship(rel);
    if (!targets || targets->empty()) continue;
    const std::string p = (*targets)[0].str();
    if (!p.empty() && ::lightusd::tydra::next::IsMaterial(
                          stage.GetPrimAtPath(p))) {
      return p;
    }
  }
  return "";
}

std::string FindInheritedMaterialBinding(const Stage& stage,
                                         const std::string& prim_path,
                                         const std::string& purpose) {
  // Core UsdShade resolution now validates targets at every purpose/ancestor
  // step and associates bindMaterialAs with the relationship that actually
  // won. Keep one implementation shared by the converter and applications.
  return ::lightusd::next::GetInheritedBoundMaterialPathForPurpose(
      stage, prim_path, purpose.empty() ? "preview" : purpose);
}
}  // namespace
void RenderSceneConverter::AssignMaterialBindings(const Stage& stage,
                                                  RenderScene* scene) {
  if (!scene) return;
  for (RenderCurves& curves : scene->curves) {
    const std::string material_path =
        FindInheritedMaterialBinding(stage, curves.prim_path,
                                     config_.material.binding_purpose);
    if (!material_path.empty()) {
      const auto it = scene->material_by_path.find(material_path);
      if (it != scene->material_by_path.end()) curves.material_id = it->second;
    }
  }
  for (RenderMesh& mesh : scene->meshes) {
    AssignMeshMaterialBinding(stage, *scene, &mesh);
  }

  // Optional default material for unbound geometry (legacy
  // assign_default_material parity).
  if (config_.material.assign_default_material) {
    for (RenderMesh& mesh : scene->meshes) {
      if (mesh.material_id < 0) {
        mesh.material_id = GetOrCreateDefaultMaterial(scene);
      }
    }
    for (RenderCurves& curves : scene->curves) {
      if (curves.material_id < 0) {
        curves.material_id = GetOrCreateDefaultMaterial(scene);
      }
    }
  }
}

void RenderSceneConverter::AssignMeshMaterialBinding(const Stage& stage,
                                                     const RenderScene& scene,
                                                     RenderMesh* mesh) {
  if (!mesh) return;
  const std::string material_path =
      FindInheritedMaterialBinding(stage, mesh->prim_path,
                                   config_.material.binding_purpose);
  if (!material_path.empty()) {
    const auto it = scene.material_by_path.find(material_path);
    if (it != scene.material_by_path.end()) mesh->material_id = it->second;
  }

  // GeomSubset material bindings (familyName == materialBind): USD subsets
  // are arbitrary face-index sets; the range-based MaterialSubset model
  // stores one entry per CONSECUTIVE run of face indices.
  UsdPrim mesh_prim = stage.GetPrimAtPath(mesh->prim_path);
  if (!mesh_prim.IsValid()) return;
  for (const GeomSubset& sub : GetGeomSubsets(mesh_prim)) {
    if (!sub.family_name.empty() && sub.family_name != "materialBind") continue;
    std::string sub_mat = sub.material_path;
    if (sub_mat.empty()) {
      UsdPrim sub_prim = stage.GetPrimAtPath(sub.path);
      if (sub_prim.IsValid()) {
        sub_mat = FirstValidBoundMaterialPath(
            stage, sub_prim, config_.material.binding_purpose);
      }
    }
    if (sub_mat.empty()) continue;
    const auto mit = scene.material_by_path.find(sub_mat);
    if (mit == scene.material_by_path.end()) continue;
    const uint32_t nfaces = static_cast<uint32_t>(mesh->face_count());
    // Sort + split into consecutive runs, dropping out-of-range faces.
    // Authored subset indices use the ORIGINAL face numbering; when
    // sanitization dropped faces, route them through the old->new remap
    // first so the surviving faces keep their bindings.
    std::vector<uint32_t> faces;
    faces.reserve(sub.indices().size());
    for (int32_t fi : sub.indices()) {
      if (fi < 0) continue;
      uint32_t face = static_cast<uint32_t>(fi);
      if (mesh->sanitize_dropped_faces > 0) {
        if (face >= mesh->sanitize_face_remap.size()) continue;
        const int32_t remapped = mesh->sanitize_face_remap[face];
        if (remapped < 0) continue;  // face was dropped by sanitize
        face = static_cast<uint32_t>(remapped);
      }
      if (face < nfaces) {
        faces.push_back(face);
      }
    }
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    size_t run_start = 0;
    for (size_t i = 1; i <= faces.size(); ++i) {
      if (i == faces.size() || faces[i] != faces[i - 1] + 1) {
        RenderMesh::MaterialSubset ms;
        ms.face_start = faces[run_start];
        ms.face_count = static_cast<uint32_t>(i - run_start);
        ms.material_id = mit->second;
        mesh->material_subsets.push_back(ms);
        run_start = i;
      }
    }
  }

  // Remap subset runs from polygon-face space into TRIANGLE space using
  // the triangulation prefix sums (an N-gon becomes N-2 triangles;
  // holes/degenerate faces contribute 0). Subset indices were already
  // remapped into the post-sanitize face numbering above, so this holds
  // even when sanitization dropped faces.
  if (!mesh->material_subsets.empty() &&
      !mesh->face_triangle_offsets.empty()) {
    const std::vector<uint32_t>& offs = mesh->face_triangle_offsets;
    const uint32_t nfaces_tri =
        static_cast<uint32_t>(offs.size() > 0 ? offs.size() - 1 : 0);
    std::vector<RenderMesh::MaterialSubset> remapped;
    remapped.reserve(mesh->material_subsets.size());
    for (const RenderMesh::MaterialSubset& ms : mesh->material_subsets) {
      if (ms.face_start >= nfaces_tri) continue;
      const uint32_t face_end =
          std::min(ms.face_start + ms.face_count, nfaces_tri);
      const uint32_t tri_start = offs[ms.face_start];
      const uint32_t tri_count = offs[face_end] - tri_start;
      if (tri_count == 0) continue;
      remapped.push_back(
          RenderMesh::MaterialSubset{tri_start, tri_count, ms.material_id});
    }
    mesh->material_subsets = std::move(remapped);
  }
}

int32_t RenderSceneConverter::GetOrCreateDefaultMaterial(RenderScene* scene) {
  // Same sentinel path as the legacy converter.
  constexpr const char* kDefaultMaterialPath = "/__lightusd_default_material__";
  const auto it = scene->material_by_path.find(kDefaultMaterialPath);
  if (it != scene->material_by_path.end()) return it->second;
  RenderMaterial material;
  material.name = config_.material.default_material_name.empty()
                      ? "defaultMaterial"
                      : config_.material.default_material_name;
  material.prim_path = kDefaultMaterialPath;
  material.shader_type = RenderMaterial::ShaderType::PreviewSurface;
  // PreviewSurfaceShader defaults (0.18 diffuse / 0.5 roughness / opaque)
  // match the legacy default material parameters.
  material.preview_surface = std::make_unique<PreviewSurfaceShader>();
  const int32_t id = static_cast<int32_t>(scene->materials.size());
  scene->materials.push_back(std::move(material));
  scene->material_by_path[kDefaultMaterialPath] = id;
  return id;
}

void RenderSceneConverter::AssignPointInstanceDrawMaterials(RenderScene* scene) {
  if (!scene) return;
  for (RenderPointInstanceDraw& draw : scene->point_instance_draws) {
    if (draw.mesh_id < 0 ||
        static_cast<size_t>(draw.mesh_id) >= scene->meshes.size()) {
      draw.material_id = -1;
      continue;
    }
    draw.material_id = scene->meshes[static_cast<size_t>(draw.mesh_id)].material_id;
  }
}
}}}  // namespace lightusd::tydra::next
