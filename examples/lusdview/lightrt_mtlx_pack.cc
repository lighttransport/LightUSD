// SPDX-License-Identifier: Apache-2.0
#include "lightrt_mtlx_bridge.hh"

#include <algorithm>
#include <cstring>
#include <vector>

namespace lusdview {

namespace {

void Store4(const float src[4], float* dst) {
  for (int i = 0; i < 4; ++i) dst[i] = src[i];
}

void StoreUvCompact(const DrawUvXformCPU& uv, float* dst) {
  dst[0] = uv.m00;
  dst[1] = uv.m01;
  dst[2] = uv.tx;
  dst[3] = uv.m10;
  dst[4] = uv.m11;
  dst[5] = uv.ty;
}

void StoreUvVec4Rows(const DrawUvXformCPU& uv, float* dst) {
  dst[0] = uv.m00;
  dst[1] = uv.m01;
  dst[2] = uv.tx;
  dst[3] = 0.0f;
  dst[4] = uv.m10;
  dst[5] = uv.m11;
  dst[6] = uv.ty;
  dst[7] = 0.0f;
}
}  // namespace

void PackLightRtOpenPBR(const DrawMaterialCPU& mat, float* dst) {
  if (!dst) return;
  DrawLightRtOpenPBRCPU fallback;
  const DrawLightRtOpenPBRCPU& m =
      mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR : fallback;
  lightusd::tydra::PackLightRtOpenPBRParams(
      m, mat.hasLightRtOpenPBR, static_cast<float>(mat.alphaMode),
      mat.alphaCutoff, dst);
}

void PackRtMaterialTextureParams(const DrawMaterialCPU& mat, float* dst) {
  if (!dst) return;
  std::fill(dst, dst + kRtMaterialTextureParamFloats, 0.0f);
  StoreUvCompact(mat.baseColorSample.uv, dst + 0);
  StoreUvCompact(mat.metallicSample.uv, dst + 6);
  StoreUvCompact(mat.roughnessSample.uv, dst + 12);
  StoreUvCompact(mat.normalSample.uv, dst + 18);
  StoreUvCompact(mat.emissiveSample.uv, dst + 24);
  StoreUvCompact(mat.opacitySample.uv, dst + 30);
  Store4(mat.baseColorSample.scale, dst + 36);
  Store4(mat.baseColorSample.bias, dst + 40);
  Store4(mat.normalSample.scale, dst + 44);
  Store4(mat.normalSample.bias, dst + 48);
  Store4(mat.emissiveSample.scale, dst + 52);
  Store4(mat.emissiveSample.bias, dst + 56);
  dst[60] = static_cast<float>(mat.metallicChannel);
  dst[61] = mat.metallicTexScale;
  dst[62] = mat.metallicTexBias;
  dst[63] = static_cast<float>(mat.roughnessChannel);
  dst[64] = mat.roughnessTexScale;
  dst[65] = mat.roughnessTexBias;
  dst[66] = static_cast<float>(mat.opacityChannel);
  dst[67] = mat.opacityTexScale;
  dst[68] = mat.opacityTexBias;
  // Per-slot UV set, bit-packed: base, metallic, roughness, normal, emissive,
  // opacity. The float stores a small exact integer.
  int uvSetBits = 0;
  if (mat.baseColorSample.uvSet == 1) uvSetBits |= 1;
  if (mat.metallicSample.uvSet == 1) uvSetBits |= 2;
  if (mat.roughnessSample.uvSet == 1) uvSetBits |= 4;
  if (mat.normalSample.uvSet == 1) uvSetBits |= 8;
  if (mat.emissiveSample.uvSet == 1) uvSetBits |= 16;
  if (mat.opacitySample.uvSet == 1) uvSetBits |= 32;
  dst[69] = static_cast<float>(uvSetBits);
  dst[70] = mat.occlusionTexScale;
  dst[71] = mat.occlusionTexBias;
  // Extra slots keep the same slot*6 UV-transform convention: 12 = occlusion,
  // 13 = coat weight, 14 = coat color, 15 = coat roughness.
  StoreUvCompact(mat.occlusionSample.uv, dst + 72);
  StoreUvCompact(mat.coatWeightSample.uv, dst + 78);
  StoreUvCompact(mat.coatColorSample.uv, dst + 84);
  StoreUvCompact(mat.coatRoughnessSample.uv, dst + 90);
  // Scalar slots default to channel 0 (R) when nothing was authored.
  dst[96] = static_cast<float>(mat.occlusionChannel < 0 ? 0
                                                        : mat.occlusionChannel);
  dst[97] = static_cast<float>(
      mat.coatWeightSample.channel < 0 ? 0 : mat.coatWeightSample.channel);
  dst[98] = static_cast<float>(mat.coatRoughnessSample.channel < 0
                                   ? 0
                                   : mat.coatRoughnessSample.channel);
  int uvSetBits2 = 0;
  if (mat.occlusionSample.uvSet == 1) uvSetBits2 |= 1;
  if (mat.coatWeightSample.uvSet == 1) uvSetBits2 |= 2;
  if (mat.coatColorSample.uvSet == 1) uvSetBits2 |= 4;
  if (mat.coatRoughnessSample.uvSet == 1) uvSetBits2 |= 8;
  dst[99] = static_cast<float>(uvSetBits2);
  Store4(mat.coatWeightSample.scale, dst + 100);
  Store4(mat.coatWeightSample.bias, dst + 104);
  Store4(mat.coatColorSample.scale, dst + 108);
  Store4(mat.coatColorSample.bias, dst + 112);
  Store4(mat.coatRoughnessSample.scale, dst + 116);
  Store4(mat.coatRoughnessSample.bias, dst + 120);
  StoreUvCompact(mat.specularColorSample.uv, dst + 124);
  dst[130] = static_cast<float>(mat.specularColorSample.uvSet);
  dst[131] = mat.useSpecularWorkflow ? 1.0f : 0.0f;
  Store4(mat.specularColorSample.scale, dst + 132);
  Store4(mat.specularColorSample.bias, dst + 136);
  dst[139] = mat.openPbrSpecularModel ? 1.0f : 0.0f;
  StoreUvCompact(mat.coatNormalSample.uv, dst + 140);
  dst[146] = static_cast<float>(mat.coatNormalSample.uvSet);
  Store4(mat.coatNormalSample.scale, dst + 147);
  Store4(mat.coatNormalSample.bias, dst + 151);
}

void PackMaterialXGraphRuntime(const DrawMaterialCPU& mat, float* dst,
                               const std::vector<int>* sourceToTable) {
  if (!dst) return;
  std::fill(dst, dst + kRtMaterialGraphFloats, 0.0f);
  for (int i = 0; i < kRtMaterialGraphOutputCount; ++i) dst[1 + i] = -1.0f;
  const MaterialXGraphRuntimeCPU& graph = mat.materialXGraph;
  if (!graph.valid) return;
  const size_t count = std::min<size_t>(graph.nodes.size(),
                                        kRtMaterialGraphMaxNodes);
  dst[0] = static_cast<float>(count);
  for (int i = 0; i < kRtMaterialGraphOutputCount; ++i)
    dst[1 + i] = static_cast<float>(graph.output[i]);
  // Header float 50 is the reserved lane; the vkr shader reads it to pick the
  // tangent basis for the normal route.
  dst[50] = static_cast<float>(graph.normal_uv_set);
  for (size_t i = 0; i < count; ++i) {
    const MaterialXGraphNodeCPU& node = graph.nodes[i];
    const size_t base = kRtMaterialGraphHeaderFloats +
                        i * kRtMaterialGraphNodeFloats;
    dst[base + 0] = static_cast<float>(node.op);
    dst[base + 1] = static_cast<float>(node.input[0]);
    dst[base + 2] = static_cast<float>(node.input[1]);
    dst[base + 3] = static_cast<float>(node.input[2]);
    for (int input = 0; input < 3; ++input)
      for (int lane = 0; lane < 4; ++lane)
        dst[base + 4 + input * 4 + lane] = node.value[input][lane];
    if (node.op == MaterialXGraphOpCPU::Image ||
        node.op == MaterialXGraphOpCPU::TiledImage) {
      // Image nodes do not consume a third value fallback. Keep the authored
      // primaries conversion in that lane without changing the shared graph ABI.
      dst[base + 12] = static_cast<float>(
          MaterialXGraphColorSpaceMode(node.colorSpaceToken));
      dst[base + 13] =
          static_cast<float>(node.colorSpaceTransferCorrection);
    }
    const bool usesAuxInput = node.op == MaterialXGraphOpCPU::SplitLR ||
                              node.op == MaterialXGraphOpCPU::SplitTB ||
                              node.op == MaterialXGraphOpCPU::IfGreater ||
                              node.op == MaterialXGraphOpCPU::IfGreaterEqual ||
                              node.op == MaterialXGraphOpCPU::IfEqual ||
                              node.op == MaterialXGraphOpCPU::Fractal2D ||
                              node.op == MaterialXGraphOpCPU::Fractal3D ||
                              node.op == MaterialXGraphOpCPU::Grid ||
                              node.op == MaterialXGraphOpCPU::Crosshatch ||
                              node.op == MaterialXGraphOpCPU::TiledCircles ||
                              node.op == MaterialXGraphOpCPU::TiledCloverleafs ||
                              node.op == MaterialXGraphOpCPU::TiledHexagons;
    const bool usesRampTable=node.op==MaterialXGraphOpCPU::Ramp||
                             node.op==MaterialXGraphOpCPU::RampGradient||
                             node.op==MaterialXGraphOpCPU::Flake||
                             node.op==MaterialXGraphOpCPU::MatrixTransform||
                             node.op==MaterialXGraphOpCPU::MatrixTranspose||
                             node.op==MaterialXGraphOpCPU::MatrixInverse||
                             node.op==MaterialXGraphOpCPU::MatrixDeterminant||
                             node.op==MaterialXGraphOpCPU::ArtisticIor;
    int textureId = (usesAuxInput||usesRampTable) ? node.auxInput : node.textureId;
    if (!usesAuxInput && sourceToTable && textureId >= 0 &&
        static_cast<size_t>(textureId) < sourceToTable->size()) {
      textureId = (*sourceToTable)[static_cast<size_t>(textureId)];
    }
    dst[base + 16] = static_cast<float>(textureId);
    if (node.op == MaterialXGraphOpCPU::Normalized ||
        node.op == MaterialXGraphOpCPU::Length ||
        node.op == MaterialXGraphOpCPU::Distance) {
      dst[base + 3] = static_cast<float>(
          node.op == MaterialXGraphOpCPU::Normalized ? node.components
                                                     : node.inputComponents);
    }
    if (node.op == MaterialXGraphOpCPU::GeomProp) {
      const uint32_t hash = MaterialXGeomPropHash(node.geomPropName);
      float encoded = 0.0f;
      std::memcpy(&encoded, &hash, sizeof(encoded));
      dst[base + 17] = encoded;
      dst[base + 18] = node.auxValue[1];
      dst[base + 19] = node.auxValue[2];
      continue;
    }
    if (node.op == MaterialXGraphOpCPU::IfGreater ||
        node.op == MaterialXGraphOpCPU::IfGreaterEqual ||
        node.op == MaterialXGraphOpCPU::IfEqual ||
        node.op == MaterialXGraphOpCPU::Fractal2D ||
        node.op == MaterialXGraphOpCPU::Fractal3D ||
        node.op == MaterialXGraphOpCPU::Grid || node.op == MaterialXGraphOpCPU::Crosshatch ||
        node.op == MaterialXGraphOpCPU::TiledCircles || node.op == MaterialXGraphOpCPU::TiledCloverleafs ||
        node.op == MaterialXGraphOpCPU::TiledHexagons || usesRampTable) {
      for (int lane = 0; lane < 4; ++lane)
        dst[base + 17 + lane] = node.auxValue[lane];
      continue;
    }
    dst[base + 17] = node.uvScale[0];
    dst[base + 18] = node.uvScale[1];
    dst[base + 19] = node.uvOffset[0];
    dst[base + 20] = node.uvOffset[1];
  }
}

void PackRasterMaterialXGraphRuntime(const DrawMaterialCPU& mat, float* dst) {
  if (!dst) return;
  std::fill(dst, dst + kRtMaterialGraphFloats, 0.0f);
  for (int i = 0; i < kRtMaterialGraphOutputCount; ++i) dst[1 + i] = -1.0f;
  const MaterialXGraphRuntimeCPU& graph = mat.materialXGraph;
  if (!graph.valid) return;
  const size_t count = std::min<size_t>(graph.nodes.size(),
                                        kRtMaterialGraphMaxNodes);
  dst[0] = static_cast<float>(count);
  for (int i = 0; i < kRtMaterialGraphOutputCount; ++i)
    dst[1 + i] = static_cast<float>(graph.output[i]);
  std::vector<int> textureIds;
  textureIds.reserve(kRasterMaterialGraphImageCount);
  auto isUdim = [&](const MaterialXGraphNodeCPU& node) {
    if (node.isUdim) return true;
    const int id = node.textureId;
    return (id >= 0 && id == mat.baseColorTex && mat.baseColorSample.isUdim) ||
           (id >= 0 && id == mat.metallicTex && mat.metallicSample.isUdim) ||
           (id >= 0 && id == mat.roughnessTex && mat.roughnessSample.isUdim) ||
           (id >= 0 && id == mat.normalTex && mat.normalSample.isUdim) ||
           (id >= 0 && id == mat.emissiveTex && mat.emissiveSample.isUdim) ||
           (id >= 0 && id == mat.opacityTex && mat.opacitySample.isUdim) ||
           (id >= 0 && id == mat.occlusionTex && mat.occlusionSample.isUdim) ||
           (id >= 0 && id == mat.specularColorTex && mat.specularColorSample.isUdim) ||
           (id >= 0 && id == mat.coatWeightTex && mat.coatWeightSample.isUdim) ||
           (id >= 0 && id == mat.coatColorTex && mat.coatColorSample.isUdim) ||
           (id >= 0 && id == mat.coatRoughnessTex && mat.coatRoughnessSample.isUdim) ||
           (id >= 0 && id == mat.coatNormalTex && mat.coatNormalSample.isUdim);
  };
  for (size_t i = 0; i < count; ++i) {
    const MaterialXGraphNodeCPU& node = graph.nodes[i];
    const size_t base = kRtMaterialGraphHeaderFloats +
                        i * kRtMaterialGraphNodeFloats;
    dst[base + 0] = static_cast<float>(node.op);
    dst[base + 1] = static_cast<float>(node.input[0]);
    dst[base + 2] = static_cast<float>(node.input[1]);
    dst[base + 3] = static_cast<float>(node.input[2]);
    for (int input = 0; input < 3; ++input)
      for (int lane = 0; lane < 4; ++lane)
        dst[base + 4 + input * 4 + lane] = node.value[input][lane];
    if (node.op == MaterialXGraphOpCPU::Image ||
        node.op == MaterialXGraphOpCPU::TiledImage)
      dst[base + 12] = static_cast<float>(
          MaterialXGraphColorSpaceMode(node.colorSpaceToken));
    if (node.op == MaterialXGraphOpCPU::Image ||
        node.op == MaterialXGraphOpCPU::TiledImage)
      dst[base + 13] =
          static_cast<float>(node.colorSpaceTransferCorrection);
    if (node.op == MaterialXGraphOpCPU::Normalized ||
        node.op == MaterialXGraphOpCPU::Length ||
        node.op == MaterialXGraphOpCPU::Distance) {
      dst[base + 3] = static_cast<float>(
          node.op == MaterialXGraphOpCPU::Normalized ? node.components
                                                     : node.inputComponents);
    }
    if (node.op == MaterialXGraphOpCPU::SplitLR ||
        node.op == MaterialXGraphOpCPU::SplitTB ||
        node.op == MaterialXGraphOpCPU::IfGreater ||
        node.op == MaterialXGraphOpCPU::IfGreaterEqual ||
        node.op == MaterialXGraphOpCPU::IfEqual ||
        node.op == MaterialXGraphOpCPU::Fractal2D ||
        node.op == MaterialXGraphOpCPU::Fractal3D ||
        node.op == MaterialXGraphOpCPU::Grid || node.op == MaterialXGraphOpCPU::Crosshatch ||
        node.op == MaterialXGraphOpCPU::TiledCircles || node.op == MaterialXGraphOpCPU::TiledCloverleafs ||
        node.op == MaterialXGraphOpCPU::TiledHexagons ||
        node.op == MaterialXGraphOpCPU::Ramp || node.op == MaterialXGraphOpCPU::RampGradient ||
        node.op == MaterialXGraphOpCPU::Flake ||
        node.op == MaterialXGraphOpCPU::MatrixTransform ||
        node.op == MaterialXGraphOpCPU::MatrixTranspose ||
        node.op == MaterialXGraphOpCPU::MatrixInverse ||
        node.op == MaterialXGraphOpCPU::MatrixDeterminant) {
      dst[base + 16] = static_cast<float>(node.auxInput);
      if (node.op == MaterialXGraphOpCPU::IfGreater ||
          node.op == MaterialXGraphOpCPU::IfGreaterEqual ||
          node.op == MaterialXGraphOpCPU::IfEqual ||
          node.op == MaterialXGraphOpCPU::Fractal2D ||
          node.op == MaterialXGraphOpCPU::Fractal3D ||
          node.op == MaterialXGraphOpCPU::Grid || node.op == MaterialXGraphOpCPU::Crosshatch ||
          node.op == MaterialXGraphOpCPU::TiledCircles || node.op == MaterialXGraphOpCPU::TiledCloverleafs ||
          node.op == MaterialXGraphOpCPU::TiledHexagons ||
          node.op == MaterialXGraphOpCPU::Ramp || node.op == MaterialXGraphOpCPU::RampGradient ||
          node.op == MaterialXGraphOpCPU::Flake ||
          node.op == MaterialXGraphOpCPU::MatrixTransform ||
          node.op == MaterialXGraphOpCPU::MatrixTranspose ||
          node.op == MaterialXGraphOpCPU::MatrixInverse ||
          node.op == MaterialXGraphOpCPU::MatrixDeterminant)
        for (int lane = 0; lane < 4; ++lane)
          dst[base + 17 + lane] = node.auxValue[lane];
      continue;
    }
    if (node.textureId < 0) {
      dst[base + 16] = -1.0f;
      continue;
    }
    auto found = std::find(textureIds.begin(), textureIds.end(), node.textureId);
    if (found == textureIds.end()) {
      if (textureIds.size() >= kRasterMaterialGraphImageCount) {
        dst[base + 16] = -1.0f;
        continue;
      }
      textureIds.push_back(node.textureId);
      found = textureIds.end() - 1;
    }
    const int local = static_cast<int>(found - textureIds.begin());
    dst[base + 16] = isUdim(node) ? -static_cast<float>(local + 1)
                                  : static_cast<float>(local);
    // The existing fixed record has no spare lane. For UDIM image nodes the
    // fallback alpha is not observable, so value.w carries the source texture
    // row used by the shared raster UDIM LUT.
    if (isUdim(node)) dst[base + 7] = static_cast<float>(node.textureId);
    dst[base + 17] = node.uvScale[0];
    dst[base + 18] = node.uvScale[1];
    dst[base + 19] = node.uvOffset[0];
    dst[base + 20] = node.uvOffset[1];
  }
}

void PackRasterMaterialTextureParams(const DrawMaterialCPU& mat, float* dst) {
  if (!dst) return;
  std::fill(dst, dst + kRasterMaterialTextureParamFloats, 0.0f);
  dst[67 * 4 + 0] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmission : 0.0f;
  dst[67 * 4 + 1] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmissionDepth : 0.0f;
  dst[67 * 4 + 2] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmissionDispersion : 0.0f;
  dst[67 * 4 + 3] = mat.hasLightRtOpenPBR &&
                            mat.lightRtOpenPBR.geometryThinWalled
                        ? 1.0f
                        : 0.0f;
  dst[68 * 4 + 0] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmissionColor[0] : 1.0f;
  dst[68 * 4 + 1] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmissionColor[1] : 1.0f;
  dst[68 * 4 + 2] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.transmissionColor[2] : 1.0f;
  dst[69 * 4 + 0] = mat.volumeDensity;
  dst[69 * 4 + 1] = mat.volumeEmissionScale;
  dst[70 * 4 + 0] = mat.volumeAlbedo[0];
  dst[70 * 4 + 1] = mat.volumeAlbedo[1];
  dst[70 * 4 + 2] = mat.volumeAlbedo[2];
  dst[71 * 4 + 0] = mat.volumeEmission[0];
  dst[71 * 4 + 1] = mat.volumeEmission[1];
  dst[71 * 4 + 2] = mat.volumeEmission[2];
  dst[72 * 4 + 0] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.subsurface : 0.0f;
  dst[72 * 4 + 1] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.subsurfaceScale : 1.0f;
  // Raster keeps a scalar radius for its bounded diffusion approximation;
  // reduce the authored OpenPBR radius color consistently with the other
  // scalar color reductions instead of silently discarding G/B channels.
  dst[72 * 4 + 2] = mat.hasLightRtOpenPBR
                        ? (0.2126f * mat.lightRtOpenPBR.subsurfaceRadius[0] +
                           0.7152f * mat.lightRtOpenPBR.subsurfaceRadius[1] +
                           0.0722f * mat.lightRtOpenPBR.subsurfaceRadius[2])
                        : 1.0f;
  dst[73 * 4 + 0] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.subsurfaceColor[0] : 1.0f;
  dst[73 * 4 + 1] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.subsurfaceColor[1] : 1.0f;
  dst[73 * 4 + 2] = mat.hasLightRtOpenPBR ? mat.lightRtOpenPBR.subsurfaceColor[2] : 1.0f;
  StoreUvVec4Rows(mat.baseColorSample.uv, dst + 0 * 4);
  StoreUvVec4Rows(mat.metallicSample.uv, dst + 2 * 4);
  StoreUvVec4Rows(mat.normalSample.uv, dst + 4 * 4);
  StoreUvVec4Rows(mat.emissiveSample.uv, dst + 6 * 4);
  StoreUvVec4Rows(mat.displacementUv, dst + 8 * 4);
  Store4(mat.baseColorSample.scale, dst + 10 * 4);
  Store4(mat.baseColorSample.bias, dst + 11 * 4);
  Store4(mat.normalSample.scale, dst + 12 * 4);
  Store4(mat.normalSample.bias, dst + 13 * 4);
  Store4(mat.emissiveSample.scale, dst + 14 * 4);
  Store4(mat.emissiveSample.bias, dst + 15 * 4);
  dst[16 * 4 + 0] = static_cast<float>(mat.metallicChannel);
  dst[16 * 4 + 1] = static_cast<float>(mat.roughnessChannel);
  dst[16 * 4 + 2] = mat.metallicTexScale;
  dst[16 * 4 + 3] = mat.metallicTexBias;
  dst[17 * 4 + 0] = mat.roughnessTexScale;
  dst[17 * 4 + 1] = mat.roughnessTexBias;
  // Ptex and UDIM displacement are baked before raster upload. Disable the
  // vertex-stage sample so the baked surface is not moved a second time (the
  // vertex stage cannot select a Ptex face and the CPU bake handles both paths).
  dst[17 * 4 + 2] = (mat.displacementSample.isPtex ||
                     mat.displacementSample.isUdim)
                         ? 0.0f
                         : mat.displacementTexScale;
  dst[17 * 4 + 3] = (mat.displacementSample.isPtex ||
                     mat.displacementSample.isUdim)
                         ? 0.0f
                         : mat.displacementTexBias;
  // Per-slot UV set. Displacement stays on uv0: it is sampled in the vertex /
  // tessellation stages, which do not carry the second set.
  dst[18 * 4 + 0] = static_cast<float>(mat.baseColorSample.uvSet);
  dst[18 * 4 + 1] = static_cast<float>(mat.metallicSample.uvSet);
  dst[18 * 4 + 2] = static_cast<float>(mat.normalSample.uvSet);
  dst[18 * 4 + 3] = static_cast<float>(mat.emissiveSample.uvSet);
  // Specular F0 (T12): rgb = inputs:specularColor, w = ior with the specular-
  // workflow flag folded into its SIGN (w < 0 => use specularColor directly as
  // F0; w >= 0 => dielectric F0 from |ior|, lerped to base by metalness). ior is
  // always positive, so the sign is a free flag and no push-constant lane is
  // needed.
  dst[19 * 4 + 0] = mat.specularColor[0];
  dst[19 * 4 + 1] = mat.specularColor[1];
  dst[19 * 4 + 2] = mat.specularColor[2];
  dst[19 * 4 + 3] = mat.useSpecularWorkflow
                         ? -mat.ior
                         : (mat.openPbrSpecularModel ? mat.ior + 100.0f
                                                     : mat.ior);
  StoreUvVec4Rows(mat.opacitySample.uv, dst + 20 * 4);
  dst[22 * 4 + 0] = static_cast<float>(mat.opacityChannel);
  dst[22 * 4 + 1] = mat.opacityTexScale;
  dst[22 * 4 + 2] = mat.opacityTexBias;
  dst[22 * 4 + 3] = static_cast<float>(mat.opacitySample.uvSet);
  // Keep atlas coordinates valid even for an unbound slot. Feature flags gate
  // all actual samples, but some software Vulkan compilers speculate both sides
  // of the texture branch and otherwise form a negative image coordinate.
  dst[23 * 4 + 0] = static_cast<float>(std::max(mat.baseColorTex, 0));
  dst[23 * 4 + 1] = static_cast<float>(std::max(mat.metallicTex, 0));
  dst[23 * 4 + 2] = static_cast<float>(std::max(mat.normalTex, 0));
  dst[23 * 4 + 3] = static_cast<float>(std::max(mat.emissiveTex, 0));
  dst[24 * 4 + 0] = static_cast<float>(std::max(mat.opacityTex, 0));
  dst[24 * 4 + 1] = static_cast<float>(std::max(mat.roughnessTex, 0));
  StoreUvVec4Rows(mat.roughnessSample.uv, dst + 25 * 4);
  // roughUv0.w is otherwise padding and carries its UV-set selector.
  dst[25 * 4 + 3] = static_cast<float>(mat.roughnessSample.uvSet);
  dst[27 * 4 + 0] = mat.coatWeight;
  dst[27 * 4 + 1] = mat.coatRoughness;
  dst[27 * 4 + 2] = mat.coatIor;
  dst[27 * 4 + 3] = mat.occlusion;
  dst[28 * 4 + 0] = mat.coatColor[0];
  dst[28 * 4 + 1] = mat.coatColor[1];
  dst[28 * 4 + 2] = mat.coatColor[2];
  StoreUvVec4Rows(mat.occlusionSample.uv, dst + 29 * 4);
  dst[31 * 4 + 0] = static_cast<float>(mat.occlusionChannel);
  dst[31 * 4 + 1] = mat.occlusionTexScale;
  dst[31 * 4 + 2] = mat.occlusionTexBias;
  dst[31 * 4 + 3] = static_cast<float>(mat.occlusionSample.uvSet);
  dst[24 * 4 + 2] = static_cast<float>(std::max(mat.occlusionTex, 0));
  // The ordinary-binding push flag distinguishes a 2D specular map from a
  // UDIM map; retain -1 here only when the semantic slot is genuinely absent.
  dst[24 * 4 + 3] = static_cast<float>(mat.specularColorTex);
  // Extra semantic slots. The loaders neutralize the matching constant to 1.0
  // when a texture is bound, so the shader always multiplies constant * texel.
  StoreUvVec4Rows(mat.specularColorSample.uv, dst + 32 * 4);
  StoreUvVec4Rows(mat.coatWeightSample.uv, dst + 34 * 4);
  StoreUvVec4Rows(mat.coatColorSample.uv, dst + 36 * 4);
  StoreUvVec4Rows(mat.coatRoughnessSample.uv, dst + 38 * 4);
  // A negative channel selector means "whole value"; the scalar coat slots
  // default that to channel 0 (R).
  dst[40 * 4 + 0] = static_cast<float>(
      mat.coatWeightSample.channel < 0 ? 0 : mat.coatWeightSample.channel);
  dst[40 * 4 + 1] = static_cast<float>(
      mat.coatRoughnessSample.channel < 0 ? 0
                                          : mat.coatRoughnessSample.channel);
  dst[40 * 4 + 2] = static_cast<float>(mat.coatWeightSample.uvSet);
  dst[40 * 4 + 3] = static_cast<float>(mat.coatRoughnessSample.uvSet);
  dst[41 * 4 + 0] = static_cast<float>(mat.specularColorSample.uvSet);
  dst[41 * 4 + 1] = static_cast<float>(mat.coatColorSample.uvSet);
  Store4(mat.specularColorSample.scale, dst + 42 * 4);
  Store4(mat.specularColorSample.bias, dst + 43 * 4);
  Store4(mat.coatWeightSample.scale, dst + 44 * 4);
  Store4(mat.coatWeightSample.bias, dst + 45 * 4);
  Store4(mat.coatColorSample.scale, dst + 46 * 4);
  Store4(mat.coatColorSample.bias, dst + 47 * 4);
  Store4(mat.coatRoughnessSample.scale, dst + 48 * 4);
  Store4(mat.coatRoughnessSample.bias, dst + 49 * 4);
  StoreUvVec4Rows(mat.coatNormalSample.uv, dst + 50 * 4);
  Store4(mat.coatNormalSample.scale, dst + 52 * 4);
  Store4(mat.coatNormalSample.bias, dst + 53 * 4);
  dst[53 * 4 + 3] = static_cast<float>(mat.coatNormalSample.uvSet);
  dst[52 * 4 + 3] = mat.coatNormalTex >= 0 ? 1.0f : 0.0f;
  dst[54 * 4 + 0] = static_cast<float>(mat.specularColorTex);
  dst[54 * 4 + 1] = static_cast<float>(mat.coatWeightTex);
  dst[54 * 4 + 2] = static_cast<float>(mat.coatColorTex);
  dst[54 * 4 + 3] = static_cast<float>(mat.coatRoughnessTex);
  dst[55 * 4 + 0] = mat.coatNormalSample.isUdim
                         ? static_cast<float>(mat.coatNormalTex)
                         : -1.0f;
  dst[55 * 4 + 1] = mat.displacementSample.isUdim
                         ? static_cast<float>(mat.displacementTex)
                         : -1.0f;
  // Ptex base-color atlas: (rect texel offset, face count, enabled, reserved).
  // The face id itself is fetched from the per-triangle source-face SSBO.
  dst[56 * 4 + 0] = mat.baseColorSample.isPtex
                         ? static_cast<float>(
                               mat.baseColorSample.ptexRectTexelOffset)
                         : 0.0f;
  dst[56 * 4 + 1] = mat.baseColorSample.isPtex
                         ? static_cast<float>(mat.baseColorSample.ptexFaceCount)
                         : 0.0f;
  dst[56 * 4 + 2] = mat.baseColorSample.isPtex
                         ? 1.0f
                         : 0.0f;
  dst[56 * 4 + 3] = 0.0f;
  auto packPtexInfo = [dst](int slot, const DrawTexSampleCPU& sample) {
    dst[slot * 4 + 0] = sample.isPtex
                             ? static_cast<float>(sample.ptexRectTexelOffset)
                             : 0.0f;
    dst[slot * 4 + 1] = sample.isPtex
                             ? static_cast<float>(sample.ptexFaceCount)
                             : 0.0f;
    dst[slot * 4 + 2] = sample.isPtex ? 1.0f : 0.0f;
    dst[slot * 4 + 3] = 0.0f;
  };
  packPtexInfo(57, mat.metallicSample);
  packPtexInfo(58, mat.roughnessSample);
  packPtexInfo(59, mat.normalSample);
  packPtexInfo(60, mat.emissiveSample);
  packPtexInfo(61, mat.opacitySample);
  packPtexInfo(62, mat.occlusionSample);
  packPtexInfo(63, mat.specularColorSample);
  packPtexInfo(64, mat.coatWeightSample);
  packPtexInfo(65, mat.coatColorSample);
  packPtexInfo(66, mat.coatRoughnessSample);
}

}  // namespace lusdview
