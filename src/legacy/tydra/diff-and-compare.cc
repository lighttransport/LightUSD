#include "diff-and-compare.hh"
#include "../layer.hh"
#include "../pprint-enum.hh"
#include "../value-pprint.hh"
#include "../math-util.inc"
#include "../value-type-macros.inc"
#include "../core/meta-variable.hh"
#include "../core/prim-metas.hh"
#include "../core/attr-metas.hh"
#include "../core/composition-types.hh"
#include "../hash-util.hh"
#include "../tiny-format.hh"
#include "../common-macros.inc"
#include <array>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

// This translation unit performs exact bit-level value comparisons for diffing
// (e.g. the `a == b` fast path in ScalarAlmostEqual deliberately treats
// inf==inf as equal and +0/-0 as equal; layerOffset compares are exact). These
// intentional float equality checks trip -Wfloat-equal under -Weverything.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace lightusd {
namespace tydra {

namespace detail {

static std::string TruncateForDiff(const std::string &s) {
  // Retain enough of each value that the differing region survives for the
  // diff-aware centering done at render time (CenterValuePairForDiff). Asset
  // paths and moderate arrays fit; pathological huge arrays are still bounded.
  constexpr size_t kMaxDiffValueChars = 4096;
  if (s.size() <= kMaxDiffValueChars) {
    return s;
  }
  return s.substr(0, kMaxDiffValueChars) + "...";
}

static std::string JoinPrimPath(const std::string &parent,
                                const std::string &child) {
  if (parent.empty() || parent == "/") {
    return "/" + child;
  }
  return parent + "/" + child;
}

static std::string JoinPathList(const std::vector<Path> &paths) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i > 0) {
      ss << ", ";
    }
    ss << "<" << paths[i].full_path_name() << ">";
  }
  ss << "]";
  return ss.str();
}

