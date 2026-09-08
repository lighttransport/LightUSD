// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Material and texture conversion routines split from render-data.cc
//
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

constexpr int32_t kWorkingColorValueTextureId = -2;

template <typename T>
bool ResolveTypedAnimatableValue(
    const Stage &stage,
    const TypedAttributeWithFallback<Animatable<T>> &attr,
    const std::string &attr_name,
    const double timecode,
    const value::TimeSampleInterpolationType tinterp,
    T *value_out,
    std::string *err) {
  return EvaluateTypedAnimatableAttribute(stage, attr, attr_name, value_out,
                                          err, timecode, tinterp);
}

template <typename T>
bool ResolveTypedAnimatableValue(
    const Stage &stage,
    const TypedAttribute<Animatable<T>> &attr,
    const std::string &attr_name,
    const double timecode,
    const value::TimeSampleInterpolationType tinterp,
    T *value_out,
    std::string *err) {
  return EvaluateTypedAnimatableAttribute(stage, attr, attr_name, value_out, err,
                                          timecode, tinterp);
}

template <typename Dty>
bool SetShaderParamFromInterfaceAttribute(const Attribute &attr,
                                          ShaderParam<Dty> *dst_param) {
  if (!dst_param) {
    return false;
  }

  if (auto v = attr.get_value<Dty>()) {
    dst_param->set_value(*v);
    return true;
  }

  if constexpr (std::is_same<Dty, float>::value) {
    if (auto v = attr.get_value<value::color3f>()) {
      dst_param->set_value((*v)[0]);
      return true;
    }
    if (auto v = attr.get_value<value::normal3f>()) {
      dst_param->set_value((*v)[0]);
      return true;
    }
    if (auto v = attr.get_value<value::float3>()) {
      dst_param->set_value((*v)[0]);
      return true;
    }
  } else if constexpr (std::is_same<Dty, value::color3f>::value) {
    if (auto v = attr.get_value<float>()) {
      value::color3f c;
      c[0] = *v;
      c[1] = *v;
      c[2] = *v;
      dst_param->set_value(c);
      return true;
    }
    if (auto v = attr.get_value<value::normal3f>()) {
      value::color3f c;
      c[0] = (*v)[0];
      c[1] = (*v)[1];
      c[2] = (*v)[2];
      dst_param->set_value(c);
      return true;
    }
    if (auto v = attr.get_value<value::float3>()) {
      value::color3f c;
      c[0] = (*v)[0];
      c[1] = (*v)[1];
      c[2] = (*v)[2];
      dst_param->set_value(c);
      return true;
    }
  } else if constexpr (std::is_same<Dty, value::float3>::value) {
    if (auto v = attr.get_value<float>()) {
      value::float3 c{{*v, *v, *v}};
      dst_param->set_value(c);
      return true;
    }
    if (auto v = attr.get_value<value::color3f>()) {
      value::float3 c{{(*v)[0], (*v)[1], (*v)[2]}};
      dst_param->set_value(c);
      return true;
    }
    if (auto v = attr.get_value<value::normal3f>()) {
      value::float3 c{{(*v)[0], (*v)[1], (*v)[2]}};
      dst_param->set_value(c);
      return true;
    }
  } else if constexpr (std::is_same<Dty, value::normal3f>::value) {
    if (auto v = attr.get_value<value::color3f>()) {
      value::normal3f n;
      n[0] = (*v)[0];
      n[1] = (*v)[1];
      n[2] = (*v)[2];
      dst_param->set_value(n);
      return true;
    }
    if (auto v = attr.get_value<value::float3>()) {
      value::normal3f n;
      n[0] = (*v)[0];
      n[1] = (*v)[1];
      n[2] = (*v)[2];
      dst_param->set_value(n);
      return true;
    }
  }

  return false;
}

template <typename EnumTy, typename EnumHandler>
bool ResolveEnumTokenAnimatableValue(
    const Stage &stage,
    const TypedAttributeWithFallback<Animatable<EnumTy>> &attr,
    const std::string &attr_name,
    EnumHandler enum_handler,
    const double timecode,
    const value::TimeSampleInterpolationType tinterp,
    EnumTy *value_out,
    std::string *err) {
  if (!value_out) {
    if (err) {
      (*err) += "`value_out` argument is nullptr.\n";
    }
    return false;
  }

  if (attr.has_connections()) {
    Attribute conn_attr;
    conn_attr.variability() = Variability::Varying;
    conn_attr.set_type_name(value::kToken);
    conn_attr.set_connections(attr.connections());

    TerminalAttributeValue resolved;
    if (!EvaluateAttribute(stage, conn_attr, attr_name, &resolved, err,
                           timecode, tinterp)) {
      return false;
    }

    std::string token_value;
    if (const auto *tok = resolved.as<value::token>()) {
      token_value = tok->str();
    } else if (const auto *str = resolved.as<std::string>()) {
      token_value = *str;
    } else {
      if (err) {
        (*err) += fmt::format(
            "Type mismatch. Value-producing attribute for `{}` has type `{}`, "
            "but `token` was expected.\n",
            attr_name, resolved.type_name());
      }
      return false;
    }

    auto parsed = enum_handler(token_value);
    if (!parsed) {
      if (err) {
        (*err) += fmt::format(
            "Failed to resolve `{}` from connected value `{}`: {}\n", attr_name,
            token_value, parsed.error());
      }
      return false;
    }

    *value_out = parsed.value();
    return true;
  }

  const auto &value = attr.get_value();
  if (value.get(timecode, value_out, tinterp)) {
    return true;
  }

  if (err) {
    (*err) += fmt::format("Failed to get `{}` at the requested time.\n",
                          attr_name);
  }
  return false;
}

bool ResolveSourceColorSpace(
    const Stage &stage,
    const TypedAttributeWithFallback<Animatable<UsdUVTexture::SourceColorSpace>>
        &sourceColorSpace,
    const double timecode,
    const value::TimeSampleInterpolationType tinterp,
    UsdUVTexture::SourceColorSpace *value_out,
    std::string *err) {
  return ResolveEnumTokenAnimatableValue(
      stage, sourceColorSpace, "inputs:sourceColorSpace",
      enum_handler::SourceColorSpace, timecode, tinterp, value_out, err);
}

bool ResolveTextureWrap(
    const Stage &stage,
    const TypedAttributeWithFallback<Animatable<UsdUVTexture::Wrap>> &wrap_attr,
    const std::string &attr_name,
    const double timecode,
    const value::TimeSampleInterpolationType tinterp,
    UsdUVTexture::Wrap *value_out,
    std::string *err) {
  return ResolveEnumTokenAnimatableValue(stage, wrap_attr, attr_name,
                                         enum_handler::TextureWrap, timecode,
                                         tinterp, value_out, err);
}

}  // namespace

bool RawAssetRead(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver,
    Asset *assetOut,
    std::string &resolvedPathOut,
    void *userdata, std::string *warn,
    std::string *err) {
  if (!assetOut) {
    if (err) {
      (*err) = "`assetOut` argument is nullptr\n";
    }
    return false;
  }

  // TODO: assetInfo
  (void)assetInfo;
  (void)userdata;
  (void)warn;

  std::string sanitized_path = utils::SanitizeAssetPath(
      assetPath.GetAssetPath(), assetResolver.get_allow_parent_relative_paths());
  if (sanitized_path.empty()) {
    if (err) {
      (*err) += fmt::format("Unsafe asset path: {}\n", assetPath.GetAssetPath());
    }
    return false;
  }

  std::string resolvedPath = assetResolver.resolve(sanitized_path);

  if (resolvedPath.empty()) {
    if (err) {
      (*err) += fmt::format("Failed to resolve asset path: {}\n",
                            assetPath.GetAssetPath());
    }
    return false;
  }

  Asset asset;
  bool ret = assetResolver.open_asset(resolvedPath, sanitized_path,
                                      &asset, warn, err);
  if (!ret) {
    if (err) {
      (*err) += fmt::format("Failed to open asset: {}", resolvedPath);
    }
    return false;
  }

  if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
    if (err) {
      (*err) += fmt::format("Resolved asset exceeds max bytes ({} > {}).",
                            asset.size(), security_policy::GetMaxAssetReadBytes());
    }
    return false;
  }

  DCOUT("Resolved asset path = " << resolvedPath);

  resolvedPathOut = resolvedPath;
  (*assetOut) = std::move(asset);

  return true;
}

struct UVConnectionResolveCacheEntry {
  bool found{false};
  Path tex_abs_path;
  const UsdUVTexture *texture{nullptr};
  const Shader *shader{nullptr};
};

struct MtlxTexcoordTransform {
  value::float2 scale;
  value::float2 translation;
  float rotation{0.0f};
  bool has_transform{false};

  MtlxTexcoordTransform() {
    scale[0] = 1.0f;
    scale[1] = 1.0f;
    translation[0] = 0.0f;
    translation[1] = 0.0f;
  }
};

struct MtlxConnectionResolveCacheEntry {
  Path tex_abs_path;
  const Shader *image_shader{nullptr};
  std::string st_varname;
  const AssetInfo *asset_info{nullptr};
  UVTexture::Channel output_channel{UVTexture::Channel::RGB};
  MtlxTexcoordTransform texcoord_transform;
};

struct ConnectionResolveCache {
  const Stage *stage{nullptr};
  lightusd::HashMap<std::string, UVConnectionResolveCacheEntry,
                    FNV1StringHash>
      uv_texture_by_connection;
  lightusd::HashMap<std::string, MtlxConnectionResolveCacheEntry,
                    FNV1StringHash>
      mtlx_texture_by_connection;
};

static ConnectionResolveCache &GetConnectionResolveCache(const Stage &stage) {
  static thread_local ConnectionResolveCache cache;
  if (cache.stage != &stage) {
    cache.stage = &stage;
    cache.uv_texture_by_connection.clear();
    cache.mtlx_texture_by_connection.clear();
  }
  return cache;
}

void ResetConnectionResolveCache(const Stage &stage) {
  ConnectionResolveCache &cache = GetConnectionResolveCache(stage);
  // Swap with empty maps to release bucket memory (clear() keeps capacity)
  {
    lightusd::HashMap<std::string, UVConnectionResolveCacheEntry, FNV1StringHash> tmp;
    cache.uv_texture_by_connection.swap(tmp);
  }
  {
    lightusd::HashMap<std::string, MtlxConnectionResolveCacheEntry, FNV1StringHash> tmp;
    cache.mtlx_texture_by_connection.swap(tmp);
  }
}

