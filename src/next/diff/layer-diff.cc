// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Layer / PrimSpec diff (see layer-diff.hh).

#include "layer-diff.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "../layer/prim-spec.hh"
#include "../layer/property-index.hh"
#include "../types/type-id.hh"
#include "../types/value.hh"
#include "../writer/value-printer.hh"

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

namespace {

constexpr uint32_t kMaxTraversalDepth = 1024;

std::string TruncateForDiff(const std::string &s) {
  // Retain enough of each value that the differing region survives for the
  // diff-aware centering done at render time (CenterValuePairForDiff).
  constexpr size_t kMaxDiffValueChars = 4096;
  if (s.size() <= kMaxDiffValueChars) {
    return s;
  }
  return s.substr(0, kMaxDiffValueChars) + "...";
}

std::string JoinPrimPath(const std::string &parent, const std::string &child) {
  if (parent.empty() || parent == "/") {
    return "/" + child;
  }
  return parent + "/" + child;
}

std::string JoinPathList(const std::vector<Path> &paths) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i > 0) {
      ss << ", ";
    }
    ss << "<" << paths[i].str() << ">";
  }
  ss << "]";
  return ss.str();
}

// ---------------------------------------------------------------------------
// Fuzzy asset-path equivalence (mirrors legacy tydra behavior).
// ---------------------------------------------------------------------------

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

float HalfBitsToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t man = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;  // +-0
    } else {
      // Subnormal half -> normalized float.
      exp = 127 - 15 + 1;
      while (!(man & 0x400u)) {
        man <<= 1;
        --exp;
      }
      man &= 0x3FFu;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (man << 13);  // inf / nan
  } else {
    bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

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
bool CompareDicts(const Dict *lhs, const Dict *rhs, const DiffOptions &opts,
                  int depth, std::vector<std::string> *changed) {
  static const Dict kEmpty;
  const Dict &l = lhs ? *lhs : kEmpty;
  const Dict &r = rhs ? *rhs : kEmpty;
  if (depth > int(kMaxTraversalDepth)) {
    if (changed) changed->push_back("<too-deep>");
    return false;
  }
  bool equal = true;
  for (const auto &kv : l.entries()) {
    const Value *rv = r.find(kv.first);
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
  const Dict *ld = l.as_dictionary();
  const Dict *rd = r.as_dictionary();
  if (!ld || !rd) return false;
  for (const auto &e : ld->entries()) {
    const Value *o = rd->find(e.first);
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
  const Dict *ld = lhs.is_dictionary() ? lhs.as_dictionary() : nullptr;
  const Dict *rd = rhs.is_dictionary() ? rhs.as_dictionary() : nullptr;
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
    if (!ScalarAlmostEqual(HalfBitsToFloat(ha), HalfBitsToFloat(hb), o)) {
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
  if (const std::vector<float> *lf = l.as_float_array()) {
    handled = true;
    const std::vector<float> *rf = r.as_float_array();
    if (!rf || rf->size() != lf->size()) return false;
    for (size_t i = 0; i < lf->size(); ++i) {
      if (!ScalarAlmostEqual((*lf)[i], (*rf)[i], o)) return false;
    }
    return true;
  }
  if (const std::vector<double> *ld = l.as_double_array()) {
    handled = true;
    const std::vector<double> *rd = r.as_double_array();
    if (!rd || rd->size() != ld->size()) return false;
    for (size_t i = 0; i < ld->size(); ++i) {
      if (!ScalarAlmostEqual((*ld)[i], (*rd)[i], o)) return false;
    }
    return true;
  }
  return false;
}

bool ValuesEquivalentForDiff(const Value &lhs, const Value &rhs,
                             const DiffOptions &opts, int depth = 0) {
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
    const std::string *la = lhs.as_asset_path();
    const std::string *ra = rhs.as_asset_path();
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

// ---------------------------------------------------------------------------
// Property view: unified enumeration of attributes and relationships.
// ---------------------------------------------------------------------------

struct PropView {
  const PropSlot *slot = nullptr;               // attribute slot (may be null)
  const std::vector<Path> *rel_targets = nullptr;  // set for relationships
  bool is_rel = false;
};

// Name-sorted so diff output ordering is deterministic.
std::map<std::string, PropView> CollectProps(const PrimSpec &ps) {
  std::map<std::string, PropView> out;
  const PropNameTable &names = GetPropNameTable();
  for (const PropSlot &slot : ps.properties().slots()) {
    if (!slot.name_id.is_valid()) continue;
    PropView v;
    v.slot = &slot;
    v.is_rel = slot.is_relationship();
    out[std::string(names.get(slot.name_id))] = v;
  }
  for (const std::string &rel : ps.relationship_names()) {
    PropView &v = out[rel];
    v.is_rel = true;
    v.rel_targets = ps.relationship(rel);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical strings / formatting.
// ---------------------------------------------------------------------------

std::string DblStr(double v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string StrListToStr(const std::vector<std::string> &v) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) ss << ", ";
    ss << v[i];
  }
  ss << "]";
  return ss.str();
}

std::string ArcEditToStr(const ArcEdit &e) {
  if (!e.authored) return "<none>";
  std::stringstream ss;
  ss << (e.is_explicit ? "explicit" : "listop");
  ss << " add" << StrListToStr(e.added);
  ss << " prepend" << StrListToStr(e.prepended);
  ss << " append" << StrListToStr(e.appended);
  ss << " delete" << StrListToStr(e.deleted);
  ss << " order" << StrListToStr(e.ordered);
  return ss.str();
}

std::string VariantSelectionsToStr(
    const std::string &legacy_single,
    const std::vector<std::pair<std::string, std::string>> &sels) {
  // Canonical, order-insensitive set of "set=sel" entries.
  std::set<std::string> all;
  if (!legacy_single.empty()) all.insert(legacy_single);
  for (const auto &kv : sels) all.insert(kv.first + "=" + kv.second);
  std::stringstream ss;
  for (const auto &s : all) ss << s << ",";
  return ss.str();
}

std::string PairListToStr(
    const std::vector<std::pair<std::string, std::string>> &v) {
  std::stringstream ss;
  for (const auto &kv : v) ss << kv.first << "->" << kv.second << ",";
  return ss.str();
}

std::string ValueToDiffString(const Value &v) {
  if (v.is_block()) return "None";
  if (v.is_empty()) return "<none>";
  PrintOptions popts;
  popts.compact = true;
  // PrintValue materializes lazy arrays internally (via a temporary copy).
  return TruncateForDiff(PrintValue(v, popts));
}

// Declared type name of a property, preferring the recorded USD type string
// and falling back to the slot's runtime type.
std::string DeclaredTypeName(const PrimSpec &ps, const std::string &name,
                             const PropView &view) {
  if (const std::string *tn = ps.property_type_name(name)) {
    return *tn;
  }
  if (view.slot) {
    return PrintTypeName(static_cast<TypeId>(view.slot->value_type),
                         view.slot->is_array());
  }
  return "";
}

// Role-type equivalence for declared type names: point3f / normal3f / color3f
// vs float3 (etc.) share storage and count as the same underlying type.
bool TypeNamesEquivalent(const std::string &lhs, const std::string &rhs) {
  if (lhs == rhs) return true;
  if (lhs.empty() || rhs.empty()) return false;

  auto split = [](const std::string &s, bool *is_array) -> std::string {
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "[]") == 0) {
      *is_array = true;
      return s.substr(0, s.size() - 2);
    }
    *is_array = false;
    return s;
  };
  bool larr = false, rarr = false;
  const std::string lbase = split(lhs, &larr);
  const std::string rbase = split(rhs, &rarr);
  if (larr != rarr) return false;

  const TypeId lt = GetTypeIdFromName(lbase.c_str());
  const TypeId rt = GetTypeIdFromName(rbase.c_str());
  if (lt == TypeId::Invalid || rt == TypeId::Invalid) return false;
  if (lt == rt) return true;

  // Quaternions stay distinct from plain vec4s (matching legacy semantics).
  auto is_quat = [](TypeId t) {
    return t == TypeId::Quath || t == TypeId::Quatf || t == TypeId::Quatd;
  };
  if (is_quat(lt) != is_quat(rt)) return false;

  const TypeId lcomp = GetComponentType(lt);
  const TypeId rcomp = GetComponentType(rt);
  if (lcomp == TypeId::Invalid || rcomp == TypeId::Invalid) return false;
  return lcomp == rcomp && GetComponentCount(lt) == GetComponentCount(rt);
}

std::string FormatPropertyForDiff(const PrimSpec &ps, const std::string &name,
                                  const PropView &view) {
  std::stringstream ss;
  const uint16_t flags =
      view.slot ? view.slot->flags
                : (view.is_rel ? ps.relationship_flags(name) : uint16_t(0));
  if (flags & PropSlot::kFlagCustom) ss << "custom ";
  if (flags & PropSlot::kFlagUniform) ss << "uniform ";

  if (view.is_rel) {
    ss << "rel";
    const std::vector<Path> *targets =
        view.rel_targets ? view.rel_targets : ps.relationship(name);
    if (targets) {
      if (targets->size() == 1) {
        ss << " target=<" << (*targets)[0].str() << ">";
      } else {
        ss << " targets=" << JoinPathList(*targets);
      }
    }
    const auto &edits = ps.relationship_edits();
    auto it = edits.find(name);
    if (it != edits.end() && it->second.has_qualifiers()) {
      ss << " listOp=" << ArcEditToStr(it->second);
    }
    return ss.str();
  }

  ss << "attr";
  const std::string tn = DeclaredTypeName(ps, name, view);
  if (!tn.empty()) ss << " type=" << tn;

  if (view.slot) {
    const std::vector<Path> *conns = ps.connection(name);
    if (conns) {
      ss << " connections=" << JoinPathList(*conns);
    }
    const Value *v = ps.property_value(view.slot->name_id);
    if (v && v->is_block()) {
      ss << " value=None";
    } else if (v && !v->is_empty()) {
      ss << " value=" << ValueToDiffString(*v);
    }
    const auto *samples = ps.time_samples(view.slot->name_id);
    if (samples && !samples->empty()) {
      ss << " timeSamples=" << samples->size()
         << " firstTime=" << samples->front().first
         << " lastTime=" << samples->back().first;
    }
  }
  return ss.str();
}

// ---------------------------------------------------------------------------
// Per-property metadata (PropMeta) comparison.
// ---------------------------------------------------------------------------

bool ExtensionFieldsEqual(const std::vector<TypedExtensionField>& lhs,
                          const std::vector<TypedExtensionField>& rhs,
                          const DiffOptions& opts) {
  if (lhs.size() != rhs.size()) return false;
  for (const TypedExtensionField& field : lhs) {
    const auto it = std::find_if(
        rhs.begin(), rhs.end(), [&](const TypedExtensionField& candidate) {
          return candidate.name == field.name;
        });
    if (it == rhs.end() || it->unregistered != field.unregistered ||
        it->unregistered_source != field.unregistered_source ||
        !ValuesEquivalentForDiff(field.value, it->value, opts)) {
      return false;
    }
  }
  return true;
}

bool ComparePropMetas(const PropMeta *lhs, const PropMeta *rhs,
                      const DiffOptions &opts,
                      std::vector<std::string> *reasons) {
  static const PropMeta kEmpty;
  const PropMeta &l = lhs ? *lhs : kEmpty;
  const PropMeta &r = rhs ? *rhs : kEmpty;
  bool equal = true;
  auto note = [&](const char *field) {
    equal = false;
    if (reasons) reasons->push_back(std::string("meta:") + field);
  };
  auto both = [&](uint32_t bit) {
    return (l.authored & bit) || (r.authored & bit);
  };
  auto cmp_str = [&](uint32_t bit, const std::string &a, const std::string &b,
                     const char *field) {
    if (((l.authored & bit) != (r.authored & bit)) || (both(bit) && a != b)) {
      note(field);
    }
  };

  cmp_str(PropMeta::kInterpolation, l.interpolation, r.interpolation,
          "interpolation");
  cmp_str(PropMeta::kColorSpace, l.colorSpace, r.colorSpace, "colorSpace");
  cmp_str(PropMeta::kDisplayName, l.displayName, r.displayName, "displayName");
  cmp_str(PropMeta::kDisplayGroup, l.displayGroup, r.displayGroup,
          "displayGroup");
  cmp_str(PropMeta::kDoc, l.doc, r.doc, "doc");
  cmp_str(PropMeta::kRenderType, l.renderType, r.renderType, "renderType");
  cmp_str(PropMeta::kConnectability, l.connectability, r.connectability,
          "connectability");
  cmp_str(PropMeta::kOutputName, l.outputName, r.outputName, "outputName");
  cmp_str(PropMeta::kBindMaterialAs, l.bindMaterialAs, r.bindMaterialAs,
          "bindMaterialAs");
  cmp_str(PropMeta::kKind, l.kind, r.kind, "kind");

  if (((l.authored & PropMeta::kElementSize) !=
       (r.authored & PropMeta::kElementSize)) ||
      (both(PropMeta::kElementSize) && l.elementSize != r.elementSize)) {
    note("elementSize");
  }
  if (((l.authored & PropMeta::kUnauthoredIdx) !=
       (r.authored & PropMeta::kUnauthoredIdx)) ||
      (both(PropMeta::kUnauthoredIdx) &&
       l.unauthoredValuesIndex != r.unauthoredValuesIndex)) {
    note("unauthoredValuesIndex");
  }
  if (((l.authored & PropMeta::kWeight) != (r.authored & PropMeta::kWeight)) ||
      (both(PropMeta::kWeight) && !ScalarAlmostEqual(l.weight, r.weight, opts))) {
    note("weight");
  }
  if (((l.authored & PropMeta::kHidden) != (r.authored & PropMeta::kHidden)) ||
      (both(PropMeta::kHidden) && l.hidden != r.hidden)) {
    note("hidden");
  }
  if (((l.authored & PropMeta::kAllowedTokens) !=
       (r.authored & PropMeta::kAllowedTokens)) ||
      (both(PropMeta::kAllowedTokens) && l.allowedTokens != r.allowedTokens)) {
    note("allowedTokens");
  }
  if (!CompareDictValues(l.customData, r.customData, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.customData, r.customData) ? "customData(type-conflict)"
                                      : "customData");
  }
  if (!CompareDictValues(l.assetInfo, r.assetInfo, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.assetInfo, r.assetInfo) ? "assetInfo(type-conflict)"
                                      : "assetInfo");
  }
  if (!CompareDictValues(l.sdrMetadata, r.sdrMetadata, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.sdrMetadata, r.sdrMetadata) ? "sdrMetadata(type-conflict)"
                                      : "sdrMetadata");
  }
  if (!ExtensionFieldsEqual(l.unknownFields, r.unknownFields, opts)) {
    note("extensionFields");
  }
  return equal;
}

