// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita.
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file typed-array.cc
/// @brief TypedArray implementation file
///

#include "typed-array-core.hh"

// Implementation file for TypedArray - since it's header-only template class,
// most functionality is implemented in the header file.
// This file can be used for explicit template instantiations if needed.

namespace lightusd {

// Explicit template instantiations for commonly used types
// This can help reduce compilation time by pre-instantiating frequently used types

// TypedArray (storage class) instantiations
template class TypedArray<uint8_t>;
template class TypedArray<uint16_t>;
template class TypedArray<uint32_t>;
template class TypedArray<int8_t>;
template class TypedArray<int16_t>;
template class TypedArray<int32_t>;
template class TypedArray<float>;
template class TypedArray<double>;

// TypedArrayView instantiations
template class TypedArrayView<uint8_t>;
template class TypedArrayView<const uint8_t>;
template class TypedArrayView<uint16_t>;
template class TypedArrayView<const uint16_t>;
template class TypedArrayView<uint32_t>;
template class TypedArrayView<const uint32_t>;
template class TypedArrayView<int8_t>;
template class TypedArrayView<const int8_t>;
template class TypedArrayView<int16_t>;
template class TypedArrayView<const int16_t>;
template class TypedArrayView<int32_t>;
template class TypedArrayView<const int32_t>;
template class TypedArrayView<float>;
template class TypedArrayView<const float>;
template class TypedArrayView<double>;
template class TypedArrayView<const double>;

} // namespace lightusd