static std::string NormalizeAssetPathForDiff(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
    path.erase(0, 2);
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

static std::string AssetPathLeafForDiff(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

static bool HasPathComponentSuffix(const std::string &path,
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

static bool AssetPathStringsEquivalentForDiff(const std::string &lhs_path,
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

// Forward declarations (ValuesEquivalentForDiff <-> CompareDictionaries recurse).
static bool ValuesEquivalentForDiff(const value::Value &lhs,
                                    const value::Value &rhs,
                                    const DiffOptions &opts, int depth = 0);
static bool CompareDictionaries(const Dictionary &lhs, const Dictionary &rhs,
                                const DiffOptions &opts, int depth,
                                std::vector<std::string> *changed);

//
// Per-scalar tolerant comparison. `a == b` is the fast path (also makes
// inf==inf true and +0/-0 equal); then optional absolute epsilon; then ULP.
//
static bool ScalarAlmostEqual(float a, float b, const DiffOptions &o) {
  if (a == b) return true;
  if (o.absEps >= 0.0 &&
      std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= o.absEps) {
    return true;
  }
  return lightusd::math::almost_equals_by_ulps(a, b, o.floatUlps);
}
static bool ScalarAlmostEqual(double a, double b, const DiffOptions &o) {
  if (a == b) return true;
  if (o.absEps >= 0.0 && std::fabs(a - b) <= o.absEps) return true;
  return lightusd::math::almost_equals_by_ulps(a, b, o.doubleUlps);
}
static bool ScalarAlmostEqual(value::half a, value::half b,
                              const DiffOptions &o) {
  if (a.value == b.value) return true;  // bitwise-equal half
  return ScalarAlmostEqual(value::half_to_float(a), value::half_to_float(b), o);
}

//
// DiffNumericEqual: element-wise tolerant comparison for every float-backed
// value type. Scalar overloads must precede the aggregate templates so
// (two-phase) name lookup at template-definition resolves them for builtin
// element types (which have no ADL associated namespace).
//
static bool DiffNumericEqual(float a, float b, const DiffOptions &o) {
  return ScalarAlmostEqual(a, b, o);
}
static bool DiffNumericEqual(double a, double b, const DiffOptions &o) {
  return ScalarAlmostEqual(a, b, o);
}
static bool DiffNumericEqual(value::half a, value::half b,
                             const DiffOptions &o) {
  return ScalarAlmostEqual(a, b, o);
}
static bool DiffNumericEqual(bool a, bool b, const DiffOptions &) {
  return a == b;
}
static bool DiffNumericEqual(int32_t a, int32_t b, const DiffOptions &) {
  return a == b;
}
static bool DiffNumericEqual(uint32_t a, uint32_t b, const DiffOptions &) {
  return a == b;
}
static bool DiffNumericEqual(int64_t a, int64_t b, const DiffOptions &) {
  return a == b;
}
static bool DiffNumericEqual(uint64_t a, uint64_t b, const DiffOptions &) {
  return a == b;
}

// std::array<E,N> -> halfN / intN / uintN / floatN / doubleN
template <typename E, std::size_t N>
static bool DiffNumericEqual(const std::array<E, N> &a,
                             const std::array<E, N> &b, const DiffOptions &o) {
  for (std::size_t i = 0; i < N; i++) {
    if (!DiffNumericEqual(a[i], b[i], o)) return false;
  }
  return true;
}

// Indexed role/vector/quat structs (operator[] yields the scalar component).
#define LIGHTUSD_DIFF_INDEXED(TY, N)                                         \
  static bool DiffNumericEqual(const value::TY &a, const value::TY &b,       \
                               const DiffOptions &o) {                       \
    for (std::size_t i = 0; i < (N); i++) {                                  \
      if (!DiffNumericEqual(a[i], b[i], o)) return false;                    \
    }                                                                        \
    return true;                                                            \
  }
LIGHTUSD_DIFF_INDEXED(quath, 4)
LIGHTUSD_DIFF_INDEXED(quatf, 4)
LIGHTUSD_DIFF_INDEXED(quatd, 4)
LIGHTUSD_DIFF_INDEXED(normal3h, 3)
LIGHTUSD_DIFF_INDEXED(normal3f, 3)
LIGHTUSD_DIFF_INDEXED(normal3d, 3)
LIGHTUSD_DIFF_INDEXED(vector3h, 3)
LIGHTUSD_DIFF_INDEXED(vector3f, 3)
LIGHTUSD_DIFF_INDEXED(vector3d, 3)
LIGHTUSD_DIFF_INDEXED(point3h, 3)
LIGHTUSD_DIFF_INDEXED(point3f, 3)
LIGHTUSD_DIFF_INDEXED(point3d, 3)
LIGHTUSD_DIFF_INDEXED(color3f, 3)
LIGHTUSD_DIFF_INDEXED(color3d, 3)
LIGHTUSD_DIFF_INDEXED(color4h, 4)
LIGHTUSD_DIFF_INDEXED(color4f, 4)
LIGHTUSD_DIFF_INDEXED(color4d, 4)
LIGHTUSD_DIFF_INDEXED(texcoord2h, 2)
LIGHTUSD_DIFF_INDEXED(texcoord2f, 2)
LIGHTUSD_DIFF_INDEXED(texcoord2d, 2)
LIGHTUSD_DIFF_INDEXED(texcoord3h, 3)
LIGHTUSD_DIFF_INDEXED(texcoord3f, 3)
LIGHTUSD_DIFF_INDEXED(texcoord3d, 3)
#undef LIGHTUSD_DIFF_INDEXED

// Matrices (.m[i][j]) and frame4d.
#define LIGHTUSD_DIFF_MATRIX(TY, N)                                          \
  static bool DiffNumericEqual(const value::TY &a, const value::TY &b,       \
                               const DiffOptions &o) {                       \
    for (std::size_t i = 0; i < (N); i++) {                                  \
      for (std::size_t j = 0; j < (N); j++) {                                \
        if (!DiffNumericEqual(a.m[i][j], b.m[i][j], o)) return false;        \
      }                                                                      \
    }                                                                        \
    return true;                                                            \
  }
LIGHTUSD_DIFF_MATRIX(matrix2d, 2)
LIGHTUSD_DIFF_MATRIX(matrix3d, 3)
LIGHTUSD_DIFF_MATRIX(matrix4d, 4)
LIGHTUSD_DIFF_MATRIX(frame4d, 4)
#undef LIGHTUSD_DIFF_MATRIX

// std::vector<bool> uses a proxy reference, so it needs its own overload.
static bool DiffNumericEqual(const std::vector<bool> &a,
                             const std::vector<bool> &b, const DiffOptions &) {
  return a == b;
}

// std::vector<E> for every element type handled above. Defined last so the
// element overloads/templates are visible to ordinary lookup.
template <typename E>
static bool DiffNumericEqual(const std::vector<E> &a, const std::vector<E> &b,
                             const DiffOptions &o) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); i++) {
    if (!DiffNumericEqual(a[i], b[i], o)) return false;
  }
  return true;
}

template <typename T>
static bool TryTypedValueEqual(const value::Value &l, const value::Value &r,
                               const DiffOptions &o, bool &handled) {
  if (l.type_id() != value::TypeTraits<T>::type_id()) return false;
  handled = true;
  const T *lp = l.as<T>();
  const T *rp = r.as<T>();  // non-strict: role/underlying-compatible RHS matches
  if (!lp || !rp) return false;
  return DiffNumericEqual(*lp, *rp, o);
}

// Dispatch over every numeric value type. Exactly one branch (keyed on the LHS
// type id) sets `handled`. Non-numeric types leave handled=false.
static bool NumericValueAlmostEqual(const value::Value &l, const value::Value &r,
                                    const DiffOptions &o, bool &handled) {
  handled = false;
  bool eq = false;
#define LIGHTUSD_TRY_ONE(T) \
  if (!handled) { eq = detail::TryTypedValueEqual<T>(l, r, o, handled); }
  APPLY_FUNC_TO_NUMERIC_VALUE_TYPES(LIGHTUSD_TRY_ONE)
#undef LIGHTUSD_TRY_ONE
  return eq;
}

static bool ValuesEquivalentForDiff(const value::Value &lhs,
                                    const value::Value &rhs,
                                    const DiffOptions &opts, int depth) {
  // 1. AssetPath fuzzy equivalence (path suffix / leaf).
  if (opts.fuzzyAssetPaths) {
    const auto lhs_asset = lhs.get_value<value::AssetPath>(false);
    const auto rhs_asset = rhs.get_value<value::AssetPath>(false);
    if (lhs_asset && rhs_asset) {
      return AssetPathStringsEquivalentForDiff(
          lhs_asset.value().GetAssetPath(), rhs_asset.value().GetAssetPath());
    }
  }

  // 2. Nested dictionary (customData / assetInfo / sdrMetadata / clips leaves).
  if (const Dictionary *ld = lhs.as<Dictionary>()) {
    if (const Dictionary *rd = rhs.as<Dictionary>()) {
      return CompareDictionaries(*ld, *rd, opts, depth + 1, nullptr);
    }
  }

  // 3. Numeric (incl. matrix4d / quatf / vecN + arrays) -> ULP / eps.
  bool handled = false;
  const bool eq = NumericValueAlmostEqual(lhs, rhs, opts, handled);
  if (handled) return eq;

  // 4. token / string / bool-block / etc. -> exact rendered-value compare.
  return value::pprint_value(lhs) == value::pprint_value(rhs);
}

static std::string ListEditQualForDiff(ListEditQual qual) {
  std::string s = lightusd::to_string(qual);
  if (s.empty()) {
    s = "explicit";
  }
  return s;
}

static const char *RelationshipTypeName(Relationship::Type type) {
  switch (type) {
    case Relationship::Type::DefineOnly:
      return "define";
    case Relationship::Type::Path:
      return "path";
    case Relationship::Type::PathVector:
      return "pathVector";
    case Relationship::Type::ValueBlock:
      return "blocked";
  }
  return "unknown";
}

static std::string FormatAttributeForDiff(const Attribute &attr) {
  std::stringstream ss;
  ss << "attr";
  ss << " type=" << attr.type_name();
  ss << " variability=" << lightusd::to_string(attr.variability());
  if (attr.is_varying_authored()) {
    ss << " varyingAuthored=true";
  }

  if (attr.has_connections()) {
    ss << " connections=" << JoinPathList(attr.connections());
  }
  if (attr.is_blocked()) {
    ss << " value=None";
  } else if (attr.has_value()) {
    ss << " value="
       << TruncateForDiff(value::pprint_value(attr.get_var().value_raw()));
  }
  if (attr.has_timesamples()) {
    const auto &samples = attr.get_var().ts_raw().get_samples();
    ss << " timeSamples=" << samples.size();
    if (!samples.empty()) {
      ss << " firstTime=" << samples.front().t
         << " lastTime=" << samples.back().t;
    }
  }

  return ss.str();
}

static std::string FormatRelationshipForDiff(const Relationship &rel) {
  std::stringstream ss;
  ss << "rel";
  ss << " type=" << RelationshipTypeName(rel.type);
  if (rel.is_varying_authored()) {
    ss << " varyingAuthored=true";
  }

  if (rel.is_path()) {
    ss << " target=<" << rel.targetPath.full_path_name() << ">";
  } else if (rel.is_pathvector()) {
    ss << " targets=" << JoinPathList(rel.targetPathVector);
  } else if (rel.is_blocked()) {
    ss << " target=None";
  }

  return ss.str();
}

static std::string FormatPropertyForDiff(const Property &prop) {
  std::stringstream ss;
  if (prop.has_custom()) {
    ss << "custom ";
  }
  ss << "listOp=" << ListEditQualForDiff(prop.get_listedit_qual()) << " ";

  if (prop.is_attribute()) {
    ss << FormatAttributeForDiff(prop.get_attribute());
  } else if (prop.is_relationship()) {
    ss << FormatRelationshipForDiff(prop.get_relationship());
  } else {
    ss << "empty";
  }

  return ss.str();
}

struct FNV1StringHash {
  size_t operator()(const std::string &s) const noexcept {
    static constexpr uint64_t kFNV_Prime = 0x00000100000001B3ull;
    static constexpr uint64_t kFNV_Offset_Basis = 0xcbf29ce484222325ull;

    uint64_t hash = kFNV_Offset_Basis;
    for (char ch : s) {
      hash = (kFNV_Prime * hash) ^ static_cast<unsigned char>(ch);
    }
    return static_cast<size_t>(hash);
  }
};

// Compare two metadata dictionaries (map<string, MetaVariable>). Records the
// changed keys (prefixed +added / -removed / ~modified) into `changed` if
// non-null; otherwise returns early on the first difference.
static bool CompareDictionaries(const Dictionary &lhs, const Dictionary &rhs,
                                const DiffOptions &opts, int depth,
                                std::vector<std::string> *changed) {
  if (depth > static_cast<int>(kMaxDefaultTraversalLimit)) {
    if (changed) changed->push_back("<too-deep>");
    return false;
  }
  bool equal = true;
  for (const auto &kv : lhs) {
    auto it = rhs.find(kv.first);
    if (it == rhs.end()) {
      equal = false;
      if (changed) changed->push_back("-" + kv.first); else return false;
    } else if (!ValuesEquivalentForDiff(kv.second.get_raw_value(),
                                        it->second.get_raw_value(), opts,
                                        depth + 1)) {
      equal = false;
      if (changed) changed->push_back("~" + kv.first); else return false;
    }
  }
  for (const auto &kv : rhs) {
    if (lhs.find(kv.first) == lhs.end()) {
      equal = false;
      if (changed) changed->push_back("+" + kv.first); else return false;
    }
  }
  return equal;
}

static std::string StripListPrefix(const std::string &k) {
  if (!k.empty() && (k[0] == '+' || k[0] == '-' || k[0] == '~')) {
    return k.substr(1);
  }
  return k;
}

// Append "meta:<key>" reasons for each changed dictionary key.
static bool CompareMetaDicts(const Dictionary &lhs, const Dictionary &rhs,
                             const DiffOptions &opts,
                             std::vector<std::string> *reasons) {
  std::vector<std::string> keys;
  if (CompareDictionaries(lhs, rhs, opts, 0, &keys)) return true;
  if (reasons) {
    for (const auto &k : keys) reasons->push_back("meta:" + StripListPrefix(k));
  }
  return false;
}

// Property / Attribute metadata (AttrMetas = MetadataBase + stringData).
static bool CompareAttrMetas(const AttrMetas &lhs, const AttrMetas &rhs,
                             const DiffOptions &opts,
                             std::vector<std::string> *reasons) {
  bool equal = CompareMetaDicts(lhs.data(), rhs.data(), opts, reasons);
  if (lhs.stringData.size() != rhs.stringData.size()) {
    equal = false;
    if (reasons) reasons->push_back("meta:stringData");
  } else {
    for (size_t i = 0; i < lhs.stringData.size(); ++i) {
      if (lhs.stringData[i].value != rhs.stringData[i].value) {
        equal = false;
        if (reasons) reasons->push_back("meta:stringData");
        break;
      }
    }
  }
  return equal;
}

// --- canonical stringifiers for PrimMetas composition arcs ---

template <typename Elem>
static std::string ArcOpsToStr(
    const nonstd::optional<
        std::vector<std::pair<ListEditQual, std::vector<Elem>>>> &a) {
  if (!a) return "<none>";
  std::stringstream ss;
  for (const auto &op : a.value()) {
    ss << ListEditQualForDiff(op.first) << "[";
    for (const auto &e : op.second) ss << lightusd::to_string(e) << ",";
    ss << "]";
  }
  return ss.str();
}

static std::string VariantSetsToStr(
    const nonstd::optional<
        std::vector<std::pair<ListEditQual, std::vector<std::string>>>> &a) {
  if (!a) return "<none>";
  std::stringstream ss;
  for (const auto &op : a.value()) {
    ss << ListEditQualForDiff(op.first) << "[";
    for (const auto &e : op.second) ss << e << ",";
    ss << "]";
  }
  return ss.str();
}

static std::string VariantsToStr(
    const nonstd::optional<VariantSelectionMap> &a) {
  if (!a) return "<none>";
  std::stringstream ss;
  for (const auto &kv : a.value()) ss << kv.first << "=" << kv.second << ",";
  return ss.str();
}

static std::string ApiSchemasToStr(const APISchemas &a) {
  std::stringstream ss;
  ss << ListEditQualForDiff(a.listOpQual) << "[";
  for (const auto &n : a.names) {
    ss << lightusd::to_string(n.first);
    if (!n.second.empty()) ss << ":" << n.second;
    ss << ",";
  }
  for (const auto &u : a.unknownSchemas) {
    ss << u.first;
    if (!u.second.empty()) ss << ":" << u.second;
    ss << ",";
  }
  ss << "]";
  if (a.explicitlyEmpty) ss << "None";
  return ss.str();
}

// Prim metadata (PrimMetas = MetadataBase + composition arcs + apiSchemas).
// The derived/order-hint members (primChildren, properties, arc_origins,
// inheritPaths, ...) are intentionally NOT compared (composition byproducts).
static bool ComparePrimMeta(const PrimMeta &lhs, const PrimMeta &rhs,
                            const DiffOptions &opts,
                            std::vector<std::string> *reasons) {
  bool equal = CompareMetaDicts(lhs.data(), rhs.data(), opts, reasons);

  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.has_apiSchemas() != rhs.has_apiSchemas() ||
      (lhs.has_apiSchemas() &&
       ApiSchemasToStr(lhs.get_apiSchemas()) !=
           ApiSchemasToStr(rhs.get_apiSchemas()))) {
    note("meta:apiSchemas");
  }
  if (ArcOpsToStr(lhs.references) != ArcOpsToStr(rhs.references)) {
    note("meta:references");
  }
  if (ArcOpsToStr(lhs.payload) != ArcOpsToStr(rhs.payload)) {
    note("meta:payload");
  }
  if (ArcOpsToStr(lhs.inherits) != ArcOpsToStr(rhs.inherits)) {
    note("meta:inherits");
  }
  if (ArcOpsToStr(lhs.specializes) != ArcOpsToStr(rhs.specializes)) {
    note("meta:specializes");
  }
  if (VariantSetsToStr(lhs.variantSets) != VariantSetsToStr(rhs.variantSets)) {
    note("meta:variantSets");
  }
  if (VariantsToStr(lhs.variants) != VariantsToStr(rhs.variants)) {
    note("meta:variants");
  }
  if (lhs.unregisteredMetas != rhs.unregisteredMetas) {
    note("meta:unregistered");
  }
  if (!CompareDictionaries(lhs.meta, rhs.meta, opts, 0, nullptr)) {
    note("meta:other");
  }
  return equal;
}

