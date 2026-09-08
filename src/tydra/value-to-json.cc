#include "value-to-json.hh"

#include <sstream>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "common-macros.inc"
#include "value-to-json-internal.hh"

// MiniJSON is the default backend. Define LIGHTUSD_VALUE_JSON_USE_NLOHMANN=1
// only for builds that require the legacy template-heavy implementation.
#ifndef LIGHTUSD_VALUE_JSON_USE_NLOHMANN
#if defined(LIGHTUSD_ENABLE_NLOHMANN_JSON_COMPAT)
#define LIGHTUSD_VALUE_JSON_USE_NLOHMANN 1
#else
#define LIGHTUSD_VALUE_JSON_USE_NLOHMANN 0
#endif
#endif

namespace lightusd {
namespace tydra {

minijson::Value ValueToMiniJSON(const value::Value &, uint32_t);
minijson::Value ValueToPlainMiniJSON(const value::Value &);
nonstd::optional<value::Value> MiniJSONToValue(const minijson::Value &,
                                                std::string *, uint32_t);

namespace {
minijson::Value NlohmannToMiniJSON(const nlohmann::json &);
nlohmann::json MiniJSONToNlohmann(const minijson::Value &);
}  // namespace

namespace {


// ---------------------------------------------------------------------------
// Helper: append elements of any array-like compound type to a JSON array
// ---------------------------------------------------------------------------
#if LIGHTUSD_VALUE_JSON_USE_NLOHMANN
template <typename T, size_t N>
void AppendCompound(nlohmann::json &arr, const std::array<T, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(static_cast<double>(v[i]));
  }
}

template <size_t N>
void AppendCompound(nlohmann::json &arr, const std::array<int32_t, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(v[i]);
  }
}

template <size_t N>
[[maybe_unused]]
void AppendCompound(nlohmann::json &arr, const std::array<uint32_t, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(static_cast<int64_t>(v[i]));
  }
}

template <size_t N>
[[maybe_unused]]
void AppendCompound(nlohmann::json &arr, const std::array<int64_t, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(v[i]);
  }
}

template <size_t N>
[[maybe_unused]]
void AppendCompound(nlohmann::json &arr, const std::array<uint64_t, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(static_cast<double>(v[i]));
  }
}

template <size_t N>
void AppendCompound(nlohmann::json &arr, const std::array<value::half, N> &v) {
  for (size_t i = 0; i < N; i++) {
    arr.push_back(static_cast<double>(value::half_to_float(v[i])));
  }
}

// ---------------------------------------------------------------------------
// Helper: compound value to JSON array (float3, int2, etc.)
// ---------------------------------------------------------------------------
template <typename T>
nlohmann::json CompoundToJSON(const T &v) {
  nlohmann::json arr = nlohmann::json::array();
  AppendCompound(arr, v);
  return arr;
}