// ---------------------------------------------------------------------------
// Property comparison (attribute or relationship).
// ---------------------------------------------------------------------------

bool ComparePropertyDetailed(const PrimSpec &lp, const std::string &name,
                             const PropView &lv, const PrimSpec &rp,
                             const PropView &rv, const DiffOptions &opts,
                             std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lv.is_rel != rv.is_rel) {
    note("kind");
    return false;
  }

  const uint16_t lflags =
      lv.slot ? lv.slot->flags
              : (lv.is_rel ? lp.relationship_flags(name) : uint16_t(0));
  const uint16_t rflags =
      rv.slot ? rv.slot->flags
              : (rv.is_rel ? rp.relationship_flags(name) : uint16_t(0));
  if ((lflags & PropSlot::kFlagCustom) != (rflags & PropSlot::kFlagCustom)) {
    note("custom");
  }
  if ((lflags & PropSlot::kFlagUniform) != (rflags & PropSlot::kFlagUniform)) {
    note("variability");
  }

  if (lv.is_rel) {
    // Relationship: target list (exact path compare).
    static const std::vector<Path> kNoPaths;
    const std::vector<Path> *lt = lv.rel_targets ? lv.rel_targets : &kNoPaths;
    const std::vector<Path> *rt = rv.rel_targets ? rv.rel_targets : &kNoPaths;
    bool tEqual = (lt->size() == rt->size());
    for (size_t i = 0; tEqual && i < lt->size(); ++i) {
      if ((*lt)[i].str() != (*rt)[i].str()) tEqual = false;
    }
    if (!tEqual) note("targets");

    // Authored list-op edits.
    static const ArcEdit kNoEdit;
    auto edit_of = [&](const PrimSpec &ps) -> const ArcEdit & {
      const auto &edits = ps.relationship_edits();
      auto it = edits.find(name);
      return (it == edits.end()) ? kNoEdit : it->second;
    };
    if (ArcEditToStr(edit_of(lp)) != ArcEditToStr(edit_of(rp))) {
      note("listOp");
    }
    return equal;
  }

  // Attribute: declared type.
  const std::string ltn = DeclaredTypeName(lp, name, lv);
  const std::string rtn = DeclaredTypeName(rp, name, rv);
  if (!TypeNamesEquivalent(ltn, rtn)) {
    note("type");
  }

  // Connections (exact path compare; nullptr == not authored).
  const std::vector<Path> *lc = lp.connection(name);
  const std::vector<Path> *rc = rp.connection(name);
  if ((lc == nullptr) != (rc == nullptr)) {
    note("connections");
  } else if (lc && rc) {
    bool cEqual = (lc->size() == rc->size());
    for (size_t i = 0; cEqual && i < lc->size(); ++i) {
      if ((*lc)[i].str() != (*rc)[i].str()) cEqual = false;
    }
    if (!cEqual) note("connections");
  }
  static const ArcEdit kNoConnectionEdit;
  const ArcEdit* lce = lp.connection_edit(name);
  const ArcEdit* rce = rp.connection_edit(name);
  if (ArcEditToStr(lce ? *lce : kNoConnectionEdit) !=
      ArcEditToStr(rce ? *rce : kNoConnectionEdit)) {
    note("connectionListOp");
  }

  // Default value. A value BLOCK (`= None`) transition gets its own
  // differential: "blocked" distinguishes deleting/blocking an opinion from
  // an ordinary value edit.
  const Value *lval = lv.slot ? lp.property_value(lv.slot->name_id) : nullptr;
  const Value *rval = rv.slot ? rp.property_value(rv.slot->name_id) : nullptr;
  const bool lhas = lval && !lval->is_empty();
  const bool rhas = rval && !rval->is_empty();
  const bool lblock = lval && lval->is_block();
  const bool rblock = rval && rval->is_block();
  if (lblock != rblock) {
    note("blocked");
  } else if (lhas != rhas) {
    note("value");
  } else if (lhas && !lblock) {
    if (!ValuesEquivalentForDiff(*lval, *rval, opts)) {
      note("value");
    }
  }

  // Time samples (times compared with timeUlps; values with the normal opts).
  const std::vector<std::pair<double, uint32_t>> *ls =
      lv.slot ? lp.time_samples(lv.slot->name_id) : nullptr;
  const std::vector<std::pair<double, uint32_t>> *rs =
      rv.slot ? rp.time_samples(rv.slot->name_id) : nullptr;
  const bool lts = ls && !ls->empty();
  const bool rts = rs && !rs->empty();
  if (lts != rts) {
    note("timeSamples");
  } else if (lts) {
    DiffOptions timeOpts = opts;
    timeOpts.doubleUlps = opts.timeUlps;
    bool tsEqual = (ls->size() == rs->size());
    for (size_t i = 0; tsEqual && i < ls->size(); ++i) {
      if (!ScalarAlmostEqual((*ls)[i].first, (*rs)[i].first, timeOpts)) {
        tsEqual = false;
        break;
      }
      const Value *lsv = lp.time_sample_value((*ls)[i].second);
      const Value *rsv = rp.time_sample_value((*rs)[i].second);
      if ((lsv == nullptr) != (rsv == nullptr)) {
        tsEqual = false;
      } else if (lsv && !ValuesEquivalentForDiff(*lsv, *rsv, opts)) {
        tsEqual = false;
      }
    }
    if (!tsEqual) note("timeSamples");
  }

  // Per-property metadata.
  if (opts.compareMetadata) {
    const PropMeta *lm = lp.property_meta(name);
    const PropMeta *rm = rp.property_meta(name);
    if (!ComparePropMetas(lm, rm, opts, reasons)) {
      equal = false;
    }
  }

  return equal;
}