namespace {

/// Try to read array attribute directly from mmap. Returns true if successful.

void ApplyTexTransform2d(float rotation, const value::float2 &scale,
                         const value::float2 &translation,
                         UVTexture *tex_out) {
  if (!tex_out) {
    return;
  }

  // Build transform matrix.
  // https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform
  // Since USD uses post-multiply,
  //
  // matrix = scale * rotate * translate
  //
  mat3 s;
  s.set_scale(scale[0], scale[1], 1.0f);

  mat3 r = mat3::identity();

  r.m[0][0] = std::cos(math::radian(rotation));
  r.m[0][1] = std::sin(math::radian(rotation));

  r.m[1][0] = -std::sin(math::radian(rotation));
  r.m[1][1] = std::cos(math::radian(rotation));

  mat3 t = mat3::identity();
  t.set_translation(translation[0], translation[1], 1.0f);

  tex_out->transform = s * r * t;
  tex_out->tx_rotation = rotation;
  tex_out->tx_translation = translation;
  tex_out->tx_scale = scale;
  tex_out->has_transform2d = true;
}

// Convert UsdTransform2d -> PrimvarReader_float2 shader network.
nonstd::expected<bool, std::string> ConvertTexTransform2d(
    const Stage &stage, const Path &tx_abs_path, const UsdTransform2d &tx,
    UVTexture *tex_out, double timecode) {
  float rotation;  // in angles
  std::string resolve_err;
  if (!ResolveTypedAnimatableValue(stage, tx.rotation, "inputs:rotation",
                                   timecode,
                                   value::TimeSampleInterpolationType::Held,
                                   &rotation, &resolve_err)) {
    return nonstd::make_unexpected(
        fmt::format("Failed to resolve rotation attribute from {}: {}",
                    tx_abs_path.full_path_name(), resolve_err));
  }

  value::float2 scale;
  resolve_err.clear();
  if (!ResolveTypedAnimatableValue(stage, tx.scale, "inputs:scale", timecode,
                                   value::TimeSampleInterpolationType::Held,
                                   &scale, &resolve_err)) {
    return nonstd::make_unexpected(
        fmt::format("Failed to resolve scale attribute from {}: {}",
                    tx_abs_path.full_path_name(), resolve_err));
  }

  value::float2 translation;
  resolve_err.clear();
  if (!ResolveTypedAnimatableValue(stage, tx.translation, "inputs:translation",
                                   timecode,
                                   value::TimeSampleInterpolationType::Held,
                                   &translation, &resolve_err)) {
    return nonstd::make_unexpected(
        fmt::format("Failed to resolve translation attribute from {}: {}",
                    tx_abs_path.full_path_name(), resolve_err));
  }

  // must be authored and connected to PrimvarReader.
  if (!tx.in.authored()) {
    return nonstd::make_unexpected("`inputs:in` must be authored.\n");
  }

  if (tx.in.get_connections().empty()) {
    // Accept a connection even if a fallback value is also present
    // (is_connection() is false then).
    return nonstd::make_unexpected("`inputs:in` must be a connection.\n");
  }

  const auto &paths = tx.in.get_connections();
  if (paths.size() != 1) {
    return nonstd::make_unexpected(
        "`inputs:in` must be a single connection Path.\n");
  }

  std::string prim_part = paths[0].prim_part();
  std::string prop_part = paths[0].prop_part();

  if (prop_part != "outputs:result") {
    return nonstd::make_unexpected(
        "`inputs:in` connection Path's property part must be "
        "`outputs:result`\n");
  }

  std::string err;

  const Prim *pprim{nullptr};
  if (!stage.find_prim_at_path(Path(prim_part, ""), pprim, &err)) {
    return nonstd::make_unexpected(fmt::format(
        "`inputs:in` connection Path not found in the Stage. {}\n", prim_part));
  }

  if (!pprim) {
    return nonstd::make_unexpected(
        fmt::format("[InternalError] Prim is nullptr: {}\n", prim_part));
  }

  const Shader *pshader = pprim->as<Shader>();
  if (!pshader) {
    return nonstd::make_unexpected(
        fmt::format("{} must be Shader Prim, but got {}\n", prim_part,
                    pprim->prim_type_name()));
  }

  const UsdPrimvarReader_float2 *preader =
      pshader->value.as<UsdPrimvarReader_float2>();
  if (!preader) {
    return nonstd::make_unexpected(fmt::format(
        "Shader {} must be UsdPrimvarReader_float2 type, but got {}(internal type {})\n",
        prim_part, pshader->info_id, pshader->value.type_name()));
  }

  // Get value producing attribute(i.e, follow .connection and return
  // terminal Attribute value)
  //value::token varname;

  // 'string' for inputs:varname preferred.
  std::string varname;
  TerminalAttributeValue attr;
  if (!tydra::EvaluateAttribute(stage, *pprim, "inputs:varname", &attr, &err)) {
    return nonstd::make_unexpected(
        "`inputs:varname` evaluation failed: " + err + "\n");
  }
  if (auto pvt = attr.as<value::token>()) {
    varname = pvt->str();
  } else if (auto pvs = attr.as<std::string>()) {
    varname = *pvs;
  } else if (auto pvsd = attr.as<value::StringData>()) {
    varname = (*pvsd).value;
  } else {
    return nonstd::make_unexpected(
        "`inputs:varname` must be `token` or `string` type, but got " + attr.type_name() +
        "\n");
  }
  if (varname.empty()) {
    return nonstd::make_unexpected("`inputs:varname` is empty token\n");
  }
  DCOUT("inputs:varname = " << varname);

  ApplyTexTransform2d(rotation, scale, translation, tex_out);
  tex_out->varname_uv = varname;

  return true;
}

nonstd::expected<bool, std::string> GetConnectedUVTexture(
    const Stage &stage, const std::vector<Path> &connections,
    Path *tex_abs_path, const UsdUVTexture **dst, const Shader **shader_out) {
  if (!dst) {
    return nonstd::make_unexpected("[InternalError] dst is nullptr.\n");
  }

  if (connections.empty()) {
    // Accept an input that has a connection even if a fallback value is also
    // authored (is_connection() is false then). In USD a connection overrides
    // the fallback value.
    return nonstd::make_unexpected("Attribute must be connection.\n");
  }

  if (connections.size() != 1) {
    return nonstd::make_unexpected(
        "Attribute connections must be single connection Path.\n");
  }

  //
  // Example: color3f inputs:diffuseColor.connect = </path/to/tex.outputs:rgb>
  //
  // => path.prim_part : /path/to/tex
  // => path.prop_part : outputs:rgb
  //

  const Path &path = connections[0];

  const std::string prim_part = path.prim_part();
  const std::string prop_part = path.prop_part();
  const std::string cache_key = path.full_path_name();
  ConnectionResolveCache &resolve_cache = GetConnectionResolveCache(stage);

  if (shader_out) {
    *shader_out = nullptr;
  }
  *dst = nullptr;

  auto cache_it = resolve_cache.uv_texture_by_connection.find(cache_key);
  if (cache_it != resolve_cache.uv_texture_by_connection.end()) {
    if (tex_abs_path) {
      *tex_abs_path = cache_it->second.tex_abs_path;
    }
    *dst = cache_it->second.texture;
    if (shader_out) {
      *shader_out = cache_it->second.shader;
    }
    return cache_it->second.found;
  }

  auto cache_result = [&](bool found, const Path &resolved_path,
                          const UsdUVTexture *texture,
                          const Shader *shader) {
    UVConnectionResolveCacheEntry entry;
    entry.found = found;
    entry.tex_abs_path = resolved_path;
    entry.texture = texture;
    entry.shader = shader;
    resolve_cache.uv_texture_by_connection[cache_key] = std::move(entry);
  };

  // NOTE: no `outputs:rgba` in the spec.
  constexpr auto kOutputsRGB = "outputs:rgb";
  constexpr auto kOutputsR = "outputs:r";
  constexpr auto kOutputsG = "outputs:g";
  constexpr auto kOutputsB = "outputs:b";
  constexpr auto kOutputsA = "outputs:a";

  // Per-texture trace: keep at debug level. At info level this floods the
  // console for material-heavy scenes (one line per texture connection, on
  // every conversion).
  LIGHTUSD_LOG_D("path: " << path);

  // Check if prop_part is a standard UsdUVTexture output
  bool is_standard_output = (prop_part == kOutputsRGB) ||
                            (prop_part == kOutputsR) ||
                            (prop_part == kOutputsG) ||
                            (prop_part == kOutputsB) ||
                            (prop_part == kOutputsA);

  const Prim *prim{nullptr};
  std::string err;
  bool found_in_stage = stage.find_prim_at_path(Path(prim_part, ""), prim, &err);

  // If not found in stage lookup, try to navigate through Material's children
  // This handles the case where NodeGraph is a child of Material but not in the Stage index
  if (!found_in_stage || !prim) {
    DCOUT("Prim not found in stage lookup, trying Material children approach");

    // Extract Material path - it should be everything before the last element
    size_t last_slash = prim_part.rfind('/');
    if (last_slash == std::string::npos) {
      return nonstd::make_unexpected(
          fmt::format("Prim {} not found in the Stage: {}\n", prim_part, err));
    }

    std::string material_path = prim_part.substr(0, last_slash);
    std::string child_name = prim_part.substr(last_slash + 1);

    DCOUT("Looking for Material at: " << material_path);
    DCOUT("Child name: " << child_name);

    // Find the Material
    const Prim *mat_prim{nullptr};
    if (!stage.find_prim_at_path(Path(material_path, ""), mat_prim, &err)) {
      return nonstd::make_unexpected(
          fmt::format("Prim {} not found (material lookup also failed): {}\n", prim_part, err));
    }

    // Look for child prim
    if (mat_prim) {
      std::string children_info = "Material has " + std::to_string(mat_prim->children().size()) + " children: ";
      for (const auto& child : mat_prim->children()) {
        std::string elem_name = child.element_name();
        std::string child_type = child.data().type_name();
        children_info += "'" + elem_name + "'(" + child_type + ") ";

        // Check by name match
        if (elem_name == child_name) {
          prim = &child;
          break;
        }
        // Also check if it's a NodeGraph/Shader by type name
        // This handles cases where element_name might not be set properly
        // e.g., looking for "NodeGraphs" and finding type "NodeGraph" with empty name
        if (child_type == "NodeGraph" && (child_name == "NodeGraphs" || child_name == "NodeGraph")) {
          prim = &child;
          break;
        }
        if (child_type == "Shader" && child_name == "Shader") {
          prim = &child;
          break;
        }
      }

      if (!prim) {
        DCOUT(children_info);
        return nonstd::make_unexpected(
            fmt::format("Child prim '{}' not found in Material {}. {}\n", child_name, material_path, children_info));
      }
    } else {
      return nonstd::make_unexpected(
          fmt::format("Material prim {} is null\n", material_path));
    }
  }

  if (!prim) {
    return nonstd::make_unexpected("[InternalError] Prim ptr is null.\n");
  }

  // Check if this is a NodeGraph - if so, we need to traverse through it
  if (const NodeGraph *ng = prim->as<NodeGraph>()) {
    DCOUT("Connection goes through NodeGraph: " << prim_part);

    // Look for the output property in the NodeGraph's props
    const auto &props = ng->props;
    auto it = props.find(prop_part);
    if (it == props.end()) {
      return nonstd::make_unexpected(
          fmt::format("NodeGraph {} does not have output property {}", prim_part, prop_part));
    }

    const Property &output_prop = it->second;
    if (!output_prop.is_attribute()) {
      return nonstd::make_unexpected(
          fmt::format("NodeGraph output {} is not an attribute", prop_part));
    }

    const Attribute &output_attr = output_prop.get_attribute();
    if (!output_attr.has_connections()) {
      return nonstd::make_unexpected(
          fmt::format("NodeGraph output {} has no connections", prop_part));
    }

    // Get the connection from the NodeGraph output
    const auto &output_conns = output_attr.connections();
    if (output_conns.size() != 1) {
      return nonstd::make_unexpected(
          fmt::format("NodeGraph output {} must have exactly one connection, got {}",
                      prop_part, output_conns.size()));
    }

    const Path &ng_output_path = output_conns[0];
    DCOUT("NodeGraph output connects to: " << ng_output_path);

    // Recursively follow the connection through the NodeGraph
    // We need to traverse to the next node in the chain
    std::string next_prim_part = ng_output_path.prim_part();
    std::string next_prop_part = ng_output_path.prop_part();

    // Find the next prim in the chain
    // It might be a child of the NodeGraph, so use the same child lookup logic
    const Prim *next_prim{nullptr};
    bool found_next = stage.find_prim_at_path(Path(next_prim_part, ""), next_prim, &err);

    // If not found in stage, it might be a child of the current NodeGraph
    if (!found_next || !next_prim) {
      DCOUT("Next prim not found in stage, checking NodeGraph children");

      // Check if it's a child of this NodeGraph
      size_t last_slash = next_prim_part.rfind('/');
      if (last_slash != std::string::npos) {
        std::string parent_path = next_prim_part.substr(0, last_slash);
        std::string child_name = next_prim_part.substr(last_slash + 1);

        // If the parent is this NodeGraph, look in its children
        if (parent_path == prim_part) {
          for (const auto& child : prim->children()) {
            std::string elem_name = child.element_name();
            if (elem_name == child_name) {
              next_prim = &child;
              break;
            }
          }

          if (!next_prim) {
            return nonstd::make_unexpected(
                fmt::format("Child prim '{}' not found in NodeGraph {}\n", child_name, prim_part));
          }
        } else {
          return nonstd::make_unexpected(
              fmt::format("Prim {} not found in the Stage: {}\n", next_prim_part, err));
        }
      } else {
        return nonstd::make_unexpected(
            fmt::format("Prim {} not found in the Stage: {}\n", next_prim_part, err));
      }
    }

    if (!next_prim) {
      return nonstd::make_unexpected("[InternalError] next_prim is null.\n");
    }

    // For nested NodeGraphs or other intermediate nodes, we would need to continue traversing
    // For now, we only support the common pattern: NodeGraph -> Shader(UsdUVTexture)
    // Nested NodeGraphs are rare and can be handled if needed

    // Check if it's a Shader with UsdUVTexture
    if (const Shader *pshader = next_prim->as<Shader>()) {
      if (const UsdUVTexture *ptex = pshader->value.as<UsdUVTexture>()) {
        // Verify the property part is valid for UsdUVTexture
        if (next_prop_part != kOutputsRGB && next_prop_part != kOutputsR &&
            next_prop_part != kOutputsG && next_prop_part != kOutputsB &&
            next_prop_part != kOutputsA) {
          return nonstd::make_unexpected(fmt::format(
              "UsdUVTexture connection property part must be outputs:rgb/r/g/b/a, got {}",
              next_prop_part));
        }

        DCOUT("Found UsdUVTexture through NodeGraph: " << next_prim_part);
        (*dst) = ptex;

        if (shader_out) {
          (*shader_out) = pshader;
        }

        if (tex_abs_path) {
          (*tex_abs_path) = ng_output_path;
        }

        cache_result(true, ng_output_path, ptex, pshader);
        return true;
      }
      // Shader exists but it's not a UsdUVTexture - this is OK, NodeGraph might connect to other shader types
      // Return false (not found) rather than error
      DCOUT(fmt::format("NodeGraph {} output {} connects to Shader {} but it's not UsdUVTexture",
                        prim_part, prop_part, next_prim_part));
      cache_result(false, ng_output_path, nullptr, pshader);
      return false;
    }

    // If we get here, the NodeGraph doesn't connect to a UsdUVTexture
    // This is not necessarily an error - the connection might be to a MaterialX shader or other node type
    DCOUT(fmt::format("NodeGraph {} output {} connects to {} (type: {}), not a UsdUVTexture",
                      prim_part, prop_part, next_prim_part, next_prim->prim_type_name()));
    cache_result(false, ng_output_path, nullptr, nullptr);
    return false;
  }

  // Not a NodeGraph - must be a direct UsdUVTexture connection
  if (!is_standard_output) {
    return nonstd::make_unexpected(fmt::format(
        "connection Path's property part must be `{}`, `{}`, `{}`, `{}` or `{}` "
        "for UsdUVTexture, but got `{}`(prim_part: {}).",
        kOutputsRGB, kOutputsR, kOutputsG, kOutputsB, kOutputsA, prop_part, prim_part));
  }

  if (tex_abs_path) {
    (*tex_abs_path) = Path(prim_part, prop_part);
  }

  if (const Shader *pshader = prim->as<Shader>()) {
    if (const UsdUVTexture *ptex = pshader->value.as<UsdUVTexture>()) {
      DCOUT("ptex = " << ptex);
      (*dst) = ptex;

      if (shader_out) {
        (*shader_out) = pshader;
      }

      cache_result(true, Path(prim_part, prop_part), ptex, pshader);
      return true;
    }
  }

  return nonstd::make_unexpected(
      fmt::format("Prim {} must be `Shader` Prim type, but got `{}`", prim_part,
                  prim->prim_type_name()));
}

// Helper function to find ND_image_color4 texture nodes in a MaterialX NodeGraph
// by traversing connections from the given output
static UVTexture::Channel MaterialXOutputChannelFromPath(const Path &path) {
  std::string prop = path.prop_part();
  if (startsWith(prop, "outputs:")) {
    prop = prop.substr(8);
  }
  if (prop == "r" || prop == "x") return UVTexture::Channel::R;
  if (prop == "g" || prop == "y") return UVTexture::Channel::G;
  if (prop == "b" || prop == "z") return UVTexture::Channel::B;
  if (prop == "a" || prop == "w") return UVTexture::Channel::A;
  if (prop == "rgba" || prop == "outcolor4") return UVTexture::Channel::RGBA;
  return UVTexture::Channel::RGB;
}

static std::string MtlxDefaultTexcoordName(const std::string &default_name) {
  return default_name.empty() ? "st" : default_name;
}

static std::string MtlxTexcoordIndexName(const std::string &default_name,
                                         int index) {
  const std::string base = MtlxDefaultTexcoordName(default_name);
  if (index <= 0) {
    return std::string(base);
  }
  if (base == "st") {
    return "st" + std::to_string(index);
  }
  return base + std::to_string(index);
}

static bool MtlxPropertyConnection(const Property &prop, Path *path_out) {
  if (prop.is_attribute()) {
    const Attribute &attr = prop.get_attribute();
    if (attr.has_connections() && !attr.connections().empty()) {
      if (path_out) {
        *path_out = attr.connections()[0];
      }
      return true;
    }
  } else if (prop.is_relationship()) {
    const std::vector<Path> targets = prop.get_relationTargets();
    if (!targets.empty()) {
      if (path_out) {
        *path_out = targets[0];
      }
      return true;
    }
  }
  return false;
}

static bool FindMtlxNamedConnection(
    const std::map<std::string, Property> &props, const std::string &name,
    Path *path_out) {
  auto it = props.find(name);
  if (it != props.end() && MtlxPropertyConnection(it->second, path_out)) {
    return true;
  }
  it = props.find(name + ".connect");
  if (it != props.end() && MtlxPropertyConnection(it->second, path_out)) {
    return true;
  }
  return false;
}

static bool FindMtlxAnyInputConnection(
    const std::map<std::string, Property> &props, Path *path_out) {
  for (const auto &prop : props) {
    if (!startsWith(prop.first, "inputs:")) {
      continue;
    }
    if (MtlxPropertyConnection(prop.second, path_out)) {
      return true;
    }
  }
  return false;
}

static bool FindMtlxShaderConnection(const Shader &shader,
                                     const std::string &name,
                                     Path *path_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node &&
      FindMtlxNamedConnection(shader_node->props, name, path_out)) {
    return true;
  }
  return FindMtlxNamedConnection(shader.props, name, path_out);
}

static bool FindMtlxShaderAnyInputConnection(const Shader &shader,
                                             Path *path_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node && FindMtlxAnyInputConnection(shader_node->props, path_out)) {
    return true;
  }
  return FindMtlxAnyInputConnection(shader.props, path_out);
}

static bool FindMtlxStringInput(const std::map<std::string, Property> &props,
                                const std::string &name,
                                std::string *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (!attr.has_value()) {
    return false;
  }
  if (auto s = attr.get_value<std::string>()) {
    if (value_out) {
      *value_out = *s;
    }
    return true;
  }
  if (auto t = attr.get_value<value::token>()) {
    if (value_out) {
      *value_out = t->str();
    }
    return true;
  }
  return false;
}

static bool FindMtlxShaderStringInput(const Shader &shader,
                                      const std::string &name,
                                      std::string *value_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node &&
      FindMtlxStringInput(shader_node->props, name, value_out)) {
    return true;
  }
  return FindMtlxStringInput(shader.props, name, value_out);
}

static bool FindMtlxIntInput(const std::map<std::string, Property> &props,
                             const std::string &name, int *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (!attr.has_value()) {
    return false;
  }
  if (auto i = attr.get_value<int>()) {
    if (value_out) {
      *value_out = *i;
    }
    return true;
  }
  return false;
}

static bool FindMtlxShaderIntInput(const Shader &shader,
                                   const std::string &name,
                                   int *value_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node && FindMtlxIntInput(shader_node->props, name, value_out)) {
    return true;
  }
  return FindMtlxIntInput(shader.props, name, value_out);
}

static bool FindMtlxFloat2Input(const std::map<std::string, Property> &props,
                                const std::string &name,
                                value::float2 *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (!attr.has_value()) {
    return false;
  }
  if (auto v = attr.get_value<value::float2>()) {
    if (value_out) {
      *value_out = *v;
    }
    return true;
  }
  return false;
}

static bool FindMtlxShaderFloat2Input(const Shader &shader,
                                      const std::string &name,
                                      value::float2 *value_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node && FindMtlxFloat2Input(shader_node->props, name, value_out)) {
    return true;
  }
  return FindMtlxFloat2Input(shader.props, name, value_out);
}

static bool FindMtlxFloatInput(const std::map<std::string, Property> &props,
                               const std::string &name, float *value_out) {
  auto it = props.find(name);
  if (it == props.end() || !it->second.is_attribute()) {
    return false;
  }
  const Attribute &attr = it->second.get_attribute();
  if (!attr.has_value()) {
    return false;
  }
  if (auto v = attr.get_value<float>()) {
    if (value_out) {
      *value_out = *v;
    }
    return true;
  }
  return false;
}

static bool FindMtlxShaderFloatInput(const Shader &shader,
                                     const std::string &name,
                                     float *value_out) {
  const ShaderNode *shader_node = shader.value.as<ShaderNode>();
  if (shader_node && FindMtlxFloatInput(shader_node->props, name, value_out)) {
    return true;
  }
  return FindMtlxFloatInput(shader.props, name, value_out);
}

static void ExtractMtlxShaderUvTransform(const Shader &shader,
                                         MtlxTexcoordTransform *transform) {
  if (!transform) {
    return;
  }

  value::float2 v;
  if (FindMtlxShaderFloat2Input(shader, "inputs:uvtiling", &v) ||
      FindMtlxShaderFloat2Input(shader, "inputs:scale", &v)) {
    transform->scale = v;
    transform->has_transform = true;
  }
  if (FindMtlxShaderFloat2Input(shader, "inputs:uvoffset", &v) ||
      FindMtlxShaderFloat2Input(shader, "inputs:translation", &v)) {
    transform->translation = v;
    transform->has_transform = true;
  }
  float f = 0.0f;
  if (FindMtlxShaderFloatInput(shader, "inputs:rotate", &f) ||
      FindMtlxShaderFloatInput(shader, "inputs:rotation", &f)) {
    transform->rotation = f;
    transform->has_transform = true;
  }
}

