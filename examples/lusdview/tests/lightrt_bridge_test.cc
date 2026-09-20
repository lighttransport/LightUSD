// SPDX-License-Identifier: Apache-2.0
#include "lightrt_mtlx_bridge.hh"
#include "rt_scene_build.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool Near(float a, float b, float eps = 1.0e-6f) {
  return std::fabs(a - b) <= eps;
}

void Identity(float m[16]) {
  for (int i = 0; i < 16; ++i) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

lusdview::DrawMaterialParamCPU FloatParam(const char* name, float v) {
  lusdview::DrawMaterialParamCPU p;
  p.shader = "OpenPBRSurface";
  p.name = name;
  p.type = lusdview::DrawMaterialParamType::Float;
  p.value[0] = v;
  return p;
}

lusdview::DrawMaterialParamCPU Vec3Param(const char* name, float x, float y,
                                         float z) {
  lusdview::DrawMaterialParamCPU p;
  p.shader = "OpenPBRSurface";
  p.name = name;
  p.type = lusdview::DrawMaterialParamType::Vec3;
  p.value[0] = x;
  p.value[1] = y;
  p.value[2] = z;
  p.value[3] = 1.0f;
  return p;
}

std::string MaterialTextureDir() {
  namespace fs = std::filesystem;
  const fs::path cwd = fs::current_path();
  const fs::path candidates[] = {
      cwd / "models" / "textures",
      cwd / ".." / "models" / "textures",
      cwd / ".." / ".." / "models" / "textures",
      cwd / ".." / ".." / ".." / "models" / "textures"};
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(candidate / "checkerboard.png", ec) && !ec) {
      return fs::absolute(candidate, ec).lexically_normal().string();
    }
  }
  return {};
}

}  // namespace

