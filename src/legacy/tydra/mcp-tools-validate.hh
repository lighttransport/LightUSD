// SPDX-License-Identifier: Apache 2.0
// Copyright 2026 Light Transport Entertainment Inc.
#pragma once

#include <string>

#include "external/jsonhpp/nlohmann/json_fwd.hpp"

#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

/// usd_validate: Validate USD content against AOUSD Core semantic rules.
///
/// Args (all optional; first present wins for the input source):
///   { data?:       string  (base64-encoded USD),
///     uri?:        string  (file path; native builds only),
///     layer_uuid?: string  (a previously-loaded layer in the session),
///     groups?:     string[] (subset of
///                            ["core","geom","shade","lux","physics","crate"],
///                            or ["all"]; default ["core"]),
///     name?:       string  (filename hint for `data`) }
///
/// With none of data/uri/layer_uuid provided, validates the current session
/// stage (serialized to USDA and re-parsed as an uncomposed Layer).
///
/// Result: { ok, error_count, warning_count, spec_version, source,
///           checked_groups,
///           issues: [{ severity, rule_id, location, message }] }
bool UsdValidate(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err);

} // namespace mcp
} // namespace tydra
} // namespace lightusd