// ---------------------------------------------------------------------------
// Deep per-field variant-set comparison. Replaces the former stringified
// name/selection/option-name comparison, which produced no differential for
// variant-INNER edits (properties, arcs, relationships, unknown metadata,
// nested sets). On the first difference, `reason` is set to a granular
// "variantSets:<set>/<option>:<field>" tag.
// ---------------------------------------------------------------------------

bool VariantOptionEqual(const VariantData &l, const VariantData &r,
                        const DiffOptions &opts, std::string *reason,
                        int depth);

bool VariantSetsEqual(const std::vector<VariantSetData> &l,
                      const std::vector<VariantSetData> &r,
                      const DiffOptions &opts, std::string *reason,
                      int depth = 0) {
  auto fail = [&](const std::string &r_) {
    if (reason && reason->empty()) *reason = r_;
    return false;
  };
  if (depth > 64) return true;  // matches the parser's nesting cap
  if (l.size() != r.size()) return fail("variantSets:count");
  for (const VariantSetData &ls : l) {
    const VariantSetData *rs = nullptr;
    for (const VariantSetData &cand : r) {
      if (cand.name == ls.name) {
        rs = &cand;
        break;
      }
    }
    if (!rs) return fail("variantSets:" + ls.name);
    if (ls.selected != rs->selected) {
      return fail("variantSets:" + ls.name + ":selected");
    }
    if (ls.variants.size() != rs->variants.size()) {
      return fail("variantSets:" + ls.name + ":options");
    }
    for (const VariantData &lv : ls.variants) {
      const VariantData *rv = nullptr;
      for (const VariantData &cand : rs->variants) {
        if (cand.name == lv.name) {
          rv = &cand;
          break;
        }
      }
      const std::string opt = "variantSets:" + ls.name + "/" + lv.name;
      if (!rv) return fail(opt);
      std::string inner;
      if (!VariantOptionEqual(lv, *rv, opts, &inner, depth)) {
        return fail(opt + ":" + inner);
      }
    }
  }
  return true;
}

bool VariantOptionEqual(const VariantData &l, const VariantData &r,
                        const DiffOptions &opts, std::string *reason,
                        int depth) {
  auto fail = [&](const char *r_) {
    if (reason && reason->empty()) *reason = r_;
    return false;
  };
  if (l.active != r.active) return fail("active");
  if (l.hidden != r.hidden) return fail("hidden");
  if (l.doc != r.doc) return fail("doc");
  if (l.references != r.references) return fail("references");
  if (l.payloads != r.payloads) return fail("payload");
  if (l.inherits != r.inherits) return fail("inherits");
  if (l.specializes != r.specializes) return fail("specializes");
  if (l.variantSelections != r.variantSelections) return fail("variants");
  if (l.unknownMeta != r.unknownMeta) return fail("unknownMeta");
  if (!ExtensionFieldsEqual(l.unknownFields, r.unknownFields, opts)) {
    return fail("extensionFields");
  }
  if (l.properties.size() != r.properties.size()) return fail("properties");
  for (const VariantProperty &lp : l.properties) {
    const VariantProperty *rp = nullptr;
    for (const VariantProperty &cand : r.properties) {
      if (cand.name == lp.name) {
        rp = &cand;
        break;
      }
    }
    if (!rp || lp.flags != rp->flags || !(lp.value == rp->value)) {
      return fail("properties");
    }
  }
  if (l.relationships.size() != r.relationships.size()) {
    return fail("relationships");
  }
  for (const auto &lr : l.relationships) {
    auto it = r.relationships.find(lr.first);
    if (it == r.relationships.end() || it->second != lr.second) {
      return fail("relationships");
    }
  }
  if (l.relationshipFlags != r.relationshipFlags) {
    return fail("relationships");
  }
  // Content subtrees compare structurally (prim count + paths): a full
  // recursive layer diff here would recurse into this same machinery.
  const size_t lc = l.content ? l.content->prim_count() : 0;
  const size_t rc = r.content ? r.content->prim_count() : 0;
  if (lc != rc) return fail("content");
  if (l.content && r.content) {
    for (size_t i = 0; i < lc; ++i) {
      const PrimSpec *lp = l.content->prim(static_cast<uint32_t>(i));
      const PrimSpec *rp = r.content->prim(static_cast<uint32_t>(i));
      if (!lp || !rp || lp->path().str() != rp->path().str()) {
        return fail("content");
      }
    }
  }
  std::string nested;
  if (!VariantSetsEqual(l.variantSets, r.variantSets, opts, &nested,
                        depth + 1)) {
    return fail("nested");
  }
  return true;
}

// ---------------------------------------------------------------------------
// PrimSpec own-field comparison (specifier / typeName / metadata).
// ---------------------------------------------------------------------------

