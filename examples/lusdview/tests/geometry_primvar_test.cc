// SPDX-License-Identifier: Apache-2.0
#include "mesh_build.hh"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lightusd.hh"
#include "tydra/render-data.hh"

namespace {

namespace tydra = lightusd::tydra;

bool Near(float a, float b, float eps = 1.0e-6f) {
  return std::fabs(a - b) <= eps;
}

template <typename T, size_t N>
void SetAttr(tydra::VertexAttributeFormat fmt, tydra::VertexVariability var,
             const std::array<T, N>& values, tydra::VertexAttribute* out) {
  out->format = fmt;
  out->set_buffer(reinterpret_cast<const uint8_t*>(values.data()),
                  values.size() * sizeof(T));
  out->variability = var;
}

bool CheckPurpose(const lusdview::DrawScene& draw, size_t i,
                  const char* expected) {
  if (i >= draw.meshes.size()) {
    std::fprintf(stderr, "missing purpose test mesh %zu\n", i);
    return false;
  }
  if (draw.meshes[i].purpose != expected) {
    std::fprintf(stderr, "mesh %zu purpose: expected %s, got %s\n", i,
                 expected, draw.meshes[i].purpose.c_str());
    return false;
  }
  return true;
}

bool TestPurposeMetadata() {
  const char kUSDA[] = R"(#usda 1.0

def Xform "Root" {
  uniform token purpose = "proxy"

  def Mesh "InheritedProxy" {
  }

  def Xform "RenderScope" {
    uniform token purpose = "render"

    def Mesh "InheritedRender" {
    }
  }

  def Mesh "GuideMesh" {
    uniform token purpose = "guide"
  }
}
)";

  lightusd::legacy::Stage stage;
  std::string warn;
  std::string err;
  if (!lightusd::legacy::LoadUSDAFromMemory(
          reinterpret_cast<const uint8_t*>(kUSDA), std::strlen(kUSDA), "",
          &stage, &warn, &err)) {
    std::fprintf(stderr, "LoadUSDAFromMemory failed: %s\n", err.c_str());
    return false;
  }

  lusdview::DrawScene draw;
  draw.meshes.resize(4);
  draw.meshes[0].absPath = "/Root/InheritedProxy";
  draw.meshes[1].absPath = "/Root/RenderScope/InheritedRender";
  draw.meshes[2].absPath = "/Root/GuideMesh";
  draw.meshes[3].absPath.clear();
  lusdview::ApplyMeshPurposes(stage, &draw);

  if (!CheckPurpose(draw, 0, "proxy") ||
      !CheckPurpose(draw, 1, "render") ||
      !CheckPurpose(draw, 2, "guide") ||
      !CheckPurpose(draw, 3, "default")) {
    return false;
  }

  if (lusdview::PurposeId(draw.meshes[0].purpose) != 2 ||
      lusdview::PurposeId(draw.meshes[1].purpose) != 1 ||
      lusdview::PurposeId(draw.meshes[2].purpose) != 3 ||
      lusdview::PurposeId(draw.meshes[3].purpose) != 0 ||
      lusdview::PurposeId("unknown") != 0) {
    std::fprintf(stderr, "purpose id mapping changed\n");
    return false;
  }

  return true;
}