// ---------------------------------------------------------------------------
// Helper: element values from array to JSON
// ---------------------------------------------------------------------------
template <typename T>
nlohmann::json ArrayValuesToJSON(const std::vector<T> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(CompoundToJSON(elem));
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<float>(const std::vector<float> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(static_cast<double>(elem));
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<double>(const std::vector<double> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<int32_t>(const std::vector<int32_t> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<value::half>(const std::vector<value::half> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(static_cast<double>(value::half_to_float(elem)));
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<std::string>(const std::vector<std::string> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<value::token>(const std::vector<value::token> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem.str());
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<uint8_t>(const std::vector<uint8_t> &vec) {
  // uchar[]: emit as integers (0..255), not characters.
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(static_cast<unsigned int>(elem));
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<bool>(const std::vector<bool> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (bool elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<uint32_t>(const std::vector<uint32_t> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<int64_t>(const std::vector<int64_t> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

template <>
nlohmann::json ArrayValuesToJSON<uint64_t>(const std::vector<uint64_t> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &elem : vec) {
    arr.push_back(elem);
  }
  return arr;
}

// ---------------------------------------------------------------------------
// Matrix helpers
// ---------------------------------------------------------------------------
template <typename T, int R, int C>
nlohmann::json MatrixToJSON(const T &m) {
  nlohmann::json arr = nlohmann::json::array();
  for (int i = 0; i < R; i++) {
    nlohmann::json row = nlohmann::json::array();
    for (int j = 0; j < C; j++) {
      row.push_back(static_cast<double>(m.m[i][j]));
    }
    arr.push_back(row);
  }
  return arr;
}

template <typename T, int R, int C>
nlohmann::json MatrixArrayToJSON(const std::vector<T> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &m : vec) {
    arr.push_back(MatrixToJSON<T, R, C>(m));
  }
  return arr;
}

// ---------------------------------------------------------------------------
// Quaternion helpers
// ---------------------------------------------------------------------------
template <typename T>
nlohmann::json QuatToJSON(const T &q) {
  nlohmann::json arr = nlohmann::json::array();
  arr.push_back(static_cast<double>(q[0]));
  arr.push_back(static_cast<double>(q[1]));
  arr.push_back(static_cast<double>(q[2]));
  arr.push_back(static_cast<double>(q[3]));
  return arr;
}

// quath uses half type which needs half_to_float conversion
template <>
nlohmann::json QuatToJSON<value::quath>(const value::quath &q) {
  nlohmann::json arr = nlohmann::json::array();
  arr.push_back(static_cast<double>(value::half_to_float(q[0])));
  arr.push_back(static_cast<double>(value::half_to_float(q[1])));
  arr.push_back(static_cast<double>(value::half_to_float(q[2])));
  arr.push_back(static_cast<double>(value::half_to_float(q[3])));
  return arr;
}

template <typename T>
nlohmann::json QuatArrayToJSON(const std::vector<T> &vec) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &q : vec) {
    arr.push_back(QuatToJSON<T>(q));
  }
  return arr;
}

// ---------------------------------------------------------------------------
// Dispatch by type_id to extract scalar / compound value
// Returns null JSON if type not handled
// ---------------------------------------------------------------------------
template <typename ValueType>
bool TryGetValueAs(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<ValueType>(false);
  if (!v) return false;
  out = static_cast<double>(v.value());
  return true;
}

// Specializations for non-arithmetic types
template <> bool TryGetValueAs<bool>(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<bool>(false);
  if (!v) return false;
  out = v.value();
  return true;
}

template <> bool TryGetValueAs<std::string>(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<std::string>(false);
  if (!v) return false;
  out = v.value();
  return true;
}

template <> bool TryGetValueAs<value::token>(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<value::token>(false);
  if (!v) return false;
  out = v.value().str();
  return true;
}

// ---------------------------------------------------------------------------
// Full typed dispatch for compound types -> returning value as typed JSON
// ---------------------------------------------------------------------------
// Templates for compound types
template <typename T>
bool TryGetCompound(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<T>(false);
  if (!v) return false;
  out = CompoundToJSON(v.value());
  return true;
}

template <typename T>
[[maybe_unused]]
bool TryGetCompoundAsElements(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<T>(false);
  if (!v) return false;
  // For compound types like float3, return as flat array of arrays
  // but since compound types ARE the value, just return the compound
  out = CompoundToJSON(v.value());
  return true;
}

// ---------------------------------------------------------------------------
// Convert a typed value to a JSON array of elements
// ---------------------------------------------------------------------------
template <typename ElemType>
bool TryGetArrayValue(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<std::vector<ElemType>>(false);
  if (!v) return false;
  out = ArrayValuesToJSON<ElemType>(v.value());
  return true;
}

// Typed arrays (TypedArray<T>)
template <typename ElemType>
[[maybe_unused]]
bool TryGetTypedArrayValue(const value::Value &val, nlohmann::json &out) {
  auto v = val.get_value<TypedArray<ElemType>>(false);
  if (!v) return false;
  auto view = v.value();
  std::vector<ElemType> tmp;
  tmp.reserve(view.size());
  for (size_t i = 0; i < view.size(); i++) {
    tmp.push_back(view[i]);
  }
  out = ArrayValuesToJSON<ElemType>(tmp);
  return true;
}

} // namespace

// ===========================================================================
// Public: ValueToJSON
// ===========================================================================
nlohmann::json ValueToJSON(const value::Value &val, uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    return {{"type", "error"}, {"error", "max recursion depth exceeded"}};
  }

  if (val.is_empty()) {
    return {{"type", "null"}};
  }

  if (val.is_none()) {
    return {{"type", "None"}};
  }

  uint32_t tid = val.type_id();
  std::string type_name = val.type_name();

  // Handle arrays first
  if (val.is_array()) {
    uint32_t elem_tid = tid & ~value::TYPE_ID_1D_ARRAY_BIT;

    // Dispatch on element type
    //
    // NOTE: this ~27-branch else-if chain on elem_tid was converted to
    // standalone ifs -- same MSVC C1061 ("blocks nested too deeply") risk
    // class already fixed for the same reason elsewhere in this codebase.
    // `matched` is only needed to reproduce the final "nothing matched ->
    // goto fallback" case correctly (each branch's own `goto fallback;` on
    // failure is an unconditional jump, unaffected by if/else-if vs
    // standalone-if; elem_tid equality checks are mutually exclusive by
    // construction, so no branch-to-branch overlap risk here).
    bool matched = false;
    nlohmann::json values;

    if (elem_tid == value::TypeTraits<float>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<float>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<double>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<double>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<int32_t>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<int32_t>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<uint8_t>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<uint8_t>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<bool>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<bool>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<uint32_t>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<uint32_t>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<int64_t>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<int64_t>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<uint64_t>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<uint64_t>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::half>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::half>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<std::string>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<std::string>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::token>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::token>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::float2>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::float2>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::float3>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::float3>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::float4>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::float4>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::double2>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::double2>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::double3>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::double3>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::double4>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::double4>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::int2>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::int2>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::int3>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::int3>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::int4>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::int4>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::half2>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::half2>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::half3>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::half3>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::half4>::type_id()) {
      matched = true;
      if (!TryGetArrayValue<value::half4>(val, values)) goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::matrix2d>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::matrix2d>>(false);
      if (v) values = MatrixArrayToJSON<value::matrix2d, 2, 2>(v.value());
      else goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::matrix3d>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::matrix3d>>(false);
      if (v) values = MatrixArrayToJSON<value::matrix3d, 3, 3>(v.value());
      else goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::matrix4d>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::matrix4d>>(false);
      if (v) values = MatrixArrayToJSON<value::matrix4d, 4, 4>(v.value());
      else goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::quatf>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::quatf>>(false);
      if (v) values = QuatArrayToJSON<value::quatf>(v.value());
      else goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::quatd>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::quatd>>(false);
      if (v) values = QuatArrayToJSON<value::quatd>(v.value());
      else goto fallback;
    }
    if (elem_tid == value::TypeTraits<value::quath>::type_id()) {
      matched = true;
      auto v = val.get_value<std::vector<value::quath>>(false);
      if (v) values = QuatArrayToJSON<value::quath>(v.value());
      else goto fallback;
    }
    if (!matched) {
      goto fallback;
    }

    return {{"type", type_name + "[]"}, {"value", values}};
  }

  // Non-array: dispatch on type_id
  //
  // NOTE: this ~26-branch else-if chain on tid (plus a 7-branch inner utype
  // fallback) was converted to standalone ifs -- same MSVC C1061 risk class
  // fixed elsewhere in this codebase. Unlike the array dispatch above,
  // `matched_known_tid` is genuinely required (not just for a trailing
  // "goto fallback"): a tid branch can match but its inner get/TryGet call
  // can fail without returning, and the original chain's structure meant
  // that once a tid branch was selected, the utype (role-type) fallback
  // below was skipped entirely regardless -- both cases (and the "no branch
  // matched" case) fall through the same way to the fallback: label further
  // down, so nothing extra is needed there.
  {
    bool matched_known_tid = false;

    // Handle scalars
    if (tid == value::TypeTraits<float>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<float>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<double>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<double>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<int32_t>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<int32_t>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<uint8_t>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<uint8_t>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<uint32_t>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<uint32_t>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<int64_t>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<int64_t>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<uint64_t>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<uint64_t>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<bool>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<bool>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<std::string>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<std::string>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::token>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetValueAs<value::token>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::half>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::half>(false);
      if (v) return {{"type", type_name}, {"value", value::half_to_float(v.value())}};
    }
    // Handle compounds
    if (tid == value::TypeTraits<value::float2>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::float2>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::float3>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::float3>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::float4>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::float4>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::double2>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::double2>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::double3>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::double3>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::double4>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::double4>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::int2>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::int2>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::int3>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::int3>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::int4>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::int4>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::half2>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::half2>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::half3>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::half3>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    if (tid == value::TypeTraits<value::half4>::type_id()) {
      matched_known_tid = true;
      nlohmann::json v;
      if (TryGetCompound<value::half4>(val, v))
        return {{"type", type_name}, {"value", v}};
    }
    // Handle matrices
    if (tid == value::TypeTraits<value::matrix2f>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix2f>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix2f, 2, 2>(v.value())}};
    }
    if (tid == value::TypeTraits<value::matrix3f>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix3f>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix3f, 3, 3>(v.value())}};
    }
    if (tid == value::TypeTraits<value::matrix4f>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix4f>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix4f, 4, 4>(v.value())}};
    }
    if (tid == value::TypeTraits<value::matrix2d>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix2d>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix2d, 2, 2>(v.value())}};
    }
    if (tid == value::TypeTraits<value::matrix3d>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix3d>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix3d, 3, 3>(v.value())}};
    }
    if (tid == value::TypeTraits<value::matrix4d>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::matrix4d>(false);
      if (v) return {{"type", type_name}, {"value", MatrixToJSON<value::matrix4d, 4, 4>(v.value())}};
    }
    // Handle quaternions
    if (tid == value::TypeTraits<value::quath>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::quath>(false);
      if (v) return {{"type", type_name}, {"value", QuatToJSON(v.value())}};
    }
    if (tid == value::TypeTraits<value::quatf>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::quatf>(false);
      if (v) return {{"type", type_name}, {"value", QuatToJSON(v.value())}};
    }
    if (tid == value::TypeTraits<value::quatd>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::quatd>(false);
      if (v) return {{"type", type_name}, {"value", QuatToJSON(v.value())}};
    }
    // Handle AssetPath
    if (tid == value::TypeTraits<value::AssetPath>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::AssetPath>(false);
      if (v) {
        return {{"type", type_name},
                {"value", {{"assetPath", v.value().GetAssetPath()},
                           {"resolvedPath", v.value().GetResolvedPath()}}}};
      }
    }
    // Handle dictionary
    if (tid == value::TypeTraits<value::dict>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::dict>(false);
      if (v) {
        nlohmann::json dict_obj = nlohmann::json::object();
        for (const auto &[k, v2] : v.value()) {
          dict_obj[k] = ValueToJSON(v2, depth + 1);
        }
        return {{"type", type_name}, {"value", dict_obj}};
      }
    }
    // Handle timecode
    if (tid == value::TypeTraits<value::timecode>::type_id()) {
      matched_known_tid = true;
      auto v = val.get_value<value::timecode>(false);
      if (v) return {{"type", type_name}, {"value", v.value().value}};
    }
    // Role types share underlying type_id, so we need to handle them by
    // underlying type. Use the same dispatch as the underlying type.
    // We catch remaining known types via type_name string match for role types.
    if (!matched_known_tid) {
      // Try role type dispatch by name
      std::string utype = val.underlying_type_name();
      if (utype == "float2") {
        nlohmann::json v;
        if (TryGetCompound<value::float2>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "float3") {
        nlohmann::json v;
        if (TryGetCompound<value::float3>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "float4") {
        nlohmann::json v;
        if (TryGetCompound<value::float4>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "double2") {
        nlohmann::json v;
        if (TryGetCompound<value::double2>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "double3") {
        nlohmann::json v;
        if (TryGetCompound<value::double3>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "double4") {
        nlohmann::json v;
        if (TryGetCompound<value::double4>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "half2") {
        nlohmann::json v;
        if (TryGetCompound<value::half2>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "half3") {
        nlohmann::json v;
        if (TryGetCompound<value::half3>(val, v))
          return {{"type", type_name}, {"value", v}};
      } else if (utype == "half4") {
        nlohmann::json v;
        if (TryGetCompound<value::half4>(val, v))
          return {{"type", type_name}, {"value", v}};
      }
    }
  }

fallback:
  // Fallback: serialise to string representation
  {
    auto s = val.get_value<std::string>(true);
    if (s) {
      return {{"type", type_name}, {"value", s.value()}};
    }
  }
  return {{"type", type_name},
          {"value", std::string("(unserializable type)")}};
}

// ===========================================================================
// Public: ValueToPlainJSON
// ===========================================================================
nlohmann::json ValueToPlainJSON(const value::Value &val) {
  nlohmann::json wrapped = ValueToJSON(val);
  if (wrapped.contains("value")) {
    return wrapped["value"];
  }
  return nullptr;
}

// ===========================================================================
// Public: JSONToValue
// ===========================================================================
nonstd::optional<value::Value> JSONToValue(const nlohmann::json &j,
                                            std::string *err, uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    if (err) *err = "JSONToValue: max recursion depth exceeded";
    return nonstd::nullopt;
  }

  if (!j.is_object() || !j.contains("type")) {
    if (err) *err = "JSON value must be object with 'type' field";
    return nonstd::nullopt;
  }

  std::string type_name = j["type"].get<std::string>();
  nlohmann::json val_json = j.value("value", nlohmann::json());

  // Handle None
  if (type_name == "None") {
    value::Value v;
    v = value::ValueBlock();
    return v;
  }

  // Handle null
  if (type_name == "null") {
    return value::Value();
  }

  // Helper lambdas for constructing values
  auto make_scalar = [&](auto typed_val) -> nonstd::optional<value::Value> {
    return value::Value(typed_val);
  };

  // Check if array type
  bool is_array = false;
  std::string base_type = type_name;
  if (type_name.size() >= 3 &&
      type_name.compare(type_name.size() - 2, 2, "[]") == 0) {
    is_array = true;
    base_type = type_name.substr(0, type_name.size() - 2);
  }

  if (is_array) {
    if (!val_json.is_array()) {
      if (err) *err = "Expected array value for type " + type_name;
      return nonstd::nullopt;
    }

    // Dispatch on base type
    if (base_type == "float" || base_type == "half") {
      std::vector<float> vec;
      for (const auto &elem : val_json) {
        if (elem.is_number()) { vec.push_back(elem.get<float>()); }
        else if (err) { *err = "Expected number in float array"; return nonstd::nullopt; }
      }
      return value::Value(vec);
    } else if (base_type == "double") {
      std::vector<double> vec;
      for (const auto &elem : val_json) {
        if (elem.is_number()) vec.push_back(elem.get<double>());
        else if (err) { *err = "Expected number in double array"; return nonstd::nullopt; }
      }
      return value::Value(vec);
    } else if (base_type == "int") {
      std::vector<int32_t> vec;
      for (const auto &elem : val_json) {
        if (elem.is_number()) vec.push_back(elem.get<int32_t>());
        else if (err) { *err = "Expected number in int array"; return nonstd::nullopt; }
      }
      return value::Value(vec);
    } else if (base_type == "string" || base_type == "token") {
      std::vector<std::string> vec;
      for (const auto &elem : val_json) {
        if (elem.is_string()) vec.push_back(elem.get<std::string>());
        else if (err) { *err = "Expected string in string array"; return nonstd::nullopt; }
      }
      return value::Value(vec);
    } else if (base_type == "float2" || base_type == "half2" ||
               base_type == "texCoord2f" || base_type == "texCoord2d" ||
               base_type == "texCoord2h") {
      std::vector<value::float2> vec;
      if (val_json.is_array()) {
        for (const auto &elem : val_json) {
          if (elem.is_array() && elem.size() >= 2) {
            vec.push_back({static_cast<float>(elem[0].get<double>()),
                           static_cast<float>(elem[1].get<double>())});
          }
        }
      }
      return value::Value(vec);
    } else if (base_type == "float3" || base_type == "color3f" ||
               base_type == "normal3f" || base_type == "vector3f" ||
               base_type == "point3f" || base_type == "half3" ||
               base_type == "color3h" || base_type == "color3d" ||
               base_type == "double3" || base_type == "normal3d" ||
               base_type == "vector3d" || base_type == "point3d") {
      // Route to float3 or double3 based on base type
      if (base_type.find("double") != std::string::npos ||
          base_type == "color3d" || base_type == "normal3d" ||
          base_type == "vector3d" || base_type == "point3d" ||
          base_type == "texCoord3d") {
        std::vector<value::double3> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 3) {
              vec.push_back({elem[0].get<double>(), elem[1].get<double>(),
                             elem[2].get<double>()});
            }
          }
        }
        return value::Value(vec);
      } else if (base_type.find("half") != std::string::npos) {
        // half arrays come as float in JSON
        std::vector<value::half3> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 3) {
              vec.push_back(
                  {value::float_to_half_full(static_cast<float>(elem[0].get<double>())),
                   value::float_to_half_full(static_cast<float>(elem[1].get<double>())),
                   value::float_to_half_full(static_cast<float>(elem[2].get<double>()))});
            }
          }
        }
        return value::Value(vec);
      } else {
        std::vector<value::float3> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 3) {
              vec.push_back({static_cast<float>(elem[0].get<double>()),
                             static_cast<float>(elem[1].get<double>()),
                             static_cast<float>(elem[2].get<double>())});
            }
          }
        }
        return value::Value(vec);
      }
    } else if (base_type == "float4" || base_type == "color4f" ||
               base_type == "color4h" || base_type == "color4d" ||
               base_type == "double4" || base_type == "half4" ||
               base_type == "quath" || base_type == "quatf" ||
               base_type == "quatd") {
      if (base_type.find("double") != std::string::npos ||
          base_type == "quatd") {
        std::vector<value::double4> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 4) {
              vec.push_back({elem[0].get<double>(), elem[1].get<double>(),
                             elem[2].get<double>(), elem[3].get<double>()});
            }
          }
        }
        return value::Value(vec);
      } else if (base_type.find("half") != std::string::npos ||
                 base_type == "quath") {
        std::vector<value::half4> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 4) {
              vec.push_back(
                  {value::float_to_half_full(static_cast<float>(elem[0].get<double>())),
                   value::float_to_half_full(static_cast<float>(elem[1].get<double>())),
                   value::float_to_half_full(static_cast<float>(elem[2].get<double>())),
                   value::float_to_half_full(static_cast<float>(elem[3].get<double>()))});
            }
          }
        }
        return value::Value(vec);
      } else {
        std::vector<value::float4> vec;
        if (val_json.is_array()) {
          for (const auto &elem : val_json) {
            if (elem.is_array() && elem.size() >= 4) {
              vec.push_back({static_cast<float>(elem[0].get<double>()),
                             static_cast<float>(elem[1].get<double>()),
                             static_cast<float>(elem[2].get<double>()),
                             static_cast<float>(elem[3].get<double>())});
            }
          }
        }
        return value::Value(vec);
      }
    } else if (base_type == "matrix2d") {
      std::vector<value::matrix2d> vec;
      if (val_json.is_array()) {
        for (const auto &mat : val_json) {
          if (mat.is_array() && mat.size() >= 2) {
            value::matrix2d m;
            for (size_t row = 0; row < 2; row++) {
              for (size_t col = 0; col < 2; col++) {
                m.m[row][col] =
                    mat[row].is_array() && mat[row].size() > col
                        ? mat[row][col].get<double>()
                        : 0.0;
              }
            }
            vec.push_back(m);
          }
        }
      }
      return value::Value(vec);
    } else if (base_type == "matrix3d") {
      std::vector<value::matrix3d> vec;
      if (val_json.is_array()) {
        for (const auto &mat : val_json) {
          if (mat.is_array() && mat.size() >= 3) {
            value::matrix3d m;
            for (size_t row = 0; row < 3; row++) {
              for (size_t col = 0; col < 3; col++) {
                m.m[row][col] =
                    mat[row].is_array() && mat[row].size() > col
                        ? mat[row][col].get<double>()
                        : 0.0;
              }
            }
            vec.push_back(m);
          }
        }
      }
      return value::Value(vec);
    } else if (base_type == "matrix4d" || base_type == "frame4d" ||
               base_type == "matrix4f" || base_type == "matrix3f" ||
               base_type == "matrix2f") {
      if (base_type.find("f") != std::string::npos && base_type != "frame4d") {
        // float matrices
        if (base_type == "matrix2f") {
          std::vector<value::matrix2f> vec;
          if (val_json.is_array()) {
            for (const auto &mat : val_json) {
              if (mat.is_array() && mat.size() >= 2) {
                value::matrix2f m;
                for (size_t row = 0; row < 2; row++) {
                  for (size_t col = 0; col < 2; col++) {
                    m.m[row][col] =
                        mat[row].is_array() && mat[row].size() > col
                            ? static_cast<float>(mat[row][col].get<double>())
                            : 0.0f;
                  }
                }
                vec.push_back(m);
              }
            }
          }
          return value::Value(vec);
        } else if (base_type == "matrix3f") {
          std::vector<value::matrix3f> vec;
          if (val_json.is_array()) {
            for (const auto &mat : val_json) {
              if (mat.is_array() && mat.size() >= 3) {
                value::matrix3f m;
                for (size_t row = 0; row < 3; row++) {
                  for (size_t col = 0; col < 3; col++) {
                    m.m[row][col] =
                        mat[row].is_array() && mat[row].size() > col
                            ? static_cast<float>(mat[row][col].get<double>())
                            : 0.0f;
                  }
                }
                vec.push_back(m);
              }
            }
          }
          return value::Value(vec);
        } else {
          std::vector<value::matrix4f> vec;
          if (val_json.is_array()) {
            for (const auto &mat : val_json) {
              if (mat.is_array() && mat.size() >= 4) {
                value::matrix4f m;
                for (size_t row = 0; row < 4; row++) {
                  for (size_t col = 0; col < 4; col++) {
                    m.m[row][col] =
                        mat[row].is_array() && mat[row].size() > col
                            ? static_cast<float>(mat[row][col].get<double>())
                            : 0.0f;
                  }
                }
                vec.push_back(m);
              }
            }
          }
          return value::Value(vec);
        }
      } else {
        std::vector<value::matrix4d> vec;
        if (val_json.is_array()) {
          for (const auto &mat : val_json) {
            if (mat.is_array() && mat.size() >= 4) {
              value::matrix4d m;
              for (size_t row = 0; row < 4; row++) {
                for (size_t col = 0; col < 4; col++) {
                  m.m[row][col] =
                      mat[row].is_array() && mat[row].size() > col
                          ? mat[row][col].get<double>()
                          : 0.0;
                }
              }
              vec.push_back(m);
            }
          }
        }
        return value::Value(vec);
      }
    }

    if (err) *err = "Unsupported array type: " + type_name;
    return nonstd::nullopt;
  }

  // Non-array: construct scalar / compound value
  if (base_type == "float" || base_type == "half") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for " + base_type;
      return nonstd::nullopt;
    }
    return make_scalar(static_cast<float>(val_json.get<double>()));
  } else if (base_type == "double") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for double";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<double>());
  } else if (base_type == "int") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for int";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<int32_t>());
  } else if (base_type == "uint") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for uint";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<uint32_t>());
  } else if (base_type == "int64") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for int64";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<int64_t>());
  } else if (base_type == "uint64") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for uint64";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<uint64_t>());
  } else if (base_type == "bool") {
    if (!val_json.is_boolean()) {
      if (err) *err = "Expected boolean for bool";
      return nonstd::nullopt;
    }
    return make_scalar(val_json.get<bool>());
  } else if (base_type == "string" || base_type == "token") {
    if (!val_json.is_string()) {
      if (err) *err = "Expected string for " + base_type;
      return nonstd::nullopt;
    }
    if (base_type == "token") {
      return make_scalar(value::token(val_json.get<std::string>()));
    }
    return make_scalar(val_json.get<std::string>());
  } else if (base_type == "timecode") {
    if (!val_json.is_number()) {
      if (err) *err = "Expected number for timecode";
      return nonstd::nullopt;
    }
    return make_scalar(value::timecode{val_json.get<double>()});
  } else if (base_type == "asset") {
    if (!val_json.is_object()) {
      if (err) *err = "Expected object for asset";
      return nonstd::nullopt;
    }
    return make_scalar(
        value::AssetPath(val_json.value("assetPath", std::string())));
  }

  // Compound types
  if (!val_json.is_array()) {
    if (err) *err = "Expected array for " + base_type;
    return nonstd::nullopt;
  }

  // float2/3/4
  if (base_type == "float2" || base_type == "half2") {
    if (val_json.size() < 2) {
      if (err) *err = "Expected 2 elements for float2";
      return nonstd::nullopt;
    }
    return make_scalar(value::float2{
        static_cast<float>(val_json[0].get<double>()),
        static_cast<float>(val_json[1].get<double>())});
  } else if (base_type == "float3" || base_type == "color3f" ||
             base_type == "normal3f" || base_type == "vector3f" ||
             base_type == "point3f" || base_type == "texCoord3f") {
    if (val_json.size() < 3) {
      if (err) *err = "Expected 3 elements for float3";
      return nonstd::nullopt;
    }
    return make_scalar(value::float3{
        static_cast<float>(val_json[0].get<double>()),
        static_cast<float>(val_json[1].get<double>()),
        static_cast<float>(val_json[2].get<double>())});
  } else if (base_type == "float4" || base_type == "color4f") {
    if (val_json.size() < 4) {
      if (err) *err = "Expected 4 elements for float4";
      return nonstd::nullopt;
    }
    return make_scalar(value::float4{
        static_cast<float>(val_json[0].get<double>()),
        static_cast<float>(val_json[1].get<double>()),
        static_cast<float>(val_json[2].get<double>()),
        static_cast<float>(val_json[3].get<double>())});
  } else if (base_type == "double2") {
    if (val_json.size() < 2) {
      if (err) *err = "Expected 2 elements for double2";
      return nonstd::nullopt;
    }
    return make_scalar(
        value::double2{val_json[0].get<double>(), val_json[1].get<double>()});
  } else if (base_type == "double3" || base_type == "color3d" ||
             base_type == "normal3d" || base_type == "vector3d" ||
             base_type == "point3d" || base_type == "texCoord3d") {
    if (val_json.size() < 3) {
      if (err) *err = "Expected 3 elements for double3";
      return nonstd::nullopt;
    }
    return make_scalar(value::double3{val_json[0].get<double>(),
                                      val_json[1].get<double>(),
                                      val_json[2].get<double>()});
  } else if (base_type == "double4" || base_type == "color4d") {
    if (val_json.size() < 4) {
      if (err) *err = "Expected 4 elements for double4";
      return nonstd::nullopt;
    }
    return make_scalar(value::double4{val_json[0].get<double>(),
                                      val_json[1].get<double>(),
                                      val_json[2].get<double>(),
                                      val_json[3].get<double>()});
  } else if (base_type == "half3" || base_type == "color3h" ||
             base_type == "normal3h" || base_type == "vector3h" ||
             base_type == "point3h" || base_type == "texCoord3h") {
    if (val_json.size() < 3) return nonstd::nullopt;
    return make_scalar(value::half3{
        value::float_to_half_full(static_cast<float>(val_json[0].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[1].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[2].get<double>()))});
  } else if (base_type == "half4" || base_type == "color4h") {
    if (val_json.size() < 4) return nonstd::nullopt;
    return make_scalar(value::half4{
        value::float_to_half_full(static_cast<float>(val_json[0].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[1].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[2].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[3].get<double>()))});
  } else if (base_type == "int2") {
    if (val_json.size() < 2) return nonstd::nullopt;
    return make_scalar(value::int2{val_json[0].get<int32_t>(),
                                   val_json[1].get<int32_t>()});
  } else if (base_type == "int3") {
    if (val_json.size() < 3) return nonstd::nullopt;
    return make_scalar(value::int3{val_json[0].get<int32_t>(),
                                   val_json[1].get<int32_t>(),
                                   val_json[2].get<int32_t>()});
  } else if (base_type == "int4") {
    if (val_json.size() < 4) return nonstd::nullopt;
    return make_scalar(value::int4{val_json[0].get<int32_t>(),
                                   val_json[1].get<int32_t>(),
                                   val_json[2].get<int32_t>(),
                                   val_json[3].get<int32_t>()});
  } else if (base_type == "quath") {
    if (val_json.size() < 4) return nonstd::nullopt;
    return make_scalar(value::quath{{
        value::float_to_half_full(static_cast<float>(val_json[0].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[1].get<double>())),
        value::float_to_half_full(static_cast<float>(val_json[2].get<double>()))},
        value::float_to_half_full(static_cast<float>(val_json[3].get<double>()))});
  } else if (base_type == "quatf") {
    if (val_json.size() < 4) return nonstd::nullopt;
    return make_scalar(value::quatf{{
        static_cast<float>(val_json[0].get<double>()),
        static_cast<float>(val_json[1].get<double>()),
        static_cast<float>(val_json[2].get<double>())},
        static_cast<float>(val_json[3].get<double>())});
  } else if (base_type == "quatd") {
    if (val_json.size() < 4) return nonstd::nullopt;
    return make_scalar(value::quatd{{val_json[0].get<double>(),
                                    val_json[1].get<double>(),
                                    val_json[2].get<double>()},
                                    val_json[3].get<double>()});
  }

  // Matrices (2D array of arrays)
  if (base_type == "matrix2f" || base_type == "matrix2d") {
    if (val_json.size() < 2) return nonstd::nullopt;
    if (base_type == "matrix2f") {
      value::matrix2f m;
      for (size_t row = 0; row < 2; row++)
        for (size_t col = 0; col < 2; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? static_cast<float>(val_json[row][col].get<double>())
                          : 0.0f;
      return make_scalar(m);
    } else {
      value::matrix2d m;
      for (size_t row = 0; row < 2; row++)
        for (size_t col = 0; col < 2; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? val_json[row][col].get<double>()
                          : 0.0;
      return make_scalar(m);
    }
  } else if (base_type == "matrix3f" || base_type == "matrix3d") {
    if (val_json.size() < 3) return nonstd::nullopt;
    if (base_type == "matrix3f") {
      value::matrix3f m;
      for (size_t row = 0; row < 3; row++)
        for (size_t col = 0; col < 3; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? static_cast<float>(val_json[row][col].get<double>())
                          : 0.0f;
      return make_scalar(m);
    } else {
      value::matrix3d m;
      for (size_t row = 0; row < 3; row++)
        for (size_t col = 0; col < 3; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? val_json[row][col].get<double>()
                          : 0.0;
      return make_scalar(m);
    }
  } else if (base_type == "matrix4f" || base_type == "matrix4d" ||
             base_type == "frame4d") {
    if (val_json.size() < 4) return nonstd::nullopt;
    if (base_type == "matrix4f") {
      value::matrix4f m;
      for (size_t row = 0; row < 4; row++)
        for (size_t col = 0; col < 4; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? static_cast<float>(val_json[row][col].get<double>())
                          : 0.0f;
      return make_scalar(m);
    } else {
      value::matrix4d m;
      for (size_t row = 0; row < 4; row++)
        for (size_t col = 0; col < 4; col++)
          m.m[row][col] = val_json[row].is_array() && val_json[row].size() > col
                          ? val_json[row][col].get<double>()
                          : 0.0;
      return make_scalar(m);
    }
  } else if (base_type == "dictionary") {
    if (!val_json.is_object()) {
      if (err) *err = "Expected object for dictionary";
      return nonstd::nullopt;
    }
    value::dict d;
    for (auto it = val_json.begin(); it != val_json.end(); ++it) {
      auto sub = JSONToValue(it.value(), err, depth + 1);
      if (sub) {
        d.emplace(it.key(), sub->get_raw());
      }
    }
    return value::Value(std::move(d));
  }

  if (err) *err = "Unsupported type: " + type_name;
  return nonstd::nullopt;
}

// ===========================================================================
// PrimMetaToJSON
// ===========================================================================
#else

}  // namespace (nlohmann helper templates)

// Compatibility bridge for consumers that still use the nlohmann API. The
// conversion is intentionally routed through MiniJSON so the default build
// does not instantiate the large type-dispatch templates above.
nlohmann::json ValueToJSON(const value::Value &val, uint32_t depth) {
  return MiniJSONToNlohmann(ValueToMiniJSON(val, depth));
}

nonstd::optional<value::Value> JSONToValue(const nlohmann::json &j,
                                           std::string *err, uint32_t depth) {
  return MiniJSONToValue(NlohmannToMiniJSON(j), err, depth);
}

nlohmann::json ValueToPlainJSON(const value::Value &val) {
  return MiniJSONToNlohmann(ValueToPlainMiniJSON(val));
}

#endif  // LIGHTUSD_VALUE_JSON_USE_NLOHMANN

nlohmann::json PrimMetaToJSON(const PrimMeta &meta) {
  nlohmann::json j;

  if (meta.has_active()) j["active"] = meta.get_active();
  if (meta.has_hidden()) j["hidden"] = meta.get_hidden();
  if (meta.has_instanceable()) j["instanceable"] = meta.get_instanceable();

  if (meta.has_kind()) j["kind"] = meta.get_kind();
  if (meta.has_doc()) j["doc"] = meta.get_doc().value;
  if (meta.has_comment()) j["comment"] = meta.get_comment().value;
  if (meta.has_displayName()) j["displayName"] = meta.get_displayName();
  if (meta.has_customData()) j["hasCustomData"] = true;
  if (meta.has_assetInfo()) j["hasAssetInfo"] = true;

  j["authored"] = meta.authored();

  // Composition arc summaries
  if (meta.references.has_value() && !meta.references.value().empty()) {
    size_t count = 0;
    for (const auto &p : meta.references.value()) count += p.second.size();
    j["referenceCount"] = count;
  }
  if (meta.payload.has_value() && !meta.payload.value().empty()) {
    size_t count = 0;
    for (const auto &p : meta.payload.value()) count += p.second.size();
    j["payloadCount"] = count;
  }
  if (meta.inherits.has_value() && !meta.inherits.value().empty()) {
    size_t count = 0;
    for (const auto &p : meta.inherits.value()) count += p.second.size();
    j["inheritCount"] = count;
  }
  if (meta.specializes.has_value() && !meta.specializes.value().empty()) {
    j["hasSpecializes"] = true;
  }

  j["unregisteredMetasCount"] = meta.unregisteredMetas.size();

  return j;
}

// ===========================================================================
// ValueTypeToJSONSchema
// ===========================================================================
nlohmann::json ValueTypeToJSONSchema(const std::string &type_name,
                                     uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    return {{"type", "error"}, {"error", "max recursion depth exceeded"}};
  }

  bool is_array = false;
  std::string base = type_name;
  if (type_name.size() >= 3 &&
      type_name.compare(type_name.size() - 2, 2, "[]") == 0) {
    is_array = true;
    base = type_name.substr(0, type_name.size() - 2);
  }

  nlohmann::json schema;
  // Arrays are payloads of the same typed wrapper as scalar values:
  // {"type":"float3[]", "value":[...]}.  The old schema incorrectly
  // described the wrapper itself as an array.
  schema["type"] = "object";

  // Build properties for the wrapped format: { type: string, value: ... }
  nlohmann::json props;
  props["type"] = {{"type", "string"}, {"enum", nlohmann::json::array({type_name})}};

  // Determine value property schema
  if (is_array) {
    props["value"] = {{"type", "array"}};
    auto sub = ValueTypeToJSONSchema(base, depth + 1);
    if (sub.contains("properties") && sub["properties"].contains("value")) {
      props["value"]["items"] = sub["properties"]["value"];
    }
  } else if (base == "float" || base == "double" || base == "half" ||
      base == "int" || base == "uint" || base == "int64" || base == "uint64" ||
      base == "timecode") {
    props["value"] = {{"type", "number"}};
  } else if (base == "bool") {
    props["value"] = {{"type", "boolean"}};
  } else if (base == "string" || base == "token") {
    props["value"] = {{"type", "string"}};
  } else if (base == "asset") {
    props["value"] = {{"type", "object"},
                      {"properties",
                       {{"assetPath", {{"type", "string"}}},
                        {"resolvedPath", {{"type", "string"}}}}}};
  } else if (base == "dictionary") {
    props["value"] = {{"type", "object"}};
  } else if (base == "None" || base == "null") {
    // no value field needed
  } else {
    // Compound - array of numbers
    props["value"] = {{"type", "array"}, {"items", {{"type", "number"}}}};
  }

  schema["properties"] = props;
  schema["required"] = nlohmann::json::array({"type"});
  if (base != "None" && base != "null") {
    schema["required"].push_back("value");
  }

  return schema;
}

// ===========================================================================
// GetRoleTypeNames
// ===========================================================================
std::vector<std::string> GetRoleTypeNames() {
  return {
      "color3f",   "color3d",   "color3h",   "color4f",   "color4d",
      "color4h",   "normal3f",  "normal3d",  "normal3h",  "vector3f",
      "vector3d",  "vector3h",  "point3f",   "point3d",   "point3h",
      "texCoord2f","texCoord2d","texCoord2h","texCoord3f","texCoord3d",
      "texCoord3h","frame4d",
  };
}

// ===========================================================================
// GetPrimTypeNames
// ===========================================================================
std::vector<std::string> GetPrimTypeNames() {
  return {
      "Xform",       "Scope",      "Mesh",     "Sphere",
      "Cube",        "Cylinder",   "Cone",     "Capsule",
      "Material",    "Shader",     "Camera",   "GeomSubset",
      "Skeleton",    "Joint",      "BlendShape",
      "PointLight",  "DistantLight","SphereLight",
      "RectLight",   "DiskLight",  "CylinderLight",
      "DomeLight",   "GeometryLight",
      "RigidBody",   "CollisionGroup", "JointPhysics",
      "Volume",      "Field3D",    "VolumeAsset",
      "RenderVar",   "RenderSettings", "PluginLight",
      "PluginLightFilter",
  };
}

namespace {

std::string PathToString(const Path &path) {
  return path.is_valid() ? path.full_path_name() : std::string();
}

const char *SpecifierToString(Specifier spec) {
  switch (spec) {
    case Specifier::Def:
      return "def";
    case Specifier::Over:
      return "over";
    case Specifier::Class:
      return "class";
    case Specifier::Invalid:
      return "invalid";
  }
  return "invalid";
}

const char *VariabilityToString(Variability variability) {
  switch (variability) {
    case Variability::Varying:
      return "varying";
    case Variability::Uniform:
      return "uniform";
    case Variability::Config:
      return "config";
    case Variability::Invalid:
      return "invalid";
  }
  return "invalid";
}

const char *ListEditQualToString(ListEditQual qual) {
  switch (qual) {
    case ListEditQual::ResetToExplicit:
      return "explicit";
    case ListEditQual::Add:
      return "add";
    case ListEditQual::Delete:
      return "delete";
    case ListEditQual::Order:
      return "order";
    case ListEditQual::Prepend:
      return "prepend";
    case ListEditQual::Append:
      return "append";
    case ListEditQual::Invalid:
      return "invalid";
  }
  return "invalid";
}

nlohmann::json PathListToJSON(const std::vector<Path> &paths) {
  nlohmann::json arr = nlohmann::json::array();
  for (const Path &path : paths) {
    arr.push_back(PathToString(path));
  }
  return arr;
}

nlohmann::json ReferencesToJSON(
    const std::vector<std::pair<ListEditQual, std::vector<Reference>>> &ops) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &op : ops) {
    nlohmann::json item;
    item["op"] = ListEditQualToString(op.first);
    item["items"] = nlohmann::json::array();
    for (const Reference &ref : op.second) {
      item["items"].push_back({
          {"assetPath", ref.asset_path.GetAssetPath()},
          {"primPath", PathToString(ref.prim_path)},
          {"layerOffset",
           {{"offset", ref.layerOffset._offset},
            {"scale", ref.layerOffset._scale}}},
      });
    }
    arr.push_back(std::move(item));
  }
  return arr;
}

nlohmann::json PayloadsToJSON(
    const std::vector<std::pair<ListEditQual, std::vector<Payload>>> &ops) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &op : ops) {
    nlohmann::json item;
    item["op"] = ListEditQualToString(op.first);
    item["items"] = nlohmann::json::array();
    for (const Payload &payload : op.second) {
      item["items"].push_back({
          {"assetPath", payload.asset_path.GetAssetPath()},
          {"primPath", PathToString(payload.prim_path)},
          {"layerOffset",
           {{"offset", payload.layerOffset._offset},
            {"scale", payload.layerOffset._scale}}},
      });
    }
    arr.push_back(std::move(item));
  }
  return arr;
}