bool ComparePrimMeta(const PrimSpecMeta &l, const PrimSpecMeta &r,
                     const DiffOptions &opts,
                     std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *field) {
    equal = false;
    if (reasons) reasons->push_back(field);
  };

  if (l.active != r.active || l.active_authored != r.active_authored) {
    note("meta:active");
  }
  if (l.hidden != r.hidden || l.hidden_authored != r.hidden_authored) {
    note("meta:hidden");
  }
  if (l.instanceable != r.instanceable) {
    note("meta:instanceable");
  }

  // Composition arcs: inline vectors + authored list-op qualifiers.
  static const ArcListOpEdits kNoEdits;
  const ArcListOpEdits &le = l.arc_edits() ? *l.arc_edits() : kNoEdits;
  const ArcListOpEdits &re = r.arc_edits() ? *r.arc_edits() : kNoEdits;
  auto cmp_arc = [&](const std::vector<std::string> &a,
                     const std::vector<std::string> &b, const ArcEdit &ea,
                     const ArcEdit &eb, const char *field) {
    if (a != b || ArcEditToStr(ea) != ArcEditToStr(eb)) {
      note(field);
    }
  };
  cmp_arc(l.references, r.references, le.references, re.references,
          "meta:references");
  cmp_arc(l.payloads, r.payloads, le.payloads, re.payloads, "meta:payload");
  cmp_arc(l.inherits, r.inherits, le.inherits, re.inherits, "meta:inherits");
  cmp_arc(l.specializes, r.specializes, le.specializes, re.specializes,
          "meta:specializes");

  if (VariantSelectionsToStr(l.variantSelection, l.variantSelections()) !=
      VariantSelectionsToStr(r.variantSelection, r.variantSelections())) {
    note("meta:variants");
  }
  {
    std::string vreason;
    if (!VariantSetsEqual(l.variantSets(), r.variantSets(), opts,
                          &vreason)) {
      equal = false;
      if (reasons) reasons->push_back("meta:" + vreason);
    }
  }
  const StringListOpEdits& lv = l.variantSetNameEdits();
  const StringListOpEdits& rv = r.variantSetNameEdits();
  if (lv.authored != rv.authored || lv.is_explicit != rv.is_explicit ||
      lv.explicit_items != rv.explicit_items || lv.added != rv.added ||
      lv.prepended != rv.prepended || lv.appended != rv.appended ||
      lv.deleted != rv.deleted || lv.ordered != rv.ordered) {
    note("meta:variantSetNames");
  }
  if (l.primOrder() != r.primOrder() ||
      l.primOrderAuthored() != r.primOrderAuthored()) {
    note("meta:primOrder");
  }
  if (l.propertyOrder() != r.propertyOrder() ||
      l.propertyOrderAuthored() != r.propertyOrderAuthored()) {
    note("meta:propertyOrder");
  }
  if (l.layer_offset != r.layer_offset) {
    note("meta:layerOffset");
  }

  if (l.kind() != r.kind() || l.kindAuthored() != r.kindAuthored())
    note("meta:kind");
  if (l.doc() != r.doc()) note("meta:doc");
  if (l.comment() != r.comment()) note("meta:comment");
  if (l.displayName() != r.displayName() ||
      l.displayNameAuthored() != r.displayNameAuthored())
    note("meta:displayName");
  if (l.instance_prototype() != r.instance_prototype()) {
    note("meta:instancePrototype");
  }
  if (l.apiSchemas() != r.apiSchemas() ||
      l.apiSchemasQualifier() != r.apiSchemasQualifier()) {
    note("meta:apiSchemas");
  }
  const StringListOpEdits& la = l.apiSchemaEdits();
  const StringListOpEdits& ra = r.apiSchemaEdits();
  if (la.authored != ra.authored || la.is_explicit != ra.is_explicit ||
      la.explicit_items != ra.explicit_items || la.added != ra.added ||
      la.prepended != ra.prepended || la.appended != ra.appended ||
      la.deleted != ra.deleted || la.ordered != ra.ordered) {
    note("meta:apiSchemasListOp");
  }
  if (PairListToStr(l.relocates()) != PairListToStr(r.relocates())) {
    note("meta:relocates");
  }
  if (!CompareDictValues(l.customData(), r.customData(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.customData(), r.customData()) ? "meta:customData(type-conflict)"
                                      : "meta:customData");
  }
  if (!CompareDictValues(l.assetInfo(), r.assetInfo(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.assetInfo(), r.assetInfo()) ? "meta:assetInfo(type-conflict)"
                                      : "meta:assetInfo");
  }
  if (!CompareDictValues(l.sdrMetadata(), r.sdrMetadata(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.sdrMetadata(), r.sdrMetadata()) ? "meta:sdrMetadata(type-conflict)"
                                      : "meta:sdrMetadata");
  }
  if (!CompareDictValues(l.clips(), r.clips(), opts, nullptr)) {
    note("meta:clips");
  }
  if (!ExtensionFieldsEqual(l.unknownFields(), r.unknownFields(), opts)) {
    note("meta:extensionFields");
  }
  const StringListOpEdits& lc = l.clipSetEdits();
  const StringListOpEdits& rc = r.clipSetEdits();
  if (lc.authored != rc.authored || lc.is_explicit != rc.is_explicit ||
      lc.explicit_items != rc.explicit_items || lc.added != rc.added ||
      lc.prepended != rc.prepended || lc.appended != rc.appended ||
      lc.deleted != rc.deleted || lc.ordered != rc.ordered) {
    note("meta:clipSets");
  }
  return equal;
}

bool ComparePrimSpecs(const PrimSpec &lhs, const PrimSpec &rhs,
                      const DiffOptions &opts,
                      std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.specifier() != rhs.specifier()) note("specifier");
  if (lhs.type_name() != rhs.type_name()) note("typeName");

  if (opts.compareMetadata) {
    if (!ComparePrimMeta(lhs.meta(), rhs.meta(), opts, reasons)) {
      equal = false;
    }
  }
  return equal;
}

// ---------------------------------------------------------------------------
// Property diff at one prim path.
// ---------------------------------------------------------------------------

void ComputePropDiff(const std::string &path, const PrimSpec &lhs,
                     const PrimSpec &rhs, const DiffOptions &opts,
                     std::unordered_map<std::string, PropDiff> &propDiffs) {
  const std::map<std::string, PropView> lprops = CollectProps(lhs);
  const std::map<std::string, PropView> rprops = CollectProps(rhs);

  PropDiff diff;

  for (const auto &kv : rprops) {
    if (lprops.find(kv.first) == lprops.end()) {
      diff.addedProps.push_back(kv.first);
    }
  }
  for (const auto &kv : lprops) {
    if (rprops.find(kv.first) == rprops.end()) {
      diff.deletedProps.push_back(kv.first);
    }
  }
  for (const auto &kv : lprops) {
    auto it = rprops.find(kv.first);
    if (it == rprops.end()) continue;
    std::vector<std::string> reasons;
    if (!ComparePropertyDetailed(lhs, kv.first, kv.second, rhs, it->second,
                                 opts, &reasons)) {
      diff.modifiedProps.push_back(kv.first);
      PropDiff::ModifiedProp mp;
      mp.name = kv.first;
      mp.lhs = FormatPropertyForDiff(lhs, kv.first, kv.second);
      mp.rhs = FormatPropertyForDiff(rhs, kv.first, it->second);
      mp.reasons = std::move(reasons);
      diff.modifiedPropDetails.push_back(std::move(mp));
    }
  }

  if (!diff.addedProps.empty() || !diff.deletedProps.empty() ||
      !diff.modifiedProps.empty()) {
    propDiffs[path] = std::move(diff);
  }
}

// ---------------------------------------------------------------------------
// Recursive prim-tree diff.
// ---------------------------------------------------------------------------