bool TestInstanceOpacityPacking() {
  tydra::RenderScene scene;

  tydra::RenderMesh mesh;
  mesh.prim_name = "Prototype";
  mesh.abs_path = "/Prototype";
  mesh.is_single_indexable = true;
  mesh.points = {{0.0f, 0.0f, 0.0f},
                 {1.0f, 0.0f, 0.0f},
                 {0.0f, 1.0f, 0.0f}};
  mesh.usdFaceVertexCounts = {3};
  mesh.usdFaceVertexIndices = {0, 1, 2};
  scene.meshes.push_back(std::move(mesh));

  for (int i = 0; i < 3; ++i) {
    tydra::RenderInstance inst;
    inst.mesh_id = 0;
    inst.visible = true;
    inst.global_matrix = lightusd::legacy::value::matrix4d::identity();
    inst.global_matrix.m[3][0] = static_cast<double>(i) * 2.0;
    if (i == 1) {
      inst.has_display_color = true;
      inst.display_color = {{0.25f, 0.5f, 0.75f}};
      inst.has_display_opacity = true;
      inst.display_opacity = 0.35f;
    } else if (i == 2) {
      inst.has_display_opacity = true;
      inst.display_opacity = 1.4f;
    }
    scene.instances.push_back(inst);
  }

  lusdview::DrawScene draw;
  lusdview::BuildDrawScene(scene, &draw);
  if (draw.meshes.size() != 1) {
    std::fprintf(stderr, "expected one instanced draw mesh, got %zu\n",
                 draw.meshes.size());
    return false;
  }

  const lusdview::DrawMeshCPU& dm = draw.meshes[0];
  if (dm.instanceCount() != 3 || dm.instanceXforms.size() != 36) {
    std::fprintf(stderr, "instance transforms were not packed\n");
    return false;
  }
  if (dm.instanceColors.size() != 9 ||
      !Near(dm.instanceColors[0], dm.flatColor[0]) ||
      !Near(dm.instanceColors[3], 0.25f) ||
      !Near(dm.instanceColors[4], 0.5f) ||
      !Near(dm.instanceColors[5], 0.75f) ||
      !Near(dm.instanceColors[6], dm.flatColor[0])) {
    std::fprintf(stderr, "instance displayColor padding changed\n");
    return false;
  }
  if (dm.instanceOpacities.size() != 3 ||
      !Near(dm.instanceOpacities[0], dm.flatOpacity) ||
      !Near(dm.instanceOpacities[1], 0.35f) ||
      !Near(dm.instanceOpacities[2], 1.0f)) {
    std::fprintf(stderr, "instance displayOpacity packing changed\n");
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestPurposeMetadata()) {
    return 1;
  }
  if (!TestInstanceOpacityPacking()) {
    return 1;
  }

  tydra::RenderScene scene;

  tydra::RenderMesh mesh;
  mesh.prim_name = "FaceVaryingPrimvars";
  mesh.abs_path = "/FaceVaryingPrimvars";
  mesh.is_single_indexable = false;
  mesh.points = {{-1.0f, -1.0f, 0.0f},
                 {1.0f, -1.0f, 0.0f},
                 {1.0f, 1.0f, 0.0f},
                 {-1.0f, 1.0f, 0.0f}};
  mesh.usdFaceVertexCounts = {3, 3};
  mesh.usdFaceVertexIndices = {0, 1, 2, 0, 2, 3};

  const std::array<tydra::vec3, 6> tangents = {
      tydra::vec3{1.0f, 0.0f, 0.0f}, tydra::vec3{1.0f, 0.0f, 0.0f},
      tydra::vec3{1.0f, 0.0f, 0.0f}, tydra::vec3{0.0f, 1.0f, 0.0f},
      tydra::vec3{0.0f, 1.0f, 0.0f}, tydra::vec3{0.0f, 1.0f, 0.0f}};
  const std::array<tydra::vec3, 6> binormals = {
      tydra::vec3{0.0f, 1.0f, 0.0f}, tydra::vec3{0.0f, 1.0f, 0.0f},
      tydra::vec3{0.0f, 1.0f, 0.0f}, tydra::vec3{-1.0f, 0.0f, 0.0f},
      tydra::vec3{-1.0f, 0.0f, 0.0f}, tydra::vec3{-1.0f, 0.0f, 0.0f}};
  const std::array<lightusd::legacy::value::texcoord2f, 6> uv1 = {
      lightusd::legacy::value::texcoord2f{0.1f, 0.2f},
      lightusd::legacy::value::texcoord2f{0.8f, 0.2f},
      lightusd::legacy::value::texcoord2f{0.8f, 0.9f},
      lightusd::legacy::value::texcoord2f{0.1f, 0.2f},
      lightusd::legacy::value::texcoord2f{0.8f, 0.9f},
      lightusd::legacy::value::texcoord2f{0.1f, 0.9f}};
  const std::array<tydra::vec3, 6> display = {
      tydra::vec3{1.0f, 0.0f, 0.0f}, tydra::vec3{0.0f, 1.0f, 0.0f},
      tydra::vec3{0.0f, 0.0f, 1.0f}, tydra::vec3{1.0f, 1.0f, 0.0f},
      tydra::vec3{0.0f, 1.0f, 1.0f}, tydra::vec3{1.0f, 0.0f, 1.0f}};

  SetAttr(tydra::VertexAttributeFormat::Vec3,
          tydra::VertexVariability::FaceVarying, tangents, &mesh.tangents);
  SetAttr(tydra::VertexAttributeFormat::Vec3,
          tydra::VertexVariability::FaceVarying, binormals, &mesh.binormals);
  SetAttr(tydra::VertexAttributeFormat::Vec2,
          tydra::VertexVariability::FaceVarying, uv1, &mesh.texcoords[1]);
  SetAttr(tydra::VertexAttributeFormat::Vec3,
          tydra::VertexVariability::FaceVarying, display, &mesh.vertex_colors);

  scene.meshes.push_back(std::move(mesh));

  tydra::RenderMesh uniformMesh;
  uniformMesh.prim_name = "UniformDisplayColor";
  uniformMesh.abs_path = "/UniformDisplayColor";
  uniformMesh.is_single_indexable = true;
  uniformMesh.points = {{-1.0f, -1.0f, 0.0f},
                        {1.0f, -1.0f, 0.0f},
                        {1.0f, 1.0f, 0.0f},
                        {-1.0f, 1.0f, 0.0f}};
  uniformMesh.usdFaceVertexCounts = {3, 3};
  uniformMesh.usdFaceVertexIndices = {0, 1, 2, 0, 2, 3};
  const std::array<tydra::vec3, 2> uniformDisplay = {
      tydra::vec3{0.25f, 0.5f, 0.75f},
      tydra::vec3{0.9f, 0.1f, 0.2f}};
  SetAttr(tydra::VertexAttributeFormat::Vec3,
          tydra::VertexVariability::Uniform, uniformDisplay,
          &uniformMesh.vertex_colors);
  scene.meshes.push_back(std::move(uniformMesh));

  tydra::RenderMesh numericMesh;
  numericMesh.prim_name = "NumericFormats";
  numericMesh.abs_path = "/NumericFormats";
  numericMesh.is_single_indexable = true;
  numericMesh.points = {{-1.0f, -1.0f, 0.0f},
                        {1.0f, -1.0f, 0.0f},
                        {1.0f, 1.0f, 0.0f},
                        {-1.0f, 1.0f, 0.0f}};
  numericMesh.usdFaceVertexCounts = {3, 3};
  numericMesh.usdFaceVertexIndices = {0, 1, 2, 0, 2, 3};
  const std::array<uint16_t, 8> halfUv = {
      0x0000u, 0x0000u, 0x3c00u, 0x0000u,
      0x3c00u, 0x3c00u, 0x0000u, 0x3c00u};
  const std::array<double, 12> doubleDisplay = {
      0.10, 0.20, 0.30, 0.40, 0.50, 0.60,
      0.70, 0.80, 0.90, 0.25, 0.50, 0.75};
  const std::array<uint16_t, 12> ushortTangents = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  SetAttr(tydra::VertexAttributeFormat::Half2,
          tydra::VertexVariability::Vertex, halfUv, &numericMesh.texcoords[0]);
  SetAttr(tydra::VertexAttributeFormat::Dvec3,
          tydra::VertexVariability::Vertex, doubleDisplay,
          &numericMesh.vertex_colors);
  SetAttr(tydra::VertexAttributeFormat::Ushort3,
          tydra::VertexVariability::Vertex, ushortTangents,
          &numericMesh.tangents);
  scene.meshes.push_back(std::move(numericMesh));

  lusdview::DrawScene draw;
  lusdview::BuildDrawScene(scene, &draw);
  if (draw.meshes.size() != 3) {
    std::fprintf(stderr, "expected three draw meshes, got %zu\n", draw.meshes.size());
    return 1;
  }

  const lusdview::DrawMeshCPU& dm = draw.meshes[0];
  if (dm.vertices.size() != 6 || dm.indices.size() != 6) {
    std::fprintf(stderr, "facevarying mesh was not expanded to six draw vertices\n");
    return 1;
  }
  if (dm.tangents.size() != dm.vertices.size() * 3 ||
      dm.binormals.size() != dm.vertices.size() * 3) {
    std::fprintf(stderr, "tangent frame primvars were not packed\n");
    return 1;
  }
  if (dm.uv1.size() != dm.vertices.size() * 2) {
    std::fprintf(stderr, "secondary texcoord primvar was not packed\n");
    return 1;
  }
  if (dm.vertexColors.size() != dm.vertices.size() * 3) {
    std::fprintf(stderr, "displayColor primvar was not packed\n");
    return 1;
  }

  if (!Near(dm.tangents[0], 1.0f) || !Near(dm.tangents[9], 0.0f) ||
      !Near(dm.tangents[10], 1.0f) || !Near(dm.binormals[9], -1.0f) ||
      !Near(dm.binormals[10], 0.0f)) {
    std::fprintf(stderr, "facevarying tangent/binormal values changed\n");
    return 1;
  }
  if (!Near(dm.uv1[0], 0.1f) || !Near(dm.uv1[1], 0.8f) ||
      !Near(dm.uv1[10], 0.1f) || !Near(dm.uv1[11], 0.1f)) {
    std::fprintf(stderr, "secondary texcoord values or USD V flip changed\n");
    return 1;
  }
  if (!Near(dm.vertexColors[0], 1.0f) || !Near(dm.vertexColors[4], 1.0f) ||
      !Near(dm.vertexColors[17], 1.0f)) {
    std::fprintf(stderr, "facevarying displayColor values changed\n");
    return 1;
  }

  const lusdview::DrawMeshCPU& uniformDm = draw.meshes[1];
  if (uniformDm.vertices.size() != 6 || uniformDm.indices.size() != 6) {
    std::fprintf(stderr, "uniform displayColor mesh was not expanded per face\n");
    return 1;
  }
  if (uniformDm.vertexColors.size() != uniformDm.vertices.size() * 3) {
    std::fprintf(stderr, "uniform displayColor primvar was not packed\n");
    return 1;
  }
  for (size_t i = 0; i < 3; ++i) {
    const size_t o = i * 3;
    if (!Near(uniformDm.vertexColors[o + 0], 0.25f) ||
        !Near(uniformDm.vertexColors[o + 1], 0.5f) ||
        !Near(uniformDm.vertexColors[o + 2], 0.75f)) {
      std::fprintf(stderr, "first uniform displayColor face changed\n");
      return 1;
    }
  }
  for (size_t i = 3; i < 6; ++i) {
    const size_t o = i * 3;
    if (!Near(uniformDm.vertexColors[o + 0], 0.9f) ||
        !Near(uniformDm.vertexColors[o + 1], 0.1f) ||
        !Near(uniformDm.vertexColors[o + 2], 0.2f)) {
      std::fprintf(stderr, "second uniform displayColor face changed\n");
      return 1;
    }
  }

  const lusdview::DrawMeshCPU& numericDm = draw.meshes[2];
  if (numericDm.vertices.size() != 4 || numericDm.indices.size() != 6) {
    std::fprintf(stderr, "numeric-format mesh did not stay indexed\n");
    return 1;
  }
  if (!Near(numericDm.vertices[0].u, 0.0f) ||
      !Near(numericDm.vertices[0].v, 1.0f) ||
      !Near(numericDm.vertices[2].u, 1.0f) ||
      !Near(numericDm.vertices[2].v, 0.0f)) {
    std::fprintf(stderr, "Half2 texcoord decoding or V flip changed\n");
    return 1;
  }
  if (numericDm.vertexColors.size() != 12 ||
      !Near(numericDm.vertexColors[0], 0.10f) ||
      !Near(numericDm.vertexColors[4], 0.50f) ||
      !Near(numericDm.vertexColors[11], 0.75f)) {
    std::fprintf(stderr, "Dvec3 displayColor decoding changed\n");
    return 1;
  }
  if (numericDm.tangents.size() != 12 ||
      !Near(numericDm.tangents[0], 1.0f) ||
      !Near(numericDm.tangents[4], 5.0f) ||
      !Near(numericDm.tangents[11], 12.0f)) {
    std::fprintf(stderr, "Ushort3 tangent decoding changed\n");
    return 1;
  }

  return 0;
}