static bool CompareAttributeValues(const Attribute &lhs, const Attribute &rhs,
                                   const DiffOptions &opts,
                                   std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.type_name() != rhs.type_name()) {
    const uint32_t lhsUnderlying = value::GetUnderlyingTypeId(lhs.type_name());
    const uint32_t rhsUnderlying = value::GetUnderlyingTypeId(rhs.type_name());
    if (lhsUnderlying == value::TYPE_ID_INVALID ||
        rhsUnderlying == value::TYPE_ID_INVALID ||
        lhsUnderlying != rhsUnderlying) {
      note("type");
    }
  }
  if (lhs.variability() != rhs.variability()) note("variability");
  if (lhs.is_varying_authored() != rhs.is_varying_authored()) {
    note("varyingAuthored");
  }
  if (lhs.is_blocked() != rhs.is_blocked()) note("blocked");

  if (lhs.has_connections() != rhs.has_connections()) {
    note("connections");
  } else if (lhs.has_connections() && (lhs.connections() != rhs.connections())) {
    note("connections");
  }

  if (lhs.has_value() != rhs.has_value()) {
    note("value");
  } else if (lhs.has_value()) {
    const auto &lhsVar = lhs.get_var();
    const auto &rhsVar = rhs.get_var();
    bool typeOk = (lhsVar.type_id() == rhsVar.type_id());
    if (!typeOk) {
      const uint32_t lu = value::GetUnderlyingTypeId(lhsVar.type_name());
      const uint32_t ru = value::GetUnderlyingTypeId(rhsVar.type_name());
      typeOk = (lu != value::TYPE_ID_INVALID && lu == ru);
    }
    if (!typeOk) {
      note("value");
    } else if (!ValuesEquivalentForDiff(lhsVar.value_raw(), rhsVar.value_raw(),
                                        opts)) {
      note("value");
    }
  }

  if (lhs.has_timesamples() != rhs.has_timesamples()) {
    note("timeSamples");
  } else if (lhs.has_timesamples()) {
    const auto &lhsSamples = lhs.get_var().ts_raw().get_samples();
    const auto &rhsSamples = rhs.get_var().ts_raw().get_samples();
    DiffOptions timeOpts = opts;
    timeOpts.doubleUlps = opts.timeUlps;
    bool tsEqual = (lhsSamples.size() == rhsSamples.size());
    for (size_t i = 0; tsEqual && i < lhsSamples.size(); ++i) {
      if (!ScalarAlmostEqual(lhsSamples[i].t, rhsSamples[i].t, timeOpts) ||
          lhsSamples[i].blocked != rhsSamples[i].blocked ||
          (!lhsSamples[i].blocked &&
           !ValuesEquivalentForDiff(lhsSamples[i].value, rhsSamples[i].value,
                                    opts))) {
        tsEqual = false;
      }
    }
    if (!tsEqual) note("timeSamples");
  }

  if (opts.compareMetadata) {
    if (!CompareAttrMetas(lhs.metas(), rhs.metas(), opts, reasons)) {
      equal = false;
    }
  }

  return equal;
}