int main() {
  lusdview::DrawScene scene;

  lusdview::DrawMaterialCPU mat;
  mat.name = "OpenPBRBridge";
  mat.hasOpenPBRSurface = true;
  mat.params.push_back(FloatParam("base_weight", 0.75f));
  mat.params.push_back(Vec3Param("base_color", 0.2f, 0.4f, 0.6f));
  mat.params.push_back(FloatParam("base_metalness", 0.5f));
  mat.params.push_back(FloatParam("specular_weight", 0.25f));
  mat.params.push_back(FloatParam("specular_roughness", 0.35f));
  mat.params.push_back(FloatParam("specular_ior", 1.6f));
  mat.params.push_back(FloatParam("coat_weight", 0.3f));
  mat.params.push_back(Vec3Param("coat_color", 0.7f, 0.8f, 0.9f));
  mat.params.push_back(FloatParam("coat_roughness", 0.2f));
  mat.params.push_back(FloatParam("coat_ior", 1.4f));
  mat.params.push_back(FloatParam("emission_luminance", 2.0f));
  mat.params.push_back(Vec3Param("emission_color", 0.1f, 0.2f, 0.3f));
  mat.params.push_back(FloatParam("transmission_weight", 0.8f));
  mat.params.push_back(Vec3Param("transmission_color", 0.25f, 0.5f, 1.0f));
  mat.params.push_back(FloatParam("geometry_thin_walled", 1.0f));
  mat.params.push_back(FloatParam("subsurface_weight", 0.22f));
  mat.params.push_back(Vec3Param("subsurface_color", 0.6f, 0.7f, 0.8f));
  mat.params.push_back(FloatParam("subsurface_anisotropy", 0.2f));
  mat.params.push_back(FloatParam("subsurface_scatter_anisotropy", -0.3f));
  mat.params.push_back(
      Vec3Param("subsurface_radius_scale", 0.7f, 0.5f, 0.3f));
  mat.params.push_back(FloatParam("thin_film_weight", 1.0f));
  mat.params.push_back(FloatParam("thin_film_thickness", 450.0f));
  mat.params.push_back(FloatParam("thin_film_ior", 1.4f));
  mat.params.push_back(FloatParam("opacity", 0.65f));
  lusdview::BakeRealtimePbrMaterial(&mat);
  if (!Near(mat.lightRtOpenPBR.thinFilmWeight, 1.0f) ||
      !Near(mat.lightRtOpenPBR.thinFilmThicknessNm, 450.0f) ||
      !Near(mat.lightRtOpenPBR.thinFilmIor, 1.4f) ||
      !Near(mat.lightRtOpenPBR.subsurfaceAnisotropy, 0.2f) ||
      !Near(mat.lightRtOpenPBR.subsurfaceScatterAnisotropy, -0.3f)) {
    std::fprintf(stderr, "OpenPBR thin-film units were not preserved\n");
    return 1;
  }
  mat.occlusion = 0.65f;
  mat.baseColorSample.uv = {1.0f, 0.1f, 0.2f, 0.9f, 0.3f, 0.4f};
  mat.metallicSample.uv = {0.5f, 0.0f, 0.0f, 0.5f, 0.1f, 0.2f};
  mat.roughnessSample.uv = mat.metallicSample.uv;
  mat.normalSample.uv = {1.0f, 0.0f, 0.0f, 1.0f, 0.7f, 0.8f};
  mat.emissiveSample.uv = {0.25f, 0.0f, 0.0f, 0.25f, 0.2f, 0.3f};
  mat.displacementUv = {2.0f, 0.0f, 0.0f, 2.0f, -0.5f, 0.5f};
  mat.displacementTex = 11;
  mat.displacementSample.isUdim = true;
  mat.baseColorSample.scale[0] = 0.25f;
  mat.baseColorSample.bias[0] = 0.10f;
  mat.normalSample.scale[1] = 2.0f;
  mat.normalSample.bias[1] = -1.0f;
  mat.emissiveSample.scale[2] = 3.0f;
  mat.emissiveSample.bias[2] = 0.30f;
  mat.metallicChannel = 0;
  mat.roughnessChannel = 3;
  mat.metallicTexScale = 0.6f;
  mat.metallicTexBias = 0.05f;
  mat.roughnessTexScale = 0.7f;
  mat.roughnessTexBias = 0.15f;
  mat.displacementTexScale = 1.25f;
  mat.displacementTexBias = -0.25f;
  mat.opacityTex = 7;
  mat.opacityChannel = 2;
  mat.opacityTexScale = 0.8f;
  mat.opacityTexBias = 0.1f;
  mat.baseColorSample.uvSet = 1;
  mat.metallicSample.uvSet = 1;
  mat.normalSample.uvSet = 1;
  mat.emissiveSample.uvSet = 1;
  mat.opacitySample.uvSet = 1;
  mat.opacitySample.uv.m00 = 1.5f;
  mat.opacitySample.uv.tx = 0.25f;
  mat.roughnessSample.uvSet = 1;
  mat.occlusionTex = 9;
  mat.occlusionChannel = 1;
  mat.occlusionTexScale = 0.75f;
  mat.occlusionTexBias = 0.125f;
  mat.occlusionSample.uvSet = 1;
  mat.occlusionSample.uv.m00 = 0.5f;
  mat.occlusionSample.uv.ty = 0.375f;
  mat.specularColorSample.scale[0] = 0.51f;
  mat.specularColorSample.bias[2] = 0.12f;
  mat.specularColorSample.uv.m00 = 0.91f;
  mat.specularColorSample.uv.ty = 0.19f;
  mat.specularColorSample.uvSet = 1;
  mat.specularColorSample.isPtex = true;
  mat.specularColorSample.ptexRectTexelOffset = 101;
  mat.specularColorSample.ptexFaceCount = 7;
  mat.useSpecularWorkflow = true;
  mat.coatWeightSample.scale[1] = 0.61f;
  mat.coatWeightSample.bias[1] = 0.13f;
  mat.coatWeightSample.uvSet = 1;
  mat.coatWeightSample.isPtex = true;
  mat.coatWeightSample.ptexRectTexelOffset = 202;
  mat.coatWeightSample.ptexFaceCount = 8;
  mat.coatColorSample.scale[2] = 0.71f;
  mat.coatColorSample.bias[0] = 0.14f;
  mat.coatColorSample.uvSet = 1;
  mat.coatColorSample.isPtex = true;
  mat.coatColorSample.ptexRectTexelOffset = 303;
  mat.coatColorSample.ptexFaceCount = 9;
  mat.coatRoughnessSample.scale[3] = 0.81f;
  mat.coatRoughnessSample.bias[3] = 0.15f;
  mat.coatRoughnessSample.uvSet = 1;
  mat.coatRoughnessSample.isPtex = true;
  mat.coatRoughnessSample.ptexRectTexelOffset = 404;
  mat.coatRoughnessSample.ptexFaceCount = 10;
  mat.coatNormalSample.uv.m00 = 0.83f;
  mat.coatNormalSample.uv.ty = 0.23f;
  mat.coatNormalSample.uvSet = 1;
  mat.coatNormalSample.scale[0] = 1.7f;
  mat.coatNormalSample.bias[1] = -0.4f;
  mat.alphaMode = static_cast<int>(lusdview::AlphaMode::Mask);
  mat.alphaCutoff = 0.42f;
  std::vector<float> directPack(lusdview::kLightRtOpenPBRFloats, -1.0f);
  lusdview::PackLightRtOpenPBR(mat, directPack.data());
  std::vector<float> directTexPack(lusdview::kRtMaterialTextureParamFloats,
                                   -1.0f);
  lusdview::PackRtMaterialTextureParams(mat, directTexPack.data());
  std::vector<float> directRasterTexPack(
      lusdview::kRasterMaterialTextureParamFloats, -1.0f);
  lusdview::PackRasterMaterialTextureParams(mat, directRasterTexPack.data());
  if (!Near(directRasterTexPack[67 * 4 + 0], 0.8f) ||
      !Near(directRasterTexPack[68 * 4 + 0], 0.25f) ||
      !Near(directRasterTexPack[68 * 4 + 1], 0.5f) ||
      !Near(directRasterTexPack[68 * 4 + 2], 1.0f) ||
      !Near(directRasterTexPack[67 * 4 + 3], 1.0f) ||
      !Near(directRasterTexPack[72 * 4 + 0], 0.22f) ||
      !Near(directRasterTexPack[72 * 4 + 1], 1.0f) ||
      !Near(directRasterTexPack[72 * 4 + 2],
            0.2126f * 0.7f + 0.7152f * 0.5f + 0.0722f * 0.3f)) {
    std::fprintf(stderr, "raster transmission controls were not packed\n");
    return 1;
  }
  if (!mat.lightRtOpenPBR.geometryThinWalled || !Near(directPack[71], 1.0f)) {
    std::fprintf(stderr, "OpenPBR geometry_thin_walled was not baked\n");
    return 1;
  }
  scene.materials.push_back(mat);

  lusdview::DrawMeshCPU mesh;
  mesh.name = "tri";
  Identity(mesh.world);
  Identity(mesh.skinGeomBind);
  mesh.vertices.resize(3);
  mesh.vertices[0].px = 0.0f; mesh.vertices[0].py = 0.0f; mesh.vertices[0].pz = 0.0f;
  mesh.vertices[1].px = 1.0f; mesh.vertices[1].py = 0.0f; mesh.vertices[1].pz = 0.0f;
  mesh.vertices[2].px = 0.0f; mesh.vertices[2].py = 1.0f; mesh.vertices[2].pz = 0.0f;
  for (lusdview::DrawVertex& v : mesh.vertices) {
    v.nz = 1.0f;
  }
  mesh.indices = {0, 1, 2};
  mesh.vertexAlpha = {0.25f, 0.5f, 0.75f};
  lusdview::DrawSubmesh sub;
  sub.indexOffset = 0;
  sub.indexCount = 3;
  sub.materialId = 0;
  mesh.submeshes.push_back(sub);
  scene.meshes.push_back(mesh);

  lusdview::HostScene host;
  std::string err;
  if (!lusdview::BuildHostScene(scene, 0, 0, 0.0f, &host, &err)) {
    std::fprintf(stderr, "BuildHostScene failed: %s\n", err.c_str());
    return 1;
  }
  if (host.prototypes.size() != 1 ||
      host.prototypes[0].triOffset != 0 ||
      host.prototypes[0].triCount != 1 ||
      host.instancePrototype.size() != host.instances.size() ||
      host.instancePrototype.empty() || host.instancePrototype[0] != 0) {
    std::fprintf(stderr, "hardware RT prototype/instance mapping failed\n");
    return 1;
  }

  // Ordinary meshes are world-baked into RT prototype vertices and therefore
  // keep an identity instance transform. Refit must apply a changed world once,
  // refresh BLAS/TLAS bounds, and avoid the historical double transform.
  {
    lusdview::HostScene refitHost;
    lusdview::RefitMap refitMap;
    std::string refitError;
    if (!lusdview::BuildHostScene(scene, 0, 0, 0.0f, &refitHost,
                                  &refitError, nullptr, &refitMap)) {
      std::fprintf(stderr, "refit scene build failed: %s\n",
                   refitError.c_str());
      return 1;
    }
    scene.meshes[0].world[12] = 3.0f;
    if (!lusdview::RefitHostScene(scene, refitMap, &refitHost, &refitError) ||
        refitHost.tris.empty() || !Near(refitHost.tris[0], 3.0f) ||
        refitHost.instances.empty() ||
        !Near(refitHost.instances[0].o2w[3], 0.0f) ||
        refitHost.tlas.empty() || !Near(refitHost.tlas[0].bmin[0], 3.0f)) {
      std::fprintf(stderr, "animated-world RT refit failed: %s\n",
                   refitError.c_str());
      return 1;
    }
    scene.meshes[0].world[12] = 0.0f;
  }

  // Non-uniform object-to-world scale must use the inverse-transpose normal
  // transform in the flattened CPU/RT scene, matching the Vulkan path.
  {
    lusdview::DrawScene normalScene;
    normalScene.materials.push_back(mat);
    lusdview::DrawMeshCPU normalMesh = mesh;
    normalMesh.world[0] = 2.0f;
    normalMesh.world[5] = 3.0f;
    normalMesh.world[10] = 1.0f;
    for (lusdview::DrawVertex& vertex : normalMesh.vertices) {
      vertex.nx = 1.0f;
      vertex.ny = 1.0f;
      vertex.nz = 0.0f;
    }
    normalScene.meshes.push_back(std::move(normalMesh));
    lusdview::HostScene normalHost;
    std::string normalErr;
    if (!lusdview::BuildHostScene(normalScene, 0, 0, 0.0f, &normalHost,
                                  &normalErr) || normalHost.nrms.size() < 3 ||
        !Near(normalHost.nrms[0], 0.5f) ||
        !Near(normalHost.nrms[1], 1.0f / 3.0f) ||
        !Near(normalHost.nrms[2], 0.0f)) {
      std::fprintf(stderr, "non-uniform normal inverse-transpose failed: %s\n",
                   normalErr.c_str());
      return 1;
    }
  }

  // CUDA and HIP upload HostScene::mat verbatim. Keep a focused assertion that
  // two GeomSubset-style EBO ranges survive BVH leaf reordering as two distinct
  // per-triangle material ids.
  {
    lusdview::DrawScene subsetScene;
    subsetScene.materials.resize(2);
    lusdview::DrawMeshCPU subsetMesh = mesh;
    subsetMesh.vertices.resize(6);
    for (size_t i = 0; i < 3; ++i) {
      subsetMesh.vertices[i + 3] = subsetMesh.vertices[i];
      subsetMesh.vertices[i + 3].px += 2.0f;
    }
    subsetMesh.indices = {0, 1, 2, 3, 4, 5};
    subsetMesh.vertexAlpha.clear();
    subsetMesh.submeshes.clear();
    lusdview::DrawSubmesh left;
    left.indexCount = 3;
    left.materialId = 0;
    subsetMesh.submeshes.push_back(left);
    lusdview::DrawSubmesh right;
    right.indexOffset = 3;
    right.indexCount = 3;
    right.materialId = 1;
    subsetMesh.submeshes.push_back(right);
    subsetScene.meshes.push_back(std::move(subsetMesh));

    lusdview::HostScene subsetHost;
    std::string subsetErr;
    if (!lusdview::BuildHostScene(subsetScene, 0, 0, 0.0f, &subsetHost,
                                  &subsetErr)) {
      std::fprintf(stderr, "GeomSubset HostScene build failed: %s\n",
                   subsetErr.c_str());
      return 1;
    }
    std::sort(subsetHost.mat.begin(), subsetHost.mat.end());
    if (subsetHost.mat != std::vector<int>({0, 1})) {
      std::fprintf(stderr,
                   "GeomSubset material ids were not preserved per triangle\n");
      return 1;
    }
  }
  if (host.matLightRt.size() < lusdview::kLightRtOpenPBRFloats) {
    std::fprintf(stderr, "matLightRt was not packed\n");
    return 1;
  }

  const float* p = host.matLightRt.data();
  for (int i = 0; i < lusdview::kLightRtOpenPBRFloats; ++i) {
    if (!Near(p[i], directPack[static_cast<size_t>(i)])) {
      std::fprintf(stderr, "HostScene pack drift at lane %d\n", i);
      return 1;
    }
  }
  if (!Near(p[0], 0.2f) || !Near(p[1], 0.4f) || !Near(p[2], 0.6f) ||
      !Near(p[3], 0.75f) || !Near(p[7], 0.25f) || !Near(p[27], 0.3f) ||
      !Near(p[35], 2.0f) || !Near(p[39], 0.65f) || !Near(p[40], 0.5f) ||
      !Near(p[42], 0.35f) || !Near(p[43], 1.6f) || !Near(p[44], 0.2f) ||
      !Near(p[51], 1.0f) || !Near(p[54], 1.0f) || !Near(p[55], 0.42f)) {
    std::fprintf(stderr, "unexpected LightRT/OpenPBR packing\n");
    return 1;
  }
  if (host.matTexParam.size() < lusdview::kRtMaterialTextureParamFloats) {
    std::fprintf(stderr, "matTexParam was not packed\n");
    return 1;
  }
  for (int i = 0; i < lusdview::kRtMaterialTextureParamFloats; ++i) {
    if (!Near(host.matTexParam[static_cast<size_t>(i)],
              directTexPack[static_cast<size_t>(i)])) {
      std::fprintf(stderr, "HostScene texture-param pack drift at lane %d\n", i);
      return 1;
    }
  }
  if (!Near(directTexPack[0], 1.0f) || !Near(directTexPack[2], 0.3f) ||
      !Near(directTexPack[5], 0.4f) || !Near(directTexPack[44], 1.0f) ||
      !Near(directTexPack[45], 2.0f) || !Near(directTexPack[49], -1.0f) ||
      !Near(directTexPack[60], 0.0f) || !Near(directTexPack[63], 3.0f) ||
      !Near(directTexPack[61], 0.6f) || !Near(directTexPack[65], 0.15f) ||
      !Near(directTexPack[66], 2.0f) || !Near(directTexPack[68], 0.1f) ||
      !Near(directTexPack[101], 0.61f) || !Near(directTexPack[105], 0.13f) ||
      !Near(directTexPack[110], 0.71f) || !Near(directTexPack[112], 0.14f) ||
      !Near(directTexPack[119], 0.81f) || !Near(directTexPack[123], 0.15f) ||
      !Near(directTexPack[124], 0.91f) || !Near(directTexPack[129], 0.19f) ||
      !Near(directTexPack[130], 1.0f) || !Near(directTexPack[131], 1.0f) ||
      !Near(directTexPack[132], 0.51f) || !Near(directTexPack[138], 0.12f) ||
      !Near(directTexPack[140], 0.83f) || !Near(directTexPack[145], 0.23f) ||
      !Near(directTexPack[146], 1.0f) || !Near(directTexPack[147], 1.7f) ||
      !Near(directTexPack[152], -0.4f)) {
    std::fprintf(stderr, "unexpected RT texture-param packing\n");
    return 1;
  }
  if (!Near(directRasterTexPack[63 * 4 + 0], 101.0f) ||
      !Near(directRasterTexPack[63 * 4 + 1], 7.0f) ||
      !Near(directRasterTexPack[64 * 4 + 0], 202.0f) ||
      !Near(directRasterTexPack[65 * 4 + 1], 9.0f) ||
      !Near(directRasterTexPack[66 * 4 + 0], 404.0f)) {
    std::fprintf(stderr, "advanced raster Ptex metadata was not packed\n");
    return 1;
  }
  if (!Near(directRasterTexPack[8 * 4 + 0], 2.0f) ||
      !Near(directRasterTexPack[8 * 4 + 6], 0.5f) ||
      !Near(directRasterTexPack[16 * 4 + 0], 0.0f) ||
      !Near(directRasterTexPack[16 * 4 + 1], 3.0f) ||
      // UDIM displacement is baked into CPU geometry before raster upload;
      // the vertex-stage scale/bias must therefore remain disabled.
      !Near(directRasterTexPack[17 * 4 + 2], 0.0f) ||
      !Near(directRasterTexPack[17 * 4 + 3], 0.0f) ||
      !Near(directRasterTexPack[18 * 4 + 0], 1.0f) ||
      !Near(directRasterTexPack[18 * 4 + 1], 1.0f) ||
      !Near(directRasterTexPack[18 * 4 + 2], 1.0f) ||
      !Near(directRasterTexPack[18 * 4 + 3], 1.0f) ||
      !Near(directRasterTexPack[20 * 4 + 0], 1.5f) ||
      !Near(directRasterTexPack[20 * 4 + 2], 0.25f) ||
      !Near(directRasterTexPack[22 * 4 + 0], 2.0f) ||
      !Near(directRasterTexPack[22 * 4 + 1], 0.8f) ||
      !Near(directRasterTexPack[22 * 4 + 2], 0.1f) ||
      !Near(directRasterTexPack[22 * 4 + 3], 1.0f) ||  // opacity
      !Near(directRasterTexPack[24 * 4 + 0], 7.0f) ||
      !Near(directRasterTexPack[27 * 4 + 0], 0.3f) ||
      !Near(directRasterTexPack[27 * 4 + 1], 0.2f) ||
      !Near(directRasterTexPack[27 * 4 + 2], 1.4f) ||
      !Near(directRasterTexPack[27 * 4 + 3], 0.65f) ||
      !Near(directRasterTexPack[28 * 4 + 0], 0.7f) ||
      !Near(directRasterTexPack[28 * 4 + 1], 0.8f) ||
      !Near(directRasterTexPack[28 * 4 + 2], 0.9f) ||
      !Near(directRasterTexPack[24 * 4 + 2], 9.0f) ||
      !Near(directRasterTexPack[25 * 4 + 3], 1.0f) ||  // roughness uvSet
      !Near(directRasterTexPack[29 * 4 + 0], 0.5f) ||
      !Near(directRasterTexPack[30 * 4 + 2], 0.375f) ||
      !Near(directRasterTexPack[31 * 4 + 0], 1.0f) ||
      !Near(directRasterTexPack[31 * 4 + 1], 0.75f) ||
      !Near(directRasterTexPack[31 * 4 + 2], 0.125f) ||
      !Near(directRasterTexPack[31 * 4 + 3], 1.0f) ||  // occlusion uvSet
      !Near(directRasterTexPack[40 * 4 + 2], 1.0f) ||  // coatWeight uvSet
      !Near(directRasterTexPack[40 * 4 + 3], 1.0f) ||  // coatRoughness uvSet
      !Near(directRasterTexPack[41 * 4 + 0], 1.0f) ||  // specularColor uvSet
      !Near(directRasterTexPack[41 * 4 + 1], 1.0f) ||  // coatColor uvSet
      !Near(directRasterTexPack[42 * 4 + 0], 0.51f) ||
      !Near(directRasterTexPack[43 * 4 + 2], 0.12f) ||
      !Near(directRasterTexPack[44 * 4 + 1], 0.61f) ||
      !Near(directRasterTexPack[45 * 4 + 1], 0.13f) ||
      !Near(directRasterTexPack[46 * 4 + 2], 0.71f) ||
      !Near(directRasterTexPack[47 * 4 + 0], 0.14f) ||
      !Near(directRasterTexPack[48 * 4 + 3], 0.81f) ||
      !Near(directRasterTexPack[49 * 4 + 3], 0.15f) ||
      !Near(directRasterTexPack[50 * 4 + 0], 0.83f) ||
      !Near(directRasterTexPack[51 * 4 + 2], 0.23f) ||
      !Near(directRasterTexPack[52 * 4 + 0], 1.7f) ||
      !Near(directRasterTexPack[53 * 4 + 1], -0.4f) ||
      !Near(directRasterTexPack[53 * 4 + 3], 1.0f) ||
      !Near(directRasterTexPack[55 * 4 + 1], 11.0f)) {
    std::fprintf(stderr, "unexpected raster texture-param packing\n");
    return 1;
  }
  if (host.cols.size() != 12 || !Near(host.cols[3], 0.25f) ||
      !Near(host.cols[7], 0.5f) || !Near(host.cols[11], 0.75f)) {
    std::fprintf(stderr, "displayOpacity RGBA packing changed\n");
    return 1;
  }

  // The shared CUDA/HIP host scene keeps a compact optional back-material
  // stream. Also lock the legacy convention that material index 0 may be a
  // real authored material; only the next loader's anonymous index-0 record is
  // suppressed for an unbound instanced prototype.
  lusdview::DrawScene sidedScene;
  lusdview::DrawMaterialCPU frontMat;
  frontMat.name = "AuthoredFrontAtZero";
  frontMat.baseColor[0] = 0.9f;
  frontMat.baseColor[1] = 0.1f;
  frontMat.baseColor[2] = 0.2f;
  lusdview::DrawMaterialCPU backMat;
  backMat.name = "AuthoredBack";
  backMat.baseColor[0] = 0.1f;
  backMat.baseColor[1] = 0.2f;
  backMat.baseColor[2] = 0.9f;
  sidedScene.materials = {frontMat, backMat};
  lusdview::DrawMeshCPU sidedMesh = mesh;
  sidedMesh.submeshes[0].backfaceMaterialId = 1;
  sidedMesh.instanceXforms = {1.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f};
  sidedScene.meshes.push_back(sidedMesh);
  lusdview::HostScene sidedHost;
  if (!lusdview::BuildHostScene(sidedScene, 0, 0, 0.0f, &sidedHost, &err) ||
      sidedHost.mat.size() != 1 || sidedHost.mat[0] != 0 ||
      sidedHost.backMat.size() != 1 || sidedHost.backMat[0] != 1 ||
      sidedHost.matBase.size() != 6 || !Near(sidedHost.matBase[0], 0.9f) ||
      !Near(sidedHost.matBase[5], 0.9f) || sidedHost.instances.size() != 1 ||
      !Near(sidedHost.instances[0].tint[0], 1.0f)) {
    std::fprintf(stderr, "front/back RT material packing changed: %s\n",
                 err.c_str());
    return 1;
  }

  lusdview::DrawScene placeholderScene;
  placeholderScene.materials.emplace_back();  // anonymous next-loader fallback
  lusdview::DrawMeshCPU placeholderMesh = sidedMesh;
  placeholderMesh.submeshes[0].backfaceMaterialId = -1;
  placeholderScene.meshes.push_back(std::move(placeholderMesh));
  lusdview::HostScene placeholderHost;
  if (!lusdview::BuildHostScene(placeholderScene, 0, 0, 0.0f,
                                &placeholderHost, &err) ||
      placeholderHost.mat.size() != 1 || placeholderHost.mat[0] != -1 ||
      !placeholderHost.backMat.empty() || placeholderHost.instances.size() != 1 ||
      !Near(placeholderHost.instances[0].tint[0], sidedMesh.flatColor[0])) {
    std::fprintf(stderr, "anonymous RT material fallback changed: %s\n",
                 err.c_str());
    return 1;
  }

  lusdview::DrawMaterialCPU defaultMat;
  std::vector<float> defaultPack(lusdview::kLightRtOpenPBRFloats, -1.0f);
  lusdview::PackLightRtOpenPBR(defaultMat, defaultPack.data());
  if (!Near(defaultPack[0], 0.8f) || !Near(defaultPack[3], 1.0f) ||
      !Near(defaultPack[39], 1.0f) || !Near(defaultPack[51], 0.0f) ||
      !Near(defaultPack[54], 0.0f) || !Near(defaultPack[55], 0.5f)) {
    std::fprintf(stderr, "unexpected default LightRT/OpenPBR packing\n");
    return 1;
  }
  std::vector<float> sharedDefaultPack(lusdview::kLightRtOpenPBRFloats, -2.0f);
  lightusd::tydra::RealtimePbrMaterial sharedDefault;
  lightusd::tydra::PackRealtimePbrMaterial(
      sharedDefault, false, 0.0f, 0.5f, sharedDefaultPack.data());
  for (int i = 0; i < lusdview::kLightRtOpenPBRFloats; ++i) {
    if (!Near(defaultPack[size_t(i)], sharedDefaultPack[size_t(i)])) {
      std::fprintf(stderr, "shared/default realtime-PBR pack mismatch at %d\n", i);
      return 1;
    }
  }
  lightusd::tydra::RealtimePbrMaterial extendedPbr;
  extendedPbr.specularAnisotropy = 0.25f;
  extendedPbr.specularRotation = 12.0f;
  extendedPbr.specularRoughnessAnisotropy = -0.15f;
  extendedPbr.transmissionDispersion = 0.4f;
  extendedPbr.transmissionDispersionAbbeNumber = 32.0f;
  extendedPbr.transmissionDispersionScale = 0.75f;
  extendedPbr.subsurfaceAnisotropy = 0.2f;
  extendedPbr.subsurfaceScatterAnisotropy = -0.3f;
  extendedPbr.coatAnisotropy = 0.1f;
  extendedPbr.coatRotation = 45.0f;
  extendedPbr.coatRoughnessAnisotropy = -0.2f;
  extendedPbr.coatDarkening = 0.6f;
  extendedPbr.geometryThinWalled = true;
  extendedPbr.volumeDensity = 2.0f;
  extendedPbr.volumeAlbedo[0] = 0.2f;
  extendedPbr.volumeAlbedo[1] = 0.3f;
  extendedPbr.volumeAlbedo[2] = 0.4f;
  extendedPbr.volumeEmission[0] = 0.5f;
  extendedPbr.volumeEmission[1] = 0.6f;
  extendedPbr.volumeEmission[2] = 0.7f;
  extendedPbr.volumeEmissionScale = 3.0f;
  std::vector<float> extendedPack(lusdview::kLightRtOpenPBRFloats, 0.0f);
  lightusd::tydra::PackRealtimePbrMaterial(
      extendedPbr, true, 0.0f, 0.5f, extendedPack.data());
  if (!Near(extendedPack[57], 0.25f) ||
      !Near(extendedPack[58], 12.0f) ||
      !Near(extendedPack[59], -0.15f) || !Near(extendedPack[60], 0.4f) ||
      !Near(extendedPack[61], 32.0f) || !Near(extendedPack[62], 0.75f) ||
      !Near(extendedPack[63], 0.2f) || !Near(extendedPack[64], -0.3f) ||
      !Near(extendedPack[65], 0.1f) || !Near(extendedPack[66], 45.0f) ||
      !Near(extendedPack[69], -0.2f) ||
      !Near(extendedPack[70], 0.6f) || !Near(extendedPack[71], 1.0f) ||
      !Near(extendedPack[72], 0.2f) ||
      !Near(extendedPack[73], 0.3f) || !Near(extendedPack[74], 0.4f) ||
      !Near(extendedPack[75], 2.0f) || !Near(extendedPack[76], 0.5f) ||
      !Near(extendedPack[77], 0.6f) || !Near(extendedPack[78], 0.7f) ||
      !Near(extendedPack[79], 3.0f)) {
    std::fprintf(stderr, "extended OpenPBR controls were not packed\n");
    return 1;
  }

  lusdview::DrawMaterialCPU neutralNormalMat;
  neutralNormalMat.hasOpenPBRSurface = true;
  neutralNormalMat.params.push_back(Vec3Param("normal", 0.0f, 0.0f, 1.0f));
  neutralNormalMat.params.push_back(Vec3Param("coat_normal", 0.0f, 0.0f, 1.0f));
  lusdview::BakeLightRtOpenPBR(&neutralNormalMat);
  if (neutralNormalMat.lightRtOpenPBR.hasNormalInput) {
    std::fprintf(stderr, "neutral OpenPBR normals were marked active\n");
    return 1;
  }

  lusdview::DrawMaterialCPU customCoatNormalMat;
  customCoatNormalMat.hasOpenPBRSurface = true;
  customCoatNormalMat.params.push_back(Vec3Param("normal", 0.0f, 0.0f, 1.0f));
  customCoatNormalMat.params.push_back(
      Vec3Param("coat_normal", 0.25f, 0.0f, 0.97f));
  lusdview::BakeLightRtOpenPBR(&customCoatNormalMat);
  if (!customCoatNormalMat.lightRtOpenPBR.hasNormalInput) {
    std::fprintf(stderr, "custom OpenPBR coat normal was not marked active\n");
    return 1;
  }

  const char* graphXml =
      "<materialx version=\"1.38\">"
      "  <multiply name=\"Tint\" type=\"color3\">"
      "    <input name=\"in1\" type=\"color3\" value=\"0.2, 0.4, 0.6\"/>"
      "    <input name=\"in2\" type=\"color3\" value=\"2.0, 0.5, 0.25\"/>"
      "  </multiply>"
      "  <open_pbr_surface name=\"GraphSurface\" type=\"surfaceshader\">"
      "    <input name=\"base_color\" type=\"color3\" nodename=\"Tint\"/>"
      "    <input name=\"base_diffuse_roughness\" type=\"float\" value=\"0.12\"/>"
      "    <input name=\"base_metalness\" type=\"float\" value=\"0.45\"/>"
      "    <input name=\"base_roughness\" type=\"float\" value=\"0.25\"/>"
      "    <input name=\"emission_luminance\" type=\"float\" value=\"1.5\"/>"
      "    <input name=\"geometry_opacity\" type=\"float\" value=\"0.8\"/>"
      "  </open_pbr_surface>"
      "  <surfacematerial name=\"GraphMat\">"
      "    <input name=\"surfaceshader\" type=\"surfaceshader\""
      "           nodename=\"GraphSurface\"/>"
      "  </surfacematerial>"
      "</materialx>";
  lusdview::tydra::LightRtOpenPBRParams graphEval;
  std::string graphErr;
  if (!lusdview::EvaluateMaterialXStringToLightRtOpenPBR(
          graphXml, "GraphMat", &graphEval, &graphErr)) {
    std::fprintf(stderr, "MaterialX graph eval failed: %s\n", graphErr.c_str());
    return 1;
  }
  if (!Near(graphEval.baseColor[0], 0.4f) ||
      !Near(graphEval.baseColor[1], 0.2f) ||
      !Near(graphEval.baseColor[2], 0.15f) ||
      !Near(graphEval.diffuseRoughness, 0.12f) ||
      !Near(graphEval.metalness, 0.45f) ||
      !Near(graphEval.specularRoughness, 0.25f) ||
      !Near(graphEval.emission, 1.5f) ||
      !Near(graphEval.opacity, 0.8f)) {
    std::fprintf(stderr, "MaterialX graph values were not evaluated\n");
    return 1;
  }

  const char* unlitXml =
      "<materialx version=\"1.39\">"
      "  <surface_unlit name=\"UnlitSurface\" type=\"surfaceshader\">"
      "    <input name=\"emission\" type=\"float\" value=\"2.5\"/>"
      "    <input name=\"emission_color\" type=\"color3\" value=\"0.2,0.4,0.8\"/>"
      "    <input name=\"transmission\" type=\"float\" value=\"0.35\"/>"
      "    <input name=\"transmission_color\" type=\"color3\" value=\"0.8,0.6,0.4\"/>"
      "    <input name=\"opacity\" type=\"float\" value=\"0.65\"/>"
      "  </surface_unlit>"
      "  <surfacematerial name=\"UnlitMat\"><input name=\"surfaceshader\""
      "    type=\"surfaceshader\" nodename=\"UnlitSurface\"/></surfacematerial>"
      "</materialx>";
  lusdview::tydra::LightRtOpenPBRParams unlitEval;
  if (!lusdview::EvaluateMaterialXStringToLightRtOpenPBR(
          unlitXml, "UnlitMat", &unlitEval, &graphErr) ||
      !Near(unlitEval.emission, 2.5f) ||
      !Near(unlitEval.emissionColor[0], 0.2f) ||
      !Near(unlitEval.emissionColor[1], 0.4f) ||
      !Near(unlitEval.emissionColor[2], 0.8f) ||
      !Near(unlitEval.transmission, 0.35f) ||
      !Near(unlitEval.transmissionColor[0], 0.8f) ||
      !Near(unlitEval.transmissionColor[1], 0.6f) ||
      !Near(unlitEval.transmissionColor[2], 0.4f) ||
      !Near(unlitEval.opacity, 0.65f)) {
    std::fprintf(stderr, "surface_unlit bridge evaluation failed: %s\n",
                 graphErr.c_str());
    return 1;
  }

  const char* standardGraphXml =
      "<materialx version=\"1.38\">"
      "  <nodegraph name=\"NG_standard\">"
      "    <combine3 name=\"packed\" type=\"color3\">"
      "      <input name=\"in1\" type=\"float\" value=\"0.2\"/>"
      "      <input name=\"in2\" type=\"float\" value=\"0.4\"/>"
      "      <input name=\"in3\" type=\"float\" value=\"0.6\"/>"
      "    </combine3>"
      "    <extract name=\"green\" type=\"float\">"
      "      <input name=\"in\" type=\"color3\" nodename=\"packed\"/>"
      "      <input name=\"index\" type=\"integer\" value=\"1\"/>"
      "    </extract>"
      "    <convert name=\"green_color\" type=\"color3\">"
      "      <input name=\"in\" type=\"float\" nodename=\"green\"/>"
      "    </convert>"
      "    <add name=\"base\" type=\"color3\">"
      "      <input name=\"in1\" type=\"color3\" nodename=\"green_color\"/>"
      "      <input name=\"in2\" type=\"color3\" value=\"0.1, 0.2, 0.3\"/>"
      "    </add>"
      "    <output name=\"base_out\" type=\"color3\" nodename=\"base\"/>"
      "  </nodegraph>"
      "  <standard_surface name=\"StandardSurface\" type=\"surfaceshader\">"
      "    <input name=\"base_color\" type=\"color3\""
      "           nodegraph=\"NG_standard\" output=\"base_out\"/>"
      "    <input name=\"roughness\" type=\"float\" value=\"0.42\"/>"
      "    <input name=\"specular_ior\" type=\"float\" value=\"1.55\"/>"
      "    <input name=\"coat_ior\" type=\"float\" value=\"1.45\"/>"
      "    <input name=\"opacity\" type=\"float\" value=\"0.37\"/>"
      "  </standard_surface>"
      "  <surfacematerial name=\"StandardGraphMat\">"
      "    <input name=\"surfaceshader\" type=\"surfaceshader\""
      "           nodename=\"StandardSurface\"/>"
      "  </surfacematerial>"
      "</materialx>";
  lusdview::tydra::LightRtOpenPBRParams standardGraphEval;
  if (!lusdview::EvaluateMaterialXStringToLightRtOpenPBR(
          standardGraphXml, "StandardGraphMat", &standardGraphEval,
          &graphErr)) {
    std::fprintf(stderr, "MaterialX standard_surface eval failed: %s\n",
                 graphErr.c_str());
    return 1;
  }
  if (!Near(standardGraphEval.baseColor[0], 0.5f) ||
      !Near(standardGraphEval.baseColor[1], 0.6f) ||
      !Near(standardGraphEval.baseColor[2], 0.7f) ||
      !Near(standardGraphEval.specularRoughness, 0.42f) ||
      !Near(standardGraphEval.specularIor, 1.55f) ||
      !Near(standardGraphEval.coatIor, 1.45f) ||
      !Near(standardGraphEval.opacity, 0.37f)) {
    std::fprintf(stderr,
                 "MaterialX standard_surface graph values were not evaluated\n");
    return 1;
  }

  lusdview::DrawMaterialCPU ifaceGraphMat;
  ifaceGraphMat.name = "InterfaceGraph";
  ifaceGraphMat.hasOpenPBRSurface = true;
  ifaceGraphMat.materialXNodeGraphJson = R"json({
    "version": "1.39",
    "nodegraph": {
      "name": "NG_interface",
      "inputs": [
        {
          "name": "tint",
          "type": "color3",
          "value": [0.25, 0.5, 0.75]
        }
      ],
      "nodes": [
        {
          "name": "mul",
          "category": "multiply",
          "type": "color3",
          "inputs": [
            {"name": "in1", "type": "color3", "interfacename": "tint"},
            {"name": "in2", "type": "color3", "value": [2.0, 0.5, 0.25]}
          ]
        }
      ],
      "outputs": [
        {
          "name": "out",
          "type": "color3",
          "nodename": "mul"
        }
      ]
    },
    "connections": [
      {
        "input": "base_color",
        "nodegraph": "NG_interface",
        "output": "out"
      },
      {
        "input": "roughness",
        "nodegraph": "NG_interface",
        "output": "out",
        "channels": "g"
      },
      {
        "input": "metalness",
        "nodegraph": "NG_interface",
        "output": "out",
        "channels": "r"
      }
    ]
  })json";
  lusdview::BakeLightRtOpenPBR(&ifaceGraphMat);
  if (!ifaceGraphMat.hasLightRtOpenPBR ||
      !Near(ifaceGraphMat.lightRtOpenPBR.baseColor[0], 0.5f) ||
      !Near(ifaceGraphMat.lightRtOpenPBR.baseColor[1], 0.25f) ||
      !Near(ifaceGraphMat.lightRtOpenPBR.baseColor[2], 0.1875f) ||
      !Near(ifaceGraphMat.lightRtOpenPBR.specularRoughness, 0.25f) ||
      !Near(ifaceGraphMat.lightRtOpenPBR.metalness, 0.5f)) {
    std::fprintf(stderr,
                 "MaterialX interface input graph was not evaluated\n");
    return 1;
  }

  lusdview::DrawMaterialCPU channelsGraphMat;
  channelsGraphMat.name = "ChannelsGraph";
  channelsGraphMat.hasOpenPBRSurface = true;
  channelsGraphMat.materialXNodeGraphJson = R"json({
    "version": "1.39",
    "nodegraph": {
      "name": "NG_channels",
      "nodes": [
        {
          "name": "packed",
          "category": "constant",
          "type": "color4",
          "inputs": [
            {"name": "value", "type": "color4", "value": [0.1, 0.2, 0.3, 0.4]}
          ]
        },
        {
          "name": "bgr",
          "category": "swizzle",
          "type": "color3",
          "inputs": [
            {"name": "in", "type": "color4", "nodename": "packed"},
            {"name": "channels", "type": "string", "value": "bgr"}
          ]
        }
      ],
      "outputs": [
        {
          "name": "base_out",
          "type": "color3",
          "nodename": "bgr"
        },
        {
          "name": "packed_out",
          "type": "color4",
          "nodename": "packed"
        }
      ]
    },
    "connections": [
      {
        "input": "base_color",
        "nodegraph": "NG_channels",
        "output": "base_out"
      },
      {
        "input": "base_roughness",
        "nodegraph": "NG_channels",
        "output": "packed_out",
        "channels": "g"
      },
      {
        "input": "opacity",
        "nodegraph": "NG_channels",
        "output": "packed_out",
        "channels": "a"
      }
    ]
  })json";
  lusdview::BakeLightRtOpenPBR(&channelsGraphMat);
  if (!channelsGraphMat.hasLightRtOpenPBR ||
      !Near(channelsGraphMat.lightRtOpenPBR.baseColor[0], 0.3f) ||
      !Near(channelsGraphMat.lightRtOpenPBR.baseColor[1], 0.2f) ||
      !Near(channelsGraphMat.lightRtOpenPBR.baseColor[2], 0.1f) ||
      !Near(channelsGraphMat.lightRtOpenPBR.specularRoughness, 0.2f) ||
      !Near(channelsGraphMat.lightRtOpenPBR.opacity, 0.4f)) {
    std::fprintf(stderr,
                 "MaterialX channel selector graph was not evaluated\n");
    return 1;
  }

  lusdview::DrawMaterialCPU imageGraphMat;
  imageGraphMat.name = "ImageGraph";
  imageGraphMat.hasOpenPBRSurface = true;
  imageGraphMat.params.push_back(Vec3Param("base_color", 0.2f, 0.4f, 0.6f));
  imageGraphMat.params.push_back(FloatParam("base_metalness", 0.25f));
  imageGraphMat.params.push_back(FloatParam("base_roughness", 0.5f));
  imageGraphMat.materialXNodeGraphJson = R"json({
    "version": "1.39",
    "nodegraph": {
      "name": "NG_image_graph",
      "nodes": [
        {
          "name": "base_img",
          "category": "image",
          "type": "color3",
          "inputs": [
            {"name": "file", "type": "filename", "value": "missing_base.png"},
            {"name": "default", "type": "color3", "value": [0.9, 0.1, 0.1]}
          ]
        },
        {
          "name": "metal_mul",
          "category": "multiply",
          "type": "float",
          "inputs": [
            {"name": "in1", "type": "float", "value": 0.6},
            {"name": "in2", "type": "float", "value": 0.7}
          ]
        }
      ],
      "outputs": [
        {
          "name": "base_color_output",
          "type": "color3",
          "nodename": "base_img",
          "output": "out"
        },
        {
          "name": "metal_output",
          "type": "float",
          "nodename": "metal_mul",
          "output": "out"
        }
      ]
    },
    "connections": [
      {
        "input": "base_color",
        "nodegraph": "NG_image_graph",
        "output": "base_color_output"
      },
      {
        "input": "base_metalness",
        "nodegraph": "NG_image_graph",
        "output": "metal_output"
      }
    ]
  })json";
  std::string graphCompileError;
  if (!lusdview::CompileMaterialXGraphRuntime(&imageGraphMat,
                                               &graphCompileError) ||
      !imageGraphMat.materialXGraph.valid ||
      imageGraphMat.materialXGraph.nodes.size() != 2 ||
      imageGraphMat.materialXGraph.output[0] < 0 ||
      imageGraphMat.materialXGraph.output[1] < 0) {
    std::fprintf(stderr, "MaterialX graph IR compilation failed: %s\n",
                 graphCompileError.c_str());
    return 1;
  }
  std::vector<float> packedGraph(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(imageGraphMat, packedGraph.data());
  if (packedGraph[0] != 2.0f || packedGraph[1] < 0.0f ||
      packedGraph[2] < 0.0f ||
      packedGraph[lusdview::kRtMaterialGraphHeaderFloats + 15] != -1.0f) {
    std::fprintf(stderr, "MaterialX graph runtime packing failed\n");
    return 1;
  }
  imageGraphMat.materialXGraph.nodes[0].textureId = 3;
  std::vector<int> sourceToTable = {-1, 7, -1, 2};
  std::fill(packedGraph.begin(), packedGraph.end(), 0.0f);
  lusdview::PackMaterialXGraphRuntime(imageGraphMat, packedGraph.data(),
                                      &sourceToTable);
  const size_t imageNodePacked =
      lusdview::kRtMaterialGraphHeaderFloats + 16;
  if (packedGraph[imageNodePacked] != 2.0f) {
    std::fprintf(stderr, "MaterialX graph source texture remapping failed\n");
    return 1;
  }
  lusdview::BakeLightRtOpenPBR(&imageGraphMat);
  if (!imageGraphMat.hasLightRtOpenPBR ||
      !imageGraphMat.lightRtOpenPBR.hasTextureInputs) {
    std::fprintf(stderr, "MaterialX image graph was not marked texture-dependent\n");
    return 1;
  }
  if (!Near(imageGraphMat.lightRtOpenPBR.baseColor[0], 0.2f) ||
      !Near(imageGraphMat.lightRtOpenPBR.baseColor[1], 0.4f) ||
      !Near(imageGraphMat.lightRtOpenPBR.baseColor[2], 0.6f)) {
    std::fprintf(stderr,
                 "MaterialX image graph default was incorrectly constant-baked\n");
    return 1;
  }
  if (!Near(imageGraphMat.lightRtOpenPBR.metalness, 0.42f)) {
    std::fprintf(stderr,
                 "MaterialX mixed graph constant lane was not baked\n");
    return 1;
  }

  // Advanced OpenPBR inputs must remain descriptor-routed, not merely baked
  // into the constant fallback. This locks the route ABI consumed by the
  // Vulkan production path tracer (and shared CUDA/HIP graph buffer).
  lusdview::DrawMaterialCPU advancedGraphMat;
  advancedGraphMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"film", "category":"constant", "inputs":[{"value":0.75}]},
      {"name":"aniso", "category":"constant", "inputs":[{"value":0.6}]},
      {"name":"disp", "category":"constant", "inputs":[{"value":1.0}]},
      {"name":"scatter_g", "category":"constant", "inputs":[{"value":0.35}]},
      {"name":"density", "category":"constant", "inputs":[{"value":0.25}]},
      {"name":"albedo", "category":"constant", "inputs":[{"value":[0.2,0.4,0.6]}]},
      {"name":"emission", "category":"constant", "inputs":[{"value":3.0}]}
    ], "outputs": [
      {"name":"film_o", "nodename":"film"},
      {"name":"aniso_o", "nodename":"aniso"},
      {"name":"disp_o", "nodename":"disp"},
      {"name":"scatter_g_o", "nodename":"scatter_g"},
      {"name":"density_o", "nodename":"density"},
      {"name":"albedo_o", "nodename":"albedo"},
      {"name":"emission_o", "nodename":"emission"}
    ]},
    "connections": [
      {"input":"thin_film_weight", "output":"film_o"},
      {"input":"specular_anisotropy", "output":"aniso_o"},
      {"input":"transmission_dispersion", "output":"disp_o"},
      {"input":"transmission_scatter_anisotropy", "output":"scatter_g_o"},
      {"input":"volume_density", "output":"density_o"},
      {"input":"volume_albedo", "output":"albedo_o"},
      {"input":"emission_luminance", "output":"emission_o"},
      {"input":"coat_affect_color", "output":"film_o"},
      {"input":"coat_affect_roughness", "output":"film_o"},
      {"input":"coat_darkening", "output":"film_o"}
    ]
  })json";
  std::string advancedError;
  if (!lusdview::CompileMaterialXGraphRuntime(&advancedGraphMat,
                                               &advancedError) ||
      advancedGraphMat.materialXGraph.output[28] < 0 ||
      advancedGraphMat.materialXGraph.output[31] < 0 ||
      advancedGraphMat.materialXGraph.output[34] < 0 ||
      advancedGraphMat.materialXGraph.output[24] < 0 ||
      advancedGraphMat.materialXGraph.output[40] < 0 ||
      advancedGraphMat.materialXGraph.output[41] < 0 ||
      advancedGraphMat.materialXGraph.output[44] < 0 ||
      advancedGraphMat.materialXGraph.output[45] < 0 ||
      advancedGraphMat.materialXGraph.output[46] < 0 ||
      advancedGraphMat.materialXGraph.output[47] < 0) {
    std::fprintf(stderr, "advanced OpenPBR graph routing failed: %s\n",
                 advancedError.c_str());
    return 1;
  }
  std::vector<float> advancedPack(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(advancedGraphMat, advancedPack.data());
  for (int route : {24, 28, 31, 34, 40, 41, 44, 45, 46, 47}) {
    if (advancedPack[1 + route] < 0.0f) {
      std::fprintf(stderr, "advanced OpenPBR route %d was not packed\n", route);
      return 1;
    }
  }

  // Keep the canonical IR operation table aligned across CPU, Vulkan, CUDA,
  // and HIP interpreters. These scalar nodes are deliberately independent of
  // LightRT's legacy bake evaluator.
  lusdview::DrawMaterialCPU extendedGraphMat;
  extendedGraphMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"pow","category":"power","inputs":[{"value":2.0},{"value":3.0},{"name":"scale","value":[2.0,3.0]},{"name":"offset","value":[0.1,0.2]}]},
      {"name":"min","category":"minimum","inputs":[{"nodename":"pow"},{"value":9.0}]},
      {"name":"max","category":"maximum","inputs":[{"nodename":"min"},{"value":1.0}]},
      {"name":"abs","category":"abs","inputs":[{"nodename":"max"}]},
      {"name":"sqrt","category":"sqrt","inputs":[{"nodename":"abs"}]},
      {"name":"sin","category":"sin","inputs":[{"nodename":"sqrt"}]},
      {"name":"cos","category":"cos","inputs":[{"nodename":"sin"}]},
      {"name":"lum","category":"luminance","inputs":[{"value":[1.0,2.0,3.0]}]},
      {"name":"sel","category":"select","inputs":[{"value":1.0},{"value":2.0},{"value":3.0}]},
      {"name":"remap","category":"remap","inputs":[{"value":0.5},{"value":0.0},{"value":1.0}]}
    ], "outputs": []}, "connections": []
  })json";
  std::string extendedError;
  if (!lusdview::CompileMaterialXGraphRuntime(&extendedGraphMat,
                                               &extendedError) ||
      extendedGraphMat.materialXGraph.nodes.size() != 10 ||
      extendedGraphMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::Power ||
      extendedGraphMat.materialXGraph.nodes[8].op !=
          lusdview::MaterialXGraphOpCPU::Select ||
      extendedGraphMat.materialXGraph.nodes[9].op !=
          lusdview::MaterialXGraphOpCPU::Remap ||
      !Near(extendedGraphMat.materialXGraph.nodes[7].value[0][1], 2.0f) ||
      !Near(extendedGraphMat.materialXGraph.nodes[8].value[2][0], 3.0f)) {
    std::fprintf(stderr, "extended MaterialX graph operators failed: %s\n",
                 extendedError.c_str());
    return 1;
  }

  lusdview::DrawMaterialCPU scalarGraphMat;
  scalarGraphMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"atan", "category":"atan2", "inputs":[{"value":1.0},{"value":2.0}]},
      {"name":"sgn", "category":"sign", "inputs":[{"value":-2.0}]},
      {"name":"rnd", "category":"round", "inputs":[{"value":1.6}]},
      {"name":"sat", "category":"clamp", "inputs":[{"name":"in","value":-0.5},{"name":"low","value":0.0},{"name":"high","value":1.0}]},
      {"name":"asin", "category":"arcsin", "inputs":[{"value":0.5}]},
      {"name":"acos", "category":"arccos", "inputs":[{"value":0.5}]},
      {"name":"contrast", "category":"contrast", "inputs":[{"value":0.25},{"value":2.0},{"value":0.5}]},
      {"name":"saturate", "category":"saturate", "type":"color3", "inputs":[{"name":"in","value":[0.2,0.4,0.8]},{"name":"amount","value":0.0}]},
      {"name":"swizzle", "category":"ND_swizzle_color4_color3", "type":"color3", "inputs":[{"name":"in","value":[0.1,0.2,0.3,0.4]},{"name":"channels","value":"bgr1"}]}
    ], "outputs": []}, "connections": []
  })json";
  std::string scalarError;
  if (!lusdview::CompileMaterialXGraphRuntime(&scalarGraphMat, &scalarError) ||
      scalarGraphMat.materialXGraph.nodes.size() != 9 ||
      scalarGraphMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::Atan2 ||
      scalarGraphMat.materialXGraph.nodes[1].op !=
          lusdview::MaterialXGraphOpCPU::Sign ||
      scalarGraphMat.materialXGraph.nodes[2].op !=
          lusdview::MaterialXGraphOpCPU::Round ||
      scalarGraphMat.materialXGraph.nodes[3].op !=
          lusdview::MaterialXGraphOpCPU::Clamp ||
      scalarGraphMat.materialXGraph.nodes[4].op !=
          lusdview::MaterialXGraphOpCPU::Arcsine ||
      scalarGraphMat.materialXGraph.nodes[5].op !=
          lusdview::MaterialXGraphOpCPU::Arccosine ||
      scalarGraphMat.materialXGraph.nodes[6].op !=
          lusdview::MaterialXGraphOpCPU::Contrast ||
      scalarGraphMat.materialXGraph.nodes[7].op !=
          lusdview::MaterialXGraphOpCPU::Saturate ||
      scalarGraphMat.materialXGraph.nodes[8].op !=
          lusdview::MaterialXGraphOpCPU::Swizzle ||
      !Near(scalarGraphMat.materialXGraph.nodes[7].value[1][0], 0.0f) ||
      !Near(scalarGraphMat.materialXGraph.nodes[8].value[1][0], 2.0f) ||
      !Near(scalarGraphMat.materialXGraph.nodes[8].value[1][3], 5.0f) ||
      !Near(scalarGraphMat.materialXGraph.nodes[3].value[2][0], 1.0f)) {
    std::fprintf(stderr, "MaterialX scalar procedural operators failed: %s\n",
                 scalarError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU colorUtilityMat;
  colorUtilityMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"to_hsv","category":"rgbtohsv","inputs":[{"name":"in","value":[1,0,0]}]},
      {"name":"to_rgb","category":"hsvtorgb","inputs":[{"name":"in","nodename":"to_hsv"}]},
      {"name":"rot","category":"rotate2d","inputs":[{"name":"in","value":[1,0]},{"name":"amount","value":90}]},
      {"name":"one_minus","category":"oneminus","inputs":[{"name":"in","value":0.25}]}
    ],"outputs":[]},"connections":[]})json";
  std::string colorUtilityError;
  if (!lusdview::CompileMaterialXGraphRuntime(&colorUtilityMat,
                                               &colorUtilityError) ||
      colorUtilityMat.materialXGraph.nodes.size() != 4 ||
      colorUtilityMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::RgbToHsv ||
      colorUtilityMat.materialXGraph.nodes[1].op !=
          lusdview::MaterialXGraphOpCPU::HsvToRgb ||
      colorUtilityMat.materialXGraph.nodes[2].op !=
          lusdview::MaterialXGraphOpCPU::Rotate2D ||
      colorUtilityMat.materialXGraph.nodes[3].op !=
          lusdview::MaterialXGraphOpCPU::Invert) {
    std::fprintf(stderr, "MaterialX color/rotation utilities failed: %s\n",
                 colorUtilityError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU loweredMat;
  loweredMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"quad","category":"ramp4","type":"color3","inputs":[
        {"name":"valuetl","value":[1,0,0]},{"name":"valuetr","value":[0,1,0]},
        {"name":"valuebl","value":[0,0,1]},{"name":"valuebr","value":[1,1,1]},
        {"name":"texcoord","value":[0.25,0.75]}]},
      {"name":"pick","category":"switch","type":"color3","inputs":[
        {"name":"which","value":1},{"name":"in1","value":[1,0,0]},
        {"name":"in2","nodename":"quad"},{"name":"in3","value":[0,0,1]}]}
    ],"outputs":[{"name":"base","nodename":"pick"}]},
    "connections":[{"input":"base_color","output":"base"}]})json";
  std::string loweredError;
  if (!lusdview::CompileMaterialXGraphRuntime(&loweredMat, &loweredError) ||
      loweredMat.materialXGraph.nodes.size() != 14 ||
      loweredMat.materialXGraph.output[0] < 0) {
    std::fprintf(stderr, "MaterialX high-arity lowering failed: %s (nodes=%zu output=%d)\n",
                 loweredError.c_str(), loweredMat.materialXGraph.nodes.size(),
                 loweredMat.materialXGraph.output[0]);
    return 1;
  }
  int rampFinal = -1, switchFinal = -1;
  for (size_t i = 0; i < loweredMat.materialXGraph.nodes.size(); ++i) {
    const auto& lowered = loweredMat.materialXGraph.nodes[i];
    if (lowered.name == "quad") rampFinal = static_cast<int>(i);
    if (lowered.name == "pick") switchFinal = static_cast<int>(i);
  }
  if (rampFinal < 0 || switchFinal < 0 ||
      loweredMat.materialXGraph.nodes[rampFinal].op !=
          lusdview::MaterialXGraphOpCPU::Mix ||
      loweredMat.materialXGraph.nodes[switchFinal].op !=
          lusdview::MaterialXGraphOpCPU::IfEqual ||
      loweredMat.materialXGraph.output[0] != switchFinal) {
    std::fprintf(stderr, "MaterialX lowered graph routes are incorrect\n");
    return 1;
  }
  lusdview::DrawMaterialCPU rampMat;
  rampMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"r","category":"ramp","type":"color4","inputs":[
      {"name":"texcoord","value":[0.25,0.5]},{"name":"interpolation","value":0},
      {"name":"num_intervals","value":3},{"name":"interval1","value":0},
      {"name":"color1","value":[0,0,0,1]},{"name":"interval2","value":0.5},
      {"name":"color2","value":[1,0,0,1]},{"name":"interval3","value":1},
      {"name":"color3","value":[1,1,1,1]}]},
    {"name":"g","category":"ramp_gradient","type":"color4","inputs":[
      {"name":"x","value":0.25},{"name":"interval1","value":0},
      {"name":"interval2","value":1},{"name":"color1","value":[0,0,0,1]},
      {"name":"color2","value":[1,0.5,0,1]},{"name":"interpolation","value":1},
      {"name":"prev_color","value":[0,0,1,1]},{"name":"interval_num","value":1},
      {"name":"num_intervals","value":2}]}
  ],"outputs":[]},"connections":[]})json";
  std::string rampError;
  if (!lusdview::CompileMaterialXGraphRuntime(&rampMat, &rampError) ||
      rampMat.materialXGraph.nodes.size() != 29 ||
      rampMat.materialXGraph.nodes[21].op != lusdview::MaterialXGraphOpCPU::Ramp ||
      rampMat.materialXGraph.nodes[28].op != lusdview::MaterialXGraphOpCPU::RampGradient ||
      !Near(rampMat.materialXGraph.nodes[21].auxValue[0], 1.0f) ||
      !Near(rampMat.materialXGraph.nodes[28].auxValue[0], 22.0f)) {
    std::fprintf(stderr, "MaterialX ramp lowering failed: %s (nodes=%zu)\n",
                 rampError.c_str(), rampMat.materialXGraph.nodes.size());
    return 1;
  }
  lusdview::DrawMaterialCPU blurMat;
  blurMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"source","category":"constant","type":"color3","inputs":[{"name":"value","value":[0.2,0.4,0.8]}]},
    {"name":"blurred","category":"blur","type":"color3","inputs":[{"name":"in","nodename":"source"},{"name":"size","value":0.5},{"name":"filtertype","value":"gaussian"}]}
  ],"outputs":[]},"connections":[]})json";
  std::string blurError;
  if (!lusdview::CompileMaterialXGraphRuntime(&blurMat, &blurError) ||
      blurMat.materialXGraph.nodes.size() != 2 ||
      blurMat.materialXGraph.nodes[1].op != lusdview::MaterialXGraphOpCPU::Convert ||
      blurMat.materialXGraph.nodes[1].input[0] != 0) {
    std::fprintf(stderr, "MaterialX blur lowering failed: %s\n", blurError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU flakeMat;
  flakeMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"flakes","category":"flake2d","type":"multioutput","inputs":[
      {"name":"size","value":0.1},{"name":"roughness","value":0.2},
      {"name":"coverage","value":0.75}]},
    {"name":"scaled_presence","category":"multiply","type":"float","inputs":[
      {"name":"in1","nodename":"flakes","output":"presence"},{"name":"in2","value":0.5}]}
  ],"outputs":[{"name":"flake_normal","type":"vector3","nodename":"flakes","output":"flakenormal"}]},
  "connections":[{"input":"geometry_normal","output":"flake_normal"}]})json";
  std::string flakeError;
  if (!lusdview::CompileMaterialXGraphRuntime(&flakeMat, &flakeError) ||
      flakeMat.materialXGraph.nodes.size() != 17 ||
      flakeMat.materialXGraph.nodes[11].op != lusdview::MaterialXGraphOpCPU::Flake ||
      flakeMat.materialXGraph.nodes[14].op != lusdview::MaterialXGraphOpCPU::Flake ||
      flakeMat.materialXGraph.nodes[16].input[0] != 13 ||
      flakeMat.materialXGraph.output[5] != 14 ||
      !Near(flakeMat.materialXGraph.nodes[11].auxValue[0], 0.0f) ||
      !Near(flakeMat.materialXGraph.nodes[14].auxValue[1], 3.0f)) {
    std::fprintf(stderr, "MaterialX flake multi-output lowering failed: %s (nodes=%zu)\n",
                 flakeError.c_str(), flakeMat.materialXGraph.nodes.size());
    return 1;
  }
  lusdview::DrawMaterialCPU matrixMat;
  matrixMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"m","category":"creatematrix","type":"matrix33","inputs":[{"name":"in1","type":"vector3","value":[2,0,0]},{"name":"in2","type":"vector3","value":[0,3,0]},{"name":"in3","type":"vector3","value":[0,0,4]}]},
    {"name":"x","category":"transformmatrix","type":"vector3","inputs":[{"name":"in","value":[1,2,3]},{"name":"mat","type":"matrix33","nodename":"m"}]},
    {"name":"mt","category":"transpose","type":"matrix33","inputs":[{"name":"in","type":"matrix33","nodename":"m"}]},
    {"name":"mi","category":"invertmatrix","type":"matrix33","inputs":[{"name":"in","type":"matrix33","nodename":"m"}]},
    {"name":"det","category":"determinant","type":"float","inputs":[{"name":"in","type":"matrix33","nodename":"m"}]},
    {"name":"back","category":"transformmatrix","type":"vector3","inputs":[{"name":"in","nodename":"x"},{"name":"mat","type":"matrix33","nodename":"mi"}]}
  ],"outputs":[]},"connections":[]})json";
  std::string matrixError;
  if(!lusdview::CompileMaterialXGraphRuntime(&matrixMat,&matrixError)||
     matrixMat.materialXGraph.nodes.size()!=12||
     matrixMat.materialXGraph.nodes[3].op!=lusdview::MaterialXGraphOpCPU::MatrixTransform||
     matrixMat.materialXGraph.nodes[4].op!=lusdview::MaterialXGraphOpCPU::MatrixTranspose||
     matrixMat.materialXGraph.nodes[7].op!=lusdview::MaterialXGraphOpCPU::MatrixInverse||
     matrixMat.materialXGraph.nodes[10].op!=lusdview::MaterialXGraphOpCPU::MatrixDeterminant||
     !Near(matrixMat.materialXGraph.nodes[3].auxValue[0],0)||
     !Near(matrixMat.materialXGraph.nodes[11].auxValue[0],7)){
    std::fprintf(stderr,"MaterialX matrix table lowering failed: %s (nodes=%zu)\n",matrixError.c_str(),matrixMat.materialXGraph.nodes.size());return 1;
  }
  lusdview::DrawMaterialCPU matrix4Mat;
  matrix4Mat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"m4","category":"creatematrix_vector3","type":"matrix44","inputs":[{"name":"in1","value":[1,0,0]},{"name":"in2","value":[0,1,0]},{"name":"in3","value":[0,0,1]},{"name":"in4","value":[5,6,7]}]},
    {"name":"x4","category":"transformmatrix_vector3","type":"vector3","inputs":[{"name":"in","value":[1,2,3]},{"name":"mat","type":"matrix44","nodename":"m4"}]},
    {"name":"literal","category":"transformmatrix_vector3","type":"vector3","inputs":[{"name":"in","value":[1,2,3]},{"name":"mat","type":"matrix44","value":[1,0,0,0,0,1,0,0,0,0,1,0,5,6,7,1]}]}
  ],"outputs":[]},"connections":[]})json";
  std::string matrix4Error;
  if(!lusdview::CompileMaterialXGraphRuntime(&matrix4Mat,&matrix4Error)||
     matrix4Mat.materialXGraph.nodes.size()!=10||
     !Near(matrix4Mat.materialXGraph.nodes[3].value[0][3],1)||
     !Near(matrix4Mat.materialXGraph.nodes[4].auxValue[0],0)||
     !Near(matrix4Mat.materialXGraph.nodes[9].auxValue[0],5)){
    std::fprintf(stderr,"MaterialX matrix44 lowering failed: %s (nodes=%zu)\n",matrix4Error.c_str(),matrix4Mat.materialXGraph.nodes.size());return 1;
  }
  lusdview::DrawMaterialCPU contextMat;
  contextMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"view","category":"viewdirection","type":"vector3","inputs":[]},
    {"name":"time","category":"time","type":"float","inputs":[]},
    {"name":"frame","category":"frame","type":"float","inputs":[]},
    {"name":"st","category":"geompropvalue","type":"vector2","inputs":[{"name":"geomprop","type":"string","value":"st"}]},
    {"name":"custom","category":"geompropvalue","type":"float","inputs":[{"name":"geomprop","type":"string","value":"temperature"},{"name":"default","type":"float","value":2700}]},
    {"name":"warm","category":"blackbody","type":"color3","inputs":[{"name":"temperature","value":800}]},
    {"name":"aniso","category":"roughness_anisotropy","type":"vector2","inputs":[{"name":"roughness","value":0.5},{"name":"anisotropy","value":0.75}]},
    {"name":"dual","category":"roughness_dual","type":"vector2","inputs":[{"name":"roughness","value":[0.5,-1]}]},
    {"name":"artistic","category":"artistic_ior","type":"multioutput","inputs":[{"name":"reflectivity","value":[0.25,0.25,0.25]},{"name":"edge_color","value":[1,1,1]}]},
    {"name":"gloss","category":"glossiness_anisotropy","type":"vector2","inputs":[{"name":"glossiness","value":0.5},{"name":"anisotropy","value":0.75}]},
    {"name":"deon","category":"deon_hair_absorption_from_melanin","type":"vector3","inputs":[{"name":"melanin_concentration","value":1},{"name":"melanin_redness","value":0},{"name":"eumelanin_color","value":[0.367879441,0.367879441,0.367879441]}]},
    {"name":"chiang_abs","category":"chiang_hair_absorption_from_color","type":"vector3","inputs":[{"name":"color","value":[0.5,0.5,0.5]},{"name":"azimuthal_roughness","value":0.2}]},
    {"name":"chiang_rough","category":"chiang_hair_roughness","type":"multioutput","inputs":[{"name":"longitudinal","value":0.1},{"name":"azimuthal","value":0.2}]}
  ],"outputs":[]},"connections":[]})json";
  std::string contextError;
  if(!lusdview::CompileMaterialXGraphRuntime(&contextMat,&contextError)||
     contextMat.materialXGraph.nodes.size()<=26||
     contextMat.materialXGraph.nodes[0].op!=lusdview::MaterialXGraphOpCPU::ViewDirection||
     contextMat.materialXGraph.nodes[1].op!=lusdview::MaterialXGraphOpCPU::Time||
     contextMat.materialXGraph.nodes[2].op!=lusdview::MaterialXGraphOpCPU::Frame||
     contextMat.materialXGraph.nodes[3].op!=lusdview::MaterialXGraphOpCPU::Texcoord||
     contextMat.materialXGraph.nodes[4].op!=lusdview::MaterialXGraphOpCPU::GeomProp||
     contextMat.materialXGraph.nodes[4].geomPropName!="temperature"||
     contextMat.materialXGraph.nodes[5].op!=lusdview::MaterialXGraphOpCPU::Blackbody||
     contextMat.materialXGraph.nodes[6].op!=lusdview::MaterialXGraphOpCPU::RoughnessAnisotropy||
     contextMat.materialXGraph.nodes[7].op!=lusdview::MaterialXGraphOpCPU::RoughnessDual||
     contextMat.materialXGraph.nodes[8].op!=lusdview::MaterialXGraphOpCPU::ArtisticIor||
     contextMat.materialXGraph.nodes[9].op!=lusdview::MaterialXGraphOpCPU::ArtisticIor||
     contextMat.materialXGraph.nodes[10].op!=lusdview::MaterialXGraphOpCPU::Convert||
     !Near(contextMat.materialXGraph.nodes[9].auxValue[0],1.0f)||
     contextMat.materialXGraph.nodes[11].op!=lusdview::MaterialXGraphOpCPU::Invert||
     contextMat.materialXGraph.nodes[12].op!=lusdview::MaterialXGraphOpCPU::RoughnessAnisotropy||
     contextMat.materialXGraph.nodes[20].op!=lusdview::MaterialXGraphOpCPU::ChiangHairAbsorption||
     contextMat.materialXGraph.nodes[21].op!=lusdview::MaterialXGraphOpCPU::Convert||
     !std::any_of(contextMat.materialXGraph.nodes.begin(),
                  contextMat.materialXGraph.nodes.end(),
                  [](const lusdview::MaterialXGraphNodeCPU& node) {
                    return node.name == "chiang_rough__roughness_R" &&
                           node.op == lusdview::MaterialXGraphOpCPU::Combine;
                  })) {
    std::fprintf(stderr,"MaterialX runtime context lowering failed: %s (nodes=%zu)\n",contextError.c_str(),contextMat.materialXGraph.nodes.size());return 1;
  }
  std::vector<float> contextPack(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(contextMat, contextPack.data());
  const size_t contextCustom = lusdview::kRtMaterialGraphHeaderFloats +
                               4 * lusdview::kRtMaterialGraphNodeFloats;
  uint32_t contextHash = 0;
  std::memcpy(&contextHash, &contextPack[contextCustom + 17],
              sizeof(contextHash));
  if (contextPack[contextCustom] !=
          static_cast<float>(lusdview::MaterialXGraphOpCPU::GeomProp) ||
      contextHash != lusdview::MaterialXGeomPropHash("temperature") ||
      contextPack[contextCustom + 18] != 1.0f) {
    std::fprintf(stderr, "MaterialX geomprop graph packing failed\n");
    return 1;
  }
  lusdview::DrawMaterialCPU latlongMat;
  latlongMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"direction","category":"constant","type":"vector3","inputs":[{"name":"value","value":[1,0,0]}]},
    {"name":"environment","category":"latlongimage","type":"color3","inputs":[{"name":"file","type":"filename","value":"environment.png"},{"name":"default","value":[0.1,0.2,0.3]},{"name":"viewdir","nodename":"direction"},{"name":"rotation","value":90}]}
  ],"outputs":[]},"connections":[]})json";
  std::string latlongError;
  if(!lusdview::CompileMaterialXGraphRuntime(&latlongMat,&latlongError)||
     latlongMat.materialXGraph.nodes.size()!=14||
     latlongMat.materialXGraph.nodes.back().op!=lusdview::MaterialXGraphOpCPU::Image||
     latlongMat.materialXGraph.nodes.back().input[0]!=12||
     latlongMat.materialXGraph.nodes.back().imagePath!="environment.png"){
    std::fprintf(stderr,"MaterialX latlongimage lowering failed: %s (nodes=%zu)\n",latlongError.c_str(),latlongMat.materialXGraph.nodes.size());return 1;
  }
  lusdview::DrawMaterialCPU latlongDefaultMat;
  latlongDefaultMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"environment","category":"latlongimage","type":"color3","inputs":[{"name":"file","type":"filename","value":"environment.png"}]}
  ],"outputs":[]},"connections":[]})json";
  std::string latlongDefaultError;
  if(!lusdview::CompileMaterialXGraphRuntime(&latlongDefaultMat,&latlongDefaultError)){
    std::fprintf(stderr,"MaterialX latlong default lowering failed: %s\n",latlongDefaultError.c_str());return 1;
  }
  bool hasViewDirection=false;
  for(const auto& graphNode:latlongDefaultMat.materialXGraph.nodes)
    hasViewDirection = hasViewDirection ||
        graphNode.op==lusdview::MaterialXGraphOpCPU::ViewDirection;
  if(!hasViewDirection){
    std::fprintf(stderr,"MaterialX latlong default did not use runtime view direction\n");return 1;
  }
  lusdview::DrawMaterialCPU triplanarMat;
  triplanarMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"projection","category":"triplanarprojection","type":"color3","inputs":[{"name":"filex","type":"filename","value":"x.png"},{"name":"filey","type":"filename","value":"y.png"},{"name":"filez","type":"filename","value":"z.png"},{"name":"default","value":[0.1,0.2,0.3]},{"name":"position","value":[1,2,3]},{"name":"normal","value":[1,1,1]},{"name":"upaxis","value":2},{"name":"blend","value":0.5}]}
  ],"outputs":[]},"connections":[]})json";
  std::string triplanarError;int triplanarImages=0;
  if(!lusdview::CompileMaterialXGraphRuntime(&triplanarMat,&triplanarError)){
    std::fprintf(stderr,"MaterialX triplanar lowering failed: %s\n",triplanarError.c_str());return 1;
  }
  for(const auto& graphNode:triplanarMat.materialXGraph.nodes)
    if(graphNode.op==lusdview::MaterialXGraphOpCPU::Image)triplanarImages++;
  if(triplanarImages!=3||triplanarMat.materialXGraph.nodes.empty()||
     triplanarMat.materialXGraph.nodes.back().op!=lusdview::MaterialXGraphOpCPU::Add){
    std::fprintf(stderr,"MaterialX triplanar topology invalid (nodes=%zu images=%d)\n",triplanarMat.materialXGraph.nodes.size(),triplanarImages);return 1;
  }
  lusdview::DrawMaterialCPU bumpMat;
  bumpMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"height","category":"image","type":"float","inputs":[{"name":"file","value":"height.png"}]},
    {"name":"bumped","category":"bump","type":"vector3","inputs":[{"name":"height","nodename":"height"},{"name":"scale","value":3},{"name":"normal","value":[0,0,1]}]}
  ],"outputs":[]},"connections":[]})json";
  std::string bumpError;
  if(!lusdview::CompileMaterialXGraphRuntime(&bumpMat,&bumpError)||
     bumpMat.materialXGraph.nodes.size()!=3||
     bumpMat.materialXGraph.nodes[1].op!=lusdview::MaterialXGraphOpCPU::HeightToNormal||
     bumpMat.materialXGraph.nodes[1].input[0]!=0||
     bumpMat.materialXGraph.nodes[2].op!=lusdview::MaterialXGraphOpCPU::NormalMap||
     bumpMat.materialXGraph.nodes[2].input[0]!=1){
    std::fprintf(stderr,"MaterialX bump lowering failed: %s (nodes=%zu)\n",bumpError.c_str(),bumpMat.materialXGraph.nodes.size());return 1;
  }
  lusdview::DrawMaterialCPU aliasesMat;
  aliasesMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"plus","category":"plus","inputs":[{"value":1},{"value":2}]},
    {"name":"minus","category":"minus","inputs":[{"value":3},{"value":1}]},
    {"name":"safe","category":"safepower","inputs":[{"value":2},{"value":3}]},
    {"name":"absolute","category":"absval","inputs":[{"value":-1}]},
    {"name":"cross","category":"crossproduct","inputs":[{"value":[1,0,0]},{"value":[0,1,0]}]},
    {"name":"length","category":"magnitude","inputs":[{"value":[3,4,0]}]},
    {"name":"log","category":"ln","inputs":[{"value":2.7182818}]},
    {"name":"sep","category":"separate3","inputs":[{"value":[1,2,3]}]},
    {"name":"hexn","category":"hextilednormalmap","inputs":[{"name":"in","value":[0.5,0.5,1]}]},
    {"name":"wave","category":"trianglewave","inputs":[{"name":"in","value":1.25}]},
    {"name":"checker","category":"checkerboard","type":"color3","inputs":[{"name":"color1","value":[1,0,0]},{"name":"color2","value":[0,0,1]},{"name":"uvtiling","value":[2,2]},{"name":"texcoord","value":[0.6,0.1]}]},
    {"name":"circle","category":"circle","type":"float","inputs":[{"name":"center","value":[0.5,0.5]},{"name":"radius","value":0.4},{"name":"texcoord","value":[0.5,0.5]}]},
    {"name":"line","category":"line","type":"float","inputs":[{"name":"point1","value":[0.25,0.25]},{"name":"point2","value":[0.75,0.75]},{"name":"radius","value":0.1},{"name":"texcoord","value":[0.5,0.5]}]},
    {"name":"cell2","category":"cellnoise2d","type":"float","inputs":[{"name":"texcoord","value":[1.2,2.8]}]},
    {"name":"cell3","category":"cellnoise3d","type":"float","inputs":[{"name":"position","value":[1.2,2.8,3.4]}]},
    {"name":"random","category":"randomfloat","type":"float","inputs":[{"name":"in","type":"float","value":0.25},{"name":"min","value":2},{"name":"max","value":4},{"name":"seed","value":7}]}
  ],"outputs":[]},"connections":[]})json";
  std::string aliasesError;
  if (!lusdview::CompileMaterialXGraphRuntime(&aliasesMat, &aliasesError) ||
      aliasesMat.materialXGraph.nodes.size() != 44) {
    std::fprintf(stderr, "MaterialX standard aliases failed: %s\n",
                 aliasesError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU randomColorMat;
  randomColorMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"randomColor","category":"randomcolor","type":"color3","inputs":[
      {"name":"in","type":"float","value":0.25},{"name":"seed","value":7},
      {"name":"huelow","value":0.1},{"name":"huehigh","value":0.2},
      {"name":"saturationlow","value":0.5},{"name":"saturationhigh","value":0.6},
      {"name":"brightnesslow","value":0.7},{"name":"brightnesshigh","value":0.8}
    ]}],"outputs":[{"name":"base","nodename":"randomColor"}]},
    "connections":[{"input":"base_color","output":"base"}]})json";
  std::string randomColorError;
  if (!lusdview::CompileMaterialXGraphRuntime(&randomColorMat,
                                               &randomColorError) ||
      randomColorMat.materialXGraph.nodes.size() != 26 ||
      randomColorMat.materialXGraph.nodes.back().op !=
          lusdview::MaterialXGraphOpCPU::HsvToRgb) {
    std::fprintf(stderr, "MaterialX randomcolor lowering failed: %s\n",
                 randomColorError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU fractal2dMat;
  fractal2dMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"scalar","category":"fractal2d","type":"float","inputs":[
      {"name":"amplitude","value":1.5},{"name":"octaves","value":3},
      {"name":"lacunarity","value":2},{"name":"diminish","value":0.5},
      {"name":"texcoord","value":[0.2,0.4]}]},
    {"name":"color","category":"fractal2d","type":"color3","inputs":[
      {"name":"amplitude","value":[1,2,3]},{"name":"octaves","value":3},
      {"name":"lacunarity","value":2},{"name":"diminish","value":0.5},
      {"name":"texcoord","value":[0.2,0.4]}]}
  ],"outputs":[]},"connections":[]})json";
  std::string fractal2dError;
  if (!lusdview::CompileMaterialXGraphRuntime(&fractal2dMat,
                                               &fractal2dError) ||
      fractal2dMat.materialXGraph.nodes.size() != 4 ||
      fractal2dMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::Fractal2D ||
      fractal2dMat.materialXGraph.nodes[2].op !=
          lusdview::MaterialXGraphOpCPU::Fractal2D ||
      fractal2dMat.materialXGraph.nodes[0].auxValue[3] != 1.0f ||
      fractal2dMat.materialXGraph.nodes[2].auxValue[3] != 3.0f) {
    std::fprintf(stderr, "MaterialX fractal2d lowering failed: %s\n",
                 fractal2dError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU fractal3dMat;
  fractal3dMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"volumeNoise","category":"fractal3d","type":"color3","inputs":[
      {"name":"position","value":[0.2,0.4,0.7]},
      {"name":"amplitude","value":[1,2,3]},{"name":"octaves","value":4},
      {"name":"lacunarity","value":2},{"name":"diminish","value":0.5}]}
  ],"outputs":[]},"connections":[]})json";
  std::string fractal3dError;
  if (!lusdview::CompileMaterialXGraphRuntime(&fractal3dMat,
                                               &fractal3dError) ||
      fractal3dMat.materialXGraph.nodes.size() != 2 ||
      fractal3dMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::Fractal3D ||
      fractal3dMat.materialXGraph.nodes[0].auxValue[3] != 3.0f) {
    std::fprintf(stderr, "MaterialX fractal3d lowering failed: %s\n",
                 fractal3dError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU unifiedNoiseMat;
  unifiedNoiseMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"unified2","category":"unifiednoise2d","type":"float","inputs":[
      {"name":"texcoord","value":[0.2,0.4]},{"name":"freq","value":[2,3]},
      {"name":"offset","value":[0.1,0.2]},{"name":"jitter","value":0.8},
      {"name":"outmin","value":-1},{"name":"outmax","value":2},
      {"name":"clampoutput","value":true},{"name":"octaves","value":3},
      {"name":"lacunarity","value":2},{"name":"diminish","value":0.5},
      {"name":"type","value":2},{"name":"style","value":0}]},
    {"name":"unified3","category":"unifiednoise3d","type":"float","inputs":[
      {"name":"position","value":[0.2,0.4,0.7]},{"name":"type","value":3}]}
  ],"outputs":[]},"connections":[]})json";
  std::string unifiedNoiseError;
  if (!lusdview::CompileMaterialXGraphRuntime(&unifiedNoiseMat,
                                               &unifiedNoiseError) ||
      unifiedNoiseMat.materialXGraph.nodes.size() >
          lusdview::kRtMaterialGraphMaxNodes ||
      unifiedNoiseMat.materialXGraph.nodes.empty() ||
      unifiedNoiseMat.materialXGraph.nodes.back().op !=
          lusdview::MaterialXGraphOpCPU::IfEqual) {
    std::fprintf(stderr, "MaterialX unified-noise lowering failed: %s\n",
                 unifiedNoiseError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU shapeMat;
  shapeMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"clover","category":"cloverleaf","type":"float","inputs":[{"name":"texcoord","value":[0.5,0.6]},{"name":"center","value":[0.5,0.5]},{"name":"radius","value":0.4}]},
    {"name":"hex","category":"hexagon","type":"float","inputs":[{"name":"texcoord","value":[0.5,0.5]},{"name":"center","value":[0.5,0.5]},{"name":"radius","value":0.4}]}
  ],"outputs":[]},"connections":[]})json";
  std::string shapeError;
  if (!lusdview::CompileMaterialXGraphRuntime(&shapeMat,&shapeError) ||
      shapeMat.materialXGraph.nodes.size()!=2 ||
      shapeMat.materialXGraph.nodes[0].op!=lusdview::MaterialXGraphOpCPU::Cloverleaf ||
      shapeMat.materialXGraph.nodes[1].op!=lusdview::MaterialXGraphOpCPU::Hexagon) {
    std::fprintf(stderr,"MaterialX shape lowering failed: %s\n",shapeError.c_str());return 1;
  }
  lusdview::DrawMaterialCPU tiledPatternMat;
  tiledPatternMat.materialXNodeGraphJson=R"json({"nodegraph":{"nodes":[
    {"name":"grid","category":"grid","type":"color3","inputs":[{"name":"texcoord","value":[0.1,0.2]},{"name":"thickness","value":0.1},{"name":"staggered","value":true}]},
    {"name":"cross","category":"crosshatch","type":"color3","inputs":[{"name":"texcoord","value":[0.1,0.2]}]},
    {"name":"circles","category":"tiledcircles","type":"color3","inputs":[{"name":"texcoord","value":[0.1,0.2]}]},
    {"name":"clovers","category":"tiledcloverleafs","type":"color3","inputs":[{"name":"texcoord","value":[0.1,0.2]}]},
    {"name":"hexagons","category":"tiledhexagons","type":"color3","inputs":[{"name":"texcoord","value":[0.1,0.2]}]}
  ],"outputs":[]},"connections":[]})json";
  std::string tiledPatternError;
  if(!lusdview::CompileMaterialXGraphRuntime(&tiledPatternMat,&tiledPatternError)||
     tiledPatternMat.materialXGraph.nodes.size()!=15||
     tiledPatternMat.materialXGraph.nodes[0].op!=lusdview::MaterialXGraphOpCPU::Grid||
     tiledPatternMat.materialXGraph.nodes[3].op!=lusdview::MaterialXGraphOpCPU::Crosshatch||
     tiledPatternMat.materialXGraph.nodes[6].op!=lusdview::MaterialXGraphOpCPU::TiledCircles||
     tiledPatternMat.materialXGraph.nodes[9].op!=lusdview::MaterialXGraphOpCPU::TiledCloverleafs||
     tiledPatternMat.materialXGraph.nodes[12].op!=lusdview::MaterialXGraphOpCPU::TiledHexagons){std::fprintf(stderr,"MaterialX tiled-pattern lowering failed: %s\n",tiledPatternError.c_str());return 1;}
  lusdview::DrawMaterialCPU compositeMat;
  compositeMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"difference","category":"difference","type":"color3","inputs":[{"name":"fg","value":[0.8,0.1,0.4]},{"name":"bg","value":[0.2,0.5,0.1]}]},
    {"name":"in","category":"in","type":"color4","inputs":[]},
    {"name":"mask","category":"mask","type":"color4","inputs":[]},
    {"name":"matte","category":"matte","type":"color4","inputs":[]},
    {"name":"out","category":"out","type":"color4","inputs":[]},
    {"name":"over","category":"over","type":"color4","inputs":[]},
    {"name":"disjoint","category":"disjointover","type":"color4","inputs":[]}
  ],"outputs":[]},"connections":[]})json";
  std::string compositeError;
  if (!lusdview::CompileMaterialXGraphRuntime(&compositeMat, &compositeError) ||
      compositeMat.materialXGraph.nodes.size() != 7 ||
      compositeMat.materialXGraph.nodes[0].op !=
          lusdview::MaterialXGraphOpCPU::Difference ||
      compositeMat.materialXGraph.nodes[6].op !=
          lusdview::MaterialXGraphOpCPU::DisjointOver) {
    std::fprintf(stderr, "MaterialX compositing operators failed: %s\n",
                 compositeError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU colorCorrectMat;
  colorCorrectMat.materialXNodeGraphJson = R"json({"nodegraph":{"nodes":[
    {"name":"source","category":"constant","type":"color4","inputs":[
      {"name":"value","value":[0.5,0.25,0.75,0.3]}]},
    {"name":"cc","category":"colorcorrect","type":"color4","inputs":[
      {"name":"in","nodename":"source"},
      {"name":"gamma","value":2},{"name":"lift","value":0.1},
      {"name":"gain","value":0.8},{"name":"exposure","value":1}
    ]}],"outputs":[{"name":"base","nodename":"cc"}]},
    "connections":[{"input":"base_color","output":"base"}]})json";
  std::string colorCorrectError;
  if (!lusdview::CompileMaterialXGraphRuntime(&colorCorrectMat,
                                               &colorCorrectError) ||
      colorCorrectMat.materialXGraph.nodes.size() != 18 ||
      colorCorrectMat.materialXGraph.nodes.back().op !=
          lusdview::MaterialXGraphOpCPU::SetAlpha) {
    std::fprintf(stderr, "MaterialX colorcorrect lowering failed: %s\n",
                 colorCorrectError.c_str());
    return 1;
  }
  int colorSource = -1, colorHsv = -1, colorGamma = -1;
  for (size_t i = 0; i < colorCorrectMat.materialXGraph.nodes.size(); ++i) {
    const auto& graphNode = colorCorrectMat.materialXGraph.nodes[i];
    if (graphNode.name == "source") colorSource = static_cast<int>(i);
    if (graphNode.name == "cc__hsv") colorHsv = static_cast<int>(i);
    if (graphNode.name == "cc__gamma_reciprocal") colorGamma = static_cast<int>(i);
  }
  if (colorSource < 0 || colorHsv < 0 || colorGamma < 0 ||
      colorCorrectMat.materialXGraph.nodes[colorHsv].input[0] != colorSource ||
      colorCorrectMat.materialXGraph.nodes[colorGamma].value[1][0] != 2.0f ||
      colorCorrectMat.materialXGraph.nodes[colorGamma].value[1][1] != 2.0f ||
      colorCorrectMat.materialXGraph.nodes[colorGamma].value[1][2] != 2.0f) {
    std::fprintf(stderr,
                 "MaterialX colorcorrect connection/scalar promotion was lost\n");
    return 1;
  }

  // MaterialX connections may reference a node authored later. GPU graph
  // evaluators are single-pass, so compilation must pack dependencies first.
  lusdview::DrawMaterialCPU forwardGraphMat;
  forwardGraphMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"consumer", "category":"invert", "type":"color3",
       "inputs":[{"name":"in", "nodename":"source"}]},
      {"name":"source", "category":"constant", "type":"color3",
       "inputs":[{"name":"value", "value":[0.2,0.4,0.8]}]}
    ], "outputs":[{"name":"base", "nodename":"consumer"}]},
    "connections":[{"input":"base_color", "output":"base"}]
  })json";
  std::string forwardError;
  if (!lusdview::CompileMaterialXGraphRuntime(&forwardGraphMat, &forwardError) ||
      forwardGraphMat.materialXGraph.nodes.size() != 2 ||
      forwardGraphMat.materialXGraph.nodes[0].name != "source" ||
      forwardGraphMat.materialXGraph.nodes[1].name != "consumer" ||
      forwardGraphMat.materialXGraph.nodes[1].input[0] != 0 ||
      forwardGraphMat.materialXGraph.output[0] != 1) {
    std::fprintf(stderr, "MaterialX dependency ordering failed: %s\n",
                 forwardError.c_str());
    return 1;
  }

  // A surface_unlit shader may be the nodegraph output itself rather than a
  // closure connected through a bsdf/edf terminal.  Lower its direct schema
  // inputs into the same OpenPBR lanes and keep the graph output mapping.
  lusdview::DrawMaterialCPU unlitRuntimeMat;
  unlitRuntimeMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"unlit","category":"surface_unlit","type":"surfaceshader",
       "inputs":[{"name":"emission","type":"float","value":2.5},
                 {"name":"emission_color","type":"color3","value":[0.2,0.4,0.8]},
                 {"name":"transmission","type":"float","value":0.35},
                 {"name":"transmission_color","type":"color3","value":[0.8,0.6,0.4]},
                 {"name":"opacity","type":"float","value":0.65}]}
    ],"outputs":[
      {"name":"e","nodename":"unlit"},{"name":"ec","nodename":"unlit"},
      {"name":"t","nodename":"unlit"},{"name":"tc","nodename":"unlit"},
      {"name":"o","nodename":"unlit"}]},
    "connections":[{"input":"emission","output":"e"},
      {"input":"emission_color","output":"ec"},
      {"input":"transmission","output":"t"},
      {"input":"transmission_color","output":"tc"},
      {"input":"opacity","output":"o"}]
  })json";
  std::string unlitRuntimeError;
  if (!lusdview::CompileMaterialXGraphRuntime(&unlitRuntimeMat,
                                               &unlitRuntimeError) ||
      unlitRuntimeMat.materialXGraph.output[44] < 0 ||
      unlitRuntimeMat.materialXGraph.output[4] < 0 ||
      unlitRuntimeMat.materialXGraph.output[11] < 0 ||
      unlitRuntimeMat.materialXGraph.output[12] < 0 ||
      unlitRuntimeMat.materialXGraph.output[3] < 0) {
    std::fprintf(stderr, "surface_unlit runtime lowering failed: %s\n",
                 unlitRuntimeError.c_str());
    return 1;
  }

  // Radius scale is vector-valued even though the bounded ABI exposes only a
  // scalar subsurface-scale lane. It must be folded into the vector radius
  // route, not truncated to its first component.
  lusdview::DrawMaterialCPU radiusScaleGraphMat;
  radiusScaleGraphMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"radius","category":"constant","type":"color3",
       "inputs":[{"name":"value","value":[0.3,0.5,0.7]}]},
      {"name":"scale","category":"constant","type":"color3",
       "inputs":[{"name":"value","value":[2.0,3.0,4.0]}]}
    ],"outputs":[{"name":"r","nodename":"radius"},
      {"name":"s","nodename":"scale"}]},
    "connections":[{"input":"subsurface_radius","output":"r"},
      {"input":"subsurface_radius_scale","output":"s"}]
  })json";
  std::string radiusScaleError;
  if (!lusdview::CompileMaterialXGraphRuntime(&radiusScaleGraphMat,
                                               &radiusScaleError) ||
      radiusScaleGraphMat.materialXGraph.output[8] < 0 ||
      radiusScaleGraphMat.materialXGraph.output[25] >= 0 ||
      radiusScaleGraphMat.materialXGraph.nodes.size() != 3) {
    std::fprintf(stderr, "subsurface radius-scale graph lowering failed: %s\n",
                 radiusScaleError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU mixedRadiusScaleMat;
  mixedRadiusScaleMat.hasOpenPBRSurface = true;
  mixedRadiusScaleMat.params.push_back(FloatParam("subsurface_radius", 0.5f));
  mixedRadiusScaleMat.params.push_back(FloatParam("subsurface_scale", 2.0f));
  mixedRadiusScaleMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"scale","category":"constant","type":"color3",
       "inputs":[{"name":"value","value":[2.0,3.0,4.0]}]}
    ],"outputs":[{"name":"s","nodename":"scale"}]},
    "connections":[{"input":"subsurface_radius_scale","output":"s"}]
  })json";
  lusdview::BakeRealtimePbrMaterial(&mixedRadiusScaleMat);
  if (!Near(mixedRadiusScaleMat.lightRtOpenPBR.subsurfaceRadius[0], 1.0f) ||
      !Near(mixedRadiusScaleMat.lightRtOpenPBR.subsurfaceRadius[1], 1.5f) ||
      !Near(mixedRadiusScaleMat.lightRtOpenPBR.subsurfaceRadius[2], 2.0f) ||
      !Near(mixedRadiusScaleMat.lightRtOpenPBR.subsurfaceScale, 2.0f)) {
    return 1;
  }

  // Four-input MaterialX conditionals use the auxiliary graph lane for in2.
  // Keep both a forward-connected branch and a literal fallback covered.
  lusdview::DrawMaterialCPU conditionalMat;
  conditionalMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"greater", "category":"ifgreater", "type":"color3",
       "inputs":[{"name":"value1","value":2.0},{"name":"value2","value":1.0},
                 {"name":"in1","value":[1.0,0.0,0.0]},
                 {"name":"in2","nodename":"other"}]},
      {"name":"other", "category":"constant", "type":"color3",
       "inputs":[{"name":"value","value":[0.0,0.0,1.0]}]},
      {"name":"equal", "category":"ifequal", "type":"color3",
       "inputs":[{"name":"value1","value":0.0},{"name":"value2","value":1.0},
                 {"name":"in1","value":[0.0,1.0,0.0]},
                 {"name":"in2","value":[0.2,0.4,0.8]}]}
    ], "outputs": []}, "connections": []
  })json";
  std::string conditionalError;
  if (!lusdview::CompileMaterialXGraphRuntime(&conditionalMat,
                                               &conditionalError) ||
      conditionalMat.materialXGraph.nodes.size() != 3 ||
      conditionalMat.materialXGraph.nodes[1].op !=
          lusdview::MaterialXGraphOpCPU::IfGreater ||
      conditionalMat.materialXGraph.nodes[1].auxInput != 0 ||
      conditionalMat.materialXGraph.nodes[2].op !=
          lusdview::MaterialXGraphOpCPU::IfEqual ||
      conditionalMat.materialXGraph.nodes[2].auxInput != -1 ||
      !Near(conditionalMat.materialXGraph.nodes[2].auxValue[2], 0.8f)) {
    std::fprintf(stderr, "MaterialX four-input conditionals failed: %s\n",
                 conditionalError.c_str());
    return 1;
  }
  std::vector<float> conditionalPack(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(conditionalMat, conditionalPack.data());
  const size_t greaterPack = lusdview::kRtMaterialGraphHeaderFloats +
      lusdview::kRtMaterialGraphNodeFloats;
  const size_t equalPack = greaterPack + lusdview::kRtMaterialGraphNodeFloats;
  if (!Near(conditionalPack[greaterPack + 16], 0.0f) ||
      !Near(conditionalPack[equalPack + 16], -1.0f) ||
      !Near(conditionalPack[equalPack + 19], 0.8f)) {
    std::fprintf(stderr, "MaterialX conditional auxiliary packing failed\n");
    return 1;
  }

  // Matrix geomprops are transported as one interpolated 9/16-component
  // stream and exposed to the bounded matrix operators one column at a time.
  lusdview::DrawMaterialCPU matrixGeompropMat;
  matrixGeompropMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"M","category":"geompropvalue","type":"matrix44",
       "inputs":[{"name":"geomprop","value":"M"}]},
      {"name":"x","category":"transformmatrix","type":"vector3",
       "inputs":[{"name":"in","value":[1,2,3]},
                 {"name":"mat","nodename":"M","type":"matrix44"}]}
    ],"outputs":[]},"connections":[]})json";
  std::string matrixGeompropError;
  if (!lusdview::CompileMaterialXGraphRuntime(&matrixGeompropMat,
                                               &matrixGeompropError) ||
      matrixGeompropMat.materialXGraph.nodes.size() != 5) {
    std::fprintf(stderr, "matrix geomprop graph lowering failed (%zu): %s\n",
                 matrixGeompropMat.materialXGraph.nodes.size(),
                 matrixGeompropError.c_str());
    return 1;
  }
  std::vector<float> matrixPack(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(matrixGeompropMat, matrixPack.data());
  int matrixColumns = 0;
  for (size_t i = 0; i < matrixGeompropMat.materialXGraph.nodes.size(); ++i) {
    const auto& node = matrixGeompropMat.materialXGraph.nodes[i];
    if (node.op != lusdview::MaterialXGraphOpCPU::GeomProp) continue;
    const size_t nodeOffset = lusdview::kRtMaterialGraphHeaderFloats +
        i * lusdview::kRtMaterialGraphNodeFloats;
    if (!Near(matrixPack[nodeOffset + 18], 16.0f) ||
        !Near(matrixPack[nodeOffset + 19],
              static_cast<float>(matrixColumns))) {
      std::fprintf(stderr, "matrix geomprop column packing failed\n");
      return 1;
    }
    ++matrixColumns;
  }
  if (matrixColumns != 4) {
    std::fprintf(stderr, "matrix geomprop columns were not emitted\n");
    return 1;
  }

  lusdview::DrawMaterialCPU cyclicMat;
  cyclicMat.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"a","category":"add","inputs":[{"nodename":"b"},{"value":1}]},
      {"name":"b","category":"multiply","inputs":[{"nodename":"a"},{"value":2}]}
    ],"outputs":[]},"connections":[]})json";
  std::string malformedError;
  if (lusdview::CompileMaterialXGraphRuntime(&cyclicMat, &malformedError) ||
      malformedError.find("cycle") == std::string::npos) {
    std::fprintf(stderr, "cyclic MaterialX graph was not rejected: %s\n",
                 malformedError.c_str());
    return 1;
  }
  lusdview::DrawMaterialCPU oversizedMat;
  oversizedMat.materialXNodeGraphJson = "{\"nodegraph\":{\"nodes\":[";
  for (int i = 0; i <= lusdview::kRtMaterialGraphMaxNodes; ++i) {
    if (i) oversizedMat.materialXNodeGraphJson += ',';
    oversizedMat.materialXNodeGraphJson +=
        "{\"name\":\"n" + std::to_string(i) +
        "\",\"category\":\"constant\",\"inputs\":[{\"value\":1}]}";
  }
  oversizedMat.materialXNodeGraphJson += "],\"outputs\":[]},\"connections\":[]}";
  malformedError.clear();
  if (lusdview::CompileMaterialXGraphRuntime(&oversizedMat, &malformedError) ||
      malformedError.find("64-node") == std::string::npos) {
    std::fprintf(stderr, "oversized MaterialX graph was not rejected: %s\n",
                 malformedError.c_str());
    return 1;
  }

  // Connected image coordinates must survive compilation and packing. This
  // catches the old raster/RT behavior where image nodes always sampled the
  // hit UV even when a place2d/transform node was authored upstream.
  lusdview::DrawMaterialCPU placedImageMat;
  placedImageMat.materialXNodeGraphJson = R"json({
    "nodegraph": {"nodes": [
      {"name":"st", "category":"texcoord1", "type":"vector2"},
      {"name":"place", "category":"place2d", "type":"vector2",
       "inputs":[{"name":"in", "nodename":"st"},
                  {"name":"scale", "value":[2.0,3.0]},
                  {"name":"offset", "value":[0.1,0.2]},
                  {"name":"rotation", "value":30.0}]},
      {"name":"img", "category":"image", "type":"color3",
       "inputs":[{"name":"file", "type":"filename", "value":"x.png"},
                  {"name":"texcoord", "nodename":"place"},
                  {"name":"default", "value":[0.2,0.3,0.4]}]}
    ], "outputs":[{"name":"base", "nodename":"img"}]},
    "connections":[{"input":"base_color", "nodegraph":"nodegraph",
                     "output":"base"}]
  })json";
  std::string placedError;
  if (!lusdview::CompileMaterialXGraphRuntime(&placedImageMat, &placedError) ||
      placedImageMat.materialXGraph.nodes.size() != 3 ||
      placedImageMat.materialXGraph.nodes[1].op !=
          lusdview::MaterialXGraphOpCPU::Transform2D ||
      placedImageMat.materialXGraph.nodes[0].value[2][2] != 1.0f ||
      placedImageMat.materialXGraph.nodes[2].value[2][3] != 0.0f) {
    std::fprintf(stderr, "MaterialX placed image graph failed: %s\n",
                 placedError.c_str());
    return 1;
  }
  std::vector<float> placedPack(lusdview::kRtMaterialGraphFloats, 0.0f);
  lusdview::PackMaterialXGraphRuntime(placedImageMat, placedPack.data());
  const size_t placedNode = lusdview::kRtMaterialGraphHeaderFloats +
                            2 * lusdview::kRtMaterialGraphNodeFloats;
  const size_t placedTransform = lusdview::kRtMaterialGraphHeaderFloats +
                                 lusdview::kRtMaterialGraphNodeFloats;
  if (placedPack[placedNode] !=
          static_cast<float>(lusdview::MaterialXGraphOpCPU::Image) ||
      placedPack[placedNode + 15] != 0.0f ||
      !Near(placedPack[placedTransform + 15], 30.0f)) {
    std::fprintf(stderr, "MaterialX placed image graph packing failed\n");
    return 1;
  }

  // Image evaluation must use the owning asset directory.  This exercises
  // the real vendored texture cache (rather than the historical no-image
  // stub) while keeping the graph's live texture lane marked as a runtime
  // dependency in BakeRealtimePbrMaterial.
  const char* imageXml =
      "<materialx version=\"1.39\">"
      "<nodegraph name=\"NG_asset_image\">"
      "<image name=\"img\" type=\"color3\">"
      "<input name=\"file\" type=\"filename\" value=\"checkerboard.png\"/>"
      "<input name=\"default\" type=\"color3\" value=\"0.123,0.234,0.345\"/>"
      "</image>"
      "<output name=\"base_out\" type=\"color3\" nodename=\"img\"/>"
      "</nodegraph>"
      "<open_pbr_surface name=\"surface\" type=\"surfaceshader\">"
      "<input name=\"base_color\" type=\"color3\" nodegraph=\"NG_asset_image\" output=\"base_out\"/>"
      "</open_pbr_surface>"
      "<surfacematerial name=\"material\" type=\"material\">"
      "<input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"surface\"/>"
      "</surfacematerial>"
      "</materialx>";
  lightusd::tydra::LightRtOpenPBRParams imageParams{};
  std::string imageErr;
  const std::string materialTextureDir = MaterialTextureDir();
  if (materialTextureDir.empty()) {
    std::fprintf(stderr, "MaterialX image fixture directory was not found\n");
    return 1;
  }
  if (!lusdview::EvaluateMaterialXStringToLightRtOpenPBRWithBaseDir(
          imageXml, "material", materialTextureDir.c_str(), &imageParams,
          &imageErr)) {
    std::fprintf(stderr, "asset-relative MaterialX image eval failed: %s\n",
                 imageErr.c_str());
    return 1;
  }
  if (Near(imageParams.baseColor[0], 0.123f, 1.0e-3f) &&
      Near(imageParams.baseColor[1], 0.234f, 1.0e-3f) &&
      Near(imageParams.baseColor[2], 0.345f, 1.0e-3f)) {
    std::fprintf(stderr,
                 "asset-relative MaterialX image unexpectedly used default\n");
    return 1;
  }
  lightusd::tydra::LightRtOpenPBRParams uvImageParams{};
  if (!lusdview::EvaluateMaterialXStringToLightRtOpenPBRAtUv(
          imageXml, "material", materialTextureDir.c_str(), 0.13f, 0.87f,
          &uvImageParams, &imageErr) ||
      !std::isfinite(uvImageParams.baseColor[0]) ||
      !std::isfinite(uvImageParams.baseColor[1]) ||
      !std::isfinite(uvImageParams.baseColor[2])) {
    std::fprintf(stderr, "UV-aware MaterialX image eval failed: %s\n",
                 imageErr.c_str());
    return 1;
  }

  lusdview::DrawScene graphBakeScene;
  lusdview::DrawMaterialCPU graphBakeMat = imageGraphMat;
  graphBakeMat.absPath =
      (std::filesystem::path(materialTextureDir) / "material.usda").string();
  const std::string missingName = "missing_base.png";
  const size_t missingPos = graphBakeMat.materialXNodeGraphJson.find(missingName);
  if (missingPos != std::string::npos) {
    graphBakeMat.materialXNodeGraphJson.replace(
        missingPos, missingName.size(), "checkerboard.png");
  }
  lusdview::BakeMaterialXGraphTextures(&graphBakeMat, &graphBakeScene);
  const bool hasBakedGraphTexture = std::any_of(
      graphBakeMat.materialXGraph.nodes.begin(),
      graphBakeMat.materialXGraph.nodes.end(),
      [](const lusdview::MaterialXGraphNodeCPU& node) {
        return node.textureId >= 0;
      });
  if (graphBakeMat.baseColorTex < 0 || graphBakeScene.textures.size() != 1 ||
      graphBakeScene.textures[0].image.width != 16 ||
      graphBakeScene.textures[0].image.height != 16 ||
      !hasBakedGraphTexture) {
    std::fprintf(stderr,
                 "MaterialX graph texture bake did not produce a map (base=%d textures=%zu nodeTex=%d)\n",
                 graphBakeMat.baseColorTex, graphBakeScene.textures.size(),
                 hasBakedGraphTexture ? 1 : -1);
    return 1;
  }

  lusdview::DrawMaterialCPU fixedIdGraphMat;
  fixedIdGraphMat.name = "FixedIdGraph";
  fixedIdGraphMat.hasOpenPBRSurface = true;
  fixedIdGraphMat.materialXNodeGraphJson = R"json({
    "version": "1.39",
    "nodegraph": {
      "name": "NG_fixed_id_graph",
      "nodes": [
        {
          "name": "tint_mul",
          "category": "MaterialXMultiply",
          "type": "ND_multiply_color3",
          "inputs": [
            {"name": "in1", "type": "color3", "value": [0.2, 0.4, 0.6]},
            {"name": "in2", "type": "color3", "value": [2.0, 0.5, 0.25]}
          ]
        }
      ],
      "outputs": [
        {
          "name": "base_color_output",
          "type": "color3",
          "nodename": "tint_mul",
          "output": "out"
        }
      ]
    },
    "connections": [
      {
        "input": "base_color",
        "nodegraph": "NG_fixed_id_graph",
        "output": "base_color_output"
      }
    ]
  })json";
  lusdview::BakeLightRtOpenPBR(&fixedIdGraphMat);
  if (!fixedIdGraphMat.hasLightRtOpenPBR ||
      !Near(fixedIdGraphMat.lightRtOpenPBR.baseColor[0], 0.4f) ||
      !Near(fixedIdGraphMat.lightRtOpenPBR.baseColor[1], 0.2f) ||
      !Near(fixedIdGraphMat.lightRtOpenPBR.baseColor[2], 0.15f)) {
    std::fprintf(stderr,
                 "MaterialX fixed info:id category graph was not normalized\n");
    return 1;
  }

  lusdview::DrawMaterialCPU normalGraphMat;
  normalGraphMat.name = "NormalGraph";
  normalGraphMat.hasOpenPBRSurface = true;
  normalGraphMat.materialXNodeGraphJson = R"json({
    "version": "1.39",
    "nodegraph": {
      "name": "NG_normal_graph",
      "nodes": [
        {
          "name": "normal_img",
          "category": "image",
          "type": "vector3",
          "inputs": [
            {"name": "file", "type": "filename", "value": "missing_normal.png"},
            {"name": "default", "type": "vector3", "value": [0.5, 0.5, 1.0]}
          ]
        },
        {
          "name": "normal_map",
          "category": "normalmap",
          "type": "vector3",
          "inputs": [
            {
              "name": "in",
              "type": "vector3",
              "nodename": "normal_img",
              "output": "out"
            }
          ]
        }
      ],
      "outputs": [
        {
          "name": "normal_output",
          "type": "vector3",
          "nodename": "normal_map",
          "output": "out"
        }
      ]
    },
    "connections": [
      {
        "input": "normal",
        "nodegraph": "NG_normal_graph",
        "output": "normal_output"
      }
    ]
  })json";
  lusdview::BakeLightRtOpenPBR(&normalGraphMat);
  if (!normalGraphMat.hasLightRtOpenPBR ||
      !normalGraphMat.lightRtOpenPBR.hasTextureInputs ||
      !normalGraphMat.lightRtOpenPBR.hasNormalInput) {
    std::fprintf(stderr,
                 "MaterialX normalmap graph was not marked as normal texture\n");
    return 1;
  }
  if (!Near(normalGraphMat.lightRtOpenPBR.normal[0], 0.0f) ||
      !Near(normalGraphMat.lightRtOpenPBR.normal[1], 0.0f) ||
      !Near(normalGraphMat.lightRtOpenPBR.normal[2], 1.0f)) {
    std::fprintf(stderr,
                 "MaterialX normalmap default was incorrectly constant-baked\n");
    return 1;
  }

  // UsdPreviewSurface keeps its authored fallback parameters in `params` even
  // when the corresponding input is texture-driven. The canonical LightRT
  // pack must use neutral factors for those lanes, matching the raster fields;
  // Vulkan RT and CUDA consume this pack directly.
  lusdview::DrawMaterialCPU previewTextureMat;
  previewTextureMat.hasUsdPreviewSurface = true;
  previewTextureMat.baseColorTex = 0;
  previewTextureMat.metallicTex = 1;
  previewTextureMat.roughnessTex = 2;
  previewTextureMat.emissiveTex = 3;
  previewTextureMat.opacityTex = 4;
  previewTextureMat.specularColorTex = 5;
  previewTextureMat.coatWeightTex = 6;
  previewTextureMat.coatColorTex = 7;
  previewTextureMat.coatRoughnessTex = 8;
  previewTextureMat.params.push_back(
      Vec3Param("diffuseColor", 0.2f, 0.3f, 0.4f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(FloatParam("metallic", 0.0f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(FloatParam("roughness", 0.2f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(
      Vec3Param("emissiveColor", 0.0f, 0.0f, 0.0f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(FloatParam("opacity", 0.1f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(
      Vec3Param("specularColor", 0.04f, 0.04f, 0.04f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(FloatParam("clearcoat", 0.0f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  previewTextureMat.params.push_back(FloatParam("clearcoatRoughness", 0.1f));
  previewTextureMat.params.back().shader = "UsdPreviewSurface";
  lusdview::BakeRealtimePbrMaterial(&previewTextureMat);
  const auto& previewPbr = previewTextureMat.lightRtOpenPBR;
  if (!Near(previewPbr.baseColor[0], 1.0f) ||
      !Near(previewPbr.metalness, 1.0f) ||
      !Near(previewPbr.specularRoughness, 1.0f) ||
      !Near(previewPbr.emissionColor[0], 1.0f) ||
      !Near(previewPbr.emission, 1.0f) || !Near(previewPbr.opacity, 1.0f) ||
      !Near(previewPbr.specularColor[0], 1.0f) ||
      !Near(previewPbr.coatWeight, 1.0f) ||
      !Near(previewPbr.coatColor[0], 1.0f) ||
      !Near(previewPbr.coatRoughness, 1.0f)) {
    std::fprintf(stderr,
                 "UsdPreviewSurface texture factors were not neutralized in LightRT\n");
    return 1;
  }

  lusdview::DrawMaterialCPU standardRoughnessTextureMat;
  standardRoughnessTextureMat.hasOpenPBRSurface = true;
  standardRoughnessTextureMat.roughnessTex = 0;
  auto baseRoughness = FloatParam("base_roughness", 0.0f);
  baseRoughness.shader = "OpenPBRSurface";
  standardRoughnessTextureMat.params.push_back(baseRoughness);
  auto specularRoughness = FloatParam("specular_roughness", 0.5f);
  specularRoughness.shader = "OpenPBRSurface";
  specularRoughness.texture = 0;
  standardRoughnessTextureMat.params.push_back(specularRoughness);
  lusdview::BakeRealtimePbrMaterial(&standardRoughnessTextureMat);
  if (!Near(standardRoughnessTextureMat.lightRtOpenPBR.specularRoughness,
            1.0f)) {
    std::fprintf(stderr,
                 "Textured Standard Surface roughness alias was not neutralized\n");
    return 1;
  }

  // The shared RT texture table must retain complete RGBA mip chains as
  // consecutive descriptors so Vulkan, CUDA, and HIP use the same trilinear
  // sampling ABI.
  lusdview::DrawTextureCPU mipTexture;
  mipTexture.image.width = 4;
  mipTexture.image.height = 4;
  mipTexture.image.channels = 4;
  mipTexture.image.data.assign(4 * 4 * 4, 255);
  light3d::Image mip2;
  mip2.width = 2;
  mip2.height = 2;
  mip2.channels = 4;
  mip2.data.assign(2 * 2 * 4, 128);
  light3d::Image mip1;
  mip1.width = 1;
  mip1.height = 1;
  mip1.channels = 4;
  mip1.data.assign(4, 64);
  mipTexture.mipImages = {mip2, mip1};
  lusdview::DrawMaterialCPU mipMaterial;
  mipMaterial.baseColorTex = 0;
  lusdview::HostTextureTable mipTable;
  lusdview::BuildHostTextureTable({mipTexture}, {mipMaterial}, &mipTable);
  if (mipTable.textures.size() != 3 || mipTable.texels.size() != 84 ||
      mipTable.sourceToTable.size() != 1 || mipTable.sourceToTable[0] != 0 ||
      mipTable.matTex.empty() || mipTable.matTex[0] != 0 ||
      mipTable.textures[0].mipCount != 3 ||
      mipTable.textures[0].firstMip != 1 ||
      mipTable.textures[1].mipCount != 2 ||
      mipTable.textures[1].firstMip != 2 ||
      mipTable.textures[2].mipCount != 1 ||
      mipTable.textures[2].firstMip != -1) {
    std::fprintf(stderr, "shared RT mip-chain packing is incorrect\n");
    return 1;
  }
  lusdview::DrawTextureCPU unusedTexture = mipTexture;
  unusedTexture.image.data.assign(4 * 4 * 4, 17);
  lusdview::HostTextureTable compactTable;
  lusdview::BuildHostTextureTable({mipTexture, unusedTexture}, {mipMaterial},
                                  &compactTable);
  if (compactTable.textures.size() != 3 || compactTable.texels.size() != 84 ||
      compactTable.sourceToTable.size() != 2 ||
      compactTable.sourceToTable[0] != 0 ||
      compactTable.sourceToTable[1] != -1 || compactTable.matTex.empty() ||
      compactTable.matTex[0] != 0) {
    std::fprintf(stderr, "shared RT unused-texture compaction is incorrect\n");
    return 1;
  }
  lusdview::DrawMaterialCPU duplicateMaterial = mipMaterial;
  duplicateMaterial.baseColorTex = 1;
  lusdview::HostTextureTable dedupTable;
  lusdview::BuildHostTextureTable({mipTexture, mipTexture},
                                  {mipMaterial, duplicateMaterial},
                                  &dedupTable);
  if (dedupTable.textures.size() != 3 || dedupTable.texels.size() != 84 ||
      dedupTable.sourceToTable.size() != 2 ||
      dedupTable.sourceToTable[0] != 0 || dedupTable.sourceToTable[1] != 0 ||
      dedupTable.matTex.size() < 2 * lusdview::kRtMaterialTexSlots ||
      dedupTable.matTex[lusdview::kRtMaterialTexSlots] != 0) {
    std::fprintf(stderr, "shared RT identical-texture deduplication is incorrect\n");
    return 1;
  }
  lusdview::HostTextureTable budgetTable;
  lusdview::BuildHostTextureTable({mipTexture}, {mipMaterial}, &budgetTable,
                                  nullptr, 32);
  if (!budgetTable.textures.empty() || !budgetTable.texels.empty() ||
      budgetTable.sourceToTable.size() != 1 ||
      budgetTable.sourceToTable[0] != -1 || budgetTable.matTex.empty() ||
      budgetTable.matTex[0] != -1) {
    std::fprintf(stderr, "shared RT texture byte budget is incorrect\n");
    return 1;
  }
  lusdview::DrawTextureCPU udimTexture;
  udimTexture.isUdim = true;
  lusdview::DrawUdimTileCPU tile;
  tile.udim = 1001;
  tile.image = mipTexture.image;
  tile.mipImages = mipTexture.mipImages;
  udimTexture.udimTiles.push_back(std::move(tile));
  // Preserve a sparse UDIM address: tile 1002 is intentionally absent while
  // 1003 retains a distinct mip chain. A dense remap would make shader lookup
  // sample the wrong tile for UV (2, 0).
  lusdview::DrawUdimTileCPU sparseTile;
  sparseTile.udim = 1003;
  sparseTile.image = mipTexture.image;
  sparseTile.mipImages = mipTexture.mipImages;
  udimTexture.udimTiles.push_back(std::move(sparseTile));
  lusdview::HostTextureTable udimTable;
  lusdview::BuildHostTextureTable({udimTexture}, {mipMaterial}, &udimTable);
  if (udimTable.textures.size() != 7 ||
      udimTable.textures[0].isUdim != 1 ||
      udimTable.textures[0].udimLayer[0] != 1 ||
      udimTable.textures[0].udimLayer[1] != -1 ||
      udimTable.textures[0].udimLayer[2] != 4 ||
      udimTable.textures[1].mipCount != 3 ||
      udimTable.textures[1].firstMip != 2 ||
      udimTable.textures[4].mipCount != 3 ||
      udimTable.textures[4].firstMip != 5) {
    std::fprintf(stderr, "shared RT UDIM mip-chain packing is incorrect\n");
    return 1;
  }
  lusdview::DrawMaterialCPU udimOpacityMaterial;
  udimOpacityMaterial.opacityTex = 0;
  lusdview::HostTextureTable udimOpacityTable;
  lusdview::BuildHostTextureTable({udimTexture}, {udimOpacityMaterial},
                                  &udimOpacityTable);
  if (udimOpacityTable.matTex.size() < lusdview::kRtMaterialTexSlots ||
      udimOpacityTable.matTex[5] != 0) {
    std::fprintf(stderr, "shared RT UDIM opacity-slot mapping is incorrect\n");
    return 1;
  }

  // Native compressed Ptex keeps its source slot so Vulkan RT can bind the
  // compressed VkImage directly, while the shared table still carries the
  // face-rectangle metadata used by CUDA/HIP and software-BVH fallback.
  lusdview::DrawTextureCPU directPtex;
  directPtex.isPtex = true;
  directPtex.image.width = 4;
  directPtex.image.height = 4;
  directPtex.image.channels = 4;
  directPtex.image.data.assign(4 * 4 * 4, 192);
  directPtex.ptexRectTexelOffset = 0;
  directPtex.ptexFaceRects.push_back({0, 0, 4, 4, 0, 0});
  directPtex.compressed.format = lusdview::DrawCompressedFormat::BC7;
  directPtex.compressed.width = 4;
  directPtex.compressed.height = 4;
  directPtex.compressed.data.assign(16, 0);
  directPtex.requestedCompressed = true;
  lusdview::DrawMaterialCPU directPtexMaterial;
  directPtexMaterial.baseColorTex = 0;
  lusdview::HostTextureTable directPtexTable;
  lusdview::BuildHostTextureTable({directPtex}, {directPtexMaterial},
                                  &directPtexTable);
  if (directPtexTable.textures.size() != 1 ||
      directPtexTable.sourceToTable.size() != 1 ||
      directPtexTable.sourceToTable[0] != 0 ||
      directPtexTable.textures[0].isPtex != 1 ||
      directPtexTable.textures[0].imageSlot != 0 ||
      directPtexTable.textures[0].ptexFaceCount != 1 ||
      directPtexTable.matTex.empty() || directPtexTable.matTex[0] != 0) {
    std::fprintf(stderr, "direct compressed Ptex RT metadata is incorrect\n");
    return 1;
  }

  // A separate displacement terminal remains present even when its resolved
  // scalar is exactly zero; do not collapse that authored output into the
  // absent-terminal case used by the geometry bake path.
  lusdview::DrawMaterialCPU zeroDisplacement;
  if (zeroDisplacement.hasDisplacement()) {
    std::fprintf(stderr, "uninitialized displacement was reported\n");
    return 1;
  }
  zeroDisplacement.hasDisplacementOutput = true;
  if (!zeroDisplacement.hasDisplacement()) {
    std::fprintf(stderr, "zero displacement terminal was dropped\n");
    return 1;
  }

  // Direct closure connections are lowered into the shared OpenPBR lanes.
  // Keep these graphs intentionally small: this verifies the Shader-to-Shader
  // connection shape used by imported MaterialX nodegraphs without relying on
  // a full surface wrapper.
  auto CheckDirectClosure = [](const char* json, const char* label,
                               const std::vector<int>& routes) {
    lusdview::DrawMaterialCPU material;
    material.materialXNodeGraphJson = json;
    std::string compileError;
    if (!lusdview::CompileMaterialXGraphRuntime(&material, &compileError) ||
        !material.materialXGraph.valid) {
      std::fprintf(stderr, "%s closure graph failed: %s\n", label,
                   compileError.c_str());
      return false;
    }
    for (int route : routes) {
      if (route < 0 || route >= lusdview::MaterialXGraphRuntimeCPU::kOutputCount ||
          material.materialXGraph.output[static_cast<size_t>(route)] < 0) {
        std::fprintf(stderr, "%s closure route %d was not emitted\n", label,
                     route);
        return false;
      }
    }
    std::vector<float> packed(lusdview::kRtMaterialGraphFloats, 0.0f);
    lusdview::PackMaterialXGraphRuntime(material, packed.data());
    for (int route : routes) {
      const size_t header = static_cast<size_t>(1 + route);
      if (!Near(packed[header], static_cast<float>(
          material.materialXGraph.output[static_cast<size_t>(route)]))) {
        std::fprintf(stderr, "%s packed route %d drifted\n", label, route);
        return false;
      }
    }
    return true;
  };
  auto CheckOnlyDirectClosureRoutes = [](const char* json, const char* label,
                                         const std::vector<int>& allowed) {
    lusdview::DrawMaterialCPU material;
    material.materialXNodeGraphJson = json;
    std::string compileError;
    if (!lusdview::CompileMaterialXGraphRuntime(&material, &compileError) ||
        !material.materialXGraph.valid) {
      std::fprintf(stderr, "%s closure graph failed: %s\n", label,
                   compileError.c_str());
      return false;
    }
    for (size_t routeIndex = 0;
         routeIndex < material.materialXGraph.output.size(); ++routeIndex) {
      const int route = material.materialXGraph.output[routeIndex];
      if (route >= 0 &&
          std::find(allowed.begin(), allowed.end(),
                    static_cast<int>(routeIndex)) == allowed.end()) {
        std::fprintf(stderr, "%s emitted unexpected output[%zu]=%d\n", label,
                     routeIndex, route);
        return false;
      }
    }
    return true;
  };
  auto CheckConicalClosure = []() {
    lusdview::DrawMaterialCPU material;
    material.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"cone","category":"conical_edf","type":"EDF","inputs":[
        {"name":"color","value":[0.2,0.4,0.8]},
        {"name":"normal","value":[0,0,1]},
        {"name":"inner_angle","value":20},
        {"name":"outer_angle","value":60}]}
    ],"outputs":[{"name":"shader","type":"EDF","nodename":"cone"}]},
    "connections":[{"input":"edf","output":"shader"}]})json";
    std::string error;
    if (!lusdview::CompileMaterialXGraphRuntime(&material, &error) ||
        !material.materialXGraph.valid ||
        material.materialXGraph.output[4] < 0) {
      std::fprintf(stderr, "conical EDF graph failed: %s\n", error.c_str());
      return false;
    }
    const bool hasCos = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.op == lusdview::MaterialXGraphOpCPU::Cosine;
        });
    const bool hasClamp = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.op == lusdview::MaterialXGraphOpCPU::Clamp;
        });
    const bool hasSmoothstep = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.op == lusdview::MaterialXGraphOpCPU::Smoothstep;
        });
    const bool hasHardCutoffBranch = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.op == lusdview::MaterialXGraphOpCPU::IfGreaterEqual;
        });
    const bool hasFalloffBranch = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.op == lusdview::MaterialXGraphOpCPU::IfGreater;
        });
    const bool hasAuthoredNormal = std::any_of(
        material.materialXGraph.nodes.begin(), material.materialXGraph.nodes.end(),
        [](const lusdview::MaterialXGraphNodeCPU& node) {
          return node.name == "cone__conical_normal" &&
                 node.op == lusdview::MaterialXGraphOpCPU::Convert;
        });
    if (!hasCos || !hasClamp || !hasSmoothstep || !hasHardCutoffBranch ||
        !hasFalloffBranch || !hasAuthoredNormal) {
      std::fprintf(stderr, "conical EDF angular falloff was flattened\n");
      return false;
    }
    return true;
  };
  auto CheckSafeVolumeAlbedo = []() {
    lusdview::DrawMaterialCPU material;
    material.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"fog","category":"absorption_vdf","type":"VDF",
       "inputs":[{"name":"absorption","value":[0,0,0]},
                 {"name":"scattering","value":[0,0,0]}]}
    ],"outputs":[{"name":"shader","type":"VDF","nodename":"fog"}]},
    "connections":[{"input":"vdf","output":"shader"}]})json";
    std::string error;
    if (!lusdview::CompileMaterialXGraphRuntime(&material, &error) ||
        !material.materialXGraph.valid) {
      std::fprintf(stderr, "safe volume albedo graph failed: %s\n",
                   error.c_str());
      return false;
    }
    const auto hasNode = [&](const char* name,
                             lusdview::MaterialXGraphOpCPU op) {
      return std::any_of(material.materialXGraph.nodes.begin(),
                          material.materialXGraph.nodes.end(),
                          [&](const lusdview::MaterialXGraphNodeCPU& node) {
                            return node.name == name && node.op == op;
                          });
    };
    if (!hasNode("fog__closure_safe_extinction",
                 lusdview::MaterialXGraphOpCPU::Maximum) ||
        !hasNode("fog__closure_volume_albedo",
                 lusdview::MaterialXGraphOpCPU::Divide)) {
      std::fprintf(stderr, "volume albedo denominator was not guarded\n");
      return false;
    }
    return true;
  };
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"w","category":"constant","type":"float","inputs":[{"name":"value","value":0.7}]},
      {"name":"c","category":"constant","type":"color3","inputs":[{"name":"value","value":[0.2,0.4,0.6]}]},
      {"name":"rough","category":"constant","type":"float","inputs":[{"name":"value","value":0.25}]},
      {"name":"diff","category":"diffuse_bsdf","type":"BSDF","inputs":[
        {"name":"weight","nodename":"w"},{"name":"color","nodename":"c"},
        {"name":"roughness","type":"vector2","value":[0.25,0.75]}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"diff"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "direct BSDF", {20, 0, 21}) ||
      !CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"emit","category":"uniform_edf","type":"EDF","inputs":[{"name":"color","value":[0.1,0.3,0.8]}]}
    ],"outputs":[{"name":"shader","type":"EDF","nodename":"emit"}]},
    "connections":[{"input":"edf","output":"shader"}]})json",
      "direct EDF", {4, 44}) ||
      !CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"emit","category":"uniform_edf","type":"EDF","inputs":[
        {"name":"color","value":[0.2,0.4,0.8]}]},
      {"name":"lamp","category":"light","type":"lightshader","inputs":[
        {"name":"edf","type":"EDF","nodename":"emit"},
        {"name":"intensity","value":[2,1,0.5]},
        {"name":"exposure","value":1}]}
    ],"outputs":[{"name":"shader","type":"EDF","nodename":"lamp"}]},
    "connections":[{"input":"edf","output":"shader"}]})json",
      "light EDF wrapper", {4, 44}) ||
      !CheckConicalClosure() ||
      !CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"measured","category":"measured_edf","type":"EDF","inputs":[
        {"name":"color","value":[0.15,0.35,0.65]},
        {"name":"file","type":"filename","value":"profile.ies"}]}
    ],"outputs":[{"name":"shader","type":"EDF","nodename":"measured"}]},
    "connections":[{"input":"edf","output":"shader"}]})json",
      "measured EDF color fallback", {4, 44}) ||
      !CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"base","category":"uniform_edf","type":"EDF","inputs":[{"name":"color","value":[0.2,0.4,0.8]}]},
      {"name":"schlick","category":"generalized_schlick_edf","type":"EDF","inputs":[
        {"name":"base","nodename":"base"},{"name":"color0","value":[0.5,0.5,0.5]},
        {"name":"color90","value":[1,0.5,0.25]},{"name":"exponent","value":5}]}
    ],"outputs":[{"name":"shader","type":"EDF","nodename":"schlick"}]},
    "connections":[{"input":"edf","output":"shader"}]})json",
      "generalized Schlick EDF", {4, 44}) ||
      !CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"a","category":"constant","type":"color3","inputs":[{"name":"value","value":[0.2,0.1,0.0]}]},
      {"name":"s","category":"constant","type":"color3","inputs":[{"name":"value","value":[0.3,0.4,0.5]}]},
      {"name":"fog","category":"absorption_vdf","type":"VDF","inputs":[{"name":"absorption","nodename":"a"},{"name":"scattering","nodename":"s"}]}
    ],"outputs":[{"name":"shader","type":"VDF","nodename":"fog"}]},
    "connections":[{"input":"vdf","output":"shader"}]})json",
      "direct VDF", {40, 41})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"subsurface","category":"subsurface_vdf","type":"VDF","inputs":[
        {"name":"scattering","value":[0.3,0.5,0.7]},
        {"name":"anisotropy","value":0.2}]}
    ],"outputs":[{"name":"shader","type":"VDF","nodename":"subsurface"}]},
    "connections":[{"input":"vdf","output":"shader"}]})json",
      "direct subsurface VDF", {24, 40, 41}) ||
      !CheckSafeVolumeAlbedo()) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"d","category":"oren_nayar_diffuse_bsdf","type":"BSDF","inputs":[{"name":"weight","value":0.8},{"name":"color","value":[0.8,0.2,0.1]}]},
      {"name":"g","category":"dielectric_bsdf","type":"BSDF","inputs":[{"name":"weight","value":0.6},{"name":"tint","value":[0.1,0.2,0.9]},{"name":"ior","value":1.45}]},
      {"name":"blend","category":"mix","type":"BSDF","inputs":[{"name":"bg","nodename":"d"},{"name":"fg","nodename":"g"},{"name":"mix","value":0.25}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"blend"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "nested BSDF mix", {0, 9, 10, 19, 20, 21})) {
    return 1;
  }
  lusdview::DrawMaterialCPU weightedMix;
  weightedMix.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"bg","category":"diffuse_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.8},{"name":"color","value":[1,0,0]}]},
      {"name":"fg","category":"diffuse_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.4},{"name":"color","value":[0,0,1]}]},
      {"name":"mix","category":"mix","type":"BSDF","inputs":[
        {"name":"bg","nodename":"bg"},{"name":"fg","nodename":"fg"},
        {"name":"mix","value":0.25}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"mix"}]},
    "connections":[{"input":"bsdf","output":"shader"}]
  })json";
  std::string weightedMixError;
  if (!lusdview::CompileMaterialXGraphRuntime(&weightedMix, &weightedMixError)) {
    std::fprintf(stderr, "weighted closure mix lowering failed: %s\n",
                 weightedMixError.c_str());
    return 1;
  }
  const auto& weightedGraph = weightedMix.materialXGraph;
  const int weightedColorIndex = weightedGraph.output[0];
  const int weightedWeightIndex = weightedGraph.output[20];
  const auto findGraphNode = [&](const char* name) -> const lusdview::MaterialXGraphNodeCPU* {
    for (const auto& node : weightedGraph.nodes)
      if (node.name == name) return &node;
    return nullptr;
  };
  const auto* weightedColor = findGraphNode("mix__closure_base_color");
  const auto* effectiveA = findGraphNode("mix__closure_base_color__mix_weight_a");
  const auto* effectiveB = findGraphNode("mix__closure_base_color__mix_weight_b");
  const auto* inverse = findGraphNode("mix__closure_mix_inverse");
  const auto* clampedFactor = findGraphNode("mix__closure_mix_factor");
  const auto* safeDenominator = findGraphNode(
      "mix__closure_base_color__safe_weight_sum");
  if (weightedColorIndex < 0 || weightedWeightIndex < 0 ||
      static_cast<size_t>(weightedColorIndex) >= weightedGraph.nodes.size() ||
      static_cast<size_t>(weightedWeightIndex) >= weightedGraph.nodes.size() ||
      weightedColor == nullptr || weightedColor->op != lusdview::MaterialXGraphOpCPU::Divide ||
      effectiveA == nullptr || effectiveA->op != lusdview::MaterialXGraphOpCPU::Multiply ||
      effectiveB == nullptr || effectiveB->op != lusdview::MaterialXGraphOpCPU::Multiply ||
      inverse == nullptr || inverse->op != lusdview::MaterialXGraphOpCPU::Subtract ||
      clampedFactor == nullptr ||
      clampedFactor->op != lusdview::MaterialXGraphOpCPU::Clamp ||
      safeDenominator == nullptr ||
      safeDenominator->op != lusdview::MaterialXGraphOpCPU::IfGreater ||
      effectiveA->input[1] != static_cast<int>(inverse - weightedGraph.nodes.data()) ||
      effectiveB->input[1] != static_cast<int>(clampedFactor - weightedGraph.nodes.data()) ||
      weightedColor->input[1] < 0) {
    std::fprintf(stderr, "weighted closure mix did not preserve mix-factor weights\n");
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"base","category":"diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"weight","value":0.7},
                 {"name":"color","value":[0.4,0.5,0.6]}]},
      {"name":"top","category":"uniform_edf","type":"EDF",
       "inputs":[{"name":"color","value":[0.1,0.2,0.3]}]},
      {"name":"layer","category":"layer","type":"BSDF",
       "inputs":[{"name":"base","nodename":"base"},
                 {"name":"top","nodename":"top"}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"layer"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "layered BSDF/EDF closure", {0, 4, 20, 21, 44})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"metal","category":"conductor_bsdf","type":"BSDF","inputs":[
        {"name":"ior","value":[0.2,0.8,1.4]},
        {"name":"extinction","value":[3.0,2.0,1.0]},
        {"name":"roughness","value":0.18}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"metal"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "direct conductor", {0, 1, 2, 20})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"diff","category":"oren_nayar_diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"weight","value":0.8},
                 {"name":"color","value":[0.7,0.4,0.2]}]},
      {"name":"scaled","category":"multiply","type":"BSDF",
       "inputs":[{"name":"in1","nodename":"diff"},
                 {"name":"in2","value":0.25}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"scaled"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "closure multiplied by scalar", {0, 20, 21})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"diff","category":"diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"weight","value":0.8},
                 {"name":"color","value":[0.7,0.4,0.2]}]},
      {"name":"scaled","category":"multiply","type":"BSDF",
       "inputs":[{"name":"in1","value":0.25},
                 {"name":"in2","nodename":"diff"}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"scaled"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "scalar multiplied by closure", {0, 20, 21})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"diff","category":"diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"weight","value":0.8},
                 {"name":"color","value":[0.7,0.4,0.2]},
                 {"name":"normal","value":[0.2,0.3,0.93]}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"diff"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "closure normal lane", {0, 5, 20, 21})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"diff","category":"diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"weight","value":0.6},{"name":"color","value":[0.3,0.5,0.7]}]},
      {"name":"emit","category":"uniform_edf","type":"EDF",
       "inputs":[{"name":"color","value":[0.1,0.2,0.4]}]},
      {"name":"surface","category":"surface","type":"surfaceshader",
       "inputs":[{"name":"bsdf","nodename":"diff"},{"name":"edf","nodename":"emit"},
                 {"name":"opacity","value":0.65}]}
    ],"outputs":[{"name":"shader","type":"surfaceshader","nodename":"surface"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "surface closure wrapper", {0, 3, 4, 20, 21, 44})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"diff","category":"diffuse_bsdf","type":"BSDF",
       "inputs":[{"name":"color","value":[0.2,0.4,0.8]}]},
      {"name":"emit","category":"uniform_edf","type":"EDF",
       "inputs":[{"name":"color","value":[0.1,0.2,0.3]}]},
      {"name":"surface","category":"surface","type":"surfaceshader",
       "inputs":[{"name":"bsdf","nodename":"diff"},
                 {"name":"edf","nodename":"emit"}]},
      {"name":"material","category":"surfacematerial","type":"material",
       "inputs":[{"name":"surfaceshader","nodename":"surface"}]}
    ],"outputs":[{"name":"shader","type":"material","nodename":"material"}]},
    "connections":[{"input":"material","output":"shader"}]})json",
      "direct surfacematerial wrapper", {0, 4, 20, 21, 44})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"fog","category":"anisotropic_vdf","type":"VDF",
       "inputs":[{"name":"absorption","value":[0.2,0.3,0.4]},
                 {"name":"scattering","value":[0.1,0.2,0.3]},
                 {"name":"anisotropy","value":0.15}]},
      {"name":"lamp","category":"uniform_edf","type":"EDF",
       "inputs":[{"name":"color","value":[0.05,0.1,0.2]}]},
      {"name":"volume","category":"volume","type":"volumeshader",
       "inputs":[{"name":"vdf","nodename":"fog"},
                 {"name":"edf","nodename":"lamp"}]}
    ],"outputs":[{"name":"shader","type":"volumeshader",
      "nodename":"volume"}]},
    "connections":[{"input":"vdf","output":"shader"}]
  })json",
      "volume shader wrapper", {4, 40, 41})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"fog","category":"absorption_vdf","type":"VDF",
       "inputs":[{"name":"absorption","value":[0.2,0.3,0.4]}]},
      {"name":"volume","category":"volume","type":"volumeshader",
       "inputs":[{"name":"vdf","nodename":"fog"}]},
      {"name":"material","category":"volumematerial","type":"material",
       "inputs":[{"name":"volumeshader","nodename":"volume"}]}
    ],"outputs":[{"name":"shader","type":"material",
      "nodename":"material"}]},
    "connections":[{"input":"vdf","output":"shader"}]
  })json",
      "direct volumematerial wrapper", {40, 41})) {
    return 1;
  }
  // Complete schema shaders can also be exposed directly as graph outputs.
  // They must lower to authored OpenPBR lanes just like closure wrappers.
  lusdview::DrawMaterialCPU standardSurfaceGraph;
  standardSurfaceGraph.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"standard","category":"standard_surface",
       "type":"surfaceshader","inputs":[
        {"name":"base","value":0.7},
        {"name":"base_color","value":[0.2,0.4,0.8]},
        {"name":"metalness","value":0.35},
        {"name":"roughness","value":0.27},
        {"name":"specular_ior","value":1.6},
        {"name":"emission","value":2.0},
        {"name":"thin_film_thickness","value":120.0},
        {"name":"thin_film_IOR","value":1.4},
        {"name":"opacity","value":0.75}]}
    ],"outputs":[{"name":"shader","type":"surfaceshader",
      "nodename":"standard"}]},
    "connections":[{"input":"bsdf","output":"shader"}]
  })json";
  std::string standardSurfaceError;
  if (!lusdview::CompileMaterialXGraphRuntime(&standardSurfaceGraph,
                                               &standardSurfaceError) ||
      standardSurfaceGraph.materialXGraph.output[0] < 0 ||
      standardSurfaceGraph.materialXGraph.output[1] < 0 ||
      standardSurfaceGraph.materialXGraph.output[2] < 0 ||
      standardSurfaceGraph.materialXGraph.output[19] < 0 ||
      standardSurfaceGraph.materialXGraph.output[20] < 0 ||
      standardSurfaceGraph.materialXGraph.output[28] < 0 ||
      standardSurfaceGraph.materialXGraph.output[44] < 0 ||
      standardSurfaceGraph.materialXGraph.output[3] < 0) {
    std::fprintf(stderr, "direct standard_surface lowering failed: %s\n",
                 standardSurfaceError.c_str());
    return 1;
  }
  const auto& standardGraph = standardSurfaceGraph.materialXGraph;
  const auto laneValue = [&](int lane, int component) {
    const int index = standardGraph.output[static_cast<size_t>(lane)];
    return index >= 0 && static_cast<size_t>(index) < standardGraph.nodes.size()
               ? standardGraph.nodes[static_cast<size_t>(index)].value[0][component]
               : -1.0f;
  };
  if (!Near(laneValue(0, 0), 0.2f) || !Near(laneValue(1, 0), 0.35f) ||
      !Near(laneValue(2, 0), 0.27f) || !Near(laneValue(19, 0), 1.6f) ||
      !Near(laneValue(20, 0), 0.7f) ||
      !Near(laneValue(44, 0), 2.0f) || !Near(laneValue(3, 0), 0.75f)) {
    std::fprintf(stderr, "direct standard_surface values were not routed\n");
    return 1;
  }
  lusdview::DrawMaterialCPU openPbrSurfaceGraph;
  openPbrSurfaceGraph.materialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"openpbr","category":"open_pbr_surface",
       "type":"surfaceshader","inputs":[
        {"name":"base_weight","value":0.6},
        {"name":"base_metalness","value":0.25},
        {"name":"base_roughness","value":0.31},
        {"name":"transmission_weight","value":0.4},
        {"name":"specular_roughness_anisotropy","value":0.11},
        {"name":"transmission_dispersion_abbe_number","value":45.0},
        {"name":"transmission_dispersion_scale","value":0.8},
        {"name":"coat_weight","value":0.15},
        {"name":"coat_roughness_anisotropy","value":0.12},
        {"name":"coat_affect_color","value":[0.7,0.8,0.9]},
        {"name":"coat_affect_roughness","value":0.2},
        {"name":"coat_darkening","value":0.1},
        {"name":"fuzz_weight","value":0.2},
        {"name":"fuzz_color","value":[0.6,0.7,0.8]},
        {"name":"fuzz_roughness","value":0.22},
        {"name":"emission_luminance","value":1.5},
        {"name":"thin_film_thickness","value":120.0},
        {"name":"thin_film_ior","value":1.3},
        {"name":"geometry_opacity","value":0.8}]}
    ],"outputs":[{"name":"shader","type":"surfaceshader",
      "nodename":"openpbr"}]},
    "connections":[{"input":"bsdf","output":"shader"}]
  })json";
  std::string openPbrSurfaceError;
  if (!lusdview::CompileMaterialXGraphRuntime(&openPbrSurfaceGraph,
                                               &openPbrSurfaceError) ||
      openPbrSurfaceGraph.materialXGraph.output[1] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[2] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[33] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[11] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[13] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[15] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[29] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[30] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[35] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[36] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[39] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[27] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[45] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[46] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[47] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[20] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[44] < 0 ||
      openPbrSurfaceGraph.materialXGraph.output[3] < 0) {
    std::fprintf(stderr, "direct open_pbr_surface lowering failed: %s\n",
                 openPbrSurfaceError.c_str());
    return 1;
  }
  const auto openPbrLaneValue = [&](int lane) {
    const int index = openPbrSurfaceGraph.materialXGraph.output[
        static_cast<size_t>(lane)];
    return index >= 0 && static_cast<size_t>(index) <
        openPbrSurfaceGraph.materialXGraph.nodes.size()
        ? openPbrSurfaceGraph.materialXGraph.nodes[static_cast<size_t>(index)]
              .value[0][0]
        : -1.0f;
  };
  if (!Near(openPbrLaneValue(15), 0.0f) ||
      !Near(openPbrLaneValue(27), 1.6f)) {
    std::fprintf(stderr, "direct open_pbr_surface defaults drifted\n");
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"subsurface","category":"subsurface_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.7},{"name":"color","value":[0.2,0.4,0.6]},
        {"name":"radius","value":[0.3,0.5,0.7]},{"name":"scale","value":2.5},
        {"name":"anisotropy","value":0.2}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"subsurface"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "direct subsurface BSDF", {6, 7, 8, 25, 26})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"schlick","category":"generalized_schlick_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.5},{"name":"color0","value":[0.2,0.3,0.4]},
        {"name":"color90","value":[0.8,0.7,0.6]},
        {"name":"roughness","value":0.2},{"name":"scatter_mode","value":"RT"}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"schlick"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "generalized Schlick RT", {9, 10, 11, 12, 2})) {
    return 1;
  }
  if (!CheckDirectClosure(R"json({
    "nodegraph":{"nodes":[
      {"name":"glass","category":"dielectric_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.6},{"name":"scatter_mode","value":"T"},
        {"name":"roughness","value":[0.2,0.4]}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"glass"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "dielectric transmission-only", {2, 11, 12}) ||
      !CheckOnlyDirectClosureRoutes(R"json({
    "nodegraph":{"nodes":[
      {"name":"glass","category":"dielectric_bsdf","type":"BSDF","inputs":[
        {"name":"weight","value":0.6},{"name":"scatter_mode","value":"TR"}]}
    ],"outputs":[{"name":"shader","type":"BSDF","nodename":"glass"}]},
    "connections":[{"input":"bsdf","output":"shader"}]})json",
      "invalid dielectric scatter mode", {2, 19, 29, 30})) {
    return 1;
  }

  const char* volumeTexturePath = "/tmp/lightusd_lightrt_volume.ppm";
  FILE* volumeTexture = std::fopen(volumeTexturePath, "wb");
  if (!volumeTexture) {
    std::fprintf(stderr, "could not create volume texture fixture\n");
    return 1;
  }
  std::fputs("P6\n1 1\n255\n", volumeTexture);
  std::fputc(128, volumeTexture);
  std::fputc(64, volumeTexture);
  std::fputc(0, volumeTexture);
  std::fclose(volumeTexture);
  const char* texturedVolumeXml =
      "<materialx version=\"1.39\">"
      " <image name=\"density\" type=\"color3\"><input name=\"file\""
      " type=\"filename\" value=\"lightusd_lightrt_volume.ppm\"/></image>"
      " <anisotropic_vdf name=\"Fog\" type=\"VDF\"><input name=\"absorption\""
      " type=\"color3\" nodename=\"density\"/></anisotropic_vdf>"
      " <volume name=\"Volume\" type=\"volumeshader\"><input name=\"vdf\""
      " type=\"VDF\" nodename=\"Fog\"/></volume>"
      " <volumematerial name=\"VolumeMat\" type=\"material\"><input"
      " name=\"volumeshader\" type=\"volumeshader\" nodename=\"Volume\"/>"
      " </volumematerial></materialx>";
  lusdview::DrawMaterialCPU texturedVolume;
  std::string texturedVolumeError;
  const bool texturedVolumeOk =
      lusdview::EvaluateMaterialXStringToLightRtVolumeWithBaseDir(
          texturedVolumeXml, "VolumeMat", "/tmp", &texturedVolume,
          &texturedVolumeError);
  std::remove(volumeTexturePath);
  if (!texturedVolumeOk || !Near(texturedVolume.volumeDensity, 128.0f / 255.0f) ||
      !Near(texturedVolume.volumeAlbedo[0], 0.0f) ||
      !Near(texturedVolume.volumeAlbedo[1], 0.0f) ||
      !Near(texturedVolume.volumeAlbedo[2], 0.0f)) {
    std::fprintf(stderr, "relative volume texture evaluation failed: %s\n",
                 texturedVolumeError.c_str());
    return 1;
  }

  const char* volumeXml =
      "<materialx version=\"1.39\">"
      "<anisotropic_vdf name=\"Fog\" type=\"VDF\">"
      "<input name=\"absorption\" type=\"vector3\" value=\"0.1,0.2,0.3\"/>"
      "<input name=\"scattering\" type=\"vector3\" value=\"0.4,0.3,0.2\"/>"
      "<input name=\"anisotropy\" type=\"float\" value=\"0.35\"/>"
      "</anisotropic_vdf>"
      "<uniform_edf name=\"Glow\" type=\"EDF\"><input name=\"color\" "
      "type=\"color3\" value=\"0.2,0.4,0.8\"/></uniform_edf>"
      "<volume name=\"Volume\" type=\"volumeshader\">"
      "<input name=\"vdf\" type=\"VDF\" nodename=\"Fog\"/>"
      "<input name=\"edf\" type=\"EDF\" nodename=\"Glow\"/></volume>"
      "<volumematerial name=\"FogMat\" type=\"material\"><input "
      "name=\"volumeshader\" type=\"volumeshader\" nodename=\"Volume\"/>"
      "</volumematerial></materialx>";
  lusdview::DrawMaterialCPU volumeMaterial;
  std::string volumeError;
  if (!lusdview::EvaluateMaterialXStringToLightRtVolume(
          volumeXml, "FogMat", &volumeMaterial, &volumeError) ||
      !volumeMaterial.hasVolumeOutput ||
      !Near(volumeMaterial.volumeDensity, 0.5f) ||
      !Near(volumeMaterial.volumeAlbedo[0], 0.8f) ||
      !Near(volumeMaterial.volumeAlbedo[1], 0.6f) ||
      !Near(volumeMaterial.volumeAlbedo[2], 0.4f) ||
      !Near(volumeMaterial.volumeAnisotropy, 0.35f) ||
      !Near(volumeMaterial.volumeEmission[2], 0.8f) ||
      !Near(volumeMaterial.volumeEmissionScale, 1.0f)) {
    std::fprintf(stderr, "MaterialX volume transport evaluation failed: %s\n",
                 volumeError.c_str());
    return 1;
  }

  // The next converter stores volume terminals separately from surface
  // graphs. Verify that the shared bridge evaluates that retained graph
  // instead of falling back to the scalar terminal defaults.
  lusdview::DrawMaterialCPU nextVolumeGraph;
  nextVolumeGraph.volumeMaterialXNodeGraphJson = R"json({
    "nodegraph":{"nodes":[
      {"name":"densityNode","category":"constant","type":"float",
       "inputs":[{"name":"value","value":0.75}]},
      {"name":"emissionNode","category":"constant","type":"color3",
       "inputs":[{"name":"value","value":[0.2,0.3,0.4]}]},
      {"name":"scaleNode","category":"constant","type":"float",
       "inputs":[{"name":"value","value":2.0}]},
      {"name":"anisotropyNode","category":"constant","type":"float",
       "inputs":[{"name":"value","value":0.25}]}
    ],"outputs":[
      {"name":"volume_density","type":"float","nodename":"densityNode","output":"out"},
      {"name":"volume_emission_color","type":"color3","nodename":"emissionNode","output":"out"},
      {"name":"volume_emission_scale","type":"float","nodename":"scaleNode","output":"out"},
      {"name":"volume_anisotropy","type":"float","nodename":"anisotropyNode","output":"out"}]},
    "connections":[{"input":"volume_density","output":"volume_density"},
      {"input":"volume_emission_color","output":"volume_emission_color"},
      {"input":"volume_emission_scale","output":"volume_emission_scale"},
      {"input":"volume_anisotropy","output":"volume_anisotropy"}]})json";
  nextVolumeGraph.hasVolumeOutput = true;
  lusdview::BakeRealtimePbrMaterial(&nextVolumeGraph);
  if (!nextVolumeGraph.hasVolumeOutput ||
      !Near(nextVolumeGraph.volumeDensity, 0.75f) ||
      !Near(nextVolumeGraph.volumeEmission[0], 0.4f) ||
      !Near(nextVolumeGraph.volumeEmission[2], 0.8f) ||
      !Near(nextVolumeGraph.volumeAnisotropy, 0.25f)) {
    std::fprintf(stderr, "next MaterialX volume graph evaluation failed (density=%g)\n",
                 nextVolumeGraph.volumeDensity);
    return 1;
  }

  return 0;
}
