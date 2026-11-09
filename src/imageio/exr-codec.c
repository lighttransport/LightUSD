/* SPDX-License-Identifier: Apache-2.0 */
#include "exr-codec.h"
#include "../external/tinyexr/include/exr.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct Budget { size_t used, limit; } Budget;
/* Spelled out: MSVC's C headers do not declare max_align_t. */
typedef union Block {
  size_t size;
  long long ll;
  long double ld;
  void* p;
} Block;
static void* allocate(void* context, size_t size) {
  Budget* b = (Budget*)context;
  if (size > SIZE_MAX - sizeof(Block)) return NULL;
  size += sizeof(Block);
  if (size > b->limit - b->used) return NULL;
  Block* block = (Block*)malloc(size);
  if (!block) return NULL;
  block->size = size; b->used += size;
  return block + 1;
}
static void release(void* context, void* ptr) {
  if (ptr) {
    Block* block = (Block*)ptr - 1;
    ((Budget*)context)->used -= block->size;
    free(block);
  }
}
static int channel_is(const char* name, const char* channel) {
  size_t n = strlen(name), c = strlen(channel);
  return n >= c && !strcmp(name + n - c, channel) && (n == c || name[n-c-1] == '.');
}
static float component(const exr_part* p, int channel, size_t i) {
  if (channel < 0 || !p->images[channel]) return 0;
  const void* data = p->images[channel];
  if (p->header.channels[channel].pixel_type == EXR_PIXEL_HALF) {
    float f; exr_half_to_float((const uint16_t*)data + i, &f, 1); return f;
  }
  if (p->header.channels[channel].pixel_type == EXR_PIXEL_UINT)
    return (float)((const uint32_t*)data)[i];
  return ((const float*)data)[i];
}
int lu_exr_info(const uint8_t* data, size_t size, size_t budget, lu_exr_result* out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!data || !size || !budget) return -1;
  Budget b = {0, budget}; exr_allocator a = {&b, allocate, release};
  exr_reader* reader = NULL;
  exr_result r = exr_reader_open_memory(data, size, &a, &reader);
  if (r < 0) return (int)r;
  r = exr_reader_parse_header(reader);
  const exr_header* h = r < 0 ? NULL : exr_reader_part_header(reader, 0);
  if (h) {
    int64_t w = (int64_t)h->data_window.max_x - h->data_window.min_x + 1;
    int64_t height = (int64_t)h->data_window.max_y - h->data_window.min_y + 1;
    if (w > 0 && w <= INT32_MAX && height > 0 && height <= INT32_MAX && h->num_channels > 0) {
      out->width = (uint32_t)w; out->height = (uint32_t)height;
      out->channels = (uint32_t)h->num_channels; out->bits = 32;
    }
  }
  exr_reader_close(reader);
  return out->width ? 0 : -2;
}
int lu_exr_decode(const uint8_t* data, size_t size, size_t budget, lu_exr_result* out) {
  int status = lu_exr_info(data, size, budget, out);
  if (status) return status;
  uint64_t pixel_count = (uint64_t)out->width * out->height;
  if (pixel_count > SIZE_MAX / 16 || pixel_count > budget / 8) return -7;
  size_t total = 0;
  Budget b = {0, budget}; exr_allocator a = {&b, allocate, release};
  exr_image image; memset(&image, 0, sizeof(image));
  exr_result r = exr_load_from_memory(data, size, &a, &image);
  if (r < 0) return (int)r;
  if (!image.num_parts || !image.parts || image.parts[0].is_deep || !image.parts[0].images) {
    exr_image_free(&image); return -3;
  }
  const exr_part* p = image.parts;
  // The adapter exposes dense RGBA. Reject subsampled planes before indexing.
  for (int c = 0; c < p->header.num_channels; ++c) {
    if (p->header.channels[c].x_sampling != 1 || p->header.channels[c].y_sampling != 1) {
      exr_image_free(&image); return -3;
    }
  }
  int indices[5] = {-1,-1,-1,-1,-1};
  const char* names[5] = {"R", "G", "B", "A", "Y"};
  for (int c = 0; c < p->header.num_channels; ++c)
    for (int k = 0; k < 5; ++k)
      if (channel_is(p->header.channels[c].name, names[k])) { indices[k] = c; break; }
  if (indices[0] < 0 && indices[1] < 0 && indices[2] < 0 && indices[4] < 0) {
    exr_image_free(&image); return -3;
  }
  int half = image.num_parts == 1 && p->header.part_type == EXR_PART_SCANLINE;
  for (int c = 0; c < p->header.num_channels; ++c)
    if (p->header.channels[c].pixel_type != EXR_PIXEL_HALF) half = 0;
  total = (size_t)pixel_count * (half ? 8 : 16);
  if (total > budget - b.used) { exr_image_free(&image); return -7; }
  void* pixels = malloc(total);
  if (!pixels) { exr_image_free(&image); return -4; }
  for (size_t i = 0; i < (size_t)pixel_count; ++i) {
    for (int k = 0; k < 4; ++k) {
      int c = indices[k];
      if (k < 3 && c < 0) c = indices[4];
      if (half) {
        ((uint16_t*)pixels)[i*4+(size_t)k] = c >= 0 && p->images[c]
          ? ((const uint16_t*)p->images[c])[i] : (k == 3 ? 0x3c00 : 0);
      } else {
        ((float*)pixels)[i*4+(size_t)k] = k == 3 && c < 0 ? 1.0f : component(p, c, i);
      }
    }
  }
  exr_image_free(&image);
  out->data = (uint8_t*)pixels; out->size = total; out->channels = 4; out->bits = half ? 16 : 32;
  return 0;
}

