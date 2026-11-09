// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Stage-converter: Light prim property extraction.
//

#include "sconv-detail.hh"
#include "usdLux.hh"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

namespace lightusd {
namespace experimental {

// ============================================================================
// UsdLux Light Property Extraction
// ============================================================================

// Macros to reduce boilerplate for common light property extraction.
// These use the same `light`, `fields`, `err` names from the enclosing scope.
#define EXTRACT_SHADOW_API(light) \
  if (!EmitTypedAnimatableAttr("inputs:shadow:enable", (light)->shadowEnable, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shadow:color", (light)->shadowColor, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shadow:distance", (light)->shadowDistance, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shadow:falloff", (light)->shadowFalloff, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shadow:falloffGamma", (light)->shadowFalloffGamma, fields, err)) return false;

#define EXTRACT_SHAPING_API(light) \
  if (!EmitTypedAnimatableAttr("inputs:shaping:focus", (light)->shapingFocus, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shaping:focusTint", (light)->shapingFocusTint, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shaping:cone:angle", (light)->shapingConeAngle, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:shaping:cone:softness", (light)->shapingConeSoftness, fields, err)) return false;

// Lights are imageable AND Xformable: `visibility` / `purpose` / xformOps are all
// typed fields, so they have to be written explicitly or they vanish on write --
// a light whose xformOps are dropped comes back at the world origin.
//
// intensity / color / exposure live on the LightAPI base, so they belong here
// too. They used to be copy-pasted into each light extractor, and half of them
// forgot `exposure`.
#define EXTRACT_COMMON_LIGHT(light) \
  if (!ExtractImageableAttrs((light)->visibility, (light)->purpose, fields, err)) return false; \
  if (!ExtractXformOpsFromXformable(prim, prim_path, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:intensity", (light)->intensity, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:color", (light)->color, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:exposure", (light)->exposure, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:specular", (light)->specular, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:diffuse", (light)->diffuse, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:normalize", (light)->normalize, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:enableColorTemperature", (light)->enableColorTemperature, fields, err)) return false; \
  if (!EmitTypedAnimatableAttr("inputs:colorTemperature", (light)->colorTemperature, fields, err)) return false;

bool CrateWriter::ExtractSphereLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const SphereLight* light = prim.data().as<SphereLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to SphereLight";
    return false;
  }

  if (!EmitTypedAnimatableAttr("inputs:radius", light->radius, fields, err)) return false;

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)
  EXTRACT_SHAPING_API(light)

  return true;
}

bool CrateWriter::ExtractRectLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const RectLight* light = prim.data().as<RectLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to RectLight";
    return false;
  }

  if (!EmitTypedAnimatableAttr("inputs:width", light->width, fields, err)) return false;
  if (!EmitTypedAnimatableAttr("inputs:height", light->height, fields, err)) return false;

  // RectLight carries a texture like DomeLight does; it was never written.
  if (light->file.authored()) {
    const auto& file_opt = light->file.get_value();
    if (file_opt) {
      const Animatable<value::AssetPath>& file_anim = *file_opt;
      value::AssetPath file_val;
      if (file_anim.has_default() && file_anim.get_default(&file_val)) {
        crate::CrateValue crate_val;
        crate_val.Set(file_val);
        fields.push_back({"inputs:texture:file", crate_val});
      }
    }
  }

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)
  EXTRACT_SHAPING_API(light)

  return true;
}

bool CrateWriter::ExtractDiskLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const DiskLight* light = prim.data().as<DiskLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to DiskLight";
    return false;
  }

  if (!EmitTypedAnimatableAttr("inputs:radius", light->radius, fields, err)) return false;

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)
  EXTRACT_SHAPING_API(light)

  return true;
}

bool CrateWriter::ExtractCylinderLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const CylinderLight* light = prim.data().as<CylinderLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to CylinderLight";
    return false;
  }

  if (!EmitTypedAnimatableAttr("inputs:radius", light->radius, fields, err)) return false;
  if (!EmitTypedAnimatableAttr("inputs:length", light->length, fields, err)) return false;

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)
  EXTRACT_SHAPING_API(light)

  return true;
}

bool CrateWriter::ExtractDistantLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const DistantLight* light = prim.data().as<DistantLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to DistantLight";
    return false;
  }

  if (!EmitTypedAnimatableAttr("inputs:angle", light->angle, fields, err)) return false;

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)

  return true;
}

