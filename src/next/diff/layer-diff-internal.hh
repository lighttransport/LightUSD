// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Compiled comparison primitives shared by structural diff helpers. Not installed.
#pragma once
#include "layer-diff.hh"
namespace lightusd { namespace next { namespace diff_detail {
constexpr uint32_t kMaxTraversalDepth = 1024;
std::string JoinPrimPath(const std::string& parent, const std::string& child);
bool AssetPathStringsEquivalentForDiff(const std::string& lhs,
                                       const std::string& rhs);
bool ScalarAlmostEqual(float a, float b, const DiffOptions& options);
bool ScalarAlmostEqual(double a, double b, const DiffOptions& options);
bool ValuesEquivalentForDiff(const Value& lhs, const Value& rhs,
                             const DiffOptions& options, int depth = 0);
bool DictsHaveTypeConflict(const Value& lhs, const Value& rhs);
bool CompareDictValues(const Value& lhs, const Value& rhs,
                       const DiffOptions& options,
                       std::vector<std::string>* changed);
} } }
