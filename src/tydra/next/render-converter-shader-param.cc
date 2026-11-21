// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "render-converter-internal.hh"
#include "render-converter-mtlx-eval.hh"
#include "render-converter-shader-param.hh"
#include "next/layer/prim-spec.hh"
#include "next/schema/usd-shade.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include "render-converter-prop-ids.hh"
#include "mem-budget.hh"
#include "next/schema/color-space.hh"
#include "next/layer/asset-anchor.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/types/type-info.hh"
#include <limits>

namespace lightusd { namespace tydra { namespace next {
namespace shader_param_detail {
namespace {
bool IsTextureEndpoint(const Stage& stage, const UsdPrim& prim,
                       double time_code) {
  if (!prim.IsValid()) return false;
  std::string id;
  GetToken(prim, "info:id", &id);
  if (id == "UsdUVTexture" || id == "HwPtexTexture" ||
      id.rfind("HwPtexTexture", 0) == 0 || id == "PxrPtexture" ||
      id == "image" || id == "tiledimage" ||
      id.rfind("ND_image_", 0) == 0 ||
      id.rfind("ND_tiledimage_", 0) == 0) {
    return true;
  }
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  return eval.EvalAssetPath(prim, "inputs:file").has_value() ||
         eval.EvalString(prim, "inputs:file").has_value() ||
         eval.EvalAssetPath(prim, "inputs:filename").has_value() ||
         eval.EvalString(prim, "inputs:filename").has_value();
}

const std::vector<::lightusd::next::Path>* PrimaryDataInputConnection(
    const UsdPrim& prim) {
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return nullptr;

  static const char* kPreferredInputs[] = {
      "inputs:in", "inputs:in1", "inputs:dispScalar", "inputs:inputRGB",
      "inputs:fg", "inputs:bg"};
  for (const char* preferred : kPreferredInputs) {
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(preferred);
    if (connections && !connections->empty()) return connections;
  }

  auto is_factor_input = [](const std::string& name) {
    return name == "inputs:mix" || name == "inputs:amount" ||
           name == "inputs:weight" || name == "inputs:factor" ||
           name == "inputs:alpha" || name == "inputs:mask";
  };
  for (const std::string& property : prim.GetPropertyNames()) {
    if (property.rfind("inputs:", 0) != 0 || is_factor_input(property)) {
      continue;
    }
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(property);
    if (connections && !connections->empty()) return connections;
  }
  return nullptr;
}

bool ResolveConnectedEndpoint(const Stage& stage,
                              const std::string& connection_path,
                              double time_code,
                              std::string* endpoint_path) {
  if (!endpoint_path) return false;
  std::string current = connection_path;
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;

    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    UsdPrim prim = stage.GetPrimAtPath(prim_path);
    if (!prim.IsValid()) return false;
    if (IsTextureEndpoint(stage, prim, time_code)) {
      *endpoint_path = current;
      return true;
    }

    const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(prim);
    }
    if (!connections || connections->empty()) {
      *endpoint_path = current;
      return true;
    }
    current = (*connections)[0].str();
  }
  return false;
}

bool ResolveConnectedValue(const Stage& stage,
                           const std::string& connection_path,
                           double time_code,
                           Value* out) {
  if (!out) return false;

  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string prop_name;
  if (!SplitConnectionPath(endpoint, &prim_path, &prop_name)) return false;

  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::next::EvalOptions opts = eval.GetOptions();
  opts.follow_connections = false;
  ::lightusd::next::EvalResult result = eval.EvalWith(prim, prop_name, opts);
  if (result.success) {
    *out = std::move(result.value);
    return true;
  }

  return false;
}

RenderTexture::Channel ChannelFromConnection(
    const std::string& connection_path, const UsdPrim& texture_prim) {
  size_t pos = connection_path.find(".outputs:");
  if (pos == std::string::npos) {
    return RenderTexture::Channel::RGBA;
  }

  const std::string channel = connection_path.substr(pos + 9);
  if (channel == "r" || channel == "x") return RenderTexture::Channel::R;
  if (channel == "g" || channel == "y") return RenderTexture::Channel::G;
  if (channel == "b" || channel == "z") return RenderTexture::Channel::B;
  if (channel == "a" || channel == "w") return RenderTexture::Channel::A;
  if (channel == "rgb" || channel == "xyz") return RenderTexture::Channel::RGB;

  // MaterialX image nodes conventionally expose a generic `outputs:out`.
  // Recover its scalar/vector shape from the synthesized output value/type so
  // roughness and metallic maps sample R while color/normal maps sample RGB.
  if (channel == "out" && texture_prim.IsValid()) {
    std::string type;
    if (const Value* value = texture_prim.GetPropertyValue(kIdOutputsOut())) {
      if (auto token = value->as_token()) type = *token;
      else if (auto str = value->as_string()) type = *str;
    }
    if (type.empty()) {
      if (const ::lightusd::next::PrimSpec* spec =
              texture_prim.GetPrimSpec()) {
        if (const std::string* declared =
                spec->property_type_name("outputs:out")) {
          type = *declared;
        }
      }
    }
    if (type == "float" || type == "integer" || type == "boolean") {
      return RenderTexture::Channel::R;
    }
    if (type == "color3" || type == "color3f" || type == "vector3" ||
        type == "vector3f") {
      return RenderTexture::Channel::RGB;
    }
  }
  return RenderTexture::Channel::RGBA;
}

WrapMode ParseWrapMode(const std::string& token) {
  // UsdUVTexture uses repeat/clamp/mirror/black, while MaterialX image nodes
  // call the equivalent modes periodic/clamp/mirror/constant. Keep the
  // translation at the RenderTexture boundary so every backend sees one
  // canonical enum.
  if (token == "repeat" || token == "periodic") return WrapMode::Repeat;
  if (token == "clamp") return WrapMode::Clamp;
  if (token == "mirror") return WrapMode::Mirror;
  if (token == "black" || token == "constant") return WrapMode::Black;
  // UsdUVTexture's wrapS/wrapT fallback is "useMetadata"; with no texture
  // metadata the effective mode is clamp-to-edge (legacy tydra behavior) —
  // NOT repeat, which visibly tiles textures authored to clamp.
  return WrapMode::Clamp;
}

ColorSpace ParseColorSpace(const std::string& token) {
  if (token == "raw") return ColorSpace::Raw;
  if (token == "linear" || token == "Linear" || token == "lin_srgb" ||
      token == "lin_rec709" || token == "scene-linear Rec.709-sRGB") {
    return ColorSpace::Linear;
  }
  if (token == "sRGB" || token == "srgb" || token == "srgb_texture") {
    return ColorSpace::sRGB;
  }
  if (token == "acescg" || token == "ACEScg") return ColorSpace::ACEScg;
  if (token == "rec709" || token == "Rec709") return ColorSpace::Rec709;
  if (token == "rec2020" || token == "Rec2020" ||
      token == "lin_rec2020") return ColorSpace::Rec2020;
  if (token == "displayP3" || token == "DisplayP3" ||
      token == "lin_displayp3" || token == "srgb_displayp3") {
    return ColorSpace::DisplayP3;
  }
  return ColorSpace::Unknown;
}

bool IsColorShaderInput(const UsdPrim& prim, const std::string& attr_name,
                        const std::string& param_name) {
  if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
    if (const std::string* type = spec->property_type_name(attr_name)) {
      if (type->rfind("color3", 0) == 0 || type->rfind("color4", 0) == 0) {
        return true;
      }
    }
  }
  static const std::set<std::string> kColorInputs = {
      "diffuseColor", "emissiveColor", "specularColor", "base_color",
      "baseColor", "specular_color", "transmission_color",
      "subsurface_color", "sheen_color", "coat_color", "emission_color"};
  return kColorInputs.count(param_name) != 0;
}

bool MaterialXConfiguredColorSpace(const UsdPrim& prim, std::string* out) {
  if (!out || !prim.IsValid()) return false;
  std::string id;
  if (!GetToken(prim, "info:id", &id) ||
      (id.rfind("ND_", 0) != 0 && id != "image" &&
       id != "tiledimage" && id != "open_pbr_surface" &&
       id != "standard_surface")) {
    return false;
  }
  for (UsdPrim current = prim; current.IsValid(); current = current.GetParent()) {
    const Value* value =
        current.GetPropertyValue("config:mtlx:colorspace");
    if (!value) continue;
    auto token = value->as_token();
    if (!token) token = value->as_string();
    if (token && !token->empty()) {
      *out = ::lightusd::color::CanonicalizeToken(*token);
      return true;
    }
  }
  return false;
}

bool ResolveConnectedColorSource(const Stage& stage,
                                 const std::string& connection_path,
                                 double time_code, UsdPrim* source_prim,
                                 std::string* source_property) {
  if (!source_prim || !source_property) return false;
  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string property;
  if (!SplitConnectionPath(endpoint, &prim_path, &property)) return false;
  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  // Prefer metadata on the resolved output itself. MaterialX constant and
  // utility nodes usually put it on their value/data input instead, so scan
  // those inputs before falling back to the terminal shader attribute.
  if (const ::lightusd::next::PropMeta* meta =
          prim.GetPropertyMeta(property)) {
    if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
      *source_prim = prim;
      *source_property = property;
      return true;
    }
  }
  std::string value_input;
  for (const std::string& candidate : prim.GetPropertyNames()) {
    if (candidate.rfind("inputs:", 0) != 0) continue;
    if (value_input.empty() || candidate == "inputs:value" ||
        candidate == "inputs:in") {
      value_input = candidate;
    }
    if (const ::lightusd::next::PropMeta* meta =
            prim.GetPropertyMeta(candidate)) {
      if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
        *source_prim = prim;
        *source_property = candidate;
        return true;
      }
    }
  }
  if (!value_input.empty()) {
    *source_prim = prim;
    *source_property = value_input;
    return true;
  }
  *source_prim = prim;
  *source_property = property;
  return true;
}

