// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Immutable built-in type metadata

#pragma once

#include "type-id.hh"

namespace lightusd {
namespace next {

/// Immutable built-in type metadata. Variable-sized types have size == 0.
/// Lifetime operations belong to Value, not a per-type callback table.
struct TypeInfo {
  TypeId id;
  const char* name;
  const char* cpp_name;
  size_t size;
  size_t alignment;
  TypeId component_type;
  uint8_t component_count;
  uint8_t flags;  // Internal scalar/numeric classification; use query functions.
};

/// Get type info by TypeId
/// Returns nullptr for out-of-range TypeId; Invalid has a zero-size descriptor
/// O(1) lookup via static array indexing
const TypeInfo* GetTypeInfo(TypeId id);

/// Compatibility no-op: built-in metadata needs no runtime initialization.
void InitTypeRegistry();

}  // namespace next
}  // namespace lightusd