static const Prim *FindMtlxNodePrim(const Stage &stage, const Prim *ng_prim,
                                    const std::string &prim_path,
                                    std::string *err) {
  const Prim *prim{nullptr};
  if (stage.find_prim_at_path(Path(prim_path, ""), prim, err) && prim) {
    return prim;
  }
  if (!ng_prim) {
    return nullptr;
  }

  const size_t last_slash = prim_path.rfind('/');
  if (last_slash == std::string::npos) {
    return nullptr;
  }
  const std::string parent_path = prim_path.substr(0, last_slash);
  const std::string child_name = prim_path.substr(last_slash + 1);
  if (parent_path.find("NodeGraphs") == std::string::npos) {
    return nullptr;
  }
  for (const auto &child : ng_prim->children()) {
    if (child.element_name() == child_name) {
      return &child;
    }
  }
  return nullptr;
}

static std::string ResolveMtlxTexcoordVarname(
    const Stage &stage, const Shader &image_shader, const Path &image_path,
    const Prim *ng_prim, const std::string &default_texcoords_primvar_name,
    MtlxTexcoordTransform *transform_out) {
  (void)image_path;
  const std::string fallback =
      MtlxDefaultTexcoordName(default_texcoords_primvar_name);
  MtlxTexcoordTransform transform;

  Path current_path;
  if (!FindMtlxShaderConnection(image_shader, "inputs:texcoord",
                                &current_path)) {
    if (transform_out) {
      *transform_out = transform;
    }
    return std::string(fallback);
  }

  std::string err;
  int max_depth = 10;
  while (max_depth-- > 0) {
    const Prim *prim =
        FindMtlxNodePrim(stage, ng_prim, current_path.prim_part(), &err);
    const Shader *shader = prim ? prim->as<Shader>() : nullptr;
    if (!shader) {
      if (transform_out) {
        *transform_out = transform;
      }
      return std::string(fallback);
    }

    if (startsWith(shader->info_id, "ND_geompropvalue_")) {
      std::string geomprop;
      if (FindMtlxShaderStringInput(*shader, "inputs:geomprop", &geomprop) &&
          !geomprop.empty()) {
        if (transform_out) {
          *transform_out = transform;
        }
        return std::string(geomprop);
      }
      if (transform_out) {
        *transform_out = transform;
      }
      return std::string(fallback);
    }

    if (shader->info_id == "ND_texcoord_vector2" ||
        shader->info_id == "ND_texcoord_vector3") {
      int index = 0;
      FindMtlxShaderIntInput(*shader, "inputs:index", &index);
      if (transform_out) {
        *transform_out = transform;
      }
      return MtlxTexcoordIndexName(default_texcoords_primvar_name, index);
    }

    if (startsWith(shader->info_id, "ND_place2d_")) {
      ExtractMtlxShaderUvTransform(*shader, &transform);
      if (FindMtlxShaderConnection(*shader, "inputs:texcoord",
                                   &current_path)) {
        continue;
      }
      if (transform_out) {
        *transform_out = transform;
      }
      return std::string(fallback);
    }

    if (FindMtlxShaderConnection(*shader, "inputs:in", &current_path) ||
        FindMtlxShaderConnection(*shader, "inputs:in1", &current_path) ||
        FindMtlxShaderAnyInputConnection(*shader, &current_path)) {
      continue;
    }
    if (transform_out) {
      *transform_out = transform;
    }
    return std::string(fallback);
  }

  DCOUT("MaterialX texcoord chain exceeded max depth for "
        << image_path.full_path_name());
  if (transform_out) {
    *transform_out = transform;
  }
  return std::string(fallback);
}

nonstd::expected<bool, std::string> GetConnectedMtlxTexture(
    const Stage &stage, const std::vector<Path> &connections,
    Path *tex_abs_path, const Shader **image_shader_out,
    std::string *st_varname_out, const AssetInfo **assetInfo_out,
    UVTexture::Channel *output_channel_out,
    MtlxTexcoordTransform *texcoord_transform_out,
    const std::string &default_texcoords_primvar_name = "st") {

  if (connections.empty()) {
    // Accept an input that has a connection even if a fallback value is also
    // authored (is_connection() is false then). In USD a connection overrides
    // the fallback value.
    return nonstd::make_unexpected("Attribute must be connection.\n");
  }

  if (connections.size() != 1) {
    return nonstd::make_unexpected(
        "Attribute connections must be single connection Path.\n");
  }

  const Path &path = connections[0];
  const std::string prim_part = path.prim_part();
  const std::string prop_part = path.prop_part();
  const std::string cache_key =
      path.full_path_name() + "|" + default_texcoords_primvar_name;
  ConnectionResolveCache &resolve_cache = GetConnectionResolveCache(stage);

  auto mtlx_cache_it =
      resolve_cache.mtlx_texture_by_connection.find(cache_key);
  if (mtlx_cache_it != resolve_cache.mtlx_texture_by_connection.end()) {
    if (tex_abs_path) {
      *tex_abs_path = mtlx_cache_it->second.tex_abs_path;
    }
    if (image_shader_out) {
      *image_shader_out = mtlx_cache_it->second.image_shader;
    }
    if (st_varname_out) {
      *st_varname_out = mtlx_cache_it->second.st_varname;
    }
    if (assetInfo_out) {
      *assetInfo_out = mtlx_cache_it->second.asset_info;
    }
    if (output_channel_out) {
      *output_channel_out = mtlx_cache_it->second.output_channel;
    }
    if (texcoord_transform_out) {
      *texcoord_transform_out = mtlx_cache_it->second.texcoord_transform;
    }
    return true;
  }

  auto cache_result = [&](const Path &resolved_path, const Shader *image_shader,
                          const std::string &st_varname,
                          const AssetInfo *asset_info,
                          UVTexture::Channel output_channel,
                          const MtlxTexcoordTransform &texcoord_transform) {
    MtlxConnectionResolveCacheEntry entry;
    entry.tex_abs_path = resolved_path;
    entry.image_shader = image_shader;
    entry.st_varname = st_varname;
    entry.asset_info = asset_info;
    entry.output_channel = output_channel;
    entry.texcoord_transform = texcoord_transform;
    resolve_cache.mtlx_texture_by_connection[cache_key] = std::move(entry);
  };

  DCOUT("Checking MaterialX connection: " << path.full_path_name());
  DCOUT("  prim_part: " << prim_part);
  DCOUT("  prop_part: " << prop_part);

  // The prim_part should be the NodeGraph path itself
  // For </root/_materials/Material/NodeGraphs.outputs:node_out>,
  // prim_part = "/root/_materials/Material/NodeGraphs"

  // First, try to find via stage lookup
  const Prim *ng_prim{nullptr};
  std::string err;
  bool found_in_stage = stage.find_prim_at_path(Path(prim_part, ""), ng_prim, &err);

  // If not found in stage lookup, try to navigate through Material's children
  if (!found_in_stage || !ng_prim) {
    DCOUT("Prim not found in stage lookup, trying Material children approach");

    // Extract Material path - it should be everything before the last element
    size_t last_slash = prim_part.rfind('/');
    if (last_slash == std::string::npos) {
      return nonstd::make_unexpected(
          fmt::format("Invalid NodeGraph path structure: {}\n", prim_part));
    }

    std::string material_path = prim_part.substr(0, last_slash);
    std::string nodegraph_name = prim_part.substr(last_slash + 1);

    DCOUT("Looking for Material at: " << material_path);
    DCOUT("NodeGraph name: " << nodegraph_name);

    // Find the Material
    const Prim *mat_prim{nullptr};
    if (!stage.find_prim_at_path(Path(material_path, ""), mat_prim, &err)) {
      return nonstd::make_unexpected(
          fmt::format("Material {} not found: {}\n", material_path, err));
    }

    // Look for NodeGraph child
    if (mat_prim) {
      std::string children_info = "Material has " + std::to_string(mat_prim->children().size()) + " children: ";
      for (const auto& child : mat_prim->children()) {
        std::string child_name = child.element_name();
        std::string child_type = child.data().type_name();
        children_info += "'" + child_name + "'(" + child_type + ") ";

        // Check if this is a NodeGraph (by type, since name might be empty)
        if (child_type == "NodeGraph") {
          // If the child has no name but is the right type, use it
          // This handles the case where the NodeGraph doesn't have element_name set
          ng_prim = &child;
          break;
        } else if (child_name == nodegraph_name) {
          // Also check by exact name match
          ng_prim = &child;
          break;
        }
      }

      if (!ng_prim) {
        return nonstd::make_unexpected(
            fmt::format("NodeGraph '{}' not found. {}\n", nodegraph_name, children_info));
      }
    } else {
      return nonstd::make_unexpected(
          fmt::format("Material prim is null\n"));
    }
  }

  DCOUT("Found prim: " << prim_part << ", type: " << (ng_prim ? ng_prim->data().type_name() : "null"));

  const NodeGraph *ng = nullptr;
  Path current_path;
  if (ng_prim && ng_prim->as<Shader>()) {
    // MaterialX node networks can connect an input directly to a sibling
    // Shader output, e.g. </Mat/mtlximage1.outputs:out>, without routing
    // through a NodeGraph terminal.
    current_path = path;
  } else {
    ng = ng_prim ? ng_prim->as<NodeGraph>() : nullptr;
    if (!ng) {
      // Debug output to understand why it's not a NodeGraph
      if (ng_prim) {
        return nonstd::make_unexpected(fmt::format(
            "{} is not a NodeGraph, prim_type: {}\n", prim_part,
            ng_prim->data().type_name()));
      }
      return nonstd::make_unexpected(
          fmt::format("{} is not a NodeGraph\n", prim_part));
    }

    // Find the output connection we're looking for
    // The prop_part should be like "outputs:node_out"
    std::string output_name = prop_part;
    if (startsWith(output_name, "outputs:")) {
      output_name = output_name.substr(8); // Remove "outputs:" prefix
    }

    // Look for the connection in props
    // Try both with and without ".connect" suffix
    std::string conn_prop_name = "outputs:" + output_name + ".connect";
    auto it = ng->props.find(conn_prop_name);

    if (it == ng->props.end()) {
      // Try without .connect suffix
      conn_prop_name = "outputs:" + output_name;
      it = ng->props.find(conn_prop_name);

      if (it == ng->props.end()) {
        // List available props for debugging
        std::string available_props = "Available props: ";
        for (const auto &prop : ng->props) {
          available_props += prop.first + " ";
        }
        return nonstd::make_unexpected(fmt::format(
            "Output connection '{}' not found in NodeGraph. {}\n",
            conn_prop_name, available_props));
      }
    }

    // NodeGraph outputs can be stored as attributes or relationships
    bool found_connection = false;

    if (it->second.is_attribute()) {
      // It's an attribute - look for connections on the attribute
      const Attribute &attr = it->second.get_attribute();
      if (attr.has_connections() && !attr.connections().empty()) {
        current_path = attr.connections()[0];
        found_connection = true;
      }
    } else if (it->second.is_relationship()) {
      // Also support relationship format
      auto targets = it->second.get_relationTargets();
      if (!targets.empty()) {
        current_path = targets[0];
        found_connection = true;
      }
    }

    if (!found_connection) {
      return nonstd::make_unexpected(
          fmt::format("Output {} has no connection targets\n", conn_prop_name));
    }
  }
  const Shader *image_shader = nullptr;

  // Traverse the node connections to find ND_image_color4
  // Maximum depth to prevent infinite loops
  int max_depth = 10;
  std::string traversal_log = "Traversal: ";
  while (max_depth-- > 0) {
    std::string current_prim_part = current_path.prim_part();

    const Prim *current_prim{nullptr};

    // First, try regular stage lookup
    bool current_found_in_stage = stage.find_prim_at_path(Path(current_prim_part, ""), current_prim, &err);

    // If not found and this is under a NodeGraph, look in NodeGraph children
    if (!current_found_in_stage || !current_prim) {
      // Check if this path is under the NodeGraph we found earlier
      size_t last_slash = current_prim_part.rfind('/');
      if (last_slash != std::string::npos) {
        std::string parent_path = current_prim_part.substr(0, last_slash);
        std::string child_name = current_prim_part.substr(last_slash + 1);

        // Check if parent is our NodeGraph
        if (ng_prim && parent_path.find("NodeGraphs") != std::string::npos) {
          // Look for the child in the NodeGraph prim
          for (const auto& child : ng_prim->children()) {
            if (child.element_name() == child_name) {
              current_prim = &child;
              break;
            }
          }
        }
      }

      if (!current_prim) {
        return nonstd::make_unexpected(
            fmt::format("Shader {} not found\n", current_prim_part));
      }
    }

    const Shader *current_shader = current_prim ? current_prim->as<Shader>() : nullptr;
    if (!current_shader) {
      return nonstd::make_unexpected(
          fmt::format("{} is not a Shader. {}\n", current_prim_part, traversal_log));
    }

    // Log this node
    traversal_log += current_shader->info_id + " -> ";

    // Check if this is an image node. ND_tiledimage_* are the tiling variants of
    // ND_image_* (Autodesk standard_surface / many MaterialX libraries author
    // these); they carry the same `inputs:file`, so treat them identically.
    if (current_shader->info_id == "ND_image_color4" ||
        current_shader->info_id == "ND_image_color3" ||
        current_shader->info_id == "ND_image_vector4" ||
        current_shader->info_id == "ND_image_vector3" ||
        current_shader->info_id == "ND_image_float" ||
        current_shader->info_id == "ND_tiledimage_color4" ||
        current_shader->info_id == "ND_tiledimage_color3" ||
        current_shader->info_id == "ND_tiledimage_vector4" ||
        current_shader->info_id == "ND_tiledimage_vector3" ||
        current_shader->info_id == "ND_tiledimage_float") {
      image_shader = current_shader;
      if (tex_abs_path) {
        *tex_abs_path = current_path;
      }
      if (image_shader_out) {
        *image_shader_out = image_shader;
      }
      if (assetInfo_out) {
        // get_assetInfo_struct returns AssetInfo converted from customData/assetInfo
        // Note: We only check if assetInfo is authored, but we don't return the pointer
        // since the storage has changed. The caller should use get_assetInfo_struct() directly.
        if (current_shader->metas().has_assetInfo()) {
          // AssetInfo is authored - caller should query it directly if needed
          *assetInfo_out = nullptr;
        }
      }

      MtlxTexcoordTransform texcoord_transform;
      const std::string resolved_st_varname = ResolveMtlxTexcoordVarname(
          stage, *image_shader, current_path, ng_prim,
          default_texcoords_primvar_name, &texcoord_transform);

      if (st_varname_out) {
        *st_varname_out = resolved_st_varname;
      }
      if (texcoord_transform_out) {
        *texcoord_transform_out = texcoord_transform;
      }

      const UVTexture::Channel output_channel =
          MaterialXOutputChannelFromPath(current_path);
      if (output_channel_out) {
        *output_channel_out = output_channel;
      }
      cache_result(current_path, image_shader, resolved_st_varname,
                   nullptr, output_channel, texcoord_transform);
      return true;
    }

    // Check if this node has an input connection we should follow
    // For ND_convert_color4_color3, follow inputs:in
    bool found_next = false;
    DCOUT("Checking shader " << current_shader->info_id << " at " << current_prim_part);

    // Debug: log all properties from both Shader and ShaderNode
    std::string props_list = "ShaderProps: ";
    for (const auto& prop : current_shader->props) {
      props_list += prop.first + " ";
    }

    // Check if the shader has a ShaderNode value with properties
    const ShaderNode *shader_node = current_shader->value.as<ShaderNode>();
    if (shader_node && !shader_node->props.empty()) {
      props_list += " NodeProps: ";
      for (const auto& prop : shader_node->props) {
        props_list += prop.first + " ";
      }
    }
    traversal_log += "[" + props_list + "] ";

    // Helper lambda to check for connections in a property map
    auto find_connection = [&](const std::map<std::string, Property>& props_map) -> bool {
      for (const auto& prop : props_map) {
        if (startsWith(prop.first, "inputs:")) {
          bool is_connection = false;
          Path next_path;

          if (endsWith(prop.first, ".connect")) {
            // Explicit .connect suffix
            is_connection = true;
            if (prop.second.is_relationship()) {
              auto next_targets = prop.second.get_relationTargets();
              if (!next_targets.empty()) {
                next_path = next_targets[0];
              }
            }
          } else if (prop.second.is_attribute()) {
            // Check if attribute has connections
            const Attribute &attr = prop.second.get_attribute();
            if (attr.has_connections() && !attr.connections().empty()) {
              is_connection = true;
              next_path = attr.connections()[0];
            }
          }

          if (is_connection && !next_path.full_path_name().empty()) {
            DCOUT("  Following connection from " << prop.first << " to " << next_path);
            current_path = next_path;
            return true;
          }
        }
      }
      return false;
    };

    // Try shader_node->props first, then fall back to current_shader->props
    if (shader_node && !shader_node->props.empty()) {
      found_next = find_connection(shader_node->props);
    }
    if (!found_next) {
      found_next = find_connection(current_shader->props);
    }

    if (!found_next) {
      break;
    }
  }

  return nonstd::make_unexpected(
      fmt::format("No image texture node found (supported: ND_image_* / ND_tiledimage_* color4/color3/vector4/vector3/float). {}\n", traversal_log));
}

}  // namespace

