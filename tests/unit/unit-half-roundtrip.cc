// SPDX-License-Identifier: Apache 2.0
// Half-precision float print->parse roundtrip test
//
// Verifies that dtos(half) produces strings that parse back to the same
// binary half value for all 65536 possible half-precision values.

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-half-roundtrip.h"
#include "str-util.hh"
#include "value-types.hh"
#include <cmath>
#include <cstdlib>
#include <cstdint>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Test that all half values round-trip correctly through dtos()
void half_roundtrip_exhaustive_test(void) {
  int errors = 0;
  int tested = 0;

  // Test all 65536 possible half values
  for (uint32_t bits = 0; bits < 65536; bits++) {
    lightusd::legacy::value::half h;
    h.value = static_cast<uint16_t>(bits);

    float f = lightusd::legacy::value::half_to_float(h);

    // Skip inf/nan (they have special string representations)
    if (!std::isfinite(f)) continue;

    tested++;

    // Convert half to string using LightUSD
    std::string s = dtos(h);

    // Parse back to float and convert to half
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);

    if (h2.value != h.value) {
      errors++;
      if (errors <= 5) {
        TEST_MSG("Roundtrip error: 0x%04x = %g -> \"%s\" -> %g -> 0x%04x",
                 bits, f, s.c_str(), parsed, h2.value);
      }
    }
  }

  TEST_MSG("Tested %d half values", tested);
  TEST_CHECK_(errors == 0, "All %d half values should roundtrip (errors=%d)", tested, errors);
}

// Test specific edge cases
void half_roundtrip_edge_cases_test(void) {
  // Test +0
  {
    lightusd::legacy::value::half h;
    h.value = 0x0000;  // +0
    std::string s = dtos(h);
    TEST_CHECK_(s == "0", "+0 should format as \"0\", got \"%s\"", s.c_str());
  }

  // Test -0
  {
    lightusd::legacy::value::half h;
    h.value = 0x8000;  // -0
    std::string s = dtos(h);
    TEST_CHECK_(s == "-0", "-0 should format as \"-0\", got \"%s\"", s.c_str());
  }

  // Test 1.0
  {
    lightusd::legacy::value::half h;
    h.value = 0x3C00;  // 1.0
    std::string s = dtos(h);
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "1.0 should roundtrip: 0x%04x -> \"%s\" -> 0x%04x",
                h.value, s.c_str(), h2.value);
  }

  // Test value 1.2 (0x3CCD) - common test case
  {
    lightusd::legacy::value::half h;
    h.value = 0x3CCD;  // ~1.2001953
    std::string s = dtos(h);

    // Verify round-trip
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "1.2 should roundtrip: 0x%04x -> \"%s\" -> 0x%04x",
                h.value, s.c_str(), h2.value);
  }

  // Test smallest positive denormal
  {
    lightusd::legacy::value::half h;
    h.value = 0x0001;  // smallest positive denormal
    std::string s = dtos(h);
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "smallest denormal should roundtrip: 0x%04x -> \"%s\" -> 0x%04x",
                h.value, s.c_str(), h2.value);
  }

  // Test max half value
  {
    lightusd::legacy::value::half h;
    h.value = 0x7BFF;  // 65504 (max half)
    std::string s = dtos(h);
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "max half should roundtrip: 0x%04x -> \"%s\" -> 0x%04x",
                h.value, s.c_str(), h2.value);
  }

  // Test value 0.7998 (0x3A66) - another common test case
  {
    lightusd::legacy::value::half h;
    h.value = 0x3A66;  // ~0.79980469
    std::string s = dtos(h);

    // Verify round-trip
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "0.7998 should roundtrip: 0x%04x -> \"%s\" -> 0x%04x",
                h.value, s.c_str(), h2.value);
  }
}

// Test that we produce short representations where possible
void half_shortest_representation_test(void) {
  // Value 0x3CCD: pxrUSD outputs "1.2002", LightUSD should output something shorter
  {
    lightusd::legacy::value::half h;
    h.value = 0x3CCD;
    std::string s = dtos(h);

    // "1.2002" has 6 chars, we should produce something shorter or equal
    TEST_CHECK_(s.length() <= 6,
                "0x3CCD should have short representation (<=6 chars), got \"%s\" (len=%zu)",
                s.c_str(), s.length());

    // Verify correctness
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "shortest representation must still roundtrip: \"%s\"", s.c_str());

    // Ideally it should be "1.2"
    TEST_CHECK_(s == "1.2",
                "0x3CCD should produce \"1.2\", got \"%s\"", s.c_str());
  }

  // Test another value that can have a shorter representation
  {
    lightusd::legacy::value::half h;
    h.value = 0x3A66;  // ~0.79980469
    std::string s = dtos(h);

    // "0.799805" has 8 chars, LightUSD should produce something shorter
    TEST_CHECK_(s.length() <= 8,
                "0x3A66 should have short representation (<=8 chars), got \"%s\" (len=%zu)",
                s.c_str(), s.length());

    // Verify correctness
    float parsed = std::strtof(s.c_str(), nullptr);
    lightusd::legacy::value::half h2 = lightusd::legacy::value::float_to_half_full(parsed);
    TEST_CHECK_(h2.value == h.value,
                "shortest representation must still roundtrip: \"%s\"", s.c_str());
  }
}
