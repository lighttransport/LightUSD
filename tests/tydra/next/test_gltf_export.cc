// SPDX-License-Identifier: Apache-2.0
#include "tydra/next/gltf-export.hh"
#include <cmath>
#include <iostream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << "failed at " << __LINE__ << ": " #x "\n"; return 1; } } while (0)
int main() {
  namespace r = lightusd::tydra::next;
  r::RenderScene scene;
  scene.meters_per_unit = 1;
  scene.up_axis = r::RenderScene::UpAxis::Y;
  scene.meshes.emplace_back();
  auto& mesh = scene.meshes.back();
  mesh.name = "Triangle"; mesh.prim_path = "/Triangle";
  for (float f : {0.f,0.f,0.f,1.f,0.f,0.f,0.f,1.f,0.f}) mesh.points.push_back(f);
  for (uint32_t i : {0u, 1u, 2u}) mesh.triangulated_indices.push_back(i);
  mesh.is_triangulated = true;
  scene.nodes.emplace_back();
  scene.nodes[0].name = "Triangle";
  scene.nodes[0].type = r::NodeType::Mesh;
  scene.nodes[0].data_id = 0;
  scene.root_nodes.push_back(0);
  auto result = r::ExportGLB(scene);
  CHECK(result.success && result.glb.size() > 20 && result.losses.empty());
  r::GltfExportOptions options;
  options.max_output_bytes = 32;
  CHECK(!r::ExportGLB(scene, options).success);
  mesh.triangulated_face_vertex_indices.push_back(0);
  CHECK(!r::ExportGLB(scene).success);
  mesh.triangulated_face_vertex_indices.clear();
  mesh.triangulated_indices.mutable_at(0) = 99;
  CHECK(!r::ExportGLB(scene).success);
  mesh.triangulated_indices.mutable_at(0) = 0;
  mesh.points.mutable_at(0) = std::numeric_limits<float>::infinity();
  CHECK(!r::ExportGLB(scene).success);
  mesh.points.mutable_at(0) = 0;
  scene.nodes[0].children.push_back(0);
  CHECK(!r::ExportGLB(scene).success);
  scene.nodes[0].children.clear();
  scene.animations.emplace_back();
  result = r::ExportGLB(scene);
  CHECK(result.success && !result.losses.empty());
  options.max_output_bytes = 1024 * 1024;
  options.fail_on_loss = true;
  result = r::ExportGLB(scene, options);
  CHECK(!result.success && result.glb.empty() && !result.losses.empty());
  return 0;
}
