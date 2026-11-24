// SPDX-License-Identifier: Apache 2.0
// Copyrigh 2022 - Present, Light Transport Entertainment Inc.
#pragma once

#include <iostream>
#include <limits>
#include <cmath>

namespace lightusd_test {

template <typename T>
static bool float_equals(T x, T y, T eps = std::numeric_limits<T>::epsilon()) {
  if (std::fabs(x - y) < eps) {
    return true;
  }

  return false;
}

// Simple function to check if two float array values are same.
// Assume two arrays have same length.
template <typename T>
static bool float_array_equals(const T *a, const T *b, const int n) {
  for (int i = 0; i < n; i++) {
    if (!float_equals(a[i], b[i])) {
      std::cerr << "float diff. a[" << i << "] = " << a[i] << ", b[" << i
                << "] = " << b[i] << std::endl;
      return false;
    }
  }

  return true;
}

}  // namespace lightusd_test

// Forward declarations for test helpers
namespace lightusd { namespace legacy {
class Stage;
class Layer;
}  // namespace legacy
}

#include <cstring>
#include "lightusd.hh"

namespace lightusd_test {

/// Parse USDA string into a Stage. Returns true on success.
static inline bool parse_usda_to_stage(const char *usda, lightusd::legacy::Stage *stage,
                                        std::string *warn = nullptr,
                                        std::string *err = nullptr) {
  std::string w, e;
  if (!warn) warn = &w;
  if (!err) err = &e;
  return lightusd::legacy::LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", stage, warn, err);
}

/// Parse USDA string into a Layer. Returns true on success.
static inline bool parse_usda_to_layer(const char *usda, lightusd::legacy::Layer *layer,
                                        std::string *warn = nullptr,
                                        std::string *err = nullptr) {
  std::string w, e;
  if (!warn) warn = &w;
  if (!err) err = &e;
  return lightusd::legacy::LoadLayerFromMemory(
      reinterpret_cast<const uint8_t *>(usda), std::strlen(usda),
      "test.usda", layer, warn, err);
}

}  // namespace lightusd_test