nlohmann::json PathListOpsToJSON(
    const std::vector<std::pair<ListEditQual, std::vector<Path>>> &ops) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto &op : ops) {
    arr.push_back({{"op", ListEditQualToString(op.first)},
                   {"items", PathListToJSON(op.second)}});
  }
  return arr;
}

nlohmann::json PrimMetaCompositionToJSON(const PrimMeta &meta) {
  nlohmann::json j = nlohmann::json::object();
  if (meta.references) {
    j["references"] = ReferencesToJSON(meta.references.value());
  }
  if (meta.payload) {
    j["payload"] = PayloadsToJSON(meta.payload.value());
  }
  if (meta.inherits) {
    j["inherits"] = PathListOpsToJSON(meta.inherits.value());
  }
  if (meta.specializes) {
    j["specializes"] = PathListOpsToJSON(meta.specializes.value());
  }
  if (meta.inheritPaths) {
    j["inheritPaths"] = PathListOpsToJSON(meta.inheritPaths.value());
  }
  if (meta.specializePaths) {
    j["specializePaths"] = PathListOpsToJSON(meta.specializePaths.value());
  }
  if (!meta.arc_origins.empty()) {
    j["arcOrigins"] = nlohmann::json::array();
    for (const ArcOrigin &origin : meta.arc_origins) {
      j["arcOrigins"].push_back({
          {"sourceLayerId", origin.source_layer_id},
          {"sourcePrimPath", PathToString(origin.source_prim_path)},
      });
    }
  }
  return j;
}

}  // namespace

