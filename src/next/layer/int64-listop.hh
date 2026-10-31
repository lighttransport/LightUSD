// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../types/value.hh"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace lightusd { namespace next {
struct DecodedExtensionField;

// AOUSD ListOp wire order: explicit, added, prepended, appended, deleted,
// ordered. Retain the header, including authored empty runs, for layer writes.
struct Int64ListOp {
  uint8_t bits = 1;
  std::array<Value, 6> items;
  bool is_explicit() const { return (bits & 3u) != 0; }
};
constexpr uint8_t kInt64ListOpBits[6] = {2, 4, 32, 64, 8, 16};

// Strongest first. Retain edits until every weaker opinion is known: resolving
// a delete against an intermediate empty list would lose its effect later.
struct Int64ListOpStack { std::vector<Int64ListOp> ops; };
Value ApplyInt64ListOps(const Int64ListOpStack& stack);
size_t Int64ListOpsMemoryUsage(const Int64ListOpStack& stack);
DecodedExtensionField MakeInt64ListOpField(std::string name, Int64ListOp op);
void ResolveInt64ListOpFields(std::vector<DecodedExtensionField>* fields);
} }
