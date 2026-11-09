// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Internal declarations shared between render-data-material*.cc.
// Carries the MaterialX node-graph evaluator types + entry points used by both
// render-data-material.cc and render-data-material-mtlx.cc.
//
#pragma once

#include <array>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"
#include "core/prim.hh"     // Prim, Stage, Path
#include "tydra/render-data.hh"

namespace lightusd {
namespace tydra {

#if defined(LIGHTUSD_WITH_TEXTOOLS)
bool IsKTX2AssetPath(const std::string &path);
bool LoadKTX2CompressedBlocks(const AssetResolutionResolver &resolver,
                              const value::AssetPath &assetPath,
                              TextureImage *texImage,
                              std::vector<uint8_t> *out_bytes,
                              std::string *warn, std::string *err);
#endif

struct MtlxNodeGraphInfo {
  float tangent_rotation{0.0f};      // From ND_rotate3d_vector3 node's "amount" input (degrees)
  float normal_map_scale{1.0f};      // From ND_normalmap_float node's "scale" input
  bool has_normal_map{false};        // True if ND_normalmap node was found in the chain
  bool has_tangent_rotation{false};  // True if ND_rotate3d_vector3 node was found
  std::string normal_map_texture;    // Path to normal map texture asset
  std::string geomprop_name;         // From ND_geompropvalue node's "geomprop" input (primvar name)
  bool has_geomprop{false};          // True if ND_geompropvalue node was found
  int texcoord_index{0};             // From ND_texcoord node's "index" input (UV set index)
  std::array<float, 2> uvtiling{{1.0f, 1.0f}};  // From ND_tiledimage's "uvtiling" input
  std::array<float, 2> uvoffset{{0.0f, 0.0f}};  // From ND_tiledimage's "uvoffset" input
  bool has_uvtransform{false};       // True if non-default tiling/offset was found
  std::array<float, 4> constant_value{{0.0f, 0.0f, 0.0f, 0.0f}};  // From ND_constant terminal node
  int constant_components{0};       // Number of components: 1=float, 2=float2, 3=color3f/float3, 4=color4f/float4
  bool has_constant{false};         // True if ND_constant node was found
};

struct MtlxConstVal {
  std::array<float, 4> v{{0.0f, 0.0f, 0.0f, 0.0f}};
  int n{0};  // number of components: 1=float, 2=vector2, 3=color3, 4=color4
  bool color_managed{false};

  static MtlxConstVal Float(float f) { MtlxConstVal r; r.v[0]=f; r.n=1; return r; }
  static MtlxConstVal Color3(float r, float g, float b) {
    MtlxConstVal c; c.v = {{r, g, b, 0.0f}}; c.n = 3; return c;
  }
  static MtlxConstVal Color4(float r, float g, float b, float a) {
    MtlxConstVal c; c.v = {{r, g, b, a}}; c.n = 4; return c;
  }

  bool is_float() const { return n == 1; }
  bool is_color3() const { return n == 3; }
  bool is_color4() const { return n == 4; }
  float as_float() const { return v[0]; }
};

// MaterialX node-graph evaluation entry points (defined in render-data-material-mtlx.cc).
nonstd::expected<MtlxConstVal, std::string> EvaluateMtlxNodeGraphAsConstant(
    const Stage &stage, const Path &connection_path,
    const std::string &evaluation_space);

nonstd::expected<MtlxNodeGraphInfo, std::string> ExtractMtlxNodeGraphInfo(
    const Stage &stage, const Prim *material_prim,
    const std::vector<Path> &connections, std::string *err);

}  // namespace tydra
}  // namespace lightusd
