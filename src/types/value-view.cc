// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Read-only array views for ::lightusd::Value.

#include "value-view.hh"
#include "c/lightusd-core-c.h"

#include "type-info.hh"
#include "crate/lazy-array.hh"
#include "crate/crate-data-source.hh"

#include <cstdint>
#include <limits>

namespace lightusd {
namespace {

TypeId ScalarComponent(TypeId id) {
  return static_cast<TypeId>(lu_scalar_component(static_cast<uint16_t>(id)));
}

bool RequiresLazyComponentReorder(TypeId id) {
  return lu_requires_lazy_component_reorder(static_cast<uint16_t>(id)) != 0;
}

template <typename T>
bool BorrowLazyFlatArray(const Value& value, TypeId component_type,
                         ArrayView<T>* out) {
  if (!CanBorrowLazyFlat(value)) return false;
  const LazyArrayRef* ref = value.lazy_ref();
  if (!ref || !ref->source || ref->is_compressed || !ref->source->can_borrow())
    return false;
  if (RequiresLazyComponentReorder(ref->value_type)) return false;
  if (ScalarComponent(ref->value_type) != component_type) return false;
  const size_t comps = GetComponentCount(ref->value_type);
  if (comps == 0) return false;
  if (ref->src_elem_stride != comps * sizeof(T)) return false;

  if (ref->element_count == 0) {
    *out = ArrayView<T>{nullptr, 0, true};
    return true;
  }
  const uint64_t header_bytes =
      CrateArrayCountHeaderBytes(ref->source->version());
  if (ref->block_offset == 0 || ref->block_len < header_bytes) return false;
  const uint64_t flat_count = ref->element_count * uint64_t(comps);
  const uint64_t byte_count = flat_count * uint64_t(sizeof(T));
  if (flat_count / comps != ref->element_count) return false;
  if (byte_count / sizeof(T) != flat_count) return false;
  if (flat_count > uint64_t((std::numeric_limits<size_t>::max)())) return false;
  if (byte_count > ref->block_len - header_bytes) return false;
  const uint64_t data_off = ref->block_offset + header_bytes;
  if (data_off < ref->block_offset || data_off > ref->source->size()) return false;
  if (byte_count > ref->source->size() - data_off) return false;
  const uintptr_t addr =
      reinterpret_cast<uintptr_t>(ref->source->base() + data_off);
  if ((addr % alignof(T)) != 0) return false;

  *out = ArrayView<T>{
      reinterpret_cast<const T*>(ref->source->base() + data_off),
      static_cast<size_t>(flat_count),
      true};
  return true;
}

// Scalar byte size / alignment for the borrowable POD component types. Returns 0
// for unsupported types (which are therefore not borrowable).
size_t ScalarByteSize(TypeId comp) {
  return lu_scalar_byte_size(static_cast<uint16_t>(comp));
}

template <typename T, typename U>
bool FinishBorrowedView(ValueArrayView<U> storage, ArrayView<T>* out) {
  if (!storage) return false;
  *out = ArrayView<T>{storage->empty() ? nullptr : storage->data(),
                      storage->size(), true};
  return true;
}

bool FinishFloatBackedView(const Value& value, ArrayView<float>* out) {
  const TypeId component = GetComponentType(value.type_id());
  // Half arrays retain authored types but use widened float storage.
  if (!value.is_array() ||
      (component != TypeId::Float && component != TypeId::Half))
    return false;
  const size_t components = GetComponentCount(value.type_id());
  if (components == 0) return false;
  const float* data = static_cast<const float*>(value.raw_data());
  const size_t elements = value.array_size();
  if (elements > (std::numeric_limits<size_t>::max)() / components)
    return false;
  *out = ArrayView<float>{data, elements * components, true};
  return data != nullptr || elements == 0;
}

}  // namespace

bool CanBorrowLazyFlat(const Value& value) {
  return CanBorrowLazyFlat(value.view());
}

bool CanBorrowLazyFlat(ValueView value) {
  // Mirrors BorrowLazyFlatArray's preconditions WITHOUT materializing: true only
  // when the lazy array's bytes can be aliased zero-copy from the source mapping
  // (so independent element ranges can be formatted concurrently, with no decode).
  if (!value.is_lazy()) return false;
  const uint16_t endian = 1;
  if (*reinterpret_cast<const uint8_t*>(&endian) != 1) return false;
  const LazyArrayRef* ref = value.lazy_ref();
  if (!ref || !ref->source || ref->is_compressed || !ref->source->can_borrow())
    return false;
  if (value.type_id() != ref->value_type ||
      value.array_size() != ref->element_count ||
      ref->element_count > ref->max_elements) return false;
  if (RequiresLazyComponentReorder(ref->value_type)) return false;
  // Decoding canonicalizes every nonzero bool byte to one.
  if (ScalarComponent(ref->value_type) == TypeId::Bool) return false;
  const size_t ts = ScalarByteSize(ScalarComponent(ref->value_type));
  if (ts == 0) return false;
  // Half and uchar arrays widen in canonical Value storage. Their on-disk
  // component widths cannot back a C/Python view of the widened components.
  if (lu_type_size(lu_value_array_component(static_cast<uint16_t>(ref->value_type))) != ts)
    return false;
  const size_t comps = GetComponentCount(ref->value_type);
  if (comps == 0) return false;
  if (ref->src_elem_stride != comps * ts) return false;
  if (ref->element_count == 0) return true;
  if (!ref->source->base()) return false;
  const uint64_t header_bytes =
      CrateArrayCountHeaderBytes(ref->source->version());
  if (ref->block_offset == 0 || ref->block_len < header_bytes) return false;
  const uint64_t flat_count = ref->element_count * uint64_t(comps);
  const uint64_t byte_count = flat_count * uint64_t(ts);
  if (flat_count / comps != ref->element_count) return false;
  if (byte_count / ts != flat_count) return false;
  if (flat_count > uint64_t((std::numeric_limits<size_t>::max)())) return false;
  if (byte_count > ref->block_len - header_bytes) return false;
  const uint64_t data_off = ref->block_offset + header_bytes;
  if (data_off < ref->block_offset || data_off > ref->source->size()) return false;
  if (byte_count > ref->source->size() - data_off) return false;
  const uintptr_t addr =
      reinterpret_cast<uintptr_t>(ref->source->base() + data_off);
  if ((addr % ts) != 0) return false;  // ts == alignof(T) for these PODs
  return true;
}

bool GetFloatArrayView(const Value& value, ArrayScratch<float>* scratch,
                       ArrayView<float>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() ||
      lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_FLOAT)
    return false;
  if (value.is_lazy() && BorrowLazyFlatArray<float>(value, TypeId::Float, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_float_array();
    if (arr) {
      *out = ArrayView<float>{arr->empty() ? nullptr : arr->data(),
                              arr->size(), true};
      return true;
    }
    // USD vector arrays (notably point3f[]) use the same flat float storage
    // as float[], but retain their element type for schema fidelity.
    return FinishFloatBackedView(value, out);
  }
  scratch->materialized = value.materialized_copy();
  if (scratch->materialized.as_float_array()) {
    return FinishBorrowedView(scratch->materialized.as_float_array(), out);
  }
  return FinishFloatBackedView(scratch->materialized, out);
}

bool GetDoubleArrayView(const Value& value, ArrayScratch<double>* scratch,
                        ArrayView<double>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() || lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_DOUBLE) return false;
  if (value.is_lazy() && BorrowLazyFlatArray<double>(value, TypeId::Double, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_double_array();
    if (!arr) return false;
    *out = ArrayView<double>{arr->empty() ? nullptr : arr->data(), arr->size(), true};
    return true;
  }
  scratch->materialized = value.materialized_copy();
  return FinishBorrowedView(scratch->materialized.as_double_array(), out);
}

bool GetIntArrayView(const Value& value, ArrayScratch<int32_t>* scratch,
                     ArrayView<int32_t>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() || lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_INT) return false;
  if (value.is_lazy() && BorrowLazyFlatArray<int32_t>(value, TypeId::Int, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_int_array();
    if (!arr) return false;
    *out = ArrayView<int32_t>{arr->empty() ? nullptr : arr->data(), arr->size(), true};
    return true;
  }
  scratch->materialized = value.materialized_copy();
  return FinishBorrowedView(scratch->materialized.as_int_array(), out);
}

bool GetInt64ArrayView(const Value& value, ArrayScratch<int64_t>* scratch,
                       ArrayView<int64_t>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() || lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_INT64) return false;
  if (value.is_lazy() && BorrowLazyFlatArray<int64_t>(value, TypeId::Int64, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_int64_array();
    if (!arr) return false;
    *out = ArrayView<int64_t>{arr->empty() ? nullptr : arr->data(), arr->size(), true};
    return true;
  }
  scratch->materialized = value.materialized_copy();
  return FinishBorrowedView(scratch->materialized.as_int64_array(), out);
}

bool GetUIntArrayView(const Value& value, ArrayScratch<uint32_t>* scratch,
                      ArrayView<uint32_t>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() || lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_UINT) return false;
  if (value.is_lazy() && BorrowLazyFlatArray<uint32_t>(value, TypeId::UInt, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_uint_array();
    if (!arr) return false;
    *out = ArrayView<uint32_t>{arr->empty() ? nullptr : arr->data(), arr->size(), true};
    return true;
  }
  scratch->materialized = value.materialized_copy();
  return FinishBorrowedView(scratch->materialized.as_uint_array(), out);
}

bool GetUInt64ArrayView(const Value& value, ArrayScratch<uint64_t>* scratch,
                        ArrayView<uint64_t>* out) {
  if (!out || !scratch) return false;
  *out = {};
  if (!value.is_array() || lu_value_array_component(static_cast<uint16_t>(value.type_id())) != LU_TYPE_UINT64) return false;
  if (value.is_lazy() && BorrowLazyFlatArray<uint64_t>(value, TypeId::UInt64, out)) {
    return true;
  }
  if (!value.is_lazy()) {
    auto arr = value.as_uint64_array();
    if (!arr) return false;
    *out = ArrayView<uint64_t>{arr->empty() ? nullptr : arr->data(), arr->size(), true};
    return true;
  }
  scratch->materialized = value.materialized_copy();
  return FinishBorrowedView(scratch->materialized.as_uint64_array(), out);
}

}  // namespace lightusd
