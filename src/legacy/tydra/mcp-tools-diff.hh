// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// mcp-tools-diff.hh - MCP tools for value-level USD layer diffing.
//
#pragma once

#include <string>

#include "mcp-context.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {
namespace mcp {

// --- MCP tool handlers (dispatched from CallTool) ---

// diff_open: load two USD sources (path / base64 data / loaded-layer uuid),
// optionally flatten, run the value-level diff, cache it in ctx.diff, and
// return a summary (counts + reason tally).
bool DiffOpen(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

// diff_summary: counts + reason tally for the cached diff.
bool DiffSummary(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err);

// diff_paths: filtered, paginated list of changed paths.
//   args: { reason?, path_substr?, kind?(added|deleted|modified), offset?, limit? }
bool DiffPaths(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
               std::string &err);

// diff_prim: full detail (reasons + lhs/rhs values) for one prim path.
bool DiffPrim(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

// diff_tree: subtree rollup of change counts under a path (navigable overview).
//   args: { path?(default "/"), depth?(default 2) }
bool DiffTree(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

// diff_text / diff_json: full rendered diff (escape hatch; token-heavy).
bool DiffText(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);
bool DiffJson(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
              std::string &err);

// --- Shared query helpers (also used by the lightusd.diff.* JS bindings) ---
// These operate on an already-computed DiffSession and never fail.

void DiffComputeSummary(const DiffSession &d, nlohmann::json &out);
void DiffComputePaths(const DiffSession &d, const nlohmann::json &filter,
                      nlohmann::json &out);
void DiffComputePrim(const DiffSession &d, const std::string &path,
                     nlohmann::json &out);
void DiffComputeTree(const DiffSession &d, const std::string &root, int depth,
                     nlohmann::json &out);

}  // namespace mcp
}  // namespace tydra
}  // namespace lightusd
