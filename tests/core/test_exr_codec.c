/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/imageio/exr-codec.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
  const size_t budget = 32u << 20;
  for (uint32_t width = 1; width <= 32; width *= 32) {
    float pixels[32*4];
    for (size_t i = 0; i < width*4; ++i) pixels[i] = (float)(i % 13) / 8.0f;
    for (uint32_t bits = 16; bits <= 32; bits *= 2) {
      lu_exr_result image = {0}, encoded = {0}, decoded = {0}, info = {0};
      image.size = width*4*sizeof(float); image.data = malloc(image.size);
      memcpy(image.data, pixels, image.size); image.bits = 32;
      assert(lu_exr_convert(&image, bits, budget) == 0);
      assert(lu_exr_encode(image.data, image.size, width, 1, 4, bits, budget, &encoded) == 0);
      assert(lu_exr_info(encoded.data, encoded.size, budget, &info) == 0);
      assert(info.width == width && info.height == 1 && info.channels == 4);
      assert(lu_exr_decode(encoded.data, encoded.size, budget, &decoded) == 0);
      assert(decoded.bits == bits && decoded.size == image.size);
      assert(memcmp(decoded.data, image.data, image.size) == 0);
      assert(lu_exr_convert(&decoded, 32, budget) == 0);
      assert(memcmp(decoded.data, pixels, width*4*sizeof(float)) == 0);
      free(decoded.data); free(image.data);
      assert(lu_exr_decode(encoded.data, encoded.size, 1, &decoded) != 0);
      assert(decoded.data == NULL);
      assert(lu_exr_decode(encoded.data, encoded.size/2, budget, &decoded) != 0);
      assert(decoded.data == NULL);
      free(encoded.data);
    }
  }
  lu_exr_result bad = {0}; uint8_t pixel[4] = {255, 0, 127, 255};
  assert(lu_exr_encode(pixel, 4, UINT32_MAX, 1, 4, 8, budget, &bad) != 0);
  assert(lu_exr_encode(pixel, 1, 1, 1, 4, 8, budget, &bad) != 0);
  assert(lu_exr_encode(pixel, 4, 1, 1, 2, 8, budget, &bad) != 0);
  assert(lu_exr_encode(pixel, 4, 1, 1, 4, 8, 1, &bad) != 0);
  assert(lu_exr_info(pixel, 4, budget, &bad) != 0);
  return 0;
}
