// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <string>
#include <vector>
#include "core/minijson.hh"
namespace lightusd {
 class Path; class Value; class ValueView; class UsdPrim; class Stage; 
namespace web_next {
minijson::Value NextValueJSON(::lightusd::ValueView value);
const std::vector<::lightusd::Path>* NextPropertyConnections(
    const ::lightusd::UsdPrim& prim, const std::string& property_name);
std::string NextConnectionPrimPath(const std::string& connection);
std::string BuildNextNodeGraphJson(const ::lightusd::UsdPrim& material,
    const ::lightusd::UsdPrim& shader, const std::string& version);
std::string CanonicalMaterialGraph(const ::lightusd::Stage& stage, const ::lightusd::UsdPrim& material);
void AppendNextPhysicsPrimJSON(const ::lightusd::UsdPrim& prim, minijson::Value* out);
}  // namespace web_next
}  // namespace lightusd
