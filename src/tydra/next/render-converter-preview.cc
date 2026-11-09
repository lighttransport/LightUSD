// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Surface-schema field mappings.
#include "render-converter-internal.hh"
#include "render-converter-mtlx-eval.hh"
#include "render-converter-shader-param.hh"
#include "next/schema/usd-shade.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>

namespace lightusd { namespace tydra { namespace next {
using shader_param_detail::FindConnectedUtilityScalar;

bool RenderSceneConverter::Impl::ExtractPreviewSurface(const Stage& stage,
                                                 const UsdPrim& shader_prim,
                                                 PreviewSurfaceShader* out,
                                                 RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  // AttributeEval returns a successful zero-valued result for an unauthored
  // shader input.  Those zeros are not the UsdPreviewSurface schema defaults
  // (most importantly, unauthored opacity is 1).  Preserve the initialized
  // PreviewSurfaceShader defaults unless the input is actually authored or
  // connected.  This also keeps the 0.5 roughness, 1.5 IOR and unit occlusion
  // fallbacks instead of silently replacing them with zero.
  ::lightusd::next::AttributeEval authored_eval(&stage);
  authored_eval.SetTime(config_.time_code);
  auto extract_authored = [&](const char* name, ShaderParam* param) {
    const std::string attr_name = std::string("inputs:") + name;
    if (GetAttribute(shader_prim, attr_name) == nullptr &&
        !authored_eval.HasConnection(shader_prim, attr_name)) {
      return;
    }
    (void)ExtractShaderParam(stage, shader_prim, name, param, scene);
  };

  extract_authored("diffuseColor", &out->diffuse_color);
  extract_authored("emissiveColor", &out->emissive_color);
  extract_authored("specularColor", &out->specular_color);
  extract_authored("metallic", &out->metallic);
  extract_authored("roughness", &out->roughness);
  extract_authored("clearcoat", &out->clearcoat);
  extract_authored("clearcoatRoughness", &out->clearcoat_roughness);
  extract_authored("opacity", &out->opacity);
  extract_authored("opacityThreshold", &out->opacity_threshold);
  extract_authored("ior", &out->ior);
  extract_authored("normal", &out->normal);
  extract_authored("displacement", &out->displacement);
  extract_authored("occlusion", &out->occlusion);

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(config_.time_code);
  if (std::optional<int32_t> use_spec =
          eval.EvalInt(shader_prim, "inputs:useSpecularWorkflow")) {
    out->use_specular_workflow = (*use_spec != 0);
  }

  return true;
}

bool RenderSceneConverter::Impl::ExtractPxrDisneyAsOpenPBR(
    const Stage& stage, const UsdPrim& shader_prim, OpenPBRSurfaceShader* out,
    RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  // RenderMan's PxrDisney is a layered Disney BRDF. The preview transport is
  // OpenPBR, so map the lobes with a direct counterpart and leave the rest at
  // their schema default. This is an approximation for previewing assets that
  // expose only the ri:surface terminal; it is not a RenderMan-compatible
  // PxrDisney evaluation.
  ExtractShaderParam(stage, shader_prim, "baseColor", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "metallic", &out->base_metalness,
                     scene);
  ExtractShaderParam(stage, shader_prim, "roughness",
                     &out->specular_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "ior", &out->specular_ior, scene);
  ExtractShaderParam(stage, shader_prim, "anisotropic",
                     &out->specular_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface",
                     &out->subsurface_weight, scene);
  ExtractShaderParam(stage, shader_prim, "sheen", &out->sheen_weight, scene);
  ExtractShaderParam(stage, shader_prim, "clearcoat", &out->coat_weight,
                     scene);
  ExtractShaderParam(stage, shader_prim, "opacity", &out->opacity, scene);
  ExtractShaderParam(stage, shader_prim, "normal", &out->normal, scene);
  // PxrDisney's clearcoatGloss is 0 = blurry, 1 = sharp; OpenPBR stores a
  // roughness.
  ShaderParam gloss = {-1, {1, 0, 0, 0}};
  if (ExtractShaderParam(stage, shader_prim, "clearcoatGloss", &gloss, scene)) {
    SetParamFloat(&out->coat_roughness, std::max(0.0f, 1.0f - gloss.value.x));
  }
  // `isThin` switches the Disney transmission from refraction to a thin wall.
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(config_.time_code);
  if (std::optional<int32_t> thin = eval.EvalInt(shader_prim, "inputs:isThin")) {
    SetParamFloat(&out->thin_walled, *thin != 0 ? 1.0f : 0.0f);
  }
  return true;
}

bool RenderSceneConverter::Impl::ExtractStandardSurfaceAsOpenPBR(
    const Stage& stage, const UsdPrim& shader_prim, OpenPBRSurfaceShader* out,
    RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  // Base layer
  // AttributeEval can return a successful zero-valued result for an
  // unauthored MaterialX input.  That is not the Standard Surface default:
  // `base` defaults to 1. Preserve the initialized schema default unless the
  // input is explicitly authored or connected.
  const bool base_authored =
      GetAttribute(shader_prim, "inputs:base") != nullptr ||
      ::lightusd::next::AttributeEval(&stage).HasConnection(
          shader_prim, "inputs:base");
  if (base_authored) {
    ExtractShaderParam(stage, shader_prim, "base", &out->base_weight, scene);
  }
  ExtractShaderParam(stage, shader_prim, "base_color", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "diffuse_roughness",
                     &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "metalness", &out->base_metalness,
                     scene);

  // Specular layer
  ExtractShaderParam(stage, shader_prim, "specular", &out->specular_weight,
                     scene);
  ExtractShaderParam(stage, shader_prim, "specular_color",
                     &out->specular_color, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness",
                     &out->specular_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "specular_IOR", &out->specular_ior,
                     scene);
  ExtractShaderParam(stage, shader_prim, "specular_anisotropy",
                     &out->specular_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness_anisotropy",
                     &out->specular_roughness_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_rotation",
                     &out->specular_rotation, scene);

  // Transmission
  ExtractShaderParam(stage, shader_prim, "transmission",
                     &out->transmission_weight, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_color",
                     &out->transmission_color, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_depth",
                     &out->transmission_depth, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion",
                     &out->transmission_dispersion, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion_scale",
                     &out->transmission_dispersion_scale, scene);

  // Subsurface
  ExtractShaderParam(stage, shader_prim, "subsurface",
                     &out->subsurface_weight, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_color",
                     &out->subsurface_color, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_radius",
                     &out->subsurface_radius, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_scale",
                     &out->subsurface_scale, scene);

  // Sheen
  ExtractShaderParam(stage, shader_prim, "sheen", &out->sheen_weight, scene);
  ExtractShaderParam(stage, shader_prim, "sheen_color", &out->sheen_color,
                     scene);
  ExtractShaderParam(stage, shader_prim, "sheen_roughness",
                     &out->sheen_roughness, scene);

  // Coat
  ExtractShaderParam(stage, shader_prim, "coat", &out->coat_weight, scene);
  ExtractShaderParam(stage, shader_prim, "coat_color", &out->coat_color,
                     scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness",
                     &out->coat_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "coat_IOR", &out->coat_ior, scene);
  ExtractShaderParam(stage, shader_prim, "coat_normal", &out->coat_normal,
                     scene);

  // Emission
  ExtractShaderParam(stage, shader_prim, "emission", &out->emission_luminance,
                     scene);
  ExtractShaderParam(stage, shader_prim, "emission_color",
                     &out->emission_color, scene);

  // Geometry
  ExtractShaderParam(stage, shader_prim, "normal", &out->normal, scene);
  ExtractShaderParam(stage, shader_prim, "opacity", &out->opacity, scene);
  ExtractShaderParam(stage, shader_prim, "displacement", &out->displacement,
                     scene);

  return true;
}

bool RenderSceneConverter::Impl::ExtractOpenPBRSurface(const Stage& stage,
                                                 const UsdPrim& shader_prim,
                                                 OpenPBRSurfaceShader* out,
                                                 RenderScene* scene) {
  if (!out || !::lightusd::next::IsShader(shader_prim)) return false;

  ExtractShaderParam(stage, shader_prim, "base_weight", &out->base_weight, scene);
  ExtractShaderParam(stage, shader_prim, "base_color", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "baseColor", &out->base_color, scene);
  ExtractShaderParam(stage, shader_prim, "base_roughness", &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "base_diffuse_roughness",
                     &out->base_diffuse_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "roughness", &out->base_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "base_metalness", &out->base_metalness, scene);
  ExtractShaderParam(stage, shader_prim, "metalness", &out->base_metalness, scene);

  ExtractShaderParam(stage, shader_prim, "specular_weight", &out->specular_weight, scene);
  ExtractShaderParam(stage, shader_prim, "specular_color", &out->specular_color, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness",
                     &out->specular_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "specular_ior", &out->specular_ior, scene);
  ExtractShaderParam(stage, shader_prim, "specular_anisotropy",
                     &out->specular_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_roughness_anisotropy",
                     &out->specular_roughness_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "specular_rotation",
                     &out->specular_rotation, scene);

  ExtractShaderParam(stage, shader_prim, "transmission_weight",
                     &out->transmission_weight, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_color",
                     &out->transmission_color, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_depth",
                     &out->transmission_depth, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_scatter",
                     &out->transmission_scatter, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_scatter_anisotropy",
                     &out->transmission_scatter_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion",
                     &out->transmission_dispersion, scene);
  ExtractShaderParam(stage, shader_prim, "transmission_dispersion_scale",
                     &out->transmission_dispersion_scale, scene);

  ExtractShaderParam(stage, shader_prim, "subsurface_weight",
                     &out->subsurface_weight, scene);
  ExtractShaderParam(stage, shader_prim, "subsurface_color",
                     &out->subsurface_color, scene);
  // OpenPBR represents the diffusion distance as a scalar radius multiplied
  // by a per-channel radius-scale color. Keep those inputs independent: all
  // transport backends already multiply the RGB radius by the scalar scale,
  // and retaining both lanes also preserves graph-driven radius textures.
  ShaderParam subsurfaceRadius;
  SetParamFloat(&subsurfaceRadius, 1.0f);
  ShaderParam subsurfaceRadiusScale;
  SetParamFloat3(&subsurfaceRadiusScale, 1.0f, 1.0f, 1.0f);
  const bool hasSubsurfaceRadius = ExtractShaderParam(
      stage, shader_prim, "subsurface_radius", &subsurfaceRadius, scene);
  const bool hasSubsurfaceRadiusScale = ExtractShaderParam(
      stage, shader_prim, "subsurface_radius_scale",
      &subsurfaceRadiusScale, scene);
  if (hasSubsurfaceRadius || hasSubsurfaceRadiusScale) {
    out->subsurface_radius = subsurfaceRadiusScale;
    out->subsurface_scale = subsurfaceRadius;
  }
  ExtractShaderParam(stage, shader_prim, "subsurface_anisotropy",
                     &out->subsurface_anisotropy, scene);

  ExtractShaderParam(stage, shader_prim, "coat_weight", &out->coat_weight, scene);
  ExtractShaderParam(stage, shader_prim, "coat_color", &out->coat_color, scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness", &out->coat_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "coat_rotation", &out->coat_rotation, scene);
  ExtractShaderParam(stage, shader_prim, "coat_ior", &out->coat_ior, scene);
  ExtractShaderParam(stage, shader_prim, "coat_affect_color",
                     &out->coat_affect_color, scene);
  ExtractShaderParam(stage, shader_prim, "coat_affect_roughness",
                     &out->coat_affect_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "coat_anisotropy",
                     &out->coat_anisotropy, scene);
  ExtractShaderParam(stage, shader_prim, "coat_roughness_anisotropy",
                     &out->coat_roughness_anisotropy, scene);
  if (!ExtractShaderParam(stage, shader_prim, "geometry_coat_normal",
                          &out->coat_normal, scene)) {
    ExtractShaderParam(stage, shader_prim, "coat_normal", &out->coat_normal,
                       scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_coat_tangent",
                          &out->coat_tangent, scene)) {
    ExtractShaderParam(stage, shader_prim, "coat_tangent", &out->coat_tangent,
                       scene);
  }
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_coat_normal", "ND_normalmap_", "scale",
      config_.time_code, &out->coat_normal_map_scale);
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_coat_tangent", "ND_rotate3d_", "amount",
      config_.time_code, &out->coat_tangent_rotation);

