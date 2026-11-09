/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIGHTUSD_EXR_CODEC_H
#define LIGHTUSD_EXR_CODEC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct lu_exr_result {
  uint8_t* data;
  size_t size;
  uint32_t width, height, channels, bits;
} lu_exr_result;
/* Zero-initialized outputs; returned bytes belong to the caller (free()).
 * budget includes codec scratch and output, excluding caller-owned input. */
int lu_exr_info(const uint8_t*, size_t, size_t budget, lu_exr_result*);
int lu_exr_decode(const uint8_t*, size_t, size_t budget, lu_exr_result*);
int lu_exr_convert(lu_exr_result*, uint32_t bits, size_t budget);
int lu_exr_encode(const uint8_t*, size_t, uint32_t width, uint32_t height,
                  uint32_t channels, uint32_t bits, size_t budget, lu_exr_result*);
#ifdef __cplusplus
}
#endif
#endif
