/* SPDX-License-Identifier: Apache-2.0 */
/*
 * lptex: a small, portable C11 reader for Ptex (.ptx) containers.
 *
 * - No global state, no exceptions, no volatile or atomics.
 * - Every size read from the file is bounds- and overflow-checked.
 * - The reader is immutable after lptex_open_memory(), so lptex_read_face()
 *   may be called concurrently on one reader without locks.
 * - Memory comes from a caller-supplied allocator (NULL means malloc/free).
 * - The core is memory-only. lptex_open_file() lives in lptex-file.c and is
 *   compiled only when file IO is enabled.
 *
 * See doc/ptex.md for the format coverage and guarantees.
 */
#ifndef LPTEX_H_
#define LPTEX_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lptex_type {
  LPTEX_TYPE_U8 = 0,
  LPTEX_TYPE_U16 = 1,
  LPTEX_TYPE_HALF = 2,
  LPTEX_TYPE_F32 = 3
} lptex_type;

typedef enum lptex_status {
  LPTEX_OK = 0,
  LPTEX_E_INVALID_ARGUMENT,
  LPTEX_E_OUT_OF_MEMORY,
  LPTEX_E_OPEN_FAILED,
  LPTEX_E_EMPTY_FILE,
  LPTEX_E_IO,
  LPTEX_E_FILE_TOO_LARGE,
  LPTEX_E_HEADER_TRUNCATED,
  LPTEX_E_BAD_MAGIC,
  LPTEX_E_UNSUPPORTED_VERSION,
  LPTEX_E_UNSUPPORTED_DATA_TYPE,
  LPTEX_E_INVALID_DIMENSIONS,
  LPTEX_E_FACE_TABLE_TOO_LARGE,
  LPTEX_E_EXT_HEADER_TRUNCATED,
  LPTEX_E_FACE_INFO_INVALID,
  LPTEX_E_FACE_RESOLUTION_INVALID,
  LPTEX_E_CONSTANT_BLOCK_INVALID,
  LPTEX_E_LEVEL_INFO_INVALID,
  LPTEX_E_LEVEL_HEADER_INVALID,
  LPTEX_E_LEVEL_FACE_COUNT_INVALID,
  LPTEX_E_LEVEL_DATA_OVERFLOW,
  LPTEX_E_LEVEL_DATA_TRUNCATED,
  LPTEX_E_BLOCK_TOO_LARGE,
  LPTEX_E_INFLATE_FAILED,
  LPTEX_E_INVALID_FACE_LEVEL,
  LPTEX_E_DECODE_BUDGET,
  LPTEX_E_CONSTANT_FACE_TRUNCATED,
  LPTEX_E_FACE_ABSENT_FROM_BASE,
  LPTEX_E_REDUCTION_UNSUPPORTED,
  LPTEX_E_REDUCTION_DIMENSIONS,
  LPTEX_E_LEVEL_BLOCK_HEADER_INVALID,
  LPTEX_E_LEVEL_BLOCK_TRUNCATED,
  LPTEX_E_FACE_PAYLOAD_OVERFLOW,
  LPTEX_E_LEVEL_CONSTANT_TRUNCATED,
  LPTEX_E_TILE_HEADER_TRUNCATED,
  LPTEX_E_TILE_RESOLUTION_INVALID,
  LPTEX_E_TILE_TABLE_INVALID,
  LPTEX_E_TILE_PAYLOAD_OVERFLOW,
  LPTEX_E_TILE_CONSTANT_TRUNCATED,
  LPTEX_E_UNSUPPORTED_TILE_ENCODING,
  LPTEX_E_UNSUPPORTED_FACE_ENCODING,
  LPTEX_E_FACE_PAYLOAD_TRUNCATED,
  LPTEX_E_PLANAR_SIZE_MISMATCH,
  LPTEX_E_BUFFER_TOO_SMALL
} lptex_status;

/* Static, never-NULL description of a status code. */
const char* lptex_status_str(lptex_status status);

typedef struct lptex_allocator {
  void* (*alloc)(void* user, size_t size); /* NULL on failure */
  void (*free)(void* user, void* ptr);     /* ptr may be NULL */
  void* user;
} lptex_allocator;

typedef struct lptex_options {
  /* NULL alloc/free means malloc/free. */
  lptex_allocator allocator;
} lptex_options;

#define LPTEX_NO_FACE 0xffffffffu
#define LPTEX_FACE_FLAG_CONSTANT 1u
#define LPTEX_FACE_FLAG_SUBFACE 8u

typedef struct lptex_face_info {
  uint8_t ures; /* log2 width, <= 30 */
  uint8_t vres; /* log2 height, <= 30 */
  uint8_t adj_edges;
  uint8_t flags;
  uint32_t adj_faces[4]; /* LPTEX_NO_FACE when absent */
} lptex_face_info;

typedef struct lptex_info {
  lptex_type type;
  uint32_t mesh_type; /* 0 triangle, 1 quad */
  uint32_t alpha_channel;
  uint16_t channels;
  uint16_t levels;
  uint32_t faces;
  const lptex_face_info* face_info; /* faces entries, owned by the reader */
} lptex_info;

/* Decoded face: channels interleaved, rows v-major starting at v=0. */
typedef struct lptex_image {
  uint32_t width;
  uint32_t height;
  uint16_t channels;
  lptex_type type;
  uint8_t* data;
  size_t size; /* bytes */
} lptex_image;

typedef struct lptex_reader lptex_reader;

size_t lptex_type_size(lptex_type type);

/* Parse a container. The reader borrows `data`; it must outlive the reader.
 * `options` may be NULL. On failure *out is NULL. */
lptex_status lptex_open_memory(const void* data, size_t size,
                               const lptex_options* options,
                               lptex_reader** out);

#ifndef LPTEX_NO_FILEIO
/* Read (or map) a whole file; the reader owns the bytes. In lptex-file.c. */
lptex_status lptex_open_file(const char* path, const lptex_options* options,
                             lptex_reader** out);
#endif

void lptex_close(lptex_reader* reader); /* NULL is a no-op */

const lptex_info* lptex_reader_info(const lptex_reader* reader);

/* Dimensions and byte size of one face at one level. */
lptex_status lptex_face_dims(const lptex_reader* reader, uint32_t face,
                             uint32_t level, uint32_t* width,
                             uint32_t* height, size_t* bytes);

/* Decode into caller memory of exactly lptex_face_dims() bytes (dst_size may
 * be larger). Fails with LPTEX_E_DECODE_BUDGET when the face exceeds
 * max_bytes. Scratch memory comes from the reader's allocator. */
lptex_status lptex_read_face_into(const lptex_reader* reader, uint32_t face,
                                  uint32_t level, size_t max_bytes, void* dst,
                                  size_t dst_size);

/* Decode into a newly allocated image; release it with lptex_image_free(). */
lptex_status lptex_read_face(const lptex_reader* reader, uint32_t face,
                             uint32_t level, size_t max_bytes,
                             lptex_image* out);
void lptex_image_free(const lptex_reader* reader, lptex_image* image);

#ifdef __cplusplus
}
#endif

#endif /* LPTEX_H_ */
