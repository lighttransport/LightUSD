// SPDX-License-Identifier: Apache-2.0
#include "lightrt_mtlx_bridge.hh"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <unordered_map>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "image-loader.hh"

extern "C" {
#include "external/lightrt/mtlxrender/mtlx_doc.h"
#include "external/lightrt/mtlxrender/mtlx_eval.h"
#include "external/lightrt/mtlxrender/texture.h"
}

namespace lusdview {

uint32_t MaterialXGeomPropHash(const std::string& name) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name) {
    h ^= static_cast<uint32_t>(c);
    h *= 16777619u;
  }
  return h;
}
namespace {

const DrawMaterialParamCPU* FindParam(const DrawMaterialCPU& mat,
                                      const char* shader,
                                      const char* name) {
  for (const DrawMaterialParamCPU& p : mat.params) {
    if (p.shader == shader && p.name == name) return &p;
  }
  return nullptr;
}

const DrawMaterialParamCPU* FindAny(const DrawMaterialCPU& mat,
                                    std::initializer_list<const char*> shaders,
                                    std::initializer_list<const char*> names) {
  for (const char* shader : shaders) {
    for (const char* name : names) {
      if (const DrawMaterialParamCPU* p = FindParam(mat, shader, name)) return p;
    }
  }
  return nullptr;
}

bool FloatParam(const DrawMaterialCPU& mat,
                std::initializer_list<const char*> shaders,
                std::initializer_list<const char*> names, float* out) {
  const DrawMaterialParamCPU* p = FindAny(mat, shaders, names);
  if (!p) return false;
  *out = p->value[0];
  return true;
}

bool Vec3Param(const DrawMaterialCPU& mat,
               std::initializer_list<const char*> shaders,
               std::initializer_list<const char*> names, float out[3]) {
  const DrawMaterialParamCPU* p = FindAny(mat, shaders, names);
  if (!p) return false;
  out[0] = p->value[0];
  out[1] = p->value[1];
  out[2] = p->value[2];
  return true;
}

bool ParamHasTexture(const DrawMaterialCPU& mat,
                     std::initializer_list<const char*> shaders,
                     std::initializer_list<const char*> names) {
  for (const char* shader : shaders) {
    for (const char* name : names) {
      const DrawMaterialParamCPU* p = FindParam(mat, shader, name);
      if (p && (p->texture >= 0 || p->renderTexture >= 0)) return true;
    }
  }
  return false;
}

bool HasTextureInput(const DrawMaterialCPU& mat) {
  for (const DrawMaterialParamCPU& p : mat.params) {
    if (p.texture >= 0 || p.renderTexture >= 0) return true;
  }
  return false;
}

bool IsNeutralNormalValue(const DrawMaterialParamCPU& p) {
  return std::fabs(p.value[0]) <= 1.0e-6f &&
         std::fabs(p.value[1]) <= 1.0e-6f &&
         std::fabs(p.value[2] - 1.0f) <= 1.0e-6f;
}

bool IsNormalParam(const DrawMaterialParamCPU& p) {
  const bool shader =
      p.shader == "OpenPBRSurface" || p.shader == "UsdPreviewSurface";
  const bool name = p.name == "normal" || p.name == "geometry_normal" ||
                    p.name == "coat_normal";
  return shader && name;
}

bool HasNormalInput(const DrawMaterialCPU& mat) {
  for (const DrawMaterialParamCPU& p : mat.params) {
    if (!IsNormalParam(p)) continue;
    if (p.texture >= 0 || p.renderTexture >= 0 || !IsNeutralNormalValue(p)) {
      return true;
    }
  }
  return false;
}

float Luminance(const float rgb[3]) {
  return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

void ClampLightRtParams(tydra::LightRtOpenPBRParams* p) {
  lightusd::tydra::ClampLightRtOpenPBRParams(p);
}


void BakeUsdPreviewSurface(const DrawMaterialCPU& mat, tydra::LightRtOpenPBRParams* p) {
  // Seed from the material's DIRECT fields first. The --core loader
  // (core_scene_loader.cc) sets baseColor/metallic/roughness/specularColor/ior/
  // alpha/emissive directly and does NOT populate mat.params, so the param
  // lookups below leave p at its constructor defaults (gray 0.8, etc.), and
  // BakeLightRtOpenPBR then copies those defaults back OVER the loader's values
  // -- graying out every untextured constant-color material on the core path.
  // The legacy path sets both the direct fields AND the params, so its lookups
  // override this seed with identical values (no behavior change there).
  p->baseColor[0] = mat.baseColor[0];
  p->baseColor[1] = mat.baseColor[1];
  p->baseColor[2] = mat.baseColor[2];
  p->specularColor[0] = mat.specularColor[0];
  p->specularColor[1] = mat.specularColor[1];
  p->specularColor[2] = mat.specularColor[2];
  p->metalness = mat.metallic;
  p->specularRoughness = mat.roughness;
  p->specularIor = mat.ior;
  p->opacity = mat.alpha;
  p->emissionColor[0] = mat.emissive[0];
  p->emissionColor[1] = mat.emissive[1];
  p->emissionColor[2] = mat.emissive[2];
  p->emission = (Luminance(p->emissionColor) > 0.0f) ? 1.0f : 0.0f;

  Vec3Param(mat, {"UsdPreviewSurface"}, {"diffuseColor"}, p->baseColor);
  Vec3Param(mat, {"UsdPreviewSurface"}, {"specularColor"}, p->specularColor);
  FloatParam(mat, {"UsdPreviewSurface"}, {"metallic"}, &p->metalness);
  FloatParam(mat, {"UsdPreviewSurface"}, {"roughness"}, &p->specularRoughness);
  FloatParam(mat, {"UsdPreviewSurface"}, {"ior"}, &p->specularIor);
  FloatParam(mat, {"UsdPreviewSurface"}, {"clearcoat"}, &p->coatWeight);
  FloatParam(mat, {"UsdPreviewSurface"}, {"clearcoatRoughness"}, &p->coatRoughness);
  FloatParam(mat, {"UsdPreviewSurface"}, {"opacity"}, &p->opacity);
  Vec3Param(mat, {"UsdPreviewSurface"}, {"normal"}, p->normal);
  if (Vec3Param(mat, {"UsdPreviewSurface"}, {"emissiveColor"}, p->emissionColor)) {
    p->emission = (Luminance(p->emissionColor) > 0.0f) ? 1.0f : 0.0f;
  }

  // Params retain the authored constant even when Tydra also resolved a live
  // UVTexture connection. The raster fields above have already neutralized
  // those constants; do the same for the canonical LightRT block, otherwise
  // Vulkan ray query/CUDA multiply the texel by the stale fallback (most
  // visibly metallic=0 and emissiveColor=(0,0,0)).
  if (mat.baseColorTex >= 0) {
    p->baseColor[0] = p->baseColor[1] = p->baseColor[2] = 1.0f;
  }
  if (mat.metallicTex >= 0) p->metalness = 1.0f;
  if (mat.roughnessTex >= 0) p->specularRoughness = 1.0f;
  if (mat.emissiveTex >= 0) {
    p->emissionColor[0] = p->emissionColor[1] = p->emissionColor[2] = 1.0f;
    p->emission = 1.0f;
  }
  if (mat.opacityTex >= 0) p->opacity = 1.0f;
  // Match Tydra's BuildRealtimePbrMaterial: inputs:specularColor is a
  // UsdPreviewSurface *specular-workflow* input whose schema fallback is
  // (0,0,0), so carrying it into the metallic workflow tints the whole
  // specular lobe to black. A bound texture is a multiplicative fallback and
  // is neutralised too. Without this the legacy and core loaders shade the
  // same UsdPreviewSurface differently (measured 3 levels on the linked-light
  // fixture).
  if (!mat.useSpecularWorkflow || mat.specularColorTex >= 0) {
    p->specularColor[0] = p->specularColor[1] = p->specularColor[2] = 1.0f;
  }
  if (mat.coatWeightTex >= 0) p->coatWeight = 1.0f;
  if (mat.coatColorTex >= 0) {
    p->coatColor[0] = p->coatColor[1] = p->coatColor[2] = 1.0f;
  }
  if (mat.coatRoughnessTex >= 0) p->coatRoughness = 1.0f;
  // Match Tydra's BuildRealtimePbrMaterial: the metallic workflow selects
  // Storm's preview F0/F90 model (slot-41 sentinel) in the RT shaders. Without
  // this the legacy loader shades the same UsdPreviewSurface differently.
  p->previewSurfaceMetallicWorkflow = !mat.useSpecularWorkflow;
}

void BakeOpenPBRSurface(const DrawMaterialCPU& mat, tydra::LightRtOpenPBRParams* p) {
  FloatParam(mat, {"OpenPBRSurface"}, {"base_weight"}, &p->baseWeight);
  Vec3Param(mat, {"OpenPBRSurface"}, {"base_color"}, p->baseColor);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"base_color"})) {
    p->baseColor[0] = 1.0f;
    p->baseColor[1] = 1.0f;
    p->baseColor[2] = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"base_diffuse_roughness"},
             &p->diffuseRoughness);
  FloatParam(mat, {"OpenPBRSurface"}, {"base_roughness"},
             &p->specularRoughness);
  FloatParam(mat, {"OpenPBRSurface"}, {"base_metalness"}, &p->metalness);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"base_metalness"})) {
    p->metalness = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_weight"}, &p->specularWeight);
  Vec3Param(mat, {"OpenPBRSurface"}, {"specular_color"}, p->specularColor);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"specular_color"})) {
    p->specularColor[0] = p->specularColor[1] = p->specularColor[2] = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_roughness"},
             &p->specularRoughness);
  FloatParam(mat, {"OpenPBRSurface"}, {"base_roughness"},
             &p->specularRoughness);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"base_roughness",
                                                "specular_roughness"})) {
    p->specularRoughness = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_ior"}, &p->specularIor);
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_anisotropy"},
             &p->specularAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_rotation"},
             &p->specularRotation);
  FloatParam(mat, {"OpenPBRSurface"}, {"specular_roughness_anisotropy"},
             &p->specularRoughnessAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_weight"},
             &p->transmission);
  Vec3Param(mat, {"OpenPBRSurface"}, {"transmission_color"},
            p->transmissionColor);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_depth"},
             &p->transmissionDepth);
  Vec3Param(mat, {"OpenPBRSurface"}, {"transmission_scatter"},
            p->transmissionScatter);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_scatter_anisotropy"},
             &p->transmissionScatterAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_dispersion"},
             &p->transmissionDispersion);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_dispersion_abbe_number"},
             &p->transmissionDispersionAbbeNumber);
  FloatParam(mat, {"OpenPBRSurface"}, {"transmission_dispersion_scale"},
             &p->transmissionDispersionScale);
  FloatParam(mat, {"OpenPBRSurface"}, {"subsurface_weight"}, &p->subsurface);
  Vec3Param(mat, {"OpenPBRSurface"}, {"subsurface_color"}, p->subsurfaceColor);
  float radius = p->subsurfaceRadius[0];
  if (FloatParam(mat, {"OpenPBRSurface"}, {"subsurface_radius"}, &radius)) {
    p->subsurfaceRadius[0] = radius;
    p->subsurfaceRadius[1] = radius;
    p->subsurfaceRadius[2] = radius;
  }
  float radiusScale[3]{1.0f, 1.0f, 1.0f};
  if (Vec3Param(mat, {"OpenPBRSurface"}, {"subsurface_radius_scale"},
                radiusScale)) {
    p->subsurfaceRadius[0] *= radiusScale[0];
    p->subsurfaceRadius[1] *= radiusScale[1];
    p->subsurfaceRadius[2] *= radiusScale[2];
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"subsurface_scale"}, &p->subsurfaceScale);
  FloatParam(mat, {"OpenPBRSurface"}, {"subsurface_anisotropy"},
             &p->subsurfaceAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"subsurface_scatter_anisotropy"},
             &p->subsurfaceScatterAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_weight"}, &p->coatWeight);
  Vec3Param(mat, {"OpenPBRSurface"}, {"coat_color"}, p->coatColor);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_roughness"}, &p->coatRoughness);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"coat_weight"})) {
    p->coatWeight = 1.0f;
  }
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"coat_color"})) {
    p->coatColor[0] = p->coatColor[1] = p->coatColor[2] = 1.0f;
  }
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"coat_roughness"})) {
    p->coatRoughness = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_ior"}, &p->coatIor);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_anisotropy"}, &p->coatAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_rotation"}, &p->coatRotation);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_affect_color"}, &p->coatAffectColor);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_affect_roughness"},
             &p->coatAffectRoughness);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_roughness_anisotropy"},
             &p->coatRoughnessAnisotropy);
  FloatParam(mat, {"OpenPBRSurface"}, {"coat_darkening"}, &p->coatDarkening);
  FloatParam(mat, {"OpenPBRSurface"}, {"sheen_weight", "fuzz_weight"},
             &p->sheenWeight);
  Vec3Param(mat, {"OpenPBRSurface"}, {"sheen_color", "fuzz_color"},
            p->sheenColor);
  FloatParam(mat, {"OpenPBRSurface"}, {"sheen_roughness", "fuzz_roughness"},
             &p->sheenRoughness);
  FloatParam(mat, {"OpenPBRSurface"}, {"thin_film_weight"}, &p->thinFilmWeight);
  if (FloatParam(mat, {"OpenPBRSurface"}, {"thin_film_thickness"},
                 &p->thinFilmThicknessNm)) {
    // OpenPBR and the LightRT evaluator both use nanometers for film
    // thickness. Keep the canonical value unchanged; multiplying here would
    // turn the documented 450 nm test film into a 450,000 nm layer.
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"thin_film_ior"}, &p->thinFilmIor);
  FloatParam(mat, {"OpenPBRSurface"}, {"emission_luminance"}, &p->emission);
  Vec3Param(mat, {"OpenPBRSurface"}, {"emission_color"}, p->emissionColor);
  if (ParamHasTexture(mat, {"OpenPBRSurface"}, {"emission_color"})) {
    p->emissionColor[0] = 1.0f;
    p->emissionColor[1] = 1.0f;
    p->emissionColor[2] = 1.0f;
  }
  FloatParam(mat, {"OpenPBRSurface"}, {"geometry_opacity", "opacity"},
             &p->opacity);
  float thinWalled = p->geometryThinWalled ? 1.0f : 0.0f;
  if (FloatParam(mat, {"OpenPBRSurface"},
                 {"geometry_thin_walled", "thin_walled"}, &thinWalled)) {
    p->geometryThinWalled = thinWalled >= 0.5f;
  }
  if (ParamHasTexture(mat, {"OpenPBRSurface"},
                      {"geometry_opacity", "opacity"})) {
    p->opacity = 1.0f;
  }
  Vec3Param(mat, {"OpenPBRSurface"}, {"geometry_normal", "normal"}, p->normal);
}