namespace {

minijson::Value MiniPrimMeta(const PrimMeta &meta) {
  minijson::Value result = minijson::Value::object();
  if (meta.has_active()) result["active"] = meta.get_active();
  if (meta.has_hidden()) result["hidden"] = meta.get_hidden();
  if (meta.has_instanceable()) result["instanceable"] = meta.get_instanceable();
  if (meta.has_kind()) result["kind"] = meta.get_kind();
  if (meta.has_doc()) result["doc"] = meta.get_doc().value;
  if (meta.has_comment()) result["comment"] = meta.get_comment().value;
  if (meta.has_displayName()) result["displayName"] = meta.get_displayName();
  if (meta.has_customData()) result["hasCustomData"] = true;
  if (meta.has_assetInfo()) result["hasAssetInfo"] = true;
  result["authored"] = meta.authored();
  if (meta.references.has_value() && !meta.references.value().empty()) {
    size_t count = 0;
    for (const auto &item : meta.references.value()) count += item.second.size();
    result["referenceCount"] = count;
  }
  if (meta.payload.has_value() && !meta.payload.value().empty()) {
    size_t count = 0;
    for (const auto &item : meta.payload.value()) count += item.second.size();
    result["payloadCount"] = count;
  }
  if (meta.inherits.has_value() && !meta.inherits.value().empty()) {
    size_t count = 0;
    for (const auto &item : meta.inherits.value()) count += item.second.size();
    result["inheritCount"] = count;
  }
  if (meta.specializes.has_value() && !meta.specializes.value().empty()) {
    result["hasSpecializes"] = true;
  }
  result["unregisteredMetasCount"] = meta.unregisteredMetas.size();
  return result;
}

minijson::Value MiniPathList(const std::vector<Path> &paths) {
  minijson::Value result = minijson::Value::array();
  result.reserve(paths.size());
  for (const auto &path : paths) result.push_back(PathToString(path));
  return result;
}

minijson::Value MiniAttribute(const Attribute &attr) {
  minijson::Value result = minijson::Value::object();
  result["kind"] = "attribute";
  result["name"] = attr.name();
  result["typeName"] = attr.type_name();
  result["typeId"] = attr.type_id();
  result["variability"] = VariabilityToString(attr.variability());
  result["varyingAuthored"] = attr.is_varying_authored();
  result["blocked"] = attr.is_blocked();
  result["hasValue"] = attr.has_value();
  result["hasTimeSamples"] = attr.has_timesamples();
  result["connections"] = MiniPathList(attr.connections());
  if (attr.has_value()) result["value"] = ValueToMiniJSON(attr.get_var().value_raw());
  if (attr.has_timesamples()) {
    minijson::Value samples = minijson::Value::array();
    for (const auto &sample : attr.get_var().ts_raw().get_samples()) {
      minijson::Value item = minijson::Value::object();
      item["time"] = sample.t;
      item["blocked"] = sample.blocked;
      item["value"] = sample.blocked ? minijson::Value(nullptr)
                                      : ValueToMiniJSON(sample.value);
      samples.push_back(std::move(item));
    }
    result["timeSamples"] = std::move(samples);
  }
  return result;
}

minijson::Value MiniRelationship(const Relationship &rel) {
  minijson::Value result = minijson::Value::object();
  result["kind"] = "relationship";
  result["listOp"] = ListEditQualToString(rel.get_listedit_qual());
  result["varyingAuthored"] = rel.is_varying_authored();
  result["blocked"] = rel.is_blocked();
  minijson::Value targets = minijson::Value::array();
  if (rel.is_path()) {
    targets.push_back(PathToString(rel.targetPath));
  } else if (rel.is_pathvector()) {
    targets = MiniPathList(rel.targetPathVector);
  }
  result["targets"] = std::move(targets);
  return result;
}

minijson::Value MiniProperty(const Property &prop) {
  minijson::Value result = minijson::Value::object();
  result["custom"] = prop.has_custom();
  result["valueTypeName"] = prop.value_type_name();
  if (prop.is_attribute()) {
    minijson::Value attr = MiniAttribute(prop.get_attribute());
    if (const auto *items = attr.object_items()) {
      for (const auto &item : *items) result.set(item.key, item.value());
    }
  } else if (prop.is_relationship()) {
    minijson::Value rel = MiniRelationship(prop.get_relationship());
    if (const auto *items = rel.object_items()) {
      for (const auto &item : *items) result.set(item.key, item.value());
    }
  } else {
    result["kind"] = "empty";
  }
  return result;
}

minijson::Value MiniPathListOps(
    const std::vector<std::pair<ListEditQual, std::vector<Path>>> &ops) {
  minijson::Value result = minijson::Value::array();
  result.reserve(ops.size());
  for (const auto &op : ops) {
    minijson::Value item = minijson::Value::object();
    item["op"] = ListEditQualToString(op.first);
    item["items"] = MiniPathList(op.second);
    result.push_back(std::move(item));
  }
  return result;
}

template <typename Arc>
minijson::Value MiniAssetArcs(
    const std::vector<std::pair<ListEditQual, std::vector<Arc>>> &ops) {
  minijson::Value result = minijson::Value::array();
  result.reserve(ops.size());
  for (const auto &op : ops) {
    minijson::Value item = minijson::Value::object();
    item["op"] = ListEditQualToString(op.first);
    minijson::Value entries = minijson::Value::array();
    entries.reserve(op.second.size());
    for (const auto &arc : op.second) {
      minijson::Value entry = minijson::Value::object();
      entry["assetPath"] = arc.asset_path.GetAssetPath();
      entry["primPath"] = PathToString(arc.prim_path);
      minijson::Value offset = minijson::Value::object();
      offset["offset"] = arc.layerOffset._offset;
      offset["scale"] = arc.layerOffset._scale;
      entry["layerOffset"] = std::move(offset);
      entries.push_back(std::move(entry));
    }
    item["items"] = std::move(entries);
    result.push_back(std::move(item));
  }
  return result;
}

minijson::Value MiniPrimMetaComposition(const PrimMeta &meta) {
  minijson::Value result = minijson::Value::object();
  if (meta.references) result["references"] = MiniAssetArcs(meta.references.value());
  if (meta.payload) result["payload"] = MiniAssetArcs(meta.payload.value());
  if (meta.inherits) result["inherits"] = MiniPathListOps(meta.inherits.value());
  if (meta.specializes) result["specializes"] = MiniPathListOps(meta.specializes.value());
  if (meta.inheritPaths) result["inheritPaths"] = MiniPathListOps(meta.inheritPaths.value());
  if (meta.specializePaths) result["specializePaths"] = MiniPathListOps(meta.specializePaths.value());
  if (!meta.arc_origins.empty()) {
    minijson::Value origins = minijson::Value::array();
    origins.reserve(meta.arc_origins.size());
    for (const auto &origin : meta.arc_origins) {
      minijson::Value item = minijson::Value::object();
      item["sourceLayerId"] = origin.source_layer_id;
      item["sourcePrimPath"] = PathToString(origin.source_prim_path);
      origins.push_back(std::move(item));
    }
    result["arcOrigins"] = std::move(origins);
  }
  return result;
}

minijson::Value MiniPrimSpec(const PrimSpec &ps, uint32_t max_depth,
                             uint32_t depth) {
  minijson::Value result = minijson::Value::object();
  result["name"] = ps.name();
  result["typeName"] = ps.typeName();
  result["specifier"] = SpecifierToString(ps.specifier());
  result["currentWorkingPath"] = ps.get_current_working_path();
  minijson::Value search_paths = minijson::Value::array();
  for (const auto &path : ps.get_asset_search_paths()) search_paths.push_back(path);
  result["assetSearchPaths"] = std::move(search_paths);
  minijson::Value properties = minijson::Value::object();
  properties.reserve(ps.props().size());
  for (const auto &prop : ps.props()) properties[prop.first] = MiniProperty(prop.second);
  result["properties"] = std::move(properties);
  result["metadata"] = MiniPrimMetaComposition(ps.metas());
  minijson::Value children = minijson::Value::array();
  children.reserve(ps.children().size());
  for (const PrimSpec &child : ps.children()) {
    if (depth < max_depth) {
      children.push_back(MiniPrimSpec(child, max_depth, depth + 1));
    } else {
      minijson::Value summary = minijson::Value::object();
      summary["name"] = child.name();
      summary["typeName"] = child.typeName();
      summary["specifier"] = SpecifierToString(child.specifier());
      summary["childCount"] = child.children().size();
      summary["propertyCount"] = child.props().size();
      children.push_back(std::move(summary));
    }
  }
  result["children"] = std::move(children);
  return result;
}

}  // namespace

