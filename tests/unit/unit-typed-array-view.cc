#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-typed-array-view.h"
#include "value-types.hh"
#include "timesamples.hh"
#include "typed-array.hh"

#include <vector>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::value;
using namespace lightusd::legacy::value;  // NOLINT

void typed_array_view_binary_float_test(void) {
  TimeSamples samples;

  float data1[] = {1.0f, 2.0f, 3.0f};
  float data2[] = {4.0f, 5.0f, 6.0f};
  float data3[] = {7.0f, 8.0f, 9.0f};

  samples.add_array_sample(1.0, data1, 3);
  samples.add_array_sample(2.0, data2, 3);
  samples.add_array_sample(4.0, data3, 3);

  // Check view at index 0
  {
    auto view = samples.get_typed_array_view_at<float>(0);
    TEST_CHECK(view.size() == 3);
    TEST_MSG("view at index 0: size = %zu, expected 3", view.size());
    TEST_CHECK(view[0] == 1.0f);
    TEST_CHECK(view[1] == 2.0f);
    TEST_CHECK(view[2] == 3.0f);
  }

  // Check view at index 1
  {
    auto view = samples.get_typed_array_view_at<float>(1);
    TEST_CHECK(view.size() == 3);
    TEST_MSG("view at index 1: size = %zu, expected 3", view.size());
    TEST_CHECK(view[0] == 4.0f);
    TEST_CHECK(view[1] == 5.0f);
    TEST_CHECK(view[2] == 6.0f);
  }
}

void typed_array_view_vector_storage_test(void) {
  TimeSamples samples;

  std::vector<double> vec1 = {1.0, 2.0, 3.0};
  std::vector<double> vec2 = {4.0, 5.0, 6.0};
  std::vector<double> vec3 = {7.0, 8.0, 9.0};

  Value v1(vec1);
  Value v2(vec2);
  Value v3(vec3);

  samples.add_sample(1.0, v1);
  samples.add_sample(2.0, v2);
  samples.add_blocked_sample(3.0, v3);
  samples.add_sample(4.0, v3);

  // Check view at index 0
  {
    auto view = samples.get_typed_array_view_at<double>(0);
    TEST_CHECK(view.size() == 3);
    TEST_MSG("view at index 0: size = %zu, expected 3", view.size());
    TEST_CHECK(view[0] == 1.0);
    TEST_CHECK(view[1] == 2.0);
    TEST_CHECK(view[2] == 3.0);
  }

  // Check blocked sample at index 2 returns empty
  {
    auto view = samples.get_typed_array_view_at<double>(2);
    TEST_CHECK(view.empty());
    TEST_MSG("blocked sample at index 2 should be empty");
  }
}

void typed_array_view_at_time_test(void) {
  TimeSamples samples;

  float data1[] = {1.0f, 2.0f, 3.0f};
  float data2[] = {4.0f, 5.0f, 6.0f};
  float data3[] = {7.0f, 8.0f, 9.0f};

  samples.add_array_sample(1.0, data1, 3);
  samples.add_array_sample(2.0, data2, 3);
  samples.add_array_sample(4.0, data3, 3);

  // Existing time 1.0 should work
  {
    auto view = samples.get_typed_array_view_at_time<float>(1.0);
    TEST_CHECK(view.size() == 3);
    TEST_MSG("view at time 1.0: size = %zu, expected 3", view.size());
    TEST_CHECK(view[0] == 1.0f);
  }

  // Non-existent time 3.0 should return empty
  {
    auto view = samples.get_typed_array_view_at_time<float>(3.0);
    TEST_CHECK(view.empty());
    TEST_MSG("view at non-existent time 3.0 should be empty");
  }
}

void typed_array_view_blocked_sample_test(void) {
  TimeSamples samples;

  std::vector<double> vec1 = {1.0, 2.0, 3.0};
  std::vector<double> vec2 = {4.0, 5.0, 6.0};
  std::vector<double> vec3 = {7.0, 8.0, 9.0};

  Value v1(vec1);
  Value v2(vec2);
  Value v3(vec3);

  samples.add_sample(1.0, v1);
  samples.add_sample(2.0, v2);
  samples.add_blocked_sample(3.0, v3);
  samples.add_sample(4.0, v3);

  // Blocked sample at index 2 returns empty view
  {
    auto view = samples.get_typed_array_view_at<double>(2);
    TEST_CHECK(view.empty());
    TEST_MSG("blocked sample at index 2 should return empty view");
  }

  // get_typed_array_view_at_time for time 1.0 should work
  {
    auto view = samples.get_typed_array_view_at_time<double>(1.0);
    TEST_CHECK(view.size() == 3);
    TEST_MSG("view at time 1.0: size = %zu, expected 3", view.size());
    TEST_CHECK(view[0] == 1.0);
  }
}

void typed_array_subspan_checked_test(void) {
  TypedArray<float> arr;
  arr.resize(10);
  for (size_t i = 0; i < 10; i++) arr[i] = float(i);

  // Valid subspan
  {
    auto result = arr.subspan_checked(2, 3);
    TEST_CHECK(result.has_value());
    auto span = result.value();
    TEST_CHECK(span.size() == 3);
    TEST_CHECK(span[0] == 2.0f);
    TEST_CHECK(span[1] == 3.0f);
    TEST_CHECK(span[2] == 4.0f);
  }

  // Valid subspan to end (default count)
  {
    auto result = arr.subspan_checked(8);
    TEST_CHECK(result.has_value());
    auto span = result.value();
    TEST_CHECK(span.size() == 2);
    TEST_CHECK(span[0] == 8.0f);
    TEST_CHECK(span[1] == 9.0f);
  }

  // Out-of-bounds offset (beyond size)
  {
    auto result = arr.subspan_checked(11, 1);
    TEST_CHECK(!result.has_value());
    TEST_CHECK(result.error().find("offset out of range") != std::string::npos);
  }

  // Count exceeds bounds
  {
    auto result = arr.subspan_checked(8, 5);
    TEST_CHECK(!result.has_value());
    TEST_CHECK(result.error().find("count exceeds array bounds") != std::string::npos);
  }

  // Valid empty subspan
  {
    auto result = arr.subspan_checked(5, 0);
    TEST_CHECK(result.has_value());
    auto span = result.value();
    TEST_CHECK(span.size() == 0);
  }

  // const overload
  {
    const TypedArray<float> &carr = arr;
    auto result = carr.subspan_checked(0, 5);
    TEST_CHECK(result.has_value());
    auto span = result.value();
    TEST_CHECK(span.size() == 5);
    TEST_CHECK(span[0] == 0.0f);
  }

  // const out-of-bounds
  {
    const TypedArray<float> &carr = arr;
    auto result = carr.subspan_checked(10, 1);
    TEST_CHECK(!result.has_value());
  }
}
