#include "value-to-json-internal.hh"
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>
#include "common-macros.inc"
namespace lightusd { namespace tydra { namespace detail {
// Native minijson fast paths.  These cover the high-frequency scalar and
// scalar-array values without constructing a temporary nlohmann document.
// Less common role/compound/container values continue through the legacy
// adapter below until their individual representations are ported.
template <typename T>
bool NativeScalar(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<T>(false);
  if (!value) return false;
  if constexpr (std::is_same<T, value::token>::value) {
    *out = value->str();
  } else {
    *out = value.value();
  }
  return true;
}

template <>
bool NativeScalar<uint8_t>(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<uint8_t>(false);
  if (!value) return false;
  *out = static_cast<int>(value.value());
  return true;
}

template <>
bool NativeScalar<value::half>(const value::Value &source,
                              minijson::Value *out) {
  auto value = source.get_value<value::half>(false);
  if (!value) return false;
  *out = static_cast<double>(value::half_to_float(value.value()));
  return true;
}

template <typename T>
bool NativeArray(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<std::vector<T>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(value->size());
  for (const auto &item : value.value()) {
    minijson::Value element;
    if (!NativeScalar<T>(value::Value(item), &element)) return false;
    array.push_back(std::move(element));
  }
  *out = std::move(array);
  return true;
}

template <typename T, size_t N>
bool NativeCompoundArray(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<std::vector<std::array<T, N>>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(value->size());
  for (const auto &item : value.value()) {
    minijson::Value compound = minijson::Value::array();
    compound.reserve(N);
    for (const auto &component : item) {
      if constexpr (std::is_same<T, value::half>::value) {
        compound.push_back(static_cast<double>(value::half_to_float(component)));
      } else {
        compound.push_back(static_cast<double>(component));
      }
    }
    array.push_back(std::move(compound));
  }
  *out = std::move(array);
  return true;
}

template <typename T, size_t N>
bool NativeIndexedCompoundArray(const value::Value &source,
                                minijson::Value *out) {
  auto value = source.get_value<std::vector<T>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(value->size());
  for (const auto &item : value.value()) {
    minijson::Value compound = minijson::Value::array();
    compound.reserve(N);
    for (size_t i = 0; i < N; ++i) {
      if constexpr (std::is_same<typename std::remove_cv<
                                     typename std::remove_reference<
                                         decltype(item[i])>::type>::type,
                                 value::half>::value) {
        compound.push_back(static_cast<double>(value::half_to_float(item[i])));
      } else {
        compound.push_back(static_cast<double>(item[i]));
      }
    }
    array.push_back(std::move(compound));
  }
  *out = std::move(array);
  return true;
}

template <typename T, size_t N>
bool NativeCompound(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<std::array<T, N>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(N);
  for (const auto &item : value.value()) {
    if constexpr (std::is_same<T, value::half>::value) {
      array.push_back(static_cast<double>(value::half_to_float(item)));
    } else if constexpr (std::is_floating_point<T>::value) {
      array.push_back(static_cast<double>(item));
    } else {
      array.push_back(item);
    }
  }
  *out = std::move(array);
  return true;
}

template <typename T, size_t N>
bool NativeIndexedCompound(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<T>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(N);
  for (size_t i = 0; i < N; ++i) {
    using Component = typename std::remove_cv<typename std::remove_reference<
        decltype((*value)[i])>::type>::type;
    if constexpr (std::is_same<Component, value::half>::value) {
      array.push_back(static_cast<double>(value::half_to_float((*value)[i])));
    } else {
      array.push_back(static_cast<double>((*value)[i]));
    }
  }
  *out = std::move(array);
  return true;
}

template <typename Matrix, size_t N>
bool NativeMatrix(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<Matrix>(false);
  if (!value) return false;
  minijson::Value rows = minijson::Value::array();
  rows.reserve(N);
  for (size_t i = 0; i < N; ++i) {
    minijson::Value row = minijson::Value::array();
    row.reserve(N);
    for (size_t j = 0; j < N; ++j) {
      row.push_back(static_cast<double>(value->m[i][j]));
    }
    rows.push_back(std::move(row));
  }
  *out = std::move(rows);
  return true;
}

template <typename Quat>
bool NativeQuat(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<Quat>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(4);
  for (size_t i = 0; i < 4; ++i) {
    using Component = typename std::remove_cv<typename std::remove_reference<
        decltype((*value)[i])>::type>::type;
    if constexpr (std::is_same<Component, value::half>::value) {
      array.push_back(static_cast<double>(value::half_to_float((*value)[i])));
    } else {
      array.push_back(static_cast<double>((*value)[i]));
    }
  }
  *out = std::move(array);
  return true;
}

template <>
bool NativeArray<std::string>(const value::Value &source,
                              minijson::Value *out) {
  auto value = source.get_value<std::vector<std::string>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(value->size());
  for (const auto &item : value.value()) array.push_back(item);
  *out = std::move(array);
  return true;
}

template <>
bool NativeArray<bool>(const value::Value &source, minijson::Value *out) {
  auto value = source.get_value<std::vector<bool>>(false);
  if (!value) return false;
  minijson::Value array = minijson::Value::array();
  array.reserve(value->size());
  for (size_t i = 0; i < value->size(); ++i) {
    array.push_back(static_cast<bool>(value.value()[i]));
  }
  *out = std::move(array);
  return true;
}

bool NativeValueToMiniJSON(const value::Value &source, uint32_t depth,
                           minijson::Value *out) {
  if (!out || depth > kMaxDefaultTraversalLimit) return false;
  const uint32_t tid = source.type_id();
  const std::string type_name = source.type_name();
  minijson::Value value;
  bool matched = false;

  if (!source.is_array() && tid == value::TypeTraits<value::AssetPath>::type_id()) {
    auto asset = source.get_value<value::AssetPath>(false);
    if (asset) {
      value = minijson::Value::object();
      value["assetPath"] = asset->GetAssetPath();
      value["resolvedPath"] = asset->GetResolvedPath();
      matched = true;
    }
  }
  if (!source.is_array() && tid == value::TypeTraits<value::timecode>::type_id()) {
    auto timecode = source.get_value<value::timecode>(false);
    if (timecode) {
      value = timecode->value;
      matched = true;
    }
  }
  if (!source.is_array() && tid == value::TypeTraits<value::dict>::type_id()) {
    auto dictionary = source.get_value<value::dict>(false);
    if (dictionary) {
      value = minijson::Value::object();
      value.reserve(dictionary->size());
      for (const auto &item : dictionary.value()) {
        minijson::Value nested;
        if (!NativeValueToMiniJSON(value::Value(item.second), depth + 1,
                                   &nested)) {
          nested = ValueToMiniJSON(value::Value(item.second), depth + 1);
        }
        value.set(item.first, std::move(nested));
      }
      matched = true;
    }
  }

  // token[] has a dedicated type id rather than the generic array bit.
  if (tid == value::TypeTraits<std::vector<value::token>>::type_id()) {
    matched = NativeArray<value::token>(source, &value);
  }

#define TRY_NATIVE_SCALAR(__type)                                           \
  if (!source.is_array() && tid == value::TypeTraits<__type>::type_id()) { \
    matched = NativeScalar<__type>(source, &value);                        \
  }
#define TRY_NATIVE_ARRAY(__type)                                           \
  if (source.is_array() &&                                               \
      (tid & ~value::TYPE_ID_1D_ARRAY_BIT) ==                           \
          value::TypeTraits<__type>::type_id()) {                        \
    matched = NativeArray<__type>(source, &value);                       \
  }
#define TRY_NATIVE_COMPOUND_ARRAY(__type, __scalar, __count)                \
  if (source.is_array() &&                                                 \
      (tid & ~value::TYPE_ID_1D_ARRAY_BIT) ==                              \
          value::TypeTraits<__type>::type_id()) {                          \
    matched = NativeCompoundArray<__scalar, __count>(source, &value);       \
  }

  TRY_NATIVE_SCALAR(float)
  TRY_NATIVE_SCALAR(double)
  TRY_NATIVE_SCALAR(int32_t)
  TRY_NATIVE_SCALAR(uint8_t)
  TRY_NATIVE_SCALAR(uint32_t)
  TRY_NATIVE_SCALAR(int64_t)
  TRY_NATIVE_SCALAR(uint64_t)
  TRY_NATIVE_SCALAR(bool)
  TRY_NATIVE_SCALAR(std::string)
  TRY_NATIVE_SCALAR(value::token)
  TRY_NATIVE_SCALAR(value::half)
  if (!source.is_array() && tid == value::TypeTraits<value::float2>::type_id())
    matched = NativeCompound<float, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::float3>::type_id())
    matched = NativeCompound<float, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::float4>::type_id())
    matched = NativeCompound<float, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::double2>::type_id())
    matched = NativeCompound<double, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::double3>::type_id())
    matched = NativeCompound<double, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::double4>::type_id())
    matched = NativeCompound<double, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::int2>::type_id())
    matched = NativeCompound<int32_t, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::int3>::type_id())
    matched = NativeCompound<int32_t, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::int4>::type_id())
    matched = NativeCompound<int32_t, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::half2>::type_id())
    matched = NativeCompound<value::half, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::half3>::type_id())
    matched = NativeCompound<value::half, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::half4>::type_id())
    matched = NativeCompound<value::half, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix2f>::type_id())
    matched = NativeMatrix<value::matrix2f, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix3f>::type_id())
    matched = NativeMatrix<value::matrix3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix4f>::type_id())
    matched = NativeMatrix<value::matrix4f, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix2d>::type_id())
    matched = NativeMatrix<value::matrix2d, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix3d>::type_id())
    matched = NativeMatrix<value::matrix3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::matrix4d>::type_id())
    matched = NativeMatrix<value::matrix4d, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::quatf>::type_id())
    matched = NativeQuat<value::quatf>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::quatd>::type_id())
    matched = NativeQuat<value::quatd>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::quath>::type_id())
    matched = NativeQuat<value::quath>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color3f>::type_id())
    matched = NativeIndexedCompound<value::color3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color4f>::type_id())
    matched = NativeIndexedCompound<value::color4f, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color3d>::type_id())
    matched = NativeIndexedCompound<value::color3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color4d>::type_id())
    matched = NativeIndexedCompound<value::color4d, 4>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::normal3f>::type_id())
    matched = NativeIndexedCompound<value::normal3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::vector3f>::type_id())
    matched = NativeIndexedCompound<value::vector3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::vector3d>::type_id())
    matched = NativeIndexedCompound<value::vector3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::point3f>::type_id())
    matched = NativeIndexedCompound<value::point3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::point3d>::type_id())
    matched = NativeIndexedCompound<value::point3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::normal3d>::type_id())
    matched = NativeIndexedCompound<value::normal3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord2f>::type_id())
    matched = NativeIndexedCompound<value::texcoord2f, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord2d>::type_id())
    matched = NativeIndexedCompound<value::texcoord2d, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord3f>::type_id())
    matched = NativeIndexedCompound<value::texcoord3f, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord3d>::type_id())
    matched = NativeIndexedCompound<value::texcoord3d, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::vector3h>::type_id())
    matched = NativeIndexedCompound<value::vector3h, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::normal3h>::type_id())
    matched = NativeIndexedCompound<value::normal3h, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::point3h>::type_id())
    matched = NativeIndexedCompound<value::point3h, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord2h>::type_id())
    matched = NativeIndexedCompound<value::texcoord2h, 2>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::texcoord3h>::type_id())
    matched = NativeIndexedCompound<value::texcoord3h, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color3h>::type_id())
    matched = NativeIndexedCompound<value::color3h, 3>(source, &value);
  if (!source.is_array() && tid == value::TypeTraits<value::color4h>::type_id())
    matched = NativeIndexedCompound<value::color4h, 4>(source, &value);
  TRY_NATIVE_ARRAY(float)
  TRY_NATIVE_ARRAY(double)
  TRY_NATIVE_ARRAY(int32_t)
  TRY_NATIVE_ARRAY(uint8_t)
  TRY_NATIVE_ARRAY(uint32_t)
  TRY_NATIVE_ARRAY(int64_t)
  TRY_NATIVE_ARRAY(uint64_t)
  TRY_NATIVE_ARRAY(bool)
  TRY_NATIVE_ARRAY(std::string)
  TRY_NATIVE_ARRAY(value::token)
  TRY_NATIVE_ARRAY(value::half)
  if (source.is_array() &&
      tid == value::TypeTraits<std::vector<value::token>>::type_id()) {
    matched = NativeArray<value::token>(source, &value);
  }
  TRY_NATIVE_COMPOUND_ARRAY(value::float2, float, 2)
  TRY_NATIVE_COMPOUND_ARRAY(value::float3, float, 3)
  TRY_NATIVE_COMPOUND_ARRAY(value::float4, float, 4)
  TRY_NATIVE_COMPOUND_ARRAY(value::double2, double, 2)
  TRY_NATIVE_COMPOUND_ARRAY(value::double3, double, 3)
  TRY_NATIVE_COMPOUND_ARRAY(value::double4, double, 4)
  TRY_NATIVE_COMPOUND_ARRAY(value::int2, int32_t, 2)
  TRY_NATIVE_COMPOUND_ARRAY(value::int3, int32_t, 3)
  TRY_NATIVE_COMPOUND_ARRAY(value::int4, int32_t, 4)
  TRY_NATIVE_COMPOUND_ARRAY(value::half2, value::half, 2)
  TRY_NATIVE_COMPOUND_ARRAY(value::half3, value::half, 3)
  TRY_NATIVE_COMPOUND_ARRAY(value::half4, value::half, 4)
