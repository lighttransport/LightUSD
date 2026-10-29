// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
/// @file prim-reconstruct-common.hh
/// @brief Common property reconstruction utilities
///
/// Provides helper functions and descriptor types for prim property
/// reconstruction, reducing boilerplate in prim-reconstruct.cc.
///

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <set>

#include "core/property.hh"  // Property (transitively: attribute, relationship, path, prim-enums)

namespace lightusd {
namespace prim {

/// Set of property names a schema reconstructor has already consumed.
///
/// std::less<> gives heterogeneous lookup, so count() can be called with a
/// string literal or a tstring_view without constructing a temporary
/// std::string - the reconstruct dispatch loop probes every candidate name of
/// every authored property.
using ParsePropertyTable = std::set<std::string, std::less<>>;

/// Result codes for property parsing
enum class PropertyParseResult {
  Success,           // Property parsed successfully
  AlreadyProcessed,  // Property was already handled
  Unmatched,         // Property name doesn't match
  TypeError,         // Type mismatch
  Error              // General error
};

/// Descriptor for a simple typed attribute
/// Use with ParseTypedAttributeFromDescriptors()
template <typename PrimT, typename AttrT>
struct TypedAttributeDescriptor {
  const char* name;
  AttrT PrimT::*member;
};

/// Helper to create a descriptor
template <typename PrimT, typename AttrT>
constexpr TypedAttributeDescriptor<PrimT, AttrT> MakeAttrDesc(
    const char* name, AttrT PrimT::*member) {
  return {name, member};
}

/// Parse typed attributes from a descriptor list
/// Returns true if property was handled (matched and parsed)
template <typename PrimT, typename AttrT, size_t N>
bool ParseTypedAttributeFromDescriptors(
    ParsePropertyTable& table,
    const std::string& prop_name,
    const Property& prop,
    PrimT* /*prim*/,
    const std::array<TypedAttributeDescriptor<PrimT, AttrT>, N>& descriptors,
    std::string* err) {

  for (const auto& desc : descriptors) {
    if (prop_name == desc.name) {
      if (table.count(prop_name)) {
        return true;  // Already processed
      }

      if (!prop.is_attribute()) {
        if (err) {
          *err = "Property `" + prop_name + "` is not an attribute";
        }
        return false;
      }

      // TODO: Add actual parsing logic here
      // For now, this is a pattern demonstration
      table.insert(prop_name);
      return true;
    }
  }

  return false;  // Not matched
}

/// Count properties in a prim type that follow the PARSE_TYPED_ATTRIBUTE pattern
/// Useful for estimating refactoring scope
template <typename PrimT>
struct PropertyCount {
  static constexpr size_t typed_attributes = 0;
  static constexpr size_t relationships = 0;
  static constexpr size_t enum_properties = 0;
};

// Example specialization - uncomment and fill in for specific prims
// template <>
// struct PropertyCount<GeomCamera> {
//   static constexpr size_t typed_attributes = 15;
//   static constexpr size_t relationships = 1;
//   static constexpr size_t enum_properties = 2;
// };

}  // namespace prim
}  // namespace lightusd