bool CrateWriter::ExtractDomeLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const DomeLight* light = prim.data().as<DomeLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to DomeLight";
    return false;
  }

  // Extract texture file path if present (special case: optional<Animatable>)
  if (light->file.authored()) {
    const auto& file_opt = light->file.get_value();
    if (file_opt) {
      const Animatable<value::AssetPath>& file_anim = *file_opt;
      if (file_anim.has_default()) {
        value::AssetPath file_val;
        if (file_anim.get_default(&file_val)) {
          crate::CrateValue crate_val;
          crate_val.Set(file_val);
          fields.push_back({"inputs:texture:file", crate_val});
        }
      }
    }
  }

  // `inputs:texture:format` is a typed enum field and was never written.
  if (light->textureFormat.authored()) {
    const auto& fmt_anim = light->textureFormat.get_value();
    DomeLight::TextureFormat fmt_enum;
    if (fmt_anim.has_default() && fmt_anim.get_default(&fmt_enum)) {
      crate::CrateValue fmt_val;
      fmt_val.Set(value::token(to_string(fmt_enum)));
      fields.push_back({"inputs:texture:format", fmt_val});
    }
  }


  // guideRadius is parsed (DOME_LIGHT_TYPED_ATTRS) and printed, so emit it too
  // for USDC round-trip parity. Note: no "inputs:" prefix (matches the reader).
  if (!EmitTypedAnimatableAttr("guideRadius", light->guideRadius, fields, err)) return false;

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)

  return true;
}

bool CrateWriter::ExtractGeometryLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const GeometryLight* light = prim.data().as<GeometryLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to GeometryLight";
    return false;
  }


  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)

  return true;
}

bool CrateWriter::ExtractPortalLightProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const PortalLight* light = prim.data().as<PortalLight>();
  if (!light) {
    if (err) *err = "Failed to cast prim to PortalLight";
    return false;
  }


  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)

  return true;
}

// ============================================================================
// DomeLight_1 Property Extraction
// ============================================================================

bool CrateWriter::ExtractDomeLight1Properties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const DomeLight_1* light = prim.data().as<DomeLight_1>();
  if (!light) {
    if (err) *err = "Failed to cast prim to DomeLight_1";
    return false;
  }

  // Extract texture file path if present (special case: optional<Animatable>)
  if (light->file.authored()) {
    const auto& file_opt = light->file.get_value();
    if (file_opt) {
      const Animatable<value::AssetPath>& file_anim = *file_opt;
      if (file_anim.has_default()) {
        value::AssetPath file_val;
        if (file_anim.get_default(&file_val)) {
          crate::CrateValue crate_val;
          crate_val.Set(file_val);
          fields.push_back({"inputs:texture:file", crate_val});
        }
      }
    }
  }

  // `inputs:texture:format` is a typed enum field and was never written.
  if (light->textureFormat.authored()) {
    const auto& fmt_anim = light->textureFormat.get_value();
    DomeLight_1::TextureFormat fmt_enum;
    if (fmt_anim.has_default() && fmt_anim.get_default(&fmt_enum)) {
      crate::CrateValue fmt_val;
      fmt_val.Set(value::token(to_string(fmt_enum)));
      fields.push_back({"inputs:texture:format", fmt_val});
    }
  }



  if (!EmitTypedAnimatableAttr("guideRadius", light->guideRadius, fields, err)) return false;

  // Extract poleAxis (uniform token with fallback) - DomeLight_1 specific
  if (light->poleAxis.authored()) {
    const value::token& tok = light->poleAxis.get_value();
    crate::CrateValue crate_val;
    crate_val.Set(tok);
    fields.push_back({"poleAxis", crate_val});
    sconv_detail::EmitAttrVariability("poleAxis", light->poleAxis, fields);
  }

  EXTRACT_COMMON_LIGHT(light)
  EXTRACT_SHADOW_API(light)

  return true;
}

// ============================================================================
// LightFilter Property Extraction
// ============================================================================

bool CrateWriter::ExtractLightFilterProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const LightFilter* filter = prim.data().as<LightFilter>();
  if (!filter) {
    if (err) *err = "Failed to cast prim to LightFilter";
    return false;
  }

  // Extract visibility
  if (filter->visibility.authored()) {
    const auto& vis_animatable = filter->visibility.get_value();
    if (vis_animatable.has_default()) {
      Visibility vis_val;
      if (vis_animatable.get_default(&vis_val)) {
        if (vis_val != Visibility::Inherited) {
          crate::CrateValue vis_crate_val;
          value::token vis_tok(to_string(vis_val));
          vis_crate_val.Set(vis_tok);
          fields.push_back({"visibility", vis_crate_val});
        }
      }
    }
  }

  // Extract purpose
  if (filter->purpose.authored()) {
    Purpose purpose_val = filter->purpose.get_value();
    if (purpose_val != Purpose::Default) {
      crate::CrateValue purpose_crate_val;
      value::token purpose_tok(to_string(purpose_val));
      purpose_crate_val.Set(purpose_tok);
      fields.push_back({"purpose", purpose_crate_val});
      sconv_detail::EmitAttrVariability("purpose", filter->purpose, fields);
    }
  }

  return true;
}

// ============================================================================
// PluginLightFilter Property Extraction
// ============================================================================