void CopyV3(const v3& src, float dst[3]) {
  dst[0] = src.x;
  dst[1] = src.y;
  dst[2] = src.z;
}

void CopyLightRtEval(const OpenPBRParams& src, DrawLightRtOpenPBRCPU* dst) {
  dst->baseWeight = src.base_weight;
  CopyV3(src.base_color, dst->baseColor);
  dst->diffuseRoughness = src.diffuse_roughness;
  dst->metalness = src.metalness;
  dst->specularWeight = src.specular_weight;
  CopyV3(src.specular_color, dst->specularColor);
  dst->specularRoughness = src.specular_roughness;
  dst->specularIor = src.specular_ior;
  dst->transmission = src.transmission;
  CopyV3(src.transmission_color, dst->transmissionColor);
  dst->transmissionDepth = src.transmission_depth;
  CopyV3(src.transmission_scatter, dst->transmissionScatter);
  dst->transmissionScatterAnisotropy = src.transmission_scatter_anisotropy;
  dst->subsurface = src.subsurface;
  CopyV3(src.subsurface_color, dst->subsurfaceColor);
  CopyV3(src.subsurface_radius, dst->subsurfaceRadius);
  dst->subsurfaceScale = src.subsurface_scale;
  dst->subsurfaceAnisotropy = src.subsurface_anisotropy;
  dst->coatWeight = src.coat_weight;
  CopyV3(src.coat_color, dst->coatColor);
  dst->coatRoughness = src.coat_roughness;
  dst->coatIor = src.coat_ior;
  dst->sheenWeight = src.sheen_weight;
  CopyV3(src.sheen_color, dst->sheenColor);
  dst->sheenRoughness = src.sheen_roughness;
  dst->thinFilmWeight = src.thin_film_weight;
  dst->thinFilmThicknessNm = src.thin_film_thickness;
  dst->thinFilmIor = src.thin_film_ior;
  dst->emission = src.emission;
  CopyV3(src.emission_color, dst->emissionColor);
  CopyV3(src.normal, dst->normal);
  dst->opacity = src.opacity;
}

std::string OpenPBREvalInputName(const std::string& name);
std::string NormalizeMtlxCategory(const std::string& category,
                                  const std::string& type);

std::set<std::string> DirectTextureInputs(const DrawMaterialCPU& mat) {
  std::set<std::string> deps;
  for (const DrawMaterialParamCPU& p : mat.params) {
    if (p.shader != "OpenPBRSurface") continue;
    if (p.texture < 0 && p.renderTexture < 0) continue;
    deps.insert(OpenPBREvalInputName(p.name));
  }
  return deps;
}

bool TextureDepsIncludeNormal(const std::set<std::string>& textureDeps) {
  return textureDeps.find("geometry_normal") != textureDeps.end() ||
         textureDeps.find("geometry_coat_normal") != textureDeps.end();
}

void MergeGraphParamsPreservingTextureDeps(
    const DrawLightRtOpenPBRCPU& graph,
    const std::set<std::string>& textureDeps,
    DrawLightRtOpenPBRCPU* dst) {
  if (!dst) return;
  auto safe = [&](const char* name) {
    return textureDeps.find(name) == textureDeps.end();
  };
  auto copy3 = [](const float src[3], float dst3[3]) {
    dst3[0] = src[0];
    dst3[1] = src[1];
    dst3[2] = src[2];
  };

  if (safe("base_weight")) dst->baseWeight = graph.baseWeight;
  if (safe("base_color")) copy3(graph.baseColor, dst->baseColor);
  if (safe("base_metalness")) dst->metalness = graph.metalness;
  if (safe("base_diffuse_roughness")) {
    dst->diffuseRoughness = graph.diffuseRoughness;
  }
  if (safe("specular_weight")) dst->specularWeight = graph.specularWeight;
  if (safe("specular_color")) copy3(graph.specularColor, dst->specularColor);
  if (safe("specular_roughness")) {
    dst->specularRoughness = graph.specularRoughness;
  }
  if (safe("specular_ior")) dst->specularIor = graph.specularIor;
  if (safe("transmission_weight")) dst->transmission = graph.transmission;
  if (safe("transmission_color")) {
    copy3(graph.transmissionColor, dst->transmissionColor);
  }
  if (safe("transmission_depth")) dst->transmissionDepth = graph.transmissionDepth;
  if (safe("transmission_scatter")) {
    copy3(graph.transmissionScatter, dst->transmissionScatter);
  }
  if (safe("transmission_scatter_anisotropy")) {
    dst->transmissionScatterAnisotropy =
        graph.transmissionScatterAnisotropy;
  }
  if (safe("subsurface_weight")) dst->subsurface = graph.subsurface;
  if (safe("subsurface_color")) {
    copy3(graph.subsurfaceColor, dst->subsurfaceColor);
  }
  if (safe("subsurface_radius")) {
    copy3(graph.subsurfaceRadius, dst->subsurfaceRadius);
  }
  if (safe("subsurface_radius_scale")) {
    copy3(graph.subsurfaceRadius, dst->subsurfaceRadius);
  }
  if (safe("subsurface_anisotropy")) {
    dst->subsurfaceAnisotropy = graph.subsurfaceAnisotropy;
  }
  if (safe("subsurface_scatter_anisotropy")) {
    dst->subsurfaceScatterAnisotropy = graph.subsurfaceScatterAnisotropy;
  }
  if (safe("coat_weight")) dst->coatWeight = graph.coatWeight;
  if (safe("coat_color")) copy3(graph.coatColor, dst->coatColor);
  if (safe("coat_roughness")) dst->coatRoughness = graph.coatRoughness;
  if (safe("coat_ior")) dst->coatIor = graph.coatIor;
  if (safe("fuzz_weight")) dst->sheenWeight = graph.sheenWeight;
  if (safe("fuzz_color")) copy3(graph.sheenColor, dst->sheenColor);
  if (safe("fuzz_roughness")) dst->sheenRoughness = graph.sheenRoughness;
  if (safe("thin_film_weight")) dst->thinFilmWeight = graph.thinFilmWeight;
  if (safe("thin_film_thickness")) {
    dst->thinFilmThicknessNm = graph.thinFilmThicknessNm;
  }
  if (safe("thin_film_ior")) dst->thinFilmIor = graph.thinFilmIor;
  if (safe("emission_luminance")) dst->emission = graph.emission;
  if (safe("emission_color")) copy3(graph.emissionColor, dst->emissionColor);
  if (safe("geometry_opacity")) dst->opacity = graph.opacity;
  if (safe("geometry_normal")) copy3(graph.normal, dst->normal);
}

std::string XmlEscape(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (char c : src) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c; break;
    }
  }
  return out;
}

bool IsMtlxTypeName(const std::string& s) {
  return s == "float" || s == "color3" || s == "color4" ||
         s == "vector2" || s == "vector3" || s == "vector4" ||
         s == "matrix33" || s == "matrix44" ||
         s == "integer" || s == "boolean" || s == "string" ||
         s == "filename";
}

std::string NormalizeMtlxType(const std::string& type) {
  if (IsMtlxTypeName(type)) return type;
  if (type == "float2") return "vector2";
  if (type == "float3") return "vector3";
  if (type == "float4") return "vector4";
  if (type == "color3f") return "color3";
  if (type == "color4f") return "color4";
  if (type == "asset") return "filename";
  if (type.rfind("ND_", 0) == 0) {
    const size_t last = type.rfind('_');
    if (last != std::string::npos && last + 1 < type.size()) {
      const std::string suffix = type.substr(last + 1);
      if (IsMtlxTypeName(suffix)) return suffix;
    }
  }
  return "float";
}

std::string TypeForParam(DrawMaterialParamType type) {
  switch (type) {
    case DrawMaterialParamType::Vec2: return "vector2";
    case DrawMaterialParamType::Vec3: return "color3";
    case DrawMaterialParamType::Vec4: return "color4";
    case DrawMaterialParamType::Float:
    default: return "float";
  }
}

int ComponentCount(DrawMaterialParamType type) {
  switch (type) {
    case DrawMaterialParamType::Vec2: return 2;
    case DrawMaterialParamType::Vec3: return 3;
    case DrawMaterialParamType::Vec4: return 4;
    case DrawMaterialParamType::Float:
    default: return 1;
  }
}

std::string ValueForParam(const DrawMaterialParamCPU& p) {
  std::stringstream ss;
  const int n = ComponentCount(p.type);
  for (int i = 0; i < n; ++i) {
    if (i) ss << ", ";
    ss << p.value[i];
  }
  return ss.str();
}

std::string JsonString(const nlohmann::json& obj, const char* key,
                       const std::string& fallback = std::string()) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return fallback;
  return it->get<std::string>();
}

bool IsTextureNodeCategory(const std::string& category) {
  return category == "image" || category == "tiledimage" ||
         category == "hextiledimage" || category == "gltf_image" ||
         category == "gltf_colorimage";
}

bool JsonNodeHasFileInput(const nlohmann::json& node) {
  const auto inputsIt = node.find("inputs");
  if (inputsIt == node.end() || !inputsIt->is_array()) return false;
  for (const nlohmann::json& input : *inputsIt) {
    const std::string name = JsonString(input, "name");
    const std::string type = NormalizeMtlxType(JsonString(input, "type"));
    if (name == "file" || type == "filename") return true;
  }
  return false;
}

struct GraphTextureDeps {
  bool parsed{false};
  bool hasTextureNodes{false};
  std::set<std::string> textureInputs;
};