int lu_exr_encode(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                  uint32_t channels, uint32_t bits, size_t budget, lu_exr_result* out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!data || !width || !height || width > INT32_MAX || height > INT32_MAX ||
      (channels != 1 && channels != 3 && channels != 4) ||
      (bits != 8 && bits != 16 && bits != 32)) return -1;
  uint64_t pixels = (uint64_t)width * height;
  if (pixels > SIZE_MAX / channels / 4) return -7;
  size_t count = (size_t)pixels * channels;
  if (count > size / (bits / 8)) return -1;
  size_t stride = (size_t)pixels * (bits == 32 ? 4 : 2);
  Budget b = {0, budget}; exr_allocator a = {&b, allocate, release};
  uint8_t* planes = (uint8_t*)allocate(&b, stride * channels);
  if (!planes) return -4;
  for (size_t i = 0; i < (size_t)pixels; ++i) for (uint32_t c = 0; c < channels; ++c) {
    const uint8_t* src = data + (i * channels + c) * (bits / 8);
    uint8_t* dst = planes + stride * c + i * (bits == 32 ? 4 : 2);
    if (bits == 8) {
      float f = (float)*src / 255.0f; uint16_t h;
      exr_float_to_half(&f, &h, 1); memcpy(dst, &h, 2);
    } else memcpy(dst, src, bits / 8);
  }
  exr_header h; memset(&h, 0, sizeof(h));
  h.part_type = EXR_PART_SCANLINE;
  h.compression = width < 16 && height < 16 ? EXR_COMPRESSION_NONE : EXR_COMPRESSION_ZIP;
  h.line_order = EXR_LINEORDER_INCREASING_Y;
  h.data_window.max_x = (int32_t)width - 1; h.data_window.max_y = (int32_t)height - 1;
  h.display_window = h.data_window; h.pixel_aspect_ratio = h.screen_window_width = 1.0f;
  exr_channel cs[4]; memset(cs, 0, sizeof(cs));
  const char* names[4] = {channels == 1 ? "Y" : "R", "G", "B", "A"};
  for (uint32_t c = 0; c < channels; ++c) {
    strcpy(cs[c].name, names[c]);
    cs[c].pixel_type = bits == 32 ? EXR_PIXEL_FLOAT : EXR_PIXEL_HALF;
    cs[c].x_sampling = cs[c].y_sampling = 1;
  }
  h.num_channels = (int32_t)channels; h.channels = cs;
  exr_writer* writer = NULL;
  void* encoded = NULL; size_t encoded_size = 0;
  exr_result r = exr_writer_create(&a, &writer);
  int32_t part = 0;
  if (r >= 0) r = exr_writer_add_part(writer, &h, &part);
  for (uint32_t c = 0; r >= 0 && c < channels; ++c)
    r = exr_writer_set_channel(writer, part, names[c], planes + stride * c);
  if (r >= 0) r = exr_writer_finalize_to_memory(writer, &encoded, &encoded_size);
  if (writer) exr_writer_destroy(writer);
  release(&b, planes);
  if (r >= 0 && encoded_size && encoded_size <= budget - b.used) {
    out->data = (uint8_t*)malloc(encoded_size);
    if (out->data) { memcpy(out->data, encoded, encoded_size); out->size = encoded_size; }
    else r = EXR_ERROR_OUT_OF_MEMORY;
  } else if (r >= 0) r = EXR_ERROR_LIMIT_EXCEEDED;
  release(&b, encoded);
  return (int)r;
}

int lu_exr_convert(lu_exr_result* result, uint32_t bits, size_t budget) {
  if (!result || !result->data || (bits != 16 && bits != 32) ||
      (result->bits != 16 && result->bits != 32)) return -1;
  if (result->bits == bits) return 0;
  size_t count = result->size / (result->bits / 8);
  if (result->size > budget || count > (budget - result->size) / (bits / 8)) return -7;
  size_t size = count * (bits / 8);
  void* data = malloc(size);
  if (!data) return -4;
  if (bits == 32) exr_half_to_float((const uint16_t*)result->data, (float*)data, count);
  else exr_float_to_half((const float*)result->data, (uint16_t*)data, count);
  free(result->data); result->data = (uint8_t*)data; result->bits = bits; result->size = size;
  return 0;
}
