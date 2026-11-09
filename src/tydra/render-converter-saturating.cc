#include "render-converter-internal.hh"
#include "render-extract.hh"
#include "scene-access.hh"

namespace lightusd {
namespace tydra {
namespace next {

size_t SaturatingAdd(size_t a, size_t b) {
  if (b > std::numeric_limits<size_t>::max() - a) {
    return std::numeric_limits<size_t>::max();
  }
  return a + b;
}

size_t SaturatingMul(size_t a, size_t b) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return std::numeric_limits<size_t>::max();
  }
  return a * b;
}

bool WouldOverflowSizeMul(size_t a, size_t b) {
  return a != 0 && b > std::numeric_limits<size_t>::max() / a;
}



}  // namespace next
}  // namespace tydra
}  // namespace lightusd