GraphTextureDeps AnalyzeMaterialXJsonTextureDeps(const std::string& jsonText) {
  GraphTextureDeps result;
  if (jsonText.empty()) return result;
  nlohmann::json j = nlohmann::json::parse(
      jsonText.begin(), jsonText.end(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) return result;
  const auto ngIt = j.find("nodegraph");
  if (ngIt == j.end() || !ngIt->is_object()) return result;
  result.parsed = true;
  const nlohmann::json& ng = *ngIt;

  std::map<std::string, nlohmann::json> nodes;
  const auto nodesIt = ng.find("nodes");
  if (nodesIt != ng.end() && nodesIt->is_array()) {
    for (const nlohmann::json& node : *nodesIt) {
      const std::string name = JsonString(node, "name");
      if (!name.empty()) nodes[name] = node;
      const std::string category = NormalizeMtlxCategory(
          JsonString(node, "category"), JsonString(node, "type"));
      if (IsTextureNodeCategory(category) ||
          (category == "normalmap" && JsonNodeHasFileInput(node))) {
        result.hasTextureNodes = true;
      }
    }
  }

  std::map<std::string, std::string> outputs;
  const auto outputsIt = ng.find("outputs");
  if (outputsIt != ng.end() && outputsIt->is_array()) {
    for (const nlohmann::json& output : *outputsIt) {
      const std::string name = JsonString(output, "name");
      const std::string nodeName = JsonString(output, "nodename");
      if (!name.empty() && !nodeName.empty()) outputs[name] = nodeName;
    }
  }

  std::map<std::string, bool> memo;
  std::set<std::string> visiting;
  std::function<bool(const std::string&)> nodeDependsOnTexture =
      [&](const std::string& nodeName) -> bool {
        const auto memoIt = memo.find(nodeName);
        if (memoIt != memo.end()) return memoIt->second;
        if (visiting.count(nodeName)) return false;
        visiting.insert(nodeName);

        bool dep = false;
        const auto nodeIt = nodes.find(nodeName);
        if (nodeIt != nodes.end()) {
          const nlohmann::json& node = nodeIt->second;
          const std::string category = NormalizeMtlxCategory(
              JsonString(node, "category"), JsonString(node, "type"));
          dep = IsTextureNodeCategory(category) ||
                (category == "normalmap" && JsonNodeHasFileInput(node));
          const auto inputsIt = node.find("inputs");
          if (!dep && inputsIt != node.end() && inputsIt->is_array()) {
            for (const nlohmann::json& input : *inputsIt) {
              const std::string srcNode = JsonString(input, "nodename");
              if (!srcNode.empty() && nodeDependsOnTexture(srcNode)) {
                dep = true;
                break;
              }
            }
          }
        }

        visiting.erase(nodeName);
        memo[nodeName] = dep;
        return dep;
      };

  const auto connIt = j.find("connections");
  if (connIt != j.end() && connIt->is_array()) {
    for (const nlohmann::json& c : *connIt) {
      const std::string input = OpenPBREvalInputName(JsonString(c, "input"));
      const std::string output = JsonString(c, "output");
      if (input.empty() || output.empty()) continue;
      const auto outIt = outputs.find(output);
      if (outIt != outputs.end() && nodeDependsOnTexture(outIt->second)) {
        result.textureInputs.insert(input);
      }
    }
  }
  return result;
}

std::string JsonValueToMtlxString(const nlohmann::json& v) {
  std::stringstream ss;
  if (v.is_number_float() || v.is_number_integer() || v.is_number_unsigned()) {
    ss << v.get<double>();
  } else if (v.is_boolean()) {
    ss << (v.get<bool>() ? "true" : "false");
  } else if (v.is_string()) {
    ss << v.get<std::string>();
  } else if (v.is_array()) {
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) ss << ", ";
      ss << JsonValueToMtlxString(v[i]);
    }
  }
  return ss.str();
}

std::string InferTypeFromJsonValue(const nlohmann::json& v) {
  if (v.is_boolean()) return "boolean";
  if (v.is_string()) return "string";
  if (v.is_array()) {
    if (v.size() == 2) return "vector2";
    if (v.size() == 3) return "color3";
    if (v.size() == 4) return "color4";
  }
  return "float";
}

std::string OpenPBREvalInputName(const std::string& name) {
  // Standard Surface and common interchange graphs use shorter parameter
  // names. The runtime bake target is OpenPBR, so normalize these aliases at
  // the graph boundary instead of silently dropping connected inputs.
  if (name == "roughness") return "specular_roughness";
  if (name == "metalness") return "base_metalness";
  if (name == "specular") return "specular_weight";
  if (name == "transmission") return "transmission_weight";
  if (name == "subsurface") return "subsurface_weight";
  if (name == "coat") return "coat_weight";
  if (name == "emission") return "emission_luminance";
  if (name == "base_roughness") return "specular_roughness";
  if (name == "opacity") return "geometry_opacity";
  if (name == "normal") return "geometry_normal";
  if (name == "tangent") return "geometry_tangent";
  if (name == "coat_normal") return "geometry_coat_normal";
  if (name == "coat_tangent") return "geometry_coat_tangent";
  return name;
}

std::string NormalizeMtlxCategory(const std::string& category,
                                  const std::string& type) {
  if (category == "MaterialXMultiply") return "multiply";
  if (category == "MaterialXMix") return "mix";
  if (category == "MaterialXNoise") return "noise3d";
  if (category == "MaterialXConstant") return "constant";

  if (category.rfind("ND_", 0) == 0) {
    std::string stem = category.substr(3);
    const std::string normalized_type = NormalizeMtlxType(type);
    if (!normalized_type.empty()) {
      const std::string suffix = "_" + normalized_type;
      if (stem.size() > suffix.size() &&
          stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
        stem.resize(stem.size() - suffix.size());
      }
    }
    if (stem == "gltf_colorimage") return "gltf_colorimage";
    if (stem == "gltf_image") return "gltf_image";
    if (stem == "gltf_normalmap") return "gltf_normalmap";
    // Swizzle nodedef names carry both input and output types (for example
    // ND_swizzle_color4_color3). Removing only the output suffix leaves the
    // input type in the stem; the runtime operation itself is type-agnostic.
    if (stem.rfind("swizzle_", 0) == 0) return "swizzle";
    if (stem == "open_pbr_surface_surfaceshader") return "open_pbr_surface";
    if (stem == "standard_surface_surfaceshader") return "standard_surface";
    if (stem == "UsdPreviewSurface_surfaceshader") return "UsdPreviewSurface";
    return stem;
  }

  return category;
}

void EmitXmlInput(std::stringstream& ss, const nlohmann::json& input) {
  const std::string name = JsonString(input, "name");
  if (name.empty()) return;
  std::string type = NormalizeMtlxType(JsonString(input, "type"));
  const auto valueIt = input.find("value");
  if (type == "float" && valueIt != input.end()) {
    type = NormalizeMtlxType(InferTypeFromJsonValue(*valueIt));
  }

  ss << "      <input name=\"" << XmlEscape(name)
     << "\" type=\"" << XmlEscape(type) << "\"";
  if (valueIt != input.end()) {
    ss << " value=\"" << XmlEscape(JsonValueToMtlxString(*valueIt)) << "\"";
  }
  const std::string nodeName = JsonString(input, "nodename");
  const std::string nodeGraph = JsonString(input, "nodegraph");
  const std::string interfaceName = JsonString(input, "interfacename");
  const std::string output = JsonString(input, "output");
  const std::string colorspace = JsonString(input, "colorspace");
  const std::string channels = JsonString(input, "channels");
  if (!nodeName.empty()) ss << " nodename=\"" << XmlEscape(nodeName) << "\"";
  if (!nodeGraph.empty()) ss << " nodegraph=\"" << XmlEscape(nodeGraph) << "\"";
  if (!interfaceName.empty()) {
    ss << " interfacename=\"" << XmlEscape(interfaceName) << "\"";
  }
  if (!output.empty()) ss << " output=\"" << XmlEscape(output) << "\"";
  if (!colorspace.empty()) ss << " colorspace=\"" << XmlEscape(colorspace) << "\"";
  if (!channels.empty()) ss << " channels=\"" << XmlEscape(channels) << "\"";
  ss << "/>\n";
}

struct MtlxConnection {
  std::string graph;
  std::string output;
  std::string channels;
};

