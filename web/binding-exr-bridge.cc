// SPDX-License-Identifier: Apache-2.0
#include "../src/imageio/exr-codec.h"
#include <emscripten/emscripten.h>
// Only POD spans cross the JS bridge; each module owns its own allocator.
EM_JS(int, LightUSDExrBridge, (int op, const uint8_t* data, size_t size,
    uint32_t width, uint32_t height, uint32_t channels, uint32_t bits,
    size_t budget, lu_exr_result* result, int pointer_bytes), {
  if (!Module['exrBridge']) return -8;
  return Module['exrBridge'](op, Number(data), Number(size), width, height,
    channels, bits, Number(budget), Number(result), pointer_bytes);
});
extern "C" {
int lu_exr_info(const uint8_t* p, size_t n, size_t b, lu_exr_result* out) {
  return LightUSDExrBridge(0, p, n, 0, 0, 0, 0, b, out, sizeof(void*));
}
int lu_exr_decode(const uint8_t* p, size_t n, size_t b, lu_exr_result* out) {
  return LightUSDExrBridge(1, p, n, 0, 0, 0, 0, b, out, sizeof(void*));
}
int lu_exr_encode(const uint8_t* p, size_t n, uint32_t w, uint32_t h,
    uint32_t c, uint32_t bits, size_t b, lu_exr_result* out) {
  return LightUSDExrBridge(2, p, n, w, h, c, bits, b, out, sizeof(void*));
}
}
