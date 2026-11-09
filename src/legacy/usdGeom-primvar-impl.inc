// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Light Transport Entertainment Inc.
//
// GeomPrimvar template bodies + helpers, split out of usdGeom.cc so the ~47-type
// flatten_with_indices / get_value instantiation matrix can be distributed across
// sibling usdGeom-primvar-inst-*.cc TUs (parallel compile, lower peak per TU).
//
// #included INSIDE `namespace lightusd` by usdGeom.cc (for the in-file
// ExpandWithIndices use in GeomMesh::get_normals) and by each usdGeom-primvar-inst-*.cc.
// The instantiations themselves live in the inst TUs; usdGeom.hh's
// EXTERN_TEMPLATE_GET_VALUE suppresses implicit instantiation everywhere else.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-template"
#endif

namespace {
// --- GeomPrimvar index-expansion helpers (internal linkage per TU) ---
// Helper trait: can use data() pointer (excludes std::vector<bool>)
template <typename T>
struct can_use_data_ptr : std::integral_constant<bool,
    !std::is_same<T, bool>::value> {};

// Helper trait: can use memcpy for block copy
template <typename T>
struct can_use_memcpy : std::integral_constant<bool,
    std::is_trivially_copyable<T>::value && !std::is_same<T, bool>::value> {};

// Block copy with memcpy for trivially copyable types
template <typename T>
inline typename std::enable_if<can_use_memcpy<T>::value>::type
CopyBlockElements(T* dest, const T* src, size_t count) {
  size_t byte_count;
  if (!safe::mul(count, sizeof(T), &byte_count)) {
    // Overflow detected - zero out dest to be safe
    memset(dest, 0, 0);  // Will likely segfault if count was huge, but prevents memcpy with overflowed value
    return;
  }
  memcpy(dest, src, byte_count);
}

// Block copy with loop for non-trivially copyable types
template <typename T>
inline typename std::enable_if<!can_use_memcpy<T>::value>::type
CopyBlockElements(T* dest, const T* src, size_t count) {
  for (size_t k = 0; k < count; k++) {
    dest[k] = src[k];
  }
}

///
/// Core expand-by-indices implementation working on raw pointers.
/// Avoids copies from the source data.
///
///  for i in len(indices):
///    for k in elementSize:
///      dest[i*elementSize + k] = srcData[indices[i]*elementSize + k]
///
/// When `indices` is empty, copies srcData to dest directly.
///
/// Optimized version for types that support data() (not std::vector<bool>)
///
template <typename T>
typename std::enable_if<can_use_data_ptr<T>::value, nonstd::expected<bool, std::string>>::type
ExpandWithIndicesFromPtr(
    const T *srcData, size_t srcSize, uint32_t elementSize,
    const std::vector<int32_t> &indices,
    std::vector<T> *dest) {
  if (!dest) {
    return nonstd::make_unexpected("`dest` is nullptr.");
  }

  if (indices.empty()) {
    dest->assign(srcData, srcData + srcSize);
    return true;
  }

  if (elementSize == 0) {
    return false;
  }

  if ((srcSize % elementSize) != 0) {
    return false;
  }

  // Check for overflow before resize
  size_t resize_size;
  if (!safe::mul(indices.size(), elementSize, &resize_size)) {
    return nonstd::make_unexpected("Integer overflow in ExpandWithIndices: indices.size() * elementSize");
  }
  dest->resize(resize_size);

  std::vector<size_t> invalidIndices;
  const size_t numIndices = indices.size();
  T* destData = dest->data();

   // Fast path for elementSize == 1 (most common case)
   if (elementSize == 1) {
     for (size_t i = 0; i < numIndices; i++) {
       int32_t idx = indices[i];
       if ((idx >= 0) && (size_t(idx) < srcSize)) {
         destData[i] = srcData[idx];
       } else {
         invalidIndices.push_back(i);
       }
     }
   }
   // Optimized path for elementSize > 1
   else {
     for (size_t i = 0; i < numIndices; i++) {
       int32_t idx = indices[i];
       if (idx >= 0) {
         size_t needed;
         if (safe::mul(size_t(idx) + 1, size_t(elementSize), &needed) && (needed <= srcSize)) {
           CopyBlockElements(destData + i * elementSize, srcData + size_t(idx) * elementSize, elementSize);
         } else {
           invalidIndices.push_back(i);
         }
       } else {
         invalidIndices.push_back(i);
       }
     }
   }

  if (invalidIndices.size()) {
    return nonstd::make_unexpected(
        "Invalid indices found: " +
        value::print_array_snipped(invalidIndices,
                                   /* N to display */ 5));
  }

  return true;
}

/// Vector convenience wrapper — delegates to pointer-based version for
/// types with data(), falls back to element-by-element for std::vector<bool>.
template <typename T>
nonstd::expected<bool, std::string>
ExpandWithIndices(
    const std::vector<T> &values, uint32_t elementSize, const std::vector<int32_t> &indices,
    std::vector<T> *dest) {
  if constexpr (can_use_data_ptr<T>::value) {
    return ExpandWithIndicesFromPtr(values.data(), values.size(), elementSize, indices, dest);
  } else {
    // std::vector<bool> fallback: element-by-element (no data() pointer)
    if (!dest) {
      return nonstd::make_unexpected("`dest` is nullptr.");
    }

    if (indices.empty()) {
      (*dest) = values;
      return true;
    }

    if (elementSize == 0) {
      return false;
    }

    if ((values.size() % elementSize) != 0) {
      return false;
    }

    // Check for overflow before resize
    size_t resize_size;
    if (!safe::mul(indices.size(), elementSize, &resize_size)) {
      return nonstd::make_unexpected("Integer overflow in ExpandWithIndices: indices.size() * elementSize");
    }
    dest->resize(resize_size);

    std::vector<size_t> invalidIndices;
    const size_t numValues = values.size();
    const size_t numIndices = indices.size();

    for (size_t i = 0; i < numIndices; i++) {
      int32_t idx = indices[i];
      if (idx >= 0) {
        size_t needed;
        if (safe::mul(size_t(idx) + 1, size_t(elementSize), &needed) && (needed <= numValues)) {
          for (size_t k = 0; k < elementSize; k++) {
            (*dest)[i*elementSize + k] = values[size_t(idx)*elementSize + k];
          }
        } else {
          invalidIndices.push_back(i);
        }
      } else {
        invalidIndices.push_back(i);
      }
    }

    if (invalidIndices.size()) {
      return nonstd::make_unexpected(
          "Invalid indices found: " +
          value::print_array_snipped(invalidIndices,
                                     /* N to display */ 5));
    }

    return true;
  }
}
}  // namespace

