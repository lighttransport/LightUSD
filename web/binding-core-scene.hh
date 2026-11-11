// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <string>
#include <vector>
#include "core/minijson.hh"
namespace lightusd {
 class Path; class Value; class ValueView; class UsdPrim; class Stage;
namespace web_core {
minijson::Value ValueJSON(::lightusd::ValueView value);
const std::vector<::lightusd::Path>* PropertyConnections(
    const ::lightusd::UsdPrim& prim, const std::string& property_name);
std::string ConnectionPrimPath(const std::string& connection);
std::string BuildNodeGraphJson(const ::lightusd::UsdPrim& material,
    const ::lightusd::UsdPrim& shader, const std::string& version);
std::string CanonicalMaterialGraph(const ::lightusd::Stage& stage, const ::lightusd::UsdPrim& material);
void AppendPhysicsPrimJSON(const ::lightusd::UsdPrim& prim, minijson::Value* out);
}  // namespace web_core
}  // namespace lightusd
