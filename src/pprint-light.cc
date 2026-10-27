// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Light prim to_string (extracted from pprinter.cc).
//
#include "pprinter.hh"       // for all declarations
#include "pprint-detail.hh"  // for templates

#include "common-macros.inc"

namespace lightusd {

template<typename LightT>
static std::string print_light_common_attrs(const LightT& light, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(light.color, "inputs:color", indent);
  ss << print_typed_attr(light.colorTemperature, "inputs:colorTemperature", indent);
  ss << print_typed_attr(light.diffuse, "inputs:diffuse", indent);
  ss << print_typed_attr(light.enableColorTemperature, "inputs:enableColorTemperature", indent);
  ss << print_typed_attr(light.exposure, "inputs:exposure", indent);
  ss << print_typed_attr(light.intensity, "inputs:intensity", indent);
  ss << print_typed_attr(light.normalize, "inputs:normalize", indent);
  ss << print_typed_attr(light.specular, "inputs:specular", indent);
  // light:filters relationship (parsed into lightFilters; emit it so USDA
  // round-trips and matches OpenUSD usdcat output).
  if (light.lightFilters.authored()) {
    ss << print_relationship(light.lightFilters.relationship(),
                             light.lightFilters.get_listedit_qual(),
                             /* custom */ false, "light:filters", indent);
  }
  // Collection instances (light:link / shadow:link, etc.) — lights inherit
  // Collection, so emit them for USDA round-trip / usdcat parity.
  ss << print_collection(&light, indent);
  return pp_ss.str();
}

template<typename LightT>
static std::string print_light_shadow_attrs(const LightT& light, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(light.shadowColor, "inputs:shadow:color", indent);
  ss << print_typed_attr(light.shadowDistance, "inputs:shadow:distance", indent);
  ss << print_typed_attr(light.shadowEnable, "inputs:shadow:enable", indent);
  ss << print_typed_attr(light.shadowFalloff, "inputs:shadow:falloff", indent);
  ss << print_typed_attr(light.shadowFalloffGamma, "inputs:shadow:falloffGamma", indent);
  return pp_ss.str();
}

template<typename LightT>
static std::string print_light_shaping_attrs(const LightT& light, uint32_t indent) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_attr(light.shapingConeAngle, "inputs:shaping:cone:angle", indent);
  ss << print_typed_attr(light.shapingConeSoftness, "inputs:shaping:cone:softness", indent);
  ss << print_typed_attr(light.shapingFocus, "inputs:shaping:focus", indent);
  ss << print_typed_attr(light.shapingFocusTint, "inputs:shaping:focusTint", indent);
  return pp_ss.str();
}

template<typename LightT>
static std::string print_light_footer(const LightT& light, uint32_t indent, bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_typed_token_attr(light.visibility, "visibility", indent + 1);
  ss << print_typed_token_attr(light.purpose, "purpose", indent + 1);
  ss << print_xformOps(light.xformOps, indent + 1);
  ss << print_props(light.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return pp_ss.str();
}

std::string to_string(const SphereLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "SphereLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.radius, "inputs:radius", indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_shaping_attrs(light, indent + 1);
  ss << print_typed_attr(light.extent, "extent", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const DistantLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "DistantLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.angle, "inputs:angle", indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const CylinderLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "CylinderLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.length, "inputs:length", indent + 1);
  ss << print_typed_attr(light.radius, "inputs:radius", indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_shaping_attrs(light, indent + 1);
  ss << print_typed_attr(light.extent, "extent", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const DiskLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "DiskLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.radius, "inputs:radius", indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_shaping_attrs(light, indent + 1);
  ss << print_typed_attr(light.extent, "extent", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const DomeLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "DomeLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.guideRadius, "guideRadius", indent + 1);
  ss << print_typed_attr(light.file, "inputs:texture:file", indent + 1);
  ss << print_typed_token_attr(light.textureFormat, "inputs:texture:format", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const RectLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "RectLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.file, "inputs:texture:file", indent + 1);
  ss << print_typed_attr(light.height, "inputs:height", indent + 1);
  ss << print_typed_attr(light.width, "inputs:width", indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_shaping_attrs(light, indent + 1);
  ss << print_typed_attr(light.extent, "extent", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const GeometryLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "GeometryLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const PortalLight &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "PortalLight", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_light_shadow_attrs(light, indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const DomeLight_1 &light, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(light, "DomeLight_1", indent);
  ss << print_light_common_attrs(light, indent + 1);
  ss << print_typed_attr(light.guideRadius, "guideRadius", indent + 1);
  ss << print_typed_attr(light.file, "inputs:texture:file", indent + 1);
  ss << print_typed_token_attr(light.textureFormat, "inputs:texture:format", indent + 1);
  ss << print_typed_attr(light.poleAxis, "poleAxis", indent + 1);
  ss << print_light_footer(light, indent, closing_brace);
  return pp_ss.str();
}

std::string to_string(const LightFilter &filter, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(filter, "LightFilter", indent);
  ss << print_typed_token_attr(filter.visibility, "visibility", indent + 1);
  ss << print_typed_token_attr(filter.purpose, "purpose", indent + 1);
  ss << print_props(filter.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return pp_ss.str();
}

std::string to_string(const PluginLightFilter &filter, const uint32_t indent,
                      bool closing_brace) {
  ::lightusd::pprint::ScratchRef pp_ss;
  std::ostream &ss = pp_ss.os();
  ss << print_prim_header(filter, "PluginLightFilter", indent);
  ss << print_typed_token_attr(filter.visibility, "visibility", indent + 1);
  ss << print_typed_token_attr(filter.purpose, "purpose", indent + 1);
  ss << print_typed_attr(filter.shaderId, "light:shaderId", indent + 1);
  ss << print_props(filter.props, indent + 1);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }
  return pp_ss.str();
}

}  // namespace lightusd
