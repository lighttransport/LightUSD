// SPDX-License-Identifier: Apache-2.0
#include <iostream>
#include <limits>

#include "math-trig.hh"
#include "tydra/mesh-index.hh"
#define CHECK(x)                                                      \
  do {                                                                \
    if (!(x)) {                                                       \
      std::cerr << "failed at " << __LINE__ << ": " #x "\n" << error; \
      return 1;                                                       \
    }                                                                 \
  } while (0)
namespace r = lightusd::tydra;
static r::RenderMesh fixture() {
  r::RenderMesh mesh;
  mesh.name = "seam";
  for (float v : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f})
    mesh.points.push_back(v);
  mesh.face_vertex_counts.push_back(3);
  mesh.face_vertex_counts.push_back(3);
  for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
    mesh.face_vertex_indices.push_back(i);
    mesh.triangulated_indices.push_back(i);
  }
  for (uint32_t i = 0; i < 6; i++)
    mesh.triangulated_face_vertex_indices.push_back(i);
  mesh.face_triangle_offsets = {0, 1, 2};
  mesh.is_triangulated = true;
  for (float v : {0.f, 0.f, 1.f}) mesh.normals.push_back(v);
  mesh.normals_interp = r::Interpolation::Constant;
  for (float v : {0.f, 0.f, 1.f, 0.f, 1.f, 1.f, .5f, .5f, 1.f, 1.f, 0.f, 1.f})
    mesh.texcoords_0.push_back(v);
  mesh.texcoords_0_interp = r::Interpolation::FaceVarying;
  mesh.material_subsets.push_back({1, 1, 3, "second_face"});
  mesh.subdivision_face_source = {10, 20};
  mesh.skin = std::make_shared<r::RenderMesh::SkinBinding>();
  mesh.skin->influences_per_vertex = 2;
  for (size_t i = 0; i < 4; i++) {
    mesh.skin->joint_indices.push_back(0);
    mesh.skin->joint_indices.push_back(1);
    mesh.skin->joint_weights.push_back(.25f);
    mesh.skin->joint_weights.push_back(.75f);
  }
  mesh.primvars.emplace_back();
  auto& p = mesh.primvars.back();
  p.name = "integer";
  p.format = r::VertexFormat::UInt;
  p.uint_data.push_back(std::numeric_limits<uint32_t>::max());
  p.uint_data.push_back(2);
  for (uint32_t i : {0u, 1u, 0u, 1u}) p.indices.push_back(i);
  mesh.blend_shapes.emplace_back();
  auto& sparse = mesh.blend_shapes.back();
  sparse.name = "sparse";
  sparse.point_indices = {0, 3};
  for (float v : {.1f, 0.f, 0.f, 0.f, .2f, 0.f})
    sparse.point_offsets.push_back(v);
  sparse.inbetweens.emplace_back();
  for (float v : sparse.point_offsets)
    sparse.inbetweens[0].point_offsets.push_back(v * .5f);
  mesh.blend_shapes.emplace_back();
  auto& dense = mesh.blend_shapes.back();
  dense.name = "dense";
  for (float z : {0.f, 1.f, 2.f, 3.f}) {
    dense.point_offsets.push_back(0);
    dense.point_offsets.push_back(0);
    dense.point_offsets.push_back(z);
  }
  return mesh;
}
int main() {
  std::string error;
  r::MeshIndexOptions options;
  auto mesh = fixture();
  CHECK(r::BuildVertexIndices(&mesh, options, &error));
  CHECK(mesh.point_count() == 5);
  CHECK(mesh.triangulated_indices.size() == 6);
  const uint32_t expected[] = {0, 1, 2, 3, 2, 4};
  for (size_t i = 0; i < 6; i++)
    CHECK(mesh.triangulated_indices[i] == expected[i]);
  CHECK(mesh.face_vertex_counts.size() == 2 && mesh.face_vertex_counts[0] == 3);
  CHECK(mesh.texcoords_0_interp == r::Interpolation::Vertex &&
        mesh.texcoords_0.size() == 10);
  CHECK(mesh.texcoords_0[6] == .5f);
  CHECK(mesh.normals.size() == 15);
  CHECK(mesh.material_subsets[0].name == "second_face" &&
        mesh.material_subsets[0].material_id == 3);
  CHECK(mesh.subdivision_face_source == std::vector<uint32_t>({10, 20}));
  CHECK(mesh.skin->joint_weights.size() == 10 &&
        mesh.skin->joint_weights[6] == .25f);
  CHECK(mesh.primvars[0].indices.empty() &&
        mesh.primvars[0].uint_data.size() == 5);
  CHECK(mesh.primvars[0].uint_data[3] == std::numeric_limits<uint32_t>::max());
  CHECK(mesh.blend_shapes[0].point_indices == std::vector<uint32_t>({0, 3, 4}));
  CHECK(mesh.blend_shapes[0].point_offsets.size() == 9 &&
        mesh.blend_shapes[0].point_offsets[3] == .1f);
  CHECK(mesh.blend_shapes[0].inbetweens[0].point_offsets.size() == 9);
  CHECK(mesh.blend_shapes[1].point_offsets.size() == 15 &&
        mesh.blend_shapes[1].point_offsets[11] == 0);
  auto fast = fixture();
  options.deduplicate = false;
  CHECK(r::BuildVertexIndices(&fast, options, &error));
  CHECK(fast.point_count() == 6);
  for (size_t i = 0; i < 6; i++) CHECK(fast.triangulated_indices[i] == i);
  auto polygon = fixture();
  polygon.triangulated_indices = r::UInt32Chunked{};
  polygon.triangulated_face_vertex_indices = r::UInt32Chunked{};
  polygon.face_triangle_offsets.clear();
  polygon.is_triangulated = false;
  options.deduplicate = true;
  CHECK(r::BuildVertexIndices(&polygon, options, &error));
  CHECK(polygon.point_count() == 5 && !polygon.is_triangulated &&
        polygon.triangulated_indices.empty());
  auto unchanged = fixture();
  options.memory_budget_bytes = 1;
  CHECK(!r::BuildVertexIndices(&unchanged, options, &error));
  CHECK(unchanged.point_count() == 4 &&
        unchanged.texcoords_0_interp == r::Interpolation::FaceVarying);
  options.memory_budget_bytes = size_t(512) << 20;
  auto invalid = fixture();
  invalid.primvars[0].indices.mutable_at(0) = 99;
  CHECK(!r::BuildVertexIndices(&invalid, options, &error));
  CHECK(invalid.point_count() == 4);
  invalid = fixture();
  invalid.blend_shapes[0].point_indices[0] = 99;
  CHECK(!r::BuildVertexIndices(&invalid, options, &error));
  CHECK(invalid.normals_interp == r::Interpolation::Constant);
  invalid = fixture();
  invalid.triangulated_indices.mutable_at(0) = 99;
  CHECK(!r::BuildVertexIndices(&invalid, options, &error));
  invalid = fixture();
  invalid.points.mutable_at(0) = std::numeric_limits<float>::infinity();
  CHECK(!r::BuildVertexIndices(&invalid, options, &error));
  invalid = fixture();
  invalid.texcoords_0.mutable_at(0) = std::numeric_limits<float>::quiet_NaN();
  CHECK(!r::BuildVertexIndices(&invalid, options, &error));
  auto winding = fixture();
  winding.skin.reset();
  winding.blend_shapes.clear();
  winding.primvars.clear();
  winding.material_subsets.clear();
  winding.subdivision_face_source.clear();
  winding.face_vertex_counts.clear();
  winding.face_vertex_counts.push_back(3);
  winding.face_vertex_indices.clear();
  winding.triangulated_indices.clear();
  winding.triangulated_face_vertex_indices.clear();
  for (uint32_t i : {0u, 1u, 2u}) winding.face_vertex_indices.push_back(i);
  for (uint32_t i : {2u, 1u, 0u}) {
    winding.triangulated_indices.push_back(i);
    winding.triangulated_face_vertex_indices.push_back(i);
  }
  winding.face_triangle_offsets = {0, 1};
  winding.texcoords_0 = r::FloatChunked{};
  winding.tangents_interp = r::Interpolation::FaceVarying;
  winding.tangents_triangulated = true;
  for (float x : {1.f, 0.f, -1.f}) {
    winding.tangents.push_back(x);
    winding.tangents.push_back(0);
    winding.tangents.push_back(0);
    winding.tangents.push_back(1);
  }
  CHECK(r::BuildVertexIndices(&winding, options, &error));
  CHECK(winding.points[0] == 1 && winding.points[1] == 1 &&
        winding.tangents[0] == 1);
  for (double x :
       {-1000000.25, -2.75, -1.75, -0.25, 0.25, 1.25, 2.25, 1000000.25}) {
    CHECK(std::abs(lightusd::math::sin_pi(x) -
                   std::sin(lightusd::math::constants<double>::pi() * x)) <
          1e-9);
    CHECK(std::abs(lightusd::math::cos_pi(x) -
                   std::cos(lightusd::math::constants<double>::pi() * x)) <
          1e-9);
  }
  CHECK(lightusd::math::sin_pi(0.25) == lightusd::math::cos_pi(0.25));
  CHECK(lightusd::math::sin_pi(0.5) == 1 && lightusd::math::cos_pi(0.5) == 0);
  CHECK(lightusd::math::sin_pi(1e100) == 0 &&
        lightusd::math::cos_pi(1e100) == 1);
  CHECK(std::isnan(
      lightusd::math::sin_pi(std::numeric_limits<double>::infinity())));
  auto binding_only = fixture();
  binding_only.skin->influences_per_vertex = 0;
  binding_only.skin->joint_indices = r::UInt16Chunked{};
  binding_only.skin->joint_weights = r::FloatChunked{};
  CHECK(r::BuildVertexIndices(&binding_only, options, &error));
  CHECK(binding_only.skin && binding_only.skin->joint_weights.empty());
  r::RenderMesh empty;
  CHECK(r::BuildVertexIndices(&empty, options, &error));
  empty.face_vertex_counts.push_back(3);
  CHECK(!r::BuildVertexIndices(&empty, options, &error));
  CHECK(!r::BuildVertexIndices(nullptr, options, &error));
  return 0;
}
