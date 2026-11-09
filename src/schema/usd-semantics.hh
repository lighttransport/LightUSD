// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "stage/stage.hh"

#include <string>
#include <vector>

namespace lightusd {
namespace next {

bool HasSemanticsLabelsAPI(const UsdPrim& prim,
                           const std::string& instance_name);
bool GetSemanticsLabels(const Stage& stage, const UsdPrim& prim,
                        const std::string& instance_name,
                        std::vector<std::string>* out,
                        double time = 0.0);

}  // namespace next
}  // namespace lightusd
