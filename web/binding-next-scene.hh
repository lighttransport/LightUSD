// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <string>
#include <vector>
#include "minijson.hh"
#include "next/prim/path.hh"
namespace lightusd {
namespace next { class Value; class UsdPrim; class Stage; }
namespace web_next {
minijson::Value NextValueJSON(const next::Value& value);
const std::vector<next::Path>* NextPropertyConnections(
    const next::UsdPrim& prim, const std::string& property_name);
std::string NextConnectionPrimPath(const std::string& connection);
std::string BuildNextNodeGraphJson(const next::UsdPrim& material,
    const next::UsdPrim& shader, const std::string& version);
std::string CanonicalMaterialGraph(const next::Stage& stage, const next::UsdPrim& material);
void AppendNextPhysicsPrimJSON(const next::UsdPrim& prim, minijson::Value* out);
}  // namespace web_next
}  // namespace lightusd