// --- GeomPrimvar::flatten_with_indices<T> / get_value<T> template bodies ---
template <typename T>
bool GeomPrimvar::flatten_with_indices(const double t, std::vector<T> *dest, const value::TimeSampleInterpolationType tinterp, std::string *err) const {
  if (!dest) {
    if (err) { (*err) += "Output value is nullptr."; }
    return false;
  }

  if (!(_attr.has_timesamples() || _attr.has_value())) {
    if (err) {
      (*err) += fmt::format("Attribute `{}` has no value or timesamples.",
                            _attr.type_name());
    }
    return false;
  }

  // A ValueBlock (`= None`) declares the type but carries no usable data. Return
  // false with NO error so callers treat it as "no data" and skip (rather than
  // emitting a misleading "type requested, but Attribute is type" failure).
  if (_attr.is_blocked()) {
    return false;
  }

  if (!IsSupportedGeomPrimvarType(_attr.type_id())) {
    if (err) {
      (*err) += fmt::format("Unsupported type for GeomPrimvar. type = `{}`",
                            _attr.type_name());
    }
    return false;
  }

  const uint32_t elementSize = get_elementSize();
  std::vector<int32_t> indices_buf;
  const auto &indices = resolve_indices_at(t, tinterp, indices_buf);

  // === Fast path: default time, no timesamples → zero-copy TypedArrayView ===
  if constexpr (std::is_trivially_copyable<T>::value && !std::is_same<T, bool>::value) {
    if (value::TimeCode(t).is_default() && !_attr.has_timesamples()) {
      TypedArrayView<const T> view = _attr.get_value_view<T>();
      if (!view.empty()) {
        auto ret = ExpandWithIndicesFromPtr(view.data(), view.size(), elementSize, indices, dest);
        if (ret) { return true; }
        if (err) {
          (*err) += fmt::format("[Internal Error] Failed to expand for GeomPrimvar type = `{}`", _attr.type_name());
          (*err) += "\n" + ret.error();
        }
        return false;
      }
    }
  }

  // === Standard path: get_value (copies from attribute storage) ===
  std::vector<T> value;
  if (!_attr.get_value<std::vector<T>>(t, &value, tinterp)) {
    if (err) {
      (*err) += fmt::format("`{}[]` type requested, but Attribute is type `{}`",
                            value::TypeTraits<T>::type_name(), _attr.type_name());
    }
    return false;
  }

  if (value.empty()) {
    return false;  // Empty authored array (OpenUSD compat)
  }

  if (indices.empty()) {
    (*dest) = std::move(value);
    return true;
  }

  auto ret = ExpandWithIndices(value, elementSize, indices, dest);
  if (ret) { return true; }
  if (err) {
    (*err) += fmt::format("[Internal Error] Failed to expand for GeomPrimvar type = `{}`", _attr.type_name());
    (*err) += "\n" + ret.error();
  }
  return false;
}