static bool CompareRelationshipValues(const Relationship &lhs,
                                      const Relationship &rhs,
                                      const DiffOptions &opts,
                                      std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.type != rhs.type) note("relType");
  if (lhs.get_listedit_qual() != rhs.get_listedit_qual()) note("listOp");
  if (lhs.is_varying_authored() != rhs.is_varying_authored()) {
    note("varyingAuthored");
  }
  // NOTE: Path::operator== returns false for an INVALID path (even vs another
  // invalid), so two empty single-target paths (the normal case for a
  // multi-target `pathVector` relationship) must be treated as equal explicitly
  // -- otherwise every multi-target relationship reports a spurious "target".
  if (lhs.targetPath.is_valid() != rhs.targetPath.is_valid() ||
      (lhs.targetPath.is_valid() && !(lhs.targetPath == rhs.targetPath))) {
    note("target");
  }
  if (lhs.targetPathVector != rhs.targetPathVector) note("targets");

  if (opts.compareMetadata) {
    if (!CompareAttrMetas(lhs.metas(), rhs.metas(), opts, reasons)) {
      equal = false;
    }
  }
  return equal;
}

static bool ComparePropertyDetailed(const Property &lhs, const Property &rhs,
                                    const DiffOptions &opts,
                                    std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.has_custom() != rhs.has_custom()) note("custom");
  if (lhs.get_listedit_qual() != rhs.get_listedit_qual()) note("listOp");
  if (lhs.get_property_type() != rhs.get_property_type()) note("propertyType");

  if (lhs.is_attribute() != rhs.is_attribute() ||
      lhs.is_relationship() != rhs.is_relationship()) {
    note("kind");
    return false;
  }

  if (lhs.is_attribute()) {
    if (!CompareAttributeValues(lhs.get_attribute(), rhs.get_attribute(), opts,
                                reasons)) {
      equal = false;
    }
  } else if (lhs.is_relationship()) {
    if (!CompareRelationshipValues(lhs.get_relationship(),
                                   rhs.get_relationship(), opts, reasons)) {
      equal = false;
    }
  } else if (lhs.is_empty() != rhs.is_empty()) {
    note("empty");
  }

  return equal;
}

// Compare a PrimSpec's own fields (specifier / typeName / metadata). Property
// and child differences are reported separately. Fills `reasons` with what
// about this prim changed.
static bool ComparePrimSpecs(const PrimSpec &lhs, const PrimSpec &rhs,
                             const DiffOptions &opts,
                             std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.specifier() != rhs.specifier()) note("specifier");
  if (lhs.typeName() != rhs.typeName()) note("typeName");

  if (opts.compareMetadata) {
    if (!ComparePrimMeta(lhs.metas(), rhs.metas(), opts, reasons)) {
      equal = false;
    }
  }
  return equal;
}

