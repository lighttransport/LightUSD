// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace lightusd {
constexpr bool NativeIOEnabled() {
#ifdef LIGHTUSD_NO_NATIVE_IO
  return false;
#else
  return true;
#endif
}
inline const char* NativeIOUnavailableMessage() {
  return "native filesystem I/O is disabled in this build";
}
inline bool NativeIOAvailable(std::string* error = nullptr) {
  if (NativeIOEnabled()) return true;
  if (error) *error = NativeIOUnavailableMessage();
  return false;
}
}  // namespace lightusd