template <typename T>
bool GeomPrimvar::flatten_with_indices(std::vector<T> *dest, std::string *err) const {
  return flatten_with_indices(value::TimeCode::Default(), dest, value::TimeSampleInterpolationType::Linear, err);
}

// Thin forwarders over the non-template get_value(value::Value*) cores
// (usdGeom.cc): the value/timesamples extraction is emitted once, not per T.
template <typename T>
bool GeomPrimvar::get_value(T *dest, std::string *err) const {
  static_assert(lightusd::value::TypeTraits<T>::type_id() != value::TypeTraits<value::token>::type_id(), "`token` type is not supported as a GeomPrimvar");
  static_assert(lightusd::value::TypeTraits<T>::type_id() != value::TypeTraits<std::vector<value::token>>::type_id(), "`token[]` type is not supported as a GeomPrimvar");

  if (!dest) {
    if (err) {
      (*err) += "Output value is nullptr.";
    }
    return false;
  }

  value::Value v;
  if (!get_value(&v, err)) {
    return false;
  }
  if (const T *pv = v.as<T>()) {
    (*dest) = *pv;
    return true;
  }
  if (err) {
    (*err) += fmt::format("Attribute value type mismatch. Requested type `{}` but Attribute has type `{}`",
                          value::TypeTraits<T>::type_name(), _attr.type_name());
  }
  return false;
}

template <typename T>
bool GeomPrimvar::get_value(double timecode, T *dest, value::TimeSampleInterpolationType interp, std::string *err) const {
  static_assert(lightusd::value::TypeTraits<T>::type_id() != value::TypeTraits<value::token>::type_id(), "`token` type is not supported as a GeomPrimvar");
  static_assert(lightusd::value::TypeTraits<T>::type_id() != value::TypeTraits<std::vector<value::token>>::type_id(), "`token[]` type is not supported as a GeomPrimvar");

  if (!dest) {
    if (err) {
      (*err) += "Output value is nullptr.";
    }
    return false;
  }

  value::Value v;
  if (!get_value(timecode, &v, interp, err)) {
    return false;
  }
  if (const T *pv = v.as<T>()) {
    (*dest) = *pv;
    return true;
  }
  if (err) {
    (*err) += fmt::format("Get Attribute value at time {} failed. Maybe type mismatch?. Requested type `{}` but Attribute has type `{}`",
                          timecode, value::TypeTraits<T>::type_name(), _attr.type_name());
  }
  return false;
}

#pragma GCC diagnostic pop
#ifdef __clang__
#pragma clang diagnostic pop
#endif