  // OpenPBR renamed the grazing cloth lobe to fuzz. Retain the older sheen
  // spellings for Standard Surface and early OpenPBR files, but prefer the
  // current names when both are authored.
  const bool hasFuzzWeight = ExtractShaderParam(
      stage, shader_prim, "fuzz_weight", &out->fuzz_weight, scene);
  ExtractShaderParam(stage, shader_prim, "sheen_weight",
                     &out->sheen_weight, scene);
  const bool hasFuzzColor = ExtractShaderParam(
      stage, shader_prim, "fuzz_color", &out->fuzz_color, scene);
  ExtractShaderParam(stage, shader_prim, "sheen_color",
                     &out->sheen_color, scene);
  const bool hasFuzzRoughness = ExtractShaderParam(
      stage, shader_prim, "fuzz_roughness", &out->fuzz_roughness, scene);
  ExtractShaderParam(stage, shader_prim, "sheen_roughness",
                     &out->sheen_roughness, scene);
  out->fuzz_authored = hasFuzzWeight || hasFuzzColor || hasFuzzRoughness;
  // The render payload's sheen slots are the effective grazing-lobe values
  // consumed by existing renderers. Keep fuzz separately for lossless C and
  // serializer queries while preserving the established renderer behavior.
  if (hasFuzzWeight) out->sheen_weight = out->fuzz_weight;
  if (hasFuzzColor) out->sheen_color = out->fuzz_color;
  if (hasFuzzRoughness) out->sheen_roughness = out->fuzz_roughness;
  ExtractShaderParam(stage, shader_prim, "thin_film_weight",
                     &out->thin_film_weight, scene);
  ExtractShaderParam(stage, shader_prim, "thin_film_thickness",
                     &out->thin_film_thickness, scene);
  ExtractShaderParam(stage, shader_prim, "thin_film_ior",
                     &out->thin_film_ior, scene);