void ConvertShaderColorToWorking(const UsdPrim& prim,
                                 const std::string& attr_name,
                                 const std::string& param_name,
                                 const RenderScene* scene,
                                 ShaderParam* param) {
  if (!scene || !param || param->is_texture() ||
      !IsColorShaderInput(prim, attr_name, param_name)) {
    return;
  }
  std::string source;
  bool authored = false;
  if (!::lightusd::next::color_management::ComputeColorSpaceName(
          prim, attr_name, &source, &authored)) {
    return;
  }
  if (!authored) {
    (void)MaterialXConfiguredColorSpace(prim, &source);
  }
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::next::color_management::BuildColorTransform(
          prim, source, scene->working_color_space, &transform)) {
    return;
  }
  float rgb[3] = {param->value.x, param->value.y, param->value.z};
  ::lightusd::color::TransformRGB(transform, rgb);
  param->value.x = rgb[0];
  param->value.y = rgb[1];
  param->value.z = rgb[2];
}

}  // namespace

bool FindConnectedUtilityScalar(const Stage& stage, const UsdPrim& shader,
                                const std::string& shader_input,
                                const std::string& node_id_prefix,
                                const std::string& node_input,
                                double time_code, float* out) {
  if (!out || !shader.IsValid()) return false;
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  const std::string property = "inputs:" + shader_input;
  if (!eval.HasConnection(shader, property)) return false;
  std::string current = eval.GetConnectionPath(shader, property);
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;
    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    const UsdPrim node = stage.GetPrimAtPath(prim_path);
    if (!node.IsValid()) return false;
    std::string id;
    GetToken(node, "info:id", &id);
    if (id.rfind(node_id_prefix, 0) == 0) {
      if (std::optional<float> value =
              eval.EvalFloat(node, "inputs:" + node_input)) {
        *out = *value;
        return true;
      }
      return false;
    }
    const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(node);
    }
    if (!connections || connections->empty()) return false;
    current = (*connections)[0].str();
  }
  return false;
}

}  // namespace shader_param_detail

