// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
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
namespace mcp {
using json = nlohmann::json;
}  // namespace mcp
}  // namespace lightusd