bool ComputeDiffImpl(uint32_t depth, const std::string &path,
                     const Layer &lhsLayer, const PrimSpec &lhs,
                     const Layer &rhsLayer, const PrimSpec &rhs,
                     std::unordered_map<std::string, PrimSpecDiff> &psDiffs,
                     std::unordered_map<std::string, PropDiff> &propDiffs,
                     const DiffOptions &opts,
                     std::vector<std::string> *selfReasons) {
  if (depth > kMaxTraversalDepth) {
    return false;
  }

  // This prim's own structural / metadata differences.
  std::vector<std::string> reasons;
  ComparePrimSpecs(lhs, rhs, opts, &reasons);
  bool hasDiff = !reasons.empty();

  // Property differences (reported separately under this path).
  ComputePropDiff(path, lhs, rhs, opts, propDiffs);

  // Children (matched by name; order changes are not reported as diffs).
  //
  // Sorted vectors of (name, spec) rather than two std::map<std::string, ...>
  // built and destroyed for EVERY visited prim: the maps cost two red-black
  // nodes plus a string copy per child, then three more lookup passes over
  // them. These hold pointers into the specs' own names (no copies) and are
  // merged in a single pass. Name order -- which the reported add/delete/
  // modify lists depend on -- is preserved.
  using ChildRef = std::pair<const std::string *, const PrimSpec *>;
  auto by_name = [](const ChildRef &a, const ChildRef &b) {
    return *a.first < *b.first;
  };
  auto collect = [&](const PrimSpec &parent, const Layer &layer,
                     std::vector<ChildRef> *out) {
    out->reserve(parent.child_indices().size());
    for (uint32_t ci : parent.child_indices()) {
      if (const PrimSpec *c = layer.prim(ci)) out->push_back({&c->name(), c});
    }
    std::stable_sort(out->begin(), out->end(), by_name);
    // Duplicate names: keep the LAST, matching the previous `map[name] = c`.
    if (!out->empty()) {
      size_t w = 0;
      for (size_t r = 1; r < out->size(); ++r) {
        if (*(*out)[r].first == *(*out)[w].first) {
          (*out)[w] = (*out)[r];
        } else {
          (*out)[++w] = (*out)[r];
        }
      }
      out->resize(w + 1);
    }
  };

  std::vector<ChildRef> lhs_children, rhs_children;
  collect(lhs, lhsLayer, &lhs_children);
  collect(rhs, rhsLayer, &rhs_children);

  PrimSpecDiff psDiff;

  // Added first, then deleted, then modified -- the emission order the three
  // separate map passes produced.
  for (const ChildRef &r : rhs_children) {
    if (!std::binary_search(lhs_children.begin(), lhs_children.end(), r,
                            by_name)) {
      psDiff.addedPS.push_back(*r.first);
      hasDiff = true;
    }
  }
  for (const ChildRef &l : lhs_children) {
    if (!std::binary_search(rhs_children.begin(), rhs_children.end(), l,
                            by_name)) {
      psDiff.deletedPS.push_back(*l.first);
      hasDiff = true;
    }
  }
  for (const ChildRef &l : lhs_children) {
    const auto it =
        std::lower_bound(rhs_children.begin(), rhs_children.end(), l, by_name);
    if (it == rhs_children.end() || *it->first != *l.first) continue;
    const std::string child_path = JoinPrimPath(path, *l.first);
    std::vector<std::string> childReasons;
    if (ComputeDiffImpl(depth + 1, child_path, lhsLayer, *l.second, rhsLayer,
                        *it->second, psDiffs, propDiffs, opts,
                        &childReasons)) {
      psDiff.modifiedPS.push_back(*l.first);
      ModifiedPrimSpec mps;
      mps.name = *l.first;
      mps.reasons = std::move(childReasons);
      psDiff.modifiedDetails.push_back(std::move(mps));
      hasDiff = true;
    }
  }

  if (!psDiff.addedPS.empty() || !psDiff.deletedPS.empty() ||
      !psDiff.modifiedPS.empty()) {
    psDiffs[path] = std::move(psDiff);
  }

  if (selfReasons) *selfReasons = std::move(reasons);
  return hasDiff;
}

// ---------------------------------------------------------------------------
// Layer (stage) metadata comparison.
// ---------------------------------------------------------------------------

bool CompareLayerMetas(const LayerMeta &lhs, const LayerMeta &rhs,
                       const DiffOptions &opts, LayerMetaDiff &out) {
  auto noteField = [&](const std::string &field, const std::string &ls,
                       const std::string &rs) {
    out.changedFields.push_back("~" + field);
    PropDiff::ModifiedProp mp;
    mp.name = field;
    mp.lhs = ls;
    mp.rhs = rs;
    out.details.push_back(std::move(mp));
  };

  auto cmpDouble = [&](const char *field, double a, double b) {
    if (!ScalarAlmostEqual(a, b, opts)) {
      noteField(field, DblStr(a), DblStr(b));
    }
  };

  cmpDouble("metersPerUnit", lhs.metersPerUnit, rhs.metersPerUnit);
  cmpDouble("timeCodesPerSecond", lhs.timeCodesPerSecond,
            rhs.timeCodesPerSecond);
  cmpDouble("framesPerSecond", lhs.framesPerSecond, rhs.framesPerSecond);
  cmpDouble("startTimeCode", lhs.startTimeCode, rhs.startTimeCode);
  cmpDouble("endTimeCode", lhs.endTimeCode, rhs.endTimeCode);
  cmpDouble("kilogramsPerUnit", lhs.kilogramsPerUnit, rhs.kilogramsPerUnit);

  if (lhs.upAxis != rhs.upAxis) {
    noteField("upAxis", lhs.upAxis, rhs.upAxis);
  }
  if (lhs.defaultPrim != rhs.defaultPrim ||
      lhs.defaultPrim_set != rhs.defaultPrim_set) {
    noteField("defaultPrim", lhs.defaultPrim, rhs.defaultPrim);
  }
  if (lhs.rootPrimOrder != rhs.rootPrimOrder ||
      lhs.rootPrimOrder_set != rhs.rootPrimOrder_set) {
    noteField("primOrder", std::to_string(lhs.rootPrimOrder.size()),
              std::to_string(rhs.rootPrimOrder.size()));
  }
  if (lhs.comment != rhs.comment) {
    noteField("comment", lhs.comment, rhs.comment);
  }
  if (lhs.doc != rhs.doc) {
    noteField("documentation", lhs.doc, rhs.doc);
  }
  if (lhs.owner != rhs.owner || lhs.owner_set != rhs.owner_set) {
    noteField("owner", lhs.owner, rhs.owner);
  }
  if (lhs.colorConfiguration != rhs.colorConfiguration) {
    noteField("colorConfiguration", lhs.colorConfiguration,
              rhs.colorConfiguration);
  }
  if (lhs.colorManagementSystem != rhs.colorManagementSystem) {
    noteField("colorManagementSystem", lhs.colorManagementSystem,
              rhs.colorManagementSystem);
  }
  if (lhs.renderSettingsPrimPath != rhs.renderSettingsPrimPath ||
      lhs.renderSettingsPrimPath_set != rhs.renderSettingsPrimPath_set) {
    noteField("renderSettingsPrimPath", lhs.renderSettingsPrimPath,
              rhs.renderSettingsPrimPath);
  }

  // subLayers (count + per-layer asset path / offset).
  bool subEqual = (lhs.subLayers.size() == rhs.subLayers.size());
  auto offset_of = [](const LayerMeta &m,
                      size_t i) -> std::pair<double, double> {
    if (i < m.subLayerOffsets.size()) return m.subLayerOffsets[i];
    return {0.0, 1.0};
  };
  for (size_t i = 0; subEqual && i < lhs.subLayers.size(); ++i) {
    const bool pathOk =
        opts.fuzzyAssetPaths
            ? AssetPathStringsEquivalentForDiff(lhs.subLayers[i],
                                                rhs.subLayers[i])
            : (lhs.subLayers[i] == rhs.subLayers[i]);
    if (!pathOk || offset_of(lhs, i) != offset_of(rhs, i)) {
      subEqual = false;
    }
  }
  if (!subEqual) {
    noteField("subLayers", std::to_string(lhs.subLayers.size()),
              std::to_string(rhs.subLayers.size()));
  }

  // customLayerData / expressionVariables dictionaries.
  {
    std::vector<std::string> keys;
    if (!CompareDictValues(lhs.customLayerData, rhs.customLayerData, opts,
                           &keys)) {
      for (const auto &k : keys) {
        out.changedFields.push_back("customLayerData:" + k);
      }
    }
  }
  {
    std::vector<std::string> keys;
    if (!CompareDictValues(lhs.expressionVariables, rhs.expressionVariables,
                           opts, &keys)) {
      for (const auto &k : keys) {
        out.changedFields.push_back("expressionVariables:" + k);
      }
    }
  }
  if (!ExtensionFieldsEqual(lhs.unknownFields, rhs.unknownFields, opts)) {
    noteField("extensionFields", std::to_string(lhs.unknownFields.size()),
              std::to_string(rhs.unknownFields.size()));
  }

  return out.changed();
}

