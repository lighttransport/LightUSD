// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data-enums.hh"
#include "render-data-math.hh"
#include "chunked-array.hh"
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {

//
// RenderTexture
//
struct RenderTexture {
  std::string name;
  std::string prim_path;
  std::string asset_path;  // Original USD asset path

  // Legacy-safe GPU-compressed companion named by the `inputs:file` attribute's
  // `customData = { asset ktx2 = @foo.ktx2@ }` hint (see doc/texcomp.md). Empty
  // when unauthored. `asset_path` still points at the plain image, so unaware
  // consumers are unaffected; a consumer that can upload GPU blocks loads this
  // instead.
  std::string ktx2_hint;

  // UV transform
  Float2 offset = {0, 0};
  Float2 scale = {1, 1};
  float rotation = 0.0f;  // Radians

  // Legacy UVTexture record fields. Preserve whether a UsdTransform2d node was
  // authored separately from the effective identity transform, and retain
  // its legacy-facing degree/scale/translation components for C adapters.
  bool has_transform2d = false;
  float tx_rotation = 0.0f;  // Degrees, as authored by UsdTransform2d
  Float2 tx_scale = {1, 1};
  Float2 tx_translation = {0, 0};

  // Sparse UDIM identity. Tile IDs are 1001..1100 and index `udim_textures`;
  // image_id remains the lowest available tile for simple renderers.
  bool is_udim = false;
  int32_t udim_texture_id = -1;
  // UV remap for a combined UDIM atlas. Sparse textures retain identity.
  Float2 udim_uv_scale = {1, 1};
  Float2 udim_uv_offset = {0, 0};

  // UV set sampled by this texture (UsdPrimvarReader varname on the st
  // chain); empty = the default uv primvar ("st").
  std::string uv_primvar;

  // Sampling
  // Effective UsdUVTexture default: unauthored/useMetadata wrap is Clamp
  // (matches legacy/pxr; the converter always assigns via ParseWrapMode).
  WrapMode wrap_s = WrapMode::Clamp;
  WrapMode wrap_t = WrapMode::Clamp;

  // Bias/scale for texture values
  Float4 bias = {0, 0, 0, 0};
  Float4 scale_value = {1, 1, 1, 1};

  // Image reference
  int32_t image_id = -1;

  // Authored colorspace: colorSpace asset metadata on inputs:file when
  // present, else inputs:sourceColorSpace ("auto"/"sRGB"/"raw"/...). Web
  // consumers decode in the browser and need the authored intent.
  std::string source_color_space = "auto";

  // Canonical working space selected from RenderSettings. Texture consumers
  // can use this with source_color_space to perform the decode/gamut step
  // before shading. Data/raw textures intentionally leave this as "data".
  std::string target_color_space = "lin_rec709_scene";

  // Resolved source -> display-linear Rec.709 transform. Unlike the token
  // alone, this preserves custom ColorSpaceDefinitionAPI chromaticities for
  // native and Web texture consumers after the Stage has gone away.
  bool color_transform_valid = false;
  bool color_transform_bypass = true;
  bool source_color_is_data = false;
  float source_gamma = 1.0f;
  float source_linear_bias = 0.0f;
  float source_to_display_linear[9] = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};

  // Which channel to use (for single-channel textures)
  enum class Channel : uint8_t { R = 0, G, B, A, RGB, RGBA };
  Channel output_channel = Channel::RGBA;
};

struct RenderUDIMTexture {
  struct Tile {
    uint32_t udim = 1001;
    int32_t image_id = -1;
  };
  std::string prim_name;
  std::string abs_path;
  std::string display_name;
  std::string asset_identifier;
  std::vector<Tile> tiles;
};

//
// TextureImage - actual image data
//
struct TextureImage {
  std::string name;
  std::string resolved_path;  // Filesystem path
  std::string asset_identifier;  // Authored asset path, when available

  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 4;
  uint8_t mip_levels = 1;

  ComponentType component_type = ComponentType::UInt8;
  ColorSpace color_space = ColorSpace::sRGB;

  // Raw image data (may be empty if not loaded)
  UInt8Chunked data;

  bool is_loaded() const { return !data.empty(); }
  size_t memory_usage() const { return data.memory_usage(); }
};

}}} // namespace lightusd::tydra::next