// Convert UsdUVTexture shader node.
// @return true upon conversion success(textures.back() contains the converted
// UVTexture)
//
// Possible network configuration
//
// - UsdUVTexture -> UsdPrimvarReader
// - UsdUVTexture -> UsdTransform2d -> UsdPrimvarReader


bool RenderSceneConverter::ConvertUVTexture(const RenderSceneConverterEnv &env,
                                            const Path &tex_abs_path,
                                            const AssetInfo &assetInfo,
                                            const UsdUVTexture &texture,
                                            UVTexture *tex_out) {
  DCOUT("ConvertUVTexture " << tex_abs_path);

  if (!tex_out) {
    PUSH_ERROR_AND_RETURN("tex_out arg is nullptr.");
  }
  std::string err;

  UVTexture tex;

  // Workaround for Blender export bug: UsdUVTexture without asset:file
  // This happens when Blender exports materials incorrectly
  bool has_file = texture.file.authored();

  value::AssetPath assetPath;
  if (has_file) {
    std::string asset_eval_err;
    if (!ResolveTypedAnimatableValue(env.stage, texture.file, "inputs:file",
                                     env.timecode, env.tinterp, &assetPath,
                                     &asset_eval_err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to resolve `asset:file` from {}: {}",
                      tex_abs_path.prim_part(), asset_eval_err));
    }
  } else {
    // `asset:file` not authored (known Blender export bug). If the caller
    // tolerates missing assets, downgrade this to a warning and return
    // a default-constructed UVTexture so higher-level conversion can
    // continue; otherwise behave as before.
    if (env.material_config.allow_missing_asset) {
      PushWarn(fmt::format(
          "`asset:file` is not authored for UsdUVTexture at {}. "
          "Returning empty texture (allow_missing_asset=true).",
          tex_abs_path.prim_part()));
      *tex_out = tex;
      return true;
    }
    PUSH_ERROR_AND_RETURN(fmt::format(
        "`asset:file` is not authored for UsdUVTexture at {}.",
        tex_abs_path.prim_part()));
  }

#if defined(LIGHTUSD_WITH_TEXTOOLS)
  // ---- Legacy-transparent KTX2 companion hint (lightusd extension) ----------
  // `inputs:file` deliberately stays a png/jpg/exr so the asset opens in stock
  // USD tools and is USDZ-legal. A per-attribute `customData` entry
  //     asset inputs:file = @diffuse.png@ ( customData = { asset ktx2 = @diffuse.ktx2@ } )
  // names a GPU-compressed companion. When it resolves via the asset resolver we
  // prefer it (the core image loader decodes/transcodes KTX2 to RGBA8); stock
  // tools ignore the hint and see only the png. Non-UDIM only. Assets without
  // the hint are unaffected.
  if (has_file) {
    const auto &fmeta = texture.file.metas();
    if (fmeta.has_customData()) {
      const auto cd = fmeta.get_customData();
      auto it = cd.find("ktx2");
      if (it != cd.end()) {
        std::string ktx2_hint;
        if (auto a = it->second.get_value<value::AssetPath>()) {
          ktx2_hint = a.value().GetAssetPath();
        } else if (auto s = it->second.get_value<std::string>()) {
          ktx2_hint = s.value();
        }
        if (!ktx2_hint.empty() && !io::IsUDIMPath(ktx2_hint)) {
          const std::string resolved = env.asset_resolver.resolve(ktx2_hint);
          if (!resolved.empty()) {
            DCOUT("Preferring KTX2 companion `" << ktx2_hint << "` over `"
                                                << assetPath.GetAssetPath()
                                                << "`");
            assetPath = value::AssetPath(ktx2_hint);
          } else {
            DCOUT("KTX2 companion `" << ktx2_hint
                                     << "` did not resolve; using original asset.");
          }
        }
      }
    }
  }
