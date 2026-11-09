// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "next/diff/layer-diff.hh"
int DiffCollections(const std::string& left, const std::string& right,
                    const lightusd::next::DiffOptions& options, bool json, bool quiet);
bool IsDiffDirectory(const std::string& path);