bool BuildMaterialXXmlFromJsonGraph(const DrawMaterialCPU& mat,
                                    std::string* xml,
                                    std::string* err,
                                    bool volume_graph = false) {
  if (!xml) {
    if (err) *err = "Output XML pointer is null";
    return false;
  }
  if (mat.materialXNodeGraphJson.empty()) {
    if (err) *err = "MaterialX node graph JSON is empty";
    return false;
  }

  nlohmann::json j = nlohmann::json::parse(
      mat.materialXNodeGraphJson.begin(), mat.materialXNodeGraphJson.end(),
      nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (err) *err = "Failed to parse MaterialX node graph JSON";
    return false;
  }
  const auto ngIt = j.find("nodegraph");
  if (ngIt == j.end() || !ngIt->is_object()) {
    if (err) *err = "MaterialX JSON has no nodegraph object";
    return false;
  }
  const nlohmann::json& ng = *ngIt;
  const std::string graphName = JsonString(ng, "name", "NG_lusdview");

  std::map<std::string, std::string> outputTypes;
  const auto outputsIt = ng.find("outputs");
  if (outputsIt != ng.end() && outputsIt->is_array()) {
    for (const nlohmann::json& output : *outputsIt) {
      const std::string name = JsonString(output, "name");
      if (name.empty()) continue;
      outputTypes[name] = NormalizeMtlxType(JsonString(output, "type"));
    }
  }

  std::map<std::string, MtlxConnection> connections;
  const auto connIt = j.find("connections");
  if (connIt != j.end() && connIt->is_array()) {
    for (const nlohmann::json& c : *connIt) {
      const std::string input = OpenPBREvalInputName(JsonString(c, "input"));
      const std::string graph = JsonString(c, "nodegraph", graphName);
      const std::string output = JsonString(c, "output");
      if (!input.empty() && !graph.empty() && !output.empty()) {
        connections[input] = {graph, output, JsonString(c, "channels")};
      }
    }
  }
  if (connections.empty()) {
    if (err) *err = "MaterialX JSON has no shader input connections";
    return false;
  }

  std::stringstream ss;
  ss << "<materialx version=\"1.39\">\n";
  ss << "  <nodegraph name=\"" << XmlEscape(graphName) << "\">\n";
  const auto graphInputsIt = ng.find("inputs");
  if (graphInputsIt != ng.end() && graphInputsIt->is_array()) {
    for (const nlohmann::json& input : *graphInputsIt) {
      EmitXmlInput(ss, input);
    }
  }
  const auto nodesIt = ng.find("nodes");
  if (nodesIt != ng.end() && nodesIt->is_array()) {
    for (const nlohmann::json& node : *nodesIt) {
      const std::string type = NormalizeMtlxType(JsonString(node, "type"));
      const std::string category =
          NormalizeMtlxCategory(JsonString(node, "category"), type);
      const std::string name = JsonString(node, "name");
      if (category.empty() || name.empty()) continue;
      ss << "    <" << XmlEscape(category)
         << " name=\"" << XmlEscape(name)
         << "\" type=\"" << XmlEscape(type) << "\">\n";
      const auto inputsIt = node.find("inputs");
      if (inputsIt != node.end() && inputsIt->is_array()) {
        for (const nlohmann::json& input : *inputsIt) {
          EmitXmlInput(ss, input);
        }
      }
      ss << "    </" << XmlEscape(category) << ">\n";
    }
  }
  if (outputsIt != ng.end() && outputsIt->is_array()) {
    for (const nlohmann::json& output : *outputsIt) {
      const std::string name = JsonString(output, "name");
      if (name.empty()) continue;
      const std::string type = NormalizeMtlxType(JsonString(output, "type"));
      ss << "    <output name=\"" << XmlEscape(name)
         << "\" type=\"" << XmlEscape(type) << "\"";
      const std::string nodeName = JsonString(output, "nodename");
      const std::string outName = JsonString(output, "output");
      if (!nodeName.empty()) ss << " nodename=\"" << XmlEscape(nodeName) << "\"";
      if (!outName.empty()) ss << " output=\"" << XmlEscape(outName) << "\"";
      ss << "/>\n";
    }
  }
  ss << "  </nodegraph>\n";

  if (!volume_graph) {
    ss << "  <open_pbr_surface name=\"lusdview_openpbr\" type=\"surfaceshader\">\n";
  }
  std::set<std::string> emitted;
  auto emit_graph_input = [&](const char* input,
                              const char* type, const MtlxConnection& conn) {
    ss << "    <input name=\"" << input << "\" type=\"" << type
       << "\" nodegraph=\"" << XmlEscape(conn.graph)
       << "\" output=\"" << XmlEscape(conn.output) << "\"/>\n";
  };
  if (volume_graph) {
    // The runtime representation stores density and albedo, while MaterialX
    // volume evaluation consumes absorption and scattering. Build that small
    // conversion explicitly so composed graph outputs retain their physical
    // meaning instead of being mistaken for raw extinction coefficients.
    const auto density = connections.find("volume_density");
    const auto albedo = connections.find("volume_albedo");
    if (density != connections.end() && albedo != connections.end()) {
      ss << "  <subtract name=\"lusdview_volume_one_minus\" type=\"color3\">\n"
         << "    <input name=\"in1\" type=\"color3\" value=\"1,1,1\"/>\n";
      emit_graph_input("in2", "color3", albedo->second);
      ss << "  </subtract>\n"
         << "  <multiply name=\"lusdview_volume_absorption\" type=\"color3\">\n";
      emit_graph_input("in1", "color3", density->second);
      ss << "    <input name=\"in2\" type=\"color3\" nodename=\"lusdview_volume_one_minus\"/>\n"
         << "  </multiply>\n"
         << "  <multiply name=\"lusdview_volume_scattering\" type=\"color3\">\n";
      emit_graph_input("in1", "color3", density->second);
      emit_graph_input("in2", "color3", albedo->second);
      ss << "  </multiply>\n";
    }
    ss << "  <anisotropic_vdf name=\"lusdview_vdf\" type=\"VDF\">\n";
    if (density != connections.end() && albedo != connections.end()) {
      ss << "    <input name=\"absorption\" type=\"color3\" nodename=\"lusdview_volume_absorption\"/>\n"
         << "    <input name=\"scattering\" type=\"color3\" nodename=\"lusdview_volume_scattering\"/>\n";
    } else if (density != connections.end()) {
      emit_graph_input("absorption", "color3", density->second);
    }
    const auto anisotropy = connections.find("volume_anisotropy");
    if (anisotropy != connections.end())
      emit_graph_input("anisotropy", "float", anisotropy->second);
    ss << "  </anisotropic_vdf>\n";
    const auto emission = connections.find("volume_emission_color");
    const auto emission_scale = connections.find("volume_emission_scale");
    if (emission != connections.end()) {
      if (emission_scale != connections.end()) {
        ss << "  <multiply name=\"lusdview_volume_emission\" type=\"color3\">\n";
        emit_graph_input("in1", "color3", emission->second);
        emit_graph_input("in2", "color3", emission_scale->second);
        ss << "  </multiply>\n";
      }
      ss << "  <uniform_edf name=\"lusdview_edf\" type=\"EDF\">\n";
      if (emission_scale != connections.end()) {
        ss << "    <input name=\"color\" type=\"color3\" "
              "nodename=\"lusdview_volume_emission\"/>\n";
      } else {
        emit_graph_input("color", "color3", emission->second);
      }
      ss << "  </uniform_edf>\n";
    }
    ss << "  <volume name=\"lusdview_volume\" type=\"volumeshader\">\n"
       << "    <input name=\"vdf\" type=\"VDF\" nodename=\"lusdview_vdf\"/>\n";
    if (emission != connections.end())
      ss << "    <input name=\"edf\" type=\"EDF\" nodename=\"lusdview_edf\"/>\n";
    ss << "  </volume>\n";
  } else for (const auto& conn : connections) {
    // The realtime bridge exposes subsurface_radius_scale as a separate
    // vector input, while the OpenPBR MaterialX node represents the same
    // authored vector through its radius lane. Keep the evaluator in sync
    // with the runtime graph ABI; otherwise it silently evaluates the alias
    // as an unrecognized input and returns the default radius.
    const std::string evalInput =
        conn.first == "subsurface_radius_scale" ? "subsurface_radius"
                                                 : conn.first;
    const std::string type = outputTypes.count(conn.second.output)
                                 ? outputTypes[conn.second.output]
                                 : "float";
    ss << "    <input name=\"" << XmlEscape(evalInput)
       << "\" type=\"" << XmlEscape(type)
       << "\" nodegraph=\"" << XmlEscape(conn.second.graph)
       << "\" output=\"" << XmlEscape(conn.second.output) << "\"";
    if (!conn.second.channels.empty()) {
      ss << " channels=\"" << XmlEscape(conn.second.channels) << "\"";
    }
    ss << "/>\n";
    emitted.insert(conn.first);
  }
  if (!volume_graph) for (const DrawMaterialParamCPU& p : mat.params) {
    if (p.shader != "OpenPBRSurface") continue;
    const std::string name = OpenPBREvalInputName(p.name);
    if (emitted.count(name)) continue;
    // Runtime texture inputs are sampled by the preview shaders; this constant
    // graph bake is only a fallback for texture-free values.
    if (p.texture >= 0 || p.renderTexture >= 0) continue;
    ss << "    <input name=\"" << XmlEscape(name)
       << "\" type=\"" << XmlEscape(TypeForParam(p.type))
       << "\" value=\"" << XmlEscape(ValueForParam(p)) << "\"/>\n";
    emitted.insert(name);
  }
  if (volume_graph) {
    ss << "  <volumematerial name=\"lusdview_material\" type=\"material\">\n";
    ss << "    <input name=\"volumeshader\" type=\"volumeshader\" "
          "nodename=\"lusdview_volume\"/>\n";
    ss << "  </volumematerial>\n";
  } else {
    ss << "  </open_pbr_surface>\n";
    ss << "  <surfacematerial name=\"lusdview_material\" type=\"material\">\n";
    ss << "    <input name=\"surfaceshader\" type=\"surfaceshader\" "
          "nodename=\"lusdview_openpbr\"/>\n";
    ss << "  </surfacematerial>\n";
  }
  ss << "</materialx>\n";

  *xml = ss.str();
  return true;
}

bool EvaluateMaterialXJsonGraphForMaterial(const DrawMaterialCPU& mat,
                                           DrawLightRtOpenPBRCPU* out,
                                           std::string* err) {
  std::string xml;
  if (!BuildMaterialXXmlFromJsonGraph(mat, &xml, err)) return false;
  // MaterialX image filenames are relative to the owning asset. Keep the
  // evaluator's cache rooted at that asset instead of silently evaluating
  // every image through its default input.
  std::string baseDir = ".";
  const size_t slash = mat.absPath.find_last_of("/\\");
  if (slash != std::string::npos) {
    baseDir = mat.absPath.substr(0, slash);
    if (baseDir.empty()) baseDir = ".";
  }

  return EvaluateMaterialXStringToLightRtOpenPBRWithBaseDir(
      xml.c_str(), "lusdview_material", baseDir.c_str(), out, err);
}

bool EvaluateMaterialXJsonGraphForVolume(const DrawMaterialCPU& mat,
                                         DrawMaterialCPU* out,
                                         std::string* err) {
  if (!out) {
    if (err) *err = "Output pointer is null";
    return false;
  }
  DrawMaterialCPU graph = mat;
  graph.materialXNodeGraphJson = mat.volumeMaterialXNodeGraphJson;
  std::string xml;
  if (!BuildMaterialXXmlFromJsonGraph(graph, &xml, err, true)) return false;
  std::string baseDir = ".";
  const size_t slash = mat.absPath.find_last_of("/\\");
  if (slash != std::string::npos) {
    baseDir = mat.absPath.substr(0, slash);
    if (baseDir.empty()) baseDir = ".";
  }
  return EvaluateMaterialXStringToLightRtVolumeWithBaseDir(
      xml.c_str(), "lusdview_material", baseDir.c_str(), out, err);
}

int FindSurfaceNode(const MtlxDoc* doc, const char* materialName,
                    std::string* err) {
  if (!doc) return -1;
  if (materialName && materialName[0]) {
    const int matId = mtlx_find_material(doc, materialName);
    if (matId < 0) {
      if (err) *err = std::string("MaterialX material not found: ") + materialName;
      return -1;
    }
    if (doc->mats[matId].surface_node < 0) {
      if (err) *err = std::string("MaterialX material has no surface shader: ") +
                      materialName;
      return -1;
    }
    return doc->mats[matId].surface_node;
  }
  if (doc->nmat > 0 && doc->mats[0].surface_node >= 0) {
    return doc->mats[0].surface_node;
  }
  for (int i = 0; i < doc->nnode; ++i) {
    const char* c = doc->nodes[i].category;
    if (!c) continue;
    if (std::strcmp(c, "open_pbr_surface") == 0 ||
        std::strcmp(c, "standard_surface") == 0 ||
        std::strcmp(c, "surface_unlit") == 0 ||
        std::strcmp(c, "UsdPreviewSurface") == 0 ||
        std::strcmp(c, "gltf_pbr") == 0 ||
        std::strcmp(c, "disney_principled") == 0) {
      return i;
    }
  }
  if (err) *err = "MaterialX document has no supported surface shader";
  return -1;
}

bool EvaluateMtlxDocAtUv(const MtlxDoc* doc, int surfaceNode,
                         TextureCache* tex, float u, float v,
                         float dudx, float dvdy,
                         std::vector<MtlxValue>* memo,
                         std::vector<char>* memoDone,
                         DrawLightRtOpenPBRCPU* out) {
  if (!doc || surfaceNode < 0 || !tex || !memo || !memoDone || !out ||
      memo->size() < static_cast<size_t>(doc->nnode) ||
      memoDone->size() < static_cast<size_t>(doc->nnode)) {
    return false;
  }
  ShadeContext ctx{};
  ctx.doc = doc;
  ctx.tex = tex;
  ctx.uv[0] = u;
  ctx.uv[1] = v;
  ctx.uv_dx[0] = dudx;
  ctx.uv_dx[1] = 0.0f;
  ctx.uv_dy[0] = 0.0f;
  ctx.uv_dy[1] = dvdy;
  ctx.has_uv_derivatives = dudx > 0.0f || dvdy > 0.0f;
  ctx.P = v3_make(0.0f, 0.0f, 0.0f);
  ctx.Ns = v3_make(0.0f, 0.0f, 1.0f);
  ctx.Ng = v3_make(0.0f, 0.0f, 1.0f);
  ctx.dpdu = v3_make(1.0f, 0.0f, 0.0f);
  ctx.dpdv = v3_make(0.0f, 1.0f, 0.0f);
  ctx.V = v3_make(0.0f, 0.0f, 1.0f);
  ctx.memo = memo->data();
  ctx.memo_done = memoDone->data();
  OpenPBRParams params;
  if (mtlx_eval_surface(&ctx, surfaceNode, &params) != 0) return false;
  CopyLightRtEval(params, out);
  out->hasTextureInputs = false;
  out->hasNormalInput = (std::fabs(out->normal[0]) > 0.0f ||
                         std::fabs(out->normal[1]) > 0.0f ||
                         std::fabs(out->normal[2] - 1.0f) > 1.0e-6f);
  ClampLightRtParams(out);
  return true;
}

}  // namespace

class MaterialXSurfaceRuntime {
 public:
  MaterialXSurfaceRuntime(MtlxDoc* document, TextureCache* textureCache,
                          int node, uint64_t runtimeId)
      : doc(document), tex(textureCache), surfaceNode(node), id(runtimeId) {}
  ~MaterialXSurfaceRuntime() {
    texcache_free(tex);
    mtlx_free(doc);
  }

  MtlxDoc* doc{nullptr};
  TextureCache* tex{nullptr};
  int surfaceNode{-1};
  uint64_t id{0};
};

namespace {
struct MtlxThreadScratch {
  std::vector<MtlxValue> memo;
  std::vector<char> memoDone;
};

std::atomic<uint64_t> gMtlxRuntimeId{1};
thread_local std::unordered_map<uint64_t, MtlxThreadScratch> gMtlxScratch;
}  // namespace

std::shared_ptr<MaterialXSurfaceRuntime> CreateMaterialXSurfaceRuntime(
    const char* xml, const char* materialName, const char* baseDir,
    std::string* err, bool buildMipmaps) {
  if (err) err->clear();
  if (!xml || !xml[0]) {
    if (err) *err = "MaterialX XML string is empty";
    return {};
  }
  MtlxDoc* doc = mtlx_load_string(xml);
  if (!doc) {
    if (err) *err = "mtlx_load_string failed";
    return {};
  }
  const int surfaceNode = FindSurfaceNode(doc, materialName, err);
  if (surfaceNode < 0) {
    mtlx_free(doc);
    return {};
  }
  TextureCache* tex = texcache_create(baseDir);
  if (!tex) {
    if (err) *err = "texcache_create failed";
    mtlx_free(doc);
    return {};
  }
  if (buildMipmaps) {
    if (!texcache_preload_mips(tex, doc)) {
      if (err) *err = "failed to build MaterialX texture mipmaps";
      texcache_free(tex);
      mtlx_free(doc);
      return {};
    }
  } else {
    texcache_preload(tex, doc);
  }
  return std::make_shared<MaterialXSurfaceRuntime>(
      doc, tex, surfaceNode, gMtlxRuntimeId.fetch_add(1, std::memory_order_relaxed));
}

