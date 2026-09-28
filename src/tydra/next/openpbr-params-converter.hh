// SPDX-License-Identifier: Apache-2.0
// Convert tydra-next render materials into the shared LightRT/OpenPBR block.
#pragma once

#include <initializer_list>

#include "render-data.hh"
#include "tydra/openpbr-params.hh"

namespace lightusd {
namespace tydra {
namespace next {

inline void CopyShaderParam3ToOpenPBR(const ShaderParam &param, float dst[3]) {
  dst[0] = param.value.x;
  dst[1] = param.value.y;
  dst[2] = param.value.z;
}

inline bool ShaderParamHasTexture(const ShaderParam &param) {
  return param.texture_id >= 0;
}

inline bool AnyShaderParamHasTexture(
    std::initializer_list<const ShaderParam *> params) {
  for (const ShaderParam *p : params) {
    if (p && ShaderParamHasTexture(*p)) return true;
  }
  return false;
}

inline bool BuildRealtimePbrMaterial(
    const RenderMaterial &rm, lightusd::tydra::RealtimePbrMaterial *out) {
  if (!out) return false;
  lightusd::tydra::RealtimePbrMaterial p;
  if (rm.shader_type == RenderMaterial::ShaderType::PreviewSurface &&
      rm.preview_surface) {
    const PreviewSurfaceShader &s = *rm.preview_surface;
    CopyShaderParam3ToOpenPBR(s.diffuse_color, p.baseColor);
    CopyShaderParam3ToOpenPBR(s.specular_color, p.specularColor);
    p.metalness = s.metallic.value.x;
    p.specularRoughness = s.roughness.value.x;
    p.specularIor = s.ior.value.x > 0.0f ? s.ior.value.x : 1.5f;
    p.coatWeight = s.clearcoat.value.x;
    p.coatRoughness = s.clearcoat_roughness.value.x;
    p.opacity = s.opacity.value.x;
    CopyShaderParam3ToOpenPBR(s.normal, p.normal);
    CopyShaderParam3ToOpenPBR(s.emissive_color, p.emissionColor);
    p.emission = (p.emissionColor[0] > 0.0f || p.emissionColor[1] > 0.0f ||
                  p.emissionColor[2] > 0.0f)
                     ? 1.0f
                     : 0.0f;
    p.hasTextureInputs = AnyShaderParamHasTexture({
        &s.diffuse_color, &s.emissive_color, &s.specular_color, &s.metallic,
        &s.roughness, &s.clearcoat, &s.clearcoat_roughness, &s.opacity,
        &s.normal, &s.displacement, &s.occlusion});
    p.hasNormalInput = ShaderParamHasTexture(s.normal);
  } else if (rm.shader_type == RenderMaterial::ShaderType::OpenPBR &&
             rm.openpbr) {
    const OpenPBRSurfaceShader &s = *rm.openpbr;
    p.baseWeight = s.base_weight.value.x;
    CopyShaderParam3ToOpenPBR(s.base_color, p.baseColor);
    p.diffuseRoughness = s.base_roughness.value.x;
    p.metalness = s.base_metalness.value.x;
    p.specularWeight = s.specular_weight.value.x;
    CopyShaderParam3ToOpenPBR(s.specular_color, p.specularColor);
    p.specularRoughness = s.specular_roughness.value.x;
    p.specularIor = s.specular_ior.value.x > 0.0f ? s.specular_ior.value.x : 1.5f;
    p.transmission = s.transmission_weight.value.x;
    CopyShaderParam3ToOpenPBR(s.transmission_color, p.transmissionColor);
    p.transmissionDepth = s.transmission_depth.value.x;
    p.subsurface = s.subsurface_weight.value.x;
    CopyShaderParam3ToOpenPBR(s.subsurface_color, p.subsurfaceColor);
    CopyShaderParam3ToOpenPBR(s.subsurface_radius, p.subsurfaceRadius);
    p.subsurfaceScale = s.subsurface_scale.value.x;
    p.coatWeight = s.coat_weight.value.x;
    CopyShaderParam3ToOpenPBR(s.coat_color, p.coatColor);
    p.coatRoughness = s.coat_roughness.value.x;
    p.coatIor = s.coat_ior.value.x > 0.0f ? s.coat_ior.value.x : 1.5f;
    const ShaderParam& sheen_weight = s.fuzz_authored ? s.fuzz_weight : s.sheen_weight;
    const ShaderParam& sheen_color = s.fuzz_authored ? s.fuzz_color : s.sheen_color;
    const ShaderParam& sheen_roughness =
        s.fuzz_authored ? s.fuzz_roughness : s.sheen_roughness;
    p.sheenWeight = sheen_weight.value.x;
    CopyShaderParam3ToOpenPBR(sheen_color, p.sheenColor);
    p.sheenRoughness = sheen_roughness.value.x;
    p.thinFilmWeight = s.thin_film_weight.value.x;
    p.thinFilmThicknessNm = s.thin_film_thickness.value.x;
    p.thinFilmIor = s.thin_film_ior.value.x;
    p.emission = s.emission_luminance.value.x;
    CopyShaderParam3ToOpenPBR(s.emission_color, p.emissionColor);
    p.opacity = s.opacity.value.x;
    p.geometryThinWalled = s.thin_walled.value.x >= 0.5f;
    CopyShaderParam3ToOpenPBR(s.normal, p.normal);
    p.hasTextureInputs = AnyShaderParamHasTexture({
        &s.base_weight, &s.base_color, &s.base_roughness, &s.base_metalness,
        &s.specular_weight, &s.specular_color, &s.specular_roughness,
        &s.specular_ior, &s.transmission_weight, &s.transmission_color,
        &s.transmission_depth, &s.subsurface_weight, &s.subsurface_color,
        &s.subsurface_radius, &s.subsurface_scale, &s.coat_weight,
        &s.coat_color, &s.coat_roughness, &s.coat_ior, &s.sheen_weight,
        &s.sheen_color, &s.sheen_roughness, &s.fuzz_weight, &s.fuzz_color,
        &s.fuzz_roughness, &s.thin_film_weight, &s.thin_film_thickness,
        &s.thin_film_ior, &s.emission_luminance, &s.emission_color,
        &s.opacity, &s.thin_walled, &s.normal, &s.coat_normal,
        &s.displacement});
    p.hasNormalInput = ShaderParamHasTexture(s.normal);
  } else {
    return false;
  }
  lightusd::tydra::ClampRealtimePbrMaterial(&p);
  *out = p;
  return true;
}

// Compatibility entry point for lusdrender and downstream callers. New code
// should use BuildRealtimePbrMaterial to avoid coupling Tydra extraction to a
// particular evaluator.
inline bool BuildLightRtOpenPBRParams(
    const RenderMaterial &rm, lightusd::tydra::LightRtOpenPBRParams *out) {
  return BuildRealtimePbrMaterial(rm, out);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