static void ComputePropDiff(const std::string &path, const PrimSpec &lhs,
                            const PrimSpec &rhs, const DiffOptions &opts,
                            lightusd::HashMap<std::string, PropDiff> &propDiffs) {
  const auto &lhs_props = lhs.props();
  const auto &rhs_props = rhs.props();

  PropDiff diff;

  // Added properties (in rhs but not in lhs)
  for (const auto &prop : rhs_props) {
    if (lhs_props.find(prop.first) == lhs_props.end()) {
      diff.addedProps.push_back(prop.first);
    }
  }

  // Deleted properties (in lhs but not in rhs)
  for (const auto &prop : lhs_props) {
    if (rhs_props.find(prop.first) == rhs_props.end()) {
      diff.deletedProps.push_back(prop.first);
    }
  }

  // Modified properties (value / type / metadata / ...).
  for (const auto &prop : lhs_props) {
    auto it = rhs_props.find(prop.first);
    if (it != rhs_props.end()) {
      std::vector<std::string> reasons;
      if (!ComparePropertyDetailed(prop.second, it->second, opts, &reasons)) {
        diff.modifiedProps.push_back(prop.first);
        PropDiff::ModifiedProp mp;
        mp.name = prop.first;
        mp.lhs = FormatPropertyForDiff(prop.second);
        mp.rhs = FormatPropertyForDiff(it->second);
        mp.reasons = std::move(reasons);
        diff.modifiedPropDetails.push_back(std::move(mp));
      }
    }
  }

  if (!diff.addedProps.empty() || !diff.deletedProps.empty() ||
      !diff.modifiedProps.empty()) {
    propDiffs[path] = diff;
  }
}

static bool ComputeDiffImpl(
    uint32_t depth, const std::string &path, const PrimSpec &lhs,
    const PrimSpec &rhs, lightusd::HashMap<std::string, PrimSpecDiff> &psDiffs,
    lightusd::HashMap<std::string, PropDiff> &propDiffs, const DiffOptions &opts,
    std::vector<std::string> *selfReasons) {

  if (size_t(depth) > kMaxDefaultTraversalLimit) {
    return false;
  }

  // This prim's own structural / metadata differences.
  std::vector<std::string> reasons;
  ComparePrimSpecs(lhs, rhs, opts, &reasons);
  bool hasDiff = !reasons.empty();

  // Property differences (reported separately under this path).
  ComputePropDiff(path, lhs, rhs, opts, propDiffs);

  // Children.
  const auto &lhs_children = lhs.children();
  const auto &rhs_children = rhs.children();

  lightusd::HashMap<std::string, const PrimSpec *, FNV1StringHash> lhs_child_map;
  lightusd::HashMap<std::string, const PrimSpec *, FNV1StringHash> rhs_child_map;
  lhs_child_map.reserve(lhs_children.size());
  rhs_child_map.reserve(rhs_children.size());

  for (const auto &child : lhs_children) {
    lhs_child_map[child.name()] = &child;
  }
  for (const auto &child : rhs_children) {
    rhs_child_map[child.name()] = &child;
  }

  PrimSpecDiff psDiff;

  for (const auto &child : rhs_children) {
    if (lhs_child_map.find(child.name()) == lhs_child_map.end()) {
      psDiff.addedPS.push_back(child.name());
      hasDiff = true;
    }
  }

  for (const auto &child : lhs_children) {
    if (rhs_child_map.find(child.name()) == rhs_child_map.end()) {
      psDiff.deletedPS.push_back(child.name());
      hasDiff = true;
    }
  }

  for (const auto &child : lhs_children) {
    auto it = rhs_child_map.find(child.name());
    if (it != rhs_child_map.end()) {
      std::string child_path = JoinPrimPath(path, child.name());
      std::vector<std::string> childReasons;
      if (ComputeDiffImpl(depth + 1, child_path, child, *it->second, psDiffs,
                          propDiffs, opts, &childReasons)) {
        psDiff.modifiedPS.push_back(child.name());
        ModifiedPrimSpec mps;
        mps.name = child.name();
        mps.reasons = std::move(childReasons);
        psDiff.modifiedDetails.push_back(std::move(mps));
        hasDiff = true;
      }
    }
  }

  if (!psDiff.addedPS.empty() || !psDiff.deletedPS.empty() ||
      !psDiff.modifiedPS.empty()) {
    psDiffs[path] = psDiff;
  }

  if (selfReasons) *selfReasons = std::move(reasons);
  return hasDiff;
}

// Layer (stage) metadata comparison.
static std::string DblStr(double v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

static bool CompareLayerMetas(const LayerMetas &lhs, const LayerMetas &rhs,
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

  auto cmpDouble = [&](const char *field,
                       const TypedAttributeWithFallback<double> &a,
                       const TypedAttributeWithFallback<double> &b) {
    if (!ScalarAlmostEqual(a.get_value(), b.get_value(), opts)) {
      noteField(field, DblStr(a.get_value()), DblStr(b.get_value()));
    }
  };

  cmpDouble("metersPerUnit", lhs.metersPerUnit, rhs.metersPerUnit);
  cmpDouble("timeCodesPerSecond", lhs.timeCodesPerSecond,
            rhs.timeCodesPerSecond);
  cmpDouble("framesPerSecond", lhs.framesPerSecond, rhs.framesPerSecond);
  cmpDouble("startTimeCode", lhs.startTimeCode, rhs.startTimeCode);
  cmpDouble("endTimeCode", lhs.endTimeCode, rhs.endTimeCode);
  cmpDouble("kilogramsPerUnit", lhs.kilogramsPerUnit, rhs.kilogramsPerUnit);

  if (lhs.upAxis.get_value() != rhs.upAxis.get_value()) {
    noteField("upAxis", lightusd::to_string(lhs.upAxis.get_value()),
              lightusd::to_string(rhs.upAxis.get_value()));
  }
  if (lhs.defaultPrim.str() != rhs.defaultPrim.str()) {
    noteField("defaultPrim", lhs.defaultPrim.str(), rhs.defaultPrim.str());
  }
  if (lhs.comment.value != rhs.comment.value) {
    noteField("comment", lhs.comment.value, rhs.comment.value);
  }
  if (lhs.doc.value != rhs.doc.value) {
    noteField("documentation", lhs.doc.value, rhs.doc.value);
  }

  // subLayers (count + per-layer asset path / offset).
  bool subEqual = (lhs.subLayers.size() == rhs.subLayers.size());
  for (size_t i = 0; subEqual && i < lhs.subLayers.size(); ++i) {
    if (!AssetPathStringsEquivalentForDiff(
            lhs.subLayers[i].assetPath.GetAssetPath(),
            rhs.subLayers[i].assetPath.GetAssetPath()) ||
        lhs.subLayers[i].layerOffset._offset !=
            rhs.subLayers[i].layerOffset._offset ||
        lhs.subLayers[i].layerOffset._scale !=
            rhs.subLayers[i].layerOffset._scale) {
      subEqual = false;
    }
  }
  if (!subEqual) {
    noteField("subLayers", std::to_string(lhs.subLayers.size()),
              std::to_string(rhs.subLayers.size()));
  }

  // customLayerData dictionary.
  std::vector<std::string> keys;
  if (!CompareDictionaries(lhs.customLayerData, rhs.customLayerData, opts, 0,
                           &keys)) {
    for (const auto &k : keys) {
      out.changedFields.push_back("customLayerData:" + k);
    }
  }

  return out.changed();
}

static std::string EscapeJSON(const std::string &str) {
  std::string result;
  for (char c : str) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result += c; break;
    }
  }
  return result;
}

