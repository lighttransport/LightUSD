// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// list-op.hh - ListOp template for USD list operations
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "value-types.hh"

namespace lightusd {

template <typename T>
class ListOp {
 public:
  ListOp() : is_explicit(false) {}

  void ClearAndMakeExplicit() {
    explicit_items.clear();
    added_items.clear();
    prepended_items.clear();
    appended_items.clear();
    deleted_items.clear();
    ordered_items.clear();

    has_explicit = false;
    has_added = false;
    has_prepended = false;
    has_appended = false;
    has_deleted = false;
    has_ordered = false;

    is_explicit = true;
  }

  bool IsExplicit() const { return is_explicit; }

  // A bucket can be AUTHORED YET EMPTY -- `delete rel myheight` is a list-edit
  // qualifier on a relationship with no targets at all -- and `size() > 0`
  // cannot tell that apart from "never authored", which is why the qualifier was
  // dropped on write. Set*Items() is the only way to populate a bucket, so the
  // flag it sets is exact; the size() term keeps any direct-mutation path honest.
  bool HasExplicitItems() const { return has_explicit || explicit_items.size(); }

  bool HasAddedItems() const { return has_added || added_items.size(); }

  bool HasPrependedItems() const { return has_prepended || prepended_items.size(); }

  bool HasAppendedItems() const { return has_appended || appended_items.size(); }

  bool HasDeletedItems() const { return has_deleted || deleted_items.size(); }

  bool HasOrderedItems() const { return has_ordered || ordered_items.size(); }

  const std::vector<T> &GetExplicitItems() const { return explicit_items; }

  const std::vector<T> &GetAddedItems() const { return added_items; }

  const std::vector<T> &GetPrependedItems() const { return prepended_items; }

  const std::vector<T> &GetAppendedItems() const { return appended_items; }

  const std::vector<T> &GetDeletedItems() const { return deleted_items; }

  const std::vector<T> &GetOrderedItems() const { return ordered_items; }

  void SetExplicitItems(const std::vector<T> &v) { explicit_items = v; has_explicit = true; }

  void SetAddedItems(const std::vector<T> &v) { added_items = v; has_added = true; }

  void SetPrependedItems(const std::vector<T> &v) { prepended_items = v; has_prepended = true; }

  void SetAppendedItems(const std::vector<T> &v) { appended_items = v; has_appended = true; }

  void SetDeletedItems(const std::vector<T> &v) { deleted_items = v; has_deleted = true; }

  void SetOrderedItems(const std::vector<T> &v) { ordered_items = v; has_ordered = true; }

 private:
  bool is_explicit{false};

  // "This bucket was authored", independent of whether it has any items.
  bool has_explicit{false};
  bool has_added{false};
  bool has_prepended{false};
  bool has_appended{false};
  bool has_deleted{false};
  bool has_ordered{false};

  std::vector<T> explicit_items;
  std::vector<T> added_items;
  std::vector<T> prepended_items;
  std::vector<T> appended_items;
  std::vector<T> deleted_items;
  std::vector<T> ordered_items;
};

struct ListOpHeader {
  enum Bits {
    IsExplicitBit = 1 << 0,
    HasExplicitItemsBit = 1 << 1,
    HasAddedItemsBit = 1 << 2,
    HasDeletedItemsBit = 1 << 3,
    HasOrderedItemsBit = 1 << 4,
    HasPrependedItemsBit = 1 << 5,
    HasAppendedItemsBit = 1 << 6
  };

  ListOpHeader() : bits(0) {}

  explicit ListOpHeader(uint8_t b) : bits(b) {}

  // Implicit copy so NRVO can apply at return sites without requiring
  // explicit reconstruction. Body preserved for parity with previous
  // per-flag rebuild path.
  ListOpHeader(ListOpHeader const &op) : bits(0) {
    bits |= op.IsExplicit() ? IsExplicitBit : 0;
    bits |= op.HasExplicitItems() ? HasExplicitItemsBit : 0;
    bits |= op.HasAddedItems() ? HasAddedItemsBit : 0;
    bits |= op.HasPrependedItems() ? HasPrependedItemsBit : 0;
    bits |= op.HasAppendedItems() ? HasAppendedItemsBit : 0;
    bits |= op.HasDeletedItems() ? HasDeletedItemsBit : 0;
    bits |= op.HasOrderedItems() ? HasOrderedItemsBit : 0;
  }

  bool IsExplicit() const { return bits & IsExplicitBit; }