std::string EscapeJSON(const std::string &str) {
  std::string result;
  for (char c : str) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xff);
          result += buf;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> CenterValuePairForDiff(
    const std::string &lhs, const std::string &rhs, size_t window) {
  if (lhs.size() <= window && rhs.size() <= window) {
    return {lhs, rhs};
  }
  // First differing byte offset.
  size_t d = 0;
  const size_t n = std::min(lhs.size(), rhs.size());
  while (d < n && lhs[d] == rhs[d]) ++d;
  // Keep ~1/3 of the window as leading context before the difference.
  const size_t lead = window / 3;
  const size_t start = (d > lead) ? (d - lead) : 0;
  auto clip = [&](const std::string &s) -> std::string {
    std::string out;
    if (start > 0) out += "\xE2\x80\xA6";  // UTF-8 ellipsis
    const size_t end = std::min(s.size(), start + window);
    if (start < s.size()) out += s.substr(start, end - start);
    if (end < s.size()) out += "\xE2\x80\xA6";
    return out;
  };
  return {clip(lhs), clip(rhs)};
}

void Diff(const Layer &lhs, const Layer &rhs,
          std::unordered_map<std::string, PrimSpecDiff> &psDiffs,
          std::unordered_map<std::string, PropDiff> &propDiffs,
          const DiffOptions &opts, LayerMetaDiff *layerMetaDiff) {
  // Root prims by name.
  std::map<std::string, const PrimSpec *> lhs_roots;
  std::map<std::string, const PrimSpec *> rhs_roots;
  for (uint32_t ri : lhs.root_indices()) {
    if (const PrimSpec *p = lhs.prim(ri)) lhs_roots[p->name()] = p;
  }
  for (uint32_t ri : rhs.root_indices()) {
    if (const PrimSpec *p = rhs.prim(ri)) rhs_roots[p->name()] = p;
  }

  PrimSpecDiff rootDiff;

  for (const auto &kv : rhs_roots) {
    if (lhs_roots.find(kv.first) == lhs_roots.end()) {
      rootDiff.addedPS.push_back(kv.first);
    }
  }
  for (const auto &kv : lhs_roots) {
    if (rhs_roots.find(kv.first) == rhs_roots.end()) {
      rootDiff.deletedPS.push_back(kv.first);
    }
  }
  for (const auto &kv : lhs_roots) {
    auto it = rhs_roots.find(kv.first);
    if (it == rhs_roots.end()) continue;
    const std::string prim_path = "/" + kv.first;
    std::vector<std::string> selfReasons;
    if (ComputeDiffImpl(0, prim_path, lhs, *kv.second, rhs, *it->second,
                        psDiffs, propDiffs, opts, &selfReasons)) {
      rootDiff.modifiedPS.push_back(kv.first);
      ModifiedPrimSpec mps;
      mps.name = kv.first;
      mps.reasons = std::move(selfReasons);
      rootDiff.modifiedDetails.push_back(std::move(mps));
    }
  }

  // Stage / layer metadata.
  LayerMetaDiff localLayerDiff;
  if (opts.compareMetadata) {
    CompareLayerMetas(lhs.meta(), rhs.meta(), opts, localLayerDiff);
  }
  if (layerMetaDiff) {
    *layerMetaDiff = std::move(localLayerDiff);
  }

  if (!rootDiff.addedPS.empty() || !rootDiff.deletedPS.empty() ||
      !rootDiff.modifiedPS.empty()) {
    psDiffs["/"] = std::move(rootDiff);
  }
}