nlohmann::json AttributeToJSON(const Attribute &attr) {
  nlohmann::json j;
  j["kind"] = "attribute";
  j["name"] = attr.name();
  j["typeName"] = attr.type_name();
  j["typeId"] = attr.type_id();
  j["variability"] = VariabilityToString(attr.variability());
  j["varyingAuthored"] = attr.is_varying_authored();
  j["blocked"] = attr.is_blocked();
  j["hasValue"] = attr.has_value();
  j["hasTimeSamples"] = attr.has_timesamples();
  j["connections"] = PathListToJSON(attr.connections());

  if (attr.has_value()) {
    j["value"] = ValueToJSON(attr.get_var().value_raw());
  }
  if (attr.has_timesamples()) {
    j["timeSamples"] = nlohmann::json::array();
    for (const auto &sample : attr.get_var().ts_raw().get_samples()) {
      j["timeSamples"].push_back({
          {"time", sample.t},
          {"blocked", sample.blocked},
          {"value", sample.blocked ? nlohmann::json(nullptr)
                                   : ValueToJSON(sample.value)},
      });
    }
  }

  return j;
}

nlohmann::json RelationshipToJSON(const Relationship &rel) {
  nlohmann::json j;
  j["kind"] = "relationship";
  j["listOp"] = ListEditQualToString(rel.get_listedit_qual());
  j["varyingAuthored"] = rel.is_varying_authored();
  j["blocked"] = rel.is_blocked();
  j["targets"] = nlohmann::json::array();
  if (rel.is_path()) {
    j["targets"].push_back(PathToString(rel.targetPath));
  } else if (rel.is_pathvector()) {
    j["targets"] = PathListToJSON(rel.targetPathVector);
  }
  return j;
}

