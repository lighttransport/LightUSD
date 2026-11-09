// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "layer-diff-internal.hh"
#include "../c/lightusd-core-c.h"
#include "../types/type-info.hh"
#include "../types/value.hh"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
// This translation unit performs exact bit-level value comparisons for diffing
// (the `a == b` fast path in ScalarAlmostEqual deliberately treats inf==inf as
// equal and +0/-0 as equal; layer-offset compares are exact). These intentional
// float equality checks trip -Wfloat-equal.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace lightusd {
namespace next {
namespace diff_detail {
std::string NormalizeAssetPathForDiff(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
    path.erase(0, 2);
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string AssetPathLeafForDiff(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

bool HasPathComponentSuffix(const std::string &path,
                            const std::string &suffix) {
  if (path == suffix) {
    return true;
  }
  if (path.size() <= suffix.size()) {
    return false;
  }
  const size_t offset = path.size() - suffix.size();
  return path.compare(offset, suffix.size(), suffix) == 0 &&
         path[offset - 1] == '/';
}

bool AssetPathStringsEquivalentForDiff(const std::string &lhs_path,
                                       const std::string &rhs_path) {
  const std::string lhs = NormalizeAssetPathForDiff(lhs_path);
  const std::string rhs = NormalizeAssetPathForDiff(rhs_path);
  if (lhs == rhs) {
    return true;
  }
  if (HasPathComponentSuffix(lhs, rhs) || HasPathComponentSuffix(rhs, lhs)) {
    return true;
  }

  const std::string lhs_leaf = AssetPathLeafForDiff(lhs);
  const std::string rhs_leaf = AssetPathLeafForDiff(rhs);
  return !lhs_leaf.empty() && lhs_leaf == rhs_leaf;
}

// ---------------------------------------------------------------------------
// ULP / eps tolerant scalar comparison.
// ---------------------------------------------------------------------------



bool UlpsEqual(float a, float b, uint32_t maxUlps) {
  if (std::isnan(a) || std::isnan(b)) {
    return false;
  }
  int32_t ia, ib;
  std::memcpy(&ia, &a, sizeof(ia));
  std::memcpy(&ib, &b, sizeof(ib));
  // Map to a lexicographically ordered (sign-magnitude -> two's-complement)
  // space so ULP distance is a simple subtraction across the 0 boundary.
  const int64_t oa = (ia < 0) ? (int64_t(INT32_MIN) - ia) : int64_t(ia);
  const int64_t ob = (ib < 0) ? (int64_t(INT32_MIN) - ib) : int64_t(ib);
  const int64_t d = (oa > ob) ? (oa - ob) : (ob - oa);
  return d <= int64_t(maxUlps);
}

bool UlpsEqual(double a, double b, uint64_t maxUlps) {
  if (std::isnan(a) || std::isnan(b)) {
    return false;
  }
  int64_t ia, ib;
  std::memcpy(&ia, &a, sizeof(ia));
  std::memcpy(&ib, &b, sizeof(ib));
  const uint64_t oa =
      (ia < 0) ? (0x8000000000000000ull - uint64_t(ia)) : (uint64_t(ia) + 0x8000000000000000ull);
  const uint64_t ob =
      (ib < 0) ? (0x8000000000000000ull - uint64_t(ib)) : (uint64_t(ib) + 0x8000000000000000ull);
  const uint64_t d = (oa > ob) ? (oa - ob) : (ob - oa);
  return d <= maxUlps;
}

bool ScalarAlmostEqual(float a, float b, const DiffOptions &o) {
  if (a == b) return true;  // fast path (also inf==inf, +0==-0)
  if (o.absEps >= 0.0 &&
      std::fabs(double(a) - double(b)) <= o.absEps) {
    return true;
  }
  return UlpsEqual(a, b, o.floatUlps);
}

bool ScalarAlmostEqual(double a, double b, const DiffOptions &o) {
  if (a == b) return true;
  if (o.absEps >= 0.0 && std::fabs(a - b) <= o.absEps) return true;
  return UlpsEqual(a, b, o.doubleUlps);
}

// ---------------------------------------------------------------------------
// Tolerant Value comparison.
// ---------------------------------------------------------------------------

bool ValuesEquivalentForDiff(const Value &lhs, const Value &rhs,
                             const DiffOptions &opts, int depth);

// Dictionary compare (order-insensitive over keys). Records changed keys
// (+added / -removed / ~modified) into `changed` if non-null; otherwise
// returns early on the first difference.
bool CompareDicts(DictView lhs, DictView rhs, const DiffOptions &opts,
                  int depth, std::vector<std::string> *changed) {
  static const Dict kEmpty;
  DictView l = lhs ? *lhs : kEmpty;
  DictView r = rhs ? *rhs : kEmpty;
  if (depth > int(kMaxTraversalDepth)) {
    if (changed) changed->push_back("<too-deep>");
    return false;
  }
  bool equal = true;
  for (const auto &kv : l.entries()) {
    auto rv = r.find(kv.first);
    if (!rv) {
      equal = false;
      if (changed) changed->push_back("-" + kv.first); else return false;
    } else if (!ValuesEquivalentForDiff(kv.second, *rv, opts, depth + 1)) {
      equal = false;
      if (changed) changed->push_back("~" + kv.first); else return false;
    }
  }
  for (const auto &kv : r.entries()) {
    if (!l.find(kv.first)) {
      equal = false;
      if (changed) changed->push_back("+" + kv.first); else return false;
    }
  }
  return equal;
}

// Dictionary-valued metadata Values (empty Value == unauthored == empty dict).
// True when two dictionaries share a key whose value is a dictionary on one
// side and a scalar on the other (recursively) — the "dictionary type
// conflict" differential: composition would keep the stronger opinion and
// silently shadow the weaker subtree.
bool DictsHaveTypeConflict(const Value &l, const Value &r) {
  auto ld = l.as_dictionary();
  auto rd = r.as_dictionary();
  if (!ld || !rd) return false;
  for (const auto &e : ld->entries()) {
    auto o = rd->find(e.first);
    if (!o) continue;
    if (e.second.is_dictionary() != o->is_dictionary()) return true;
    if (e.second.is_dictionary() && DictsHaveTypeConflict(e.second, *o)) {
      return true;
    }
  }
  return false;
}

bool CompareDictValues(const Value &lhs, const Value &rhs,
                       const DiffOptions &opts,
                       std::vector<std::string> *changed) {
  auto ld = lhs.is_dictionary() ? lhs.as_dictionary() : nullptr;
  auto rd = rhs.is_dictionary() ? rhs.as_dictionary() : nullptr;
  return CompareDicts(ld, rd, opts, 0, changed);
}

// Numeric component kind of a (possibly role-typed) scalar/vector/matrix type.
// Returns Invalid component for non-float-backed types.
TypeId FloatingComponentType(TypeId id) {
  switch (id) {
    case TypeId::Half:
      return TypeId::Half;
    case TypeId::Float:
      return TypeId::Float;
    case TypeId::Double:
    case TypeId::TimeCode:
      return TypeId::Double;
    default: {
      const TypeId comp = GetComponentType(id);
      if (comp == TypeId::Half || comp == TypeId::Float ||
          comp == TypeId::Double) {
        return comp;
      }
      return TypeId::Invalid;
    }
  }
}

size_t FloatingComponentCount(TypeId id) {
  if (id == TypeId::TimeCode) return 1;
  return GetComponentCount(id);
}

// Tolerant compare of non-array float-backed POD values (scalar / vecN / quat /
// matrix, incl. role types). Sets `handled=true` when the LHS is such a type.
bool NumericPodAlmostEqual(const Value &l, const Value &r,
                           const DiffOptions &o, bool &handled) {
  handled = false;
  const TypeId lcomp = FloatingComponentType(l.type_id());
  if (lcomp == TypeId::Invalid) {
    return false;
  }
  handled = true;
  const TypeId rcomp = FloatingComponentType(r.type_id());
  const size_t lcount = FloatingComponentCount(l.type_id());
  const size_t rcount = FloatingComponentCount(r.type_id());
  if (rcomp != lcomp || lcount == 0 || lcount != rcount) {
    return false;
  }
  size_t lsize = 0, rsize = 0;
  const uint8_t *lb = l.raw_bytes(&lsize);
  const uint8_t *rb = r.raw_bytes(&rsize);
  if (!lb || !rb) {
    return lb == rb;
  }
  if (lcomp == TypeId::Float) {
    if (lsize < lcount * sizeof(float) || rsize < rcount * sizeof(float)) {
      return false;
    }
    for (size_t i = 0; i < lcount; ++i) {
      float a, b;
      std::memcpy(&a, lb + i * sizeof(float), sizeof(float));
      std::memcpy(&b, rb + i * sizeof(float), sizeof(float));
      if (!ScalarAlmostEqual(a, b, o)) return false;
    }
    return true;
  }
  if (lcomp == TypeId::Double) {
    if (lsize < lcount * sizeof(double) || rsize < rcount * sizeof(double)) {
      return false;
    }
    for (size_t i = 0; i < lcount; ++i) {
      double a, b;
      std::memcpy(&a, lb + i * sizeof(double), sizeof(double));
      std::memcpy(&b, rb + i * sizeof(double), sizeof(double));
      if (!ScalarAlmostEqual(a, b, o)) return false;
    }
    return true;
  }
  // Half: compare in float space.
  if (lsize < lcount * sizeof(uint16_t) || rsize < rcount * sizeof(uint16_t)) {
    return false;
  }
  for (size_t i = 0; i < lcount; ++i) {
    uint16_t ha, hb;
    std::memcpy(&ha, lb + i * sizeof(uint16_t), sizeof(uint16_t));
    std::memcpy(&hb, rb + i * sizeof(uint16_t), sizeof(uint16_t));
    if (ha == hb) continue;  // bitwise-equal half
    if (!ScalarAlmostEqual(lu_half_to_float(ha), lu_half_to_float(hb), o)) {
      return false;
    }
  }
  return true;
}

// Tolerant compare of float/double-backed arrays (flat component buffers; half
// element types materialize into float buffers). Sets `handled=true` when the
// LHS is such an array.
bool NumericArrayAlmostEqual(const Value &l, const Value &r,
                             const DiffOptions &o, bool &handled) {
  handled = false;
  if (auto lf = l.as_float_array()) {
    handled = true;
    auto rf = r.as_float_array();
    if (!rf || rf->size() != lf->size()) return false;
    for (size_t i = 0; i < lf->size(); ++i) {
      if (!ScalarAlmostEqual((*lf)[i], (*rf)[i], o)) return false;
    }
    return true;
  }
  if (auto ld = l.as_double_array()) {
    handled = true;
    auto rd = r.as_double_array();
    if (!rd || rd->size() != ld->size()) return false;
    for (size_t i = 0; i < ld->size(); ++i) {
      if (!ScalarAlmostEqual((*ld)[i], (*rd)[i], o)) return false;
    }
    return true;
  }
  return false;
}

bool ValuesEquivalentForDiff(const Value &lhs, const Value &rhs,
                             const DiffOptions &opts, int depth) {
  if (depth > int(kMaxTraversalDepth)) {
    return false;
  }

  // Authored blocks (`= None`) and empties compare structurally.
  if (lhs.is_block() != rhs.is_block()) return false;
  if (lhs.is_block()) return true;
  if (lhs.is_empty() != rhs.is_empty()) return false;
  if (lhs.is_empty()) return true;

  // 1. AssetPath fuzzy equivalence (path suffix / leaf), scalar only.
  if (opts.fuzzyAssetPaths && !lhs.is_array() && !rhs.is_array()) {
    auto la = lhs.as_asset_path();
    auto ra = rhs.as_asset_path();
    if (la && ra) {
      return AssetPathStringsEquivalentForDiff(*la, *ra);
    }
  }

  // 2. Nested dictionary (customData / assetInfo / ... leaves).
  if (lhs.is_dictionary() && rhs.is_dictionary()) {
    return CompareDicts(lhs.as_dictionary(), rhs.as_dictionary(), opts,
                        depth + 1, nullptr);
  }

  if (lhs.is_array() != rhs.is_array()) return false;

  // 3. Numeric float-backed -> ULP / eps.
  bool handled = false;
  if (lhs.is_array()) {
    const bool eq = NumericArrayAlmostEqual(lhs, rhs, opts, handled);
    if (handled) return eq;
  } else {
    const bool eq = NumericPodAlmostEqual(lhs, rhs, opts, handled);
    if (handled) return eq;
  }

  // 4. Everything else (int/uint/bool/token/string arrays and scalars) ->
  //    exact compare.
  return lhs == rhs;
}


}  // namespace diff_detail

}  // namespace next
}  // namespace lightusd
