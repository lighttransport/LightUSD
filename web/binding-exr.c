/* SPDX-License-Identifier: Apache-2.0 */
#include "../src/imageio/exr-codec.h"
#include <emscripten/emscripten.h>
#include <stdlib.h>
#include <string.h>
static lu_exr_result result;
EMSCRIPTEN_KEEPALIVE void* lu_feature_alloc(uint32_t size) { return malloc(size); }
EMSCRIPTEN_KEEPALIVE void lu_feature_free(void* ptr) { free(ptr); }
EMSCRIPTEN_KEEPALIVE void lu_exr_clear(void) { free(result.data); memset(&result, 0, sizeof(result)); }
EMSCRIPTEN_KEEPALIVE const uint8_t* lu_exr_data(void) { return result.data; }
EMSCRIPTEN_KEEPALIVE uint32_t lu_exr_meta(uint32_t field) {
  switch (field) {
    case 0: return result.width; case 1: return result.height;
    case 2: return result.channels; case 3: return result.bits;
    case 4: return result.size <= UINT32_MAX ? (uint32_t)result.size : 0;
    default: return 0;
  }
}
EMSCRIPTEN_KEEPALIVE int lu_exr_run(uint32_t op, const uint8_t* data, uint32_t size,
    uint32_t width, uint32_t height, uint32_t channels, uint32_t bits, uint32_t budget) {
  lu_exr_clear();
  if (!budget || budget > (UINT32_C(512) << 20) || size > (UINT32_C(256) << 20)) return -7;
  if (op == 0) return lu_exr_info(data, size, budget, &result);
  if (op == 1 || op == 3 || op == 4) {
    int status = lu_exr_decode(data, size, budget, &result);
    if (!status && op != 1) status = lu_exr_convert(&result, op == 3 ? 16 : 32, budget);
    return status;
  }
  if (op == 2) return lu_exr_encode(data, size, width, height, channels, bits, budget, &result);
  return -1;
}