namespace {

minijson::Value NlohmannToMiniJSON(const nlohmann::json &source) {
  if (source.is_null()) return nullptr;
  if (source.is_boolean()) return source.get<bool>();
  if (source.is_number_unsigned()) return source.get<uint64_t>();
  if (source.is_number_integer()) return source.get<int64_t>();
  if (source.is_number_float()) return source.get<double>();
  if (source.is_string()) return source.get<std::string>();
  if (source.is_array()) {
    minijson::Value result = minijson::Value::array();
    result.reserve(source.size());
    for (const auto &item : source) result.push_back(NlohmannToMiniJSON(item));
    return result;
  }
  if (source.is_object()) {
    minijson::Value result = minijson::Value::object();
    result.reserve(source.size());
    for (const auto &item : source.items()) {
      result.set(item.key(), NlohmannToMiniJSON(item.value()));
    }
    return result;
  }
  return nullptr;
}

nlohmann::json MiniJSONToNlohmann(const minijson::Value &source) {
  if (source.is_null()) return nullptr;
  if (source.is_boolean()) return source.get_bool();
  if (source.type() == minijson::Type::UnsignedInteger) {
    return source.get_uint64();
  }
  if (source.type() == minijson::Type::SignedInteger) {
    return source.get_int64();
  }
  if (source.type() == minijson::Type::Number) return source.get_double();
  if (source.is_string()) return source.get_string();
  if (source.is_array()) {
    nlohmann::json result = nlohmann::json::array();
    if (const auto *items = source.array_items()) {
      for (const auto &item : *items) result.push_back(MiniJSONToNlohmann(item));
    }
    return result;
  }
  if (source.is_object()) {
    nlohmann::json result = nlohmann::json::object();
    if (const auto *items = source.object_items()) {
      for (const auto &item : *items) {
        result[item.key] = MiniJSONToNlohmann(item.value());
      }
    }
    return result;
  }
  return nullptr;
}

}  // namespace

