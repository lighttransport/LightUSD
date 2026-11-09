// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "render-converter-shader-param.hh"
#include "layer/prim-spec.hh"
#include "render-extract.hh"
#include "scene-access.hh"
#include "render-converter-mtlx-eval.hh"
#include "mem-budget.hh"
#include "eval/attribute-eval.hh"
#include "schema/usd-shade.hh"
#include "core/safe-arithmetic.hh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>

namespace lightusd { namespace tydra { namespace next {
namespace shader_param_detail {
bool CombineUDIMTileImages(RenderScene* scene,
                           const std::vector<RenderUDIMTexture::Tile>& tiles,
                           const std::string& identifier,
                           size_t resident_limit, int32_t* image_id,
                           Float2* uv_scale, Float2* uv_offset,
                           std::string* error) {
  if (!scene || !image_id || !uv_scale || !uv_offset || tiles.empty()) return false;
  uint32_t min_u = 9, max_u = 0, min_v = 9, max_v = 0;
  const TextureImage* first = nullptr;
  for (const auto& tile : tiles) {
    if (tile.udim < 1001 || tile.udim > 1100 || tile.image_id < 0 ||
        static_cast<size_t>(tile.image_id) >= scene->images.size()) return false;
    const uint32_t u = (tile.udim - 1001) % 10;
    const uint32_t v = (tile.udim - 1001) / 10;
    min_u = std::min(min_u, u); max_u = std::max(max_u, u);
    min_v = std::min(min_v, v); max_v = std::max(max_v, v);
    const TextureImage& image = scene->images[static_cast<size_t>(tile.image_id)];
    if (!image.is_loaded() || image.component_type != ComponentType::UInt8 ||
        (image.channels != 1 && image.channels != 3 && image.channels != 4)) {
      if (error) *error = "UDIM atlas requires decoded 8-bit tiles";
      return false;
    }
    size_t source_pixels = 0, source_bytes = 0;
    if (!safe::mul(static_cast<size_t>(image.width),
                   static_cast<size_t>(image.height), &source_pixels) ||
        !safe::mul(source_pixels, static_cast<size_t>(image.channels),
                   &source_bytes) || image.data.size() != source_bytes) {
      if (error) *error = "UDIM tile payload has an invalid size";
      return false;
    }
    if (!first) first = &image;
    else if (image.width != first->width || image.height != first->height ||
             image.channels != first->channels) {
      if (error) *error = "UDIM atlas tiles must have matching dimensions and channels";
      return false;
    }
  }
  if (!first || first->width == 0 || first->height == 0) return false;
  const uint32_t cols = max_u - min_u + 1;
  const uint32_t rows = max_v - min_v + 1;
  size_t wide_width = 0, wide_height = 0;
  size_t pixels = 0, bytes = 0;
  if (!safe::mul(static_cast<size_t>(first->width), static_cast<size_t>(cols),
                 &wide_width) ||
      !safe::mul(static_cast<size_t>(first->height), static_cast<size_t>(rows),
                 &wide_height) ||
      wide_width > 4096 || wide_height > 4096 ||
      wide_width > (std::numeric_limits<uint32_t>::max)() ||
      wide_height > (std::numeric_limits<uint32_t>::max)() ||
      !safe::mul(wide_width, wide_height, &pixels) ||
      !safe::mul(pixels, size_t{4}, &bytes)) {
    if (error) *error = "UDIM atlas exceeds the 4096-pixel edge limit";
    return false;
  }
  const uint32_t width = static_cast<uint32_t>(wide_width);
  const uint32_t height = static_cast<uint32_t>(wide_height);
  if (width == 0 || height == 0 ||
      bytes == 0) return false;
  const size_t resident = scene->memory_usage();
  if (resident > resident_limit || bytes > resident_limit - resident) {
    if (error) *error = "UDIM atlas exceeds the remaining scene memory budget";
    return false;
  }
  std::vector<uint8_t> atlas(bytes, 0);
  for (const auto& tile : tiles) {
    const TextureImage& image = scene->images[static_cast<size_t>(tile.image_id)];
    const uint32_t u = (tile.udim - 1001) % 10;
    const uint32_t v = (tile.udim - 1001) / 10;
    const uint32_t dst_x = (u - min_u) * first->width;
    const uint32_t dst_y = (rows - 1 - (v - min_v)) * first->height;
    for (uint32_t y = 0; y < first->height; ++y) {
      for (uint32_t x = 0; x < first->width; ++x) {
        const size_t src = (static_cast<size_t>(y) * first->width + x) * image.channels;
        const size_t dst = (static_cast<size_t>(dst_y + y) * width + dst_x + x) * 4;
        if (image.channels == 1) {
          atlas[dst] = atlas[dst + 1] = atlas[dst + 2] = image.data[src];
          atlas[dst + 3] = 255;
        } else {
          atlas[dst] = image.data[src]; atlas[dst + 1] = image.data[src + 1];
          atlas[dst + 2] = image.data[src + 2];
          atlas[dst + 3] = image.channels == 4 ? image.data[src + 3] : 255;
        }
      }
    }
  }
  if (scene->images.size() >= static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return false;
  TextureImage combined;
  combined.name = first->name;
  combined.asset_identifier = identifier;
  combined.width = width;
  combined.height = height;
  combined.channels = 4;
  combined.component_type = ComponentType::UInt8;
  combined.color_space = first->color_space;
  if (!combined.data.append(atlas.data(), atlas.size())) return false;
  *image_id = static_cast<int32_t>(scene->images.size());
  scene->images.push_back(std::move(combined));
  *uv_scale = Float2(1.0f / static_cast<float>(cols),
                     1.0f / static_cast<float>(rows));
  *uv_offset = Float2(min_u == 0 ? 0.0f :
                          -static_cast<float>(min_u) / static_cast<float>(cols),
                      min_v == 0 ? 0.0f :
                          -static_cast<float>(min_v) / static_cast<float>(rows));
  return true;
}

static bool GetFloat2Local(const UsdPrim& prim, const std::string& name,
                           float* out2) {
  const ::lightusd::next::Value* v = GetAttribute(prim, name);
  if (!v) return false;
  const float* f = v->as_float2();
  if (!f) return false;
  out2[0] = f[0];
  out2[1] = f[1];
  return true;
}

void TraceTextureStChain(const Stage& stage, const UsdPrim& texture_prim,
                         TextureNodeData* out) {
  UsdPrim cur = texture_prim;
  std::string prop = "inputs:st";

  // Chained UsdTransform2d nodes COMPOSE (each node applies
  // uv' = R(rotation) * (scale * uv) + translation to its input). Accumulate
  // the affine composition walking from the texture (outermost) towards the
  // primvar reader (innermost): F_total(uv) = F_outer(F_inner(uv)).
  // A single node keeps its authored values verbatim (no decomposition).
  // Note: animated or connected translation/rotation/scale inputs are NOT
  // evaluated here (only directly-authored values are read).
  int transform_nodes = 0;
  double A[2][2] = {{1.0, 0.0}, {0.0, 1.0}};  // accumulated linear part
  double T[2] = {0.0, 0.0};                   // accumulated offset

  for (int hop = 0; hop < 4 && cur.IsValid(); ++hop) {
    const ::lightusd::next::PrimSpec* spec = cur.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* conns =
        spec ? spec->connection(prop) : nullptr;
    if (!conns || conns->empty()) break;
    const std::string next_path = SourcePrimPathFromConnection((*conns)[0].str());
    UsdPrim np = stage.GetPrimAtPath(next_path);
    if (!np.IsValid()) break;
    std::string id;
    GetToken(np, "info:id", &id);
    if (id == "UsdTransform2d") {
      out->has_transform2d = true;
      float tr[2] = {0.0f, 0.0f};
      GetFloat2Local(np, "inputs:translation", tr);
      float rot = 0.0f;
      GetFloat(np, "inputs:rotation", &rot);
      float sc[2] = {1.0f, 1.0f};
      GetFloat2Local(np, "inputs:scale", sc);

      if (transform_nodes == 0) {
        // First (outermost) node: keep the raw authored values so a single
        // Transform2d round-trips exactly.
        out->uv_translation[0] = tr[0];
        out->uv_translation[1] = tr[1];
        out->uv_rotation = rot;
        out->uv_scale[0] = sc[0];
        out->uv_scale[1] = sc[1];
      }

      // Local affine L(uv) = R * diag(sc) * uv + tr.
      const double rad = double(rot) * 3.14159265358979323846 / 180.0;
      const double c = std::cos(rad), s = std::sin(rad);
      const double L[2][2] = {{c * sc[0], -s * sc[1]},
                              {s * sc[0], c * sc[1]}};
      // Accumulated-so-far F is applied AFTER this (deeper) node:
      // F_new(uv) = F(L(uv)) => A_new = A*L, T_new = A*t_L + T.
      const double A00 = A[0][0] * L[0][0] + A[0][1] * L[1][0];
      const double A01 = A[0][0] * L[0][1] + A[0][1] * L[1][1];
      const double A10 = A[1][0] * L[0][0] + A[1][1] * L[1][0];
      const double A11 = A[1][0] * L[0][1] + A[1][1] * L[1][1];
      T[0] += A[0][0] * tr[0] + A[0][1] * tr[1];
      T[1] += A[1][0] * tr[0] + A[1][1] * tr[1];
      A[0][0] = A00; A[0][1] = A01; A[1][0] = A10; A[1][1] = A11;

      ++transform_nodes;
      cur = np;
      prop = "inputs:in";
      continue;
    }
    if (id.rfind("UsdPrimvarReader", 0) == 0) {
      out->uv_primvar = ::lightusd::next::GetPrimvarReaderVarname(stage, np);
      break;
    }
    break;
  }

  if (transform_nodes > 1) {
    // Decompose the composite affine back into the translate/rotate/scale
    // model RenderTexture carries (uv' = R * diag(scale) * uv + offset).
    // Composition of non-uniform scales and rotations can introduce shear,
    // which this model cannot represent; the QR-style decomposition below
    // drops it (best effort).
    out->uv_translation[0] = static_cast<float>(T[0]);
    out->uv_translation[1] = static_cast<float>(T[1]);
    const double sx = std::sqrt(A[0][0] * A[0][0] + A[1][0] * A[1][0]);
    if (sx > 1.0e-12) {
      const double c = A[0][0] / sx;
      const double s = A[1][0] / sx;
      const double sy = -s * A[0][1] + c * A[1][1];
      out->uv_rotation = static_cast<float>(
          std::atan2(s, c) * 180.0 / 3.14159265358979323846);
      out->uv_scale[0] = static_cast<float>(sx);
      out->uv_scale[1] = static_cast<float>(sy);
    } else {
      out->uv_rotation = 0.0f;
      out->uv_scale[0] = static_cast<float>(A[0][0]);
      out->uv_scale[1] = static_cast<float>(A[1][1]);
    }
  }
}

bool ExtractTextureNodeData(const Stage& stage,
                            const UsdPrim& texture_prim,
                            double time_code,
                            TextureNodeData* out) {
  if (!out || !texture_prim.IsValid()) return false;

  // Apply metadata from the property that actually authors the asset. Ptex
  // networks frequently forward a shader input through a material interface,
  // e.g. `inputs:file.connect = </Mat.inputs:surfaceMap>`, so that property is
  // not necessarily on the texture shader itself.
  auto apply_file_metadata = [out](const UsdPrim& owner,
                                   const std::string& property) {
    const ::lightusd::next::PropMeta* pm = owner.GetPropertyMeta(property);
    if (!pm) return;
    if (!pm->colorSpace.empty()) out->source_color_space = pm->colorSpace;
    if (const auto cd = pm->customData.as_dictionary()) {
      if (auto v = cd->find("ktx2")) {
        if (auto s = v->as_asset_path()) {
          out->ktx2_hint = *s;
        } else if (auto t = v->as_string()) {
          out->ktx2_hint = *t;
        }
      }
    }
  };

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);

  // Anchor of the prim that authors the selected property value. A texture
  // shader reached through a material interface holds only the connection, so
  // its own layer is the wrong base for a relative asset path.
  auto authoring_anchor = [&](const char* property) -> uint32_t {
    UsdPrim owner = texture_prim;
    std::string name = property;
    std::set<std::string> visited;
    for (int depth = 0; depth <= kMaxMtlxConstantDepth && owner.IsValid();
         ++depth) {
      const std::string visit_key = owner.GetPath().str() + "." + name;
      if (!visited.insert(visit_key).second) break;
      // Follow the connection chain to the property that authors the value.
      // (EvalAssetPath itself resolves connections, so asking it here would
      // stop at the first prim and return the shader's layer.)
      if (!eval.HasConnection(owner, name)) break;
      std::string prim_path;
      std::string next_property;
      if (!SplitConnectionPath(eval.GetConnectionPath(owner, name),
                               &prim_path, &next_property)) {
        break;
      }
      owner = stage.GetPrimAtPath(prim_path);
      name = std::move(next_property);
    }
    const auto* spec = owner.GetPrimSpec();
    return spec ? spec->asset_anchor_id() : 0u;
  };

  ::lightusd::next::UVTextureData uv;
  if (::lightusd::next::GetUVTextureData(stage, texture_prim, &uv, time_code)) {
    out->file = uv.file;
    out->wrap_s = uv.wrap_s;
    out->wrap_t = uv.wrap_t;
    out->source_color_space = uv.source_color_space;
    std::memcpy(out->scale, uv.scale, sizeof(out->scale));
    std::memcpy(out->bias, uv.bias, sizeof(out->bias));
    apply_file_metadata(texture_prim, "inputs:file");
    out->file_asset_anchor = authoring_anchor("inputs:file");
    out->file_asset_anchor_valid = true;
    TraceTextureStChain(stage, texture_prim, out);
    return !out->file.empty();
  }

  std::string shader_id;
  GetToken(texture_prim, "info:id", &shader_id);
  const bool materialx_image =
      shader_id == "image" || shader_id == "tiledimage" ||
      shader_id.rfind("ND_image_", 0) == 0 ||
      shader_id.rfind("ND_tiledimage_", 0) == 0;
  if (materialx_image) {
    // MaterialX image/tiledimage defaults are periodic in both directions.
    // Do not inherit UsdUVTexture's useMetadata -> clamp fallback: production
    // MaterialX assets commonly use coordinates outside [0,1] and rely on the
    // nodedef default even when no address-mode input is authored.
    out->wrap_s = "periodic";
    out->wrap_t = "periodic";
  }

  UsdPrim fileOwner;
  std::string fileAttribute;
  auto resolve_asset_input = [&](const std::string& initial,
                                 UsdPrim* resolved_owner,
                                 std::string* resolved_property)
      -> std::optional<std::string> {
    UsdPrim owner = texture_prim;
    std::string property = initial;
    std::set<std::string> visited;
    for (int depth = 0; depth <= kMaxMtlxConstantDepth && owner.IsValid();
         ++depth) {
      const std::string visit_key = owner.GetPath().str() + "." + property;
      if (!visited.insert(visit_key).second) break;

      std::optional<std::string> value = eval.EvalAssetPath(owner, property);
      if (!value) value = eval.EvalString(owner, property);
      if (value && !value->empty()) {
        *resolved_owner = owner;
        *resolved_property = property;
        return value;
      }
      if (!eval.HasConnection(owner, property)) break;

      std::string prim_path;
      std::string next_property;
      if (!SplitConnectionPath(eval.GetConnectionPath(owner, property),
                               &prim_path, &next_property)) {
        break;
      }
      owner = stage.GetPrimAtPath(prim_path);
      property = std::move(next_property);
    }
    return std::nullopt;
  };

  std::optional<std::string> file =
      resolve_asset_input("inputs:file", &fileOwner, &fileAttribute);
  if (!file) {
    file = resolve_asset_input("inputs:filename", &fileOwner, &fileAttribute);
  }
  if (!file || file->empty()) return false;
  out->file = *file;

  if (std::optional<std::string> wrap_s = eval.EvalToken(texture_prim, "inputs:wrapS")) {
    out->wrap_s = *wrap_s;
  }
  if (std::optional<std::string> wrap_t = eval.EvalToken(texture_prim, "inputs:wrapT")) {
    out->wrap_t = *wrap_t;
  }
  if (materialx_image) {
    auto address_mode = [&](const char* name) -> std::optional<std::string> {
      if (std::optional<std::string> value =
              eval.EvalToken(texture_prim, name)) {
        return value;
      }
      return eval.EvalString(texture_prim, name);
    };
    if (std::optional<std::string> mode =
            address_mode("inputs:uaddressmode")) {
      out->wrap_s = *mode;
    }
    if (std::optional<std::string> mode =
            address_mode("inputs:vaddressmode")) {
      out->wrap_t = *mode;
    }
  }
  float scale[4];
  if (eval.EvalFloat4(texture_prim, "inputs:scale", scale)) {
    std::memcpy(out->scale, scale, sizeof(out->scale));
  }
  float bias[4];
  if (eval.EvalFloat4(texture_prim, "inputs:bias", bias)) {
    std::memcpy(out->bias, bias, sizeof(out->bias));
  }
  if (std::optional<std::string> cs = eval.EvalToken(texture_prim, "inputs:sourceColorSpace")) {
    out->source_color_space = *cs;
  }
  apply_file_metadata(fileOwner, fileAttribute);
  // Follow the interface connection to the prim that authors the value; its
  // layer, not the texture shader's, anchors a relative asset path.
  out->file_asset_anchor = authoring_anchor(
      fileAttribute.empty() ? "inputs:filename" : fileAttribute.c_str());
  out->file_asset_anchor_valid = true;
  TraceTextureStChain(stage, texture_prim, out);

  return true;
}

}  // namespace shader_param_detail

} } }  // namespace lightusd::tydra::next
