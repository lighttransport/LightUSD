// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Tydra Next - light conversion
#include "render-converter.hh"
#include "next/schema/usd-lux.hh"
#include <string>
#include <vector>
namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
namespace {

bool GetBool(const UsdPrim& prim, const char* name, bool* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  const bool* result = value ? value->as_bool() : nullptr;
  if (!result) return false;
  *out = *result;
  return true;
}
bool GetFloat(const UsdPrim& prim, const char* name, float* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const float* result = value->as_float()) { *out = *result; return true; }
  if (const double* result = value->as_double()) { *out = static_cast<float>(*result); return true; }
  return false;
}
bool GetFloat3(const UsdPrim& prim, const char* name, float* x, float* y, float* z) {
  if (!x || !y || !z) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const float* result = value->as_float3()) { *x=result[0]; *y=result[1]; *z=result[2]; return true; }
  if (const double* result = value->as_double3()) { *x=static_cast<float>(result[0]); *y=static_cast<float>(result[1]); *z=static_cast<float>(result[2]); return true; }
  return false;
}
bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  if (!out) return false;
  const Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (const std::string* result = value->as_token()) { *out = *result; return true; }
  if (const std::string* result = value->as_string()) { *out = *result; return true; }
  return false;
}
bool ReadStringLikeProperty(const UsdPrim& prim, const char* name, std::string* out) {
  if (!out) return false;
  if (GetToken(prim, name, out)) return true;
  const Value* value = prim.GetPropertyValue(name);
  if (value && value->as_asset_path()) { *out = *value->as_asset_path(); return true; }
  return false;
}
std::vector<std::string> ReadRelationshipTargets(const UsdPrim& prim, const char* name) {
  std::vector<std::string> out;
  const auto* targets = prim.GetRelationship(name);
  if (!targets) return out;
  out.reserve(targets->size());
  for (const auto& target : *targets) out.push_back(target.str());
  return out;
}

}  // namespace