minijson::Value ValueToMiniJSON(const value::Value &val, uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    minijson::Value result = minijson::Value::object();
    result["type"] = "error";
    result["error"] = "max recursion depth exceeded";
    return result;
  }
  if (val.is_empty()) {
    minijson::Value result = minijson::Value::object();
    result["type"] = "null";
    return result;
  }
  if (val.is_none()) {
    minijson::Value result = minijson::Value::object();
    result["type"] = "None";
    return result;
  }
  minijson::Value native;
  if (detail::NativeValueToMiniJSON(val, depth, &native)) {
    return static_cast<minijson::Value &&>(native);
  }
  return NlohmannToMiniJSON(ValueToJSON(val, depth));
}

nonstd::optional<value::Value> MiniJSONToValue(const minijson::Value &j,
                                                std::string *err,
                                                uint32_t depth) {
  if (auto native = detail::NativeMiniJSONToValue(j, err, depth)) return native;
  return JSONToValue(MiniJSONToNlohmann(j), err, depth);
}

minijson::Value ValueToPlainMiniJSON(const value::Value &val) {
  const minijson::Value wrapped = ValueToMiniJSON(val);
  if (wrapped.is_object()) {
    if (const minijson::Value *payload = wrapped.find("value")) {
      return *payload;
    }
  }
  return minijson::Value(nullptr);
}

