#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-typed-array-timesamples.h"
#include "lightusd.hh"
#include "timesamples.hh"
#include "typed-array.hh"

#include <vector>
#include <string>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::value;
using namespace lightusd::legacy::value;  // NOLINT

// Helper: test TypedArray dedup for a given type
template<typename T>
static bool run_typed_array_dedup() {
  TimeSamples ts;

  TypedArray<T> arr(100);
  for (size_t i = 0; i < 100; i++) {
    arr[i] = static_cast<T>(i);
  }

  std::string err;
  bool ret = ts.add_array_sample<T>(0.0, arr, &err);
  if (!ret) return false;

  ret = ts.add_array_sample<T>(1.0, arr, &err);
  if (!ret) return false;

  ret = ts.add_array_sample<T>(2.0, arr, &err);
  if (!ret) return false;

  return ts.size() == 3;
}

// Helper: test vector compatibility for a given type
template<typename T>
static bool run_vector_compat() {
  TimeSamples ts;

  std::vector<T> vec(50);
  for (size_t i = 0; i < 50; i++) {
    vec[i] = static_cast<T>(i * 2);
  }

  std::string err;
  bool ret = ts.add_array_sample<T>(0.0, vec, &err);
  if (!ret) return false;

  return ts.size() == 1;
}

// Helper: test scalar values for a given type
template<typename T>
static bool run_scalar_values() {
  TimeSamples ts;

  std::string err;
  T val1 = static_cast<T>(42);
  T val2 = static_cast<T>(84);

  bool ret = ts.add_sample<T>(0.0, val1, &err);
  if (!ret) return false;

  ret = ts.add_sample<T>(1.0, val2, &err);
  if (!ret) return false;

  return ts.size() == 2;
}

void typed_array_dedup_int_test(void) {
  TEST_CHECK(run_typed_array_dedup<int32_t>());
  TEST_MSG("int32_t dedup failed");

  TEST_CHECK(run_typed_array_dedup<uint32_t>());
  TEST_MSG("uint32_t dedup failed");

  TEST_CHECK(run_typed_array_dedup<int64_t>());
  TEST_MSG("int64_t dedup failed");

  TEST_CHECK(run_typed_array_dedup<uint64_t>());
  TEST_MSG("uint64_t dedup failed");
}

void typed_array_dedup_float_double_test(void) {
  TEST_CHECK(run_typed_array_dedup<float>());
  TEST_MSG("float dedup failed");

  TEST_CHECK(run_typed_array_dedup<double>());
  TEST_MSG("double dedup failed");
}

void typed_array_vector_compat_test(void) {
  TEST_CHECK(run_vector_compat<int32_t>());
  TEST_MSG("int32_t vector compat failed");

  TEST_CHECK(run_vector_compat<float>());
  TEST_MSG("float vector compat failed");

  TEST_CHECK(run_vector_compat<double>());
  TEST_MSG("double vector compat failed");
}

void typed_array_scalar_values_test(void) {
  TEST_CHECK(run_scalar_values<int32_t>());
  TEST_MSG("int32_t scalar values failed");

  TEST_CHECK(run_scalar_values<float>());
  TEST_MSG("float scalar values failed");

  TEST_CHECK(run_scalar_values<double>());
  TEST_MSG("double scalar values failed");
}
