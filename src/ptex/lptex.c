/* SPDX-License-Identifier: Apache-2.0 */
/* lptex: portable C11 Ptex container reader. See lptex.h and doc/ptex.md. */
#include "lptex.h"
#include "lptex-internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../external/miniz.h"

/* ------------------------------------------------------------------------ */
/* Status strings. They match the messages of the former C++ reader.        */

const char* lptex_status_str(lptex_status status) {
  switch (status) {
    case LPTEX_OK: return "ok";
    case LPTEX_E_INVALID_ARGUMENT: return "invalid Ptex memory";
    case LPTEX_E_OUT_OF_MEMORY: return "Ptex reader is out of memory";
    case LPTEX_E_OPEN_FAILED: return "cannot open Ptex file";
    case LPTEX_E_EMPTY_FILE: return "empty Ptex file";
    case LPTEX_E_IO: return "cannot read Ptex file";
    case LPTEX_E_FILE_TOO_LARGE:
      return "Ptex file too large or not a regular file";
    case LPTEX_E_HEADER_TRUNCATED: return "Ptex header is truncated";
    case LPTEX_E_BAD_MAGIC: return "invalid Ptex magic";
    case LPTEX_E_UNSUPPORTED_VERSION: return "unsupported Ptex major version";
    case LPTEX_E_UNSUPPORTED_DATA_TYPE: return "unsupported Ptex data type";
    case LPTEX_E_INVALID_DIMENSIONS: return "invalid Ptex dimensions";
    case LPTEX_E_FACE_TABLE_TOO_LARGE: return "Ptex face table is too large";
    case LPTEX_E_EXT_HEADER_TRUNCATED: return "Ptex extended header truncated";
    case LPTEX_E_FACE_INFO_INVALID: return "Ptex face-info block is invalid";
    case LPTEX_E_FACE_RESOLUTION_INVALID: return "invalid Ptex face resolution";
    case LPTEX_E_CONSTANT_BLOCK_INVALID: return "Ptex constant block is invalid";
    case LPTEX_E_LEVEL_INFO_INVALID: return "Ptex level-info block is invalid";
    case LPTEX_E_LEVEL_HEADER_INVALID: return "invalid Ptex level header";
    case LPTEX_E_LEVEL_FACE_COUNT_INVALID:
      return "invalid Ptex level face count";
    case LPTEX_E_LEVEL_DATA_OVERFLOW: return "Ptex level data overflow";
    case LPTEX_E_LEVEL_DATA_TRUNCATED: return "Ptex level data is truncated";
    case LPTEX_E_BLOCK_TOO_LARGE: return "Ptex block is too large";
    case LPTEX_E_INFLATE_FAILED: return "Ptex deflate block failed";
    case LPTEX_E_INVALID_FACE_LEVEL: return "invalid Ptex face/level";
    case LPTEX_E_DECODE_BUDGET: return "Ptex face exceeds decode budget";
    case LPTEX_E_CONSTANT_FACE_TRUNCATED:
      return "Ptex constant face is truncated";
    case LPTEX_E_FACE_ABSENT_FROM_BASE:
      return "Ptex face is absent from base level";
    case LPTEX_E_REDUCTION_UNSUPPORTED:
      return "dynamic Ptex reduction for this type is unsupported";
    case LPTEX_E_REDUCTION_DIMENSIONS:
      return "invalid Ptex reduction dimensions";
    case LPTEX_E_LEVEL_BLOCK_HEADER_INVALID:
      return "Ptex level header is invalid";
    case LPTEX_E_LEVEL_BLOCK_TRUNCATED: return "Ptex level block is truncated";
    case LPTEX_E_FACE_PAYLOAD_OVERFLOW: return "Ptex face payload overflow";
    case LPTEX_E_LEVEL_CONSTANT_TRUNCATED:
      return "Ptex level constant is truncated";
    case LPTEX_E_TILE_HEADER_TRUNCATED:
      return "Ptex tiled face header is truncated";
    case LPTEX_E_TILE_RESOLUTION_INVALID:
      return "Ptex tile resolution is invalid";
    case LPTEX_E_TILE_TABLE_INVALID: return "Ptex tile table is invalid";
    case LPTEX_E_TILE_PAYLOAD_OVERFLOW: return "Ptex tile payload overflow";
    case LPTEX_E_TILE_CONSTANT_TRUNCATED:
      return "Ptex tile constant is truncated";
    case LPTEX_E_UNSUPPORTED_TILE_ENCODING:
      return "unsupported Ptex tile encoding";
    case LPTEX_E_UNSUPPORTED_FACE_ENCODING:
      return "unsupported Ptex face encoding";
    case LPTEX_E_FACE_PAYLOAD_TRUNCATED: return "Ptex face payload is truncated";
    case LPTEX_E_PLANAR_SIZE_MISMATCH: return "Ptex planar face size mismatch";
    case LPTEX_E_BUFFER_TOO_SMALL: return "Ptex destination buffer is too small";
  }
  return "unknown Ptex error";
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */

typedef struct lptex_level {
  uint64_t offset; /* relative to level_data_offset */
  uint64_t size;
  uint32_t header_size;
  uint32_t faces;
} lptex_level;

struct lptex_reader {
  lptex_allocator alloc;
  const uint8_t* data;
  size_t size;
  lptex_info info;
  lptex_face_info* faces;
  uint8_t* constant_data;
  size_t constant_size;
  lptex_level* levels;
  size_t level_data_offset;
  /* Optional owner of `data` (set by lptex-file.c). */
  void (*release)(void* ctx, const void* data, size_t size);
  void* release_ctx;
};

static void* default_alloc(void* user, size_t size) {
  (void)user;
  return malloc(size ? size : 1u);
}
static void default_free(void* user, void* ptr) {
  (void)user;
  free(ptr);
}

static void* r_alloc(const lptex_reader* r, size_t size) {
  return r->alloc.alloc(r->alloc.user, size ? size : 1u);
}
static void r_free(const lptex_reader* r, void* ptr) {
  if (ptr) r->alloc.free(r->alloc.user, ptr);
}

static int add_sz(size_t a, size_t b, size_t* out) {
  if (b > SIZE_MAX - a) return 0;
  *out = a + b;
  return 1;
}
static int mul_sz(size_t a, size_t b, size_t* out) {
  if (a && b > SIZE_MAX / a) return 0;
  *out = a * b;
  return 1;
}
static int in_range(size_t off, size_t n, size_t size) {
  size_t end = 0;
  return add_sz(off, n, &end) && end <= size;
}
static uint16_t ld16(const uint8_t* p) {
  return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static uint32_t ld32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t ld64(const uint8_t* p) {
  return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}
static uint32_t max_u32(uint32_t a, uint32_t b) { return a < b ? b : a; }
static uint32_t min_u32(uint32_t a, uint32_t b) { return b < a ? b : a; }

size_t lptex_type_size(lptex_type type) {
  return type == LPTEX_TYPE_U8 ? 1u
         : (type == LPTEX_TYPE_U16 || type == LPTEX_TYPE_HALF) ? 2u
                                                               : 4u;
}

/* Inflate a zlib stream that must expand to exactly dst_size bytes. */
static lptex_status inflate_exact(const uint8_t* src, size_t src_size,
                                  uint8_t* dst, size_t dst_size) {
  mz_ulong n;
  uint8_t dummy = 0;
#if SIZE_MAX > ULONG_MAX
  if (dst_size > (size_t)ULONG_MAX || src_size > (size_t)ULONG_MAX)
    return LPTEX_E_BLOCK_TOO_LARGE;
#endif
  n = (mz_ulong)dst_size;
  if (mz_uncompress(dst_size ? dst : &dummy, &n, src, (mz_ulong)src_size) !=
          MZ_OK ||
      n != (mz_ulong)dst_size)
    return LPTEX_E_INFLATE_FAILED;
  return LPTEX_OK;
}

/* Inflate into a fresh allocation. */
static lptex_status inflate_alloc(const lptex_reader* r, const uint8_t* src,
                                  size_t src_size, size_t dst_size,
                                  uint8_t** out) {
  lptex_status st;
  uint8_t* buf = (uint8_t*)r_alloc(r, dst_size);
  *out = NULL;
  if (!buf) return LPTEX_E_OUT_OF_MEMORY;
  st = inflate_exact(src, src_size, buf, dst_size);
  if (st != LPTEX_OK) {
    r_free(r, buf);
    return st;
  }
  *out = buf;
  return LPTEX_OK;
}

/* ------------------------------------------------------------------------ */
/* Open / close                                                              */

static lptex_status parse(lptex_reader* r) {
  const uint8_t* d = r->data;
  lptex_info* info = &r->info;
  uint32_t type, ext_size, face_size, const_size, level_info_size, i;
  uint64_t level_data_size, cursor;
  size_t expected = 0, off = 64, const_expected = 0, level_info_expected = 0;
  uint8_t* face_bytes = NULL;
  lptex_status st;

  if (!d || r->size < 64) return LPTEX_E_HEADER_TRUNCATED;
  if (memcmp(d, "Ptex", 4) != 0) return LPTEX_E_BAD_MAGIC;
  if (ld32(d + 4) != 1) return LPTEX_E_UNSUPPORTED_VERSION;
  info->mesh_type = ld32(d + 8);
  type = ld32(d + 12);
  if (type > 3) return LPTEX_E_UNSUPPORTED_DATA_TYPE;
  info->type = (lptex_type)type;
  info->alpha_channel = ld32(d + 16);
  info->channels = ld16(d + 20);
  info->levels = ld16(d + 22);
  info->faces = ld32(d + 24);
  ext_size = ld32(d + 28);
  face_size = ld32(d + 32);
  const_size = ld32(d + 36);
  level_info_size = ld32(d + 40);
  level_data_size = ld64(d + 48);
  /* d + 56: metadata size; metadata is intentionally left unread. */
  if (!info->channels || !info->levels || info->levels > 32 || !info->faces ||
      info->faces > 100000000u)
    return LPTEX_E_INVALID_DIMENSIONS;
  if (!mul_sz(info->faces, 20u, &expected) || expected > ((size_t)1 << 31))
    return LPTEX_E_FACE_TABLE_TOO_LARGE;
  if (!in_range(off, ext_size, r->size)) return LPTEX_E_EXT_HEADER_TRUNCATED;
  off += ext_size;
  if (!in_range(off, face_size, r->size)) return LPTEX_E_FACE_INFO_INVALID;
  st = inflate_alloc(r, d + off, face_size, expected, &face_bytes);
  if (st == LPTEX_E_OUT_OF_MEMORY) return st;
  if (st != LPTEX_OK) return LPTEX_E_FACE_INFO_INVALID;
  r->faces = (lptex_face_info*)r_alloc(r, sizeof(lptex_face_info) *
                                              (size_t)info->faces);
  if (!r->faces) {
    r_free(r, face_bytes);
    return LPTEX_E_OUT_OF_MEMORY;
  }
  for (i = 0; i < info->faces; ++i) {
    const uint8_t* p = face_bytes + (size_t)i * 20u;
    lptex_face_info* fi = &r->faces[i];
    int e;
    fi->ures = p[0];
    fi->vres = p[1];
    fi->adj_edges = p[2];
    fi->flags = p[3];
    for (e = 0; e < 4; ++e) fi->adj_faces[e] = ld32(p + 4 + e * 4);
    if (fi->ures > 30 || fi->vres > 30) {
      r_free(r, face_bytes);
      return LPTEX_E_FACE_RESOLUTION_INVALID;
    }
  }
  r_free(r, face_bytes);
  info->face_info = r->faces;
  off += face_size;

  if (!mul_sz(info->faces, info->channels, &const_expected) ||
      !mul_sz(const_expected, lptex_type_size(info->type), &const_expected) ||
      const_expected > ((size_t)1 << 31) || !in_range(off, const_size, r->size))
    return LPTEX_E_CONSTANT_BLOCK_INVALID;
  st = inflate_alloc(r, d + off, const_size, const_expected, &r->constant_data);
  if (st != LPTEX_OK) return st;
  r->constant_size = const_expected;
  off += const_size;

  if (!mul_sz(info->levels, 16u, &level_info_expected) ||
      level_info_size != level_info_expected ||
      !in_range(off, level_info_size, r->size))
    return LPTEX_E_LEVEL_INFO_INVALID;
  r->levels = (lptex_level*)r_alloc(r, sizeof(lptex_level) * info->levels);
  if (!r->levels) return LPTEX_E_OUT_OF_MEMORY;
  cursor = 0;
  for (i = 0; i < info->levels; ++i) {
    const uint8_t* p = d + off + (size_t)i * 16u;
    lptex_level* lv = &r->levels[i];
    lv->size = ld64(p);
    lv->header_size = ld32(p + 8);
    lv->faces = ld32(p + 12);
    lv->offset = cursor;
    if (lv->header_size > lv->size) return LPTEX_E_LEVEL_HEADER_INVALID;
    if (lv->faces > info->faces) return LPTEX_E_LEVEL_FACE_COUNT_INVALID;
    if (lv->size > UINT64_MAX - cursor) return LPTEX_E_LEVEL_DATA_OVERFLOW;
    cursor += lv->size;
  }
  off += level_info_size;
  if (cursor != level_data_size) return LPTEX_E_LEVEL_DATA_TRUNCATED;
#if SIZE_MAX < UINT64_MAX
  if (level_data_size > (uint64_t)SIZE_MAX) return LPTEX_E_LEVEL_DATA_TRUNCATED;
#endif
  if (!in_range(off, (size_t)level_data_size, r->size))
    return LPTEX_E_LEVEL_DATA_TRUNCATED;
  r->level_data_offset = off;
  return LPTEX_OK;
}

void lptex_internal_set_release(lptex_reader* r,
                        void (*release)(void* ctx, const void* data,
                                        size_t size),
                        void* ctx) {
  r->release = release;
  r->release_ctx = ctx;
}

lptex_status lptex_open_memory(const void* data, size_t size,
                               const lptex_options* options,
                               lptex_reader** out) {
  lptex_allocator a;
  lptex_reader* r;
  lptex_status st;
  if (!out) return LPTEX_E_INVALID_ARGUMENT;
  *out = NULL;
  if (!data && size) return LPTEX_E_INVALID_ARGUMENT;
  a.alloc = default_alloc;
  a.free = default_free;
  a.user = NULL;
  if (options && options->allocator.alloc && options->allocator.free)
    a = options->allocator;
  r = (lptex_reader*)a.alloc(a.user, sizeof(*r));
  if (!r) return LPTEX_E_OUT_OF_MEMORY;
  memset(r, 0, sizeof(*r));
  r->alloc = a;
  r->data = (const uint8_t*)data;
  r->size = size;
  st = parse(r);
  if (st != LPTEX_OK) {
    lptex_close(r);
    return st;
  }
  *out = r;
  return LPTEX_OK;
}

void lptex_close(lptex_reader* r) {
  lptex_allocator a;
  if (!r) return;
  a = r->alloc;
  r_free(r, r->faces);
  r_free(r, r->constant_data);
  r_free(r, r->levels);
  if (r->release) r->release(r->release_ctx, r->data, r->size);
  a.free(a.user, r);
}

const lptex_info* lptex_reader_info(const lptex_reader* r) {
  return r ? &r->info : NULL;
}

/* ------------------------------------------------------------------------ */
/* Face decode                                                               */

static uint32_t face_w(const lptex_face_info* fi) { return 1u << fi->ures; }
static uint32_t face_h(const lptex_face_info* fi) { return 1u << fi->vres; }

lptex_status lptex_face_dims(const lptex_reader* r, uint32_t face,
                             uint32_t level, uint32_t* width,
                             uint32_t* height, size_t* bytes) {
  const lptex_face_info* fi;
  uint32_t w, h;
  size_t n = 0;
  if (!r || face >= r->info.faces || level >= r->info.levels)
    return LPTEX_E_INVALID_FACE_LEVEL;
  fi = &r->faces[face];
  w = max_u32(1u, face_w(fi) >> level);
  h = max_u32(1u, face_h(fi) >> level);
  if (!mul_sz(w, h, &n) || !mul_sz(n, r->info.channels, &n) ||
      !mul_sz(n, lptex_type_size(r->info.type), &n))
    return LPTEX_E_DECODE_BUDGET;
  if (width) *width = w;
  if (height) *height = h;
  if (bytes) *bytes = n;
  return LPTEX_OK;
}

/* The predictor spans the entire planar stream, including channel
 * boundaries. Half/float data is stored without integer differencing. */
static void decode_difference(uint8_t* p, size_t n, lptex_type type) {
  size_t i;
  if (type == LPTEX_TYPE_U8) {
    for (i = 1; i < n; ++i) p[i] = (uint8_t)(p[i] + p[i - 1]);
  } else if (type == LPTEX_TYPE_U16) {
    for (i = 2; i + 1 < n; i += 2) {
      const uint16_t v = (uint16_t)(ld16(p + i) + ld16(p + i - 2));
      p[i] = (uint8_t)v;
      p[i + 1] = (uint8_t)(v >> 8);
    }
  }
}

/* dst holds w*h*channels*bpc bytes; planar is the same size. */
static void planar_to_interleaved(const uint8_t* planar, uint32_t w,
                                  uint32_t h, uint16_t channels, size_t bpc,
                                  uint8_t* dst) {
  const size_t pixels = (size_t)w * h;
  const size_t plane = pixels * bpc;
  size_t p;
  uint16_t c;
  if (channels == 1) {
    memcpy(dst, planar, plane);
    return;
  }
  for (c = 0; c < channels; ++c) {
    const uint8_t* src = planar + (size_t)c * plane;
    uint8_t* o = dst + (size_t)c * bpc;
    const size_t stride = (size_t)channels * bpc;
    if (bpc == 1) {
      for (p = 0; p < pixels; ++p) o[p * stride] = src[p];
    } else {
      for (p = 0; p < pixels; ++p) memcpy(o + p * stride, src + p * bpc, bpc);
    }
  }
}

static lptex_status decode_face(const lptex_reader* r, uint32_t face,
                                uint32_t level, size_t max_bytes,
                                uint8_t* out, uint32_t w, uint32_t h,
                                size_t bytes);

/* Missing low-resolution records are generated from the preceding level.
 * OpenPtex's generated integer mips reduce one axis at a time, with
 * truncation at each step, alternating the final axis by u resolution. */
static lptex_status reduce_previous(const lptex_reader* r, uint32_t face,
                                    uint32_t level, size_t max_bytes,
                                    uint8_t* out, uint32_t w, uint32_t h) {
  const lptex_face_info* fi = &r->faces[face];
  const uint16_t channels = r->info.channels;
  const size_t bpc = lptex_type_size(r->info.type);
  uint32_t pw = 0, ph = 0, nx, ny, x, y, dx, dy;
  size_t pbytes = 0;
  uint8_t* parent;
  lptex_status st;
  uint16_t c;
  int ulog;

  if (!level) return LPTEX_E_FACE_ABSENT_FROM_BASE;
  if (r->info.mesh_type != 1 ||
      (r->info.type != LPTEX_TYPE_U8 && r->info.type != LPTEX_TYPE_U16))
    return LPTEX_E_REDUCTION_UNSUPPORTED;
  st = lptex_face_dims(r, face, level - 1, &pw, &ph, &pbytes);
  if (st != LPTEX_OK) return st;
  if (pbytes > max_bytes) return LPTEX_E_DECODE_BUDGET;
  parent = (uint8_t*)r_alloc(r, pbytes);
  if (!parent) return LPTEX_E_OUT_OF_MEMORY;
  st = decode_face(r, face, level - 1, max_bytes, parent, pw, ph, pbytes);
  if (st != LPTEX_OK) {
    r_free(r, parent);
    return st;
  }
  nx = pw / w;
  ny = ph / h;
  if (!nx || nx > 2 || !ny || ny > 2) {
    r_free(r, parent);
    return LPTEX_E_REDUCTION_DIMENSIONS;
  }
  ulog = (int)fi->ures - (int)level;
  if (ulog < 0) ulog = 0;
  for (y = 0; y < h; ++y) {
    for (x = 0; x < w; ++x) {
      for (c = 0; c < channels; ++c) {
        uint32_t s[2][2] = {{0, 0}, {0, 0}};
        uint32_t reduced = 0;
        uint16_t value;
        size_t dst;
        for (dy = 0; dy < ny; ++dy) {
          for (dx = 0; dx < nx; ++dx) {
            const size_t src =
                (((size_t)(y * ny + dy) * pw + x * nx + dx) * channels + c) *
                bpc;
            s[dy][dx] = bpc == 1 ? parent[src] : ld16(parent + src);
          }
        }
        if (nx == 2 && ny == 2) {
          reduced = (ulog & 1)
                        ? ((s[0][0] + s[1][0]) / 2 + (s[0][1] + s[1][1]) / 2) / 2
                        : ((s[0][0] + s[0][1]) / 2 + (s[1][0] + s[1][1]) / 2) / 2;
        } else {
          for (dy = 0; dy < ny; ++dy)
            for (dx = 0; dx < nx; ++dx) reduced += s[dy][dx];
          reduced /= nx * ny;
        }
        value = (uint16_t)reduced;
        dst = (((size_t)y * w + x) * channels + c) * bpc;
        out[dst] = (uint8_t)value;
        if (bpc == 2) out[dst + 1] = (uint8_t)(value >> 8);
      }
    }
  }
  r_free(r, parent);
  return LPTEX_OK;
}

/* Position of `face` in a level's face order. Level 0 stores faces in index
 * order. Reduction levels store non-constant faces sorted by
 * min(width,height) >> level descending, ties by ascending face index. That
 * comparator is a strict total order, so the rank below equals the position
 * any correct sort (including the former std::sort) produces. */
static uint32_t face_ordinal(const lptex_reader* r, uint32_t face,
                             uint32_t level) {
  const lptex_face_info* fi = r->faces;
  uint32_t key, i, rank = 0;
  if (level == 0) return face;
  key = min_u32(face_w(&fi[face]), face_h(&fi[face])) >> level;
  for (i = 0; i < r->info.faces; ++i) {
    uint32_t k;
    if (i == face || (fi[i].flags & LPTEX_FACE_FLAG_CONSTANT)) continue;
    k = min_u32(face_w(&fi[i]), face_h(&fi[i])) >> level;
    if (k > key || (k == key && i < face)) ++rank;
  }
  return rank;
}

static lptex_status decode_tiled(const lptex_reader* r, const uint8_t* q,
                                 size_t compressed_size, uint32_t w,
                                 uint32_t h, uint8_t* out, size_t bytes) {
  const uint16_t channels = r->info.channels;
  const size_t bpc = lptex_type_size(r->info.type);
  const size_t value_bytes = (size_t)channels * bpc;
  uint32_t tw, th, tile_header_size, nx, ny, tx, ty, y;
  size_t tile_count = 0, cursor, tile_cap = 0;
  uint8_t* tile_header = NULL;
  uint8_t* tile = NULL;
  uint8_t* planar = NULL;
  lptex_status st = LPTEX_OK;

  if (q[0] > 30 || q[1] > 30) return LPTEX_E_TILE_RESOLUTION_INVALID;
  tw = 1u << q[0];
  th = 1u << q[1];
  tile_header_size = ld32(q + 2);
  nx = (uint32_t)(((uint64_t)w + tw - 1u) / tw);
  ny = (uint32_t)(((uint64_t)h + th - 1u) / th);
  if (!mul_sz(nx, ny, &tile_count) || tile_header_size > compressed_size - 6 ||
      tile_count > ((size_t)1 << 28))
    return LPTEX_E_TILE_TABLE_INVALID;
  st = inflate_alloc(r, q + 6, tile_header_size, tile_count * 4u, &tile_header);
  if (st != LPTEX_OK) return st;
  memset(out, 0, bytes);
  cursor = 6u + tile_header_size;
  for (ty = 0; ty < ny && st == LPTEX_OK; ++ty) {
    for (tx = 0; tx < nx; ++tx) {
      const uint32_t tp = ld32(tile_header + ((size_t)ty * nx + tx) * 4u);
      const uint32_t te = tp >> 30;
      const size_t ts = tp & 0x3fffffffu;
      const uint32_t cw = min_u32(tw, w - tx * tw);
      const uint32_t ch = min_u32(th, h - ty * th);
      const size_t row = (size_t)cw * value_bytes;
      size_t tile_bytes = 0, p;
      const uint8_t* rows;
      if (!mul_sz(cw, ch, &tile_bytes) ||
          !mul_sz(tile_bytes, channels, &tile_bytes) ||
          !mul_sz(tile_bytes, bpc, &tile_bytes) || cursor > compressed_size ||
          ts > compressed_size - cursor) {
        st = LPTEX_E_TILE_PAYLOAD_OVERFLOW;
        break;
      }
      if (tile_bytes > tile_cap) {
        r_free(r, tile);
        r_free(r, planar);
        planar = NULL;
        tile = (uint8_t*)r_alloc(r, tile_bytes);
        planar = (uint8_t*)r_alloc(r, tile_bytes);
        if (!tile || !planar) {
          st = LPTEX_E_OUT_OF_MEMORY;
          tile_cap = 0;
          break;
        }
        tile_cap = tile_bytes;
      }
      if (te == 0) {
        if (ts < value_bytes) {
          st = LPTEX_E_TILE_CONSTANT_TRUNCATED;
          break;
        }
        for (p = 0; p < (size_t)cw * ch; ++p)
          memcpy(tile + p * value_bytes, q + cursor, value_bytes);
        rows = tile;
      } else if (te == 1 || te == 2) {
        st = inflate_exact(q + cursor, ts, planar, tile_bytes);
        if (st != LPTEX_OK) break;
        if (te == 2) decode_difference(planar, tile_bytes, r->info.type);
        planar_to_interleaved(planar, cw, ch, channels, bpc, tile);
        rows = tile;
      } else {
        st = LPTEX_E_UNSUPPORTED_TILE_ENCODING;
        break;
      }
      for (y = 0; y < ch; ++y) {
        const size_t dst =
            ((size_t)(ty * th + y) * w + (size_t)tx * tw) * value_bytes;
        memcpy(out + dst, rows + (size_t)y * row, row);
      }
      cursor += ts;
    }
  }
  r_free(r, tile_header);
  r_free(r, tile);
  r_free(r, planar);
  return st;
}

static lptex_status decode_face(const lptex_reader* r, uint32_t face,
                                uint32_t level, size_t max_bytes,
                                uint8_t* out, uint32_t w, uint32_t h,
                                size_t bytes) {
  const lptex_face_info* fi = &r->faces[face];
  const uint16_t channels = r->info.channels;
  const size_t bpc = lptex_type_size(r->info.type);
  const size_t pixel_bytes = (size_t)channels * bpc;
  const lptex_level* li = &r->levels[level];
  const uint8_t* level_bytes;
  uint8_t* header = NULL;
  uint8_t* planar = NULL;
  size_t level_base, payload, compressed_size, i, header_bytes = 0;
  uint32_t ordinal, packed, encoding;
  lptex_status st;

  if ((fi->flags & LPTEX_FACE_FLAG_CONSTANT) || (w == 1 && h == 1)) {
    size_t src = 0;
    if (!mul_sz(face, pixel_bytes, &src) ||
        !in_range(src, pixel_bytes, r->constant_size))
      return LPTEX_E_CONSTANT_FACE_TRUNCATED;
    for (i = 0; i < bytes; i += pixel_bytes)
      memcpy(out + i, r->constant_data + src, pixel_bytes);
    return LPTEX_OK;
  }
  if (!li->faces) return reduce_previous(r, face, level, max_bytes, out, w, h);
  if (li->header_size == 0 || li->header_size > li->size)
    return LPTEX_E_LEVEL_BLOCK_HEADER_INVALID;
  /* parse() proved level_data_offset + all level sizes <= r->size. */
  level_base = r->level_data_offset + (size_t)li->offset;
  if (!in_range(level_base, (size_t)li->size, r->size))
    return LPTEX_E_LEVEL_BLOCK_TRUNCATED;
  level_bytes = r->data + level_base;
  if (!mul_sz(li->faces, 4u, &header_bytes)) return LPTEX_E_BLOCK_TOO_LARGE;
  ordinal = face_ordinal(r, face, level);
  if (ordinal >= li->faces) {
    /* The old reader inflated the level header before looking up the face,
     * so a broken header failed even for faces it then reduced. Keep that. */
    st = inflate_alloc(r, level_bytes, li->header_size, header_bytes, &header);
    if (st != LPTEX_OK) return st;
    r_free(r, header);
    return reduce_previous(r, face, level, max_bytes, out, w, h);
  }
  st = inflate_alloc(r, level_bytes, li->header_size, header_bytes, &header);
  if (st != LPTEX_OK) return st;
  payload = li->header_size;
  for (i = 0; i < ordinal; ++i) {
    const size_t n = ld32(header + i * 4u) & 0x3fffffffu;
    if (!add_sz(payload, n, &payload) || payload > li->size) {
      r_free(r, header);
      return LPTEX_E_FACE_PAYLOAD_OVERFLOW;
    }
  }
  packed = ld32(header + (size_t)ordinal * 4u);
  r_free(r, header);
  encoding = packed >> 30;
  compressed_size = packed & 0x3fffffffu;
  if (encoding == 0) {
    if (compressed_size < pixel_bytes ||
        !in_range(payload, pixel_bytes, (size_t)li->size))
      return LPTEX_E_LEVEL_CONSTANT_TRUNCATED;
    for (i = 0; i < bytes; i += pixel_bytes)
      memcpy(out + i, level_bytes + payload, pixel_bytes);
    return LPTEX_OK;
  }
  if (encoding == 3) {
    if (compressed_size < 6 ||
        !in_range(level_base + payload, compressed_size, r->size))
      return LPTEX_E_TILE_HEADER_TRUNCATED;
    return decode_tiled(r, level_bytes + payload, compressed_size, w, h, out,
                        bytes);
  }
  if (!in_range(level_base + payload, compressed_size, r->size))
    return LPTEX_E_FACE_PAYLOAD_TRUNCATED;
  if (channels == 1) {
    st = inflate_exact(level_bytes + payload, compressed_size, out, bytes);
    if (st == LPTEX_OK && encoding == 2)
      decode_difference(out, bytes, r->info.type);
    return st;
  }
  st = inflate_alloc(r, level_bytes + payload, compressed_size, bytes, &planar);
  if (st != LPTEX_OK) return st;
  if (encoding == 2) decode_difference(planar, bytes, r->info.type);
  planar_to_interleaved(planar, w, h, channels, bpc, out);
  r_free(r, planar);
  return LPTEX_OK;
}

lptex_status lptex_read_face_into(const lptex_reader* r, uint32_t face,
                                  uint32_t level, size_t max_bytes, void* dst,
                                  size_t dst_size) {
  uint32_t w = 0, h = 0;
  size_t bytes = 0;
  lptex_status st = lptex_face_dims(r, face, level, &w, &h, &bytes);
  if (st != LPTEX_OK) return st;
  if (bytes > max_bytes) return LPTEX_E_DECODE_BUDGET;
  if (!dst || dst_size < bytes) return LPTEX_E_BUFFER_TOO_SMALL;
  return decode_face(r, face, level, max_bytes, (uint8_t*)dst, w, h, bytes);
}

lptex_status lptex_read_face(const lptex_reader* r, uint32_t face,
                             uint32_t level, size_t max_bytes,
                             lptex_image* out) {
  uint32_t w = 0, h = 0;
  size_t bytes = 0;
  lptex_status st;
  uint8_t* data;
  if (!out) return LPTEX_E_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  st = lptex_face_dims(r, face, level, &w, &h, &bytes);
  if (st != LPTEX_OK) return st;
  if (bytes > max_bytes) return LPTEX_E_DECODE_BUDGET;
  data = (uint8_t*)r_alloc(r, bytes);
  if (!data) return LPTEX_E_OUT_OF_MEMORY;
  st = decode_face(r, face, level, max_bytes, data, w, h, bytes);
  if (st != LPTEX_OK) {
    r_free(r, data);
    return st;
  }
  out->width = w;
  out->height = h;
  out->channels = r->info.channels;
  out->type = r->info.type;
  out->data = data;
  out->size = bytes;
  return LPTEX_OK;
}

void lptex_image_free(const lptex_reader* r, lptex_image* image) {
  if (!image) return;
  if (r && image->data) r_free(r, image->data);
  memset(image, 0, sizeof(*image));
}
