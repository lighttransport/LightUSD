// SPDX-License-Identifier: Apache-2.0
#include <iostream>
#include <limits>

#include "reader/obj-reader.hh"
#include "tydra/obj-export.hh"
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) {                                              \
      std::cerr << "failed at " << __LINE__ << ": " #x "\n"; \
      return 1;                                              \
    }                                                        \
  } while (0)
int main() {
  namespace r = lightusd::tydra;
  r::RenderScene scene;
  scene.meshes.emplace_back();
  auto& mesh = scene.meshes[0];
  mesh.name = "Quad";
  for (float v : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f})
    mesh.points.push_back(v);
  mesh.face_vertex_counts.push_back(4);
  for (uint32_t i : {0u, 1u, 2u, 3u}) mesh.face_vertex_indices.push_back(i);
  for (float v : {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f})
    mesh.texcoords_0.push_back(v);
  mesh.texcoords_0_interp = r::Interpolation::FaceVarying;
  for (float v : {0.f, 0.f, 1.f}) mesh.normals.push_back(v);
  mesh.normals_interp = r::Interpolation::Uniform;
  scene.materials.resize(2);
  scene.materials[0].name = "Red";
  scene.materials[1].name = "Green";
  scene.materials[0].preview_surface =
      std::make_shared<r::PreviewSurfaceShader>();
  scene.materials[1].preview_surface =
      std::make_shared<r::PreviewSurfaceShader>();
  scene.materials[0].preview_surface->diffuse_color.value = {1, 0, 0, 1};
  scene.materials[1].preview_surface->diffuse_color.value = {0, 1, 0, 1};
  mesh.material_id = 0;
  std::string obj, mtl, warn, error;
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(obj.find("f 1/1/1 2/2/1 3/3/1 4/4/1") != std::string::npos);
  CHECK(mtl.find("Kd 1 0 0") != std::string::npos);
  mesh.hole_faces = {0};
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(obj.find("\nf ") == std::string::npos);
  mesh.hole_faces.clear();
  for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
    mesh.triangulated_indices.push_back(i);
    mesh.triangulated_face_vertex_indices.push_back(i);
  }
  mesh.is_triangulated = true;
  mesh.face_triangle_offsets = {0, 2};
  mesh.material_subsets.push_back({1, 1, 1, "green_faces"});
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(obj.find("g green_faces\nusemtl Green\nf 1/1/1 3/3/1 4/4/1") !=
        std::string::npos);
  CHECK(mtl.find("Kd 0 1 0") != std::string::npos);
  scene.textures.emplace_back();
  scene.textures[0].asset_path = "textures/red.png";
  scene.materials[1].preview_surface->diffuse_color.texture_id = 0;
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(mtl.find("map_Kd textures/red.png") != std::string::npos);
  mesh.skin = std::make_shared<r::RenderMesh::SkinBinding>();
  mesh.skin->influences_per_vertex = 2;
  for (size_t i = 0; i < mesh.point_count(); ++i) {
    mesh.skin->joint_indices.push_back(0);
    mesh.skin->joint_indices.push_back(1);
    mesh.skin->joint_weights.push_back(0.25f);
    mesh.skin->joint_weights.push_back(0.75f);
  }
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(obj.find("vw 0 0 0.25 1 0.75") != std::string::npos);
  scene.materials[1].preview_surface->normal.texture_id = 0;
  CHECK(r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(mtl.find("map_Bump textures/red.png") != std::string::npos);
  auto reloaded = lightusd::LoadOBJLayerFromMemory(
      "memory.obj", reinterpret_cast<const uint8_t*>(obj.data()), obj.size(),
      {}, &warn, &error);
  CHECK(reloaded && reloaded->prim_count() > 0);
  const std::string prior_obj = obj, prior_mtl = mtl;
  CHECK(!r::export_to_obj(scene, 0, obj, mtl, &warn, &error, 32));
  CHECK(obj == prior_obj && mtl == prior_mtl);
  mesh.triangulated_indices.mutable_at(0) = 100;
  CHECK(!r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(obj == prior_obj && mtl == prior_mtl);
  mesh.triangulated_indices.mutable_at(0) = 0;
  scene.textures[0].asset_path = "bad\nnewmtl injected";
  CHECK(!r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  scene.textures[0].asset_path = "red.png";
  mesh.material_subsets.push_back({1, 1, 0});
  CHECK(!r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  mesh.material_subsets.pop_back();
  mesh.points.mutable_at(0) = std::numeric_limits<float>::infinity();
  CHECK(!r::export_to_obj(scene, 0, obj, mtl, &warn, &error));
  CHECK(r::OBJMeshBasename(mesh, 0) == "0Quad");
  return 0;
}
