// SPDX-License-Identifier: Apache-2.0
#include "escape-string.hh"
#include "../c/usda-string.h"
#include <cstdint>
#include <cstdlib>
namespace lightusd { namespace next {
std::string EscapeUSDAString(const std::string& text) {
  const size_t size = lu_usda_quote(text.data(), text.size(), nullptr, 0);
  // Matches the existing no-exceptions string allocation failure policy.
  if (size == SIZE_MAX) std::abort();
  std::string result(size, '\0');
  lu_usda_quote(text.data(), text.size(), &result[0], result.size());
  return result;
}
} }
