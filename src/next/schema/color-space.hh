// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "../stage/stage.hh"
#include "core/color-transform.hh"

#include <string>

namespace lightusd {
namespace next {
namespace color_management {

using ::lightusd::color::ColorSpaceDesc;
using ::lightusd::color::ColorTransform;

struct RenderingColorConfig {
  std::string render_settings_path;
  std::string working_space = "lin_rec709_scene";
  ColorSpaceDesc working_definition;
  bool used_override = false;
  bool used_fallback = true;
};

// Resolve an attribute's source color-space token using OpenUSD precedence.
// `authored` distinguishes an explicit USD opinion from the default fallback.
bool ComputeColorSpaceName(const UsdPrim &prim, const std::string &property,
                           std::string *name, bool *authored = nullptr);

// Resolve a canonical or ancestor-visible custom definition for `context`.
bool ResolveColorSpaceDefinition(const UsdPrim &context,
                                 const std::string &name,
                                 ColorSpaceDesc *definition,
                                 std::string *error = nullptr);

bool BuildColorTransform(const UsdPrim &context,
                         const std::string &source,
                         const std::string &destination,
                         ColorTransform *transform,
                         std::string *error = nullptr);

// Select: explicit override > stage renderSettingsPrimPath > renderer default.
// Invalid settings produce a warning and the linear Rec.709 fallback.
bool ResolveRenderingColorConfig(const Stage &stage,
                                 const std::string &override_path,
                                 RenderingColorConfig *config,
                                 std::string *warning = nullptr);

}  // namespace color_management
}  // namespace next
}  // namespace lightusd