// ---- Instance-flatten canonicalization ---------------------------------
// Match flatten prototype prims by CONTENT so non-deterministic
// `/Flattened_Prototype_N` numbering does not produce spurious diffs.

template <class F>
static void ForEachIntraRef(const PrimSpec &ps, F &&f) {
  if (!ps.metas().references.has_value()) return;
  for (const auto &qr : ps.metas().references.value()) {
    for (const auto &ref : qr.second) {
      if (ref.asset_path.GetAssetPath().empty() && ref.prim_path.is_valid()) {
        f(ref.prim_path.full_path_name());
      }
    }
  }
}

static void CollectRefTargetsRec(const PrimSpec &ps, std::set<std::string> &out) {
  ForEachIntraRef(ps, [&](const std::string &t) { out.insert(t); });
  for (const auto &c : ps.children()) CollectRefTargetsRec(c, out);
}

// A top-level reference target "/Name" whose Name is a root prim.
static bool TopLevelProtoName(const std::string &target,
                              const std::set<std::string> &rootNames,
                              std::string *name) {
  if (target.size() < 2 || target[0] != '/') return false;
  if (target.find('/', 1) != std::string::npos) return false;
  std::string nm = target.substr(1);
  if (!rootNames.count(nm)) return false;
  *name = std::move(nm);
  return true;
}

static uint64_t ProtoHashByName(
    const std::string &name,
    const std::unordered_map<std::string, const PrimSpec *> &roots,
    const std::set<std::string> &rootNames,
    const std::set<std::string> &protoNames,
    std::unordered_map<std::string, uint64_t> &cache, int depth);

// Deterministic content serialization of a prim subtree. The root prototype's
// own NAME is excluded (it is the varying /Flattened_Prototype_N number); child
// names ARE included. Intra-layer references to a root-prototype are folded as
// the target's content hash so nested prototypes match by content too.
static void SerializePrim(
    const PrimSpec &ps, bool includeName, std::string &out,
    const std::unordered_map<std::string, const PrimSpec *> &roots,
    const std::set<std::string> &rootNames,
    const std::set<std::string> &protoNames,
    std::unordered_map<std::string, uint64_t> &cache, int depth) {
  if (depth > 512) return;
  out += "{";
  if (includeName) { out += "n:"; out += ps.name(); out += ";"; }
  out += "s:"; out += std::to_string(static_cast<int>(ps.specifier()));
  out += ";t:"; out += ps.typeName(); out += ";";
  ForEachIntraRef(ps, [&](const std::string &t) {
    std::string nm;
    out += "r:";
    if (TopLevelProtoName(t, rootNames, &nm) && protoNames.count(nm)) {
      out += "P";
      out += fmt::hex(
          ProtoHashByName(nm, roots, rootNames, protoNames, cache, depth + 1), 16);
    } else {
      out += t;
    }
    out += ";";
  });
  for (const auto &pr : ps.props()) {  // std::map -> name-sorted
    out += "p:"; out += pr.first; out += "=";
    out += FormatPropertyForDiff(pr.second); out += ";";
  }
  std::vector<const PrimSpec *> kids;
  kids.reserve(ps.children().size());
  for (const auto &c : ps.children()) kids.push_back(&c);
  std::sort(kids.begin(), kids.end(),
            [](const PrimSpec *a, const PrimSpec *b) { return a->name() < b->name(); });
  for (const auto *c : kids)
    SerializePrim(*c, true, out, roots, rootNames, protoNames, cache, depth + 1);
  out += "}";
}

static uint64_t ProtoHashByName(
    const std::string &name,
    const std::unordered_map<std::string, const PrimSpec *> &roots,
    const std::set<std::string> &rootNames,
    const std::set<std::string> &protoNames,
    std::unordered_map<std::string, uint64_t> &cache, int depth) {
  auto cit = cache.find(name);
  if (cit != cache.end()) return cit->second;
  cache[name] = 0;  // provisional (cycle guard; prototypes should not cycle)
  auto rit = roots.find(name);
  if (rit == roots.end()) return 0;
  std::string s;
  SerializePrim(*rit->second, /*includeName=*/false, s, roots, rootNames,
                protoNames, cache, depth);
  uint64_t h = SpookyHash::Hash64(s.data(), s.size(), 0x9E3779B97F4A7C15ull);
  if (h == 0) h = 1;  // reserve 0 as "in progress"
  cache[name] = h;
  return h;
}

static void RewriteRefsRec(
    PrimSpec &ps, const std::set<std::string> &rootNames,
    const std::unordered_map<std::string, std::string> &canon) {
  if (ps.metas().references.has_value()) {
    for (auto &qr : ps.metas().references.value()) {
      for (auto &ref : qr.second) {
        if (ref.asset_path.GetAssetPath().empty() && ref.prim_path.is_valid()) {
          std::string nm;
          if (TopLevelProtoName(ref.prim_path.full_path_name(), rootNames, &nm)) {
            auto c = canon.find(nm);
            if (c != canon.end()) ref.prim_path = Path("/" + c->second, "");
          }
        }
      }
    }
  }
  for (auto &c : ps.children()) RewriteRefsRec(c, rootNames, canon);
}

} // namespace detail

