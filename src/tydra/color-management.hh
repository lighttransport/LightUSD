// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "core/color-transform.hh"
#include "core/attr-metas.hh"
#include "core/path.hh"

#include <string>

namespace lightusd {
class Stage;

namespace tydra {
namespace color_management {

struct RenderingColorConfig {
  std::string render_settings_path;
  std::string working_space{"lin_rec709_scene"};
  color::ColorSpaceDesc working_definition;
  bool used_override{false};
  bool used_fallback{true};
};

// Resolve source color space with USD precedence: attribute metadata, inherited
// ColorSpaceAPI, then the linear Rec.709 fallback.
bool ComputeColorSpaceName(const Stage &stage, const Path &prim_path,
                           const AttrMetas *attribute_metadata,
                           std::string *name, bool *authored = nullptr);

bool ResolveColorSpaceDefinition(const Stage &stage, const Path &context_path,
                                 const std::string &name,
                                 color::ColorSpaceDesc *definition,
                                 std::string *error = nullptr);

bool BuildColorTransform(const Stage &stage, const Path &context_path,
                         const std::string &source,
                         const std::string &destination,
                         color::ColorTransform *transform,
                         std::string *error = nullptr);

// Select explicit override, stage renderSettingsPrimPath, or linear Rec.709.
// The rendering working space must be linear; invalid settings warn and fall
// back deterministically.
bool ResolveRenderingColorConfig(const Stage &stage,
                                 const std::string &override_path,
                                 RenderingColorConfig *config,
                                 std::string *warning = nullptr);

}  // namespace color_management
}  // namespace tydra
}  // namespace lightusd
