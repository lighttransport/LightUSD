// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Shared helpers for the next MCP tools: base64 with request limits, session
// lookups, next value <-> JSON conversion, and prim-tree JSON.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mcp-context.hh"

namespace lightusd {
namespace next {
class Stage;
class UsdPrim;
class Value;
}  // namespace next

namespace mcp {

// Request limits (same values as the legacy MCP server).
constexpr size_t kMaxBase64InputBytes = 64 * 1024 * 1024;
constexpr size_t kMaxBase64DecodedBytes = 48 * 1024 * 1024;

bool DecodeBase64(const std::string& in, std::string* out, std::string* err);
std::string EncodeBase64(const uint8_t* data, size_t size);
std::string GenerateUUID();

// Read-only view of a C stage (nullptr for an empty reference).
const next::Stage* NativeStage(const StageRef& ref);

// A prim found in the session: the session stage first, then loaded layers.
struct PrimSite {
  lightusd_stage* stage = nullptr;
  std::string layer;  // "" for the session stage, else the layer name
};
bool FindPrimSite(Context& ctx, const std::string& path, PrimSite* site,
                  std::string* err);
std::string FindLayerUUID(const Context& ctx, const std::string& name);

// Session stage presence check shared by the stage tools.
bool RequireStage(const Context& ctx, std::string* err);

// {"type": "<usd type>", "value": <json>, "usda": "<literal>"}; arrays longer
// than `max_elements` (0 = no limit) are truncated and marked.
json ValueToJSON(const next::Value& value, const std::string& type_name,
                 size_t max_elements = 1000);

// Parse {"type": "<usd type>", "value": <json>} (a string value for a
// non-string type is taken as a USDA literal) into a next value.
bool ValueFromJSON(const json& j, std::string* type_name, next::Value* out,
                   std::string* err);

const char* SpecifierName(int specifier);
json PrimToJSON(const next::UsdPrim& prim, lightusd_stage* stage,
                int max_depth, bool include_attributes);
json PrimMetaToJSON(const next::UsdPrim& prim);
// Declared type name of a property ("" when unknown).
std::string PropertyTypeName(const next::UsdPrim& prim, const std::string& name);

// Tool result helper: {"content": [{"type": "text", "text": text}]}.
void SetTextContent(json& result, const std::string& text,
                    const char* mime_type = nullptr);

}  // namespace mcp
}  // namespace lightusd