#endif  // LIGHTUSD_WITH_TEXTOOLS

  std::string authored_color_space;
  bool authored_color_space_set = false;
  const AttrMetas *file_metadata = has_file ? &texture.file.metas() : nullptr;
  if (!color_management::ComputeColorSpaceName(
          env.stage, tex_abs_path, file_metadata, &authored_color_space,
          &authored_color_space_set)) {
    authored_color_space.clear();
    authored_color_space_set = false;
  }

  UsdUVTexture::SourceColorSpace resolved_source_color_space =
      UsdUVTexture::SourceColorSpace::Auto;
  std::string source_color_space_error;
  const bool resolved_source_color_space_valid =
      authored_color_space_set ||
      ResolveSourceColorSpace(env.stage, texture.sourceColorSpace, env.timecode,
                              env.tinterp, &resolved_source_color_space,
                              &source_color_space_error);

  // TextureImage and BufferData
  {
    // Check image cache first - if the same asset path was already loaded,
    // reuse the existing image to avoid redundant I/O and memory usage
    std::string cacheKey = env.asset_resolver.resolve(assetPath.GetAssetPath());
    if (cacheKey.empty()) cacheKey = assetPath.GetAssetPath();
    if (authored_color_space_set) {
      lightusd::color::ColorTransform transform;
      std::string transform_error;
      if (color_management::BuildColorTransform(
              env.stage, tex_abs_path, authored_color_space,
              "lin_rec709_scene", &transform, &transform_error)) {
        cacheKey += "|source:" +
                    lightusd::color::CanonicalizeToken(authored_color_space) +
                    ":" + std::to_string(transform.source.gamma) + ":" +
                    std::to_string(transform.source.linear_bias);
        for (float coefficient : transform.matrix) {
          cacheKey += ":" + std::to_string(coefficient);
        }
      } else {
        cacheKey += "|invalid-source:" + authored_color_space + ":" +
                    tex_abs_path.prim_part();
      }
    } else if (resolved_source_color_space_valid) {
      cacheKey += "|source:" +
                  to_string(resolved_source_color_space);
    }

    // UDIM texture (e.g. `diffuse.<UDIM>.png`)?
    const bool is_udim = io::IsUDIMPath(assetPath.GetAssetPath());
    const bool udim_keep_as_is =
        is_udim && !env.material_config.combine_udim_tiles;

    const auto cachedImageIt = imageMap.find(cacheKey);
    if (cachedImageIt != imageMap.s_end()) {
      // Image already loaded, reuse it
      tex.texture_image_id = int64_t(cachedImageIt->second);
      DCOUT("Reusing cached image for: " << cacheKey << " (image_id=" << tex.texture_image_id << ")");

      // Restore UDIM remap / sparse-texture linkage for the reused image.
      const auto udimInfoIt = udimInfoMap.find(cacheKey);
      if (udimInfoIt != udimInfoMap.end() && udimInfoIt->second.is_udim) {
        tex.is_udim = true;
        tex.udim_uv_scale = udimInfoIt->second.uv_scale;
        tex.udim_uv_offset = udimInfoIt->second.uv_offset;
        tex.udim_texture_id = udimInfoIt->second.udim_texture_id;
      }
    } else if (udim_keep_as_is) {
      // ---- UDIM keep-as-is mode: load resolved tiles as separate images ----
      // and record a sparse `tydra::UDIMTexture` for web editing.
      std::vector<UDIMTile> tiles;
      std::string udim_warn, udim_err;
      if (!ExpandUDIMTiles(assetPath.GetAssetPath(), env.asset_resolver,
                           env.material_config.udim_max_tiles, &tiles,
                           &udim_warn, &udim_err)) {
        if (udim_warn.size()) PushWarn(udim_warn);
        if (!env.material_config.allow_texture_load_failure &&
            !env.material_config.allow_missing_asset) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to expand UDIM tiles for `{}`: {}",
              assetPath.GetAssetPath(), udim_err));
        }
        PUSH_WARN(fmt::format(
            "Failed to expand UDIM tiles for `{}`. Skip. reason = {}",
            assetPath.GetAssetPath(), udim_err));
      } else {
        if (udim_warn.size()) PushWarn(udim_warn);

        TextureImageLoaderFunction tile_loader =
            env.material_config.texture_image_loader_function;
        if (!tile_loader) {
          tile_loader = DefaultTextureImageLoaderFunction;
        }

        UDIMTexture udim;
        udim.asset_identifier = assetPath.GetAssetPath();
        udim.prim_name = tex_abs_path.element_name();
        udim.abs_path = tex_abs_path.prim_part();

        int64_t representative = -1;
        for (const auto &t : tiles) {
          TextureImage tileImage;
          BufferData tileBuffer;
          tileBuffer.componentType = ComponentType::UInt8;

          std::string w, e;
          value::AssetPath tileAssetPath(t.asset_path);
          if (!tile_loader(
                  tileAssetPath, assetInfo, env.asset_resolver, &tileImage,
                  &tileBuffer.data,
                  env.material_config.texture_image_loader_function_userdata,
                  &w, &e)) {
            if (w.size()) PushWarn(w);
            PUSH_WARN(fmt::format("Skip UDIM tile {} (`{}`): {}", t.udim_id,
                                  t.asset_path, e));
            continue;
          }
          if (w.size()) PushWarn(w);

          tileImage.asset_identifier = t.asset_path;
          tileImage.decoded = true;

          tileImage.buffer_id = int64_t(buffers.size());
          buffers.emplace_back(std::move(tileBuffer));

          const int64_t imgId = int64_t(images.size());
          images.emplace_back(tileImage);

          udim.imageTileIds[t.udim_id] = int32_t(imgId);
          if (representative < 0) {
            representative = imgId;
          }
        }

        if (!udim.imageTileIds.empty()) {
          tex.is_udim = true;
          tex.texture_image_id = representative;
          tex.udim_texture_id = int64_t(udim_textures.size());

          imageMap.add(cacheKey, uint64_t(representative));
          UDIMInfo info;
          info.is_udim = true;
          info.udim_texture_id = tex.udim_texture_id;
          udimInfoMap[cacheKey] = info;

          udim_textures.emplace_back(std::move(udim));
          DCOUT("Loaded UDIM (keep-as-is) " << cacheKey << " : "
                                            << tex.udim_texture_id);
        }
      }
    } else {
      // Image not in cache, need to load it

    TextureImage texImage;
    BufferData assetImageBuffer;

    // Texel data is treated as byte array
    assetImageBuffer.componentType = ComponentType::UInt8;

    bool tex_loaded{false};

    if (env.scene_config.load_texture_assets) {
      DCOUT("load texture : " << assetPath.GetAssetPath());
      std::string warn;

      TextureImageLoaderFunction tex_loader_fun =
          env.material_config.texture_image_loader_function;

      if (!tex_loader_fun) {
        tex_loader_fun = DefaultTextureImageLoaderFunction;
      }

      if (is_udim) {
        // UDIM combine mode: discover tiles, build a single grid atlas and
        // inject it as the decoded image so the existing color-space /
        // bit-depth pipeline below handles it like an ordinary texture. The
        // referenced mesh UV set is rebaked afterwards (ApplyUDIMUVTransforms).
        std::vector<UDIMTile> tiles;
        std::string udim_warn, udim_err;
        UDIMAtlas atlas;

        // Resize tiles in sRGB-aware space unless the texture is explicitly
        // authored as Raw (e.g. normal / data maps).
        bool srgb_resize = true;
        if (texture.sourceColorSpace.authored()) {
          UsdUVTexture::SourceColorSpace scs;
          std::string scs_err;
          if (ResolveSourceColorSpace(env.stage, texture.sourceColorSpace,
                                      env.timecode, env.tinterp, &scs,
                                      &scs_err)) {
            if (scs == UsdUVTexture::SourceColorSpace::Raw) {
              srgb_resize = false;
            }
          }
        }

        if (ExpandUDIMTiles(assetPath.GetAssetPath(), env.asset_resolver,
                            env.material_config.udim_max_tiles, &tiles,
                            &udim_warn, &udim_err) &&
            BuildUDIMAtlas(tiles, env.asset_resolver,
                           env.material_config.udim_max_atlas_size, srgb_resize,
                           &atlas, &udim_warn, &udim_err)) {
          if (udim_warn.size()) PushWarn(udim_warn);

          texImage.channels = atlas.image.channels;
          texImage.width = atlas.image.width;
          texImage.height = atlas.image.height;
          texImage.assetTexelComponentType = ComponentType::UInt8;
          assetImageBuffer.componentType = ComponentType::UInt8;
          assetImageBuffer.data = std::move(atlas.image.data);

          tex_loaded = true;

          tex.is_udim = true;
          tex.udim_uv_scale = atlas.uv_scale;
          tex.udim_uv_offset = atlas.uv_offset;
        } else {
          if (udim_warn.size()) PushWarn(udim_warn);
          err += udim_err;
          tex_loaded = false;
        }
      } else {
#if defined(LIGHTUSD_WITH_TEXTOOLS)
        // Keep-compressed KTX2 fast path: store the GPU block payload verbatim
        // (blockFormat set) instead of decoding to RGBA8. Falls through to the
        // normal decode when disabled, when the asset is not a .ktx2, or on any
        // parse failure.
        if (env.scene_config.keep_compressed_textures &&
            IsKTX2AssetPath(assetPath.GetAssetPath())) {
          std::string kc_warn;
          if (LoadKTX2CompressedBlocks(env.asset_resolver, assetPath, &texImage,
                                       &assetImageBuffer.data, &kc_warn, &err)) {
            tex_loaded = true;
          } else if (kc_warn.size()) {
            PushWarn(kc_warn);
          }
        }
        if (texImage.blockFormat == TextureBlockFormat::None)
#endif
        {
          tex_loaded = tex_loader_fun(
              assetPath, assetInfo, env.asset_resolver, &texImage,
              &assetImageBuffer.data,
              env.material_config.texture_image_loader_function_userdata, &warn,
              &err);
        }
      }

      if (warn.size()) {
        DCOUT("WARN: " << warn);
        PushWarn(warn);
      }

      if (!tex_loaded) {
        if (!env.material_config.allow_texture_load_failure) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to load texture image: `{}` err = {}",
              assetPath.GetAssetPath(), err));
        }

        const std::string load_err =
            err.empty() ? std::string("loader returned failure") : err;
        PUSH_WARN(fmt::format(
            "Failed to load texture image: `{}`. Skip loading. reason = {}",
            assetPath.GetAssetPath(), load_err));
      }

      // store unresolved asset path.
      texImage.asset_identifier = assetPath.GetAssetPath();
      texImage.decoded = tex_loaded;

    } else {
      // Metadata-only path. Keep the resolved asset identifier, but do not
      // read or copy texture bytes during RenderScene conversion. Web/native
      // clients can fetch the asset lazily when the texture is actually used.
      std::string resolvedPath = env.asset_resolver.resolve(assetPath.GetAssetPath());
      if (resolvedPath.empty()) {
        resolvedPath = assetPath.GetAssetPath();
      }
      texImage.asset_identifier = resolvedPath;
      texImage.decoded = false;
      DCOUT("store asset path.");
    }

    // colorSpace.
    // First look into `colorSpace` metadata of asset, then
    // look into `inputs:sourceColorSpace' attribute.
    // When both `colorSpace` metadata and `inputs:sourceColorSpace' attribute
    // exists, `colorSpace` metadata supercedes.
    // NOTE: `inputs:sourceColorSpace` attribute should be deprecated in favor of `colorSpace` metadata.
    auto storeColorTransform = [&](const std::string &source) -> bool {
      lightusd::color::ColorTransform transform;
      std::string transform_error;
      if (!color_management::BuildColorTransform(
              env.stage, tex_abs_path, source, "lin_rec709_scene",
              &transform, &transform_error)) {
        return false;
      }
      texImage.sourceColorSpaceName =
          lightusd::color::CanonicalizeToken(source);
      texImage.colorTransformValid = true;
      texImage.colorTransformBypass = transform.bypass;
      texImage.sourceColorIsData =
          transform.source.kind == lightusd::color::ColorSpaceKind::Data;
      texImage.sourceGamma = transform.source.gamma;
      texImage.sourceLinearBias = transform.source.linear_bias;
      std::memcpy(texImage.sourceToDisplayLinear, transform.matrix,
                  sizeof(texImage.sourceToDisplayLinear));
      return true;
    };

    bool sourceColorSpaceSet = false;
    bool inferColorSpaceFailed = false;
    if (authored_color_space_set) {
      ColorSpace cs;
      value::token cs_token(authored_color_space);
      if (InferColorSpace(cs_token, &cs)) {
        texImage.usdColorSpace = cs;
        (void)storeColorTransform(cs_token.str());
        sourceColorSpaceSet = true;
        DCOUT("Inferred colorSpace: " << to_string(cs));
      } else if (storeColorTransform(cs_token.str())) {
        texImage.usdColorSpace = tydra::ColorSpace::Custom;
        sourceColorSpaceSet = true;
        DCOUT("Resolved custom colorSpace: " << cs_token.str());
      } else {
        inferColorSpaceFailed = true;
      }
    }

    if (!authored_color_space_set) {
      // NOTE: Apply `inputs:sourceColorSpace` resolution even when the
      // attribute is not authored: its fallback value is `auto`
      // (UsdPreviewSurface spec), which must resolve from the texture's
      // bit depth/channel count instead of assuming sRGB.
      {
        const UsdUVTexture::SourceColorSpace cs = resolved_source_color_space;
        if (resolved_source_color_space_valid) {
          if (cs == UsdUVTexture::SourceColorSpace::SRGB) {
            texImage.usdColorSpace = tydra::ColorSpace::sRGB;
            sourceColorSpaceSet = true;
            (void)storeColorTransform("sRGB");
          } else if (cs == UsdUVTexture::SourceColorSpace::Raw) {
            texImage.usdColorSpace = tydra::ColorSpace::Raw;
            sourceColorSpaceSet = true;
            (void)storeColorTransform("raw");
          } else if (cs == UsdUVTexture::SourceColorSpace::Auto) {

            if (tex_loaded) {

              // The spec says: https://openusd.org/release/spec_usdpreviewsurface.html
              //
              // auto : Check for gamma/color space metadata in the texture file itself; if metadata is indicative of sRGB, mark texture as sRGB . If no relevant metadata is found, mark texture as sRGB if it is either 8-bit and has 3 channels or if it is 8-bit and has 4 channels. Otherwise, do not mark texture as sRGB and use texture data as it was read from the texture.
              //
              if (((texImage.assetTexelComponentType == ComponentType::UInt8) ||
                  (texImage.assetTexelComponentType == ComponentType::Int8)) &&
                ((texImage.channels == 3) || (texImage.channels ==4))) {
                texImage.usdColorSpace = tydra::ColorSpace::sRGB;
                sourceColorSpaceSet = true;
                (void)storeColorTransform("sRGB");
              } else {
                // For auto mode, non-8bit RGB(A) textures should be used as
                // read rather than warned about as ambiguous sRGB candidates.
                texImage.usdColorSpace = tydra::ColorSpace::Raw;
                sourceColorSpaceSet = true;
                (void)storeColorTransform("raw");
              }
            } else {
              texImage.usdColorSpace = tydra::ColorSpace::Unknown;
              sourceColorSpaceSet = true;
            }
          }
        } else if (!source_color_space_error.empty()) {
          PUSH_WARN(fmt::format(
              "Failed to resolve `inputs:sourceColorSpace`: {}",
              source_color_space_error));
        }
      }
    }

    if (!sourceColorSpaceSet && inferColorSpaceFailed &&
        authored_color_space_set) {
      value::token cs_token(authored_color_space);
      PUSH_ERROR_AND_RETURN(
          fmt::format("Invalid or unknown colorSpace metadataum: {}. Please "
                      "report an issue to LightUSD github repo.",
                      cs_token.str()));
    }

    if (tex_loaded) {
      BufferData imageBuffer;

#if defined(LIGHTUSD_WITH_TEXTOOLS)
      // Keep-compressed: the buffer holds GPU block bytes, not texels. Skip all
      // color-space linearization / bit-depth widening and pass the blocks
      // through unchanged.
      if (texImage.blockFormat != TextureBlockFormat::None) {
        imageBuffer = std::move(assetImageBuffer);
        texImage.colorSpace = texImage.usdColorSpace;
      } else
#endif
      // Linearlization and widen texel bit depth if required.
      if (env.material_config.linearize_color_space) {
        DCOUT("linearlize colorspace.");
        size_t width = size_t(texImage.width);
        size_t height = size_t(texImage.height);
        size_t channels = size_t(texImage.channels);

        if (channels > 4) {
          PUSH_ERROR_AND_RETURN(
              fmt::format("Multiband color channels(5 or more) are not "
                          "supported for texture color conversion."));
        }

        // Helper: convert u8 image data to f32 buffer
        auto u8_data_to_f32_buf = [&](std::vector<float> &buf) -> bool {
          bool ret = u8_to_f32_image(assetImageBuffer.data, width, height,
                                     channels, &buf, &_err);
          if (!ret) {
            PUSH_ERROR_AND_RETURN("Failed to convert u8 image to f32 image.");
          }
          return true;
        };

        // Helper: store f32 buffer into imageBuffer
        auto store_f32_buf = [&](const std::vector<float> &buf) {
          imageBuffer.componentType = tydra::ComponentType::Float;
          size_t resize_size;
          if (!safe::mul(buf.size(), sizeof(float), &resize_size)) {
            return;  // Overflow - skip
          }
          imageBuffer.data.resize(resize_size);
          size_t memcpy_size;
          if (!safe::mul(buf.size(), sizeof(float), &memcpy_size)) {
            return;  // Overflow - skip
          }
          memcpy(imageBuffer.data.data(), buf.data(), memcpy_size);
        };

        // Helper: extract f32 buffer from assetImageBuffer
        auto asset_data_to_f32_buf = [&](std::vector<float> &buf) {
          buf.resize(assetImageBuffer.data.size() / sizeof(float));
          size_t memcpy_size;
          if (!safe::mul(buf.size(), sizeof(float), &memcpy_size)) {
            return;  // Overflow - skip
          }
          memcpy(buf.data(), assetImageBuffer.data.data(), memcpy_size);
        };

        if (assetImageBuffer.componentType == tydra::ComponentType::UInt8) {
          // NOTE: this ~8-branch else-if chain on texImage.usdColorSpace was
          // converted to standalone ifs -- same MSVC C1061 ("blocks nested
          // too deeply") risk class already fixed for the same reason
          // elsewhere in this codebase. colorspace_matched reproduces the
          // original chain's "only report Unsupported when nothing matched"
          // fallback semantics (the trailing PUSH_ERROR here does not
          // return, unlike PUSH_ERROR_AND_RETURN inside the branches).
          bool colorspace_matched = false;

          if (texImage.usdColorSpace == tydra::ColorSpace::sRGB ||
              texImage.usdColorSpace == tydra::ColorSpace::sRGB_Texture) {
            colorspace_matched = true;
            if (env.material_config.preserve_texel_bitdepth) {
              imageBuffer.componentType = tydra::ComponentType::UInt8;
              bool ret = srgb_8bit_to_linear_8bit(
                  assetImageBuffer.data, width, height, channels,
                  channels, &imageBuffer.data, &_err);
              if (!ret) {
                PUSH_ERROR_AND_RETURN("Failed to convert sRGB u8 image to Linear u8 image.");
              }
            } else {
              std::vector<float> buf;
              bool ret = srgb_8bit_to_linear_f32(
                  assetImageBuffer.data, width, height, channels,
                  channels, &buf, &_err);
              if (!ret) {
                PUSH_ERROR_AND_RETURN("Failed to convert sRGB u8 image to Linear f32 image.");
              }
              store_f32_buf(buf);
            }
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Lin_sRGB ||
              texImage.usdColorSpace == tydra::ColorSpace::Lin_Rec709) {
            colorspace_matched = true;
            if (env.material_config.preserve_texel_bitdepth) {
              imageBuffer = std::move(assetImageBuffer);
            } else {
              std::vector<float> buf;
              if (!u8_data_to_f32_buf(buf)) return false;
              store_f32_buf(buf);
            }
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Raw) {
            colorspace_matched = true;
            // Raw data — no color conversion, just optional bit depth change
            if (env.material_config.preserve_texel_bitdepth) {
              imageBuffer = std::move(assetImageBuffer);
            } else {
              std::vector<float> buf;
              if (!u8_data_to_f32_buf(buf)) return false;
              store_f32_buf(buf);
            }
            texImage.colorSpace = tydra::ColorSpace::Raw;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Custom &&
              texImage.colorTransformValid) {
            colorspace_matched = true;
            std::vector<float> buf;
            if (!u8_data_to_f32_buf(buf)) return false;
            if (channels < 3) {
              PUSH_ERROR_AND_RETURN(
                  "Custom color-space texture requires at least 3 channels.");
            }
            lightusd::color::ColorTransform transform;
            transform.source.name = texImage.sourceColorSpaceName;
            transform.source.gamma = texImage.sourceGamma;
            transform.source.linear_bias = texImage.sourceLinearBias;
            transform.source.kind = texImage.sourceColorIsData
                ? lightusd::color::ColorSpaceKind::Data
                : lightusd::color::ColorSpaceKind::Color;
            (void)lightusd::color::GetBuiltinColorSpace(
                "lin_rec709_scene", &transform.destination);
            transform.bypass = texImage.colorTransformBypass;
            std::memcpy(transform.matrix, texImage.sourceToDisplayLinear,
                        sizeof(transform.matrix));
            for (size_t i = 0; i < width * height; ++i) {
              lightusd::color::TransformRGB(
                  transform, &buf[i * channels]);
            }
            store_f32_buf(buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::g22_Rec709) {
            colorspace_matched = true;
            // Gamma 2.2 u8 -> linear f32 (via gamma removal)
            std::vector<float> buf;
            if (!u8_data_to_f32_buf(buf)) return false;
            std::vector<float> out_buf;
            if (!gamma22_f32_to_linear_f32(buf, width, height, channels, channels, &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert gamma 2.2 image to linear.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::g18_Rec709) {
            colorspace_matched = true;
            std::vector<float> buf;
            if (!u8_data_to_f32_buf(buf)) return false;
            std::vector<float> out_buf;
            if (!gamma18_f32_to_linear_f32(buf, width, height, channels, channels, &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert gamma 1.8 image to linear.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (!colorspace_matched) {
            PUSH_ERROR(fmt::format("Unsupported color space for u8 textures: {}",
                                   to_string(texImage.usdColorSpace)));
          }

        } else if (assetImageBuffer.componentType == tydra::ComponentType::Float) {
          std::vector<float> in_buf;
          asset_data_to_f32_buf(in_buf);

          // NOTE: same standalone-if / colorspace_matched conversion as the
          // UInt8 branch above (MSVC C1061 risk); see the comment there.
          bool colorspace_matched = false;

          if (texImage.usdColorSpace == tydra::ColorSpace::sRGB ||
              texImage.usdColorSpace == tydra::ColorSpace::sRGB_Texture) {
            colorspace_matched = true;
            std::vector<float> out_buf(in_buf.size());
            float scale_factor = 1.0f, bias = 0.0f;
            float alpha_scale_factor = 1.0f, alpha_bias = 0.0f;
            if (!srgb_f32_to_linear_f32(in_buf, width, height, channels, channels,
                                        &out_buf, scale_factor, bias,
                                        alpha_scale_factor, alpha_bias, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert sRGB f32 image to Linear f32 image.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Lin_sRGB ||
              texImage.usdColorSpace == tydra::ColorSpace::Lin_Rec709) {
            colorspace_matched = true;
            imageBuffer = std::move(assetImageBuffer);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Raw) {
            colorspace_matched = true;
            imageBuffer = std::move(assetImageBuffer);
            texImage.colorSpace = tydra::ColorSpace::Raw;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Custom &&
              texImage.colorTransformValid) {
            colorspace_matched = true;
            if (channels < 3) {
              PUSH_ERROR_AND_RETURN(
                  "Custom color-space texture requires at least 3 channels.");
            }
            lightusd::color::ColorTransform transform;
            transform.source.name = texImage.sourceColorSpaceName;
            transform.source.gamma = texImage.sourceGamma;
            transform.source.linear_bias = texImage.sourceLinearBias;
            transform.source.kind = texImage.sourceColorIsData
                ? lightusd::color::ColorSpaceKind::Data
                : lightusd::color::ColorSpaceKind::Color;
            (void)lightusd::color::GetBuiltinColorSpace(
                "lin_rec709_scene", &transform.destination);
            transform.bypass = texImage.colorTransformBypass;
            std::memcpy(transform.matrix, texImage.sourceToDisplayLinear,
                        sizeof(transform.matrix));
            for (size_t i = 0; i < width * height; ++i) {
              lightusd::color::TransformRGB(
                  transform, &in_buf[i * channels]);
            }
            store_f32_buf(in_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Lin_ACEScg) {
            colorspace_matched = true;
            // ACEScg (AP1 linear) -> linear sRGB
            std::vector<float> out_buf;
            if (!ACEScg_to_linear_sRGB(in_buf, width, height, channels,
                                       &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert ACEScg to linear sRGB.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::ACES2065_1) {
            colorspace_matched = true;
            // ACES 2065-1 (AP0 linear) -> linear sRGB
            std::vector<float> out_buf;
            if (!ACES2065_1_to_linear_sRGB(in_buf, width, height, channels,
                                           &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert ACES 2065-1 to linear sRGB.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Lin_DisplayP3) {
            colorspace_matched = true;
            // Linear Display P3 -> linear sRGB
            std::vector<float> out_buf;
            if (!linear_displayp3_to_linear_sRGB(in_buf, width, height, channels,
                                                 &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert Linear DisplayP3 to linear sRGB.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::sRGB_DisplayP3) {
            colorspace_matched = true;
            // sRGB DisplayP3: first sRGB EOTF, then DisplayP3 -> sRGB gamut
            std::vector<float> linear_p3(in_buf.size());
            float sf = 1.0f, b = 0.0f, asf = 1.0f, ab = 0.0f;
            if (!srgb_f32_to_linear_f32(in_buf, width, height, channels, channels,
                                        &linear_p3, sf, b, asf, ab, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to linearize sRGB DisplayP3.");
            }
            std::vector<float> out_buf;
            if (!linear_displayp3_to_linear_sRGB(linear_p3, width, height, channels,
                                                 &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert DisplayP3 to linear sRGB.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::Lin_Rec2020) {
            colorspace_matched = true;
            std::vector<float> out_buf;
            if (!linear_rec2020_to_linear_sRGB(in_buf, width, height, channels,
                                               &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert Linear Rec.2020 to linear sRGB.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::g22_Rec709) {
            colorspace_matched = true;
            std::vector<float> out_buf;
            if (!gamma22_f32_to_linear_f32(in_buf, width, height, channels, channels,
                                           &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert gamma 2.2 f32 to linear.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (texImage.usdColorSpace == tydra::ColorSpace::g18_Rec709) {
            colorspace_matched = true;
            std::vector<float> out_buf;
            if (!gamma18_f32_to_linear_f32(in_buf, width, height, channels, channels,
                                           &out_buf, &_err)) {
              PUSH_ERROR_AND_RETURN("Failed to convert gamma 1.8 f32 to linear.");
            }
            store_f32_buf(out_buf);
            texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;
          }
          if (!colorspace_matched) {
            PUSH_ERROR(fmt::format("Unsupported color space for f32 textures: {}",
                                   to_string(texImage.usdColorSpace)));
          }

        } else if (texImage.usdColorSpace == tydra::ColorSpace::Raw) {
          imageBuffer = std::move(assetImageBuffer);
          texImage.colorSpace = tydra::ColorSpace::Raw;

        } else if ((texImage.usdColorSpace == tydra::ColorSpace::Lin_sRGB ||
                    texImage.usdColorSpace == tydra::ColorSpace::Lin_Rec709) &&
                   env.material_config.preserve_texel_bitdepth) {
          imageBuffer = std::move(assetImageBuffer);
          texImage.colorSpace = tydra::ColorSpace::Lin_sRGB;

        } else {
          PUSH_ERROR(fmt::format(
              "Unsupported asset texture texel format {} for color conversion "
              "from {}",
              to_string(assetImageBuffer.componentType),
              to_string(texImage.usdColorSpace)));
        }

      } else {
        // Same color space.
        DCOUT("assetImageBuffer.sz = " << assetImageBuffer.data.size());

        if (assetImageBuffer.componentType == tydra::ComponentType::UInt8) {
          if (env.material_config.preserve_texel_bitdepth) {
            // Do nothing.
            imageBuffer = std::move(assetImageBuffer);

          } else {
            size_t width = size_t(texImage.width);
            size_t height = size_t(texImage.height);
            size_t channels = size_t(texImage.channels);

            // u8 to f32, but no sRGB -> linear conversion(this would break
            // UsdPreviewSurface's spec though)
            PUSH_WARN(
                "8bit sRGB texture is converted to fp32 sRGB texture(without "
                "linearlization)");
            std::vector<float> buf;
            bool ret = u8_to_f32_image(assetImageBuffer.data, width, height,
                                       channels, &buf, &_err);
            if (!ret) {
              PUSH_ERROR_AND_RETURN("Failed to convert u8 image to f32 image.");
            }
            imageBuffer.componentType = tydra::ComponentType::Float;

            size_t resize_size;
            if (!safe::mul(buf.size(), sizeof(float), &resize_size)) {
              PUSH_ERROR_AND_RETURN("Integer overflow: buf.size() * sizeof(float)");
            }
            imageBuffer.data.resize(resize_size);
            size_t memcpy_size;
            if (!safe::mul(buf.size(), sizeof(float), &memcpy_size)) {
              PUSH_ERROR_AND_RETURN("Integer overflow in memcpy");
            }
            memcpy(imageBuffer.data.data(), buf.data(), memcpy_size);
          }

          texImage.colorSpace = texImage.usdColorSpace;

        } else if (assetImageBuffer.componentType ==
                   tydra::ComponentType::Float) {
          // ignore preserve_texel_bitdepth

          // f32 to f32, so no op
          imageBuffer = std::move(assetImageBuffer);

        } else {
          imageBuffer = std::move(assetImageBuffer);
        }
        texImage.colorSpace = texImage.usdColorSpace;
      }

      if (env.material_config.linearize_color_space &&
          texImage.colorTransformValid) {
        texImage.colorTransformApplied = true;
      }

      // Assign buffer id
      texImage.buffer_id = int64_t(buffers.size());

      buffers.emplace_back(std::move(imageBuffer));

      tex.texture_image_id = int64_t(images.size());

      // Add to image cache for reuse by other textures with same asset path
      imageMap.add(cacheKey, uint64_t(tex.texture_image_id));

      // Cache UDIM remap so reused (combined) atlas images restore the mesh-UV
      // transform on subsequent textures referencing the same `<UDIM>` path.
      if (tex.is_udim) {
        UDIMInfo info;
        info.is_udim = true;
        info.uv_scale = tex.udim_uv_scale;
        info.uv_offset = tex.udim_uv_offset;
        info.udim_texture_id = tex.udim_texture_id;
        udimInfoMap[cacheKey] = info;
      }

      images.emplace_back(texImage);

      std::stringstream ss;
      ss << "Loaded texture image " << assetPath.GetAssetPath()
         << " : buffer_id " + std::to_string(texImage.buffer_id) << "\n";
      ss << "  width x height x components " << texImage.width << " x "
         << texImage.height << " x " << texImage.channels << "\n";
      ss << "  colorSpace " << lightusd::tydra::to_string(texImage.colorSpace)
         << "\n";
      PushInfo(ss.str());
    } else {

      tex.texture_image_id = int64_t(images.size());

      // Add to image cache for reuse by other textures with same asset path
      imageMap.add(cacheKey, uint64_t(tex.texture_image_id));

      images.emplace_back(texImage);

      std::stringstream ss;
      ss << "Loaded texture image " << assetPath.GetAssetPath()
         << " : buffer_id " + std::to_string(texImage.buffer_id) << "\n";
      ss << "  width x height x components " << texImage.width << " x "
         << texImage.height << " x " << texImage.channels << "\n";
      ss << "  colorSpace " << lightusd::tydra::to_string(texImage.colorSpace)
         << "\n";
      PushInfo(ss.str());

    }
    } // end of image cache else block (image not in cache)
  }

  //
  // Set authored outputChannels
  //
  if (texture.outputsRGB.authored()) {
    tex.authoredOutputChannels.insert(UVTexture::Channel::RGB);
  }

  if (texture.outputsA.authored()) {
    tex.authoredOutputChannels.insert(UVTexture::Channel::A);
  }

  if (texture.outputsR.authored()) {
    tex.authoredOutputChannels.insert(UVTexture::Channel::R);
  }

  if (texture.outputsG.authored()) {
    tex.authoredOutputChannels.insert(UVTexture::Channel::G);
  }

  if (texture.outputsB.authored()) {
    tex.authoredOutputChannels.insert(UVTexture::Channel::B);
  }


  //
  // Convert other UVTexture parameters
  //

  if (texture.bias.authored()) {
    tex.bias = texture.bias.get_value();
  }

  if (texture.scale.authored()) {
    tex.scale = texture.scale.get_value();
  }

  if (texture.st.authored()) {
    if (!texture.st.get_connections().empty()) {
      // Follow the connection even if a fallback value is present
      // (is_connection() is false then) — the connection (e.g. to a
      // UsdTransform2d / UsdPrimvarReader_float2) overrides the fallback.
      const auto &paths = texture.st.get_connections();
      if (paths.size() != 1) {
        PUSH_ERROR_AND_RETURN(
            "UsdUVTexture inputs:st connection must be single Path.");
      }
      const Path &path = paths[0];

      const Prim *readerPrim{nullptr};
      if (!env.stage.find_prim_at_path(Path(path.prim_part(), ""), readerPrim,
                                       &err)) {
        PUSH_ERROR_AND_RETURN(
            "UsdUVTexture inputs:st connection targetPath not found in the "
            "Stage: " +
            err);
      }

      if (!readerPrim) {
        PUSH_ERROR_AND_RETURN(
            "[InternlError] Invalid Prim connected to inputs:st");
      }

      const Shader *pshader = readerPrim->as<Shader>();
      if (!pshader) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("UsdUVTexture inputs:st connected Prim must be "
                        "Shader Prim, but got {} Prim",
                        readerPrim->prim_type_name()));
      }

      // currently UsdTranform2d or PrimvarReaer_float2 only for inputs:st
      if (const UsdPrimvarReader_float2 *preader =
              pshader->value.as<UsdPrimvarReader_float2>()) {
        if (!preader) {
          PUSH_ERROR_AND_RETURN(
              fmt::format("Shader's info:id must be UsdPrimvarReader_float2, "
                          "but got {}",
                          pshader->info_id));
        }

        // Get value producing attribute(i.e, follow .connection and return
        // terminal Attribute value)
        std::string varname;
        TerminalAttributeValue attr;
        if (!tydra::EvaluateAttribute(env.stage, *readerPrim, "inputs:varname",
                                      &attr, &err)) {
          PUSH_ERROR_AND_RETURN(
              fmt::format("Failed to evaluate UsdPrimvarReader_float2's "
                          "inputs:varname.\n{}",
                          err));
        }

        if (auto pv = attr.as<value::token>()) {
          varname = (*pv).str();
        } else if (auto pvs = attr.as<std::string>()) {
          varname = (*pvs);
        } else if (auto pvsd = attr.as<value::StringData>()) {
          varname = (*pvsd).value;
        } else {
          PUSH_ERROR_AND_RETURN(
              "`inputs:varname` must be `string` or `token` type, but got " +
              attr.type_name());
        }
        if (varname.empty()) {
          PUSH_ERROR_AND_RETURN("`inputs:varname` is empty token.");
        }
        DCOUT("inputs:varname = " << varname);

        tex.varname_uv = varname;
      } else if (const UsdTransform2d *ptransform =
                     pshader->value.as<UsdTransform2d>()) {
        auto result = ConvertTexTransform2d(env.stage, path, *ptransform, &tex,
                                            env.timecode);
        if (!result) {
          PUSH_ERROR_AND_RETURN(result.error());
        }
      } else {
        PUSH_ERROR_AND_RETURN(
            "Unsupported Shader type for `inputs:st` connection: " +
            pshader->info_id + "\n");
      }

    } else {
      //LIGHTUSD_LOG_I("get_value");
      Animatable<value::texcoord2f> fallbacks = texture.st.get_value();
      value::texcoord2f uv;
      if (fallbacks.get(env.timecode, &uv)) {
        tex.fallback_uv[0] = uv[0];
        tex.fallback_uv[1] = uv[1];
      } else {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to get fallback `st` texcoord attribute for "
            "UsdUVTexture at {}.",
            tex_abs_path.prim_part()));
      }
      //LIGHTUSD_LOG_I("uv done");
    }
  }

  if (texture.wrapS.authored()) {
    lightusd::UsdUVTexture::Wrap wrap;
    std::string wrap_err;

    if (!ResolveTextureWrap(env.stage, texture.wrapS, "inputs:wrapS",
                            env.timecode, env.tinterp, &wrap, &wrap_err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Invalid UsdUVTexture `inputs:wrapS` value: {}", wrap_err));
    }

    if (wrap == UsdUVTexture::Wrap::Repeat) {
      tex.wrapS = UVTexture::WrapMode::REPEAT;
    } else if (wrap == UsdUVTexture::Wrap::Mirror) {
      tex.wrapS = UVTexture::WrapMode::MIRROR;
    } else if (wrap == UsdUVTexture::Wrap::Clamp) {
      tex.wrapS = UVTexture::WrapMode::CLAMP_TO_EDGE;
    } else if (wrap == UsdUVTexture::Wrap::Black) {
      tex.wrapS = UVTexture::WrapMode::CLAMP_TO_BORDER;
    } else {
      tex.wrapS = UVTexture::WrapMode::CLAMP_TO_EDGE;
    }
  }

  if (texture.wrapT.authored()) {
    lightusd::UsdUVTexture::Wrap wrap;
    std::string wrap_err;

    if (!ResolveTextureWrap(env.stage, texture.wrapT, "inputs:wrapT",
                            env.timecode, env.tinterp, &wrap, &wrap_err)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Invalid UsdUVTexture `inputs:wrapT` value: {}", wrap_err));
    }

    if (wrap == UsdUVTexture::Wrap::Repeat) {
      tex.wrapT = UVTexture::WrapMode::REPEAT;
    } else if (wrap == UsdUVTexture::Wrap::Mirror) {
      tex.wrapT = UVTexture::WrapMode::MIRROR;
    } else if (wrap == UsdUVTexture::Wrap::Clamp) {
      tex.wrapT = UVTexture::WrapMode::CLAMP_TO_EDGE;
    } else if (wrap == UsdUVTexture::Wrap::Black) {
      tex.wrapT = UVTexture::WrapMode::CLAMP_TO_BORDER;
    } else {
      tex.wrapT = UVTexture::WrapMode::CLAMP_TO_EDGE;
    }
  }

  DCOUT("Converted UVTexture.");

  (*tex_out) = tex;
  return true;
}

static const std::map<std::string, Property> *ColorSourceProperties(
    const Prim &prim) {
  if (const Material *p = prim.as<Material>()) return &p->props;
  if (const NodeGraph *p = prim.as<NodeGraph>()) return &p->props;
  if (const Shader *p = prim.as<Shader>()) {
    if (const ShaderNode *node = p->value.as<ShaderNode>()) {
      if (!node->props.empty()) return &node->props;
    }
    return &p->props;
  }
  return nullptr;
}

static bool ResolveConnectedColorSource(
    const Stage &stage, const Path &connection, Path *source_prim_path,
    AttrMetas *source_metadata, std::set<std::string> *visited,
    int depth = 0) {
  if (!source_prim_path || !source_metadata || !visited || depth > 32 ||
      !connection.is_valid()) {
    return false;
  }
  const std::string key = connection.full_path_name();
  if (!visited->insert(key).second) return false;
  const Prim *prim = nullptr;
  std::string lookup_error;
  if (!stage.find_prim_at_path(Path(connection.prim_part(), ""), prim,
                               &lookup_error) || !prim) {
    return false;
  }
  const std::map<std::string, Property> *props = ColorSourceProperties(*prim);
  if (!props) return false;

  const auto exact = props->find(connection.prop_part());
  if (exact != props->end() && exact->second.is_attribute()) {
    const Attribute &attribute = exact->second.get_attribute();
    if (attribute.metas().has_colorSpace()) {
      *source_prim_path = Path(connection.prim_part(), "");
      *source_metadata = attribute.metas();
      return true;
    }
    if (attribute.has_connections() && !attribute.connections().empty() &&
        ResolveConnectedColorSource(stage, attribute.connections()[0],
                                    source_prim_path, source_metadata, visited,
                                    depth + 1)) {
      return true;
    }
  }

  // MaterialX constant/utility outputs commonly carry no metadata. Their
  // value-bearing input is the authored color source, so prefer explicit
  // input metadata and then a connected input recursively.
  const Attribute *fallback_input = nullptr;
  for (const auto &entry : *props) {
    if (entry.first.rfind("inputs:", 0) != 0 ||
        !entry.second.is_attribute()) {
      continue;
    }
    const Attribute &attribute = entry.second.get_attribute();
    if (!fallback_input || entry.first == "inputs:value" ||
        entry.first == "inputs:in") {
      fallback_input = &attribute;
    }
    if (attribute.metas().has_colorSpace()) {
      *source_prim_path = Path(connection.prim_part(), "");
      *source_metadata = attribute.metas();
      return true;
    }
    if (attribute.has_connections() && !attribute.connections().empty() &&
        ResolveConnectedColorSource(stage, attribute.connections()[0],
                                    source_prim_path, source_metadata, visited,
                                    depth + 1)) {
      return true;
    }
  }
  if (fallback_input) {
    *source_prim_path = Path(connection.prim_part(), "");
    *source_metadata = fallback_input->metas();
    return true;
  }
  return false;
}

static bool MaterialXConfiguredColorSpace(const Stage &stage,
                                          const Path &source_prim_path,
                                          std::string *out) {
  if (!out) return false;
  // Graph resolvers commonly return the connected output property path. Stage
  // prim lookup and ancestor traversal must start from its absolute prim path,
  // otherwise a valid `/Mat/Graph/Image.outputs:out` silently misses the
  // enclosing MaterialXConfigAPI.
  const Path source_path(source_prim_path.prim_part(), "");
  auto source_result = stage.GetPrimAtPath(source_path);
  const Prim *source_prim = source_result ? source_result.value() : nullptr;
  const Shader *source_shader = source_prim ? source_prim->as<Shader>() : nullptr;
  if (!source_shader ||
      (source_shader->info_id.rfind("ND_", 0) != 0 &&
       source_shader->info_id != "image" &&
       source_shader->info_id != "tiledimage")) {
    return false;
  }
  Path current = source_path.get_parent_prim_path();
  while (current.is_valid() && current.prim_part() != "/") {
    auto candidate_result = stage.GetPrimAtPath(current);
    const Prim *candidate = candidate_result
        ? candidate_result.value() : nullptr;
    const Material *material = candidate ? candidate->as<Material>() : nullptr;
    if (material && material->materialXConfig) {
      const std::string configured =
          material->materialXConfig->mtlx_colorspace.get_value();
      if (!configured.empty()) {
        *out = color::CanonicalizeToken(configured);
        return true;
      }
      return false;
    }
    const Path parent = current.get_parent_prim_path();
    if (!parent.is_valid() || parent.prim_part() == current.prim_part()) break;
    current = parent;
  }
  return false;
}

static bool TransformShaderColorConstant(
    const RenderSceneConverterEnv &env, const Path &shader_abs_path,
    const AttrMetas &metadata, const Path *connection,
    const std::string &working_color_space, bool is_materialx,
    ShaderParam<vec3> *parameter, std::string *error) {
  if (!parameter || parameter->is_texture()) return true;
  // -2 is an internal handoff marker used only between MaterialX constant
  // folding and this post-conversion pass. Restore the public invalid-texture
  // sentinel after avoiding a second color transform.
  if (parameter->texture_id == kWorkingColorValueTextureId) {
    parameter->texture_id = -1;
    return true;
  }
  Path source_prim_path = shader_abs_path;
  AttrMetas source_metadata = metadata;
  if (connection) {
    std::set<std::string> visited;
    (void)ResolveConnectedColorSource(env.stage, *connection,
                                      &source_prim_path, &source_metadata,
                                      &visited);
  }
  std::string source;
  bool authored = false;
  if (!color_management::ComputeColorSpaceName(
          env.stage, source_prim_path, &source_metadata, &source,
          &authored)) return false;
  if (!authored && is_materialx) {
    (void)MaterialXConfiguredColorSpace(env.stage, source_prim_path, &source);
  }
  color::ColorTransform transform;
  if (!color_management::BuildColorTransform(
          env.stage, source_prim_path, source, working_color_space,
          &transform, error)) return false;
  float rgb[3] = {parameter->value[0], parameter->value[1],
                  parameter->value[2]};
  color::TransformRGB(transform, rgb);
  parameter->value[0] = rgb[0];
  parameter->value[1] = rgb[1];
  parameter->value[2] = rgb[2];
  return true;
}

template <typename T, typename Dty>
bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam(
    const RenderSceneConverterEnv &env, const Path &shader_abs_path,
    const TypedAttributeWithFallback<Animatable<T>> &param,
    const std::string &param_name, ShaderParam<Dty> &dst_param,
    bool is_materialx) {
  if (!param.authored()) {
    return true;
  }

  if (param.is_blocked()) {
    PUSH_ERROR_AND_RETURN(fmt::format("{} attribute is blocked.", param_name));
  } else if (!param.get_connections().empty()) {
    // Follow the connection whenever one is authored — even if a fallback value
    // is ALSO present (is_connection() is false in that case). In USD a
    // connection overrides the shader input's fallback value, so a textured
    // input like `float3 inputs:diffuseColor = (0.18,..) ; .connect = </tex>`
    // must use the texture, not the (0.18) fallback (usd-wg TextureTransformTest).
    DCOUT(fmt::format("{} is attribute connection.", param_name));

    // Some USD-authored OpenPBR/standard_surface graphs use a direct
    // UsdUVTexture connection even though the terminal shader is classified
    // as MaterialX. Prefer that concrete USD texture node when present; the
    // MaterialX graph resolver is for ND_image/NodeGraph connections and would
    // otherwise reject this valid mixed graph and retain only the fallback.
    if (is_materialx) {
      const UsdUVTexture *direct_texture{nullptr};
      const Shader *direct_shader{nullptr};
      Path direct_path;
      auto direct_result = GetConnectedUVTexture(
          env.stage, param.get_connections(), &direct_path, &direct_texture, &direct_shader);
      if (direct_result && *direct_result && direct_texture && direct_shader) {
        is_materialx = false;
      }
    }

    // Check if this is a MaterialX connection to a NodeGraph
    if (is_materialx && param.get_connections().size() == 1) {
      const Path &conn_path = param.get_connections()[0];
      // Attempt MaterialX texture/graph resolution for ANY single connection.
      // GetConnectedMtlxTexture resolves the connected prim via stage lookup, so
      // it handles NodeGraph outputs of any name (e.g. `NG_brass1`), not just
      // graphs literally named "/NodeGraphs".
      {
        // Traverse the MaterialX node graph to find a texture
        const Shader *image_shader{nullptr};
        Path texPath;
        std::string st_varname;
        const AssetInfo *assetInfo{nullptr};
        UVTexture::Channel mtlx_output_channel{UVTexture::Channel::RGB};
        MtlxTexcoordTransform mtlx_texcoord_transform;
        std::string mtlx_input_color_space;

        auto mtlx_result = GetConnectedMtlxTexture(
            env.stage, param.get_connections(), &texPath, &image_shader, &st_varname, &assetInfo,
            &mtlx_output_channel, &mtlx_texcoord_transform,
            env.mesh_config.default_texcoords_primvar_name);

        if (mtlx_result) {
          // Found a MaterialX texture node
          DCOUT("Found MaterialX texture node: " << texPath);

          // Extract the file path from the image shader
          value::AssetPath texAssetPath;
          bool found_file = false;

          // Helper lambda to find file input in a property map
          auto find_file_input = [&](const std::map<std::string, Property>& props_map) -> bool {
            for (const auto& prop : props_map) {
              if (prop.first == "inputs:file" && prop.second.is_attribute()) {
                const Attribute &attr = prop.second.get_attribute();
                if (attr.has_value()) {
                  auto asset_val = attr.get_value<value::AssetPath>();
                  if (asset_val) {
                    texAssetPath = *asset_val;
                    if (attr.metas().has_colorSpace()) {
                      mtlx_input_color_space =
                          attr.metas().get_colorSpace().str();
                    }
                    return true;
                  }
                }
              }
            }
            return false;
          };

          // Check both ShaderNode props and Shader props
          const ShaderNode *shader_node = image_shader->value.as<ShaderNode>();
          if (shader_node && !shader_node->props.empty()) {
            found_file = find_file_input(shader_node->props);
          }
          if (!found_file) {
            found_file = find_file_input(image_shader->props);
          }

          if (!found_file) {
            // A MaterialX <image> node with no `inputs:file` legitimately
            // outputs its default/constant value (e.g. an ND_image_vector3 used
            // as a zero source). Treat it as "no texture" and keep this shader
            // input's authored fallback rather than failing the whole material.
            PUSH_WARN(fmt::format(
                "MaterialX image node {} has no file input; using the shader "
                "input's default value for `{}`.",
                texPath.prim_part(), param_name));
            return true;
          }

          // Create a synthetic UsdUVTexture to pass to ConvertUVTexture
          UsdUVTexture synth_tex;
          synth_tex.file.set_value(texAssetPath);

          value::float2 mtlx_uv_scale;
          mtlx_uv_scale[0] = 1.0f;
          mtlx_uv_scale[1] = 1.0f;
          value::float2 mtlx_uv_translation;
          mtlx_uv_translation[0] = 0.0f;
          mtlx_uv_translation[1] = 0.0f;
          float mtlx_uv_rotation = 0.0f;
          bool has_mtlx_uv_transform = false;
          if (mtlx_texcoord_transform.has_transform) {
            mtlx_uv_scale = mtlx_texcoord_transform.scale;
            mtlx_uv_translation = mtlx_texcoord_transform.translation;
            mtlx_uv_rotation = mtlx_texcoord_transform.rotation;
            has_mtlx_uv_transform = true;
          }

          // Helper lambda to extract wrap mode from properties
          auto extract_wrap_modes = [&](const std::map<std::string, Property>& props_map) {
            auto map_address_mode = [](const std::string &mode,
                                       UsdUVTexture::Wrap *wrap) {
              if (!wrap) {
                return false;
              }
              if (mode == "periodic") {
                *wrap = UsdUVTexture::Wrap::Repeat;
                return true;
              }
              if (mode == "clamp") {
                *wrap = UsdUVTexture::Wrap::Clamp;
                return true;
              }
              if (mode == "mirror") {
                *wrap = UsdUVTexture::Wrap::Mirror;
                return true;
              }
              if (mode == "constant") {
                *wrap = UsdUVTexture::Wrap::Black;
                return true;
              }
              return false;
            };
            std::string mode;
            UsdUVTexture::Wrap wrap;
            if (FindMtlxStringInput(props_map, "inputs:uaddressmode", &mode) &&
                map_address_mode(mode, &wrap)) {
              synth_tex.wrapS.set_value(wrap);
            }
            if (FindMtlxStringInput(props_map, "inputs:vaddressmode", &mode) &&
                map_address_mode(mode, &wrap)) {
              synth_tex.wrapT.set_value(wrap);
            }
          };

          auto extract_uv_transform =
              [&](const std::map<std::string, Property> &props_map) {
                for (const auto &prop : props_map) {
                  if (!prop.second.is_attribute()) {
                    continue;
                  }
                  const Attribute &attr = prop.second.get_attribute();
                  if (!attr.has_value()) {
                    continue;
                  }

                  if (prop.first == "inputs:uvtiling" ||
                      prop.first == "inputs:scale") {
                    auto val = attr.get_value<value::float2>();
                    if (val) {
                      mtlx_uv_scale = *val;
                      has_mtlx_uv_transform = true;
                    }
                  } else if (prop.first == "inputs:uvoffset" ||
                             prop.first == "inputs:translation") {
                    auto val = attr.get_value<value::float2>();
                    if (val) {
                      mtlx_uv_translation = *val;
                      has_mtlx_uv_transform = true;
                    }
                  } else if (prop.first == "inputs:rotate" ||
                             prop.first == "inputs:rotation") {
                    auto val = attr.get_value<float>();
                    if (val) {
                      mtlx_uv_rotation = *val;
                      has_mtlx_uv_transform = true;
                    }
                  }
                }
              };

          // Map MaterialX wrap modes to USD - check both ShaderNode and Shader props
          if (shader_node && !shader_node->props.empty()) {
            extract_wrap_modes(shader_node->props);
            extract_uv_transform(shader_node->props);
          }
          extract_wrap_modes(image_shader->props);
          extract_uv_transform(image_shader->props);

          // Use ConvertUVTexture to properly handle the texture
          UVTexture rtex;
          AssetInfo mtlx_assetInfo; // Use the assetInfo if available
          if (assetInfo) {
            mtlx_assetInfo = *assetInfo;
          }

          // Explicit image metadata wins, then the MaterialX document working
          // space recorded by MaterialXConfigAPI. Only unconfigured color
          // inputs use the historical sRGB heuristic; data inputs remain Raw.
          {
            static const std::set<std::string> srgb_params = {
              "diffuseColor", "emissiveColor", "specularColor",
              "base_color", "emission_color", "specular_color",
              "coat_color", "sheen_color", "subsurface_color",
              "transmission_color", "fuzz_color",
            };
            const bool is_color = srgb_params.count(param_name) != 0;
            std::string effective_color_space = mtlx_input_color_space;
            if (is_color && effective_color_space.empty()) {
              bool inherited_authored = false;
              AttrMetas no_file_metadata;
              (void)color_management::ComputeColorSpaceName(
                  env.stage, Path(texPath.prim_part(), ""),
                  &no_file_metadata, &effective_color_space,
                  &inherited_authored);
              if (!inherited_authored) {
                effective_color_space.clear();
                (void)MaterialXConfiguredColorSpace(
                    env.stage, texPath, &effective_color_space);
              }
            }
            if (is_color && !effective_color_space.empty()) {
              synth_tex.file.metas().set_colorSpace(effective_color_space);
            } else {
              Animatable<UsdUVTexture::SourceColorSpace> cs;
              cs.set_default(is_color
                  ? UsdUVTexture::SourceColorSpace::SRGB
                  : UsdUVTexture::SourceColorSpace::Raw);
              synth_tex.sourceColorSpace.set_value(cs);
            }
          }

          if (!ConvertUVTexture(env, texPath, mtlx_assetInfo, synth_tex, &rtex)) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "Failed to convert MaterialX texture for {}", param_name));
          }

          // Set the connected output channel and UV primvar name
          rtex.connectedOutputChannel = mtlx_output_channel;
          rtex.varname_uv = st_varname;
          if (has_mtlx_uv_transform) {
            ApplyTexTransform2d(mtlx_uv_rotation, mtlx_uv_scale,
                                mtlx_uv_translation, &rtex);
          }

          uint64_t texId = textures.size();
          textures.push_back(rtex);

          textureMap.add(texId, shader_abs_path.prim_part() + "." + param_name);

          DCOUT(fmt::format("MaterialX TexId {}.{} = {}",
                            shader_abs_path.prim_part(), param_name, texId));

          dst_param.texture_id = int32_t(texId);

          return true;
        } else {
          // No texture found — try evaluating the node graph as a constant
          // value (e.g., ND_add_float or ND_multiply_color3 with constant inputs).
          std::string evaluation_space = "lin_rec709_scene";
          (void)MaterialXConfiguredColorSpace(env.stage, shader_abs_path,
                                               &evaluation_space);
          std::string mtlx_const_err;
          auto const_result = EvaluateMtlxNodeGraphAsConstant(
              env.stage, conn_path, evaluation_space);
          if (const_result) {
            DCOUT(fmt::format("MaterialX constant evaluation {}.{} components={}",
                              shader_abs_path.prim_part(), param_name,
                              const_result->n));
            if (std::is_same<T, float>::value && const_result->is_float()) {
              float v = const_result->as_float();
              memcpy(&dst_param.value, &v, sizeof(float));
              return true;
            }
            if (std::is_same<T, value::color3f>::value &&
                const_result->is_color3() && const_result->color_managed) {
              value::color3f c;
              c[0] = const_result->v[0];
              c[1] = const_result->v[1];
              c[2] = const_result->v[2];
              color::ColorTransform transform;
              std::string transform_error;
              if (!color_management::BuildColorTransform(
                      env.stage, shader_abs_path, evaluation_space,
                      _working_color_space, &transform, &transform_error)) {
                mtlx_const_err = transform_error;
              } else {
                float rgb[3] = {c[0], c[1], c[2]};
                color::TransformRGB(transform, rgb);
                c[0] = rgb[0];
                c[1] = rgb[1];
                c[2] = rgb[2];
                memcpy(&dst_param.value, &c, sizeof(value::color3f));
                dst_param.texture_id = kWorkingColorValueTextureId;
                return true;
              }
            }
            if (std::is_same<T, value::color3f>::value &&
                const_result->is_color3()) {
              value::color3f c;
              c[0] = const_result->v[0];
              c[1] = const_result->v[1];
              c[2] = const_result->v[2];
              memcpy(&dst_param.value, &c, sizeof(value::color3f));
              return true;
            }
            // Float result can also be used for color3 (broadcast)
            if (std::is_same<T, value::color3f>::value && const_result->is_float()) {
              float f = const_result->as_float();
              value::color3f c;
              c[0] = f; c[1] = f; c[2] = f;
              color::ColorTransform transform;
              std::string transform_error;
              if (color_management::BuildColorTransform(
                      env.stage, shader_abs_path, evaluation_space,
                      _working_color_space, &transform, &transform_error)) {
                float rgb[3] = {c[0], c[1], c[2]};
                color::TransformRGB(transform, rgb);
                c[0] = rgb[0];
                c[1] = rgb[1];
                c[2] = rgb[2];
                memcpy(&dst_param.value, &c, sizeof(value::color3f));
                dst_param.texture_id = kWorkingColorValueTextureId;
                return true;
              }
              mtlx_const_err = transform_error;
            }
            // Color3 result used for float (take first component)
            if (std::is_same<T, float>::value && const_result->is_color3()) {
              float v = const_result->v[0];
              memcpy(&dst_param.value, &v, sizeof(float));
              return true;
            }
          }
          // Capture the constant-evaluator's diagnostic (e.g. "Unsupported node
          // type ...") so it can be surfaced in the fallback warning below;
          // otherwise it is lost (it only reached DCOUT before).
          if (!const_result && mtlx_const_err.empty()) {
            mtlx_const_err = const_result.error();
          }
          // Interface passthrough: the connection may target a Material /
          // Shader / NodeGraph interface input that holds a constant value
          // (e.g. `Material.inputs:metalness = 1`, common in flattened
          // MaterialX). Follow `inputs:` connections a few hops and read the
          // first constant value found into the param.
          {
            Path cur = conn_path;
            for (int hop = 0; hop < 8; hop++) {
              const Prim *cp{nullptr};
              std::string ce;
              if (!env.stage.find_prim_at_path(Path(cur.prim_part(), ""), cp,
                                               &ce) ||
                  !cp) {
                break;
              }
              const std::map<std::string, Property> *pm = nullptr;
              if (const Material *mp = cp->as<Material>()) {
                pm = &mp->props;
              } else if (const Shader *sp = cp->as<Shader>()) {
                const ShaderNode *sn = sp->value.as<ShaderNode>();
                pm = (sn && !sn->props.empty()) ? &sn->props : &sp->props;
              } else if (const NodeGraph *ngp = cp->as<NodeGraph>()) {
                pm = &ngp->props;
              }
              if (!pm) break;
              auto pit = pm->find(cur.prop_part());
              if (pit == pm->end() || !pit->second.is_attribute()) break;
              const Attribute &ia = pit->second.get_attribute();
              if (ia.has_connections() && !ia.connections().empty()) {
                cur = ia.connections()[0];  // forward one hop
                continue;
              }
              if (SetShaderParamFromInterfaceAttribute(ia, &dst_param)) {
                return true;
              }
              break;
            }
          }
          T fallback_val;
          if (!param.is_value_empty() &&
              param.get_value().get(env.timecode, &fallback_val)) {
            dst_param.set_value(fallback_val);
            PushWarn(fmt::format(
                "MaterialX connection for {} could not be resolved to a "
                "texture ({}); using the authored/fallback value instead.",
                param_name, mtlx_result.error()));
            return true;
          }

          // MaterialX input we couldn't resolve to a texture or constant. Do
          // NOT fall through to the UsdUVTexture path — MaterialX does not use
          // UsdUVTexture and that path would replace the clearer graph error
          // with a generic interface-connection error.
          DCOUT(fmt::format(
              "MaterialX: no texture or fallback for {} ({})",
              param_name, mtlx_result.error()));
          if (env.material_config.strict_material_check) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "Failed to find MaterialX texture for {}: {}", param_name,
                mtlx_result.error()));
          }
          PushWarn(fmt::format(
              "MaterialX connection for {} could not be resolved to a "
              "texture or constant ({}){}; using the parameter default instead.",
              param_name, mtlx_result.error(),
              mtlx_const_err.empty() ? std::string()
                                     : " [" + mtlx_const_err + "]"));
          return true;
        }
      }
    }

    // Fall back to standard UsdUVTexture handling
    const UsdUVTexture *ptex{nullptr};
    const Shader *pshader{nullptr};
    Path texPath;
    auto result =
        GetConnectedUVTexture(env.stage, param.get_connections(), &texPath, &ptex, &pshader);

    if (!result) {
      if (is_materialx) {
        // MaterialX connection we could not resolve to a texture (e.g. it
        // targets a Material interface input/constant or an unmodeled node).
        // Keep an authored fallback value if it exists; otherwise surface the
        // MaterialX graph error instead of silently accepting an unresolved
        // texture input.
        T fallback_val;
        if (!param.is_value_empty() &&
            param.get_value().get(env.timecode, &fallback_val)) {
          dst_param.set_value(fallback_val);
          PushWarn(fmt::format(
              "MaterialX connection for {} could not be resolved to a "
              "UsdUVTexture ({}); using the authored/fallback value instead.",
              param_name, result.error()));
          return true;
        }
        if (env.material_config.strict_material_check) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to find MaterialX texture for {}: {}", param_name,
              result.error()));
        }
        PushWarn(fmt::format(
            "MaterialX connection for {} could not be resolved to a "
            "UsdUVTexture ({}); using the parameter default instead.",
            param_name, result.error()));
        return true;
      }
      // The connection did not resolve to a UsdUVTexture (e.g. a UsdPreviewSurface
      // input wired to a non-texture node such as a UsdPrimvarReader) while a
      // value is also authored. Degrade to that value instead of failing the
      // whole material — we now follow connections even when a value is present,
      // so erroring here would be a regression vs. the previous value path.
      T fallback_val;
      if (!param.is_value_empty() &&
          param.get_value().get(env.timecode, &fallback_val)) {
        dst_param.set_value(fallback_val);
        PushWarn(fmt::format(
            "{} connection could not be resolved to a UsdUVTexture ({}); using "
            "the authored/fallback value instead.",
            param_name, result.error()));
        return true;
      }
      PUSH_ERROR_AND_RETURN(result.error());
    }

    if (!ptex) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "[InternalError] ptex is nullptr for parameter '{}' in shader '{}'.",
          param_name, shader_abs_path.full_path_name()));
    }
    DCOUT("ptex = " << ptex->name);

    if (!pshader) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "[InternalError] pshader is nullptr for parameter '{}' in shader '{}'.",
          param_name, shader_abs_path.full_path_name()));
    }

    DCOUT("Get connected UsdUVTexture Prim: " << texPath);

    UVTexture rtex;
    const AssetInfo assetInfo = pshader->metas().get_assetInfo_struct();
    if (!ConvertUVTexture(env, texPath, assetInfo, *ptex, &rtex)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to convert UVTexture connected to {}", param_name));
    }

    // Extract connected outputChannel from prop part.
    std::string prop_part = texPath.prop_part();

    // TODO: Attribute type check.
    if (prop_part == "outputs:r") {
      rtex.connectedOutputChannel = tydra::UVTexture::Channel::R;
    } else if (prop_part == "outputs:g") {
      rtex.connectedOutputChannel = tydra::UVTexture::Channel::G;
    } else if (prop_part == "outputs:b") {
      rtex.connectedOutputChannel = tydra::UVTexture::Channel::B;
    } else if (prop_part == "outputs:a") {
      rtex.connectedOutputChannel = tydra::UVTexture::Channel::A;
    } else if (prop_part == "outputs:rgb") {
      rtex.connectedOutputChannel = tydra::UVTexture::Channel::RGB;
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format("Unknown or invalid connection to a property of output channel: {}(Abs path {})", prop_part, texPath.full_path_name()));
    }


    uint64_t texId = textures.size();
    textures.push_back(rtex);

    textureMap.add(texId, shader_abs_path.prim_part() + "." + param_name);

    DCOUT(fmt::format("TexId {}.{} = {}",
                      shader_abs_path.prim_part(), param_name, texId));

    dst_param.texture_id = int32_t(texId);

    return true;
  } else {
    T val;
    if (!param.get_value().get(env.timecode, &val)) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Failed to get {} at `default` timecode.", param_name));
    }

    dst_param.set_value(val);

    return true;
  }
}