  ExtractShaderParam(stage, shader_prim, "emission_luminance",
                     &out->emission_luminance, scene);
  ExtractShaderParam(stage, shader_prim, "emission_color", &out->emission_color, scene);

  // OpenPBR prefixes geometry inputs. Accept the older short aliases as a
  // fallback for exporters that authored pre-1.39 spellings.
  if (!ExtractShaderParam(stage, shader_prim, "geometry_opacity",
                          &out->opacity, scene)) {
    ExtractShaderParam(stage, shader_prim, "opacity", &out->opacity, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_thin_walled",
                          &out->thin_walled, scene)) {
    ExtractShaderParam(stage, shader_prim, "thin_walled",
                       &out->thin_walled, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_normal",
                          &out->normal, scene)) {
    ExtractShaderParam(stage, shader_prim, "normal", &out->normal, scene);
  }
  if (!ExtractShaderParam(stage, shader_prim, "geometry_tangent",
                          &out->tangent, scene)) {
    ExtractShaderParam(stage, shader_prim, "tangent", &out->tangent, scene);
  }
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_normal", "ND_normalmap_", "scale",
      config_.time_code, &out->normal_map_scale);
  (void)FindConnectedUtilityScalar(
      stage, shader_prim, "geometry_tangent", "ND_rotate3d_", "amount",
      config_.time_code, &out->tangent_rotation);

  return true;
}

} } }  // namespace lightusd::tydra::next
