#include "value-to-json-internal.hh"
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>
#include "common-macros.inc"
namespace lightusd { namespace tydra { namespace detail {
template <typename T>
bool NativeMiniScalar(const minijson::Value &source, T *out) {
  if (!out) return false;
  if constexpr (std::is_same<T, bool>::value) {
    return source.as_bool(out);
  } else if constexpr (std::is_same<T, value::half>::value) {
    double value = 0.0;
    if (!source.as_double(&value) || !std::isfinite(value)) return false;
    *out = value::float_to_half_full(static_cast<float>(value));
    return true;
  } else if constexpr (std::is_floating_point<T>::value) {
    double value = 0.0;
    if (!source.as_double(&value) || !std::isfinite(value)) return false;
    *out = static_cast<T>(value);
    return true;
  } else if constexpr (std::is_integral<T>::value && std::is_signed<T>::value) {
    int64_t value = 0;
    if (!source.as_int64(&value)) return false;
    if (value < static_cast<int64_t>((std::numeric_limits<T>::min)()) ||
        value > static_cast<int64_t>((std::numeric_limits<T>::max)())) {
      return false;
    }
    *out = static_cast<T>(value);
    return true;
  } else if constexpr (std::is_integral<T>::value && std::is_unsigned<T>::value) {
    uint64_t value = 0;
    if (!source.as_uint64(&value)) return false;
    if (value > static_cast<uint64_t>((std::numeric_limits<T>::max)())) {
      return false;
    }
    *out = static_cast<T>(value);
    return true;
  } else {
    return false;
  }
}

template <typename T>
nonstd::optional<value::Value> NativeMiniArray(const minijson::Value &source) {
  if (!source.is_array() || !source.array_items()) return nonstd::nullopt;
  std::vector<T> values;
  values.reserve(source.size());
  for (const auto &item : *source.array_items()) {
    T value{};
    if (!NativeMiniScalar(item, &value)) return nonstd::nullopt;
    values.push_back(value);
  }
  return value::Value(std::move(values));
}

template <typename T, size_t N>
nonstd::optional<value::Value> NativeMiniCompound(
    const minijson::Value &source) {
  if (!source.is_array() || source.size() != N || !source.array_items()) {
    return nonstd::nullopt;
  }
  std::array<T, N> values{};
  for (size_t i = 0; i < N; ++i) {
    if (!NativeMiniScalar((*source.array_items())[i], &values[i])) {
      return nonstd::nullopt;
    }
  }
  return value::Value(values);
}

template <typename T, size_t N>
nonstd::optional<value::Value> NativeMiniCompoundArray(
    const minijson::Value &source) {
  if (!source.is_array() || !source.array_items()) return nonstd::nullopt;
  std::vector<std::array<T, N>> values;
  values.reserve(source.size());
  for (const auto &item : *source.array_items()) {
    auto compound = NativeMiniCompound<T, N>(item);
    if (!compound) return nonstd::nullopt;
    auto typed = compound->template get_value<std::array<T, N>>();
    if (!typed) return nonstd::nullopt;
    values.push_back(*typed);
  }
  return value::Value(std::move(values));
}

template <typename T, size_t N>
nonstd::optional<value::Value> NativeMiniIndexedArray(
    const minijson::Value &source) {
  if (!source.is_array() || !source.array_items()) return nonstd::nullopt;
  std::vector<T> values;
  values.reserve(source.size());
  for (const auto &item : *source.array_items()) {
    if (!item.is_array() || item.size() != N || !item.array_items()) {
      return nonstd::nullopt;
    }
    T element{};
    for (size_t i = 0; i < N; ++i) {
      double component = 0.0;
      if (!NativeMiniScalar((*item.array_items())[i], &component)) {
        return nonstd::nullopt;
      }
      using Component = typename std::remove_reference<decltype(element[i])>::type;
      if constexpr (std::is_same<Component, value::half>::value) {
        element[i] = value::float_to_half_full(static_cast<float>(component));
      } else {
        element[i] = static_cast<Component>(component);
      }
    }
    values.push_back(element);
  }
  return value::Value(std::move(values));
}

template <typename T, size_t N>
nonstd::optional<value::Value> NativeMiniIndexedCompound(
    const minijson::Value &source) {
  if (!source.is_array() || source.size() != N || !source.array_items()) {
    return nonstd::nullopt;
  }
  T values{};
  for (size_t i = 0; i < N; ++i) {
    double item = 0.0;
    if (!NativeMiniScalar((*source.array_items())[i], &item)) {
      return nonstd::nullopt;
    }
    using Component = typename std::remove_reference<decltype(values[i])>::type;
    if constexpr (std::is_same<Component, value::half>::value) {
      values[i] = value::float_to_half_full(static_cast<float>(item));
    } else {
      values[i] = static_cast<Component>(item);
    }
  }
  return value::Value(values);
}

template <typename Matrix, size_t N>
nonstd::optional<value::Value> NativeMiniMatrix(
    const minijson::Value &source) {
  if (!source.is_array() || source.size() != N || !source.array_items()) {
    return nonstd::nullopt;
  }
  Matrix matrix{};
  for (size_t i = 0; i < N; ++i) {
    const minijson::Value &row = (*source.array_items())[i];
    if (!row.is_array() || row.size() != N || !row.array_items()) {
      return nonstd::nullopt;
    }
    for (size_t j = 0; j < N; ++j) {
      double item = 0.0;
      if (!NativeMiniScalar((*row.array_items())[j], &item)) {
        return nonstd::nullopt;
      }
      using MatrixScalar = typename std::remove_reference<
          decltype(matrix.m[i][j])>::type;
      matrix.m[i][j] = static_cast<MatrixScalar>(item);
    }
  }
  return value::Value(matrix);
}

template <typename Quat>
nonstd::optional<value::Value> NativeMiniQuat(const minijson::Value &source) {
  if (!source.is_array() || source.size() != 4 || !source.array_items()) {
    return nonstd::nullopt;
  }
  Quat quat{};
  for (size_t i = 0; i < 4; ++i) {
    double item = 0.0;
    if (!NativeMiniScalar((*source.array_items())[i], &item)) {
      return nonstd::nullopt;
    }
    using QuatScalar = typename std::remove_cv<typename std::remove_reference<
        decltype(quat[i])>::type>::type;
    if constexpr (std::is_same<QuatScalar, value::half>::value) {
      quat[i] = value::float_to_half_full(static_cast<float>(item));
    } else {
      quat[i] = static_cast<QuatScalar>(item);
    }
  }
  return value::Value(quat);
}

template <typename T>
bool StoreNativeValue(const value::Value &source, value::any_value *out) {
  if (!out) return false;
  auto value = source.get_value<T>(false);
  if (!value) return false;
  *out = value.value();
  return true;
}

bool StoreNativeDictionaryValue(const value::Value &source,
                                value::any_value *out) {
  if (!out) return false;
  const std::string type_name = source.type_name();
  if (type_name == "float") return StoreNativeValue<float>(source, out);
  if (type_name == "double") return StoreNativeValue<double>(source, out);
  if (type_name == "int") return StoreNativeValue<int32_t>(source, out);
  if (type_name == "uint8") return StoreNativeValue<uint8_t>(source, out);
  if (type_name == "uint") return StoreNativeValue<uint32_t>(source, out);
  if (type_name == "int64") return StoreNativeValue<int64_t>(source, out);
  if (type_name == "uint64") return StoreNativeValue<uint64_t>(source, out);
  if (type_name == "bool") return StoreNativeValue<bool>(source, out);
  if (type_name == "string") return StoreNativeValue<std::string>(source, out);
  if (type_name == "token") return StoreNativeValue<value::token>(source, out);
  if (type_name == "half") return StoreNativeValue<value::half>(source, out);
  if (type_name == "float[]") return StoreNativeValue<std::vector<float>>(source, out);
  if (type_name == "double[]") return StoreNativeValue<std::vector<double>>(source, out);
  if (type_name == "int[]") return StoreNativeValue<std::vector<int32_t>>(source, out);
  if (type_name == "bool[]") return StoreNativeValue<std::vector<bool>>(source, out);
  if (type_name == "half[]") return StoreNativeValue<std::vector<value::half>>(source, out);
  if (type_name == "float2") return StoreNativeValue<value::float2>(source, out);
  if (type_name == "float3") return StoreNativeValue<value::float3>(source, out);
  if (type_name == "float4") return StoreNativeValue<value::float4>(source, out);
  if (type_name == "double2") return StoreNativeValue<value::double2>(source, out);
  if (type_name == "double3") return StoreNativeValue<value::double3>(source, out);
  if (type_name == "double4") return StoreNativeValue<value::double4>(source, out);
  if (type_name == "half2") return StoreNativeValue<value::half2>(source, out);
  if (type_name == "half3") return StoreNativeValue<value::half3>(source, out);
  if (type_name == "half4") return StoreNativeValue<value::half4>(source, out);
  if (type_name == "color3f") return StoreNativeValue<value::color3f>(source, out);
  if (type_name == "color4f") return StoreNativeValue<value::color4f>(source, out);
  if (type_name == "color3h") return StoreNativeValue<value::color3h>(source, out);
  if (type_name == "color4h") return StoreNativeValue<value::color4h>(source, out);
  if (type_name == "normal3f") return StoreNativeValue<value::normal3f>(source, out);
  if (type_name == "normal3h") return StoreNativeValue<value::normal3h>(source, out);
  if (type_name == "point3f") return StoreNativeValue<value::point3f>(source, out);
  if (type_name == "point3h") return StoreNativeValue<value::point3h>(source, out);
  if (type_name == "vector3f") return StoreNativeValue<value::vector3f>(source, out);
  if (type_name == "vector3h") return StoreNativeValue<value::vector3h>(source, out);
  if (type_name == "texcoord2f") return StoreNativeValue<value::texcoord2f>(source, out);
  if (type_name == "texcoord2h") return StoreNativeValue<value::texcoord2h>(source, out);
  if (type_name == "texcoord3f") return StoreNativeValue<value::texcoord3f>(source, out);
  if (type_name == "texcoord3h") return StoreNativeValue<value::texcoord3h>(source, out);
  if (type_name == "matrix2d") return StoreNativeValue<value::matrix2d>(source, out);
  if (type_name == "matrix3d") return StoreNativeValue<value::matrix3d>(source, out);
  if (type_name == "matrix4d") return StoreNativeValue<value::matrix4d>(source, out);
  if (type_name == "asset") return StoreNativeValue<value::AssetPath>(source, out);
  if (type_name == "timecode") return StoreNativeValue<value::timecode>(source, out);
  if (type_name == "dictionary") return StoreNativeValue<value::dict>(source, out);
  return false;
}

nonstd::optional<value::Value> NativeMiniJSONToValue(
    const minijson::Value &source, std::string *err, uint32_t depth) {
  if (depth > kMaxDefaultTraversalLimit) {
    if (err) *err = "MiniJSONToValue: max recursion depth exceeded";
    return nonstd::nullopt;
  }
  if (!source.is_object()) return nonstd::nullopt;
  const minijson::Value *type = source.find("type");
  const minijson::Value *payload = source.find("value");
  if (!type || !payload || !type->is_string()) return nonstd::nullopt;
  const std::string type_name = type->get_string();

#define TRY_NATIVE_DECODE(__name, __type)                                    \
  if (type_name == __name) {                                                \
    __type value{};                                                          \
    if (!NativeMiniScalar(*payload, &value)) {                              \
      if (err) *err = std::string("Invalid value for type ") + __name;     \
      return nonstd::nullopt;                                                \
    }                                                                         \
    return value::Value(value);                                              \
  }

  TRY_NATIVE_DECODE("float", float)
  TRY_NATIVE_DECODE("double", double)
  TRY_NATIVE_DECODE("int", int32_t)
  TRY_NATIVE_DECODE("uint8", uint8_t)
  TRY_NATIVE_DECODE("uint", uint32_t)
  TRY_NATIVE_DECODE("int64", int64_t)
  TRY_NATIVE_DECODE("uint64", uint64_t)
  TRY_NATIVE_DECODE("bool", bool)

  if (type_name == "string") {
    std::string value;
    if (!payload->as_string(&value)) {
      if (err) *err = "Invalid value for type string";
      return nonstd::nullopt;
    }
    return value::Value(std::move(value));
  }
  if (type_name == "token[]") {
    if (!payload->is_array() || !payload->array_items()) return nonstd::nullopt;
    std::vector<value::token> values;
    values.reserve(payload->size());
    for (const auto &item : *payload->array_items()) {
      std::string token;
      if (!item.as_string(&token)) return nonstd::nullopt;
      values.emplace_back(std::move(token));
    }
    return value::Value(std::move(values));
  }
  if (type_name == "token") {
    std::string token;
    if (!payload->as_string(&token)) return nonstd::nullopt;
    return value::Value(value::token(std::move(token)));
  }
  if (type_name == "asset") {
    if (!payload->is_object()) return nonstd::nullopt;
    std::string asset_path, resolved_path;
    const minijson::Value *asset = payload->find("assetPath");
    const minijson::Value *resolved = payload->find("resolvedPath");
    if (!asset || !asset->as_string(&asset_path)) return nonstd::nullopt;
    if (resolved && !resolved->as_string(&resolved_path)) return nonstd::nullopt;
    return value::Value(value::AssetPath(std::move(asset_path),
                                         std::move(resolved_path)));
  }
  if (type_name == "timecode") {
    double time = 0.0;
    if (!NativeMiniScalar(*payload, &time)) return nonstd::nullopt;
    value::timecode timecode{time};
    return value::Value(timecode);
  }
  if (type_name == "dictionary") {
    if (!payload->is_object() || !payload->object_items()) {
      return nonstd::nullopt;
    }
    value::dict dictionary;
    for (const auto &item : *payload->object_items()) {
      auto nested = NativeMiniJSONToValue(item.value(), err, depth + 1);
      if (!nested || !StoreNativeDictionaryValue(nested.value(),
                                                  &dictionary[item.key])) {
        return nonstd::nullopt;
      }
    }
    return value::Value(std::move(dictionary));
  }

#undef TRY_NATIVE_DECODE

  if (type_name == "float[]") return NativeMiniArray<float>(*payload);
  if (type_name == "double[]") return NativeMiniArray<double>(*payload);
  if (type_name == "int[]") return NativeMiniArray<int32_t>(*payload);
  if (type_name == "uint8[]") return NativeMiniArray<uint8_t>(*payload);
  if (type_name == "uint[]") return NativeMiniArray<uint32_t>(*payload);
  if (type_name == "int64[]") return NativeMiniArray<int64_t>(*payload);
  if (type_name == "uint64[]") return NativeMiniArray<uint64_t>(*payload);
  if (type_name == "bool[]") return NativeMiniArray<bool>(*payload);
  if (type_name == "float2[]") return NativeMiniCompoundArray<float, 2>(*payload);
  if (type_name == "float3[]") return NativeMiniCompoundArray<float, 3>(*payload);
  if (type_name == "float4[]") return NativeMiniCompoundArray<float, 4>(*payload);
  if (type_name == "double2[]") return NativeMiniCompoundArray<double, 2>(*payload);
  if (type_name == "double3[]") return NativeMiniCompoundArray<double, 3>(*payload);
  if (type_name == "double4[]") return NativeMiniCompoundArray<double, 4>(*payload);
  if (type_name == "int2[]") return NativeMiniCompoundArray<int32_t, 2>(*payload);
  if (type_name == "int3[]") return NativeMiniCompoundArray<int32_t, 3>(*payload);
  if (type_name == "int4[]") return NativeMiniCompoundArray<int32_t, 4>(*payload);
  if (type_name == "color3f[]") return NativeMiniIndexedArray<value::color3f, 3>(*payload);
  if (type_name == "color4f[]") return NativeMiniIndexedArray<value::color4f, 4>(*payload);
  if (type_name == "color3d[]") return NativeMiniIndexedArray<value::color3d, 3>(*payload);
  if (type_name == "color4d[]") return NativeMiniIndexedArray<value::color4d, 4>(*payload);
  if (type_name == "normal3f[]") return NativeMiniIndexedArray<value::normal3f, 3>(*payload);
  if (type_name == "normal3d[]") return NativeMiniIndexedArray<value::normal3d, 3>(*payload);
  if (type_name == "point3f[]") return NativeMiniIndexedArray<value::point3f, 3>(*payload);
  if (type_name == "point3d[]") return NativeMiniIndexedArray<value::point3d, 3>(*payload);
  if (type_name == "vector3f[]") return NativeMiniIndexedArray<value::vector3f, 3>(*payload);
  if (type_name == "vector3d[]") return NativeMiniIndexedArray<value::vector3d, 3>(*payload);
  if (type_name == "texcoord2f[]") return NativeMiniIndexedArray<value::texcoord2f, 2>(*payload);
  if (type_name == "texcoord2d[]") return NativeMiniIndexedArray<value::texcoord2d, 2>(*payload);
  if (type_name == "texcoord3f[]") return NativeMiniIndexedArray<value::texcoord3f, 3>(*payload);
  if (type_name == "texcoord3d[]") return NativeMiniIndexedArray<value::texcoord3d, 3>(*payload);
  if (type_name == "vector3h[]") return NativeMiniIndexedArray<value::vector3h, 3>(*payload);
  if (type_name == "normal3h[]") return NativeMiniIndexedArray<value::normal3h, 3>(*payload);
  if (type_name == "point3h[]") return NativeMiniIndexedArray<value::point3h, 3>(*payload);
  if (type_name == "texcoord2h[]") return NativeMiniIndexedArray<value::texcoord2h, 2>(*payload);
  if (type_name == "texcoord3h[]") return NativeMiniIndexedArray<value::texcoord3h, 3>(*payload);
  if (type_name == "color3h[]") return NativeMiniIndexedArray<value::color3h, 3>(*payload);
  if (type_name == "color4h[]") return NativeMiniIndexedArray<value::color4h, 4>(*payload);
  if (type_name == "float2") return NativeMiniCompound<float, 2>(*payload);
  if (type_name == "float3") return NativeMiniCompound<float, 3>(*payload);
  if (type_name == "float4") return NativeMiniCompound<float, 4>(*payload);
  if (type_name == "double2") return NativeMiniCompound<double, 2>(*payload);
  if (type_name == "double3") return NativeMiniCompound<double, 3>(*payload);
  if (type_name == "double4") return NativeMiniCompound<double, 4>(*payload);
  if (type_name == "int2") return NativeMiniCompound<int32_t, 2>(*payload);
  if (type_name == "int3") return NativeMiniCompound<int32_t, 3>(*payload);
  if (type_name == "int4") return NativeMiniCompound<int32_t, 4>(*payload);
  if (type_name == "color3f") return NativeMiniIndexedCompound<value::color3f, 3>(*payload);
  if (type_name == "color4f") return NativeMiniIndexedCompound<value::color4f, 4>(*payload);
  if (type_name == "color3d") return NativeMiniIndexedCompound<value::color3d, 3>(*payload);
  if (type_name == "color4d") return NativeMiniIndexedCompound<value::color4d, 4>(*payload);
  if (type_name == "normal3f") return NativeMiniIndexedCompound<value::normal3f, 3>(*payload);
  if (type_name == "vector3f") return NativeMiniIndexedCompound<value::vector3f, 3>(*payload);
  if (type_name == "vector3d") return NativeMiniIndexedCompound<value::vector3d, 3>(*payload);
  if (type_name == "point3f") return NativeMiniIndexedCompound<value::point3f, 3>(*payload);
  if (type_name == "point3d") return NativeMiniIndexedCompound<value::point3d, 3>(*payload);
  if (type_name == "normal3d") return NativeMiniIndexedCompound<value::normal3d, 3>(*payload);
  if (type_name == "texcoord2f") return NativeMiniIndexedCompound<value::texcoord2f, 2>(*payload);
  if (type_name == "texcoord2d") return NativeMiniIndexedCompound<value::texcoord2d, 2>(*payload);
  if (type_name == "texcoord3f") return NativeMiniIndexedCompound<value::texcoord3f, 3>(*payload);
  if (type_name == "texcoord3d") return NativeMiniIndexedCompound<value::texcoord3d, 3>(*payload);
  if (type_name == "vector3h") return NativeMiniIndexedCompound<value::vector3h, 3>(*payload);
  if (type_name == "normal3h") return NativeMiniIndexedCompound<value::normal3h, 3>(*payload);
  if (type_name == "point3h") return NativeMiniIndexedCompound<value::point3h, 3>(*payload);
  if (type_name == "texcoord2h") return NativeMiniIndexedCompound<value::texcoord2h, 2>(*payload);
  if (type_name == "texcoord3h") return NativeMiniIndexedCompound<value::texcoord3h, 3>(*payload);
  if (type_name == "color3h") return NativeMiniIndexedCompound<value::color3h, 3>(*payload);
  if (type_name == "color4h") return NativeMiniIndexedCompound<value::color4h, 4>(*payload);
  if (type_name == "matrix2f") return NativeMiniMatrix<value::matrix2f, 2>(*payload);
  if (type_name == "matrix3f") return NativeMiniMatrix<value::matrix3f, 3>(*payload);
  if (type_name == "matrix4f") return NativeMiniMatrix<value::matrix4f, 4>(*payload);
  if (type_name == "matrix2d") return NativeMiniMatrix<value::matrix2d, 2>(*payload);
  if (type_name == "matrix3d") return NativeMiniMatrix<value::matrix3d, 3>(*payload);
  if (type_name == "matrix4d") return NativeMiniMatrix<value::matrix4d, 4>(*payload);
  if (type_name == "quatf") return NativeMiniQuat<value::quatf>(*payload);
  if (type_name == "quatd") return NativeMiniQuat<value::quatd>(*payload);
  if (type_name == "quath") return NativeMiniQuat<value::quath>(*payload);
  return nonstd::nullopt;
}
} } }