bool RenderSceneConverter::ConvertPreviewSurfaceShader(
    const RenderSceneConverterEnv &env, const Path &shader_abs_path,
    const UsdPreviewSurface &shader, PreviewSurfaceShader *rshader_out,
    bool is_materialx) {
  if (!rshader_out) {
    PUSH_ERROR_AND_RETURN("rshader_out arg is nullptr.");
  }

  PreviewSurfaceShader rshader;

  if (shader.useSpecularWorkflow.authored()) {
    if (shader.useSpecularWorkflow.is_blocked()) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("useSpecularWorkflow attribute is blocked."));
    } else {
      int val;
      std::string eval_err;
      if (ResolveTypedAnimatableValue(
              env.stage, shader.useSpecularWorkflow,
              "inputs:useSpecularWorkflow", env.timecode, env.tinterp, &val,
              &eval_err)) {
        rshader.useSpecularWorkflow = val ? true : false;
      } else {
        // `useSpecularWorkflow` is authored only as a connection that resolves
        // to no value — e.g. a MaterialX ND_UsdPreviewSurface_surfaceshader
        // whose `inputs:useSpecularWorkflow.connect` points at an unvalued
        // Material input (usd-wg MaterialXTest/basic_flatten). Fall back to the
        // UsdPreviewSurface default (false) rather than failing the material,
        // matching how the other (connected) parameters degrade gracefully.
        PushWarn(fmt::format(
            "useSpecularWorkflow could not be resolved ({}); using default "
            "(false).", eval_err));
      }
    }
  }

  // Macro to reduce repetitive ConvertPreviewSurfaceShaderParam calls.