size_t CanonicalizeInstances(Layer &layer) {
  auto &roots = layer.primspecs();  // unordered_map<name, PrimSpec>
  std::set<std::string> rootNames;
  for (auto &kv : roots) rootNames.insert(kv.first);

  // Prototype roots = top-level prims that are intra-layer reference targets.
  std::set<std::string> targets;
  for (auto &kv : roots) detail::CollectRefTargetsRec(kv.second, targets);
  std::set<std::string> protoNames;
  for (const auto &t : targets) {
    std::string nm;
    if (detail::TopLevelProtoName(t, rootNames, &nm)) protoNames.insert(nm);
  }
  if (protoNames.empty()) return 0;

  std::unordered_map<std::string, const PrimSpec *> rootPtr;
  for (auto &kv : roots) rootPtr[kv.first] = &kv.second;

  // Content-hash each prototype -> canonical name (computed before any mutation).
  std::unordered_map<std::string, uint64_t> cache;
  std::unordered_map<std::string, std::string> canon;
  for (const auto &nm : protoNames) {
    uint64_t h = detail::ProtoHashByName(nm, rootPtr, rootNames, protoNames,
                                         cache, 0);
    canon[nm] = "__Proto_" + fmt::hex(h, 16);
  }

  // Retarget every reference, then rename the prototype roots (identical-content
  // prototypes collapse to one canonical key -- correct dedup).
  for (auto &kv : roots) detail::RewriteRefsRec(kv.second, rootNames, canon);
  std::vector<std::pair<std::string, PrimSpec>> moved;
  moved.reserve(protoNames.size());
  for (const auto &nm : protoNames) {
    auto it = roots.find(nm);
    if (it == roots.end()) continue;
    PrimSpec ps = std::move(it->second);
    ps.name() = canon[nm];
    roots.erase(it);
    moved.emplace_back(canon[nm], std::move(ps));
  }
  for (auto &m : moved) roots[m.first] = std::move(m.second);
  return protoNames.size();
}

namespace detail {
static void StripLargeArraysRec(PrimSpec &ps, size_t threshold, size_t &count) {
  for (auto &pr : ps.props()) {
    if (!pr.second.is_attribute()) continue;
    Attribute &attr = pr.second.attribute();
    if (attr.is_blocked() || !attr.has_value()) continue;
    primvar::PrimVar &var = attr.get_var();
    const value::Value &val = var.value_raw();
    if (!val.is_array() || val.array_size() < threshold) continue;
    // Pin the declared type so it survives the value replacement, then fold the
    // FULL value into a 64-bit fingerprint and free the array payload. The diff
    // compares fingerprints (exact) instead of materialized elements.
    attr.set_type_name(attr.type_name());
    std::string s = value::pprint_value(val);
    uint64_t fp = SpookyHash::Hash64(s.data(), s.size(), 0xD1B54A32D192ED03ull);
    var.set_value(fp);
    ++count;
  }
  for (auto &c : ps.children()) StripLargeArraysRec(c, threshold, count);
}
} // namespace detail

size_t StripLargeArrays(Layer &layer, size_t threshold) {
  size_t count = 0;
  for (auto &kv : layer.primspecs())
    detail::StripLargeArraysRec(kv.second, threshold, count);
  return count;
}

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
    out += s.substr(start, end - start);
    if (end < s.size()) out += "\xE2\x80\xA6";
    return out;
  };
  return {clip(lhs), clip(rhs)};
}

void Diff(const Layer &lhs, const Layer &rhs,
  lightusd::HashMap<std::string, PrimSpecDiff> &psDiffs,
  lightusd::HashMap<std::string, PropDiff> &propDiffs,
  const DiffOptions &opts,
  LayerMetaDiff *layerMetaDiff) {

  const auto &lhs_primspecs = lhs.primspecs();
  const auto &rhs_primspecs = rhs.primspecs();

  PrimSpecDiff rootDiff;

  // Find added root primspecs (in rhs but not in lhs)
  for (const auto &rhs_prim : rhs_primspecs) {
    if (lhs_primspecs.find(rhs_prim.first) == lhs_primspecs.end()) {
      rootDiff.addedPS.push_back(rhs_prim.first);
    }
  }

  // Find deleted root primspecs (in lhs but not in rhs)
  for (const auto &lhs_prim : lhs_primspecs) {
    if (rhs_primspecs.find(lhs_prim.first) == rhs_primspecs.end()) {
      rootDiff.deletedPS.push_back(lhs_prim.first);
    }
  }

  // Compare common root primspecs
  for (const auto &lhs_prim : lhs_primspecs) {
    auto it = rhs_primspecs.find(lhs_prim.first);
    if (it != rhs_primspecs.end()) {
      std::string prim_path = "/" + lhs_prim.first;
      std::vector<std::string> selfReasons;
      if (detail::ComputeDiffImpl(0, prim_path, lhs_prim.second, it->second,
                                  psDiffs, propDiffs, opts, &selfReasons)) {
        rootDiff.modifiedPS.push_back(lhs_prim.first);
        ModifiedPrimSpec mps;
        mps.name = lhs_prim.first;
        mps.reasons = std::move(selfReasons);
        rootDiff.modifiedDetails.push_back(std::move(mps));
      }
    }
  }

  // Stage / layer metadata.
  LayerMetaDiff localLayerDiff;
  if (opts.compareMetadata) {
    detail::CompareLayerMetas(lhs.metas(), rhs.metas(), opts, localLayerDiff);
  }
  if (layerMetaDiff) {
    *layerMetaDiff = std::move(localLayerDiff);
  }

  // Add root level differences if any
  if (!rootDiff.addedPS.empty() || !rootDiff.deletedPS.empty() || !rootDiff.modifiedPS.empty()) {
    psDiffs["/"] = rootDiff;
  }
}