minijson::Value ValueTypeToMiniJSONSchema(const std::string &type_name,
                                          uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    minijson::Value result = minijson::Value::object();
    result["type"] = "error";
    result["error"] = "max recursion depth exceeded";
    return result;
  }

  const bool is_array = type_name.size() >= 2 &&
                        type_name.compare(type_name.size() - 2, 2, "[]") == 0;
  const std::string base = is_array
                               ? type_name.substr(0, type_name.size() - 2)
                               : type_name;

  minijson::Value schema = minijson::Value::object();
  schema["type"] = "object";
  minijson::Value properties = minijson::Value::object();
  minijson::Value type_schema = minijson::Value::object();
  type_schema["type"] = "string";
  minijson::Value type_enum = minijson::Value::array();
  type_enum.push_back(type_name);
  type_schema["enum"] = std::move(type_enum);
  properties["type"] = std::move(type_schema);

  minijson::Value value_schema = minijson::Value::object();
  if (is_array) {
    value_schema["type"] = "array";
    minijson::Value item_schema = ValueTypeToMiniJSONSchema(base, depth + 1);
    if (item_schema.is_object()) {
      if (const minijson::Value *item_value = item_schema.find("properties")) {
        if (const minijson::Value *item_payload = item_value->find("value")) {
          value_schema["items"] = *item_payload;
        }
      }
    }
  } else if (base == "float" || base == "double" || base == "half" ||
             base == "int" || base == "uint" || base == "int64" ||
             base == "uint64" || base == "timecode") {
    value_schema["type"] = "number";
  } else if (base == "bool") {
    value_schema["type"] = "boolean";
  } else if (base == "string" || base == "token") {
    value_schema["type"] = "string";
  } else if (base == "asset" || base == "dictionary") {
    value_schema["type"] = "object";
  } else if (base != "None" && base != "null") {
    value_schema["type"] = "array";
    minijson::Value number_schema = minijson::Value::object();
    number_schema["type"] = "number";
    value_schema["items"] = std::move(number_schema);
  }
  if (base != "None" && base != "null") {
    properties["value"] = std::move(value_schema);
  }
  schema["properties"] = std::move(properties);
  minijson::Value required = minijson::Value::array();
  required.push_back("type");
  if (base != "None" && base != "null") required.push_back("value");
  schema["required"] = std::move(required);
  return schema;
}

minijson::Value PrimMetaToMiniJSON(const PrimMeta &meta) {
  return MiniPrimMeta(meta);
}

minijson::Value PrimSpecToMiniJSON(const PrimSpec &ps, uint32_t max_depth,
                                   uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    minijson::Value result = minijson::Value::object();
    result["type"] = "error";
    result["error"] = "max recursion depth exceeded";
    return result;
  }
  return MiniPrimSpec(ps, max_depth, depth);
}

minijson::Value PropertyToMiniJSON(const Property &prop) {
  return MiniProperty(prop);
}

minijson::Value AttributeToMiniJSON(const Attribute &attr) {
  return MiniAttribute(attr);
}

minijson::Value RelationshipToMiniJSON(const Relationship &rel) {
  return MiniRelationship(rel);
}

nlohmann::json PropertyToJSON(const Property &prop) {
  nlohmann::json j;
  j["custom"] = prop.has_custom();
  j["valueTypeName"] = prop.value_type_name();
  if (prop.is_attribute()) {
    j.update(AttributeToJSON(prop.get_attribute()));
  } else if (prop.is_relationship()) {
    j.update(RelationshipToJSON(prop.get_relationship()));
  } else {
    j["kind"] = "empty";
  }
  return j;
}

nlohmann::json PrimSpecToJSON(const PrimSpec &ps, uint32_t max_depth,
                              uint32_t depth) {
  nlohmann::json j;
  j["name"] = ps.name();
  j["typeName"] = ps.typeName();
  j["specifier"] = SpecifierToString(ps.specifier());
  j["currentWorkingPath"] = ps.get_current_working_path();
  j["assetSearchPaths"] = ps.get_asset_search_paths();

  j["properties"] = nlohmann::json::object();
  for (const auto &prop : ps.props()) {
    j["properties"][prop.first] = PropertyToJSON(prop.second);
  }

  j["metadata"] = PrimMetaCompositionToJSON(ps.metas());
  j["children"] = nlohmann::json::array();
  if (depth < max_depth) {
    for (const PrimSpec &child : ps.children()) {
      j["children"].push_back(PrimSpecToJSON(child, max_depth, depth + 1));
    }
  } else {
    for (const PrimSpec &child : ps.children()) {
      j["children"].push_back({{"name", child.name()},
                               {"typeName", child.typeName()},
                               {"specifier", SpecifierToString(child.specifier())},
                               {"childCount", child.children().size()},
                               {"propertyCount", child.props().size()}});
    }
  }
  return j;
}

} // namespace tydra
} // namespace lightusd