#define CONVERT_PREVIEW_PARAM(field, name) \
  if (!ConvertPreviewSurfaceShaderParam( \
          env, shader_abs_path, shader.field, name, rshader.field, \
          is_materialx)) { \
    PushWarn(fmt::format("Failed to convert " name " parameter for shader: {}", \
                         shader_abs_path.prim_part())); \
    return false; \
  }

  CONVERT_PREVIEW_PARAM(diffuseColor, "diffuseColor")
  CONVERT_PREVIEW_PARAM(emissiveColor, "emissiveColor")
  CONVERT_PREVIEW_PARAM(specularColor, "specularColor")
  CONVERT_PREVIEW_PARAM(normal, "normal")
  CONVERT_PREVIEW_PARAM(roughness, "roughness")
  CONVERT_PREVIEW_PARAM(metallic, "metallic")
  CONVERT_PREVIEW_PARAM(clearcoat, "clearcoat")
  CONVERT_PREVIEW_PARAM(clearcoatRoughness, "clearcoatRoughness")
  CONVERT_PREVIEW_PARAM(opacity, "opacity")
  CONVERT_PREVIEW_PARAM(opacityThreshold, "opacityThreshold")
  CONVERT_PREVIEW_PARAM(ior, "ior")
  CONVERT_PREVIEW_PARAM(occlusion, "occlusion")
  CONVERT_PREVIEW_PARAM(displacement, "displacement")

