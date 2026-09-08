// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "../types/type-id.hh"
#include <string>

namespace lightusd {
namespace next {
TypeId ParseTypeName(const std::string& type_name, bool& is_array) {
  size_t size = type_name.size();
  is_array = size > 2 && type_name[size - 2] == '[' && type_name[size - 1] == ']';
  if (is_array) size -= 2;
  const TypeId id = GetTypeIdFromName(type_name.data(), size);
  // Internal metadata descriptors are not authored USDA value types.
  return id == TypeId::Extent || id == TypeId::Reference ? TypeId::Invalid : id;
}
}  // namespace next
}  // namespace lightusd
