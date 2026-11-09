#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-timesamples.h"
#include "core/prim.hh"
#include "../../src/timesamples.hh"
#include "core/attribute.hh"
#include "core/animatable.hh"
#include "math-util.inc"

using namespace lightusd;

void timesamples_test(void) {

  {
    value::token tok1("bora");
    value::token tok2("muda");

    Animatable<value::token> toks;
    toks.add_sample(0, tok1);
    toks.add_sample(10, tok2);

    {
      value::token tok;
      TEST_CHECK(toks.get(value::TimeCode::Default(), &tok));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(tok.str() == "bora");
    }

    // Held interpolation
    {
      value::token tok;
      TEST_CHECK(toks.get(0.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(-1.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(1.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(10.0, &tok));
      TEST_CHECK(tok.str() == "muda");

      TEST_CHECK(toks.get(1000.0, &tok));
      TEST_CHECK(tok.str() == "muda");
    }
  }

  {
    Animatable<float> samples;
    samples.add_sample(0, 0.0f);
    samples.add_sample(1, 10.0f);

    {
      float f;
      TEST_CHECK(samples.get(value::TimeCode::Default(), &f));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(math::is_close(f, 0.0f));
    }

    // Linear interpolation
    {
      float f;
      TEST_CHECK(samples.get(0.0, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(samples.get(0.5, &f));
      TEST_CHECK(math::is_close(f, 5.0f));

      TEST_CHECK(samples.get(1.0, &f));
      TEST_CHECK(math::is_close(f, 10.0f));

    }
  }

  {
    primvar::PrimVar pbar;
    value::TimeSamples ts;
    ts.add_sample(0, value::Value(0.0f));
    ts.add_sample(1, value::Value(10.0f));
    pbar.set_timesamples(ts);
    pbar.set_value(2000.0f); // default value

    {
      float f;
      TEST_CHECK(pbar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Held, &f));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(math::is_close(f, 2000.0f));
    }

    // Linear interpolation
    {
      float f;
      TEST_CHECK(pbar.get_interpolated_value(-10.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(pbar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(pbar.get_interpolated_value(0.5, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 5.0f));

      TEST_CHECK(pbar.get_interpolated_value(1.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 10.0f));

      TEST_CHECK(pbar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 2000.0f));
    }
  }

  {
    primvar::PrimVar pbar;
    value::TimeSamples ts;
    ts.add_sample(0, value::Value(0.0f));
    ts.add_sample(1, value::Value(10.0f));
    pbar.set_timesamples(ts);
    pbar.set_value(2000.0f); // default value

    Attribute attr;
    attr.set_var(pbar);

    {
      float f;
      TEST_CHECK(attr.get(value::TimeCode::Default(), &f, value::TimeSampleInterpolationType::Held));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(math::is_close(f, 2000.0f));
    }

    // Linear interpolation
    {
      float f;
      TEST_CHECK(attr.get(-10.0, &f, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(attr.get(0.0, &f, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(attr.get(0.5, &f, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(math::is_close(f, 5.0f));

      TEST_CHECK(attr.get(1.0, &f, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(math::is_close(f, 10.0f));

      TEST_CHECK(attr.get(value::TimeCode::Default(), &f, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(math::is_close(f, 2000.0f));
    }
  }

  {
    primvar::PrimVar pbar;
    value::TimeSamples ts;
    std::vector<value::float2> ts0 = {{0.0f, 5.0f}};
    std::vector<value::float2> ts1 = {{10.0f, 15.0f}};

    ts.add_sample(0, ts0);
    ts.add_sample(1, ts1);
    pbar.set_timesamples(ts);
    std::vector<value::float2> default_value = {{100.0f, 200.0f}};
    pbar.set_value(default_value); // default value

    Attribute attr;
    attr.set_var(pbar);

    {
      std::vector<value::float2> v;
      TEST_CHECK(attr.get(value::TimeCode::Default(), &v, value::TimeSampleInterpolationType::Held));
      TEST_CHECK(v.size() == 1);

      TEST_CHECK(math::is_close(v[0][0], 100.0f));
      TEST_CHECK(math::is_close(v[0][1], 200.0f));
    }

    // Linear interpolation
    {
      std::vector<value::float2> vs;
      TEST_CHECK(attr.get(0.0, &vs, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(vs.size() == 1);
      TEST_CHECK(math::is_close(vs[0][0], 0.0f));
      TEST_CHECK(math::is_close(vs[0][1], 5.0f));

      TEST_CHECK(attr.get(0.5, &vs, value::TimeSampleInterpolationType::Linear));
      TEST_CHECK(vs.size() == 1);
      TEST_CHECK(math::is_close(vs[0][0], 5.0f));
      TEST_CHECK(math::is_close(vs[0][1], 10.0f));

    }
  }

  {
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<value::float2>::type_id()));
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<std::vector<value::float2>>::type_id()));
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<value::texcoord2f>::type_id()));
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<std::vector<value::texcoord2f>>::type_id()));
    TEST_CHECK(!value::IsLerpSupportedType(value::TypeTraits<int>::type_id()));
    TEST_CHECK(!value::IsLerpSupportedType(value::TypeTraits<std::vector<int>>::type_id()));
    TEST_CHECK(!value::IsLerpSupportedType(value::TypeTraits<std::string>::type_id()));
    TEST_CHECK(!value::IsLerpSupportedType(value::TypeTraits<std::vector<std::string>>::type_id()));

    // Matrices are interpolatable and now have a consistent Lerp() arm. Previously
    // IsLerpSupportedType() reported true but Lerp() had no matrix case and returned
    // false (so array matrix timesamples failed to interpolate). Guard the fix.
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<value::matrix4d>::type_id()));
    TEST_CHECK(value::IsLerpSupportedType(value::TypeTraits<std::vector<value::matrix4d>>::type_id()));
    {
      value::matrix4d ma{};
      value::matrix4d mb{};
      value::Value a(ma), b(mb), out;
      TEST_CHECK(value::Lerp(a, b, 0.5, &out));
      TEST_CHECK(out.as<value::matrix4d>() != nullptr);
    }
  }

  // Test TimeSamples sorting
  {
    value::TimeSamples ts;

    // Add samples out of order
    ts.add_sample(5.0, value::Value(50.0f));
    ts.add_sample(2.0, value::Value(20.0f));
    ts.add_sample(8.0, value::Value(80.0f));
    ts.add_sample(1.0, value::Value(10.0f));

    // Force sorting by accessing samples
    const auto& samples = ts.get_samples();

    // Verify sorted order
    TEST_CHECK(samples.size() == 4);
    TEST_CHECK(math::is_close(samples[0].t, 1.0));
    TEST_CHECK(math::is_close(samples[1].t, 2.0));
    TEST_CHECK(math::is_close(samples[2].t, 5.0));
    TEST_CHECK(math::is_close(samples[3].t, 8.0));

    // Verify values are correctly sorted with times
    {
      const float* f1 = samples[0].value.as<float>();
      TEST_CHECK(f1 != nullptr);
      if (f1) TEST_CHECK(math::is_close(*f1, 10.0f));
    }
    {
      const float* f2 = samples[1].value.as<float>();
      TEST_CHECK(f2 != nullptr);
      if (f2) TEST_CHECK(math::is_close(*f2, 20.0f));
    }
    {
      const float* f3 = samples[2].value.as<float>();
      TEST_CHECK(f3 != nullptr);
      if (f3) TEST_CHECK(math::is_close(*f3, 50.0f));
    }
    {
      const float* f4 = samples[3].value.as<float>();
      TEST_CHECK(f4 != nullptr);
      if (f4) TEST_CHECK(math::is_close(*f4, 80.0f));
    }
  }

  // Test TimeSamples with blocked (None) values
  {
    value::TimeSamples ts;

    // Add mix of regular and blocked samples using ValueBlock
    ts.add_sample(0.0, value::Value(10.0));
    ts.add_sample(1.0, value::Value(20.0));
    ts.add_sample(2.0, value::Value(value::ValueBlock()));  // blocked sample
    ts.add_sample(3.0, value::Value(30.0));
    ts.add_sample(4.0, value::Value(value::ValueBlock()));  // blocked sample
    ts.add_sample(5.0, value::Value(50.0));

    const auto& samples = ts.get_samples();

    TEST_CHECK(samples.size() == 6);

    // Check blocked flags
    TEST_CHECK(samples[0].blocked == false);
    TEST_CHECK(samples[1].blocked == false);
    TEST_CHECK(samples[2].blocked == true);
    TEST_CHECK(samples[3].blocked == false);
    TEST_CHECK(samples[4].blocked == true);
    TEST_CHECK(samples[5].blocked == false);

    // Verify values for non-blocked samples
    {
      const double* v = samples[0].value.as<double>();
      TEST_CHECK(v != nullptr);
      if (v) TEST_CHECK(math::is_close(*v, 10.0));
    }
    {
      const double* v = samples[1].value.as<double>();
      TEST_CHECK(v != nullptr);
      if (v) TEST_CHECK(math::is_close(*v, 20.0));
    }
    {
      const double* v = samples[3].value.as<double>();
      TEST_CHECK(v != nullptr);
      if (v) TEST_CHECK(math::is_close(*v, 30.0));
    }
    {
      const double* v = samples[5].value.as<double>();
      TEST_CHECK(v != nullptr);
      if (v) TEST_CHECK(math::is_close(*v, 50.0));
    }
  }

  // Test sorting with blocked values
  {
    value::TimeSamples ts;

    // Add samples out of order with blocked values
    ts.add_sample(5.0, value::Value(50.0));
    ts.add_sample(2.0, value::Value(value::ValueBlock()));
    ts.add_sample(1.0, value::Value(10.0));
    ts.add_sample(4.0, value::Value(value::ValueBlock()));
    ts.add_sample(3.0, value::Value(30.0));

    const auto& samples = ts.get_samples();

    // Verify sorted order
    TEST_CHECK(samples.size() == 5);
    TEST_CHECK(math::is_close(samples[0].t, 1.0));
    TEST_CHECK(math::is_close(samples[1].t, 2.0));
    TEST_CHECK(math::is_close(samples[2].t, 3.0));
    TEST_CHECK(math::is_close(samples[3].t, 4.0));
    TEST_CHECK(math::is_close(samples[4].t, 5.0));

    // Verify blocked flags after sorting
    TEST_CHECK(samples[0].blocked == false);
    TEST_CHECK(samples[1].blocked == true);
    TEST_CHECK(samples[2].blocked == false);
    TEST_CHECK(samples[3].blocked == true);
    TEST_CHECK(samples[4].blocked == false);
  }

  // Test dedup array sorting with index remap
  {
    value::TimeSamples ts;

    // Build nearly-sorted samples so insertion sort would be chosen without
    // dedup-aware fallback.
    for (size_t i = 0; i < 25; ++i) {
      float v = static_cast<float>(i);
      TEST_CHECK(ts.add_array_sample<float>(static_cast<double>(i), &v, 1));
    }

    // Add a sample whose value matches sample at index 20 (value=20.0f).
    float dup_val = 20.0f;
    TEST_CHECK(ts.add_array_sample<float>(12.5, &dup_val, 1));

    std::vector<float> out;
    TEST_CHECK(ts.get_vector_at_time<float>(12.5, &out));
    TEST_CHECK(out.size() == 1);
    if (out.size() == 1) {
      TEST_CHECK(math::is_close(out[0], 20.0f));
    }
  }

  // Empty array samples may use vector::data() == nullptr, but a non-empty
  // sample must provide storage. Rejected input must not add a partial sample.
  {
    value::TimeSamples ts;
    std::string err;
    TEST_CHECK(ts.add_array_sample<float>(0.0, nullptr, 0, &err));
    TEST_CHECK(ts.size() == 1);

    err.clear();
    TEST_CHECK(!ts.add_array_sample<float>(1.0, nullptr, 1, &err));
    TEST_CHECK(err.find("values is null") != std::string::npos);
    TEST_CHECK(ts.size() == 1);

    if ((std::numeric_limits<size_t>::max)() >
        (std::numeric_limits<uint32_t>::max)()) {
      const size_t excessive_count =
          static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) + 1;
      const float dummy = 0.0f;
      err.clear();
      TEST_CHECK(!ts.add_array_sample<float>(1.0, &dummy, excessive_count,
                                             &err));
      TEST_CHECK(err.find("element count") != std::string::npos);
      TEST_CHECK(ts.size() == 1);
    }
  }

  // Unified small-binary storage should keep values aligned with sorted times.
  {
    value::TimeSamples ts;

    TEST_CHECK(ts.add_sample<float>(5.0, 50.0f));
    TEST_CHECK(ts.add_blocked_sample<float>(3.0));
    TEST_CHECK(ts.add_sample<float>(1.0, 10.0f));
    TEST_CHECK(ts.add_sample<float>(4.0, 40.0f));

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 4);
    TEST_CHECK(math::is_close(samples[0].t, 1.0));
    TEST_CHECK(math::is_close(samples[1].t, 3.0));
    TEST_CHECK(math::is_close(samples[2].t, 4.0));
    TEST_CHECK(math::is_close(samples[3].t, 5.0));
    TEST_CHECK(samples[1].blocked == true);

    const float* v0 = samples[0].value.as<float>();
    const float* v2 = samples[2].value.as<float>();
    const float* v3 = samples[3].value.as<float>();
    TEST_CHECK(v0 != nullptr);
    TEST_CHECK(v2 != nullptr);
    TEST_CHECK(v3 != nullptr);
    if (v0) TEST_CHECK(math::is_close(*v0, 10.0f));
    if (v2) TEST_CHECK(math::is_close(*v2, 40.0f));
    if (v3) TEST_CHECK(math::is_close(*v3, 50.0f));

    TEST_CHECK(ts.has_sample_at(4.0));
    {
      // value at the exact sample time t=4.0
      float sample_value = 0.0f;
      TEST_CHECK(ts.get<float>(&sample_value, 4.0,
                               value::TimeSampleInterpolationType::Held));
      TEST_CHECK(math::is_close(sample_value, 40.0f));
    }

    auto value_opt = ts.get_value(2);
    TEST_CHECK(value_opt.has_value());
    if (value_opt.has_value()) {
      const float* sample_value = value_opt.value().as<float>();
      TEST_CHECK(sample_value != nullptr);
      if (sample_value) TEST_CHECK(math::is_close(*sample_value, 40.0f));
    }

    // Reconstruct once, append again, and verify the cache is invalidated.
    TEST_CHECK(ts.add_sample<float>(2.0, 20.0f));
    const auto& updated = ts.get_samples();
    TEST_CHECK(updated.size() == 5);
    TEST_CHECK(math::is_close(updated[0].t, 1.0));
    TEST_CHECK(math::is_close(updated[1].t, 2.0));
    const float* v1 = updated[1].value.as<float>();
    TEST_CHECK(v1 != nullptr);
    if (v1) TEST_CHECK(math::is_close(*v1, 20.0f));
  }

  // Blocked-only generic samples should allow deferred type initialization.
  {
    value::TimeSamples ts;
    std::string err;

    TEST_CHECK(ts.add_blocked_sample(5.0, value::Value(), &err));
    TEST_CHECK(ts.type_id() == 0);
    TEST_CHECK(ts.add_sample(10.0, value::Value(42.0f), &err));
    TEST_CHECK(ts.type_id() == value::TypeTraits<float>::type_id());

    const auto &samples = ts.get_samples();
    TEST_CHECK(samples.size() == 2);
    if (samples.size() == 2) {
      TEST_CHECK(samples[0].blocked == true);
      TEST_CHECK(samples[1].blocked == false);
      const float *v = samples[1].value.as<float>();
      TEST_CHECK(v != nullptr);
      if (v) TEST_CHECK(math::is_close(*v, 42.0f));
    }
  }

  // Scalar and array unified backends must not be mixed.
  {
    value::TimeSamples ts;
    std::string err;

    TEST_CHECK(ts.add_sample<float>(0.0, 1.0f, &err));
    TEST_CHECK(!ts.add_array_sample<float>(1.0, std::vector<float>{1.0f, 2.0f}, &err));
    TEST_CHECK(!err.empty());
  }

  // In the flat buffer design, std::vector and TypedArray share the same binary storage.
  // Mixing them is allowed since the underlying data layout is identical.
  {
    value::TimeSamples ts;
    std::string err;

    TEST_CHECK(ts.add_array_sample<float>(0.0, std::vector<float>{1.0f, 2.0f}, &err));
    TypedArray<float> typed = {3.0f, 4.0f};
    TEST_CHECK(ts.add_array_sample<float>(1.0, typed, &err));
    TEST_CHECK(ts.size() == 2);
  }

  // The raw-pointer overload is public too; it must not bypass type checking
  // and reinterpret a later sample using a new element size.
  {
    value::TimeSamples ts;
    std::string err;
    const float f = 1.0f;
    const int32_t i = 2;

    TEST_CHECK(ts.add_array_sample<float>(0.0, &f, 1, &err));
    TEST_CHECK(!ts.add_array_sample<int32_t>(1.0, &i, 1, &err));
    TEST_CHECK(!err.empty());
    TEST_CHECK(ts.size() == 1);
  }

  // Variable-sized unified arrays should preserve per-sample element counts.
  {
    value::TimeSamples ts;

    TEST_CHECK(ts.add_array_sample<float>(5.0, std::vector<float>{5.0f, 6.0f, 7.0f, 8.0f}));
    TEST_CHECK(ts.add_array_sample<float>(1.0, std::vector<float>{1.0f, 2.0f}));
    TEST_CHECK(ts.add_array_sample<float>(3.0, std::vector<float>{3.0f, 4.0f, 5.0f}));

    (void)ts.get_time(0);

    // Array counts differ per sample (2, 3, 4), so no uniform size
    TEST_CHECK(ts.get_array_count(0) == 2);
    TEST_CHECK(ts.get_array_count(1) == 3);
    TEST_CHECK(ts.get_array_count(2) == 4);

    std::vector<float> out;
    TEST_CHECK(ts.get_vector_at_time<float>(1.0, &out));
    TEST_CHECK(out.size() == 2);
    if (out.size() == 2) {
      TEST_CHECK(math::is_close(out[0], 1.0f));
      TEST_CHECK(math::is_close(out[1], 2.0f));
    }

    TEST_CHECK(ts.get_vector_at_time<float>(3.0, &out));
    TEST_CHECK(out.size() == 3);
    if (out.size() == 3) {
      TEST_CHECK(math::is_close(out[0], 3.0f));
      TEST_CHECK(math::is_close(out[2], 5.0f));
    }

    TEST_CHECK(ts.get_vector_at_time<float>(5.0, &out));
    TEST_CHECK(out.size() == 4);
    if (out.size() == 4) {
      TEST_CHECK(math::is_close(out[0], 5.0f));
      TEST_CHECK(math::is_close(out[3], 8.0f));
    }

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 3);
    if (samples.size() == 3) {
      const auto* first = samples[0].value.as<std::vector<float>>();
      const auto* last = samples[2].value.as<std::vector<float>>();
      TEST_CHECK(first != nullptr);
      TEST_CHECK(last != nullptr);
      if (first) {
        TEST_CHECK(first->size() == 2);
        if (first->size() == 2) {
          TEST_CHECK(math::is_close((*first)[0], 1.0f));
          TEST_CHECK(math::is_close((*first)[1], 2.0f));
        }
      }
      if (last) {
        TEST_CHECK(last->size() == 4);
        if (last->size() == 4) {
          TEST_CHECK(math::is_close((*last)[0], 5.0f));
          TEST_CHECK(math::is_close((*last)[3], 8.0f));
        }
      }
    }

    auto value_opt = ts.get_value(1);
    TEST_CHECK(value_opt.has_value());
    if (value_opt.has_value()) {
      const auto* middle = value_opt.value().as<std::vector<float>>();
      TEST_CHECK(middle != nullptr);
      if (middle) {
        TEST_CHECK(middle->size() == 3);
        if (middle->size() == 3) {
          TEST_CHECK(math::is_close((*middle)[0], 3.0f));
          TEST_CHECK(math::is_close((*middle)[2], 5.0f));
        }
      }
    }
  }

  // Out-of-order arrays must preserve per-sample counts after sorting.
  {
    value::TimeSamples ts;

    TEST_CHECK(ts.add_array_sample<float>(5.0, std::vector<float>{5.0f, 6.0f, 7.0f}));
    TEST_CHECK(ts.add_array_sample<float>(1.0, std::vector<float>{1.0f}));
    TEST_CHECK(ts.add_array_sample<float>(3.0, std::vector<float>{5.0f, 6.0f, 7.0f}));

    const auto &samples = ts.get_samples();
    TEST_CHECK(samples.size() == 3);
    TEST_CHECK(ts.get_array_count(0) == 1);
    TEST_CHECK(ts.get_array_count(1) == 3);
    TEST_CHECK(ts.get_array_count(2) == 3);

    std::vector<float> out;
    TEST_CHECK(ts.get_vector_at_time<float>(3.0, &out));
    TEST_CHECK(out.size() == 3);
    if (out.size() == 3) {
      TEST_CHECK(math::is_close(out[0], 5.0f));
      TEST_CHECK(math::is_close(out[2], 7.0f));
    }
  }

  // Generic blocked and non-blocked array samples should preserve array values and block state.
  {
    value::TimeSamples ts;
    std::string err;

    TEST_CHECK(ts.add_sample(3.0, value::Value(std::vector<float>{3.0f, 4.0f}), &err));
    TEST_CHECK(ts.add_blocked_sample(1.0, value::Value(std::vector<float>{}), &err));
    TEST_CHECK(ts.add_sample(5.0, value::Value(std::vector<float>{5.0f, 6.0f, 7.0f}), &err));

    const auto &samples = ts.get_samples();
    TEST_CHECK(samples.size() == 3);
    if (samples.size() == 3) {
      TEST_CHECK(samples[0].blocked == true);
      TEST_CHECK(samples[1].blocked == false);
      TEST_CHECK(samples[2].blocked == false);
    }

    std::vector<float> out;
    bool blocked = false;
    TEST_CHECK(!ts.get_vector_at<float>(0, &out, &blocked));
    TEST_CHECK(blocked == true);
    TEST_CHECK(ts.get_vector_at<float>(1, &out, &blocked));
    TEST_CHECK(blocked == false);
    TEST_CHECK(out.size() == 2);
  }

  // TypedArray-backed unified storage should keep the TypedArray type id and counts.
  {
    value::TimeSamples ts;

    TypedArray<float> late = {7.0f, 8.0f};
    TypedArray<float> early = {1.0f, 2.0f, 3.0f};

    TEST_CHECK(ts.add_array_sample<float>(5.0, late));
    TEST_CHECK(ts.add_array_sample<float>(1.0, early));

    TEST_CHECK(ts.is_array());
    TEST_CHECK(ts.type_id() == value::TypeTraits<TypedArray<float>>::type_id());

    TypedArray<float> out;
    TEST_CHECK(ts.get_typed_array_at_time<float>(1.0, &out));
    TEST_CHECK(out.size() == 3);
    if (out.size() == 3) {
      TEST_CHECK(math::is_close(out[0], 1.0f));
      TEST_CHECK(math::is_close(out[2], 3.0f));
    }

    auto view = ts.get_typed_array_view_at<float>(1);
    TEST_CHECK(view.size() == 2);
    if (view.size() == 2) {
      TEST_CHECK(math::is_close(view[0], 7.0f));
      TEST_CHECK(math::is_close(view[1], 8.0f));
    }

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 2);
    if (samples.size() == 2) {
      const auto* first = samples[0].value.as<TypedArray<float>>();
      const auto* second = samples[1].value.as<TypedArray<float>>();
      TEST_CHECK(first != nullptr);
      TEST_CHECK(second != nullptr);
      if (first) {
        TEST_CHECK(first->size() == 3);
        if (first->size() == 3) {
          TEST_CHECK(math::is_close((*first)[0], 1.0f));
          TEST_CHECK(math::is_close((*first)[2], 3.0f));
        }
      }
      if (second) {
        TEST_CHECK(second->size() == 2);
        if (second->size() == 2) {
          TEST_CHECK(math::is_close((*second)[0], 7.0f));
          TEST_CHECK(math::is_close((*second)[1], 8.0f));
        }
      }
    }

    auto value_opt = ts.get_value(0);
    TEST_CHECK(value_opt.has_value());
    if (value_opt.has_value()) {
      const auto* first = value_opt.value().as<TypedArray<float>>();
      TEST_CHECK(first != nullptr);
      if (first) {
        TEST_CHECK(first->size() == 3);
      }
    }
  }

  // Empty TypedArray samples should round-trip through unified reconstruction.
  {
    value::TimeSamples ts;

    TypedArray<float> empty;
    TEST_CHECK(ts.add_array_sample<float>(2.0, empty));
    TEST_CHECK(ts.is_array());
    TEST_CHECK(ts.size() == 1);
    TEST_CHECK(ts.get_array_count(0) == 0);

    TypedArray<float> out;
    bool blocked = true;
    TEST_CHECK(ts.get_typed_array_at_time<float>(2.0, &out, &blocked));
    TEST_CHECK(blocked == false);
    TEST_CHECK(out.size() == 0);

    auto view = ts.get_typed_array_view_at<float>(0);
    TEST_CHECK(view.size() == 0);

    auto value_opt = ts.get_value(0);
    TEST_CHECK(value_opt.has_value());
    if (value_opt.has_value()) {
      const auto *typed = value_opt.value().as<TypedArray<float>>();
      TEST_CHECK(typed != nullptr);
      if (typed) {
        TEST_CHECK(typed->size() == 0);
      }
    }
  }

  // Bool scalars should stay on the generic Value path instead of binary storage.
  {
    value::TimeSamples ts;
    std::string err;

    TEST_CHECK(ts.add_sample<bool>(3.0, true, &err));
    TEST_CHECK(ts.add_blocked_sample<bool>(1.0, &err));
    TEST_CHECK(!ts.is_using_binary_storage());
    TEST_CHECK(ts.type_id() == value::TypeTraits<bool>::type_id());

    bool out = false;
    TEST_CHECK(ts.get(&out, 3.0, value::TimeSampleInterpolationType::Held));
    TEST_CHECK(out == true);

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 2);
    if (samples.size() == 2) {
      TEST_CHECK(samples[0].blocked == true);
      TEST_CHECK(samples[1].blocked == false);
      const bool* bool_value = samples[1].value.as<bool>();
      TEST_CHECK(bool_value != nullptr);
      if (bool_value) {
        TEST_CHECK(*bool_value == true);
      }
    }
  }

  // Bool arrays should use the generic Value path and generic dedup.
  {
    value::TimeSamples ts;
    std::string err;
    std::vector<bool> authored = {true, false, true};

    TEST_CHECK(ts.add_array_sample<bool>(1.0, authored, &err));
    TEST_CHECK(ts.add_sample(3.0, value::Value(authored), &err));
    TEST_CHECK(!ts.is_using_binary_storage());
    TEST_CHECK(ts.type_id() == value::TypeTraits<std::vector<bool>>::type_id());

    std::vector<bool> out;
    bool blocked = true;
    TEST_CHECK(ts.get_vector_at_time<bool>(1.0, &out, &blocked));
    TEST_CHECK(blocked == false);
    TEST_CHECK(out.size() == 3);
    if (out.size() == 3) {
      TEST_CHECK(out[0] == true);
      TEST_CHECK(out[1] == false);
      TEST_CHECK(out[2] == true);
    }

    TEST_CHECK(ts.get_vector_at_time<bool>(3.0, &out, &blocked));
    TEST_CHECK(blocked == false);
    TEST_CHECK(out.size() == 3);
    if (out.size() == 3) {
      TEST_CHECK(out[0] == true);
      TEST_CHECK(out[1] == false);
      TEST_CHECK(out[2] == true);
    }
  }

  // Value-array storage should sort times and keep per-sample array sizes aligned.
  {
    value::TimeSamples ts;

    TEST_CHECK(ts.add_array_sample<float>(5.0, std::vector<float>{5.0f, 6.0f, 7.0f, 8.0f}));
    TEST_CHECK(ts.add_array_sample<float>(1.0, std::vector<float>{1.0f, 2.0f}));
    TEST_CHECK(ts.add_array_sample<float>(3.0, std::vector<float>{1.0f, 2.0f}));

    (void)ts.get_samples();

    TEST_CHECK(ts.get_array_count(0) == 2);
    TEST_CHECK(ts.get_array_count(1) == 2);
    TEST_CHECK(ts.get_array_count(2) == 4);

    std::vector<float> out;
    TEST_CHECK(ts.get_vector_at_time<float>(1.0, &out));
    TEST_CHECK(out.size() == 2);
    TEST_CHECK(ts.get_vector_at_time<float>(3.0, &out));
    TEST_CHECK(out.size() == 2);
    if (out.size() == 2) {
      TEST_CHECK(math::is_close(out[0], 1.0f));
      TEST_CHECK(math::is_close(out[1], 2.0f));
    }
    TEST_CHECK(ts.get_vector_at_time<float>(5.0, &out));
    TEST_CHECK(out.size() == 4);

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 3);
    TEST_CHECK(math::is_close(samples[0].t, 1.0));
    TEST_CHECK(math::is_close(samples[1].t, 3.0));
    TEST_CHECK(math::is_close(samples[2].t, 5.0));
  }


  // Test empty TimeSamples
  {
    value::TimeSamples ts;
    TEST_CHECK(ts.empty() == true);
    TEST_CHECK(ts.size() == 0);

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 0);
  }

  // Test single sample
  {
    value::TimeSamples ts;
    ts.add_sample(5.0, value::Value(50.0));

    TEST_CHECK(ts.empty() == false);
    TEST_CHECK(ts.size() == 1);

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 1);
    TEST_CHECK(math::is_close(samples[0].t, 5.0));

    const double* v = samples[0].value.as<double>();
    TEST_CHECK(v != nullptr);
    if (v) TEST_CHECK(math::is_close(*v, 50.0));
  }

  // Test has_sample_at and get_sample_at
  {
    value::TimeSamples ts;
    ts.add_sample(1.0, value::Value(10.0f));
    ts.add_sample(2.0, value::Value(20.0f));
    ts.add_sample(3.0, value::Value(30.0f));

    TEST_CHECK(ts.has_sample_at(1.0) == true);
    TEST_CHECK(ts.has_sample_at(2.0) == true);
    TEST_CHECK(ts.has_sample_at(3.0) == true);
    TEST_CHECK(ts.has_sample_at(1.5) == false);
    TEST_CHECK(ts.has_sample_at(4.0) == false);

    {
      TEST_CHECK(ts.has_sample_at(2.0));
      float v = 0.0f;
      TEST_CHECK(ts.get<float>(&v, 2.0,
                               value::TimeSampleInterpolationType::Held));
      TEST_CHECK(math::is_close(v, 20.0f));
    }

    // No sample exactly at t=4.0.
    TEST_CHECK(ts.has_sample_at(4.0) == false);
  }

  // Test get_time API
  {
    value::TimeSamples ts;
    ts.add_sample(5.0, value::Value(50.0));
    ts.add_sample(2.0, value::Value(20.0));
    ts.add_sample(8.0, value::Value(80.0));

    // Get times at indices (after sorting)
    auto t0 = ts.get_time(0);
    TEST_CHECK(t0.has_value());
    if (t0.has_value()) {
      TEST_CHECK(math::is_close(t0.value(), 2.0));
    }

    auto t1 = ts.get_time(1);
    TEST_CHECK(t1.has_value());
    if (t1.has_value()) {
      TEST_CHECK(math::is_close(t1.value(), 5.0));
    }

    auto t2 = ts.get_time(2);
    TEST_CHECK(t2.has_value());
    if (t2.has_value()) {
      TEST_CHECK(math::is_close(t2.value(), 8.0));
    }

    // Out of bounds
    auto t3 = ts.get_time(3);
    TEST_CHECK(!t3.has_value());
  }



  // Test duplicate time entries (std::stable_sort preserves order)
  {
    value::TimeSamples ts;
    ts.add_sample(1.0, value::Value(10.0f));
    ts.add_sample(2.0, value::Value(20.0f));
    ts.add_sample(1.0, value::Value(15.0f)); // Duplicate time

    const auto& samples = ts.get_samples();
    TEST_CHECK(samples.size() == 3);
    TEST_CHECK(math::is_close(samples[0].t, 1.0));
    TEST_CHECK(math::is_close(samples[1].t, 1.0));
    TEST_CHECK(math::is_close(samples[2].t, 2.0));
    const float* v0 = samples[0].value.as<float>();
    const float* v1 = samples[1].value.as<float>();
    TEST_CHECK(v0 != nullptr);
    TEST_CHECK(v1 != nullptr);
    if (v0) TEST_CHECK(math::is_close(*v0, 10.0f));
    if (v1) TEST_CHECK(math::is_close(*v1, 15.0f));
  }

  // Test interpolation of arrays with different sizes
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    std::vector<float> v1 = {1.0f, 2.0f};
    std::vector<float> v2 = {3.0f, 4.0f, 5.0f};
    ts.add_sample(0.0, value::Value(v1));
    ts.add_sample(1.0, value::Value(v2));
    pvar.set_timesamples(ts);

    value::Value result_val;
    // Linear interpolation should fail because array sizes are different,
    // and it should return the value of the lower sample (held interpolation).
    TEST_CHECK(pvar.get_interpolated_value(0.5, value::TimeSampleInterpolationType::Linear, &result_val) == true);
    const std::vector<float> *result = result_val.as<std::vector<float>>();
    TEST_CHECK(result != nullptr);
    if (result) {
        TEST_CHECK(result->size() == 2);
        TEST_CHECK(math::is_close((*result)[0], 1.0f));
        TEST_CHECK(math::is_close((*result)[1], 2.0f));
    }
  }

  // ==========================================================================
  // OpenUSD Behavior Compatibility Tests
  // ==========================================================================
  // These tests ensure LightUSD matches OpenUSD's timeSamples evaluation behavior

  // Test 1: Single TimeSample Behavior (should be held constant for all times)
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    value::float3 scale_value = {0.1f, 0.2f, 0.3f};
    ts.add_sample(0.0, value::Value(scale_value));
    pvar.set_timesamples(ts);

    value::float3 result;

    // Test before the sample (t = -10)
    TEST_CHECK(pvar.get_interpolated_value(-10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));

    // Test at the sample (t = 0)
    TEST_CHECK(pvar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));

    // Test after the sample (t = 10)
    TEST_CHECK(pvar.get_interpolated_value(10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));
  }

  // Test 2: Default Value vs TimeSamples Coexistence
  // Default value should be returned for Default TimeCode,
  // TimeSamples should be used for numeric time codes
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    value::float3 sample_value = {0.1f, 0.2f, 0.3f};
    value::float3 default_value = {7.0f, 8.0f, 9.0f};

    ts.add_sample(0.0, value::Value(sample_value));
    pvar.set_timesamples(ts);
    pvar.set_value(default_value);  // Set default value

    value::float3 result;

    // Test Default TimeCode - should return default value (7, 8, 9)
    TEST_CHECK(pvar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));

    // Test numeric time codes - should use time samples
    TEST_CHECK(pvar.get_interpolated_value(-10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));

    TEST_CHECK(pvar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));

    TEST_CHECK(pvar.get_interpolated_value(10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.2f));
    TEST_CHECK(math::is_close(result[2], 0.3f));
  }

  // Test 3: Multiple TimeSamples with Linear Interpolation
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    value::float3 sample1 = {0.1f, 0.1f, 0.1f};
    value::float3 sample2 = {0.5f, 0.5f, 0.5f};
    value::float3 sample3 = {1.0f, 1.0f, 1.0f};

    ts.add_sample(-5.0, value::Value(sample1));
    ts.add_sample(0.0, value::Value(sample2));
    ts.add_sample(5.0, value::Value(sample3));
    pvar.set_timesamples(ts);

    value::float3 result;

    // Test before first sample (t = -10) - should hold first value
    TEST_CHECK(pvar.get_interpolated_value(-10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));
    TEST_CHECK(math::is_close(result[1], 0.1f));
    TEST_CHECK(math::is_close(result[2], 0.1f));

    // Test at first sample (t = -5)
    TEST_CHECK(pvar.get_interpolated_value(-5.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.1f));

    // Test between samples (t = -2.5) - should interpolate
    TEST_CHECK(pvar.get_interpolated_value(-2.5, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.3f));  // Linear interpolation between 0.1 and 0.5
    TEST_CHECK(math::is_close(result[1], 0.3f));
    TEST_CHECK(math::is_close(result[2], 0.3f));

    // Test at middle sample (t = 0)
    TEST_CHECK(pvar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.5f));

    // Test between samples (t = 2.5) - should interpolate
    TEST_CHECK(pvar.get_interpolated_value(2.5, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 0.75f));  // Linear interpolation between 0.5 and 1.0
    TEST_CHECK(math::is_close(result[1], 0.75f));
    TEST_CHECK(math::is_close(result[2], 0.75f));

    // Test at last sample (t = 5)
    TEST_CHECK(pvar.get_interpolated_value(5.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 1.0f));

    // Test after last sample (t = 10) - should hold last value
    TEST_CHECK(pvar.get_interpolated_value(10.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 1.0f));
    TEST_CHECK(math::is_close(result[1], 1.0f));
    TEST_CHECK(math::is_close(result[2], 1.0f));
  }

  // Test 4: Attribute::get() with Default Value and TimeSamples
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    value::float3 sample1 = {0.1f, 0.1f, 0.1f};
    value::float3 sample2 = {0.5f, 0.5f, 0.5f};
    value::float3 sample3 = {1.0f, 1.0f, 1.0f};
    value::float3 default_value = {7.0f, 8.0f, 9.0f};

    ts.add_sample(-5.0, value::Value(sample1));
    ts.add_sample(0.0, value::Value(sample2));
    ts.add_sample(5.0, value::Value(sample3));
    pvar.set_timesamples(ts);
    pvar.set_value(default_value);

    Attribute attr;
    attr.set_var(pvar);

    value::float3 result;

    // Test Default TimeCode via Attribute::get()
    TEST_CHECK(attr.get(value::TimeCode::Default(), &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));

    // Test numeric time codes via Attribute::get()
    TEST_CHECK(attr.get(-10.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 0.1f));  // Before samples, held constant

    TEST_CHECK(attr.get(-2.5, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 0.3f));  // Interpolated

    TEST_CHECK(attr.get(0.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 0.5f));  // At sample

    TEST_CHECK(attr.get(2.5, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 0.75f));  // Interpolated

    TEST_CHECK(attr.get(10.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 1.0f));  // After samples, held constant
  }

  // Test 5: Default Value Only (no time samples)
  {
    primvar::PrimVar pvar;
    value::float3 default_value = {7.0f, 8.0f, 9.0f};
    pvar.set_value(default_value);  // Only default value, no time samples

    Attribute attr;
    attr.set_var(pvar);

    value::float3 result;

    // All time codes should return the default value when no time samples exist
    TEST_CHECK(attr.get(value::TimeCode::Default(), &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));

    TEST_CHECK(attr.get(-10.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));

    TEST_CHECK(attr.get(0.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));

    TEST_CHECK(attr.get(10.0, &result, value::TimeSampleInterpolationType::Linear));
    TEST_CHECK(math::is_close(result[0], 7.0f));
    TEST_CHECK(math::is_close(result[1], 8.0f));
    TEST_CHECK(math::is_close(result[2], 9.0f));
  }

  // Test 6: Held Interpolation Mode
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    value::float3 sample1 = {1.0f, 1.0f, 1.0f};
    value::float3 sample2 = {2.0f, 2.0f, 2.0f};
    value::float3 sample3 = {3.0f, 3.0f, 3.0f};

    ts.add_sample(0.0, value::Value(sample1));
    ts.add_sample(5.0, value::Value(sample2));
    ts.add_sample(10.0, value::Value(sample3));
    pvar.set_timesamples(ts);

    value::float3 result;

    // With Held interpolation, values between samples should hold the earlier sample
    TEST_CHECK(pvar.get_interpolated_value(2.5, value::TimeSampleInterpolationType::Held, &result));
    TEST_CHECK(math::is_close(result[0], 1.0f));  // Should hold earlier value

    TEST_CHECK(pvar.get_interpolated_value(7.5, value::TimeSampleInterpolationType::Held, &result));
    TEST_CHECK(math::is_close(result[0], 2.0f));  // Should hold earlier value

    // At exact sample times
    TEST_CHECK(pvar.get_interpolated_value(5.0, value::TimeSampleInterpolationType::Held, &result));
    TEST_CHECK(math::is_close(result[0], 2.0f));  // Exact value at sample
  }

  // Test 7: Edge Cases - Empty TimeSamples with Default Value
  {
    primvar::PrimVar pvar;
    value::float3 default_value = {100.0f, 200.0f, 300.0f};
    pvar.set_value(default_value);  // Default value only

    // TimeSamples exist but are empty
    value::TimeSamples ts;
    pvar.set_timesamples(ts);

    value::float3 result;

    // Should still return default value for all time codes
    TEST_CHECK(pvar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 100.0f));
    TEST_CHECK(math::is_close(result[1], 200.0f));
    TEST_CHECK(math::is_close(result[2], 300.0f));

    TEST_CHECK(pvar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result[0], 100.0f));
  }

  // Test 8: Test Boundary Conditions with Epsilon Values
  {
    primvar::PrimVar pvar;
    value::TimeSamples ts;
    ts.add_sample(0.0, value::Value(10.0f));
    ts.add_sample(1.0, value::Value(20.0f));
    pvar.set_timesamples(ts);

    float result;
    const float epsilon = 1e-6f;

    // Just before and after samples
    TEST_CHECK(pvar.get_interpolated_value(0.0 - epsilon, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result, 10.0f, 1e-3f));  // Should be very close to first sample

    TEST_CHECK(pvar.get_interpolated_value(1.0 + epsilon, value::TimeSampleInterpolationType::Linear, &result));
    TEST_CHECK(math::is_close(result, 20.0f, 1e-3f));  // Should be very close to last sample
  }

  // ----------------------------------------------------------------------
  // [Phase 1] Parity guard: the production TimeSamples::get<T>() (now the
  // binary-direct evaluator for scalar T) must match a naive value::Value-path
  // reference across {default, held, lerp} x {blocked variants}. The reference
  // is the generic get_samples()/Lerp path that get<T>() used to inline; this
  // guards that the consolidated non-template core stayed behavior-compatible.
  // (Note: the scenarios below deliberately avoid matrix/timecode types, whose
  // Value-path Lerp arm is missing — the binary core interpolates them, so the
  // two paths intentionally diverge there until Phase 2 unifies Lerp.)
  // ----------------------------------------------------------------------
  {
    using Interp = value::TimeSampleInterpolationType;
    const Interp Held = Interp::Held;
    const Interp Linear = Interp::Linear;

    // Independent reference: interpolate via the generic get_samples()/Lerp path.
    auto reference_get = [&](const value::TimeSamples &ts, double t, Interp interp,
                             auto tag, bool *ok) {
      using T = decltype(tag);
      using Sample = value::TimeSamples::Sample;
      T out{};
      *ok = false;
      const auto &samples = ts.get_samples();
      if (samples.empty()) return out;
      if (value::TimeCode(t).is_default()) {
        for (const auto &s : samples) {
          if (!s.blocked) {
            if (const T *pv = s.value.template as<T>()) { out = *pv; *ok = true; }
            return out;
          }
        }
        return out;
      }
      if (samples.size() == 1) {
        if (samples[0].blocked) return out;
        if (const T *pv = samples[0].value.template as<T>()) { out = *pv; *ok = true; }
        return out;
      }
      if (interp == Linear && value::IsLerpSupportedType(ts.type_id())) {
        auto it = std::lower_bound(samples.begin(), samples.end(), t,
            [](const Sample &a, double tv) { return a.t < tv; });
        const auto itm1 = (it == samples.begin()) ? samples.begin() : (it - 1);
        const size_t i0 = size_t(std::max<int64_t>(0, std::min<int64_t>(
            int64_t(samples.size()) - 1,
            int64_t(std::distance(samples.begin(), itm1)))));
        const size_t i1 = size_t(std::max<int64_t>(0, std::min<int64_t>(
            int64_t(samples.size()) - 1, int64_t(i0) + 1)));
        if (samples[i0].blocked && samples[i1].blocked) return out;
        if (samples[i0].blocked) {
          if (const T *pv = samples[i1].value.template as<T>()) { out = *pv; *ok = true; }
          return out;
        }
        if (samples[i1].blocked) {
          if (const T *pv = samples[i0].value.template as<T>()) { out = *pv; *ok = true; }
          return out;
        }
        const double tl = samples[i0].t, tu = samples[i1].t;
        double dt = (std::fabs(tu - tl) < std::numeric_limits<double>::epsilon())
                        ? 0.0 : (t - tl) / (tu - tl);
        dt = std::max(0.0, std::min(1.0, dt));
        value::Value p;
        if (value::Lerp(samples[i0].value, samples[i1].value, dt, &p)) {
          if (const T *pv = p.template as<T>()) { out = *pv; *ok = true; }
        }
        return out;
      }
      auto it = std::upper_bound(samples.begin(), samples.end(), t,
          [](double tv, const Sample &a) { return tv < a.t; });
      const auto ith = (it == samples.begin()) ? samples.begin() : (it - 1);
      if (ith->blocked) return out;
      if (const T *pv = ith->value.template as<T>()) { out = *pv; *ok = true; }
      return out;
    };

    auto check = [&](const value::TimeSamples &ts, double t, Interp interp,
                     auto tag) {
      using T = decltype(tag);
      T a{};
      const bool ra = ts.template get<T>(&a, t, interp);
      bool rb = false;
      const T b = reference_get(ts, t, interp, tag, &rb);
      TEST_CHECK(ra == rb);
      if (ra && rb) {
        TEST_CHECK(a == b);
      }
    };

    const double T_DEFAULT = value::TimeCode::Default();
    const double times[] = {-1.0, 0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 100.0, T_DEFAULT};

    // float (lerp scalar), two samples
    {
      value::TimeSamples ts;
      ts.add_sample<float>(0.0, 1.0f);
      ts.add_sample<float>(2.0, 5.0f);
      for (double t : times) { check(ts, t, Held, float{}); check(ts, t, Linear, float{}); }
    }
    // double, single sample
    {
      value::TimeSamples ts;
      ts.add_sample<double>(5.0, 42.0);
      for (double t : times) { check(ts, t, Held, double{}); check(ts, t, Linear, double{}); }
    }
    // float3 (lerp vector)
    {
      value::TimeSamples ts;
      ts.add_sample<value::float3>(0.0, value::float3{0.f, 0.f, 0.f});
      ts.add_sample<value::float3>(2.0, value::float3{2.f, 4.f, 6.f});
      for (double t : times) { check(ts, t, Held, value::float3{}); check(ts, t, Linear, value::float3{}); }
    }
    // color3f (role type — shares float3 layout via shared switch arm)
    {
      value::TimeSamples ts;
      ts.add_sample<value::color3f>(0.0, value::color3f{0.f, 0.f, 0.f});
      ts.add_sample<value::color3f>(1.0, value::color3f{1.f, 0.5f, 0.25f});
      for (double t : times) { check(ts, t, Held, value::color3f{}); check(ts, t, Linear, value::color3f{}); }
    }
    // int32 (non-lerp scalar): Linear must behave as Held
    {
      value::TimeSamples ts;
      ts.add_sample<int32_t>(0.0, 3);
      ts.add_sample<int32_t>(2.0, 9);
      for (double t : times) { check(ts, t, Held, int32_t{}); check(ts, t, Linear, int32_t{}); }
    }
    // blocked middle (double)
    {
      value::TimeSamples ts;
      ts.add_sample<double>(0.0, 10.0);
      ts.add_blocked_sample<double>(1.0);
      ts.add_sample<double>(2.0, 20.0);
      for (double t : times) { check(ts, t, Held, double{}); check(ts, t, Linear, double{}); }
    }
    // blocked endpoints (float)
    {
      value::TimeSamples ts;
      ts.add_blocked_sample<float>(0.0);
      ts.add_sample<float>(1.0, 7.0f);
      ts.add_blocked_sample<float>(2.0);
      for (double t : times) { check(ts, t, Held, float{}); check(ts, t, Linear, float{}); }
    }
    // dedup: duplicate_sample shares the same _data offset (zero-copy)
    {
      value::TimeSamples ts;
      ts.add_sample<value::float3>(0.0, value::float3{1.f, 2.f, 3.f});
      ts.duplicate_sample(0, 5.0);
      for (double t : times) { check(ts, t, Held, value::float3{}); check(ts, t, Linear, value::float3{}); }
    }
    // generic (non-binary) storage: token
    {
      value::TimeSamples ts;
      ts.add_sample<value::token>(0.0, value::token("a"));
      ts.add_sample<value::token>(2.0, value::token("b"));
      for (double t : times) { check(ts, t, Held, value::token{}); check(ts, t, Linear, value::token{}); }
    }
  }
}

