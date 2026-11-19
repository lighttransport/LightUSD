// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#pragma once

#include "stage/stage.hh"

#include <string>
#include <vector>

namespace lightusd {

bool HasSemanticsLabelsAPI(const UsdPrim& prim,
                           const std::string& instance_name);
bool GetSemanticsLabels(const Stage& stage, const UsdPrim& prim,
                        const std::string& instance_name,
                        std::vector<std::string>* out,
                        double time = 0.0);

/// UsdSemanticsLabelsAPI::ComputeInheritedTaxonomies: sorted unique
/// SemanticsLabelsAPI instance names applied on `prim` or its ancestors.
std::vector<std::string> ComputeInheritedTaxonomies(const Stage& stage, const UsdPrim& prim);

/// UsdSemanticsLabelsQuery::ComputeUnique{Direct,Inherited}Labels for
/// `taxonomy`, sorted and unique. With `interval` the labels are gathered at
/// every authored sample time in [t0, t1] plus t0 (UsdSemanticsLabelsQuery
/// built from a GfInterval); otherwise at `t0`.
std::vector<std::string> ComputeUniqueSemanticsLabels(const Stage& stage, const UsdPrim& prim,
                                                      const std::string& taxonomy,
                                                      bool inherited, double t0,
                                                      bool interval = false,
                                                      double t1 = 0.0);

}  // namespace lightusd