bool EvaluateMaterialXSurfaceRuntimeAt(
    MaterialXSurfaceRuntime* runtime, const MaterialXSurfaceContext& context,
    tydra::LightRtOpenPBRParams* out, std::string* err) {
  if (err) err->clear();
  if (!runtime || !runtime->doc || !runtime->tex || !out) {
    if (err) *err = "MaterialX surface runtime is not initialized";
    return false;
  }
  MtlxThreadScratch& scratch = gMtlxScratch[runtime->id];
  const size_t nodeCount = static_cast<size_t>(runtime->doc->nnode);
  if (scratch.memo.size() != nodeCount) {
    scratch.memo.resize(nodeCount);
    scratch.memoDone.resize(nodeCount);
  }
  ShadeContext ctx{};
  ctx.doc = runtime->doc;
  ctx.tex = runtime->tex;
  ctx.uv[0] = context.uv[0];
  ctx.uv[1] = context.uv[1];
  ctx.uv_dx[0] = context.uv_dx[0];
  ctx.uv_dx[1] = context.uv_dx[1];
  ctx.uv_dy[0] = context.uv_dy[0];
  ctx.uv_dy[1] = context.uv_dy[1];
  ctx.has_uv_derivatives = context.has_uv_derivatives ? 1 : 0;
  ctx.P = v3_make(context.position[0], context.position[1], context.position[2]);
  ctx.Ns = v3_make(context.shading_normal[0], context.shading_normal[1],
                   context.shading_normal[2]);
  ctx.Ng = v3_make(context.geometric_normal[0], context.geometric_normal[1],
                   context.geometric_normal[2]);
  ctx.V = v3_make(context.view_direction[0], context.view_direction[1],
                  context.view_direction[2]);
  ctx.dpdu = v3_make(context.dpdu[0], context.dpdu[1], context.dpdu[2]);
  ctx.dpdv = v3_make(context.dpdv[0], context.dpdv[1], context.dpdv[2]);
  if (context.has_uv_derivatives) {
    ctx.dPdx = v3_add(v3_scale(ctx.dpdu, context.uv_dx[0]),
                      v3_scale(ctx.dpdv, context.uv_dx[1]));
    ctx.dPdy = v3_add(v3_scale(ctx.dpdu, context.uv_dy[0]),
                      v3_scale(ctx.dpdv, context.uv_dy[1]));
    ctx.has_position_derivatives = 1;
  }
  ctx.memo = scratch.memo.data();
  ctx.memo_done = scratch.memoDone.data();
  OpenPBRParams evaluated;
  if (mtlx_eval_surface(&ctx, runtime->surfaceNode, &evaluated) != 0) {
    if (err) *err = "mtlx_eval_surface failed";
    return false;
  }
  CopyLightRtEval(evaluated, out);
  out->hasTextureInputs = false;
  out->hasNormalInput =
      std::fabs(out->normal[0] - context.shading_normal[0]) > 1.0e-6f ||
      std::fabs(out->normal[1] - context.shading_normal[1]) > 1.0e-6f ||
      std::fabs(out->normal[2] - context.shading_normal[2]) > 1.0e-6f;
  ClampLightRtParams(out);
  return true;
}

bool EvaluateMaterialXNodeRuntimeAt(
    MaterialXSurfaceRuntime* runtime, const MaterialXSurfaceContext& context,
    const char* nodeName, MaterialXNodeValue* out, std::string* err) {
  if (err) err->clear();
  if (!runtime || !runtime->doc || !runtime->tex || !nodeName || !nodeName[0] ||
      !out) {
    if (err) *err = "MaterialX node AOV request is not initialized";
    return false;
  }
  int nodeId = -1;
  for (int i = 0; i < runtime->doc->nnode; ++i) {
    const char* name = runtime->doc->nodes[i].name;
    if (name && std::strcmp(name, nodeName) == 0) {
      if (nodeId >= 0) {
        if (err) *err = "MaterialX node name is ambiguous: " + std::string(nodeName);
        return false;
      }
      nodeId = i;
    }
  }
  if (nodeId < 0) {
    if (err) *err = "MaterialX node not found: " + std::string(nodeName);
    return false;
  }

  MtlxThreadScratch& scratch = gMtlxScratch[runtime->id];
  const size_t nodeCount = static_cast<size_t>(runtime->doc->nnode);
  if (scratch.memo.size() != nodeCount) {
    scratch.memo.resize(nodeCount);
    scratch.memoDone.resize(nodeCount);
  }
  ShadeContext ctx{};
  ctx.doc = runtime->doc;
  ctx.tex = runtime->tex;
  ctx.uv[0] = context.uv[0];
  ctx.uv[1] = context.uv[1];
  ctx.uv_dx[0] = context.uv_dx[0];
  ctx.uv_dx[1] = context.uv_dx[1];
  ctx.uv_dy[0] = context.uv_dy[0];
  ctx.uv_dy[1] = context.uv_dy[1];
  ctx.has_uv_derivatives = context.has_uv_derivatives ? 1 : 0;
  ctx.P = v3_make(context.position[0], context.position[1], context.position[2]);
  ctx.Ns = v3_make(context.shading_normal[0], context.shading_normal[1],
                   context.shading_normal[2]);
  ctx.Ng = v3_make(context.geometric_normal[0], context.geometric_normal[1],
                   context.geometric_normal[2]);
  ctx.V = v3_make(context.view_direction[0], context.view_direction[1],
                  context.view_direction[2]);
  ctx.dpdu = v3_make(context.dpdu[0], context.dpdu[1], context.dpdu[2]);
  ctx.dpdv = v3_make(context.dpdv[0], context.dpdv[1], context.dpdv[2]);
  if (context.has_uv_derivatives) {
    ctx.dPdx = v3_add(v3_scale(ctx.dpdu, context.uv_dx[0]),
                      v3_scale(ctx.dpdv, context.uv_dx[1]));
    ctx.dPdy = v3_add(v3_scale(ctx.dpdu, context.uv_dy[0]),
                      v3_scale(ctx.dpdv, context.uv_dy[1]));
    ctx.has_position_derivatives = 1;
  }
  ctx.memo = scratch.memo.data();
  ctx.memo_done = scratch.memoDone.data();
  const MtlxValue value = mtlx_eval_node_test(&ctx, nodeId);
  switch (value.type) {
    case MV_FLOAT:
    case MV_INT:
    case MV_BOOL:
      out->rgb[0] = value.v[0];
      out->rgb[1] = value.v[0];
      out->rgb[2] = value.v[0];
      return true;
    case MV_COLOR3:
    case MV_VEC3:
      out->rgb[0] = value.v[0];
      out->rgb[1] = value.v[1];
      out->rgb[2] = value.v[2];
      return true;
    case MV_COLOR4:
    case MV_VEC4:
    case MV_MATRIX33:
    case MV_MATRIX44:
      out->rgb[0] = value.v[0];
      out->rgb[1] = value.v[1];
      out->rgb[2] = value.v[2];
      return true;
    case MV_VEC2:
      out->rgb[0] = value.v[0];
      out->rgb[1] = value.v[1];
      out->rgb[2] = 0.0f;
      return true;
    default:
      if (err) *err = "MaterialX node AOV requires a numeric node output";
      return false;
  }
}

bool EvaluateMaterialXStringToLightRtOpenPBRAtUv(
    const char* xml, const char* materialName, const char* baseDir, float u,
    float v, DrawLightRtOpenPBRCPU* out, std::string* err) {
  if (err) err->clear();
  if (!xml || !xml[0]) {
    if (err) *err = "MaterialX XML string is empty";
    return false;
  }
  if (!out) {
    if (err) *err = "Output pointer is null";
    return false;
  }

  MtlxDoc* doc = mtlx_load_string(xml);
  if (!doc) {
    if (err) *err = "mtlx_load_string failed";
    return false;
  }

  const int surfaceNode = FindSurfaceNode(doc, materialName, err);
  if (surfaceNode < 0) {
    mtlx_free(doc);
    return false;
  }

  TextureCache* tex = texcache_create(baseDir);
  std::vector<MtlxValue> memo(static_cast<size_t>(doc->nnode));
  std::vector<char> memoDone(static_cast<size_t>(doc->nnode), 0);
  DrawLightRtOpenPBRCPU baked;
  const bool ok = EvaluateMtlxDocAtUv(doc, surfaceNode, tex, u, v,
                                      0.0f, 0.0f, &memo, &memoDone, &baked);
  texcache_free(tex);
  mtlx_free(doc);
  if (!ok) {
    if (err) *err = "mtlx_eval_surface failed";
    return false;
  }
  *out = baked;
  return true;
}

bool EvaluateMaterialXStringToLightRtOpenPBRWithBaseDir(
    const char* xml, const char* materialName, const char* baseDir,
    DrawLightRtOpenPBRCPU* out, std::string* err) {
  return EvaluateMaterialXStringToLightRtOpenPBRAtUv(
      xml, materialName, baseDir, 0.5f, 0.5f, out, err);
}

bool EvaluateMaterialXStringToLightRtOpenPBR(const char* xml,
                                             const char* materialName,
                                             DrawLightRtOpenPBRCPU* out,
                                             std::string* err) {
  return EvaluateMaterialXStringToLightRtOpenPBRWithBaseDir(
      xml, materialName, nullptr, out, err);
}

bool EvaluateMaterialXStringToLightRtVolume(const char* xml,
                                            const char* materialName,
                                            DrawMaterialCPU* out,
                                            std::string* err) {
  return EvaluateMaterialXStringToLightRtVolumeWithBaseDir(
      xml, materialName, nullptr, out, err);
}

bool EvaluateMaterialXStringToLightRtVolumeWithBaseDir(
    const char* xml, const char* materialName, const char* baseDir,
    DrawMaterialCPU* out, std::string* err) {
  if (err) err->clear();
  if (!xml || !xml[0] || !out) {
    if (err) *err = !out ? "Output pointer is null" :
                           "MaterialX XML string is empty";
    return false;
  }
  MtlxDoc* doc = mtlx_load_string(xml);
  if (!doc) {
    if (err) *err = "mtlx_load_string failed";
    return false;
  }
  int material = materialName && materialName[0]
      ? mtlx_find_material(doc, materialName) : (doc->nmat == 1 ? 0 : -1);
  if (material < 0 || doc->mats[material].volume_node < 0) {
    if (err) *err = "MaterialX volume material or shader not found";
    mtlx_free(doc);
    return false;
  }
  TextureCache* tex = texcache_create(baseDir);
  ShadeContext ctx{};
  ctx.doc = doc;
  ctx.tex = tex;
  ctx.uv[0] = 0.5f;
  ctx.uv[1] = 0.5f;
  ctx.P = v3_make(0.0f, 0.0f, 0.0f);
  ctx.Ns = v3_make(0.0f, 0.0f, 1.0f);
  ctx.Ng = ctx.Ns;
  ctx.V = ctx.Ns;
  std::vector<MtlxValue> memo(static_cast<size_t>(doc->nnode));
  std::vector<char> done(static_cast<size_t>(doc->nnode), 0);
  ctx.memo = memo.data();
  ctx.memo_done = done.data();
  MtlxVolumeParams volume{};
  const bool ok = mtlx_eval_volume(
      &ctx, doc->mats[material].volume_node, &volume) == 0;
  texcache_free(tex);
  mtlx_free(doc);
  if (!ok) {
    if (err) *err = "mtlx_eval_volume failed";
    return false;
  }
  const float extinction[3] = {
      volume.absorption.x + volume.scattering.x,
      volume.absorption.y + volume.scattering.y,
      volume.absorption.z + volume.scattering.z};
  out->volumeDensity = std::max(extinction[0],
      std::max(extinction[1], extinction[2]));
  const float scattering[3] = {volume.scattering.x, volume.scattering.y,
                               volume.scattering.z};
  for (int i = 0; i < 3; ++i) {
    out->volumeAlbedo[i] = extinction[i] > 1.0e-8f
        ? scattering[i] / extinction[i] : 0.0f;
  }
  out->volumeEmission[0] = volume.emission.x;
  out->volumeEmission[1] = volume.emission.y;
  out->volumeEmission[2] = volume.emission.z;
  out->volumeAnisotropy = volume.anisotropy;
  out->volumeEmissionScale = v3_maxc(volume.emission) > 0.0f ? 1.0f : 0.0f;
  out->hasVolumeOutput = true;
  return true;
}