void timesamples_variable_array_inmemory_test(void) {
  // Variable per-sample counts in binary storage.
  value::TimeSamples ts;
  std::vector<float> a = {1.0f, 2.0f, 3.0f};
  std::vector<float> b = {4.0f};
  std::vector<float> c = {};  // empty (will be blocked via array-blocked)
  ts.add_array_sample<float>(2.0, a.data(), a.size());
  ts.add_array_sample<float>(0.0, b.data(), b.size());  // out of order
  ts.add_array_blocked_sample<float>(1.0);
  ts.add_array_sample<float>(3.0, nullptr, 0);  // empty non-blocked

  // Out-of-order insertion must sort without error.
  ts.update();
  TEST_CHECK(!ts.has_error());
  TEST_CHECK(ts.size() == 4);

  // Counts aligned with sorted times.
  TEST_CHECK(ts.get_array_count(0) == 1);  // t=0 -> b (size1)
  TEST_CHECK(ts.get_array_count(1) == 0);  // t=1 -> blocked
  TEST_CHECK(ts.get_array_count(2) == 3);  // t=2 -> a
  TEST_CHECK(ts.get_array_count(3) == 0);  // t=3 -> empty non-blocked

  // Blocked flag.
  value::TimeSamples::Sample s;
  TEST_CHECK(ts.get_sample_at(1, &s) && s.blocked);

  // duplicate_sample propagates count.
  TEST_CHECK(ts.duplicate_sample(2, 2.5));
  ts.update();
  // find index for 2.5
  size_t idx = (size_t)-1;
  for (size_t i = 0; i < ts.size(); ++i) {
    auto t = ts.get_time(i);
    if (t && *t == 2.5) { idx = i; break; }
  }
  TEST_CHECK(idx != (size_t)-1);
  if (idx != (size_t)-1) TEST_CHECK(ts.get_array_count(idx) == 3);

  // has_error stays false.
  TEST_CHECK(!ts.has_error());
}