bool RenderSceneConverter::ConvertLight(const UsdPrim& prim, RenderLight* out) {
  if (!out || !::lightusd::tydra::next::IsLight(prim)) {
    SetLastError("Invalid light prim");
    return false;
  }

  out->name = prim.GetName();
  out->prim_path = prim.GetPath().str();

  // Determine light type
  LightKind kind = GetLightKind(prim);
  switch (kind) {
    case LightKind::DistantLight: out->type = LightType::Directional; break;
    case LightKind::DomeLight: out->type = LightType::Dome; break;
    case LightKind::RectLight: out->type = LightType::Rect; break;
    case LightKind::DiskLight: out->type = LightType::Disk; break;
    case LightKind::SphereLight: out->type = LightType::Sphere; break;
    case LightKind::CylinderLight: out->type = LightType::Cylinder; break;
    case LightKind::GeometryLight: out->type = LightType::Geometry; break;
    case LightKind::PortalLight: out->type = LightType::Rect; break;
    case LightKind::PluginLight:
      out->type = LightType::Point;
      AddWarning("PluginLight '" + prim.GetPath().str() +
                          "': shader registry evaluation is unsupported; "
                          "using point light fallback");
      break;
    case LightKind::LightFilter:
    case LightKind::PluginLightFilter:
      out->type = LightType::Point;
      AddWarning("Light filter '" + prim.GetPath().str() +
                          "': filter evaluation is unsupported; "
                          "using inert point light fallback");
      out->intensity = 0.0f;
      break;
    default: out->type = LightType::Point; break;
  }

  // Common properties
  GetFloat3(prim, "inputs:color", &out->color.x, &out->color.y, &out->color.z);
  GetFloat(prim, "inputs:intensity", &out->intensity);
  GetFloat(prim, "inputs:exposure", &out->exposure);
  GetBool(prim, "inputs:normalize", &out->normalize);
  GetBool(prim, "inputs:enableColorTemperature", &out->enable_color_temperature);
  GetFloat(prim, "inputs:colorTemperature", &out->color_temperature);
  GetFloat(prim, "inputs:diffuse", &out->diffuse);
  GetFloat(prim, "inputs:specular", &out->specular);
  GetFloat(prim, "inputs:shaping:cone:angle", &out->shaping_cone_angle);
  GetFloat(prim, "inputs:shaping:focus", &out->shaping_focus);
  GetFloat3(prim, "inputs:shaping:focusTint", &out->shaping_focus_tint.x,
            &out->shaping_focus_tint.y, &out->shaping_focus_tint.z);
  GetFloat(prim, "inputs:shaping:cone:softness", &out->shaping_cone_softness);
  ReadStringLikeProperty(prim, "inputs:shaping:ies:file",
                        &out->shaping_ies_file);
  GetFloat(prim, "inputs:shaping:ies:angleScale", &out->shaping_ies_angle_scale);
  GetBool(prim, "inputs:shaping:ies:normalize", &out->shaping_ies_normalize);
  out->light_link_targets = ReadRelationshipTargets(prim, "light:link");
  if (out->light_link_targets.empty()) {
    out->light_link_targets = ReadRelationshipTargets(prim, "collection:lightLink:includes");
  }
  out->shadow_link_targets = ReadRelationshipTargets(prim, "shadow:link");
  if (out->shadow_link_targets.empty()) {
    out->shadow_link_targets =
        ReadRelationshipTargets(prim, "collection:shadowLink:includes");
  }
  out->filter_targets = ReadRelationshipTargets(prim, "filters");
  if (out->filter_targets.empty()) {
    out->filter_targets = ReadRelationshipTargets(prim, "light:filters");
  }

  // Type-specific properties
  switch (out->type) {
    case LightType::Sphere: {
      out->params.sphere.radius = 0.5f;
      GetFloat(prim, "inputs:radius", &out->params.sphere.radius);
      // Cone shaping on a sphere light makes it a spot light.
      float cone_angle = 0.0f;
      if (GetFloat(prim, "inputs:shaping:cone:angle", &cone_angle)) {
        out->type = LightType::Spot;
        out->params.spot.angle = cone_angle * 3.14159265358979323846f / 180.0f;
      }
      break;
    }
    case LightType::Rect:
      out->params.rect.width = 1.0f;
      out->params.rect.height = 1.0f;
      GetFloat(prim, "inputs:width", &out->params.rect.width);
      GetFloat(prim, "inputs:height", &out->params.rect.height);
      break;
    case LightType::Disk:
      out->params.disk.radius = 0.5f;
      GetFloat(prim, "inputs:radius", &out->params.disk.radius);
      break;
    case LightType::Cylinder:
      out->params.cylinder.radius = 0.5f;
      out->params.cylinder.length = 1.0f;
      GetFloat(prim, "inputs:radius", &out->params.cylinder.radius);
      GetFloat(prim, "inputs:length", &out->params.cylinder.length);
      break;
    case LightType::Directional:
      out->params.distant.angle = 0.53f;
      GetFloat(prim, "inputs:angle", &out->params.distant.angle);
      break;
    case LightType::Dome: {
      std::string format;
      if (GetToken(prim, "inputs:texture:format", &format)) {
        if (format == "latlong") {
          out->params.dome.texture_format =
              RenderLight::DomeTextureFormat::Latlong;
        } else if (format == "mirroredBall") {
          out->params.dome.texture_format =
              RenderLight::DomeTextureFormat::MirroredBall;
        } else if (format == "angular") {
          out->params.dome.texture_format =
              RenderLight::DomeTextureFormat::Angular;
        } else {
          out->params.dome.texture_format =
              RenderLight::DomeTextureFormat::Automatic;
        }
      }
      break;
    }
    default:
      break;
  }

  // Shadow settings (UsdLux authors `inputs:shadow:enable`; accept the
  // legacy `inputs:enableShadows` spelling too).
  if (!GetBool(prim, "inputs:shadow:enable", &out->enable_shadow)) {
    GetBool(prim, "inputs:enableShadows", &out->enable_shadow);
  }
  GetFloat3(prim, "inputs:shadow:color", &out->shadow_color.x,
            &out->shadow_color.y, &out->shadow_color.z);
  GetFloat(prim, "inputs:shadow:distance", &out->shadow_distance);
  GetFloat(prim, "inputs:shadow:falloff", &out->shadow_falloff);
  GetFloat(prim, "inputs:shadow:falloffGamma", &out->shadow_falloff_gamma);

  return true;
}
}}}  // namespace lightusd::tydra::next