bool CrateWriter::ExtractPluginLightFilterProperties(
    const Prim& prim,
    const Path& prim_path,
    crate::FieldValuePairVector& fields,
    std::string* err) {
  const PluginLightFilter* filter = prim.data().as<PluginLightFilter>();
  if (!filter) {
    if (err) *err = "Failed to cast prim to PluginLightFilter";
    return false;
  }

  // Extract visibility (inherited from LightFilter)
  if (filter->visibility.authored()) {
    const auto& vis_animatable = filter->visibility.get_value();
    if (vis_animatable.has_default()) {
      Visibility vis_val;
      if (vis_animatable.get_default(&vis_val)) {
        if (vis_val != Visibility::Inherited) {
          crate::CrateValue vis_crate_val;
          value::token vis_tok(to_string(vis_val));
          vis_crate_val.Set(vis_tok);
          fields.push_back({"visibility", vis_crate_val});
        }
      }
    }
  }

  // Extract purpose (inherited from LightFilter)
  if (filter->purpose.authored()) {
    Purpose purpose_val = filter->purpose.get_value();
    if (purpose_val != Purpose::Default) {
      crate::CrateValue purpose_crate_val;
      value::token purpose_tok(to_string(purpose_val));
      purpose_crate_val.Set(purpose_tok);
      fields.push_back({"purpose", purpose_crate_val});
      sconv_detail::EmitAttrVariability("purpose", filter->purpose, fields);
    }
  }

  // Extract light:shaderId (PluginLightFilter-specific; parsed but previously not
  // written). TypedAttribute<Animatable<token>>.
  if (filter->shaderId.authored()) {
    const auto& sid_opt = filter->shaderId.get_value();
    if (sid_opt && sid_opt.value().has_default()) {
      value::token sid;
      if (sid_opt.value().get_default(&sid)) {
        crate::CrateValue sid_crate_val;
        sid_crate_val.Set(sid);
        fields.push_back({"light:shaderId", sid_crate_val});
      }
    }
  }

  return true;
}

// ============================================================================
// Light Filter Relationship Specs (called AFTER Light prim spec is added)
// ============================================================================

bool CrateWriter::AddLightFilterSpecs(
  const Prim& prim,
  const Path& prim_path,
  std::string* err
) {
  // Extract and write light filter relationships from a Light prim
  // Light filters are relationships that need separate specs in Crate format

  // Get the appropriate light type and extract lightFilters relationship
  // Light filters are supported for lights that inherit from BoundableLight or NonboundableLight
  const SphereLight* sphere_light = prim.data().as<SphereLight>();
  const RectLight* rect_light = nullptr;
  const DiskLight* disk_light = nullptr;
  const CylinderLight* cylinder_light = nullptr;
  const DistantLight* distant_light = nullptr;
  const DomeLight* dome_light = nullptr;
  const GeometryLight* geometry_light = nullptr;
  const PortalLight* portal_light = nullptr;

  // Determine which light type the prim is
  const void* light_base = nullptr;

  if (sphere_light) {
    light_base = static_cast<const void*>(sphere_light);
  } else if ((rect_light = prim.data().as<RectLight>())) {
    light_base = static_cast<const void*>(rect_light);
  } else if ((disk_light = prim.data().as<DiskLight>())) {
    light_base = static_cast<const void*>(disk_light);
  } else if ((cylinder_light = prim.data().as<CylinderLight>())) {
    light_base = static_cast<const void*>(cylinder_light);
  } else if ((distant_light = prim.data().as<DistantLight>())) {
    light_base = static_cast<const void*>(distant_light);
  } else if ((dome_light = prim.data().as<DomeLight>())) {
    light_base = static_cast<const void*>(dome_light);
  } else if ((geometry_light = prim.data().as<GeometryLight>())) {
    light_base = static_cast<const void*>(geometry_light);
  } else if ((portal_light = prim.data().as<PortalLight>())) {
    light_base = static_cast<const void*>(portal_light);
  }

  if (!light_base) {
    // Prim is not a light type with filter support
    return true;
  }

  // Get the lightFilters relationship from the appropriate light type
  const RelationshipProperty *light_filters = nullptr;

  if (sphere_light) {
    light_filters = &sphere_light->lightFilters;
  } else if (rect_light) {
    light_filters = &rect_light->lightFilters;
  } else if (disk_light) {
    light_filters = &disk_light->lightFilters;
  } else if (cylinder_light) {
    light_filters = &cylinder_light->lightFilters;
  } else if (distant_light) {
    light_filters = &distant_light->lightFilters;
  } else if (dome_light) {
    light_filters = &dome_light->lightFilters;
  } else if (geometry_light) {
    light_filters = &geometry_light->lightFilters;
  } else if (portal_light) {
    light_filters = &portal_light->lightFilters;
  }

  // Add light:filters relationship if present
  if (light_filters && light_filters->authored()) {
    const Relationship& filters = light_filters->relationship();
    if (!ConvertRelationshipToFields("light:filters", filters, prim_path, err)) {
      if (err) *err = "Failed to add light:filters relationship: " + *err;
      return false;
    }
  }

  return true;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
