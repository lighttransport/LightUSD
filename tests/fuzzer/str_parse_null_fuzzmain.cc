// SPDX-License-Identifier: MIT
// Fuzzer for hardened numeric string parse helpers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "str-util.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 4096) return 0;

  const size_t n = std::min<size_t>(size, 256);
  const std::string s(reinterpret_cast<const char *>(data), n);

  (void)lightusd::legacy::atoi(s);
  (void)lightusd::legacy::atoll(s);
  (void)lightusd::legacy::atof_float(s);
  (void)lightusd::legacy::atod(s);

  const char *cstr = s.c_str();
  (void)lightusd::legacy::atoi(cstr);
  (void)lightusd::legacy::atoll(cstr);
  (void)lightusd::legacy::atof_float(cstr);
  (void)lightusd::legacy::atod(cstr);

  (void)lightusd::legacy::atoi(static_cast<const char *>(nullptr));
  (void)lightusd::legacy::atoll(static_cast<const char *>(nullptr));
  (void)lightusd::legacy::atof_float(static_cast<const char *>(nullptr));
  (void)lightusd::legacy::atod(static_cast<const char *>(nullptr));

  return 0;
}