#undef CONVERT_PREVIEW_PARAM

#define TRANSFORM_PREVIEW_COLOR(field)                                      \
  {                                                                         \
    std::string color_error;                                                \
    const Path *color_connection =                                          \
        (shader.field.has_connections() &&                                  \
         !shader.field.get_connections().empty())                           \
            ? &shader.field.get_connections()[0]                            \
            : nullptr;                                                      \
    if (!TransformShaderColorConstant(                                      \
            env, shader_abs_path, shader.field.metas(),                     \
            color_connection, _working_color_space, is_materialx,          \
            &rshader.field,                                                 \
            &color_error)) {                                                \
      PushWarn("Failed to transform " #field " color: " + color_error);    \
    }                                                                       \
  }
  TRANSFORM_PREVIEW_COLOR(diffuseColor)
  TRANSFORM_PREVIEW_COLOR(emissiveColor)
  TRANSFORM_PREVIEW_COLOR(specularColor)
#undef TRANSFORM_PREVIEW_COLOR

  (*rshader_out) = rshader;
  return true;
}

bool RenderSceneConverter::ConvertOpenPBRSurfaceShader(
    const RenderSceneConverterEnv &env, const Path &shader_abs_path,
    const OpenPBRSurface &shader, OpenPBRSurfaceShader *rshader_out,
    bool is_materialx, bool standard_surface_source) {
  if (!rshader_out) {
    PUSH_ERROR_AND_RETURN("rshader_out argument is nullptr.");
  }

  OpenPBRSurfaceShader rshader;

  // Standard Surface is represented internally through an OpenPBR-compatible
  // struct, but MaterialX resolution still queries the original USD shader.
  // Preserve the original property spelling for fields renamed by that
  // conversion; querying `inputs:specular_ior`, for example, cannot find the
  // authored Standard Surface `inputs:specular_IOR` value.
  auto sourceName = [standard_surface_source](const char *openpbr_name) {
    if (!standard_surface_source) return openpbr_name;
    static const std::map<std::string, const char *> renamed = {
        {"base_weight", "base"},
        {"base_roughness", "diffuse_roughness"},
        {"base_metalness", "metalness"},
        {"specular_weight", "specular"},
        {"specular_ior", "specular_IOR"},
        {"transmission_weight", "transmission"},
        {"subsurface_weight", "subsurface"},
        {"sheen_weight", "sheen"},
        {"coat_weight", "coat"},
        {"coat_ior", "coat_IOR"},
        {"thin_film_ior", "thin_film_IOR"},
        {"emission_luminance", "emission"},
    };
    const auto it = renamed.find(openpbr_name);
    return it == renamed.end() ? openpbr_name : it->second;
  };

  // Macros to reduce repetitive ConvertPreviewSurfaceShaderParam calls. When
  // this shader came from a MaterialX network (standard_surface / OpenPBR), EVERY
  // input is MaterialX-style (connects to NodeGraph outputs or Material interface
  // inputs), so all params must use the MaterialX resolution path — not just the
  // historically-tagged subset. The `_MTLX` variant is kept for source clarity
  // but resolves to the same is_materialx flag.
#define CONVERT_OPENPBR_PARAM(field, name) \
  if (!ConvertPreviewSurfaceShaderParam( \
          env, shader_abs_path, shader.field, sourceName(name), rshader.field, is_materialx)) { \
    PushWarn(fmt::format("Failed to convert " name " parameter for shader: {}", shader_abs_path.prim_part())); \
    return false; \
  }
#define CONVERT_OPENPBR_PARAM_MTLX(field, name) \
  if (!ConvertPreviewSurfaceShaderParam( \
          env, shader_abs_path, shader.field, sourceName(name), rshader.field, is_materialx)) { \
    PushWarn(fmt::format("Failed to convert " name " parameter for shader: {}", shader_abs_path.prim_part())); \
    return false; \
  }

  // Base layer
  CONVERT_OPENPBR_PARAM_MTLX(base_weight, "base_weight")
  CONVERT_OPENPBR_PARAM_MTLX(base_color, "base_color")
  CONVERT_OPENPBR_PARAM_MTLX(base_roughness, "base_roughness")
  CONVERT_OPENPBR_PARAM_MTLX(base_metalness, "base_metalness")
  CONVERT_OPENPBR_PARAM_MTLX(base_diffuse_roughness, "base_diffuse_roughness")

  // Specular layer
  CONVERT_OPENPBR_PARAM_MTLX(specular_weight, "specular_weight")
  CONVERT_OPENPBR_PARAM_MTLX(specular_color, "specular_color")
  CONVERT_OPENPBR_PARAM_MTLX(specular_roughness, "specular_roughness")
  CONVERT_OPENPBR_PARAM_MTLX(specular_ior, "specular_ior")
  CONVERT_OPENPBR_PARAM(specular_ior_level, "specular_ior_level")
  CONVERT_OPENPBR_PARAM(specular_anisotropy, "specular_anisotropy")
  CONVERT_OPENPBR_PARAM(specular_rotation, "specular_rotation")
  CONVERT_OPENPBR_PARAM(specular_roughness_anisotropy, "specular_roughness_anisotropy")

  // Transmission
  CONVERT_OPENPBR_PARAM_MTLX(transmission_weight, "transmission_weight")
  CONVERT_OPENPBR_PARAM_MTLX(transmission_color, "transmission_color")
  CONVERT_OPENPBR_PARAM(transmission_depth, "transmission_depth")
  CONVERT_OPENPBR_PARAM(transmission_scatter, "transmission_scatter")
  CONVERT_OPENPBR_PARAM(transmission_scatter_anisotropy, "transmission_scatter_anisotropy")
  CONVERT_OPENPBR_PARAM(transmission_dispersion, "transmission_dispersion")
  CONVERT_OPENPBR_PARAM(transmission_dispersion_abbe_number, "transmission_dispersion_abbe_number")
  CONVERT_OPENPBR_PARAM(transmission_dispersion_scale, "transmission_dispersion_scale")

  // Subsurface
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_weight, "subsurface_weight")
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_color, "subsurface_color")
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_radius, "subsurface_radius")
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_radius_scale, "subsurface_radius_scale")
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_scale, "subsurface_scale")
  CONVERT_OPENPBR_PARAM_MTLX(subsurface_anisotropy, "subsurface_anisotropy")
  CONVERT_OPENPBR_PARAM(subsurface_scatter_anisotropy, "subsurface_scatter_anisotropy")

  // Sheen
  CONVERT_OPENPBR_PARAM_MTLX(sheen_weight, "sheen_weight")
  CONVERT_OPENPBR_PARAM_MTLX(sheen_color, "sheen_color")
  CONVERT_OPENPBR_PARAM_MTLX(sheen_roughness, "sheen_roughness")

  // Fuzz
  CONVERT_OPENPBR_PARAM_MTLX(fuzz_weight, "fuzz_weight")
  CONVERT_OPENPBR_PARAM_MTLX(fuzz_color, "fuzz_color")
  CONVERT_OPENPBR_PARAM_MTLX(fuzz_roughness, "fuzz_roughness")

  // Thin film
  CONVERT_OPENPBR_PARAM_MTLX(thin_film_weight, "thin_film_weight")
  CONVERT_OPENPBR_PARAM_MTLX(thin_film_thickness, "thin_film_thickness")
  CONVERT_OPENPBR_PARAM_MTLX(thin_film_ior, "thin_film_ior")

  // Coat layer
  CONVERT_OPENPBR_PARAM_MTLX(coat_weight, "coat_weight")
  CONVERT_OPENPBR_PARAM_MTLX(coat_color, "coat_color")
  CONVERT_OPENPBR_PARAM_MTLX(coat_roughness, "coat_roughness")
  CONVERT_OPENPBR_PARAM_MTLX(coat_anisotropy, "coat_anisotropy")
  CONVERT_OPENPBR_PARAM_MTLX(coat_rotation, "coat_rotation")
  CONVERT_OPENPBR_PARAM_MTLX(coat_ior, "coat_ior")
  CONVERT_OPENPBR_PARAM_MTLX(coat_affect_color, "coat_affect_color")
  CONVERT_OPENPBR_PARAM_MTLX(coat_affect_roughness, "coat_affect_roughness")
  CONVERT_OPENPBR_PARAM(coat_roughness_anisotropy, "coat_roughness_anisotropy")
  CONVERT_OPENPBR_PARAM(coat_darkening, "coat_darkening")

  // Emission
  CONVERT_OPENPBR_PARAM_MTLX(emission_luminance, "emission_luminance")
  CONVERT_OPENPBR_PARAM_MTLX(emission_color, "emission_color")

  // Geometry
  CONVERT_OPENPBR_PARAM_MTLX(opacity, "opacity")
  CONVERT_OPENPBR_PARAM_MTLX(normal, "normal")
  CONVERT_OPENPBR_PARAM_MTLX(tangent, "tangent")

#define TRANSFORM_OPENPBR_COLOR(field)                                      \
  {                                                                         \
    std::string color_error;                                                \
    const Path *color_connection =                                          \
        (shader.field.has_connections() &&                                  \
         !shader.field.get_connections().empty())                           \
            ? &shader.field.get_connections()[0]                            \
            : nullptr;                                                      \
    if (!TransformShaderColorConstant(                                      \
            env, shader_abs_path, shader.field.metas(),                     \
            color_connection, _working_color_space, is_materialx,          \
            &rshader.field,                                                 \
            &color_error)) {                                                \
      PushWarn("Failed to transform " #field " color: " + color_error);    \
    }                                                                       \
  }
  TRANSFORM_OPENPBR_COLOR(base_color)
  TRANSFORM_OPENPBR_COLOR(specular_color)
  TRANSFORM_OPENPBR_COLOR(transmission_color)
  TRANSFORM_OPENPBR_COLOR(transmission_scatter)
  TRANSFORM_OPENPBR_COLOR(subsurface_color)
  TRANSFORM_OPENPBR_COLOR(sheen_color)
  TRANSFORM_OPENPBR_COLOR(fuzz_color)
  TRANSFORM_OPENPBR_COLOR(coat_color)
  TRANSFORM_OPENPBR_COLOR(emission_color)
#undef TRANSFORM_OPENPBR_COLOR

#undef CONVERT_OPENPBR_PARAM

  if (standard_surface_source) {
    // OpenPBR's renderer-side fallback thickness is 500 nm, whereas Standard
    // Surface defaults to no film (0 nm) and has no separate weight input.
    // A missing Standard input must therefore not retain the OpenPBR fallback.
    float thickness = 0.0f;
    shader.thin_film_thickness.get_value().get(value::TimeCode::Default(),
                                                &thickness);
    if (!shader.thin_film_thickness.has_connections()) {
      rshader.thin_film_thickness.value = thickness;
    }
    rshader.thin_film_weight.value =
        (shader.thin_film_thickness.has_connections() || thickness > 0.0f)
            ? 1.0f
            : 0.0f;
  }

  // Convert MaterialX NodeGraph connections to JSON if present
  // This allows reconstruction of node-based shading in JavaScript/WASM
  {
    const Prim *shader_prim_ptr = nullptr;
    std::string lookup_err;
    if (env.stage.find_prim_at_path(shader_abs_path, shader_prim_ptr, &lookup_err) && shader_prim_ptr) {
      std::string nodegraph_json;
      std::string conv_err;
      if (ConvertShaderWithNodeGraphToJson(*shader_prim_ptr, shader_abs_path, env.stage, &nodegraph_json, &conv_err)) {
        if (!nodegraph_json.empty()) {
          rshader.nodeGraphJson = nodegraph_json;
          DCOUT("Successfully converted MaterialX NodeGraph to JSON for shader: " << shader_abs_path.prim_part());
        }
      } else {
        // Not an error - shader may not have node graph connections
        DCOUT("No MaterialX NodeGraph found for shader: " << shader_abs_path.prim_part() << " (" << conv_err << ")");
      }
    }
  }

  (*rshader_out) = rshader;
  return true;
}

// Convert MtlxAutodeskStandardSurface → OpenPBRSurface.
// Maps StandardSurface parameters to their OpenPBR equivalents.
// Key differences: naming (base vs base_weight), opacity type (color3f vs float),
// no fuzz layer in StandardSurface.

// Material assembly uses these two parameter forms directly. Keep their code
// generation in the shader TU rather than implicitly instantiating it in every
// material-assembly consumer.
template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<
    value::normal3f, value::color3f>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<value::normal3f>> &,
    const std::string &, ShaderParam<value::color3f> &, bool);
template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<
    value::normal3f, value::float3>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<value::normal3f>> &,
    const std::string &, ShaderParam<value::float3> &, bool);
template bool RenderSceneConverter::ConvertPreviewSurfaceShaderParam<float, float>(
    const RenderSceneConverterEnv &, const Path &,
    const TypedAttributeWithFallback<Animatable<float>> &, const std::string &,
    ShaderParam<float> &, bool);

}  // namespace tydra
}  // namespace lightusd
