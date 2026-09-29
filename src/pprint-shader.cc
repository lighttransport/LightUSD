// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Shader/Material/NodeGraph to_string (extracted from pprinter.cc).
//
#include "pprinter.hh"       // for all declarations
#include "pprint-detail.hh"  // for templates
#include "usdMtlx.hh"       // for MtlxOpenPBRSurface, OpenPBRSurface
#include "str-util.hh"       // for pquote

#include "common-macros.inc"

namespace lightusd {

//
// Domain-specific enum converters
//

std::string to_string(const UsdPreviewSurface::OpacityMode v) {
  std::string s;

  switch (v) {
    case UsdPreviewSurface::OpacityMode::Opacity: {
      s = "opacity";
      break;
    }
    case UsdPreviewSurface::OpacityMode::Transparent: {
      s = "transparent";
      break;
    }
    case UsdPreviewSurface::OpacityMode::Presence: {
      s = "presence";
      break;
    }
  }

  return s;
}

std::string to_string(const UsdUVTexture::SourceColorSpace v) {
  std::string s;

  switch (v) {
    case UsdUVTexture::SourceColorSpace::Auto: {
      s = "auto";
      break;
    }
    case UsdUVTexture::SourceColorSpace::Raw: {
      s = "raw";
      break;
    }
    case UsdUVTexture::SourceColorSpace::SRGB: {
      s = "sRGB";
      break;
    }
  }

  return s;
}

std::string to_string(const UsdUVTexture::Wrap v) {
  std::string s;

  switch (v) {
    case UsdUVTexture::Wrap::UseMetadata: {
      s = "useMetadata";
      break;
    }
    case UsdUVTexture::Wrap::Black: {
      s = "black";
      break;
    }
    case UsdUVTexture::Wrap::Clamp: {
      s = "clamp";
      break;
    }
    case UsdUVTexture::Wrap::Repeat: {
      s = "repeat";
      break;
    }
    case UsdUVTexture::Wrap::Mirror: {
      s = "mirror";
      break;
    }
  }

  return s;
}

//
// Static helper functions
//

static std::string print_common_shader_params(const ShaderNode &shader,
                                              const uint32_t indent) {
  std::stringstream ss;

  ss << print_props(shader.props, indent);

  return ss.str();
}

template<typename PrimvarReaderT>
static std::string print_shader_params(const PrimvarReaderT &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  ss << print_str_attr(shader.varname, "inputs:varname", indent);
  ss << print_typed_attr(shader.fallback, "inputs:fallback", indent);
  ss << print_typed_terminal_attr(shader.result, "outputs:result", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(const UsdTransform2d &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  ss << print_typed_attr(shader.in, "inputs:in", indent);
  ss << print_typed_attr(shader.rotation, "inputs:rotation", indent);
  ss << print_typed_attr(shader.scale, "inputs:scale", indent);
  ss << print_typed_attr(shader.translation, "inputs:translation", indent);
  ss << print_typed_terminal_attr(shader.result, "outputs:result", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(const UsdPreviewSurface &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  ss << print_typed_attr(shader.diffuseColor, "inputs:diffuseColor", indent);
  ss << print_typed_attr(shader.emissiveColor, "inputs:emissiveColor", indent);
  ss << print_typed_attr(shader.useSpecularWorkflow,
                         "inputs:useSpecularWorkflow", indent);
  ss << print_typed_attr(shader.ior, "inputs:ior", indent);
  ss << print_typed_attr(shader.specularColor, "inputs:specularColor", indent);
  ss << print_typed_attr(shader.metallic, "inputs:metallic", indent);
  ss << print_typed_attr(shader.clearcoat, "inputs:clearcoat", indent);
  ss << print_typed_attr(shader.clearcoatRoughness, "inputs:clearcoatRoughness",
                         indent);
  ss << print_typed_attr(shader.roughness, "inputs:roughness", indent);
  ss << print_typed_attr(shader.opacity, "inputs:opacity", indent);
  ss << print_typed_token_attr(shader.opacityMode, "inputs:opacityMode",
                         indent);
  ss << print_typed_attr(shader.opacityThreshold, "inputs:opacityThreshold",
                         indent);
  ss << print_typed_attr(shader.normal, "inputs:normal", indent);
  ss << print_typed_attr(shader.displacement, "inputs:displacement", indent);
  ss << print_typed_attr(shader.occlusion, "inputs:occlusion", indent);

  ss << print_typed_terminal_attr(shader.outputsSurface, "outputs:surface",
                                  indent);
  ss << print_typed_terminal_attr(shader.outputsDisplacement,
                                  "outputs:displacement", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(const UsdUVTexture &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  ss << print_typed_attr(shader.file, "inputs:file", indent);

  ss << print_typed_token_attr(shader.sourceColorSpace,
                               "inputs:sourceColorSpace", indent);

  ss << print_typed_attr(shader.fallback, "inputs:fallback", indent);

  ss << print_typed_attr(shader.bias, "inputs:bias", indent);
  ss << print_typed_attr(shader.scale, "inputs:scale", indent);

  ss << print_typed_attr(shader.st, "inputs:st", indent);
  ss << print_typed_token_attr(shader.wrapS, "inputs:wrapS", indent);
  ss << print_typed_token_attr(shader.wrapT, "inputs:wrapT", indent);

  // lightusd extensions: UV set selection (index / name).
  ss << print_typed_attr(shader.uv_set, "inputs:uv_set", indent);
  ss << print_typed_attr(shader.uv_set_name, "inputs:uv_set_name", indent);

  ss << print_typed_terminal_attr(shader.outputsR, "outputs:r", indent);
  ss << print_typed_terminal_attr(shader.outputsG, "outputs:g", indent);
  ss << print_typed_terminal_attr(shader.outputsB, "outputs:b", indent);
  ss << print_typed_terminal_attr(shader.outputsA, "outputs:a", indent);
  ss << print_typed_terminal_attr(shader.outputsRGB, "outputs:rgb", indent);
  ss << print_typed_terminal_attr(shader.outputsRGBA, "outputs:rgba", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(const MtlxOpenPBRSurface &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  // Base properties
  ss << print_typed_attr(shader.base_weight, "inputs:base_weight", indent);
  ss << print_typed_attr(shader.base_color, "inputs:base_color", indent);
  ss << print_typed_attr(shader.base_metalness, "inputs:base_metalness", indent);
  ss << print_typed_attr(shader.base_diffuse_roughness, "inputs:base_diffuse_roughness", indent);

  // Specular properties
  ss << print_typed_attr(shader.specular_weight, "inputs:specular_weight", indent);
  ss << print_typed_attr(shader.specular_color, "inputs:specular_color", indent);
  ss << print_typed_attr(shader.specular_roughness, "inputs:specular_roughness", indent);
  ss << print_typed_attr(shader.specular_ior, "inputs:specular_ior", indent);
  ss << print_typed_attr(shader.specular_anisotropy, "inputs:specular_anisotropy", indent);
  ss << print_typed_attr(shader.specular_rotation, "inputs:specular_rotation", indent);
  ss << print_typed_attr(shader.specular_roughness_anisotropy, "inputs:specular_roughness_anisotropy", indent);

  // Transmission properties
  ss << print_typed_attr(shader.transmission_weight, "inputs:transmission_weight", indent);
  ss << print_typed_attr(shader.transmission_color, "inputs:transmission_color", indent);
  ss << print_typed_attr(shader.transmission_depth, "inputs:transmission_depth", indent);
  ss << print_typed_attr(shader.transmission_scatter, "inputs:transmission_scatter", indent);
  ss << print_typed_attr(shader.transmission_scatter_anisotropy, "inputs:transmission_scatter_anisotropy", indent);
  ss << print_typed_attr(shader.transmission_dispersion, "inputs:transmission_dispersion", indent);
  ss << print_typed_attr(shader.transmission_dispersion_abbe_number, "inputs:transmission_dispersion_abbe_number", indent);
  ss << print_typed_attr(shader.transmission_dispersion_scale, "inputs:transmission_dispersion_scale", indent);

  // Subsurface properties
  ss << print_typed_attr(shader.subsurface_weight, "inputs:subsurface_weight", indent);
  ss << print_typed_attr(shader.subsurface_color, "inputs:subsurface_color", indent);
  ss << print_typed_attr(shader.subsurface_radius, "inputs:subsurface_radius", indent);
  ss << print_typed_attr(shader.subsurface_radius_scale, "inputs:subsurface_radius_scale", indent);
  ss << print_typed_attr(shader.subsurface_scale, "inputs:subsurface_scale", indent);
  ss << print_typed_attr(shader.subsurface_anisotropy, "inputs:subsurface_anisotropy", indent);
  ss << print_typed_attr(shader.subsurface_scatter_anisotropy, "inputs:subsurface_scatter_anisotropy", indent);

  // Coat properties
  ss << print_typed_attr(shader.coat_weight, "inputs:coat_weight", indent);
  ss << print_typed_attr(shader.coat_color, "inputs:coat_color", indent);
  ss << print_typed_attr(shader.coat_roughness, "inputs:coat_roughness", indent);
  ss << print_typed_attr(shader.coat_anisotropy, "inputs:coat_anisotropy", indent);
  ss << print_typed_attr(shader.coat_rotation, "inputs:coat_rotation", indent);
  ss << print_typed_attr(shader.coat_roughness_anisotropy, "inputs:coat_roughness_anisotropy", indent);
  ss << print_typed_attr(shader.coat_ior, "inputs:coat_ior", indent);
  ss << print_typed_attr(shader.coat_darkening, "inputs:coat_darkening", indent);
  ss << print_typed_attr(shader.coat_affect_color, "inputs:coat_affect_color", indent);
  ss << print_typed_attr(shader.coat_affect_roughness, "inputs:coat_affect_roughness", indent);

  // Fuzz properties
  ss << print_typed_attr(shader.fuzz_weight, "inputs:fuzz_weight", indent);
  ss << print_typed_attr(shader.fuzz_color, "inputs:fuzz_color", indent);
  ss << print_typed_attr(shader.fuzz_roughness, "inputs:fuzz_roughness", indent);

  // Thin film properties
  ss << print_typed_attr(shader.thin_film_thickness, "inputs:thin_film_thickness", indent);
  ss << print_typed_attr(shader.thin_film_ior, "inputs:thin_film_ior", indent);
  ss << print_typed_attr(shader.thin_film_weight, "inputs:thin_film_weight", indent);

  // Emission properties
  ss << print_typed_attr(shader.emission_luminance, "inputs:emission_luminance", indent);
  ss << print_typed_attr(shader.emission_color, "inputs:emission_color", indent);

  // Geometry properties
  ss << print_typed_attr(shader.geometry_opacity, "inputs:geometry_opacity", indent);
  ss << print_typed_attr(shader.geometry_thin_walled, "inputs:geometry_thin_walled", indent);
  ss << print_typed_attr(shader.geometry_normal, "inputs:geometry_normal", indent);
  ss << print_typed_attr(shader.geometry_tangent, "inputs:geometry_tangent", indent);
  ss << print_typed_attr(shader.geometry_coat_normal, "inputs:geometry_coat_normal", indent);
  ss << print_typed_attr(shader.geometry_coat_tangent, "inputs:geometry_coat_tangent", indent);

  // Output
  ss << print_typed_terminal_attr(shader.surface, "outputs:surface", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(
    const MtlxAutodeskStandardSurface &shader, const uint32_t indent) {
  std::stringstream ss;

  ss << print_typed_attr(shader.base, "inputs:base", indent);
  ss << print_typed_attr(shader.base_color, "inputs:base_color", indent);
  ss << print_typed_attr(shader.diffuse_roughness, "inputs:diffuse_roughness",
                         indent);
  ss << print_typed_attr(shader.metalness, "inputs:metalness", indent);

  ss << print_typed_attr(shader.specular, "inputs:specular", indent);
  ss << print_typed_attr(shader.specular_color, "inputs:specular_color",
                         indent);
  ss << print_typed_attr(shader.specular_roughness,
                         "inputs:specular_roughness", indent);
  ss << print_typed_attr(shader.specular_IOR, "inputs:specular_IOR", indent);
  ss << print_typed_attr(shader.specular_anisotropy,
                         "inputs:specular_anisotropy", indent);
  ss << print_typed_attr(shader.specular_rotation, "inputs:specular_rotation",
                         indent);

  ss << print_typed_attr(shader.transmission, "inputs:transmission", indent);
  ss << print_typed_attr(shader.transmission_color,
                         "inputs:transmission_color", indent);
  ss << print_typed_attr(shader.transmission_depth,
                         "inputs:transmission_depth", indent);
  ss << print_typed_attr(shader.transmission_scatter,
                         "inputs:transmission_scatter", indent);
  ss << print_typed_attr(shader.transmission_scatter_anisotropy,
                         "inputs:transmission_scatter_anisotropy", indent);
  ss << print_typed_attr(shader.transmission_dispersion,
                         "inputs:transmission_dispersion", indent);
  ss << print_typed_attr(shader.transmission_extra_roughness,
                         "inputs:transmission_extra_roughness", indent);

  ss << print_typed_attr(shader.subsurface, "inputs:subsurface", indent);
  ss << print_typed_attr(shader.subsurface_color, "inputs:subsurface_color",
                         indent);
  ss << print_typed_attr(shader.subsurface_radius,
                         "inputs:subsurface_radius", indent);
  ss << print_typed_attr(shader.subsurface_scale, "inputs:subsurface_scale",
                         indent);
  ss << print_typed_attr(shader.subsurface_anisotropy,
                         "inputs:subsurface_anisotropy", indent);

  ss << print_typed_attr(shader.sheen, "inputs:sheen", indent);
  ss << print_typed_attr(shader.sheen_color, "inputs:sheen_color", indent);
  ss << print_typed_attr(shader.sheen_roughness, "inputs:sheen_roughness",
                         indent);

  ss << print_typed_attr(shader.coat, "inputs:coat", indent);
  ss << print_typed_attr(shader.coat_color, "inputs:coat_color", indent);
  ss << print_typed_attr(shader.coat_roughness, "inputs:coat_roughness",
                         indent);
  ss << print_typed_attr(shader.coat_anisotropy, "inputs:coat_anisotropy",
                         indent);
  ss << print_typed_attr(shader.coat_rotation, "inputs:coat_rotation",
                         indent);
  ss << print_typed_attr(shader.coat_IOR, "inputs:coat_IOR", indent);
  ss << print_typed_attr(shader.coat_affect_color,
                         "inputs:coat_affect_color", indent);
  ss << print_typed_attr(shader.coat_affect_roughness,
                         "inputs:coat_affect_roughness", indent);

  ss << print_typed_attr(shader.thin_film_thickness,
                         "inputs:thin_film_thickness", indent);
  ss << print_typed_attr(shader.thin_film_IOR, "inputs:thin_film_IOR",
                         indent);

  ss << print_typed_attr(shader.emission, "inputs:emission", indent);
  ss << print_typed_attr(shader.emission_color, "inputs:emission_color",
                         indent);
  ss << print_typed_attr(shader.opacity, "inputs:opacity", indent);
  ss << print_typed_attr(shader.thin_walled, "inputs:thin_walled", indent);
  ss << print_typed_attr(shader.normal, "inputs:normal", indent);
  ss << print_typed_attr(shader.tangent, "inputs:tangent", indent);
  ss << print_typed_attr(shader.coat_normal, "inputs:coat_normal", indent);
  ss << print_typed_attr(shader.displacement, "inputs:displacement", indent);
  ss << print_typed_terminal_attr(shader.out, "outputs:out", indent);
  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

static std::string print_shader_params(const OpenPBRSurface &shader,
                                       const uint32_t indent) {
  std::stringstream ss;

  // Base properties
  ss << print_typed_attr(shader.base_weight, "inputs:base_weight", indent);
  ss << print_typed_attr(shader.base_color, "inputs:base_color", indent);
  ss << print_typed_attr(shader.base_roughness, "inputs:base_roughness", indent);
  ss << print_typed_attr(shader.base_metalness, "inputs:base_metalness", indent);
  ss << print_typed_attr(shader.base_diffuse_roughness, "inputs:base_diffuse_roughness", indent);

  // Specular properties
  ss << print_typed_attr(shader.specular_weight, "inputs:specular_weight", indent);
  ss << print_typed_attr(shader.specular_color, "inputs:specular_color", indent);
  ss << print_typed_attr(shader.specular_roughness, "inputs:specular_roughness", indent);
  ss << print_typed_attr(shader.specular_ior, "inputs:specular_ior", indent);
  ss << print_typed_attr(shader.specular_ior_level, "inputs:specular_ior_level", indent);
  ss << print_typed_attr(shader.specular_anisotropy, "inputs:specular_anisotropy", indent);
  ss << print_typed_attr(shader.specular_rotation, "inputs:specular_rotation", indent);

  // Transmission properties
  ss << print_typed_attr(shader.transmission_weight, "inputs:transmission_weight", indent);
  ss << print_typed_attr(shader.transmission_color, "inputs:transmission_color", indent);
  ss << print_typed_attr(shader.transmission_depth, "inputs:transmission_depth", indent);
  ss << print_typed_attr(shader.transmission_scatter, "inputs:transmission_scatter", indent);
  ss << print_typed_attr(shader.transmission_scatter_anisotropy, "inputs:transmission_scatter_anisotropy", indent);
  ss << print_typed_attr(shader.transmission_dispersion, "inputs:transmission_dispersion", indent);

  // Subsurface properties
  ss << print_typed_attr(shader.subsurface_weight, "inputs:subsurface_weight", indent);
  ss << print_typed_attr(shader.subsurface_color, "inputs:subsurface_color", indent);
  ss << print_typed_attr(shader.subsurface_radius, "inputs:subsurface_radius", indent);
  ss << print_typed_attr(shader.subsurface_radius_scale, "inputs:subsurface_radius_scale", indent);
  ss << print_typed_attr(shader.subsurface_scale, "inputs:subsurface_scale", indent);
  ss << print_typed_attr(shader.subsurface_anisotropy, "inputs:subsurface_anisotropy", indent);

  // Sheen properties
  ss << print_typed_attr(shader.sheen_weight, "inputs:sheen_weight", indent);
  ss << print_typed_attr(shader.sheen_color, "inputs:sheen_color", indent);
  ss << print_typed_attr(shader.sheen_roughness, "inputs:sheen_roughness", indent);

  // Fuzz properties
  ss << print_typed_attr(shader.fuzz_weight, "inputs:fuzz_weight", indent);
  ss << print_typed_attr(shader.fuzz_color, "inputs:fuzz_color", indent);
  ss << print_typed_attr(shader.fuzz_roughness, "inputs:fuzz_roughness", indent);

  // Thin film properties
  ss << print_typed_attr(shader.thin_film_weight, "inputs:thin_film_weight", indent);
  ss << print_typed_attr(shader.thin_film_thickness, "inputs:thin_film_thickness", indent);
  ss << print_typed_attr(shader.thin_film_ior, "inputs:thin_film_ior", indent);

  // Coat properties
  ss << print_typed_attr(shader.coat_weight, "inputs:coat_weight", indent);
  ss << print_typed_attr(shader.coat_color, "inputs:coat_color", indent);
  ss << print_typed_attr(shader.coat_roughness, "inputs:coat_roughness", indent);
  ss << print_typed_attr(shader.coat_anisotropy, "inputs:coat_anisotropy", indent);
  ss << print_typed_attr(shader.coat_rotation, "inputs:coat_rotation", indent);
  ss << print_typed_attr(shader.coat_ior, "inputs:coat_ior", indent);
  ss << print_typed_attr(shader.coat_affect_color, "inputs:coat_affect_color", indent);
  ss << print_typed_attr(shader.coat_affect_roughness, "inputs:coat_affect_roughness", indent);

  // Emission properties
  ss << print_typed_attr(shader.emission_luminance, "inputs:emission_luminance", indent);
  ss << print_typed_attr(shader.emission_color, "inputs:emission_color", indent);

  // Geometry properties
  ss << print_typed_attr(shader.opacity, "inputs:opacity", indent);
  ss << print_typed_attr(shader.normal, "inputs:normal", indent);
  ss << print_typed_attr(shader.tangent, "inputs:tangent", indent);

  // Output
  ss << print_typed_terminal_attr(shader.surface, "outputs:surface", indent);

  ss << print_common_shader_params(shader, indent);

  return ss.str();
}

//
// Prim to_string functions
//

std::string to_string(const Material &material, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(material.spec) << " Material \""
     << material.name << "\"\n";
  if (material.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(material.meta, indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // Imageable: `purpose` is a typed field, so print it from there or it is
  // silently dropped on write.
  ss << print_typed_token_attr(material.purpose, "purpose", indent + 1);

  if (material.surface.authored()) {
    // assume connection when authored.
    // TODO: list edit?.
    ss << pprint::Indent(indent + 1) << "token outputs:surface.connect ";

    const auto &conns = material.surface.get_connections();
    if (conns.size() == 1) {
      ss << "= " << pquote(conns[0].full_path_name());
    } else if (conns.size() > 1) {
      ss << "= [";
      for (size_t i = 0; i < conns.size(); i++) {
        ss << pquote(conns[i].full_path_name());
        if (i != (conns.size() - 1)) {
          ss << ", ";
        }
      }
      ss << "]";
    }

    if (material.surface.metas().authored()) {
      ss << "(\n"
         << print_attr_metas(material.surface.metas(), indent + 2)
         << pprint::Indent(indent + 1) << ")";
    }
    ss << "\n";
  }

  if (material.mtlxSurface.authored()) {
    ss << pprint::Indent(indent + 1)
       << "token outputs:mtlx:surface.connect ";

    const auto &conns = material.mtlxSurface.get_connections();
    if (conns.size() == 1) {
      ss << "= " << pquote(conns[0].full_path_name());
    } else if (conns.size() > 1) {
      ss << "= [";
      for (size_t i = 0; i < conns.size(); i++) {
        ss << pquote(conns[i].full_path_name());
        if (i != (conns.size() - 1)) {
          ss << ", ";
        }
      }
      ss << "]";
    }

    if (material.mtlxSurface.metas().authored()) {
      ss << "(\n"
         << print_attr_metas(material.mtlxSurface.metas(), indent + 2)
         << pprint::Indent(indent + 1) << ")";
    }
    ss << "\n";
  }

  if (material.displacement.authored()) {
    // assume connection when authored.
    // TODO: list edit?.
    ss << pprint::Indent(indent + 1) << "token outputs:displacement.connect ";

    const auto &conns = material.displacement.get_connections();
    if (conns.size() == 1) {
      ss << "= " << pquote(conns[0].full_path_name());
    } else if (conns.size() > 1) {
      ss << "= [";
      for (size_t i = 0; i < conns.size(); i++) {
        ss << pquote(conns[i].full_path_name());
        if (i != (conns.size() - 1)) {
          ss << ", ";
        }
      }
      ss << "]";
    }

    if (material.displacement.metas().authored()) {
      ss << "(\n"
         << print_attr_metas(material.displacement.metas(), indent + 2)
         << pprint::Indent(indent + 1) << ")";
    }
    ss << "\n";
  }

  if (material.volume.authored()) {
    // assume connection when authored.
    // TODO: list edit?.
    ss << pprint::Indent(indent + 1) << "token outputs:volume.connect ";

    const auto &conns = material.volume.get_connections();
    if (conns.size() == 1) {
      ss << "= " << pquote(conns[0].full_path_name());
    } else if (conns.size() > 1) {
      ss << "= [";
      for (size_t i = 0; i < conns.size(); i++) {
        ss << pquote(conns[i].full_path_name());
        if (i != (conns.size() - 1)) {
          ss << ", ";
        }
      }
      ss << "]";
    }

    if (material.volume.metas().authored()) {
      ss << "(\n"
         << print_attr_metas(material.volume.metas(), indent + 2)
         << pprint::Indent(indent + 1) << ")";
    }
    ss << "\n";
  }

  // Print MaterialXConfigAPI attributes if present
  if (material.materialXConfig) {
    const auto &mtlxConfig = material.materialXConfig.value();
    if (mtlxConfig.mtlx_version.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:mtlx:version = "
         << quote(mtlxConfig.mtlx_version.get_value()) << "\n";
    }
    if (mtlxConfig.mtlx_namespace.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:mtlx:namespace = "
         << quote(mtlxConfig.mtlx_namespace.get_value()) << "\n";
    }
    if (mtlxConfig.mtlx_colorspace.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:mtlx:colorspace = "
         << quote(mtlxConfig.mtlx_colorspace.get_value()) << "\n";
    }
    if (mtlxConfig.mtlx_sourceUri.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:mtlx:sourceUri = "
         << quote(mtlxConfig.mtlx_sourceUri.get_value()) << "\n";
    }
  }

  // LightUSD Unreal material preservation layer.
  if (material.materialUEConfig) {
    const auto &ue = material.materialUEConfig.value();
    if (ue.version.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:version = "
         << buildEscapedAndQuotedStringForUSDA(ue.version.get_value()) << "\n";
    }
    if (ue.engineVersion.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:engineVersion = "
         << buildEscapedAndQuotedStringForUSDA(ue.engineVersion.get_value()) << "\n";
    }
    if (ue.sourceAsset.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:sourceAsset = "
         << buildEscapedAndQuotedStringForUSDA(ue.sourceAsset.get_value()) << "\n";
    }
    if (ue.graphAuthority.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:graphAuthority = "
         << buildEscapedAndQuotedStringForUSDA(ue.graphAuthority.get_value()) << "\n";
    }
    if (ue.materialDomain.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:materialDomain = "
         << buildEscapedAndQuotedStringForUSDA(ue.materialDomain.get_value()) << "\n";
    }
    if (ue.blendMode.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:blendMode = "
         << buildEscapedAndQuotedStringForUSDA(ue.blendMode.get_value()) << "\n";
    }
    if (ue.shadingModels.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:shadingModels = "
         << buildEscapedAndQuotedStringForUSDA(ue.shadingModels.get_value()) << "\n";
    }
    if (ue.twoSided.authored()) {
      ss << pprint::Indent(indent + 1) << "bool config:unreal:twoSided = "
         << (ue.twoSided.get_value() ? "true" : "false") << "\n";
    }
    if (ue.opacityMaskClipValue.authored()) {
      ss << pprint::Indent(indent + 1) << "float config:unreal:opacityMaskClipValue = "
         << ue.opacityMaskClipValue.get_value() << "\n";
    }
    if (ue.propertyArchive.authored()) {
      ss << pprint::Indent(indent + 1) << "string config:unreal:propertyArchive = "
         << buildEscapedAndQuotedStringForUSDA(ue.propertyArchive.get_value()) << "\n";
    }
  }

  ss << print_props(material.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const Shader &shader, const uint32_t indent,
                      bool closing_brace) {
  // generic Shader class
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(shader.spec) << " Shader \""
     << shader.name << "\"\n";
  if (shader.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(shader.metas(), indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  // members
  if (shader.info_id.size()) {
    ss << pprint::Indent(indent + 1) << "uniform token info:id = \""
       << shader.info_id << "\"\n";
  }

  if (auto pvr = shader.value.as<UsdPrimvarReader_float>()) {
    ss << print_shader_params(*pvr, indent + 1);
  } else if (auto pvr2 = shader.value.as<UsdPrimvarReader_float2>()) {
    ss << print_shader_params(*pvr2, indent + 1);
  } else if (auto pvr3 = shader.value.as<UsdPrimvarReader_float3>()) {
    ss << print_shader_params(*pvr3, indent + 1);
  } else if (auto pvr4 = shader.value.as<UsdPrimvarReader_float4>()) {
    ss << print_shader_params(*pvr4, indent + 1);
  } else if (auto pvrs = shader.value.as<UsdPrimvarReader_string>()) {
    ss << print_shader_params(*pvrs, indent + 1);
  } else if (auto pvrn = shader.value.as<UsdPrimvarReader_normal>()) {
    ss << print_shader_params(*pvrn, indent + 1);
  } else if (auto pvrv = shader.value.as<UsdPrimvarReader_vector>()) {
    ss << print_shader_params(*pvrv, indent + 1);
  } else if (auto pvrp = shader.value.as<UsdPrimvarReader_point>()) {
    ss << print_shader_params(*pvrp, indent + 1);
  } else if (auto pvrm = shader.value.as<UsdPrimvarReader_matrix>()) {
    ss << print_shader_params(*pvrm, indent + 1);
  } else if (auto pvtex = shader.value.as<UsdUVTexture>()) {
    ss << print_shader_params(*pvtex, indent + 1);
  } else if (auto pvtx2d = shader.value.as<UsdTransform2d>()) {
    ss << print_shader_params(*pvtx2d, indent + 1);
  } else if (auto pvs = shader.value.as<UsdPreviewSurface>()) {
    ss << print_shader_params(*pvs, indent + 1);
  } else if (auto mtlx_opbr = shader.value.as<MtlxOpenPBRSurface>()) {
    // Blender v4.5 MaterialX OpenPBR Surface
    ss << print_shader_params(*mtlx_opbr, indent + 1);
  } else if (auto mtlx_standard =
                 shader.value.as<MtlxAutodeskStandardSurface>()) {
    ss << print_shader_params(*mtlx_standard, indent + 1);
  } else if (auto opbr = shader.value.as<OpenPBRSurface>()) {
    // Native OpenPBR Surface shader
    ss << print_shader_params(*opbr, indent + 1);
  } else if (auto pvsn = shader.value.as<ShaderNode>()) {
    // Generic ShaderNode
    ss << print_common_shader_params(*pvsn, indent + 1);
  } else {
    ss << pprint::Indent(indent + 1)
       << "[???] Invalid ShaderNode in Shader Prim\n";
  }

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const NodeGraph &nodegraph, const uint32_t indent,
                      bool closing_brace) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << to_string(nodegraph.spec) << " NodeGraph \""
     << nodegraph.name << "\"\n";
  if (nodegraph.meta.authored()) {
    ss << pprint::Indent(indent) << "(\n";
    ss << print_prim_metas(nodegraph.metas(), indent + 1);
    ss << pprint::Indent(indent) << ")\n";
  }
  ss << pprint::Indent(indent) << "{\n";

  ss << print_typed_token_attr(nodegraph.purpose, "purpose", indent + 1);

  // NodeGraph-specific attributes
  if (nodegraph.nodedef.authored()) {
    ss << print_typed_attr(nodegraph.nodedef, "nodedef", indent + 1);
  }

  if (nodegraph.nodegraph_type.authored()) {
    ss << print_typed_attr(nodegraph.nodegraph_type, "nodegraph_type", indent + 1);
  }

  // Print properties (inputs, outputs, etc.)
  ss << print_props(nodegraph.props, indent + 1);

  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const UsdPreviewSurface &surf, const uint32_t indent,
                      bool closing_brace) {
  // TODO: Print spec and meta?
  std::stringstream ss;

  ss << pprint::Indent(indent) << "{\n";
  ss << print_shader_params(surf, indent);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const UsdUVTexture &tex, const uint32_t indent,
                      bool closing_brace) {
  // TODO: Print spec and meta?
  std::stringstream ss;

  ss << pprint::Indent(indent) << "{\n";
  ss << print_shader_params(tex, indent);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

std::string to_string(const UsdPrimvarReader_float2 &preader,
                      const uint32_t indent, bool closing_brace) {
  // TODO: Print spec and meta?
  std::stringstream ss;

  ss << pprint::Indent(indent) << "{\n";
  ss << print_shader_params(preader, indent);
  if (closing_brace) {
    ss << pprint::Indent(indent) << "}\n";
  }

  return ss.str();
}

}  // namespace lightusd
