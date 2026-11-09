// SPDX-License-Identifier: Apache-2.0
#include "scene_optimize.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "lightrt_mtlx_bridge.hh"

namespace lusdview {
namespace {

constexpr size_t kPackedMaterialSampleCount = 13;
constexpr size_t kPackedMaterialSampleMetaWidth = 8;

struct PackedMaterial {
  std::array<float, kLightRtOpenPBRFloats> lightRt{};
  std::array<float, kRtMaterialTextureParamFloats> rtTextures{};
  std::array<float, kRasterMaterialTextureParamFloats> rasterTextures{};
  std::array<float, 23> scalars{};
  std::array<int, 14> textureIds{};
  // Descriptor selection depends on the sampling intent, not only the image
  // id. In particular, one ORM image may be sampled with different wrap,
  // channel, or colorspace settings by two material inputs.
  std::array<int, kPackedMaterialSampleCount * kPackedMaterialSampleMetaWidth>
      sampleMeta{};
  std::array<int, kRasterMaterialGraphImageCount> graphTextureIds{};
  std::array<float, kRtMaterialGraphFloats> graph{};
  int alphaMode{0};
  float alphaCutoff{0.0f};
  uint32_t flags{0};
  bool finite{true};
};

void PackSampleMeta(const DrawTexSampleCPU& sample, int textureId,
                    int* dst) {
  dst[0] = textureId;
  dst[1] = sample.channel;
  dst[2] = sample.uvSet;
  dst[3] = static_cast<int>(sample.wrapS);
  dst[4] = static_cast<int>(sample.wrapT);
  dst[5] = static_cast<int>(sample.colorSpace);
  dst[6] = sample.isUdim ? 1 : 0;
  dst[7] = sample.isPtex ? 1 : 0;
}

float NormalizeFloat(float value, bool* finite) {
  if (!std::isfinite(value)) *finite = false;
  return value == 0.0f ? 0.0f : value;
}

template <size_t N>
void Normalize(std::array<float, N>* values, bool* finite) {
  for (float& value : *values) value = NormalizeFloat(value, finite);
}

PackedMaterial Pack(const DrawMaterialCPU& material) {
  PackedMaterial packed;
  PackLightRtOpenPBR(material, packed.lightRt.data());
  PackRtMaterialTextureParams(material, packed.rtTextures.data());
  PackRasterMaterialTextureParams(material, packed.rasterTextures.data());
  PackRasterMaterialXGraphRuntime(material, packed.graph.data());
  packed.scalars = {
      material.baseColor[0], material.baseColor[1], material.baseColor[2],
      material.metallic, material.roughness,
      material.emissive[0], material.emissive[1], material.emissive[2],
      material.alpha, material.alphaCutoff,
      material.specularColor[0], material.specularColor[1],
      material.specularColor[2], material.ior, material.occlusion,
      material.coatWeight, material.coatColor[0], material.coatColor[1],
      material.coatColor[2], material.coatRoughness, material.coatIor,
      material.displacementConst, material.displacementTexScale};
  packed.textureIds = {
      material.baseColorTex, material.metallicTex, material.roughnessTex,
      material.normalTex, material.coatNormalTex, material.emissiveTex,
      material.opacityTex, material.occlusionTex, material.specularColorTex,
      material.coatWeightTex, material.coatColorTex,
      material.coatRoughnessTex, material.coatNormalTex,
      material.displacementTex};
  const std::array<const DrawTexSampleCPU*, kPackedMaterialSampleCount> samples = {
      &material.baseColorSample,      &material.metallicSample,
      &material.roughnessSample,      &material.normalSample,
      &material.coatNormalSample,     &material.emissiveSample,
      &material.opacitySample,        &material.occlusionSample,
      &material.specularColorSample,  &material.coatWeightSample,
      &material.coatColorSample,      &material.coatRoughnessSample,
      &material.displacementSample};
  const std::array<int, kPackedMaterialSampleCount> sampleTextureIds = {
      material.baseColorTex,      material.metallicTex,
      material.roughnessTex,      material.normalTex,
      material.coatNormalTex,     material.emissiveTex,
      material.opacityTex,        material.occlusionTex,
      material.specularColorTex,  material.coatWeightTex,
      material.coatColorTex,      material.coatRoughnessTex,
      material.displacementTex};
  for (size_t i = 0; i < samples.size(); ++i) {
    PackSampleMeta(*samples[i], sampleTextureIds[i],
                   packed.sampleMeta.data() +
                       i * kPackedMaterialSampleMetaWidth);
  }
  packed.graphTextureIds.fill(-1);
  size_t graphTextureCount = 0;
  for (const MaterialXGraphNodeCPU& node : material.materialXGraph.nodes) {
    if (node.textureId < 0 || graphTextureCount >= packed.graphTextureIds.size()) {
      continue;
    }
    if (std::find(packed.graphTextureIds.begin(),
                  packed.graphTextureIds.begin() + graphTextureCount,
                  node.textureId) ==
        packed.graphTextureIds.begin() + graphTextureCount) {
      packed.graphTextureIds[graphTextureCount++] = node.textureId;
    }
  }
  packed.alphaMode = material.alphaMode;
  packed.alphaCutoff = material.alphaCutoff;
  packed.flags = (material.hasUsdPreviewSurface ? 1u : 0u) |
                 (material.hasOpenPBRSurface ? 2u : 0u) |
                 (material.hasDisplacementOutput ? 4u : 0u) |
                 (material.hasVolumeOutput ? 8u : 0u) |
                 (material.useSpecularWorkflow ? 16u : 0u) |
                 (material.openPbrSpecularModel ? 32u : 0u) |
                 (material.alphaMaskHeuristic ? 64u : 0u) |
                 (material.materialXVolumeGraph ? 128u : 0u) |
                 (material.hasLightRtOpenPBR ? 256u : 0u);
  Normalize(&packed.lightRt, &packed.finite);
  Normalize(&packed.rtTextures, &packed.finite);
  Normalize(&packed.rasterTextures, &packed.finite);
  Normalize(&packed.scalars, &packed.finite);
  Normalize(&packed.graph, &packed.finite);
  packed.alphaCutoff = NormalizeFloat(packed.alphaCutoff, &packed.finite);
  return packed;
}

uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

uint64_t DrawMaterialRenderHash(const DrawMaterialCPU& material) {
  const PackedMaterial packed = Pack(material);
  uint64_t hash = 1469598103934665603ull;
  hash = HashBytes(hash, packed.lightRt.data(), sizeof(packed.lightRt));
  hash = HashBytes(hash, packed.rtTextures.data(), sizeof(packed.rtTextures));
  hash = HashBytes(hash, packed.rasterTextures.data(),
                   sizeof(packed.rasterTextures));
  hash = HashBytes(hash, packed.scalars.data(), sizeof(packed.scalars));
  hash = HashBytes(hash, packed.textureIds.data(), sizeof(packed.textureIds));
  hash = HashBytes(hash, packed.sampleMeta.data(), sizeof(packed.sampleMeta));
  hash = HashBytes(hash, packed.graphTextureIds.data(),
                   sizeof(packed.graphTextureIds));
  hash = HashBytes(hash, packed.graph.data(), sizeof(packed.graph));
  hash = HashBytes(hash, &packed.alphaMode, sizeof(packed.alphaMode));
  hash = HashBytes(hash, &packed.alphaCutoff, sizeof(packed.alphaCutoff));
  return HashBytes(hash, &packed.flags, sizeof(packed.flags));
}

bool DrawMaterialsRenderEquivalent(const DrawMaterialCPU& a,
                                   const DrawMaterialCPU& b) {
  const PackedMaterial pa = Pack(a);
  const PackedMaterial pb = Pack(b);
  if (!pa.finite || !pb.finite) return false;
  return pa.lightRt == pb.lightRt && pa.rtTextures == pb.rtTextures &&
         pa.rasterTextures == pb.rasterTextures && pa.scalars == pb.scalars &&
         pa.textureIds == pb.textureIds &&
         pa.sampleMeta == pb.sampleMeta &&
         pa.graphTextureIds == pb.graphTextureIds && pa.graph == pb.graph &&
         pa.alphaMode == pb.alphaMode && pa.alphaCutoff == pb.alphaCutoff &&
         pa.flags == pb.flags;
}

DrawMaterialTable BuildDrawMaterialTable(
    const std::vector<DrawMaterialCPU>& materials) {
  DrawMaterialTable table;
  table.logicalToCanonical.assign(materials.size(), -1);
  std::unordered_map<uint64_t, std::vector<int>> candidates;
  candidates.reserve(materials.size());
  for (size_t logical = 0; logical < materials.size(); ++logical) {
    const DrawMaterialCPU& material = materials[logical];
    const uint64_t hash = DrawMaterialRenderHash(material);
    int canonical = -1;
    auto& bucket = candidates[hash];
    for (int candidate : bucket) {
      const int representative =
          table.canonicalRepresentatives[static_cast<size_t>(candidate)];
      if (DrawMaterialsRenderEquivalent(
              material, materials[static_cast<size_t>(representative)])) {
        canonical = candidate;
        break;
      }
    }
    if (canonical < 0) {
      canonical = static_cast<int>(table.canonicalRepresentatives.size());
      table.canonicalRepresentatives.push_back(static_cast<int>(logical));
      bucket.push_back(canonical);
    }
    table.logicalToCanonical[logical] = canonical;
  }
  return table;
}

size_t CanonicalizeDrawMaterials(DrawScene* scene) {
  if (!scene) return 0;
  if (scene->optimization.sourceMaterials == 0) {
    scene->optimization.sourceMaterials = scene->materials.size();
  }
  const DrawMaterialTable table = BuildDrawMaterialTable(scene->materials);
  scene->optimization.uniqueMaterials = scene->materials.size();
  scene->optimization.canonicalMaterials =
      table.canonicalRepresentatives.size();
  const size_t shared = scene->materials.size() -
                        table.canonicalRepresentatives.size();
  scene->optimization.deduplicatedMaterials = shared;
  return shared;
}

size_t DeduplicateDrawMaterials(DrawScene* scene) {
  return CanonicalizeDrawMaterials(scene);
}

}  // namespace lusdview
