// SPDX-License-Identifier: Apache-2.0
#include <iostream>
#include <limits>

#include "lightusd-core.hh"
#include "tydra/render-converter.hh"
#include "tydra/usd-export.hh"
#define CHECK(x)                                                      \
  do {                                                                \
    if (!(x)) {                                                       \
      std::cerr << "failed at " << __LINE__ << ": " #x "\n" << error; \
      return 1;                                                       \
    }                                                                 \
  } while (0)
int main() {
  namespace r = lightusd::tydra;
  std::string output, warning, error;
  r::RenderScene scene;
  scene.up_axis = r::RenderScene::UpAxis::Z;
  scene.meters_per_unit = 0.01f;
  scene.meshes.emplace_back();
  auto& mesh = scene.meshes[0];
  mesh.name = "Quoted \"quad\"";
  for (float f : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f})
    mesh.points.push_back(f);
  mesh.face_vertex_counts.push_back(4);
  for (uint32_t v : {0u, 1u, 2u, 3u}) mesh.face_vertex_indices.push_back(v);
  for (uint32_t v : {0u, 1u, 2u, 0u, 2u, 3u}) {
    mesh.triangulated_indices.push_back(v);
    mesh.triangulated_face_vertex_indices.push_back(v);
  }
  mesh.face_triangle_offsets = {0, 2};
  mesh.is_triangulated = true;
  for (float f : {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f})
    mesh.texcoords_0.push_back(f);
  mesh.texcoords_0_interp = r::Interpolation::FaceVarying;
  for (float f : {0.f, 0.f, 1.f}) mesh.normals.push_back(f);
  mesh.normals_interp = r::Interpolation::Uniform;
  for (size_t i = 0; i < 4; i++) {
    mesh.colors.push_back(1);
    mesh.colors.push_back(0);
    mesh.colors.push_back(0);
    mesh.colors.push_back(0.5f);
    mesh.opacities.push_back(0.5f);
  }
  mesh.primvars.emplace_back();
  mesh.primvars[0].name = "basis";
  mesh.primvars[0].format = r::VertexFormat::Matrix33;
  mesh.primvars[0].interpolation = r::Interpolation::Constant;
  for (size_t i = 0; i < 9; i++)
    mesh.primvars[0].float_data.push_back(i % 4 == 0 ? 1.f : 0.f);
  mesh.material_id = 0;
  mesh.material_subsets.push_back({1, 1, 1, "second_triangle"});
  scene.materials.resize(2);
  scene.materials[0].name = "Red";
  scene.materials[1].name = "Green";
  for (auto& material : scene.materials)
    material.preview_surface = std::make_shared<r::PreviewSurfaceShader>();
  scene.materials[0].preview_surface->diffuse_color.value = {1, 0, 0, 1};
  scene.textures.emplace_back();
  scene.textures[0].asset_path = "textures/checker@asset.png";
  scene.textures[0].output_channel = r::RenderTexture::Channel::G;
  scene.textures[0].scale_value = {2, 2, 2, 1};
  scene.textures[0].bias = {-1, -1, -1, 0};
  scene.materials[1].preview_surface->normal.texture_id = 0;
  scene.materials[1].preview_surface->diffuse_color.texture_id = 0;
  scene.nodes.emplace_back();
  scene.nodes[0].type = r::NodeType::Mesh;
  scene.nodes[0].data_id = 0;
  scene.nodes[0].world_transform.m[12] = 7;
  scene.skeletons.emplace_back();
  scene.skeletons[0].name = "Skeleton";
  scene.skeletons[0].joints.emplace_back();
  scene.skeletons[0].joints[0].path = "Root";
  scene.skeletons[0].joints[0].name = "Root";
  mesh.skin = std::make_shared<r::RenderMesh::SkinBinding>();
  mesh.skin->influences_per_vertex = 1;
  mesh.skin->skeleton_id = 0;
  for (size_t i = 0; i < 4; i++) {
    mesh.skin->joint_indices.push_back(0);
    mesh.skin->joint_weights.push_back(1);
  }
  mesh.blend_shapes.emplace_back();
  mesh.blend_shapes[0].name = "Smile";
  for (size_t i = 0; i < 12; i++)
    mesh.blend_shapes[0].point_offsets.push_back(i == 1 ? 0.5f : 0.f);
  CHECK(r::export_to_usda(scene, output, &warning, &error));
  lightusd::Stage stage;
  if (!lightusd::LoadUSDFromMemory(
          reinterpret_cast<const uint8_t*>(output.data()), output.size(),
          &stage, &warning, &error)) {
    std::cerr << output << "\n";
    CHECK(false);
  }
  r::ConverterConfig config;
  config.material.load_textures = false;
  r::RenderSceneConverter converter(config);
  auto converted = converter.Convert(stage);
  error += converted.error;
  CHECK(converted.success);
  CHECK(converted.scene.meshes.size() == 1);
  CHECK(converted.scene.skeletons.size() == 1);
  CHECK(converted.scene.materials.size() >= 2);
  CHECK(converted.scene.meshes[0].point_count() == 4);
  CHECK(converted.scene.meshes[0].material_subsets.size() == 1);
  CHECK(converted.scene.meshes[0].blend_shapes.size() == 1);
  CHECK(!converted.scene.meshes[0].opacities.empty());
  CHECK(converted.scene.meshes[0].opacities[0] == 0.25f);
  CHECK(converted.scene.meshes[0].primvars.size() == 1);
  CHECK(converted.scene.meshes[0].primvars[0].format ==
        r::VertexFormat::Matrix33);
  CHECK(output.find("outputs:g>") != std::string::npos);
  CHECK(output.find("float4 inputs:scale=(2, 2, 2, 1)") != std::string::npos);
  bool translated = false;
  for (const auto& node : converted.scene.nodes)
    if (node.type == r::NodeType::Mesh)
      translated = node.world_transform.m[12] == 7;
  CHECK(translated);
  const auto prior = output;
  CHECK(!r::export_to_usda(scene, output, &warning, &error, 64));
  CHECK(output == prior);
  mesh.triangulated_indices.mutable_at(0) = 100;
  CHECK(!r::export_to_usda(scene, output, &warning, &error));
  CHECK(output == prior);
  mesh.triangulated_indices.mutable_at(0) = 0;
  scene.textures[0].asset_path = "bad@@@asset";
  CHECK(!r::export_to_usda(scene, output, &warning, &error));
  scene.textures[0].asset_path = "checker.png";
  mesh.primvars[0].format = static_cast<r::VertexFormat>(255);
  CHECK(!r::export_to_usda(scene, output, &warning, &error));
  CHECK(output == prior);
  mesh.primvars[0].format = r::VertexFormat::Matrix33;
  mesh.normals_interp = static_cast<r::Interpolation>(255);
  CHECK(!r::export_to_usda(scene, output, &warning, &error));
  CHECK(output == prior);
  mesh.normals_interp = r::Interpolation::Uniform;
  scene.animations.emplace_back();
  CHECK(!r::export_to_usda(scene, output, &warning, &error));
  return 0;
}
