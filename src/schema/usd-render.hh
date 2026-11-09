// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "stage/stage.hh"

#include <string>
#include <vector>

namespace lightusd {

struct RenderSettingsBaseData {
  std::vector<Path> camera;
  int32_t resolution[2] = {2048, 1080};
  float pixel_aspect_ratio = 1.0f;
  std::string aspect_ratio_conform_policy = "expandAperture";
  float data_window_ndc[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  bool instantaneous_shutter = false;
  bool disable_motion_blur = false;
  bool disable_depth_of_field = false;
};

struct RenderSettingsData : RenderSettingsBaseData {
  std::vector<Path> products;
  std::vector<std::string> included_purposes = {"default", "render"};
  std::vector<std::string> material_binding_purposes = {"full", ""};
  std::string rendering_color_space;
};

struct RenderProductData : RenderSettingsBaseData {
  std::string product_type = "raster";
  std::string product_name;
  std::vector<Path> ordered_vars;
};

struct RenderVarData {
  std::string data_type = "color3f";
  std::string source_name;
  std::string source_type = "raw";
};

struct RenderPassData {
  std::string pass_type;
  Value command;
  std::vector<Path> render_source;
  std::vector<Path> input_passes;
  std::vector<Path> render_visibility_includes;
  std::vector<Path> render_visibility_excludes;
  std::vector<Path> camera_visibility_includes;
  std::vector<Path> camera_visibility_excludes;
  std::string file_name;
  bool render_visibility_include_root = true;
  bool camera_visibility_include_root = true;
};

bool IsRenderSettings(const UsdPrim& prim);
bool IsRenderProduct(const UsdPrim& prim);
bool IsRenderVar(const UsdPrim& prim);
bool IsRenderPass(const UsdPrim& prim);

bool GetRenderSettingsData(const Stage& stage, const UsdPrim& prim,
                           RenderSettingsData* out, double time = 0.0);
bool GetRenderProductData(const Stage& stage, const UsdPrim& prim,
                          RenderProductData* out, double time = 0.0);
bool GetRenderVarData(const Stage& stage, const UsdPrim& prim,
                      RenderVarData* out, double time = 0.0);
bool GetRenderPassData(const Stage& stage, const UsdPrim& prim,
                       RenderPassData* out, double time = 0.0);

}  // namespace lightusd