void BakeMaterialXGraphTextures(DrawMaterialCPU* mat, DrawScene* scene) {
  if (!mat || !scene || mat->materialXNodeGraphJson.empty()) return;
  if (!mat->materialXGraph.valid) {
    std::string compileError;
    CompileMaterialXGraphRuntime(mat, &compileError);
  }
  // A converted surface may retain a graph solely because another terminal
  // input (for example displacement) is connected.  The compiler still emits
  // constant lanes for the surface's typed/default inputs, but those lanes
  // must not replace the authoritative DrawMaterialCPU values at runtime.
  // Keep non-constant routes executable for genuine graph-driven inputs.
  if (mat->materialXGraph.valid) {
    std::function<bool(int)> constantOnly = [&](int index) {
      if (index < 0 || static_cast<size_t>(index) >=
                           mat->materialXGraph.nodes.size()) return false;
      const MaterialXGraphNodeCPU& node =
          mat->materialXGraph.nodes[static_cast<size_t>(index)];
      if (node.op == MaterialXGraphOpCPU::Constant) return true;
      if (node.op == MaterialXGraphOpCPU::Convert)
        return constantOnly(node.input[0]);
      return false;
    };
    for (size_t lane = 0; lane < 6 &&
                          lane < mat->materialXGraph.output.size(); ++lane) {
      const int output = mat->materialXGraph.output[lane];
      if (constantOnly(output)) {
        mat->materialXGraph.output[lane] = -1;
      }
    }
  }
  // Bind graph image nodes to the already-resolved DrawScene texture slots.
  // MaterialX JSON stores the authored filename while the scene loader may
  // store an absolute or normalized asset identifier, so accept exact,
  // normalized, and basename matches. This keeps the runtime IR independent
  // of the loader that populated the texture array.
  auto normalizePath = [](std::string value) {
    for (char& c : value) if (c == '\\') c = '/';
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
  };
  std::string baseDir = ".";
  const size_t materialSlash = mat->absPath.find_last_of("/\\");
  if (materialSlash != std::string::npos) {
    baseDir = mat->absPath.substr(0, materialSlash);
    if (baseDir.empty()) baseDir = ".";
  }
  auto isAbsolutePath = [](const std::string& value) {
    return (!value.empty() && (value[0] == '/' || value[0] == '\\')) ||
           (value.size() > 1 && value[1] == ':');
  };
  // Infer the color-data subgraph for images that must be loaded here.  The
  // normal scene texture table already carries authored sRGB metadata, but a
  // graph image that was not discovered by the loader needs the same choice:
  // base/emission/subsurface-color are color data; roughness, normal, and
  // scalar masks are linear.  Shared nodes conservatively use sRGB whenever
  // they feed any color output.
  std::set<int> srgbNodes;
  std::function<void(int)> markSrgb = [&](int index) {
    if (index < 0 || static_cast<size_t>(index) >= mat->materialXGraph.nodes.size() ||
        !srgbNodes.insert(index).second) return;
    const MaterialXGraphNodeCPU& source = mat->materialXGraph.nodes[static_cast<size_t>(index)];
    for (int input : source.input) markSrgb(input);
  };
  for (int route : {0, 4, 7, 10, 12, 14, 17, 22, 41, 42})
    markSrgb(mat->materialXGraph.output[route]);

  for (size_t nodeIndex = 0; nodeIndex < mat->materialXGraph.nodes.size();
       ++nodeIndex) {
    MaterialXGraphNodeCPU& node = mat->materialXGraph.nodes[nodeIndex];
    if (node.textureId >= 0 || node.imagePath.empty()) continue;
    const std::string wanted = normalizePath(node.imagePath);
    const size_t slash = wanted.find_last_of('/');
    const std::string basename = slash == std::string::npos
                                     ? wanted
                                     : wanted.substr(slash + 1);
    for (size_t textureId = 0; textureId < scene->textures.size(); ++textureId) {
      const std::string asset = normalizePath(scene->textures[textureId].assetIdentifier);
      const size_t assetSlash = asset.find_last_of('/');
      const std::string assetBase = assetSlash == std::string::npos
                                        ? asset
                                        : asset.substr(assetSlash + 1);
      if (asset == wanted || assetBase == basename ||
          (!mat->absPath.empty() &&
           normalizePath(mat->absPath.substr(0, mat->absPath.find_last_of("/\\")) +
                         "/" + wanted) == asset)) {
        node.textureId = static_cast<int>(textureId);
        break;
      }
    }
    if (node.textureId < 0) {
      const std::string assetPath = normalizePath(
          isAbsolutePath(wanted) ? wanted : baseDir + "/" + wanted);
      auto loaded = lightusd::image::LoadImageFromFile(assetPath);
      if (loaded) {
        const lightusd::Image& image = loaded.value().image;
        if (image.width > 0 && image.height > 0 && image.bpp == 8 &&
            (image.channels == 1 || image.channels == 2 ||
             image.channels == 3 || image.channels == 4) &&
            image.data.size() >= static_cast<size_t>(image.width) *
                                      static_cast<size_t>(image.height) *
                                      static_cast<size_t>(image.channels)) {
          DrawTextureCPU texture;
          texture.assetIdentifier = assetPath;
          texture.srgb = srgbNodes.count(static_cast<int>(nodeIndex)) != 0;
          texture.image.width = image.width;
          texture.image.height = image.height;
          texture.image.channels = 4;
          texture.image.data.resize(static_cast<size_t>(image.width) *
                                    static_cast<size_t>(image.height) * 4u);
          for (size_t p = 0, n = static_cast<size_t>(image.width) *
                                       static_cast<size_t>(image.height);
               p < n; ++p) {
            const uint8_t* src = image.data.data() + p * image.channels;
            uint8_t* dst = texture.image.data.data() + p * 4u;
            dst[0] = src[0];
            dst[1] = image.channels > 1 ? src[1] : src[0];
            dst[2] = image.channels > 2 ? src[2] : src[0];
            dst[3] = image.channels > 3 ? src[3] : 255u;
          }
          node.textureId = static_cast<int>(scene->textures.size());
          scene->textures.push_back(std::move(texture));
        }
      }
    }
    if (node.textureId >= 0 &&
        static_cast<size_t>(node.textureId) < scene->textures.size()) {
      node.isUdim = scene->textures[static_cast<size_t>(node.textureId)].isUdim;
    }
  }
  const GraphTextureDeps deps =
      AnalyzeMaterialXJsonTextureDeps(mat->materialXNodeGraphJson);
  if (!deps.parsed || !deps.hasTextureNodes) return;

  const auto& outputs = mat->materialXGraph.output;
  std::function<bool(int, std::set<int>*)> dependsOnImage =
      [&](int index, std::set<int>* visiting) {
        if (index < 0 || static_cast<size_t>(index) >=
                              mat->materialXGraph.nodes.size()) {
          return false;
        }
        const MaterialXGraphNodeCPU& node =
            mat->materialXGraph.nodes[static_cast<size_t>(index)];
        if (node.op == MaterialXGraphOpCPU::Image ||
            node.op == MaterialXGraphOpCPU::TiledImage ||
            node.textureId >= 0 || !node.imagePath.empty()) {
          return true;
        }
        if (!visiting->insert(index).second) return false;
        for (int input : node.input) {
          if (dependsOnImage(input, visiting)) return true;
        }
        visiting->erase(index);
        return false;
      };
  std::string xml;
  std::string error;
  if (!BuildMaterialXXmlFromJsonGraph(*mat, &xml, &error)) return;
  MtlxDoc* graphDoc = mtlx_load_string(xml.c_str());
  if (!graphDoc) return;
  std::string surfaceError;
  const int surfaceNode = FindSurfaceNode(
      graphDoc, "lusdview_material", &surfaceError);
  if (surfaceNode < 0) {
    mtlx_free(graphDoc);
    return;
  }
  TextureCache* graphTex = texcache_create(baseDir.c_str());
  if (!graphTex) {
    mtlx_free(graphDoc);
    return;
  }
  texcache_preload(graphTex, graphDoc);
  std::vector<MtlxValue> graphMemo(static_cast<size_t>(graphDoc->nnode));
  std::vector<char> graphMemoDone(static_cast<size_t>(graphDoc->nnode), 0);

  // Bounded compatibility bake: arbitrary image/procedural networks become
  // ordinary semantic maps until descriptor-indexed graph evaluation lands.
  constexpr int kSize = 16;
  struct Lane { const char* name; int* slot; };
  Lane lanes[] = {{"base_color", &mat->baseColorTex},
                  {"base_metalness", &mat->metallicTex},
                  {"specular_roughness", &mat->roughnessTex},
                  {"geometry_opacity", &mat->opacityTex},
                  {"emission_color", &mat->emissiveTex},
                  {"geometry_normal", &mat->normalTex}};
  for (int laneIndex = 0; laneIndex < 6; ++laneIndex) {
    const Lane& lane = lanes[laneIndex];
    std::set<int> visiting;
    if (outputs[laneIndex] < 0 || *lane.slot >= 0 ||
        !dependsOnImage(outputs[laneIndex], &visiting)) {
      continue;
    }
    DrawTextureCPU tex;
    tex.image.width = kSize;
    tex.image.height = kSize;
    tex.image.channels = 4;
    tex.image.data.resize(static_cast<size_t>(kSize * kSize * 4));
    tex.assetIdentifier = "mtlx-bake:" + mat->name + ":" + lane.name;
    bool ok = true;
    for (int y = 0; y < kSize && ok; ++y) {
      for (int x = 0; x < kSize; ++x) {
        DrawLightRtOpenPBRCPU p;
        if (!EvaluateMtlxDocAtUv(
                graphDoc, surfaceNode, graphTex,
                (static_cast<float>(x) + 0.5f) / kSize,
                (static_cast<float>(y) + 0.5f) / kSize,
                1.0f / kSize, 1.0f / kSize, &graphMemo,
                &graphMemoDone, &p)) {
          ok = false;
          break;
        }
        float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (std::strcmp(lane.name, "base_color") == 0) {
          value[0] = p.baseColor[0]; value[1] = p.baseColor[1]; value[2] = p.baseColor[2];
        } else if (std::strcmp(lane.name, "base_metalness") == 0) {
          value[0] = value[1] = value[2] = p.metalness;
        } else if (std::strcmp(lane.name, "specular_roughness") == 0) {
          value[0] = value[1] = value[2] = p.specularRoughness;
        } else if (std::strcmp(lane.name, "geometry_opacity") == 0) {
          value[0] = value[1] = value[2] = p.opacity;
        } else if (std::strcmp(lane.name, "emission_color") == 0) {
          value[0] = p.emissionColor[0] * p.emission;
          value[1] = p.emissionColor[1] * p.emission;
          value[2] = p.emissionColor[2] * p.emission;
        } else {
          value[0] = p.normal[0] * 0.5f + 0.5f;
          value[1] = p.normal[1] * 0.5f + 0.5f;
          value[2] = p.normal[2] * 0.5f + 0.5f;
        }
        const size_t base = static_cast<size_t>((y * kSize + x) * 4);
        for (int c = 0; c < 4; ++c) {
          const float v = std::max(0.0f, std::min(1.0f, value[c]));
          tex.image.data[base + static_cast<size_t>(c)] =
              static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
      }
    }
    if (!ok) continue;
    *lane.slot = static_cast<int>(scene->textures.size());
    scene->textures.push_back(std::move(tex));
    for (size_t nodeIndex = 0; nodeIndex < mat->materialXGraph.nodes.size();
         ++nodeIndex) {
      if (outputs[laneIndex] == static_cast<int>(nodeIndex))
        mat->materialXGraph.nodes[nodeIndex].textureId = *lane.slot;
    }
    if (std::strcmp(lane.name, "base_color") == 0) {
      mat->baseColor[0] = mat->baseColor[1] = mat->baseColor[2] = 1.0f;
    } else if (std::strcmp(lane.name, "base_metalness") == 0) {
      mat->metallic = 1.0f;
    } else if (std::strcmp(lane.name, "specular_roughness") == 0) {
      mat->roughness = 1.0f;
    } else if (std::strcmp(lane.name, "geometry_opacity") == 0) {
      mat->alpha = 1.0f;
    } else if (std::strcmp(lane.name, "emission_color") == 0) {
      mat->emissive[0] = mat->emissive[1] = mat->emissive[2] = 1.0f;
    } else {
      for (int c = 0; c < 3; ++c) {
        mat->normalSample.scale[c] = 2.0f;
        mat->normalSample.bias[c] = -1.0f;
      }
    }
  }
  texcache_free(graphTex);
  mtlx_free(graphDoc);
}

static void StoreEditableParam(DrawMaterialCPU* mat,
                               std::initializer_list<const char*> names,
                               const float* value, int components) {
  if (!mat || !value) return;
  for (DrawMaterialParamCPU& param : mat->params) {
    if (param.shader != "OpenPBRSurface" || param.texture >= 0 ||
        param.renderTexture >= 0) {
      continue;
    }
    bool match = false;
    for (const char* name : names) {
      if (param.name == name) {
        match = true;
        break;
      }
    }
    if (!match) continue;
    for (int c = 0; c < components; ++c) param.value[c] = value[c];
  }
}

void ApplyOpenPBRMaterialConstants(
    DrawMaterialCPU* mat, const DrawLightRtOpenPBRCPU& constants) {
  if (!mat || !mat->hasOpenPBRSurface) return;

  mat->lightRtOpenPBR = constants;
  lightusd::tydra::ClampRealtimePbrMaterial(&mat->lightRtOpenPBR);
  mat->hasLightRtOpenPBR = true;
  mat->openPbrSpecularModel = true;
  const DrawLightRtOpenPBRCPU& p = mat->lightRtOpenPBR;

  auto copy3 = [](const float src[3], float dst[3]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
  };
  copy3(p.baseColor, mat->baseColor);
  mat->metallic = p.metalness;
  mat->roughness = p.specularRoughness;
  copy3(p.specularColor, mat->specularColor);
  mat->ior = p.specularIor;
  mat->coatWeight = p.coatWeight;
  copy3(p.coatColor, mat->coatColor);
  mat->coatRoughness = p.coatRoughness;
  mat->coatIor = p.coatIor;
  mat->emissive[0] = p.emissionColor[0] * p.emission;
  mat->emissive[1] = p.emissionColor[1] * p.emission;
  mat->emissive[2] = p.emissionColor[2] * p.emission;
  mat->alpha = p.opacity;
  if (mat->opacityTex < 0) {
    mat->alphaMode = p.opacity < 0.999f
                         ? static_cast<int>(AlphaMode::Blend)
                         : static_cast<int>(AlphaMode::Opaque);
  }
  mat->volumeDensity = p.volumeDensity;
  copy3(p.volumeAlbedo, mat->volumeAlbedo);
  copy3(p.volumeEmission, mat->volumeEmission);
  mat->volumeEmissionScale = p.volumeEmissionScale;

  auto scalar = [&](std::initializer_list<const char*> names, float value) {
    StoreEditableParam(mat, names, &value, 1);
  };
  auto color = [&](std::initializer_list<const char*> names,
                   const float value[3]) {
    StoreEditableParam(mat, names, value, 3);
  };
  scalar({"base_weight"}, p.baseWeight);
  color({"base_color"}, p.baseColor);
  scalar({"base_diffuse_roughness"}, p.diffuseRoughness);
  scalar({"base_metalness"}, p.metalness);
  scalar({"specular_weight"}, p.specularWeight);
  color({"specular_color"}, p.specularColor);
  scalar({"specular_roughness", "base_roughness"}, p.specularRoughness);
  scalar({"specular_ior"}, p.specularIor);
  scalar({"specular_anisotropy"}, p.specularAnisotropy);
  scalar({"specular_rotation"}, p.specularRotation);
  scalar({"specular_roughness_anisotropy"},
         p.specularRoughnessAnisotropy);
  scalar({"transmission_weight"}, p.transmission);
  color({"transmission_color"}, p.transmissionColor);
  scalar({"transmission_depth"}, p.transmissionDepth);
  color({"transmission_scatter"}, p.transmissionScatter);
  scalar({"transmission_scatter_anisotropy"},
         p.transmissionScatterAnisotropy);
  scalar({"transmission_dispersion"}, p.transmissionDispersion);
  scalar({"transmission_dispersion_abbe_number"},
         p.transmissionDispersionAbbeNumber);
  scalar({"transmission_dispersion_scale"}, p.transmissionDispersionScale);
  scalar({"subsurface_weight"}, p.subsurface);
  color({"subsurface_color"}, p.subsurfaceColor);
  bool hasRadiusScaleParam = false;
  for (const DrawMaterialParamCPU& param : mat->params) {
    if (param.shader == "OpenPBRSurface" &&
        param.name == "subsurface_radius_scale" && param.texture < 0 &&
        param.renderTexture < 0) {
      hasRadiusScaleParam = true;
      break;
    }
  }
  const float radius = (p.subsurfaceRadius[0] + p.subsurfaceRadius[1] +
                        p.subsurfaceRadius[2]) /
                       3.0f;
  scalar({"subsurface_radius"}, hasRadiusScaleParam ? 1.0f : radius);
  if (hasRadiusScaleParam)
    color({"subsurface_radius_scale"}, p.subsurfaceRadius);
  scalar({"subsurface_scale"}, p.subsurfaceScale);
  scalar({"subsurface_anisotropy"}, p.subsurfaceAnisotropy);
  scalar({"subsurface_scatter_anisotropy"},
         p.subsurfaceScatterAnisotropy);
  scalar({"coat_weight"}, p.coatWeight);
  color({"coat_color"}, p.coatColor);
  scalar({"coat_roughness"}, p.coatRoughness);
  scalar({"coat_ior"}, p.coatIor);
  scalar({"coat_anisotropy"}, p.coatAnisotropy);
  scalar({"coat_rotation"}, p.coatRotation);
  scalar({"coat_affect_color"}, p.coatAffectColor);
  scalar({"coat_affect_roughness"}, p.coatAffectRoughness);
  scalar({"coat_roughness_anisotropy"}, p.coatRoughnessAnisotropy);
  scalar({"coat_darkening"}, p.coatDarkening);
  scalar({"sheen_weight", "fuzz_weight"}, p.sheenWeight);
  color({"sheen_color", "fuzz_color"}, p.sheenColor);
  scalar({"sheen_roughness", "fuzz_roughness"}, p.sheenRoughness);
  scalar({"thin_film_weight"}, p.thinFilmWeight);
  scalar({"thin_film_thickness"}, p.thinFilmThicknessNm);
  scalar({"thin_film_ior"}, p.thinFilmIor);
  scalar({"emission_luminance"}, p.emission);
  color({"emission_color"}, p.emissionColor);
  scalar({"geometry_opacity", "opacity"}, p.opacity);
  const float thinWalled = p.geometryThinWalled ? 1.0f : 0.0f;
  scalar({"geometry_thin_walled", "thin_walled"}, thinWalled);
}

void MakeConstantOpenPBRMaterial(DrawMaterialCPU* mat) {
  if (!mat || !mat->hasOpenPBRSurface) return;

  int* textureSlots[] = {
      &mat->baseColorTex,   &mat->metallicTex,      &mat->roughnessTex,
      &mat->normalTex,      &mat->coatNormalTex,    &mat->emissiveTex,
      &mat->opacityTex,     &mat->occlusionTex,     &mat->specularColorTex,
      &mat->coatWeightTex,  &mat->coatColorTex,     &mat->coatRoughnessTex};
  for (int* slot : textureSlots) *slot = -1;

  DrawTexSampleCPU* samples[] = {
      &mat->baseColorSample,  &mat->metallicSample,
      &mat->roughnessSample,  &mat->normalSample,
      &mat->coatNormalSample, &mat->emissiveSample,
      &mat->opacitySample,    &mat->occlusionSample,
      &mat->specularColorSample, &mat->coatWeightSample,
      &mat->coatColorSample,  &mat->coatRoughnessSample};
  for (DrawTexSampleCPU* sample : samples) sample->tex = -1;

  for (DrawMaterialParamCPU& param : mat->params) {
    if (param.shader == "OpenPBRSurface") {
      param.texture = -1;
      param.renderTexture = -1;
      param.sample.tex = -1;
    }
  }
  mat->materialXNodeGraphJson.clear();
  mat->materialXGraph = MaterialXGraphRuntimeCPU{};
  mat->lightRtOpenPBR.hasTextureInputs = false;
  mat->lightRtOpenPBR.hasNormalInput = false;
  mat->hasUsdPreviewSurface = false;
  mat->openPbrSpecularModel = true;
}

void BakeRealtimePbrMaterial(DrawMaterialCPU* mat) {
  if (!mat) return;
  if (!mat->volumeMaterialXNodeGraphJson.empty()) {
    DrawMaterialCPU volume;
    std::string volumeErr;
    if (EvaluateMaterialXJsonGraphForVolume(*mat, &volume, &volumeErr)) {
      mat->volumeDensity = volume.volumeDensity;
      std::memcpy(mat->volumeAlbedo, volume.volumeAlbedo,
                  sizeof(mat->volumeAlbedo));
      mat->volumeAnisotropy = volume.volumeAnisotropy;
      std::memcpy(mat->volumeEmission, volume.volumeEmission,
                  sizeof(mat->volumeEmission));
      mat->volumeEmissionScale = volume.volumeEmissionScale;
      mat->hasVolumeOutput = true;
    }
  }
  DrawLightRtOpenPBRCPU p;
  if (mat->hasOpenPBRSurface) {
    BakeOpenPBRSurface(*mat, &p);
  } else if (mat->hasUsdPreviewSurface) {
    BakeUsdPreviewSurface(*mat, &p);
  } else {
    std::memcpy(p.baseColor, mat->baseColor, sizeof(p.baseColor));
    p.metalness = mat->metallic;
    p.specularRoughness = mat->roughness;
    std::memcpy(p.emissionColor, mat->emissive, sizeof(p.emissionColor));
    p.emission = (Luminance(p.emissionColor) > 0.0f) ? 1.0f : 0.0f;
    p.opacity = mat->alpha;
  }
  p.hasTextureInputs = HasTextureInput(*mat);
  p.hasNormalInput = HasNormalInput(*mat);
  p.volumeDensity = mat->volumeDensity;
  std::memcpy(p.volumeAlbedo, mat->volumeAlbedo, sizeof(p.volumeAlbedo));
  if (mat->hasVolumeOutput) {
    p.transmissionScatterAnisotropy = mat->volumeAnisotropy;
  }
  std::memcpy(p.volumeEmission, mat->volumeEmission, sizeof(p.volumeEmission));
  p.volumeEmissionScale = mat->volumeEmissionScale;
  ClampLightRtParams(&p);

  std::set<std::string> textureDeps = DirectTextureInputs(*mat);
  const GraphTextureDeps graphDeps =
      AnalyzeMaterialXJsonTextureDeps(mat->materialXNodeGraphJson);
  textureDeps.insert(graphDeps.textureInputs.begin(), graphDeps.textureInputs.end());
  if (!textureDeps.empty()) {
    p.hasTextureInputs = true;
  }
  if (TextureDepsIncludeNormal(textureDeps)) {
    p.hasNormalInput = true;
  }

  // If Tydra extracted a MaterialX node graph, use LightRT's evaluator for the
  // graph constants. For mixed graphs, preserve lanes driven by image/normalmap
  // nodes or direct runtime textures; LightRT's no-image texture stub would
  // otherwise bake MaterialX `default` values over live texture inputs.
  if (mat->hasOpenPBRSurface && !mat->materialXNodeGraphJson.empty()) {
    const float directThinFilmWeight = p.thinFilmWeight;
    const float directThinFilmThickness = p.thinFilmThicknessNm;
    DrawLightRtOpenPBRCPU graphParams;
    std::string graphErr;
    if (EvaluateMaterialXJsonGraphForMaterial(*mat, &graphParams, &graphErr)) {
      const DrawLightRtOpenPBRCPU directParams = p;
      graphParams.hasTextureInputs = p.hasTextureInputs;
      graphParams.hasNormalInput = p.hasNormalInput ||
          TextureDepsIncludeNormal(textureDeps) ||
          (std::fabs(graphParams.normal[0]) > 0.0f ||
           std::fabs(graphParams.normal[1]) > 0.0f ||
           std::fabs(graphParams.normal[2] - 1.0f) > 1.0e-6f);
      MergeGraphParamsPreservingTextureDeps(graphParams, textureDeps, &p);
      // A mixed typed/graph material may contain an image on one surface
      // input (for example displacement) while its other inputs remain
      // authored directly on the surface shader. The graph evaluator starts
      // every lane from its own defaults; do not let that unrelated default
      // replace a correctly extracted typed value. Texture-driven lanes are
      // preserved by MergeGraphParamsPreservingTextureDeps above, and the
      // corresponding non-textured typed lanes are restored here.
      std::set<std::string> graphConnections;
      const nlohmann::json connectionJson = nlohmann::json::parse(
          mat->materialXNodeGraphJson.begin(),
          mat->materialXNodeGraphJson.end(), nullptr, false);
      const auto connectionIt = connectionJson.is_discarded()
                                    ? connectionJson.end()
                                    : connectionJson.find("connections");
      if (!connectionJson.is_discarded() &&
          connectionIt != connectionJson.end() && connectionIt->is_array()) {
        for (const nlohmann::json& connection : *connectionIt) {
          const std::string input = OpenPBREvalInputName(
              JsonString(connection, "input"));
          if (!input.empty()) graphConnections.insert(input);
        }
      }
      auto graphDrives = [&](const char* name) {
        static const std::map<std::string, size_t> graphLanes = {
            {"base_color", 0}, {"base_metalness", 1},
            {"specular_roughness", 2}, {"geometry_opacity", 3},
            {"emission_color", 4}, {"geometry_normal", 5}};
        const auto lane = graphLanes.find(name);
        if (lane != graphLanes.end() && mat->materialXGraph.valid)
          return mat->materialXGraph.output[lane->second] >= 0;
        return graphConnections.find(name) != graphConnections.end();
      };
      // Graph routes take precedence over direct fallback values; direct
      // parameters are restored only for lanes without an authored route.
      const bool hasDirectBaseColor =
          FindParam(*mat, "OpenPBRSurface", "base_color") != nullptr;
      const bool hasDirectMetalness =
          FindParam(*mat, "OpenPBRSurface", "base_metalness") != nullptr;
      const bool hasDirectRoughness =
          FindParam(*mat, "OpenPBRSurface", "specular_roughness") != nullptr ||
          FindParam(*mat, "OpenPBRSurface", "base_roughness") != nullptr;
      const bool hasDirectOpacity =
          FindParam(*mat, "OpenPBRSurface", "geometry_opacity") != nullptr;
      const bool hasDirectEmissionColor =
          FindParam(*mat, "OpenPBRSurface", "emission_color") != nullptr;
      const bool hasDirectEmission =
          FindParam(*mat, "OpenPBRSurface", "emission_luminance") != nullptr;
      const bool hasDirectNormal =
          FindParam(*mat, "OpenPBRSurface", "geometry_normal") != nullptr;
      if (hasDirectBaseColor && mat->baseColorTex < 0 &&
          !graphDrives("base_color")) {
        std::copy(std::begin(directParams.baseColor),
                  std::end(directParams.baseColor), std::begin(p.baseColor));
      }
      if (hasDirectMetalness && mat->metallicTex < 0 &&
          !graphDrives("base_metalness"))
        p.metalness = directParams.metalness;
      if (hasDirectRoughness && mat->roughnessTex < 0 &&
          !graphDrives("base_diffuse_roughness") &&
          !graphDrives("specular_roughness")) {
        p.diffuseRoughness = directParams.diffuseRoughness;
        p.specularRoughness = directParams.specularRoughness;
      }
      if (hasDirectOpacity && mat->opacityTex < 0 &&
          !graphDrives("geometry_opacity"))
        p.opacity = directParams.opacity;
      if (hasDirectEmissionColor && mat->emissiveTex < 0 &&
          !graphDrives("emission_color")) {
        std::copy(std::begin(directParams.emissionColor),
                  std::end(directParams.emissionColor),
                  std::begin(p.emissionColor));
        if (hasDirectEmission && !graphDrives("emission_luminance"))
          p.emission = directParams.emission;
      }
      if (hasDirectNormal && mat->normalTex < 0 &&
          !graphDrives("geometry_normal")) {
        std::copy(std::begin(directParams.normal), std::end(directParams.normal),
                  std::begin(p.normal));
      }
      // The core loader keeps typed OpenPBR values next to a JSON graph even
      // when only one surface input is graph-connected. Preserve every other
      // authored typed lane; the evaluator's defaults are not authored graph
      // routes and must not replace them. The legacy loader exposed this most
      // visibly as an IOR/F0 mismatch, but the same rule applies to all lobes.
      auto hasDirectParam = [&](std::initializer_list<const char*> names) {
        for (const char* name : names) {
          if (FindParam(*mat, "OpenPBRSurface", name) != nullptr) return true;
        }
        return false;
      };
      auto restoreScalar = [&](std::initializer_list<const char*> names,
                               const char* graphName, float direct,
                               float* destination) {
        if (destination && hasDirectParam(names) &&
            !ParamHasTexture(*mat, {"OpenPBRSurface"}, names) &&
            !graphDrives(graphName)) {
          *destination = direct;
        }
      };
      auto restoreColor = [&](std::initializer_list<const char*> names,
                              const char* graphName, const float direct[3],
                              float destination[3]) {
        if (hasDirectParam(names) &&
            !ParamHasTexture(*mat, {"OpenPBRSurface"}, names) &&
            !graphDrives(graphName)) {
          std::copy(direct, direct + 3, destination);
        }
      };
      restoreScalar({"base_weight"}, "base_weight", directParams.baseWeight,
                    &p.baseWeight);
      restoreScalar({"specular_weight"}, "specular_weight",
                    directParams.specularWeight, &p.specularWeight);
      restoreColor({"specular_color"}, "specular_color",
                   directParams.specularColor, p.specularColor);
      restoreScalar({"specular_ior"}, "specular_ior",
                    directParams.specularIor, &p.specularIor);
      restoreScalar({"specular_anisotropy"}, "specular_anisotropy",
                    directParams.specularAnisotropy, &p.specularAnisotropy);
      restoreScalar({"specular_rotation"}, "specular_rotation",
                    directParams.specularRotation, &p.specularRotation);
      restoreScalar({"specular_roughness_anisotropy"},
                    "specular_roughness_anisotropy",
                    directParams.specularRoughnessAnisotropy,
                    &p.specularRoughnessAnisotropy);
      restoreScalar({"transmission_weight"}, "transmission_weight",
                    directParams.transmission, &p.transmission);
      restoreColor({"transmission_color"}, "transmission_color",
                   directParams.transmissionColor, p.transmissionColor);
      restoreScalar({"transmission_depth"}, "transmission_depth",
                    directParams.transmissionDepth, &p.transmissionDepth);
      restoreColor({"transmission_scatter"}, "transmission_scatter",
                   directParams.transmissionScatter, p.transmissionScatter);
      restoreScalar({"transmission_scatter_anisotropy"},
                    "transmission_scatter_anisotropy",
                    directParams.transmissionScatterAnisotropy,
                    &p.transmissionScatterAnisotropy);
      restoreScalar({"transmission_dispersion"}, "transmission_dispersion",
                    directParams.transmissionDispersion,
                    &p.transmissionDispersion);
      restoreScalar({"transmission_dispersion_abbe_number"},
                    "transmission_dispersion_abbe_number",
                    directParams.transmissionDispersionAbbeNumber,
                    &p.transmissionDispersionAbbeNumber);
      restoreScalar({"transmission_dispersion_scale"},
                    "transmission_dispersion_scale",
                    directParams.transmissionDispersionScale,
                    &p.transmissionDispersionScale);
      restoreScalar({"subsurface_weight"}, "subsurface_weight",
                    directParams.subsurface, &p.subsurface);
      restoreColor({"subsurface_color"}, "subsurface_color",
                   directParams.subsurfaceColor, p.subsurfaceColor);
      restoreScalar({"subsurface_scale"}, "subsurface_scale",
                    directParams.subsurfaceScale, &p.subsurfaceScale);
      restoreScalar({"subsurface_anisotropy"}, "subsurface_anisotropy",
                    directParams.subsurfaceAnisotropy,
                    &p.subsurfaceAnisotropy);
      restoreScalar({"subsurface_scatter_anisotropy"},
                    "subsurface_scatter_anisotropy",
                    directParams.subsurfaceScatterAnisotropy,
                    &p.subsurfaceScatterAnisotropy);
      restoreScalar({"coat_weight"}, "coat_weight", directParams.coatWeight,
                    &p.coatWeight);
      restoreColor({"coat_color"}, "coat_color", directParams.coatColor,
                   p.coatColor);
      restoreScalar({"coat_roughness"}, "coat_roughness",
                    directParams.coatRoughness, &p.coatRoughness);
      restoreScalar({"coat_ior"}, "coat_ior", directParams.coatIor,
                    &p.coatIor);
      restoreScalar({"coat_anisotropy"}, "coat_anisotropy",
                    directParams.coatAnisotropy, &p.coatAnisotropy);
      restoreScalar({"coat_rotation"}, "coat_rotation",
                    directParams.coatRotation, &p.coatRotation);
      restoreScalar({"coat_affect_color"}, "coat_affect_color",
                    directParams.coatAffectColor, &p.coatAffectColor);
      restoreScalar({"coat_affect_roughness"}, "coat_affect_roughness",
                    directParams.coatAffectRoughness,
                    &p.coatAffectRoughness);
      restoreScalar({"coat_roughness_anisotropy"},
                    "coat_roughness_anisotropy",
                    directParams.coatRoughnessAnisotropy,
                    &p.coatRoughnessAnisotropy);
      restoreScalar({"coat_darkening"}, "coat_darkening",
                    directParams.coatDarkening, &p.coatDarkening);
      restoreScalar({"sheen_weight", "fuzz_weight"}, "sheen_weight",
                    directParams.sheenWeight, &p.sheenWeight);
      restoreColor({"sheen_color", "fuzz_color"}, "sheen_color",
                   directParams.sheenColor, p.sheenColor);
      restoreScalar({"sheen_roughness", "fuzz_roughness"},
                    "sheen_roughness", directParams.sheenRoughness,
                    &p.sheenRoughness);
      restoreScalar({"thin_film_weight"}, "thin_film_weight",
                    directParams.thinFilmWeight, &p.thinFilmWeight);
      restoreScalar({"thin_film_thickness"}, "thin_film_thickness",
                    directParams.thinFilmThicknessNm,
                    &p.thinFilmThicknessNm);
      restoreScalar({"thin_film_ior"}, "thin_film_ior",
                    directParams.thinFilmIor, &p.thinFilmIor);
      const bool graphHasRadiusScale =
          graphConnections.find("subsurface_radius_scale") !=
          graphConnections.end();
      const bool graphHasRadius =
          graphConnections.find("subsurface_radius") != graphConnections.end();
      if (graphHasRadiusScale && !graphHasRadius) {
        // A graph radius-scale output is represented by the vector radius lane.
        // When the radius itself is authored directly on the material, combine
        // the two values instead of allowing the evaluator's default radius to
        // replace the authored scalar.
        for (int i = 0; i < 3; ++i) {
          p.subsurfaceRadius[i] = directParams.subsurfaceRadius[i] *
                                  graphParams.subsurfaceRadius[i];
        }
      }
      // A graph evaluator starts from OpenPBR defaults (500 nm film), while a
      // converted Standard Surface with no authored film is explicitly 0 nm.
      // Do not let evaluation of an unrelated graph-connected lane (such as a
      // specular-color image) manufacture an iridescent film lobe.
      if (directThinFilmWeight <= 0.0f) {
        p.thinFilmWeight = 0.0f;
        p.thinFilmThicknessNm = directThinFilmThickness;
      }
      p.hasTextureInputs = graphParams.hasTextureInputs;
      p.hasNormalInput = graphParams.hasNormalInput;
      ClampLightRtParams(&p);
    }
  }

  mat->lightRtOpenPBR = p;
  mat->hasLightRtOpenPBR = true;

  // Keep legacy material slots aligned with the canonical OpenPBR block;
  // texture-backed lanes remain neutralized and are sampled at hit time.
  if (mat->baseColorTex < 0) {
    std::memcpy(mat->baseColor, p.baseColor, sizeof(mat->baseColor));
  }
  if (mat->metallicTex < 0) {
    mat->metallic = p.metalness;
  }
  if (mat->roughnessTex < 0) {
    mat->roughness = p.specularRoughness;
  }
  mat->ior = p.specularIor;
  if (mat->specularColorTex < 0) {
    mat->specularColor[0] = p.specularColor[0];
    mat->specularColor[1] = p.specularColor[1];
    mat->specularColor[2] = p.specularColor[2];
  }
  if (mat->emissiveTex < 0) {
    mat->emissive[0] = p.emissionColor[0] * p.emission;
    mat->emissive[1] = p.emissionColor[1] * p.emission;
    mat->emissive[2] = p.emissionColor[2] * p.emission;
  }
  mat->alpha = p.opacity;
  mat->coatWeight = p.coatWeight;
  mat->coatColor[0] = p.coatColor[0];
  mat->coatColor[1] = p.coatColor[1];
  mat->coatColor[2] = p.coatColor[2];
  mat->coatRoughness = p.coatRoughness;
  mat->coatIor = p.coatIor;
  FloatParam(*mat, {"UsdPreviewSurface"}, {"occlusion"}, &mat->occlusion);
}

void BakeLightRtOpenPBR(DrawMaterialCPU* mat) {
  BakeRealtimePbrMaterial(mat);
}

}  // namespace lusdview