std::string DiffToText(const Layer &lhs, const Layer &rhs,
                       const std::string &lhs_name,
                       const std::string &rhs_name,
                       const DiffOptions &opts) {
  lightusd::HashMap<std::string, PrimSpecDiff> psDiffs;
  lightusd::HashMap<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto joinReasons = [](const std::vector<std::string> &reasons) -> std::string {
    std::stringstream r;
    for (size_t i = 0; i < reasons.size(); ++i) {
      if (i) r << ", ";
      r << reasons[i];
    }
    return r.str();
  };

  bool any = false;
  std::stringstream ss;
  ss << "--- " << lhs_name << std::endl;
  ss << "+++ " << rhs_name << std::endl;

  // Stage (layer) metadata differences.
  if (layerMetaDiff.changed()) {
    any = true;
    ss << "~ <stage metadata> (Stage metadata modified: "
       << joinReasons(layerMetaDiff.changedFields) << ")" << std::endl;
    for (const auto &d : layerMetaDiff.details) {
      ss << "  - " << d.name << ": " << d.lhs << std::endl;
      ss << "  + " << d.name << ": " << d.rhs << std::endl;
    }
  }

  // Sort paths for consistent output
  std::unordered_set<std::string, detail::FNV1StringHash> uniquePaths;
  uniquePaths.reserve(psDiffs.size() + propDiffs.size());
  for (const auto &entry : psDiffs) {
    uniquePaths.insert(entry.first);
  }
  for (const auto &entry : propDiffs) {
    uniquePaths.insert(entry.first);
  }
  std::vector<std::string> sortedPaths;
  sortedPaths.reserve(uniquePaths.size());
  sortedPaths.insert(sortedPaths.end(), uniquePaths.begin(), uniquePaths.end());
  std::sort(sortedPaths.begin(), sortedPaths.end());

  for (const std::string &path : sortedPaths) {
    // PrimSpec changes
    auto psIt = psDiffs.find(path);
    if (psIt != psDiffs.end()) {
      const PrimSpecDiff &psDiff = psIt->second;

      for (const std::string &name : psDiff.deletedPS) {
        any = true;
        ss << "- " << detail::JoinPrimPath(path, name) << " (PrimSpec deleted)" << std::endl;
      }

      for (const std::string &name : psDiff.addedPS) {
        any = true;
        ss << "+ " << detail::JoinPrimPath(path, name) << " (PrimSpec added)" << std::endl;
      }

      // Prefer modifiedDetails (carry reasons); fall back to modifiedPS.
      if (!psDiff.modifiedDetails.empty()) {
        for (const ModifiedPrimSpec &m : psDiff.modifiedDetails) {
          any = true;
          ss << "~ " << detail::JoinPrimPath(path, m.name) << " (PrimSpec modified";
          if (!m.reasons.empty()) ss << ": " << joinReasons(m.reasons);
          ss << ")" << std::endl;
        }
      } else {
        for (const std::string &name : psDiff.modifiedPS) {
          any = true;
          ss << "~ " << detail::JoinPrimPath(path, name) << " (PrimSpec modified)" << std::endl;
        }
      }
    }

    // Property changes
    auto propIt = propDiffs.find(path);
    if (propIt != propDiffs.end()) {
      const PropDiff &propDiff = propIt->second;

      for (const std::string &name : propDiff.deletedProps) {
        any = true;
        ss << "- " << path << "." << name << " (Property deleted)" << std::endl;
      }

      for (const std::string &name : propDiff.addedProps) {
        any = true;
        ss << "+ " << path << "." << name << " (Property added)" << std::endl;
      }

      for (const PropDiff::ModifiedProp &modified :
           propDiff.modifiedPropDetails) {
        any = true;
        ss << "~ " << path << "." << modified.name << " (Property modified";
        if (!modified.reasons.empty()) ss << ": " << joinReasons(modified.reasons);
        ss << ")" << std::endl;
        // Center long values on the first difference so a shared prefix (e.g.
        // asset paths) does not hide what actually changed.
        auto pr = CenterValuePairForDiff(modified.lhs, modified.rhs);
        ss << "  - " << pr.first << std::endl;
        ss << "  + " << pr.second << std::endl;
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
                       const std::string &rhs_name,
                       const DiffOptions &opts) {
  lightusd::HashMap<std::string, PrimSpecDiff> psDiffs;
  lightusd::HashMap<std::string, PropDiff> propDiffs;
  LayerMetaDiff layerMetaDiff;

  Diff(lhs, rhs, psDiffs, propDiffs, opts, &layerMetaDiff);

  auto emitStrArray = [](std::stringstream &o,
                         const std::vector<std::string> &v) {
    o << "[";
    for (size_t i = 0; i < v.size(); ++i) {
      if (i > 0) o << ", ";
      o << "\"" << detail::EscapeJSON(v[i]) << "\"";
    }
    o << "]";
  };

  std::stringstream ss;
  ss << "{\n";
  ss << "  \"comparison\": {\n";
  ss << "    \"left\": \"" << detail::EscapeJSON(lhs_name) << "\",\n";
  ss << "    \"right\": \"" << detail::EscapeJSON(rhs_name) << "\"\n";
  ss << "  },\n";

  // PrimSpec differences
  ss << "  \"primspec_diffs\": {\n";
  bool firstPrimDiff = true;
  for (const auto &entry : psDiffs) {
    if (!firstPrimDiff) ss << ",\n";
    firstPrimDiff = false;

    const std::string &path = entry.first;
    const PrimSpecDiff &diff = entry.second;

    ss << "    \"" << detail::EscapeJSON(path) << "\": {\n";

    ss << "      \"added\": ";
    emitStrArray(ss, diff.addedPS);
    ss << ",\n";

    ss << "      \"deleted\": ";
    emitStrArray(ss, diff.deletedPS);
    ss << ",\n";

    ss << "      \"modified\": ";
    emitStrArray(ss, diff.modifiedPS);
    ss << ",\n";

    // Reasons per modified PrimSpec.
    ss << "      \"modified_details\": [";
    for (size_t i = 0; i < diff.modifiedDetails.size(); ++i) {
      if (i > 0) ss << ", ";
      ss << "{\"name\":\"" << detail::EscapeJSON(diff.modifiedDetails[i].name)
         << "\", \"reasons\":";
      emitStrArray(ss, diff.modifiedDetails[i].reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Property differences
  ss << "  \"property_diffs\": {\n";
  bool firstPropDiff = true;
  for (const auto &entry : propDiffs) {
    if (!firstPropDiff) ss << ",\n";
    firstPropDiff = false;

    const std::string &path = entry.first;
    const PropDiff &diff = entry.second;

    ss << "    \"" << detail::EscapeJSON(path) << "\": {\n";

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
      ss << "{\"name\":\"" << detail::EscapeJSON(modified.name)
         << "\", \"left\":\"" << detail::EscapeJSON(modified.lhs)
         << "\", \"right\":\"" << detail::EscapeJSON(modified.rhs)
         << "\", \"reasons\":";
      emitStrArray(ss, modified.reasons);
      ss << "}";
    }
    ss << "]\n";

    ss << "    }";
  }
  ss << "\n  },\n";

  // Stage / layer metadata differences
  ss << "  \"layer_meta_diff\": {\n";
  ss << "    \"changed\": ";
  emitStrArray(ss, layerMetaDiff.changedFields);
  ss << ",\n";
  ss << "    \"details\": [";
  for (size_t i = 0; i < layerMetaDiff.details.size(); ++i) {
    if (i > 0) ss << ", ";
    const PropDiff::ModifiedProp &d = layerMetaDiff.details[i];
    ss << "{\"name\":\"" << detail::EscapeJSON(d.name)
       << "\", \"left\":\"" << detail::EscapeJSON(d.lhs)
       << "\", \"right\":\"" << detail::EscapeJSON(d.rhs) << "\"}";
  }
  ss << "]\n";
  ss << "  }\n";

  ss << "}\n";

  return ss.str();
}

} // namespace tydra
} // namespace lightusd