using namespace shader_param_detail;

bool RenderSceneConverter::Impl::ExtractShaderParam(const Stage& stage,
                                              const UsdPrim& shader_prim,
                                              const std::string& param_name,
                                              ShaderParam* out,
                                              RenderScene* scene) {
  // Material interface inputs use the same `inputs:*` namespace and value /
  // connection semantics as Shader inputs. Accept either here so degraded
  // material recovery can preserve constants authored on the Material prim.
  if (!out || !shader_prim.IsValid() ||
      (!::lightusd::next::IsShader(shader_prim) &&
       !::lightusd::next::IsMaterial(shader_prim))) {
    return false;
  }

  const std::string attr_name = "inputs:" + param_name;
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(config_.time_code);

  if (eval.HasConnection(shader_prim, attr_name)) {
    std::string connection_path = eval.GetConnectionPath(shader_prim, attr_name);
    std::string evaluation_space = "lin_rec709_scene";
    UsdPrim evaluation_context = shader_prim;
    std::string evaluation_endpoint;
    if (ResolveConnectedEndpoint(stage, connection_path, config_.time_code,
                                 &evaluation_endpoint)) {
      std::string evaluation_prim_path;
      std::string evaluation_property;
      if (SplitConnectionPath(evaluation_endpoint, &evaluation_prim_path,
                              &evaluation_property)) {
        UsdPrim candidate = stage.GetPrimAtPath(evaluation_prim_path);
        if (candidate.IsValid()) evaluation_context = candidate;
      }
    }
    (void)MaterialXConfiguredColorSpace(evaluation_context,
                                        &evaluation_space);
    MtlxConstantValue evaluated;
    std::set<std::string> visiting;
    if (EvalMtlxConstantConnection(stage, connection_path, config_.time_code,
                                   evaluation_space, &evaluated, &visiting,
                                   0)) {
      if (evaluated.components >= 3) {
        SetParamFloat3(out, evaluated.value[0], evaluated.value[1],
                       evaluated.value[2]);
      } else if (evaluated.components == 2) {
        SetParamFloat4(out, evaluated.value[0], evaluated.value[1], 0.0f,
                       1.0f);
      } else {
        SetParamFloat(out, evaluated.value[0]);
      }
      if (scene && evaluated.components >= 3 && evaluated.color_managed &&
          IsColorShaderInput(shader_prim, attr_name, param_name)) {
        ::lightusd::color::ColorTransform transform;
        if (::lightusd::next::color_management::BuildColorTransform(
                evaluation_context, evaluation_space,
                scene->working_color_space, &transform)) {
          float rgb[3] = {out->value.x, out->value.y, out->value.z};
          ::lightusd::color::TransformRGB(transform, rgb);
          out->value.x = rgb[0];
          out->value.y = rgb[1];
          out->value.z = rgb[2];
        }
      }
      return true;
    }
    // Follow localized NodeGraph outputs and primary utility-node inputs to
    // the terminal image/value. The shared walker is cycle-safe and is also
    // used by ResolveConnectedValue below.
    std::string endpoint;
    if (ResolveConnectedEndpoint(stage, connection_path, config_.time_code,
                                 &endpoint)) {
      connection_path = std::move(endpoint);
    }
    const std::string texture_prim_path = SourcePrimPathFromConnection(connection_path);
    UsdPrim texture_prim = stage.GetPrimAtPath(texture_prim_path);

    TextureNodeData tex_data;
    if (scene && ExtractTextureNodeData(stage, texture_prim, config_.time_code, &tex_data)) {
      // UsdUVTexture's sourceColorSpace schema only accepts "raw", "sRGB" and
      // "auto". OpenUSD ignores any other token and falls back to "auto"; the
      // ColorSpaceTests/UsdPreviewSurface README states this explicitly for
      // "lin_ap1_scene" ("the current behavior of OpenUSD is that it seems to
      // ignore the error and treat it as if it were set to auto"). Resolving
      // such a token as a real colour space instead diverged from the SDK.
      {
        std::string &cs = tex_data.source_color_space;
        if (!cs.empty() && cs != "auto" && cs != "raw" && cs != "sRGB" &&
            cs != "srgb") {
          cs = "auto";
        }
      }
      // An authored UsdColorSpaceAPI `colorSpace` (file metadata, the
      // `inputs:file:colorSpace:name` attribute, or an applied ColorSpaceAPI on
      // an enclosing prim) takes precedence over `sourceColorSpace`, including an
      // explicit raw/sRGB token. This is the opinion the ColorSpaceTests/
      // UsdPreviewSurface asset documents and builds its expected values on.
      {
        bool authored = false;
        std::string inherited;
        (void)::lightusd::next::color_management::ComputeColorSpaceName(
            texture_prim, "inputs:file", &inherited, &authored);
        if (authored) {
          tex_data.source_color_space = inherited;
        } else if (tex_data.source_color_space.empty() ||
                   tex_data.source_color_space == "auto") {
          (void)MaterialXConfiguredColorSpace(
              texture_prim, &tex_data.source_color_space);
        }
      }
      // "auto" resolves to sRGB only for 8-bit images; floating-point formats
      // (.hdr/.exr/.pfm) hold scene-linear data and must not be decoded again.
      if (tex_data.source_color_space.empty() ||
          tex_data.source_color_space == "auto") {
        auto lower_ext_is = [&](const char *ext) {
          const std::string &f = tex_data.file;
          const size_t n = std::strlen(ext);
          if (f.size() < n) return false;
          for (size_t i = 0; i < n; ++i) {
            char ch = f[f.size() - n + i];
            if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
            if (ch != ext[i]) return false;
          }
          return true;
        };
        if (lower_ext_is(".hdr") || lower_ext_is(".exr") ||
            lower_ext_is(".pfm")) {
          tex_data.source_color_space = "raw";
        }
      }
      const ColorSpace cs = ParseColorSpace(tex_data.source_color_space);
      const ColorSpace image_color_space =
          cs == ColorSpace::Unknown ? ColorSpace::sRGB : cs;
      const uint32_t asset_anchor =
          tex_data.file_asset_anchor_valid ? tex_data.file_asset_anchor
                                           : AssetAnchorOf(texture_prim);
      const std::string authored_anchor =
          ::lightusd::next::AssetAnchorPath(asset_anchor);
      std::vector<RenderUDIMTexture::Tile> udim_tiles;
      int32_t udim_texture_id = -1;
      const bool udim_path = tex_data.file.find("<UDIM>") != std::string::npos;
      if (udim_path) {
        udim_tiles.reserve(8);
        for (uint32_t udim_id = 1001; udim_id <= 1100; ++udim_id) {
          std::string tile_asset = tex_data.file;
          const size_t udim_token = tile_asset.find("<UDIM>");
          if (udim_token == std::string::npos) break;
          tile_asset.replace(udim_token, 6, std::to_string(udim_id));
          ::lightusd::next::ResolvedAsset resolved_tile;
          if (config_.asset_resolver) {
            resolved_tile = config_.asset_resolver->Resolve(
                tile_asset, authored_anchor.empty() ? std::string()
                                                    : (::lightusd::next::AssetResolver::IsPackagePath(authored_anchor) ? authored_anchor : authored_anchor + "/"));
          } else if (!config_.asset_base_dir.empty()) {
            ::lightusd::next::AssetResolver local_resolver;
            local_resolver.SetWorkingDirectory(config_.asset_base_dir);
            if (!authored_anchor.empty()) {
              resolved_tile = local_resolver.Resolve(tile_asset,
                                                     (::lightusd::next::AssetResolver::IsPackagePath(authored_anchor) ? authored_anchor : authored_anchor + "/"));
            }
            if (!resolved_tile.exists) {
              resolved_tile = local_resolver.Resolve(tile_asset);
            }
          }
          if (!resolved_tile.exists || resolved_tile.resolved_path.empty()) continue;
          const std::string& tile_path = resolved_tile.resolved_path;
          int32_t tile_image_id =
              FindCachedImageId(scene, tile_path, image_color_space);
          if (tile_image_id < 0) {
            TextureImage tile_image;
            tile_image.name = texture_prim.IsValid()
                                  ? texture_prim.GetName() + "." +
                                        std::to_string(udim_id)
                                  : tile_asset;
            tile_image.resolved_path = tile_path;
            tile_image.asset_identifier = tile_path;
            tile_image.color_space = image_color_space;
            if (config_.material.load_textures) {
              TextureImage loaded;
              if (LoadTexture(tile_path, &loaded)) {
                if (loaded.name.empty()) loaded.name = tile_image.name;
                if (loaded.resolved_path.empty()) loaded.resolved_path = tile_path;
                if (!config_.material.custom_texture_loader ||
                    loaded.color_space == ColorSpace::Unknown) {
                  loaded.color_space = image_color_space;
                }
                tile_image = std::move(loaded);
                tile_image.asset_identifier = tile_path;
              } else if (!config_.material.allow_missing_textures) {
                AddWarning("Failed to load UDIM tile: " + tile_asset);
                return false;
              }
            }
            tile_image_id = static_cast<int32_t>(scene->images.size());
            scene->images.push_back(std::move(tile_image));
            RememberImageId(scene, tile_path, image_color_space, tile_image_id);
          }
          udim_tiles.push_back({udim_id, tile_image_id});
        }
      }

      int32_t image_id = -1;
      std::string resolved;
      bool combined_udim = false;
      Float2 udim_uv_scale(1.0f, 1.0f);
      Float2 udim_uv_offset(0.0f, 0.0f);
      if (!udim_tiles.empty()) {
        std::string atlas_error;
        if (config_.material.combine_udim_tiles &&
            CombineUDIMTileImages(scene, udim_tiles, tex_data.file,
                                  config_.limits.max_resident_bytes, &image_id,
                                  &udim_uv_scale, &udim_uv_offset,
                                  &atlas_error)) {
          combined_udim = true;
          resolved = tex_data.file;
        } else {
          if (!atlas_error.empty()) {
            AddWarning("Unable to combine UDIM tiles for " + tex_data.file +
                       ": " + atlas_error + "; retaining sparse tiles");
          }
          RenderUDIMTexture udim;
          udim.prim_name = texture_prim.IsValid() ? texture_prim.GetName() : param_name;
          udim.abs_path = texture_prim_path;
          udim.display_name = udim.prim_name;
          if (texture_prim.IsValid()) {
            const auto* spec = texture_prim.GetPrimSpec();
            if (spec && !spec->meta().displayName().empty()) {
              udim.display_name = spec->meta().displayName();
            }
          }
          udim.asset_identifier = tex_data.file;
          udim.tiles = std::move(udim_tiles);
          image_id = udim.tiles.front().image_id;
          resolved = scene->images[static_cast<size_t>(image_id)].resolved_path;
          // Stable index, shared by textures which resolve the same UDIM asset.
          auto existing = std::find_if(
              scene->udim_textures.begin(), scene->udim_textures.end(),
              [&udim](const RenderUDIMTexture& candidate) {
                if (candidate.asset_identifier != udim.asset_identifier ||
                    candidate.tiles.size() != udim.tiles.size()) return false;
                for (size_t i = 0; i < candidate.tiles.size(); ++i) {
                  if (candidate.tiles[i].udim != udim.tiles[i].udim ||
                      candidate.tiles[i].image_id != udim.tiles[i].image_id) return false;
                }
                return true;
              });
          if (existing == scene->udim_textures.end()) {
            udim_texture_id = static_cast<int32_t>(scene->udim_textures.size());
            scene->udim_textures.push_back(std::move(udim));
          } else {
            udim_texture_id = static_cast<int32_t>(
                std::distance(scene->udim_textures.begin(), existing));
          }
        }
      } else {
        resolved = ResolveAssetPath(tex_data.file, asset_anchor);
        image_id = FindCachedImageId(scene, resolved, image_color_space);
      }
      if (image_id < 0) {
        TextureImage image;
        image.name = texture_prim.IsValid() ? texture_prim.GetName() : tex_data.file;
        image.resolved_path = resolved;
        image.asset_identifier = tex_data.file;
        image.color_space = image_color_space;
        if (config_.material.load_textures) {
          TextureImage loaded;
          if (LoadTexture(resolved, &loaded)) {
            if (loaded.name.empty()) loaded.name = image.name;
            if (loaded.resolved_path.empty()) loaded.resolved_path = resolved;
            if (!config_.material.custom_texture_loader ||
                loaded.color_space == ColorSpace::Unknown) {
              loaded.color_space = image.color_space;
            }
            image = std::move(loaded);
            image.asset_identifier = tex_data.file;
          } else if (!config_.material.allow_missing_textures) {
            AddWarning("Failed to load texture: " + tex_data.file);
            return false;
          }
        }
        image_id = static_cast<int32_t>(scene->images.size());
        // Key on the RESOLVED path/colorspace we looked up with -- a loaded
        // image may carry different values, and the next lookup uses these.
        scene->images.push_back(std::move(image));
        RememberImageId(scene, resolved, image_color_space, image_id);
      }

      RenderTexture texture;
      texture.name = texture_prim.IsValid() ? texture_prim.GetName() : param_name;
      texture.prim_path = texture_prim_path;
      texture.asset_path = tex_data.file;
      texture.ktx2_hint = tex_data.ktx2_hint;
      texture.wrap_s = ParseWrapMode(tex_data.wrap_s);
      texture.wrap_t = ParseWrapMode(tex_data.wrap_t);
      texture.scale_value = Float4(tex_data.scale[0], tex_data.scale[1],
                                   tex_data.scale[2], tex_data.scale[3]);
      texture.bias = Float4(tex_data.bias[0], tex_data.bias[1],
                            tex_data.bias[2], tex_data.bias[3]);
      texture.image_id = image_id;
      texture.is_udim = udim_texture_id >= 0 || combined_udim;
      texture.udim_texture_id = texture.is_udim ? udim_texture_id : -1;
      texture.udim_uv_scale = udim_uv_scale;
      texture.udim_uv_offset = udim_uv_offset;
      texture.source_color_space = tex_data.source_color_space;
      texture.target_color_space = scene->working_color_space;
      {
        std::string source_space = tex_data.source_color_space;
        if (source_space.empty() || source_space == "auto") {
          source_space = "srgb_rec709_scene";
        }
        ::lightusd::color::ColorTransform source_to_display;
        if (::lightusd::next::color_management::BuildColorTransform(
                texture_prim, source_space, "lin_rec709_scene",
                &source_to_display)) {
          texture.color_transform_valid = true;
          texture.color_transform_bypass = source_to_display.bypass;
          texture.source_color_is_data =
              source_to_display.source.kind ==
              ::lightusd::color::ColorSpaceKind::Data;
          texture.source_gamma = source_to_display.source.gamma;
          texture.source_linear_bias = source_to_display.source.linear_bias;
          std::copy(source_to_display.matrix, source_to_display.matrix + 9,
                    texture.source_to_display_linear);
        }
      }
      texture.output_channel =
          ChannelFromConnection(connection_path, texture_prim);
      // UsdTransform2d on the st chain (rotation is authored in degrees;
      // RenderTexture stores radians).
      texture.offset = Float2(tex_data.uv_translation[0],
                              tex_data.uv_translation[1]);
      texture.scale = Float2(tex_data.uv_scale[0], tex_data.uv_scale[1]);
      texture.rotation = tex_data.uv_rotation * 3.14159265358979323846f / 180.0f;
      texture.has_transform2d = tex_data.has_transform2d;
      texture.tx_rotation = tex_data.uv_rotation;
      texture.tx_scale = Float2(tex_data.uv_scale[0], tex_data.uv_scale[1]);
      texture.tx_translation = Float2(tex_data.uv_translation[0],
                                      tex_data.uv_translation[1]);
      texture.uv_primvar = tex_data.uv_primvar;

      out->texture_id = static_cast<int32_t>(scene->textures.size());
      scene->textures.push_back(std::move(texture));
      return true;
    }

    Value connected_value;
    if (ResolveConnectedValue(stage, connection_path, config_.time_code,
                              &connected_value) &&
        ValueToShaderParam(connected_value, out)) {
      UsdPrim color_prim = shader_prim;
      std::string color_property = attr_name;
      (void)ResolveConnectedColorSource(stage, connection_path,
                                        config_.time_code, &color_prim,
                                        &color_property);
      ConvertShaderColorToWorking(color_prim, color_property, param_name,
                                  scene, out);
      return true;
    }
  }

  ::lightusd::next::EvalOptions direct_opts = eval.GetOptions();
  direct_opts.follow_connections = false;
  ::lightusd::next::EvalResult direct =
      eval.EvalWith(shader_prim, attr_name, direct_opts);
  if (direct.success && ValueToShaderParam(direct.value, out)) {
    ConvertShaderColorToWorking(shader_prim, attr_name, param_name, scene,
                                out);
    return true;
  }

  ::lightusd::next::EvalResult followed = eval.Eval(shader_prim, attr_name);
  if (followed.success && ValueToShaderParam(followed.value, out)) {
    ConvertShaderColorToWorking(shader_prim, attr_name, param_name, scene,
                                out);
    return true;
  }

  return false;
}

} } }  // namespace lightusd::tydra::next
