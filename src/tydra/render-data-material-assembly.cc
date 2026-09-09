// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Material and texture conversion routines split from render-data.cc
//
#include <cctype>
#include <chrono>
#include <numeric>
#include <set>

#include "common-utils.hh"
#include "common-types.hh"
#include "color-management.hh"
#include "enum-handlers.hh"
#include "../tiny-hashmap.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#if defined(LIGHTUSD_WITH_TEXTOOLS)
// KTX2 reader for the keep-compressed texture path (RenderSceneConverterConfig::
// keep_compressed_textures). Pulls in texcomp.h too.
#include "texpipe.h"
#if defined(LIGHTUSD_WITH_ZSTD_COMPRESSION)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/zstd.h"  // ZSTD_decompress for supercompressionScheme 2
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#endif
#include "io-util.hh"
#include "linear-algebra.hh"
#include "pprinter.hh"
#include "math-util.inc"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "safe-arithmetic.hh"
#include "usdMtlx.hh"
#include "value-pprint.hh"
#include "logger.hh"
#include "materialx-to-json.hh"
#include "security-policy.hh"

//
#include "common-macros.inc"

//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"
#include "tydra/render-data-material-internal.hh"

namespace lightusd {
namespace tydra {

namespace {

static std::string MtlxTexcoordIndexName(const std::string &default_name,
                                         int index) {
  return index == 0 ? default_name : default_name + std::to_string(index);
}

static void ApplyMaterialVolumeConstants(const ShaderNode &shader,
                                         float *density_scale, float albedo[3],
                                         float emission_color[3],
                                         float *emission_scale) {
  auto attr = [&](const char *name) -> const Attribute * {
    const auto it = shader.props.find(name);
    return (it == shader.props.end() || !it->second.is_attribute())
               ? nullptr : it->second.get_attribute_or_null();
  };
  auto scalar = [&](const char *name, float *out) {
    const Attribute *a = attr(name);
    if (!a || a->has_timesamples()) return;
    if (auto value = a->get_value<float>()) *out = value.value();
    else if (auto double_value = a->get_value<double>()) *out = float(double_value.value());
  };
  auto color = [&](const char *name, float out[3]) {
    const Attribute *a = attr(name);
    if (!a || a->has_timesamples()) return;
    if (auto value = a->get_value<value::color3f>()) {
      out[0] = (*value)[0]; out[1] = (*value)[1]; out[2] = (*value)[2];
    } else if (auto float_value = a->get_value<value::float3>()) {
      out[0] = (*float_value)[0]; out[1] = (*float_value)[1]; out[2] = (*float_value)[2];
    }
  };
  scalar("inputs:density", density_scale);
  color("inputs:scattering_color", albedo); color("inputs:scatter_color", albedo);
  color("inputs:emission_color", emission_color); color("inputs:emissionColor", emission_color);
  scalar("inputs:emission", emission_scale); scalar("inputs:emission_intensity", emission_scale);
  scalar("inputs:emissionIntensity", emission_scale);
  *density_scale = std::max(0.0f, *density_scale);
  *emission_scale = std::max(0.0f, *emission_scale);
}

}  // namespace

static void ApplyTexTransform2d(float rotation, const value::float2 &scale,
                                const value::float2 &translation,
                                UVTexture *tex_out) {
  if (!tex_out) return;
  mat3 s; s.set_scale(scale[0], scale[1], 1.0f);
  mat3 r = mat3::identity();
  r.m[0][0] = std::cos(math::radian(rotation));
  r.m[0][1] = std::sin(math::radian(rotation));
  r.m[1][0] = -std::sin(math::radian(rotation));
  r.m[1][1] = std::cos(math::radian(rotation));
  mat3 t = mat3::identity(); t.set_translation(translation[0], translation[1], 1.0f);
  tex_out->transform = s * r * t;
  tex_out->tx_rotation = rotation;
  tex_out->tx_translation = translation;
  tex_out->tx_scale = scale;
  tex_out->has_transform2d = true;
}

static OpenPBRSurface ConvertMtlxStandardSurfaceToOpenPBRSurface(
    const MtlxAutodeskStandardSurface &src) {
  OpenPBRSurface dst;

  // Base layer
  dst.base_weight = src.base;
  dst.base_color = src.base_color;
  dst.base_diffuse_roughness = src.diffuse_roughness;
  dst.base_metalness = src.metalness;

  // Specular layer
  dst.specular_weight = src.specular;
  dst.specular_color = src.specular_color;
  dst.specular_roughness = src.specular_roughness;
  dst.specular_ior = src.specular_IOR;
  dst.specular_anisotropy = src.specular_anisotropy;
  dst.specular_rotation = src.specular_rotation;

  // Transmission
  dst.transmission_weight = src.transmission;
  dst.transmission_color = src.transmission_color;
  dst.transmission_depth = src.transmission_depth;
  dst.transmission_scatter = src.transmission_scatter;
  dst.transmission_scatter_anisotropy = src.transmission_scatter_anisotropy;
  dst.transmission_dispersion = src.transmission_dispersion;
  // Note: StandardSurface.transmission_extra_roughness has no OpenPBR equivalent

  // Subsurface
  dst.subsurface_weight = src.subsurface;
  dst.subsurface_color = src.subsurface_color;
  // StandardSurface stores mean-free-path radius as a color3 (one value per
  // channel), while OpenPBR splits the same quantity into a scalar radius and
  // a color scale. Preserve the authored per-channel values in the scale and
  // use the neutral scalar radius so graph-connected radius inputs are not
  // discarded during the conversion.
  dst.subsurface_radius_scale = src.subsurface_radius;
  dst.subsurface_radius.set_value(Animatable<float>(1.0f));
  dst.subsurface_scale = src.subsurface_scale;
  dst.subsurface_anisotropy = src.subsurface_anisotropy;

  // Sheen
  dst.sheen_weight = src.sheen;
  dst.sheen_color = src.sheen_color;
  dst.sheen_roughness = src.sheen_roughness;

  // Coat
  dst.coat_weight = src.coat;
  dst.coat_color = src.coat_color;
  dst.coat_roughness = src.coat_roughness;
  dst.coat_anisotropy = src.coat_anisotropy;
  dst.coat_rotation = src.coat_rotation;
  dst.coat_ior = src.coat_IOR;
  dst.coat_affect_roughness = src.coat_affect_roughness;
  dst.coat_affect_color = src.coat_affect_color;

  // Thin film
  dst.thin_film_thickness = src.thin_film_thickness;
  dst.thin_film_ior = src.thin_film_IOR;
  // StandardSurface has no separate film-weight input. A positive authored
  // thickness denotes an active film; keeping the weight at one preserves
  // that intent in the OpenPBR representation (zero thickness remains a
  // no-op in the shader).
  float thin_film_thickness = 0.0f;
  src.thin_film_thickness.get_value().get(value::TimeCode::Default(),
                                           &thin_film_thickness);
  if (!src.thin_film_thickness.has_connections()) {
    // Assigning between TypedAttributeWithFallback instances copies authored
    // state, not the source schema's fallback. OpenPBR's 500 nm fallback must
    // not replace Standard Surface's 0 nm fallback.
    dst.thin_film_thickness.set_value(
        Animatable<float>(thin_film_thickness));
  }
  dst.thin_film_weight.set_value(Animatable<float>(
      (src.thin_film_thickness.has_connections() ||
       thin_film_thickness > 0.0f)
          ? 1.0f
          : 0.0f));

  // Emission
  dst.emission_luminance = src.emission;
  dst.emission_color = src.emission_color;

  // Opacity: StandardSurface is color3f, OpenPBR is float. Preserve a graph
  // connection so ConvertOpenPBRSurfaceShader can resolve its texture; only
  // reduce an authored constant to Rec.709 luminance.
  if (src.opacity.has_connections()) {
    dst.opacity.set_connections(src.opacity.get_connections());
  } else {
    value::color3f opacity{1.0f, 1.0f, 1.0f};
    src.opacity.get_value().get(value::TimeCode::Default(), &opacity);
    const float alpha = 0.2126f * opacity[0] + 0.7152f * opacity[1] +
                        0.0722f * opacity[2];
    dst.opacity.set_value(Animatable<float>(alpha));
  }

  // Geometry (normal, tangent)
  // StandardSurface uses TypedAttribute (optional, no fallback),
  // OpenPBR uses TypedAttributeWithFallback. Extract value if authored.
  if (src.normal.has_connections()) {
    dst.normal.set_connections(src.normal.get_connections());
  } else if (src.normal.authored()) {
    auto nval = src.normal.get_value();  // nonstd::optional<Animatable<normal3f>>
    if (nval) {
      dst.normal.set_value(*nval);
    }
  }
  if (src.tangent.has_connections()) {
    dst.tangent.set_connections(src.tangent.get_connections());
  } else if (src.tangent.authored()) {
    auto tval = src.tangent.get_value();  // nonstd::optional<Animatable<vector3f>>
    if (tval) {
      dst.tangent.set_value(*tval);
    }
  }

  return dst;
}

static OpenPBRSurface ConvertMtlxOpenPBRSurfaceToOpenPBRSurface(
    const MtlxOpenPBRSurface &src) {
  OpenPBRSurface dst;

  // Copy base layer properties
  dst.base_weight = src.base_weight;
  dst.base_color = src.base_color;
  dst.base_roughness = src.base_diffuse_roughness;
  dst.base_metalness = src.base_metalness;
  dst.base_diffuse_roughness = src.base_diffuse_roughness;

  // Copy specular properties
  dst.specular_weight = src.specular_weight;
  dst.specular_color = src.specular_color;
  dst.specular_roughness = src.specular_roughness;
  dst.specular_ior = src.specular_ior;
  dst.specular_anisotropy = src.specular_anisotropy;
  dst.specular_rotation = src.specular_rotation;
  dst.specular_roughness_anisotropy = src.specular_roughness_anisotropy;

  // Copy transmission properties
  dst.transmission_weight = src.transmission_weight;
  dst.transmission_color = src.transmission_color;
  dst.transmission_depth = src.transmission_depth;
  dst.transmission_scatter = src.transmission_scatter;
  dst.transmission_scatter_anisotropy = src.transmission_scatter_anisotropy;
  dst.transmission_dispersion = src.transmission_dispersion;
  dst.transmission_dispersion_abbe_number = src.transmission_dispersion_abbe_number;
  dst.transmission_dispersion_scale = src.transmission_dispersion_scale;

  // Copy subsurface properties
  dst.subsurface_weight = src.subsurface_weight;
  dst.subsurface_color = src.subsurface_color;
  dst.subsurface_scale = src.subsurface_scale;
  dst.subsurface_anisotropy = src.subsurface_anisotropy;
  dst.subsurface_scatter_anisotropy = src.subsurface_scatter_anisotropy;

  // Copy coat properties
  dst.coat_weight = src.coat_weight;
  dst.coat_color = src.coat_color;
  dst.coat_roughness = src.coat_roughness;
  dst.coat_anisotropy = src.coat_anisotropy;
  dst.coat_rotation = src.coat_rotation;
  dst.coat_ior = src.coat_ior;
  dst.coat_affect_color = src.coat_affect_color;
  dst.coat_affect_roughness = src.coat_affect_roughness;
  dst.coat_roughness_anisotropy = src.coat_roughness_anisotropy;
  dst.coat_darkening = src.coat_darkening;

  // Copy fuzz properties (velvet/fabric-like appearance)
  dst.fuzz_weight = src.fuzz_weight;
  dst.fuzz_color = src.fuzz_color;
  dst.fuzz_roughness = src.fuzz_roughness;

  // Copy thin film properties (iridescence)
  dst.thin_film_weight = src.thin_film_weight;
  dst.thin_film_thickness = src.thin_film_thickness;
  dst.thin_film_ior = src.thin_film_ior;

  // Copy emission properties
  dst.emission_luminance = src.emission_luminance;
  dst.emission_color = src.emission_color;

  // Copy geometry properties
  dst.opacity = src.geometry_opacity;
  if (src.geometry_normal.has_value()) {
    auto normal_val = src.geometry_normal.get_value();
    if (normal_val) {
      dst.normal = normal_val.value();
    }
  }
  if (src.geometry_tangent.has_value()) {
    auto tangent_val = src.geometry_tangent.get_value();
    if (tangent_val) {
      dst.tangent = tangent_val.value();
    }
  }

  return dst;
}

static std::string MtlxNormalMapUvName(
    const MtlxNodeGraphInfo &normal_info,
    const std::string &default_uv_name) {
  if (normal_info.has_geomprop && !normal_info.geomprop_name.empty()) {
    return normal_info.geomprop_name;
  }
  return MtlxTexcoordIndexName(default_uv_name, normal_info.texcoord_index);
}

static void ApplyMtlxNormalMapTextureInfo(
    const MtlxNodeGraphInfo &normal_info, const std::string &default_uv_name,
    UVTexture *uvtex) {
  if (!uvtex) {
    return;
  }
  uvtex->varname_uv = MtlxNormalMapUvName(normal_info, default_uv_name);
  uvtex->connectedOutputChannel = UVTexture::Channel::RGB;
  uvtex->wrapS = UVTexture::WrapMode::REPEAT;
  uvtex->wrapT = UVTexture::WrapMode::REPEAT;
  if (normal_info.has_uvtransform) {
    value::float2 scale;
    scale[0] = normal_info.uvtiling[0];
    scale[1] = normal_info.uvtiling[1];
    value::float2 translation;
    translation[0] = normal_info.uvoffset[0];
    translation[1] = normal_info.uvoffset[1];
    ApplyTexTransform2d(0.0f, scale, translation, uvtex);
  }
}

static int32_t ApplyMtlxNormalMapInfoToOpenPBRShader(
    const MtlxNodeGraphInfo &normal_info, const std::string &default_uv_name,
    std::vector<TextureImage> *images, std::vector<UVTexture> *textures,
    OpenPBRSurfaceShader *openpbr_shader) {
  if (!images || !textures || !openpbr_shader) {
    return -1;
  }

  if (!normal_info.has_normal_map) {
    return -1;
  }

  openpbr_shader->normal_map_scale = normal_info.normal_map_scale;

  if (normal_info.normal_map_texture.empty()) {
    return -1;
  }

  TextureImage tex_image;
  tex_image.asset_identifier = normal_info.normal_map_texture;
  tex_image.colorSpace = ColorSpace::Raw;  // Normal maps are always raw/linear
  tex_image.usdColorSpace = ColorSpace::Raw;

  int64_t image_id = -1;
  for (size_t i = 0; i < images->size(); ++i) {
    if ((*images)[i].asset_identifier == normal_info.normal_map_texture) {
      image_id = static_cast<int64_t>(i);
      break;
    }
  }

  if (image_id < 0) {
    image_id = static_cast<int64_t>(images->size());
    images->push_back(tex_image);
  }

  UVTexture uvtex;
  uvtex.texture_image_id = static_cast<int32_t>(image_id);
  ApplyMtlxNormalMapTextureInfo(normal_info, default_uv_name, &uvtex);

  int32_t tex_id = static_cast<int32_t>(textures->size());
  textures->push_back(uvtex);
  openpbr_shader->normal.texture_id = tex_id;
  return tex_id;
}

static bool ApplyMtlxTangentInfoToOpenPBRShader(
    const MtlxNodeGraphInfo &tangent_info,
    OpenPBRSurfaceShader *openpbr_shader) {
  if (!openpbr_shader) {
    return false;
  }

  if (!tangent_info.has_tangent_rotation) {
    return false;
  }

  openpbr_shader->tangent_rotation = tangent_info.tangent_rotation;
  return true;
}

static void ApplyMtlxGeometryNodeGraphInfoToOpenPBRShader(
    const Stage &stage, const Prim *material_prim,
    const MtlxOpenPBRSurface &mtlx_openpbr, const std::string &default_uv_name,
    std::vector<TextureImage> *images, std::vector<UVTexture> *textures,
    OpenPBRSurfaceShader *openpbr_shader, std::string *err,
    bool emit_extract_debug_trace) {
  if (!material_prim || !images || !textures || !openpbr_shader) {
    return;
  }

  // Check if geometry_normal has connections (links to NodeGraph with ND_normalmap node)
  const auto &normal_conns = mtlx_openpbr.geometry_normal.get_connections();
  DCOUT("DEBUG: geometry_normal has " << normal_conns.size()
        << " connections, has_value="
        << mtlx_openpbr.geometry_normal.has_value());

  if (!normal_conns.empty()) {
    if (emit_extract_debug_trace) {
      DCOUT("DEBUG: First connection path: " << normal_conns[0].full_path_name());
    }

    std::string extract_debug;
    std::string *extract_err = emit_extract_debug_trace ? &extract_debug : err;
    auto normal_info_result = ExtractMtlxNodeGraphInfo(
        stage, material_prim, normal_conns, extract_err);

    if (emit_extract_debug_trace && !extract_debug.empty()) {
      DCOUT("ExtractMtlxNodeGraphInfo debug:\n" << extract_debug);
    }

    if (normal_info_result) {
      const auto &normal_info = normal_info_result.value();
      DCOUT("DEBUG: ExtractMtlxNodeGraphInfo returned: has_normal_map="
            << normal_info.has_normal_map
            << ", normal_map_scale=" << normal_info.normal_map_scale
            << ", normal_map_texture='" << normal_info.normal_map_texture << "'");

      int32_t tex_id = ApplyMtlxNormalMapInfoToOpenPBRShader(
          normal_info, default_uv_name, images, textures, openpbr_shader);
      if (normal_info.has_normal_map) {
        DCOUT("DEBUG: Extracted normal_map_scale: "
              << normal_info.normal_map_scale);
      }
      if (tex_id >= 0) {
        DCOUT("DEBUG: Created normal map UVTexture with tex_id: " << tex_id);
      }
    } else {
      std::string error_message;
      if (emit_extract_debug_trace) {
        error_message = extract_debug;
      } else if (err) {
        error_message = *err;
      }
      DCOUT("DEBUG: ExtractMtlxNodeGraphInfo failed: " << error_message);
    }
  }

  // Check if geometry_tangent has connections (links to NodeGraph with ND_rotate3d_vector3 node)
  const auto &tangent_conns = mtlx_openpbr.geometry_tangent.get_connections();
  if (!tangent_conns.empty()) {
    auto tangent_info_result = ExtractMtlxNodeGraphInfo(
        stage, material_prim, tangent_conns, err);
    if (tangent_info_result) {
      const auto &tangent_info = tangent_info_result.value();
      if (ApplyMtlxTangentInfoToOpenPBRShader(tangent_info, openpbr_shader)) {
        DCOUT("DEBUG: Extracted tangent_rotation: "
              << tangent_info.tangent_rotation);
      }
    }
  }

  // Check if geometry_coat_normal has connections
  const auto &coat_normal_conns = mtlx_openpbr.geometry_coat_normal.get_connections();
  if (!coat_normal_conns.empty()) {
    auto coat_normal_info_result = ExtractMtlxNodeGraphInfo(
        stage, material_prim, coat_normal_conns, err);
    if (coat_normal_info_result) {
      const auto &coat_normal_info = coat_normal_info_result.value();
      if (coat_normal_info.has_normal_map) {
        openpbr_shader->coat_normal_map_scale = coat_normal_info.normal_map_scale;
        // Create coat normal map texture (same logic as base normal map)
        if (!coat_normal_info.normal_map_texture.empty()) {
          TextureImage coat_nmap_img;
          coat_nmap_img.asset_identifier = coat_normal_info.normal_map_texture;
          coat_nmap_img.colorSpace = ColorSpace::Raw;
          coat_nmap_img.usdColorSpace = ColorSpace::Raw;
          images->push_back(coat_nmap_img);

          UVTexture coat_nmap_tex;
          coat_nmap_tex.texture_image_id = static_cast<int32_t>(images->size() - 1);
          ApplyMtlxNormalMapTextureInfo(coat_normal_info, default_uv_name,
                                        &coat_nmap_tex);
          textures->push_back(coat_nmap_tex);

          openpbr_shader->coat_normal.texture_id = static_cast<int32_t>(textures->size() - 1);
        }
      }
    }
  }

  // Check if geometry_coat_tangent has connections
  const auto &coat_tangent_conns = mtlx_openpbr.geometry_coat_tangent.get_connections();
  if (!coat_tangent_conns.empty()) {
    auto coat_tangent_info_result = ExtractMtlxNodeGraphInfo(
        stage, material_prim, coat_tangent_conns, err);
    if (coat_tangent_info_result) {
      const auto &coat_tangent_info = coat_tangent_info_result.value();
      if (coat_tangent_info.has_tangent_rotation) {
        openpbr_shader->coat_tangent_rotation = coat_tangent_info.tangent_rotation;
      }
    }
  }
}

static bool GetMaterialXMtlxSurfaceConnection(const Material &material,
                                              Path *surface_path,
                                              std::string *err) {
  if (!surface_path) {
    if (err) *err += "surface_path argument is nullptr.\n";
    return false;
  }

  std::vector<Path> paths;
  if (material.mtlxSurface.authored()) {
    paths = material.mtlxSurface.get_connections();
  } else {
    auto it = material.props.find("outputs:mtlx:surface.connect");
    if (it != material.props.end()) {
      const Property &prop = it->second;
      if (prop.is_attribute()) {
        paths = prop.get_attribute().connections();
      } else if (prop.is_relationship()) {
        paths = prop.get_relationTargets();
      }
    }
  }

  if (paths.empty()) {
    return false;
  }
  if (paths.size() != 1) {
    if (err) {
      (*err) += "outputs:mtlx:surface must be connection with single target "
                "Path.\n";
    }
    return false;
  }

  (*surface_path) = paths[0];
  return true;
}

// Resolve a material surface-terminal connection that lands on a NodeGraph down
// to the terminal surface Shader Prim. MaterialX-authored materials commonly
// wrap the surface Shader inside a NodeGraph and connect the material's surface
// output (outputs:surface / outputs:mtlx:surface) to that NodeGraph, in one of
// two styles:
//   (a) the NodeGraph authors the referenced output (e.g. `outputs:surface`)
//       with a single connection forwarding to the inner Shader -> follow it;
//   (b) the NodeGraph does not author/connect that output (e.g. a MaterialX
//       `outputs:out` terminal is implicit) -> scan the NodeGraph's direct
//       child Shaders for a supported surface `info:id`.
// On success returns the terminal Shader Prim and rewrites `*surfacePath` to
// point at it. Returns `startPrim` unchanged when it is already a non-NodeGraph
// prim (the common direct-Shader case), or nullptr if no Shader was reached
// within the depth budget.
static const Prim *ResolveSurfaceShaderThroughNodeGraph(const Stage &stage,
                                                        const Prim *startPrim,
                                                        Path *surfacePath) {
  if (!startPrim) {
    return nullptr;
  }
  const Prim *prim = startPrim;
  std::string err;
  constexpr int kMaxNodeGraphDepth = 16;
  for (int depth = 0; depth < kMaxNodeGraphDepth; ++depth) {
    const NodeGraph *ng = prim->as<NodeGraph>();
    if (!ng) {
      return prim;  // terminal (Shader or other) prim reached.
    }

    // (a) authored + singly-connected output on the NodeGraph -> follow it.
    auto it = ng->props.find(surfacePath->prop_part());
    if (it != ng->props.end() && it->second.is_attribute() &&
        it->second.get_attribute().has_connections()) {
      const auto &conns = it->second.get_attribute().connections();
      const Prim *next{nullptr};
      if (conns.size() == 1 &&
          stage.find_prim_at_path(Path(conns[0].prim_part(), ""), next, &err) &&
          next) {
        *surfacePath = conns[0];
        prim = next;
        continue;
      }
      // The output IS explicitly authored, but its connection is broken
      // (multiple targets, or a target that fails stage lookup). Do NOT mask it
      // by heuristically grabbing an unrelated child Shader (style b) — surface
      // the failure so the caller can degrade/warn.
      return nullptr;
    }

    // (b) implicit terminal: the NodeGraph does not author/connect this output,
    // so fall back to a direct child Shader with a supported surface info:id.
    const Prim *childShader{nullptr};
    for (const auto &child : prim->children()) {
      const Shader *sh = child.as<Shader>();
      if (!sh) continue;
      if (sh->info_id == kNdOpenPbrSurfaceSurfaceshader ||
          sh->info_id == kNdUsdPreviewSurfaceSurfaceshader ||
          sh->info_id == kNdStandardSurfaceSurfaceshader) {
        childShader = &child;
        *surfacePath = Path(surfacePath->prim_part(), std::string())
                           .append_element(child.element_name());
        break;
      }
    }
    if (!childShader) {
      return nullptr;
    }
    prim = childShader;  // a Shader -> returns on the next iteration.
  }
  return nullptr;
}

bool RenderSceneConverter::ConvertMaterial(const RenderSceneConverterEnv &env,
                                           const Path &mat_abs_path,
                                           const lightusd::Material &material,
                                           RenderMaterial *rmat_out) {
  if (!rmat_out) {
    PUSH_ERROR_AND_RETURN("rmat_out argument is nullptr.");
  }

  RenderMaterial rmat;
  rmat.abs_path = mat_abs_path.prim_part();
  rmat.name = mat_abs_path.element_name();
  if (material.materialXConfig) {
    const MaterialXConfigAPI &config = *material.materialXConfig;
    rmat.materialXConfig.authored = true;
    rmat.materialXConfig.version = config.mtlx_version.get_value();
    rmat.materialXConfig.name_space = config.mtlx_namespace.get_value();
    rmat.materialXConfig.colorspace = config.mtlx_colorspace.get_value();
    rmat.materialXConfig.source_uri = config.mtlx_sourceUri.get_value();
  }
  DCOUT("rmat.abs_path = " << rmat.abs_path);
  DCOUT("rmat.name = " << rmat.name);
  std::string err;
  Path surfacePath;

  //
  // surface shader
  // First try outputs:surface (standard USD), then outputs:mtlx:surface (MaterialX)
  bool has_surface_connection = false;
  {
    if (material.surface.authored()) {
      auto paths = material.surface.get_connections();
      DCOUT("paths = " << paths);
      // must have single targetPath.
      if (paths.size() != 1) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("{}'s outputs:surface must be connection with single "
                        "target Path.\n",
                        mat_abs_path.full_path_name()));
      }
      surfacePath = paths[0];
      has_surface_connection = true;
    } else if (env.material_config.strict_material_check) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "{}'s outputs:surface isn't authored.",
          mat_abs_path.full_path_name()));
    } else {
      PUSH_WARN(fmt::format(
          "{}'s outputs:surface isn't authored; producing an unshaded material. "
          "(set material_config.strict_material_check=true to make this an error.)",
          mat_abs_path.full_path_name()));
    }
  }
  if (has_surface_connection) {
    const Prim *shaderPrim{nullptr};
    if (!env.stage.find_prim_at_path(
            Path(surfacePath.prim_part(), /* prop part */ ""), shaderPrim,
            &err)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "{}'s outputs:surface isn't connected to exising Prim path.\n",
          mat_abs_path.full_path_name()));
    }

    if (!shaderPrim) {
      // this should not happen though.
      PUSH_ERROR_AND_RETURN("[InternalError] invalid Shader Prim.\n");
    }

    // MaterialX-style materials wrap the surface Shader in a NodeGraph and
    // connect outputs:surface to that NodeGraph's passthrough output (e.g.
    // material.outputs:surface -> </.../usdpreview.outputs:surface>, where
    // `usdpreview` is a NodeGraph whose own outputs:surface forwards to the
    // UsdPreviewSurface Shader defined inside it). Resolve such passthroughs
    // down to the terminal Shader prim.
    shaderPrim =
        ResolveSurfaceShaderThroughNodeGraph(env.stage, shaderPrim, &surfacePath);

    const Shader *shader = shaderPrim ? shaderPrim->as<Shader>() : nullptr;

    if (!shader) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("{}'s outputs:surface must be connected to Shader Prim, "
                      "but connected to `{}` Prim.\n",
                      mat_abs_path.full_path_name(),
                      shaderPrim ? shaderPrim->prim_type_name()
                                 : std::string("<unresolved NodeGraph>")));
    }

    // Check for UsdPreviewSurface, OpenPBRSurface, MtlxOpenPBRSurface, or MtlxAutodeskStandardSurface
    const UsdPreviewSurface *psurface = shader->value.as<UsdPreviewSurface>();
    const OpenPBRSurface *openpbr = shader->value.as<OpenPBRSurface>();
    const MtlxOpenPBRSurface *mtlx_openpbr = shader->value.as<MtlxOpenPBRSurface>();
    const MtlxAutodeskStandardSurface *mtlx_standard = shader->value.as<MtlxAutodeskStandardSurface>();

    // The surface terminal output is `outputs:surface` for USD / UsdPreview /
    // OpenPBR shaders and `outputs:out` for MaterialX surfaceshader nodes; a
    // path resolved through a NodeGraph child-Shader scan may carry an empty
    // prop part. The shader type is validated above (`shader`) and below (the
    // value casts), and prop_part is not used to select any data, so accept
    // those cases and reject only a prop that clearly points elsewhere (e.g. a
    // shader input).
    const std::string surfaceProp = surfacePath.prop_part();
    if (!surfaceProp.empty() && surfaceProp != "outputs:surface" &&
        surfaceProp != "outputs:out") {
      PUSH_ERROR_AND_RETURN(
          fmt::format("{}'s outputs:surface connection must point to a surface "
                      "output (`outputs:surface`/`outputs:out`), but got `{}`",
                      mat_abs_path.full_path_name(), surfaceProp));
    }

    if (psurface) {
      // Convert UsdPreviewSurface. MaterialX's ND_UsdPreviewSurface_surfaceshader
      // wires its inputs to the enclosing Material's interface inputs (not to
      // UsdUVTexture nodes), so resolve them via the MaterialX interface path.
      const bool psurface_is_mtlx =
          (shader->info_id == kNdUsdPreviewSurfaceSurfaceshader);
      PreviewSurfaceShader pss;
      if (!ConvertPreviewSurfaceShader(env, surfacePath, *psurface, &pss,
                                       psurface_is_mtlx)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert UsdPreviewSurface : {}", surfacePath.prim_part()));
      }
      rmat.surfaceShader = pss;
    }

    if (openpbr) {
      // Convert OpenPBRSurface
      OpenPBRSurfaceShader openpbr_shader;
      if (!ConvertOpenPBRSurfaceShader(env, surfacePath, *openpbr, &openpbr_shader,
                                       /* is_materialx */ true)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert OpenPBRSurface : {}", surfacePath.prim_part()));
      }
      rmat.openPBRShader = openpbr_shader;
    }

    if (mtlx_openpbr) {
      // Convert MtlxOpenPBRSurface (Blender v4.5+ MaterialX export with ND_open_pbr_surface_surfaceshader)
      OpenPBRSurface converted_openpbr =
          ConvertMtlxOpenPBRSurfaceToOpenPBRSurface(*mtlx_openpbr);

      // Convert to OpenPBRSurfaceShader
      OpenPBRSurfaceShader openpbr_shader;
      if (!ConvertOpenPBRSurfaceShader(env, surfacePath, converted_openpbr,
                                       &openpbr_shader, /* is_materialx */ true,
                                       /* standard_surface_source */ true)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert MtlxOpenPBRSurface : {}", surfacePath.prim_part()));
      }
      // geometry_coat_normal has no field in the compatibility
      // OpenPBRSurface intermediate. Preserve its direct UsdUVTexture or
      // MaterialX image connection before the NodeGraph-specific normal-map
      // extraction below (which may refine/override it).
      TypedAttributeWithFallback<Animatable<value::normal3f>> coat_normal{
          value::normal3f{0.0f, 0.0f, 1.0f}};
      coat_normal.set_connections(
          mtlx_openpbr->geometry_coat_normal.get_connections());
      if (!ConvertPreviewSurfaceShaderParam(
              env, surfacePath, coat_normal,
              "geometry_coat_normal", openpbr_shader.coat_normal,
              /*is_materialx=*/true)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert OpenPBR coat normal : {}",
            surfacePath.prim_part()));
      }

      // Extract tangent rotation, normal map scale, and normal map texture from NodeGraph connections
      const Prim *material_prim{nullptr};
      bool found_prim = env.stage.find_prim_at_path(
              Path(mat_abs_path.prim_part(), /* prop part */ ""), material_prim,
              &err);
      if (found_prim && material_prim) {
        ApplyMtlxGeometryNodeGraphInfoToOpenPBRShader(
            env.stage, material_prim, *mtlx_openpbr,
            env.mesh_config.default_texcoords_primvar_name, &images, &textures,
            &openpbr_shader, &err,
            /*emit_extract_debug_trace*/ false);
      }

      rmat.openPBRShader = openpbr_shader;
    }

    if (mtlx_standard) {
      // Convert MtlxAutodeskStandardSurface (MaterialX StandardSurface via
      // ND_standard_surface_surfaceshader or MtlxAutodeskStandardSurface info:id)
      OpenPBRSurface converted_openpbr =
          ConvertMtlxStandardSurfaceToOpenPBRSurface(*mtlx_standard);

      OpenPBRSurfaceShader openpbr_shader;
      if (!ConvertOpenPBRSurfaceShader(env, surfacePath, converted_openpbr, &openpbr_shader, /* is_materialx */ true)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert MtlxAutodeskStandardSurface : {}", surfacePath.prim_part()));
      }
      if (mtlx_standard->coat_normal.authored() ||
          mtlx_standard->coat_normal.has_connections()) {
        TypedAttributeWithFallback<Animatable<value::normal3f>> coat_normal{
            value::normal3f{0.0f, 0.0f, 1.0f}};
        coat_normal.set_connections(
            mtlx_standard->coat_normal.get_connections());
        const auto coat_normal_value = mtlx_standard->coat_normal.get_value();
        if (coat_normal_value) {
          coat_normal.set_value(*coat_normal_value);
        }
        if (!ConvertPreviewSurfaceShaderParam(
                env, surfacePath, coat_normal, "coat_normal",
                openpbr_shader.coat_normal, /*is_materialx=*/true)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to convert Standard Surface coat normal : {}",
              surfacePath.prim_part()));
        }
      }
      if (!ConvertPreviewSurfaceShaderParam(
              env, surfacePath, mtlx_standard->displacement, "displacement",
              openpbr_shader.displacement, /*is_materialx=*/true)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to convert Standard Surface displacement : {}",
            surfacePath.prim_part()));
      }

      // Extract normal map and tangent info from NodeGraph connections
      // StandardSurface uses `normal` and `tangent` fields (not geometry_normal/geometry_tangent)
      const Prim *material_prim{nullptr};
      bool found_prim = env.stage.find_prim_at_path(
              Path(mat_abs_path.prim_part(), ""), material_prim, &err);
      if (found_prim && material_prim) {
        // Normal map extraction
        const auto &normal_conns = mtlx_standard->normal.get_connections();
        if (!normal_conns.empty()) {
          auto normal_info_result = ExtractMtlxNodeGraphInfo(
              env.stage, material_prim, normal_conns, &err);
          if (normal_info_result) {
            ApplyMtlxNormalMapInfoToOpenPBRShader(
                normal_info_result.value(),
                env.mesh_config.default_texcoords_primvar_name,
                &images, &textures, &openpbr_shader);
          }
        }
        // Tangent rotation extraction
        const auto &tangent_conns = mtlx_standard->tangent.get_connections();
        if (!tangent_conns.empty()) {
          auto tangent_info_result = ExtractMtlxNodeGraphInfo(
              env.stage, material_prim, tangent_conns, &err);
          if (tangent_info_result) {
            ApplyMtlxTangentInfoToOpenPBRShader(
                tangent_info_result.value(), &openpbr_shader);
          }
        }
      }

      rmat.openPBRShader = openpbr_shader;
    }

    if (!psurface && !openpbr && !mtlx_openpbr && !mtlx_standard) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Shader's info:id must be UsdPreviewSurface, OpenPBRSurface, "
                      "ND_open_pbr_surface_surfaceshader, or ND_standard_surface_surfaceshader, but got {}",
                      shader->info_id));
    }
  }

  //
  // Process MaterialX-specific surface shader when MaterialXConfigAPI is present
  // When MaterialXConfigAPI is authored, we look for MaterialX shaders
  {
    // Check if MaterialXConfigAPI is applied (via materialXConfig field)
    // For now, we only check the materialXConfig field as apiSchemas checking would need
    // proper MaterialXConfigAPI enum support in APISchemas::APIName
    bool has_materialx_api = material.materialXConfig.has_value();
    bool has_explicit_mtlx_surface =
        material.mtlxSurface.authored() ||
        material.props.find("outputs:mtlx:surface.connect") !=
            material.props.end();

    if (has_materialx_api || has_explicit_mtlx_surface) {
      DCOUT("Material has MaterialX terminal, looking for MaterialX shaders");

      // First try to parse outputs:mtlx:surface connection
      Path mtlxSurfacePath;
      bool has_mtlx_surface = false;

      if (has_explicit_mtlx_surface) {
        std::string mtlx_surface_err;
        if (GetMaterialXMtlxSurfaceConnection(material, &mtlxSurfacePath,
                                              &mtlx_surface_err)) {
          has_mtlx_surface = true;
          DCOUT("Found MaterialX surface connection: " << mtlxSurfacePath);
        } else if (!mtlx_surface_err.empty()) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "{}'s {}", mat_abs_path.full_path_name(), mtlx_surface_err));
        }
      }

      // If direct connection parsing failed, look for child Shader prims with a
      // supported MaterialX surface info:id.
      if (!has_mtlx_surface) {
        DCOUT("Direct connection not found, searching for child MaterialX "
              "surface shader info:id");

        // Get the material prim from the stage to access its children
        const Prim* mat_prim = nullptr;
        if (env.stage.find_prim_at_path(mat_abs_path, mat_prim, &err)) {
          if (mat_prim) {
            // Iterate through children to find OpenPBR shader
            for (const auto& child : mat_prim->children()) {
              const Shader* child_shader = child.as<Shader>();
              if (child_shader) {
                // Check if this is a supported MaterialX surface shader by its
                // info:id. MaterialX also defines a UsdPreviewSurface node with
                // the same inputs as USD PreviewSurface.
                if (child_shader->info_id == kNdOpenPbrSurfaceSurfaceshader ||
                    child_shader->info_id ==
                        "ND_open_pbr_surface_surfaceshader" ||
                    child_shader->info_id == kNdUsdPreviewSurfaceSurfaceshader ||
                    child_shader->info_id == kNdStandardSurfaceSurfaceshader) {
                  Path child_path = mat_abs_path;
                  child_path = child_path.append_element(child.element_name());
                  mtlxSurfacePath = child_path;
                  has_mtlx_surface = true;
                  DCOUT("Found MaterialX surface shader child: " << child_path);
                  break;
                }
              }
            }
          }
        }
      }

      // Process the found MaterialX shader
      if (has_mtlx_surface) {
        const Prim *mtlxShaderPrim{nullptr};
        if (!env.stage.find_prim_at_path(
                Path(mtlxSurfacePath.prim_part(), /* prop part */ ""), mtlxShaderPrim,
                &err) ||
            !mtlxShaderPrim) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "MaterialX shader path {} not found in stage",
              mtlxSurfacePath.full_path_name()));
        }

        // MaterialX materials commonly connect outputs:mtlx:surface to a
        // NodeGraph (whose authored terminal output, or a child surface Shader,
        // is the actual shader). Resolve the passthrough to the terminal Shader.
        mtlxShaderPrim = ResolveSurfaceShaderThroughNodeGraph(
            env.stage, mtlxShaderPrim, &mtlxSurfacePath);
        const Shader *mtlxShader =
            mtlxShaderPrim ? mtlxShaderPrim->as<Shader>() : nullptr;

        if (!mtlxShader && has_surface_connection) {
          // A primary outputs:surface shader was already converted above, so a
          // MaterialX terminal we cannot resolve to a Shader is demoted to a
          // warning and skipped rather than failing an otherwise-shaded material.
          PUSH_WARN(fmt::format(
              "{}'s outputs:mtlx:surface could not be resolved to a Shader Prim "
              "through its NodeGraph; skipping MaterialX surface.",
              mat_abs_path.full_path_name()));
        } else if (!mtlxShader) {
          // No primary surface was shaded either — this material's ONLY surface
          // terminal is the unresolved MaterialX one, so fail (the caller
          // substitutes the default material and records it as degraded) rather
          // than silently returning an all-default, unshaded material.
          PUSH_ERROR_AND_RETURN(fmt::format(
              "{}'s outputs:mtlx:surface could not be resolved to a Shader Prim "
              "through its NodeGraph, and no outputs:surface shader is authored.",
              mat_abs_path.full_path_name()));
        } else {
          // Check if it's an OpenPBR shader
          const UsdPreviewSurface *mtlx_psurface =
              mtlxShader->value.as<UsdPreviewSurface>();
          const MtlxOpenPBRSurface *mtlx_openpbr =
              mtlxShader->value.as<MtlxOpenPBRSurface>();
          const MtlxAutodeskStandardSurface *mtlx_standard =
              mtlxShader->value.as<MtlxAutodeskStandardSurface>();

          if (mtlx_psurface &&
              mtlxShader->info_id == kNdUsdPreviewSurfaceSurfaceshader) {
            DCOUT("Converting MaterialX UsdPreviewSurface to RenderMaterial");

            PreviewSurfaceShader pss;
            if (!ConvertPreviewSurfaceShader(env, mtlxSurfacePath,
                                             *mtlx_psurface, &pss,
                                             /* is_materialx */ true)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Failed to convert MaterialX UsdPreviewSurface : {}",
                  mtlxSurfacePath.prim_part()));
            }

            rmat.surfaceShader = pss;
            DCOUT("Successfully attached MaterialX UsdPreviewSurface shader to "
                  "RenderMaterial: "
                  << mtlxSurfacePath.full_path_name());
          } else if (mtlx_openpbr) {
            DCOUT("Converting MtlxOpenPBRSurface to RenderMaterial");

            OpenPBRSurface converted_openpbr =
                ConvertMtlxOpenPBRSurfaceToOpenPBRSurface(*mtlx_openpbr);

            // Convert to OpenPBRSurfaceShader
            OpenPBRSurfaceShader openpbr_shader;
            if (!ConvertOpenPBRSurfaceShader(env, mtlxSurfacePath,
                                             converted_openpbr,
                                             &openpbr_shader,
                                             /* is_materialx */ true,
                                             /* standard_surface_source */ true)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Failed to convert MtlxOpenPBRSurface : {}",
                  mtlxSurfacePath.prim_part()));
            } else {
              // Extract normal map texture from NodeGraph connections
              const Prim *material_prim_for_ng = nullptr;
              if (!env.stage.find_prim_at_path(mat_abs_path, material_prim_for_ng,
                                               &err)) {
                DCOUT("Could not find material prim at "
                      << mat_abs_path.full_path_name());
                material_prim_for_ng = nullptr;
              }

              ApplyMtlxGeometryNodeGraphInfoToOpenPBRShader(
                  env.stage, material_prim_for_ng, *mtlx_openpbr,
                  env.mesh_config.default_texcoords_primvar_name, &images,
                  &textures, &openpbr_shader, &err,
                  /*emit_extract_debug_trace*/ false);

              std::string nodegraph_json;
              std::string conv_err;
              if (ConvertShaderWithNodeGraphToJson(
                      *mtlxShaderPrim, mtlxSurfacePath, env.stage,
                      &nodegraph_json, &conv_err)) {
                if (!nodegraph_json.empty()) {
                  openpbr_shader.nodeGraphJson = nodegraph_json;
                }
              } else {
                DCOUT("No MaterialX NodeGraph found for explicit MaterialX "
                      "surface shader: "
                      << mtlxSurfacePath.prim_part() << " (" << conv_err
                      << ")");
              }

              rmat.openPBRShader = openpbr_shader;
              DCOUT("Successfully attached MaterialX OpenPBR shader to "
                    "RenderMaterial: "
                    << mtlxSurfacePath.full_path_name());
            }
          } else if (mtlx_standard) {
            DCOUT("Converting MtlxAutodeskStandardSurface to RenderMaterial");

            OpenPBRSurface converted_openpbr =
                ConvertMtlxStandardSurfaceToOpenPBRSurface(*mtlx_standard);

            OpenPBRSurfaceShader openpbr_shader;
            if (!ConvertOpenPBRSurfaceShader(env, mtlxSurfacePath,
                                             converted_openpbr,
                                             &openpbr_shader,
                                             /* is_materialx */ true)) {
              PUSH_ERROR_AND_RETURN(fmt::format(
                  "Failed to convert MtlxAutodeskStandardSurface : {}",
                  mtlxSurfacePath.prim_part()));
            } else {
              if (mtlx_standard->coat_normal.authored() ||
                  mtlx_standard->coat_normal.has_connections()) {
                TypedAttributeWithFallback<Animatable<value::normal3f>>
                    coat_normal{value::normal3f{0.0f, 0.0f, 1.0f}};
                coat_normal.set_connections(
                    mtlx_standard->coat_normal.get_connections());
                const auto coat_normal_value =
                    mtlx_standard->coat_normal.get_value();
                if (coat_normal_value) {
                  coat_normal.set_value(*coat_normal_value);
                }
                if (!ConvertPreviewSurfaceShaderParam(
                        env, mtlxSurfacePath, coat_normal, "coat_normal",
                        openpbr_shader.coat_normal,
                        /*is_materialx=*/true)) {
                  PUSH_ERROR_AND_RETURN(fmt::format(
                      "Failed to convert Standard Surface coat normal : {}",
                      mtlxSurfacePath.prim_part()));
                }
              }
              if (!ConvertPreviewSurfaceShaderParam(
                      env, mtlxSurfacePath, mtlx_standard->displacement,
                      "displacement", openpbr_shader.displacement,
                      /*is_materialx=*/true)) {
                PUSH_ERROR_AND_RETURN(fmt::format(
                    "Failed to convert Standard Surface displacement : {}",
                    mtlxSurfacePath.prim_part()));
              }
              const Prim *material_prim_for_ng = nullptr;
              if (!env.stage.find_prim_at_path(mat_abs_path,
                                               material_prim_for_ng, &err)) {
                DCOUT("Could not find material prim at "
                      << mat_abs_path.full_path_name());
                material_prim_for_ng = nullptr;
              }

              if (material_prim_for_ng) {
                const auto &normal_conns = mtlx_standard->normal.get_connections();
                if (!normal_conns.empty()) {
                  auto normal_info_result = ExtractMtlxNodeGraphInfo(
                      env.stage, material_prim_for_ng, normal_conns, &err);
                  if (normal_info_result) {
                    ApplyMtlxNormalMapInfoToOpenPBRShader(
                        normal_info_result.value(),
                        env.mesh_config.default_texcoords_primvar_name,
                        &images, &textures, &openpbr_shader);
                  }
                }
                const auto &tangent_conns =
                    mtlx_standard->tangent.get_connections();
                if (!tangent_conns.empty()) {
                  auto tangent_info_result = ExtractMtlxNodeGraphInfo(
                      env.stage, material_prim_for_ng, tangent_conns, &err);
                  if (tangent_info_result) {
                    ApplyMtlxTangentInfoToOpenPBRShader(
                        tangent_info_result.value(), &openpbr_shader);
                  }
                }
              }

              std::string nodegraph_json;
              std::string conv_err;
              if (ConvertShaderWithNodeGraphToJson(
                      *mtlxShaderPrim, mtlxSurfacePath, env.stage,
                      &nodegraph_json, &conv_err)) {
                if (!nodegraph_json.empty()) {
                  openpbr_shader.nodeGraphJson = nodegraph_json;
                }
              } else {
                DCOUT("No MaterialX NodeGraph found for standard_surface "
                      "shader: "
                      << mtlxSurfacePath.prim_part() << " (" << conv_err
                      << ")");
              }

              rmat.openPBRShader = openpbr_shader;
              DCOUT("Successfully attached MaterialX standard_surface shader "
                    "to RenderMaterial: "
                    << mtlxSurfacePath.full_path_name());
            }
          } else {
            const std::string msg = fmt::format(
                "Found shader {} but it's not a supported MaterialX surface "
                "shader (expected ND_open_pbr_surface_surfaceshader or "
                "ND_standard_surface_surfaceshader or "
                "ND_UsdPreviewSurface_surfaceshader, got {})",
                mtlxSurfacePath.prim_part(), mtlxShader->info_id);
            if (env.material_config.strict_material_check) {
              PUSH_ERROR_AND_RETURN(msg);
            }
            PUSH_WARN(msg + "; using default material appearance.");
          }
        }
      } else {
        DCOUT("No MaterialX OpenPBR shader found for material with MaterialXConfigAPI");
      }
    }
  }

  //
  // displacement output (outputs:displacement)
  //
  if (material.displacement.authored()) {
    auto disp_paths = material.displacement.get_connections();
    if (disp_paths.size() == 1) {
      rmat.has_displacement = true;
      rmat.displacement_shader_path = disp_paths[0].full_path_name();
      DCOUT("Material has displacement shader: " << rmat.displacement_shader_path);
    }
  }

  //
  // volume output (outputs:volume)
  //
  if (material.volume.authored()) {
    auto vol_paths = material.volume.get_connections();
    if (vol_paths.size() == 1) {
      rmat.has_volume = true;
      rmat.volume_shader_path = vol_paths[0].full_path_name();
      const Prim *volume_shader_prim = nullptr;
      std::string volume_lookup_err;
      if (env.stage.find_prim_at_path(
              Path(vol_paths[0].prim_part(), ""), volume_shader_prim,
              &volume_lookup_err) && volume_shader_prim) {
        if (const Shader *volume_shader = volume_shader_prim->as<Shader>()) {
          if (const ShaderNode *node = volume_shader->value.as<ShaderNode>()) {
            ApplyMaterialVolumeConstants(*node, &rmat.volume_density,
                                          rmat.volume_albedo,
                                          rmat.volume_emission_color,
                                          &rmat.volume_emission_scale);
          }
        }
      }
      DCOUT("Material has volume shader: " << rmat.volume_shader_path);
    }
  }

  DCOUT("Converted Material: " << mat_abs_path);

  (*rmat_out) = rmat;
  return true;
}


}  // namespace tydra
}  // namespace lightusd
