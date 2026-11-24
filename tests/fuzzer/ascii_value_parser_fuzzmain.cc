// SPDX-License-Identifier: MIT
// Fuzz test for the 21 parse_* functions in tiny-string.cc
//
// Build with: meson + clang -fsanitize=address,fuzzer
// Run:  ./fuzz_ascii_value_parser -max_total_time=60

#include <cstdint>
#include <cstddef>
#include <vector>

#include "tiny-string.hh"
#include "value-types.hh"

using lightusd::legacy::tstring_view;
namespace str = lightusd::legacy::str;

// 64 KB cap to avoid OOM from repeated valid array elements.
static constexpr size_t kMaxInputSize = 64u * 1024u;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // Need at least the selector byte.
  if (size < 1) return 0;

  // Cap total input size.
  if (size > kMaxInputSize) return 0;

  const uint8_t selector = data[0] % 21;
  const char *payload = reinterpret_cast<const char *>(data + 1);
  const size_t payload_len = size - 1;

  tstring_view sv(payload, payload_len);

  switch (selector) {
    // --- Scalar parsers (0-5) ---
    case 0: {
      int32_t v;
      (void)str::parse_int(sv, &v);
      break;
    }
    case 1: {
      int64_t v;
      (void)str::parse_int64(sv, &v);
      break;
    }
    case 2: {
      uint32_t v;
      (void)str::parse_uint(sv, &v);
      break;
    }
    case 3: {
      uint64_t v;
      (void)str::parse_uint64(sv, &v);
      break;
    }
    case 4: {
      float v;
      (void)str::parse_float(sv, &v);
      break;
    }
    case 5: {
      double v;
      (void)str::parse_double(sv, &v);
      break;
    }

    // --- Scalar array parsers (6-8) ---
    case 6: {
      std::vector<int32_t> v;
      (void)str::parse_int_array(sv, &v);
      break;
    }
    case 7: {
      std::vector<float> v;
      (void)str::parse_float_array(sv, &v);
      break;
    }
    case 8: {
      std::vector<double> v;
      (void)str::parse_double_array(sv, &v);
      break;
    }

    // --- Vector array parsers (9-14) ---
    case 9: {
      std::vector<lightusd::legacy::value::float2> v;
      (void)str::parse_float2_array(sv, &v);
      break;
    }
    case 10: {
      std::vector<lightusd::legacy::value::float3> v;
      (void)str::parse_float3_array(sv, &v);
      break;
    }
    case 11: {
      std::vector<lightusd::legacy::value::float4> v;
      (void)str::parse_float4_array(sv, &v);
      break;
    }
    case 12: {
      std::vector<lightusd::legacy::value::double2> v;
      (void)str::parse_double2_array(sv, &v);
      break;
    }
    case 13: {
      std::vector<lightusd::legacy::value::double3> v;
      (void)str::parse_double3_array(sv, &v);
      break;
    }
    case 14: {
      std::vector<lightusd::legacy::value::double4> v;
      (void)str::parse_double4_array(sv, &v);
      break;
    }

    // --- Matrix array parsers (15-20) ---
    case 15: {
      std::vector<lightusd::legacy::value::matrix2f> v;
      (void)str::parse_matrix2f_array(sv, &v);
      break;
    }
    case 16: {
      std::vector<lightusd::legacy::value::matrix3f> v;
      (void)str::parse_matrix3f_array(sv, &v);
      break;
    }
    case 17: {
      std::vector<lightusd::legacy::value::matrix4f> v;
      (void)str::parse_matrix4f_array(sv, &v);
      break;
    }
    case 18: {
      std::vector<lightusd::legacy::value::matrix2d> v;
      (void)str::parse_matrix2d_array(sv, &v);
      break;
    }
    case 19: {
      std::vector<lightusd::legacy::value::matrix3d> v;
      (void)str::parse_matrix3d_array(sv, &v);
      break;
    }
    case 20: {
      std::vector<lightusd::legacy::value::matrix4d> v;
      (void)str::parse_matrix4d_array(sv, &v);
      break;
    }
  }

  return 0;
}