  bool HasExplicitItems() const { return bits & HasExplicitItemsBit; }
  bool HasAddedItems() const { return bits & HasAddedItemsBit; }
  bool HasPrependedItems() const { return bits & HasPrependedItemsBit; }
  bool HasAppendedItems() const { return bits & HasAppendedItemsBit; }
  bool HasDeletedItems() const { return bits & HasDeletedItemsBit; }
  bool HasOrderedItems() const { return bits & HasOrderedItemsBit; }

  uint8_t bits;
};

///
/// Compose (reduce) a ListOp into a flat result vector.
///
/// Per AOUSD Core Spec 6.6.3:
///   - If explicit: result = explicit_items
///   - Otherwise: result = prepended + added + (existing minus deleted) + appended
///   - Then apply ordering if ordered_items present
///
/// @param[in] op The ListOp to reduce
/// @param[in] existing Existing items from weaker opinion (default: empty)
/// @return The composed flat list
///
template <typename T>
std::vector<T> ComposeListOp(const ListOp<T> &op,
                              const std::vector<T> &existing = {}) {
  if (op.IsExplicit()) {
    return op.GetExplicitItems();
  }

  std::vector<T> result;
  result.reserve(op.GetPrependedItems().size() + existing.size() +
                 op.GetAddedItems().size() + op.GetAppendedItems().size());

  // Prepend
  const auto &prepended = op.GetPrependedItems();
  for (const auto &item : prepended) {
    result.push_back(item);
  }

  // Existing items minus deleted, minus already prepended
  const auto &deleted = op.GetDeletedItems();
  for (const auto &item : existing) {
    // Skip deleted
    bool skip = false;
    for (const auto &d : deleted) {
      if (item == d) { skip = true; break; }
    }
    if (skip) continue;
    // Skip already prepended
    for (const auto &p : prepended) {
      if (item == p) { skip = true; break; }
    }
    if (skip) continue;
    result.push_back(item);
  }

  // Added items not already present in result
  for (const auto &item : op.GetAddedItems()) {
    bool found = false;
    for (const auto &r : result) {
      if (item == r) { found = true; break; }
    }
    if (!found) {
      result.push_back(item);
    }
  }

  // Append: remove existing then re-add at end
  for (const auto &item : op.GetAppendedItems()) {
    auto it = std::remove(result.begin(), result.end(), item);
    result.erase(it, result.end());
    result.push_back(item);
  }

  // Ordering — use flag-vector to avoid the second O(M×N) pass
  if (op.HasOrderedItems()) {
    const auto &order = op.GetOrderedItems();
    std::vector<T> ordered;
    ordered.reserve(result.size());

    std::vector<bool> placed(result.size(), false);

    // First add items in the specified order
    for (const auto &o : order) {
      for (size_t i = 0; i < result.size(); i++) {
        if (!placed[i] && result[i] == o) {
          ordered.push_back(o);
          placed[i] = true;
          break;
        }
      }
    }

    // Then add remaining items not in the order list (O(M) pass)
    for (size_t i = 0; i < result.size(); i++) {
      if (!placed[i]) {
        ordered.push_back(result[i]);
      }
    }

    result = std::move(ordered);
  }

  return result;
}

// Forward declarations needed for ListOp type traits
class Path;
struct Reference;
struct Payload;

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(ListOp<value::token>, "ListOpToken", TYPE_ID_LIST_OP_TOKEN,
                  1);
DEFINE_TYPE_TRAIT(ListOp<std::string>, "ListOpString", TYPE_ID_LIST_OP_STRING,
                  1);
DEFINE_TYPE_TRAIT(ListOp<Path>, "ListOpPath", TYPE_ID_LIST_OP_PATH, 1);
DEFINE_TYPE_TRAIT(ListOp<Reference>, "ListOpReference",
                  TYPE_ID_LIST_OP_REFERENCE, 1);
DEFINE_TYPE_TRAIT(ListOp<int32_t>, "ListOpInt", TYPE_ID_LIST_OP_INT, 1);
DEFINE_TYPE_TRAIT(ListOp<uint32_t>, "ListOpUInt", TYPE_ID_LIST_OP_UINT, 1);
DEFINE_TYPE_TRAIT(ListOp<int64_t>, "ListOpInt64", TYPE_ID_LIST_OP_INT64, 1);
DEFINE_TYPE_TRAIT(ListOp<uint64_t>, "ListOpUInt64", TYPE_ID_LIST_OP_UINT64, 1);
DEFINE_TYPE_TRAIT(ListOp<Payload>, "ListOpPayload", TYPE_ID_LIST_OP_PAYLOAD, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
