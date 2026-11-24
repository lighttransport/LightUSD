// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstddef>
#include <ostream>
namespace lightusd_examples {
inline void Base64(std::ostream& out, const uint8_t* bytes, size_t size) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char buffer[4096];
  size_t used = 0;
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t value = (uint32_t(bytes[i]) << 16) |
        (i + 1 < size ? uint32_t(bytes[i + 1]) << 8 : 0u) |
        (i + 2 < size ? uint32_t(bytes[i + 2]) : 0u);
    buffer[used++] = alphabet[(value >> 18) & 63];
    buffer[used++] = alphabet[(value >> 12) & 63];
    buffer[used++] = i + 1 < size ? alphabet[(value >> 6) & 63] : '=';
    buffer[used++] = i + 2 < size ? alphabet[value & 63] : '=';
    if (used == sizeof(buffer)) { out.write(buffer, static_cast<std::streamsize>(sizeof(buffer))); used = 0; }
  }
  if (used) out.write(buffer, static_cast<std::streamsize>(used));
}
}
