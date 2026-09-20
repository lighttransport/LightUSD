// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "minijson.hh"
#include <string>

namespace lusdchecker {
// Adds baselineState to issues and new/existing counts; validity still means
// whole-input validity. gatePassed is the CI result after baseline comparison.
bool ApplyBaseline(lightusd::minijson::Value* report, const std::string& path,
                   std::string* error);
lightusd::minijson::Value ToSarif(const lightusd::minijson::Value& report);
}  // namespace lusdchecker