std::string DiffToText(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name,
                       const std::string &rhs_name, const DiffOptions &opts) {
  std::unordered_map<std::string, PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto joinReasons = [](const std::vector<std::string> &reasons) {
    std::stringstream r;
    for (size_t i = 0; i < reasons.size(); ++i) {
      if (i) r << ", ";
      r << reasons[i];
    }
    return r.str();
  };

  bool any = false;
  std::stringstream ss;
  ss << "--- " << lhs_name << "\n";
  ss << "+++ " << rhs_name << "\n";

  // Stage (layer) metadata differences.
  if (layerMetaDiff.changed()) {
    any = true;
    ss << "~ <stage metadata> (Stage metadata modified: "
       << joinReasons(layerMetaDiff.changedFields) << ")\n";
    for (const auto &d : layerMetaDiff.details) {
      ss << "  - " << d.name << ": " << d.lhs << "\n";
      ss << "  + " << d.name << ": " << d.rhs << "\n";
    }
  }

  // Sort paths for consistent output.
  std::set<std::string> sortedPaths;
  for (const auto &entry : psDiffs) sortedPaths.insert(entry.first);
  for (const auto &entry : propDiffs) sortedPaths.insert(entry.first);

  for (const std::string &path : sortedPaths) {
    auto psIt = psDiffs.find(path);
    if (psIt != psDiffs.end()) {
      const PrimSpecDiff &psDiff = psIt->second;

      for (const std::string &name : psDiff.deletedPS) {
        any = true;
        ss << "- " << JoinPrimPath(path, name) << " (PrimSpec deleted)\n";
      }
      for (const std::string &name : psDiff.addedPS) {
        any = true;
        ss << "+ " << JoinPrimPath(path, name) << " (PrimSpec added)\n";
      }
      // Prefer modifiedDetails (carry reasons); fall back to modifiedPS.
      if (!psDiff.modifiedDetails.empty()) {
        for (const ModifiedPrimSpec &m : psDiff.modifiedDetails) {
          any = true;
          ss << "~ " << JoinPrimPath(path, m.name) << " (PrimSpec modified";
          if (!m.reasons.empty()) ss << ": " << joinReasons(m.reasons);
          ss << ")\n";
        }
      } else {
        for (const std::string &name : psDiff.modifiedPS) {
          any = true;
          ss << "~ " << JoinPrimPath(path, name) << " (PrimSpec modified)\n";
        }
      }
    }

    auto propIt = propDiffs.find(path);
    if (propIt != propDiffs.end()) {
      const PropDiff &propDiff = propIt->second;

      for (const std::string &name : propDiff.deletedProps) {
        any = true;
        ss << "- " << path << "." << name << " (Property deleted)\n";
      }
      for (const std::string &name : propDiff.addedProps) {
        any = true;
        ss << "+ " << path << "." << name << " (Property added)\n";
      }
      for (const PropDiff::ModifiedProp &modified :
           propDiff.modifiedPropDetails) {
        any = true;
        ss << "~ " << path << "." << modified.name << " (Property modified";
        if (!modified.reasons.empty()) {
          ss << ": " << joinReasons(modified.reasons);
        }
        ss << ")\n";
        // Center long values on the first difference so a shared prefix (e.g.
        // asset paths) does not hide what actually changed.
        auto pr = CenterValuePairForDiff(modified.lhs, modified.rhs);
        ss << "  - " << pr.first << "\n";
        ss << "  + " << pr.second << "\n";
      }
    }
  }

  if (!any) {
    return "No differences found.\n";
  }

  return ss.str();
}

std::string DiffToJSON(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name,
                       const std::string &rhs_name, const DiffOptions &opts) {
  std::unordered_map<std::string, PrimSpecDiff> psDiffs;
  std::unordered_map<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto emitStrArray = [](std::stringstream &o,
                         const std::vector<std::string> &v) {
    o << "[";
    for (size_t i = 0; i < v.size(); ++i) {
      if (i > 0) o << ", ";
      o << "\"" << EscapeJSON(v[i]) << "\"";
    }
    o << "]";
  };

  // Deterministic key order.
  std::vector<std::string> psKeys, propKeys;
  psKeys.reserve(psDiffs.size());
  propKeys.reserve(propDiffs.size());
  for (const auto &e : psDiffs) psKeys.push_back(e.first);
  for (const auto &e : propDiffs) propKeys.push_back(e.first);
  std::sort(psKeys.begin(), psKeys.end());
  std::sort(propKeys.begin(), propKeys.end());

  std::stringstream ss;
  ss << "{\n";
  ss << "  \"comparison\": {\n";
  ss << "    \"left\": \"" << EscapeJSON(lhs_name) << "\",\n";
  ss << "    \"right\": \"" << EscapeJSON(rhs_name) << "\"\n";
  ss << "  },\n";

  // PrimSpec differences.
  ss << "  \"primspec_diffs\": {\n";
  bool firstPrimDiff = true;
  for (const std::string &path : psKeys) {
    const PrimSpecDiff &diff = psDiffs[path];
    if (!firstPrimDiff) ss << ",\n";
    firstPrimDiff = false;

    ss << "    \"" << EscapeJSON(path) << "\": {\n";

    ss << "      \"added\": ";
    emitStrArray(ss, diff.addedPS);
    ss << ",\n";

    ss << "      \"deleted\": ";
    emitStrArray(ss, diff.deletedPS);
    ss << ",\n";

    ss << "      \"modified\": ";
    emitStrArray(ss, diff.modifiedPS);
    ss << ",\n";

    ss << "      \"modified_details\": [";
    for (size_t i = 0; i < diff.modifiedDetails.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << "{\"name\":\"" << EscapeJSON(diff.modifiedDetails[i].name)
         << "\", \"reasons\":";
      emitStrArray(ss, diff.modifiedDetails[i].reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Property differences.
  ss << "  \"property_diffs\": {\n";
  bool firstPropDiff = true;
  for (const std::string &path : propKeys) {
    const PropDiff &diff = propDiffs[path];
    if (!firstPropDiff) ss << ",\n";
    firstPropDiff = false;

    ss << "    \"" << EscapeJSON(path) << "\": {\n";

    ss << "      \"added\": ";
    emitStrArray(ss, diff.addedProps);
    ss << ",\n";

    ss << "      \"deleted\": ";
    emitStrArray(ss, diff.deletedProps);
    ss << ",\n";

    ss << "      \"modified\": ";
    emitStrArray(ss, diff.modifiedProps);
    ss << ",\n";

    ss << "      \"modified_details\": [";
    for (size_t i = 0; i < diff.modifiedPropDetails.size(); ++i) {
      if (i > 0) ss << ", ";
      const PropDiff::ModifiedProp &modified = diff.modifiedPropDetails[i];
      ss << "{\"name\":\"" << EscapeJSON(modified.name)
         << "\", \"left\":\"" << EscapeJSON(modified.lhs)
         << "\", \"right\":\"" << EscapeJSON(modified.rhs)
         << "\", \"reasons\":";
      emitStrArray(ss, modified.reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Stage / layer metadata differences.
  ss << "  \"layer_meta_diff\": {\n";
  ss << "    \"changed\": ";
  emitStrArray(ss, layerMetaDiff.changedFields);
  ss << ",\n";
  ss << "    \"details\": [";
  for (size_t i = 0; i < layerMetaDiff.details.size(); ++i) {
    if (i > 0) ss << ", ";
    const PropDiff::ModifiedProp &d = layerMetaDiff.details[i];
    ss << "{\"name\":\"" << EscapeJSON(d.name)
       << "\", \"left\":\"" << EscapeJSON(d.lhs)
       << "\", \"right\":\"" << EscapeJSON(d.rhs) << "\"}";
  }
  ss << "]\n";
  ss << "  }\n";

  ss << "}\n";

  return ss.str();
}

}  // namespace next
}  // namespace lightusd