#define TRY_NATIVE_INDEXED_ARRAY(__type, __count)                            \
  if (source.is_array() &&                                                  \
      (tid & ~value::TYPE_ID_1D_ARRAY_BIT) ==                               \
          value::TypeTraits<__type>::type_id()) {                           \
    matched = NativeIndexedCompoundArray<__type, __count>(source, &value);  \
  }
  TRY_NATIVE_INDEXED_ARRAY(value::color3f, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::color4f, 4)
  TRY_NATIVE_INDEXED_ARRAY(value::color3d, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::color4d, 4)
  TRY_NATIVE_INDEXED_ARRAY(value::normal3f, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::normal3d, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::point3f, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::point3d, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::vector3f, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::vector3d, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord2f, 2)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord2d, 2)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord3f, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord3d, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::vector3h, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::normal3h, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::point3h, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::color3h, 3)
  TRY_NATIVE_INDEXED_ARRAY(value::color4h, 4)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord2h, 2)
  TRY_NATIVE_INDEXED_ARRAY(value::texcoord3h, 3)
#undef TRY_NATIVE_INDEXED_ARRAY

#undef TRY_NATIVE_SCALAR
#undef TRY_NATIVE_ARRAY
#undef TRY_NATIVE_COMPOUND_ARRAY

  if (!matched) return false;
  minijson::Value result = minijson::Value::object();
  result["type"] = source.is_array() ? type_name : type_name;
  result["value"] = std::move(value);
  *out = std::move(result);
  return true;
}

} } }
