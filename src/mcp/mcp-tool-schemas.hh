// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// MCP tool definitions (name, description, JSON input schema) shared by the
// legacy tydra MCP server and the next MCP product, so both advertise the
// same tools/list contract.
#pragma once

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "external/jsonhpp/nlohmann/json.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace mcp_schema {

// Fill result["tools"] with every tool definition.
void AppendToolSchemas(nlohmann::json &result);

}  // namespace mcp_schema
}  // namespace lightusd
