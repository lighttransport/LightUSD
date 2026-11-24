// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Private compiled shader/texture resolution helpers. Not installed.
#pragma once
#include "render-data.hh"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace lightusd {  class Stage; class UsdPrim;  }

namespace lightusd { namespace tydra {
using ::lightusd::Stage;
using ::lightusd::UsdPrim;
namespace shader_param_detail {
struct TextureNodeData {
  std::string file;
  // `inputs:file` customData { asset ktx2 = @...@ } — the legacy-safe compressed
  // companion hint (doc/texcomp.md). Empty when unauthored.
  std::string ktx2_hint;
  std::string wrap_s = "useMetadata";
  std::string wrap_t = "useMetadata";
  float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  std::string source_color_space = "auto";
  // Anchor of the prim that actually authors `inputs:file`. Ptex/material
  // interface forwarding (`inputs:file.connect = </Mat.inputs:surfaceMap>`)
  // puts the asset value on the material prim, whose layer is not necessarily
  // the texture shader's layer; resolving against the shader's layer silently
  // drops the texture.
  uint32_t file_asset_anchor = 0;
  // 0 is also the root-layer anchor, so validity needs its own flag.
  bool file_asset_anchor_valid = false;
  // From the inputs:st chain (UsdTransform2d -> UsdPrimvarReader_float2):
  std::string uv_primvar;               // varname of the primvar reader
  float uv_translation[2] = {0.0f, 0.0f};
  float uv_rotation = 0.0f;             // degrees (UsdTransform2d convention)
  float uv_scale[2] = {1.0f, 1.0f};
  bool has_transform2d = false;
};

bool FindConnectedUtilityScalar(const Stage& stage, const UsdPrim& shader,
                                const std::string& shader_input,
                                const std::string& node_id_prefix,
                                const std::string& node_input,
                                double time_code, float* out);
bool CombineUDIMTileImages(RenderScene* scene,
                           const std::vector<RenderUDIMTexture::Tile>& tiles,
                           const std::string& identifier,
                           size_t resident_limit, int32_t* image_id,
                           Float2* uv_scale, Float2* uv_offset,
                           std::string* error);
bool ExtractTextureNodeData(const Stage& stage,
                            const UsdPrim& texture_prim,
                            double time_code,
                            TextureNodeData* out);
}  // namespace shader_param_detail

 } }  // namespace lightusd::tydra
