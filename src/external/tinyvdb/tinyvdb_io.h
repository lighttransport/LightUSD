/*
 * TinyVDB I/O — Header-only C11 OpenVDB I/O library (part of TinyVDB).
 *
 * Copyright (c) 2026 - Present Syoyo Fujita
 * Copyright Contributors to the OpenVDB Project (original I/O logic)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Usage:
 *   #define TINYVDB_IO_IMPLEMENTATION
 *   #include "tinyvdb_io.h"
 *
 * Compile flags:
 *   TVDB_USE_SYSTEM_ZLIB  — Use system zlib instead of bundled miniz
 *   TVDB_NO_MMAP          — Disable mmap, always read into heap buffer
 *
 * BLOSC compression (LZ4) is always available — no external dependency needed.
 */
#ifndef TINYVDB_IO_H_
#define TINYVDB_IO_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*  Compile-time configuration                                                */
/* ========================================================================== */

#ifndef TVDB_MAX_TREE_DEPTH
#define TVDB_MAX_TREE_DEPTH 8
#endif

#ifndef TVDB_MAX_ERROR_MSG
#define TVDB_MAX_ERROR_MSG 512
#endif

/* ========================================================================== */
/*  Status codes                                                              */
/* ========================================================================== */

#ifndef TVDB_STATUS_T_DEFINED
#define TVDB_STATUS_T_DEFINED
typedef enum tvdb_status {
    TVDB_OK = 0,
    TVDB_ERROR_INVALID_FILE,
    TVDB_ERROR_INVALID_HEADER,
    TVDB_ERROR_INVALID_DATA,
    TVDB_ERROR_INVALID_ARGUMENT,
    TVDB_ERROR_UNSUPPORTED_VERSION,
    TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
    TVDB_ERROR_UNSUPPORTED_COMPRESSION,
    TVDB_ERROR_UNSUPPORTED_TRANSFORM,
    TVDB_ERROR_DECOMPRESSION_FAILED,
    TVDB_ERROR_OUT_OF_MEMORY,
    TVDB_ERROR_IO,
    TVDB_ERROR_MMAP_FAILED,
    TVDB_ERROR_PATH_CONVERSION,
    TVDB_ERROR_UNIMPLEMENTED
} tvdb_status_t;

/* ========================================================================== */
/*  Error context                                                             */
/* ========================================================================== */

typedef struct tvdb_error {
    tvdb_status_t status;
    char          message[TVDB_MAX_ERROR_MSG];
    uint64_t      byte_offset;
    int32_t       grid_index;
} tvdb_error_t;
#endif /* TVDB_STATUS_T_DEFINED */

/* ========================================================================== */
/*  Custom memory allocator                                                   */
/* ========================================================================== */

typedef struct tvdb_allocator {
    void *(*malloc_fn)(size_t size, void *user_ctx);
    void *(*realloc_fn)(void *ptr, size_t old_size, size_t new_size,
                        void *user_ctx);
    void (*free_fn)(void *ptr, size_t size, void *user_ctx);
    void *user_ctx;
} tvdb_allocator_t;

/* ========================================================================== */
/*  Value types                                                               */
/* ========================================================================== */

typedef enum tvdb_value_type {
    TVDB_VALUE_NULL = 0,
    TVDB_VALUE_BOOL,
    TVDB_VALUE_INT32,
    TVDB_VALUE_INT64,
    TVDB_VALUE_FLOAT,
    TVDB_VALUE_DOUBLE,
    TVDB_VALUE_HALF,
    TVDB_VALUE_VEC3I,
    TVDB_VALUE_VEC3F,
    TVDB_VALUE_VEC3D,
    TVDB_VALUE_STRING
} tvdb_value_type_t;

typedef struct tvdb_value {
    tvdb_value_type_t type;
    union {
        int       b;
        int32_t   i32;
        int64_t   i64;
        float     f;
        double    d;
        int32_t   vec3i[3];
        float     vec3f[3];
        double    vec3d[3];
        struct {
            char  *str;
            size_t len;
        } s;
    } u;
} tvdb_value_t;

/* ========================================================================== */
/*  Node / tree types                                                         */
/* ========================================================================== */

typedef enum tvdb_node_type {
    TVDB_NODE_ROOT = 0,
    TVDB_NODE_INTERNAL,
    TVDB_NODE_LEAF
} tvdb_node_type_t;

/* ========================================================================== */
/*  Bitset & node mask                                                        */
/* ========================================================================== */

typedef struct tvdb_bitset {
    uint8_t         *data;
    size_t           num_bits;
    size_t           num_bytes;
    tvdb_allocator_t *alloc;
} tvdb_bitset_t;

typedef struct tvdb_nodemask {
    tvdb_bitset_t bits;
    int32_t       log2dim;
    int32_t       bitsize; /* 1 << (3 * log2dim) */
} tvdb_nodemask_t;

/* ========================================================================== */
/*  Metadata                                                                  */
/* ========================================================================== */

typedef struct tvdb_meta_entry {
    char            *name;
    char            *type_name;
    tvdb_value_t     value;
    uint8_t         *raw_data;
    size_t           raw_data_len;
} tvdb_meta_entry_t;

typedef struct tvdb_metadata {
    tvdb_meta_entry_t *entries;
    size_t             count;
    size_t             capacity;
    tvdb_allocator_t  *alloc;
} tvdb_metadata_t;

/* ========================================================================== */
/*  Grid descriptor                                                           */
/* ========================================================================== */

typedef struct tvdb_grid_descriptor {
    char    *grid_name;
    char    *unique_name;
    char    *grid_type;
    char    *instance_parent_name;
    int      save_float_as_half;
    uint64_t grid_byte_offset;
    uint64_t block_byte_offset;
    uint64_t end_byte_offset;
} tvdb_grid_descriptor_t;

/* ========================================================================== */
/*  Header                                                                    */
/* ========================================================================== */

typedef struct tvdb_header {
    uint32_t file_version;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t compression_flags;
    int      has_grid_offsets;
    int      half_precision;
    char     uuid[37]; /* 36 chars + NUL */
    uint64_t offset_to_data;
} tvdb_header_t;

/* ========================================================================== */
/*  Transform                                                                 */
/* ========================================================================== */

typedef enum tvdb_transform_type {
    TVDB_TRANSFORM_UNIFORM_SCALE,
    TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE,
    TVDB_TRANSFORM_SCALE,
    TVDB_TRANSFORM_SCALE_TRANSLATE,
    TVDB_TRANSFORM_TRANSLATION,
    TVDB_TRANSFORM_AFFINE,
    TVDB_TRANSFORM_UNKNOWN
} tvdb_transform_type_t;

typedef struct tvdb_transform {
    tvdb_transform_type_t type;
    double scale_values[3];
    double voxel_size[3];
    double translation[3];
    double matrix[4][4]; /* for AffineMap: row-major 4x4 */
} tvdb_transform_t;

/* ========================================================================== */
/*  Grid layout                                                               */
/* ========================================================================== */

typedef struct tvdb_node_info {
    tvdb_node_type_t  node_type;
    tvdb_value_type_t value_type;
    int32_t           log2dim;
} tvdb_node_info_t;

typedef struct tvdb_grid_layout {
    tvdb_node_info_t levels[TVDB_MAX_TREE_DEPTH];
    int              num_levels;
} tvdb_grid_layout_t;

/* ========================================================================== */
/*  Tree nodes                                                                */
/* ========================================================================== */

typedef struct tvdb_root_node {
    tvdb_value_t  background;
    uint32_t      num_tiles;
    uint32_t      num_children;
    int32_t      *tile_origins;   /* [num_tiles * 3] */
    tvdb_value_t *tile_values;
    int          *tile_active;
    int32_t      *child_origins;  /* [num_children * 3] */
    size_t       *child_indices;  /* indices into tree.nodes[] */
} tvdb_root_node_t;

typedef struct tvdb_internal_node {
    tvdb_nodemask_t child_mask;
    tvdb_nodemask_t value_mask;
    uint8_t        *values;
    size_t          values_size;
    size_t         *child_indices; /* sparse: count_on(child_mask) entries */
    size_t          num_children;
} tvdb_internal_node_t;

typedef struct tvdb_leaf_node {
    tvdb_nodemask_t value_mask;
    uint8_t        *data;
    size_t          data_size;
    uint32_t        num_voxels;
    /* PointIndexGrid leaf payload (OpenVDB tools::PointIndexLeafNode). */
    int32_t        *point_indices;
    uint64_t        num_point_indices;
    uint8_t        *point_aux_data;
    uint64_t        point_aux_data_size;
} tvdb_leaf_node_t;

typedef struct tvdb_tree_node {
    tvdb_node_type_t type;
    int              level;
    int32_t          origin[3];
    unsigned char    pooled;  /* values/data carved from tree->pool */
    union {
        tvdb_root_node_t     root;
        tvdb_internal_node_t internal;
        tvdb_leaf_node_t     leaf;
    } u;
} tvdb_tree_node_t;

/* Chunked allocation pool for node payloads built by the from-sparse builder.
 * Chunks are never moved, so carved pointers stay valid; destroy walks the
 * chunk list. */
typedef struct tvdb_pool_chunk {
    struct tvdb_pool_chunk *next;
    size_t                  size;
    size_t                  used;
} tvdb_pool_chunk_t;

typedef struct tvdb_tree {
    tvdb_tree_node_t *nodes;
    size_t            num_nodes;
    size_t            nodes_capacity;
    tvdb_grid_layout_t layout;
    int               is_point_data_grid;
    int               is_point_index_grid;
    tvdb_allocator_t  *alloc;
    tvdb_pool_chunk_t *pool;  /* from-sparse builder payload arena */
} tvdb_tree_t;

/* ========================================================================== */
/*  Grid                                                                      */
/* ========================================================================== */

typedef struct tvdb_grid {
    tvdb_grid_descriptor_t descriptor;
    tvdb_metadata_t        metadata;
    tvdb_transform_t       transform;
    tvdb_tree_t            tree;
    uint32_t               compression_flags;
    /* Raw PointDataGrid payload (treebase+topology+buffers), preserved
       opaquely for round-trip serialization. */
    uint8_t               *point_data_blob;
    size_t                 point_data_blob_size;
} tvdb_grid_t;

/* ========================================================================== */
/*  mmap                                                                      */
/* ========================================================================== */

typedef struct tvdb_mmap {
    const uint8_t *data;
    uint64_t       mapped_len;
    uint64_t       file_size;
#if defined(_WIN32)
    void          *file_handle_;
    void          *map_handle_;
    void          *base_addr_;
#else
    int            fd_;
    void          *base_addr_;
    uint64_t       base_len_;
#endif
} tvdb_mmap_t;

/* ========================================================================== */
/*  File data source                                                          */
/* ========================================================================== */

typedef enum tvdb_source_type {
    TVDB_SOURCE_NONE = 0,
    TVDB_SOURCE_MMAP,
    TVDB_SOURCE_BUFFER,
    TVDB_SOURCE_EXTERNAL
} tvdb_source_type_t;

typedef struct tvdb_file_data {
    const uint8_t     *data;
    uint64_t           data_len;
    tvdb_source_type_t source;
    tvdb_mmap_t        mmap;
    uint8_t           *buffer;
    tvdb_allocator_t   alloc;
} tvdb_file_data_t;

/* ========================================================================== */
/*  Top-level file context                                                    */
/* ========================================================================== */

typedef struct tvdb_file {
    tvdb_header_t    header;
    tvdb_metadata_t  file_metadata;
    tvdb_grid_t     *grids;
    size_t           num_grids;
    tvdb_allocator_t alloc;
    tvdb_file_data_t file_data;
} tvdb_file_t;

/* ========================================================================== */
/*  Compression flags (public constants)                                      */
/* ========================================================================== */

#define TVDB_COMPRESS_NONE        0x0
#define TVDB_COMPRESS_ZIP         0x1
#define TVDB_COMPRESS_ACTIVE_MASK 0x2
#define TVDB_COMPRESS_BLOSC       0x4

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

static inline void tvdb_grid_set_background(tvdb_grid_t *grid, tvdb_value_t background) {
    if (!grid) return;
    tvdb_root_node_t *root = &grid->tree.nodes[0].u.root;
    root->background = background;
}

static inline void tvdb_nodemask_set_on(tvdb_nodemask_t *m, int32_t i) {
    if (!m || i < 0 || i >= (1 << (3 * m->log2dim))) return;
    m->bits.data[i >> 3] |= (1 << (i & 7));
}

static inline void tvdb_nodemask_set_off(tvdb_nodemask_t *m, int32_t i) {
    if (!m || i < 0 || i >= (1 << (3 * m->log2dim))) return;
    m->bits.data[i >> 3] &= ~(1 << (i & 7));
}

/* Nodemask helpers (for accessing tree data from application code) */
int    tvdb_nodemask_is_on(const tvdb_nodemask_t *m, int32_t i);
size_t tvdb_nodemask_count_on(const tvdb_nodemask_t *m);

/* Open a VDB file and parse its header, metadata and grid descriptors.
   On failure everything allocated so far is released and *file is left
   zeroed (tvdb_file_close on it is a harmless no-op). Malformed, truncated
   or oversized inputs fail with TVDB_ERROR_INVALID_DATA before any
   header-sized allocation is attempted. */
tvdb_status_t tvdb_file_open(tvdb_file_t *file, const char *filepath_utf8,
                             const tvdb_allocator_t *alloc, tvdb_error_t *err);

tvdb_status_t tvdb_file_open_memory(tvdb_file_t *file, const uint8_t *data,
                                    size_t data_len,
                                    const tvdb_allocator_t *alloc,
                                    tvdb_error_t *err);

void tvdb_file_close(tvdb_file_t *file);

/* Destroy grid contents without freeing the grid itself. alloc owns descriptor
   strings and point_data_blob; NULL selects malloc/free. Tree and metadata use
   their recorded allocators. The grid is cleared and can be destroyed again. */
void tvdb_grid_destroy(tvdb_grid_t *grid, const tvdb_allocator_t *alloc);

/* Each successful read replaces that grid's payload using independent storage.
 * A failed read preserves that grid, including its descriptor, and other grids
 * are still attempted. Reload requires valid offsets into the original file. */
tvdb_status_t tvdb_read_all_grids(tvdb_file_t *file, tvdb_error_t *err);

size_t        tvdb_grid_count(const tvdb_file_t *file);
const char   *tvdb_grid_name(const tvdb_file_t *file, size_t idx);
const char   *tvdb_grid_type_name(const tvdb_file_t *file, size_t idx);

const char   *tvdb_status_string(tvdb_status_t status);
int           tvdb_is_big_endian(void);

size_t        tvdb_value_type_size(tvdb_value_type_t type);

/* ---- Writing API ---- */

/* Write VDB data to a memory buffer.
   Caller must free *out_data with the file's allocator (or free() if default);
   the allocation is exactly *out_size bytes. Returns TVDB_ERROR_OUT_OF_MEMORY
   (and no buffer) if any part of the stream could not be allocated.
   Grids flagged save_float_as_half store float, double, Vec3s and Vec3d
   values as half precision, matching OpenVDB's _HalfFloat encoding.
   compression_flags: combination of TVDB_COMPRESS_* flags.
   compression_level: 1 (fastest) to 9 (best ratio), 0 for default (5). */
/* Initialize an empty file for writing caller-owned grids: header file
 * version 224 (OpenVDB 10.0), grid offsets on, the fixed UUID
 * 00000000-0000-0000-0000-000000000000 (so output is deterministic; set
 * header.uuid to override), and alloc (NULL selects malloc/free). Point
 * grids/num_grids at the grids, then call tvdb_write_to_memory or
 * tvdb_file_save. The file does not own the grids: do not tvdb_file_close it. */
void tvdb_file_init_for_write(tvdb_file_t *file, const tvdb_allocator_t *alloc);

tvdb_status_t tvdb_write_to_memory(const tvdb_file_t *file,
                                   uint32_t compression_flags,
                                   int compression_level,
                                   uint8_t **out_data, size_t *out_size,
                                   tvdb_error_t *err);

/* Write VDB data to a file.
   use_mmap: if nonzero, use mmap for file I/O (ignored if TVDB_NO_MMAP).
   compression_level: 1 (fastest) to 9 (best ratio), 0 for default (5). */
tvdb_status_t tvdb_file_save(const tvdb_file_t *file,
                             const char *filepath_utf8,
                             uint32_t compression_flags,
                             int compression_level,
                             int use_mmap,
                             tvdb_error_t *err);

/* ---- Point grid helpers ---- */
int            tvdb_grid_is_point_data(const tvdb_file_t *file, size_t idx);
int            tvdb_grid_is_point_index(const tvdb_file_t *file, size_t idx);
const uint8_t *tvdb_grid_point_data_blob(const tvdb_file_t *file, size_t idx);
size_t         tvdb_grid_point_data_blob_size(const tvdb_file_t *file, size_t idx);
tvdb_status_t  tvdb_grid_set_point_data_blob(tvdb_file_t *file, size_t idx,
                                             const uint8_t *data, size_t size,
                                             tvdb_error_t *err);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ========================================================================== */
/* ========================================================================== */
/*  IMPLEMENTATION                                                            */
/* ========================================================================== */
/* ========================================================================== */

#ifdef TINYVDB_IO_IMPLEMENTATION

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifndef TVDB_ASSERT
#define TVDB_ASSERT(x) assert(x)
#endif

/* -------------------------------------------------------------------------- */
/*  Platform detection                                                        */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32)
#  if defined(__MINGW32__)
#    include <windows.h>
#  else
#    include <Windows.h>
#  endif
#  include <io.h>
#  include <fcntl.h>
#endif

#if !defined(TVDB_NO_MMAP)
#  if defined(_WIN32)
     /* Windows file mapping already available from <Windows.h> */
#  else
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <fcntl.h>
#    include <unistd.h>
#    include <errno.h>
#  endif
#endif

/* Compression headers */
#if !defined(TVDB_USE_SYSTEM_ZLIB)
#  ifndef MINIZ_NO_STDIO
#    define MINIZ_NO_STDIO
#  endif
#  include "../miniz.h"
#else
#  include <zlib.h>
#endif

/* LZ4 for BLOSC frame decompression/compression */
#include "../../lz4/lz4.h"

#if defined(TVDB_USE_ZSTD)
#  include "../zstd.h"
#endif

/* ========================================================================== */
/*  VDB file format constants                                                 */
/* ========================================================================== */

#define TVDB_MAGIC 0x56444220 /* "VDB " */

enum {
    TVDB_FILE_VERSION_SELECTIVE_COMPRESSION = 220,
    TVDB_FILE_VERSION_FLOAT_FRUSTUM_BBOX   = 221,
    TVDB_FILE_VERSION_NODE_MASK_COMPRESSION = 222,
    TVDB_FILE_VERSION_BLOSC_COMPRESSION    = 223,
    TVDB_FILE_VERSION_POINT_INDEX_GRID     = 223,
    TVDB_FILE_VERSION_MULTIPASS_IO         = 224,
    TVDB_FILE_VERSION_HALF_GRID            = 225
};

/* Compression flags are defined as macros in the public header section above */

/* Active-mask compression per-node flags */
enum {
    TVDB_NO_MASK_OR_INACTIVE_VALS    = 0,
    TVDB_NO_MASK_AND_MINUS_BG        = 1,
    TVDB_NO_MASK_AND_ONE_INACTIVE_VAL = 2,
    TVDB_MASK_AND_NO_INACTIVE_VALS   = 3,
    TVDB_MASK_AND_ONE_INACTIVE_VAL   = 4,
    TVDB_MASK_AND_TWO_INACTIVE_VALS  = 5,
    TVDB_NO_MASK_AND_ALL_VALS        = 6
};

/* ========================================================================== */
/*  Internal helper: error formatting                                         */
/* ========================================================================== */

static void tvdb__set_error(tvdb_error_t *err, tvdb_status_t status,
                            const char *msg) {
    if (!err) return;
    err->status = status;
    if (msg) {
        size_t len = strlen(msg);
        if (len >= TVDB_MAX_ERROR_MSG) len = TVDB_MAX_ERROR_MSG - 1;
        memcpy(err->message, msg, len);
        err->message[len] = '\0';
    } else {
        err->message[0] = '\0';
    }
}

/* ========================================================================== */
/*  Default allocator                                                         */
/* ========================================================================== */

static void *tvdb__default_malloc(size_t size, void *ctx) {
    (void)ctx;
    return malloc(size);
}

static void *tvdb__default_realloc(void *ptr, size_t old_size, size_t new_size,
                                   void *ctx) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void tvdb__default_free(void *ptr, size_t size, void *ctx) {
    (void)ctx;
    (void)size;
    free(ptr);
}

static tvdb_allocator_t tvdb__default_allocator(void) {
    tvdb_allocator_t a;
    a.malloc_fn  = tvdb__default_malloc;
    a.realloc_fn = tvdb__default_realloc;
    a.free_fn    = tvdb__default_free;
    a.user_ctx   = NULL;
    return a;
}

/* Allocator wrappers */
static void *tvdb__alloc(tvdb_allocator_t *a, size_t size) {
    return a->malloc_fn(size, a->user_ctx);
}

static void *tvdb__realloc(tvdb_allocator_t *a, void *ptr,
                           size_t old_size, size_t new_size) {
    return a->realloc_fn(ptr, old_size, new_size, a->user_ctx);
}

static void tvdb__free(tvdb_allocator_t *a, void *ptr, size_t size) {
    if (ptr) a->free_fn(ptr, size, a->user_ctx);
}

/* Duplicate a null-terminated string using the allocator */
static char *tvdb__strdup(tvdb_allocator_t *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = (char *)tvdb__alloc(a, len + 1);
    if (d) { memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

/* Overflow-safe multiply for allocation sizes. Returns 0 on overflow. */
static size_t tvdb__safe_mul(size_t a_val, size_t b_val) {
    if (a_val == 0 || b_val == 0) return 0;
    if (a_val > SIZE_MAX / b_val) return 0; /* overflow */
    return a_val * b_val;
}

static char *tvdb__strndup(tvdb_allocator_t *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *d = (char *)tvdb__alloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = '\0'; }
    return d;
}

/* ========================================================================== */
/*  Bitset                                                                    */
/* ========================================================================== */

static void tvdb__bitset_init(tvdb_bitset_t *bs) {
    memset(bs, 0, sizeof(*bs));
}

static int tvdb__bitset_alloc(tvdb_bitset_t *bs, size_t num_bits,
                              tvdb_allocator_t *a) {
    bs->num_bits  = num_bits;
    bs->num_bytes = (num_bits + 7) / 8;
    bs->alloc     = a;
    bs->data      = (uint8_t *)tvdb__alloc(a, bs->num_bytes);
    if (!bs->data) return 0;
    memset(bs->data, 0, bs->num_bytes);
    return 1;
}

static void tvdb__bitset_destroy(tvdb_bitset_t *bs) {
    if (bs->data && bs->alloc) {
        tvdb__free(bs->alloc, bs->data, bs->num_bytes);
    }
    memset(bs, 0, sizeof(*bs));
}

static int tvdb__bitset_test(const tvdb_bitset_t *bs, size_t bit) {
    if (bit >= bs->num_bits) return 0;
    return (bs->data[bit / 8] >> (bit % 8)) & 1;
}

static size_t tvdb__bitset_count_on(const tvdb_bitset_t *bs) {
    /* Count via a popcount instead of the classic
     * `while (v) { v &= v-1; count++; }` loop, which costs a branch per set
     * bit. This runs once per compressed node and showed up as ~12% of load
     * time.
     *
     * The trailing partial byte is counted once, masked to num_bits, so
     * padding bits never inflate the result. */
    size_t count = 0;
    size_t full = bs->num_bits / 8u;      /* whole bytes */
    size_t tail = bs->num_bits & 7u;      /* bits in the last byte */
    for (size_t i = 0; i < full; ++i) {
        uint8_t v = bs->data[i];
        v = (uint8_t)(v - ((v >> 1) & 0x55));
        v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
        count += (size_t)((v + (v >> 4)) & 0x0F);
    }
    if (tail) {
        uint8_t v = (uint8_t)(bs->data[full] & ((1u << tail) - 1u));
        v = (uint8_t)(v - ((v >> 1) & 0x55));
        v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
        count += (size_t)((v + (v >> 4)) & 0x0F);
    }
    return count;
}

/* ========================================================================== */
/*  Node mask                                                                 */
/* ========================================================================== */

static void tvdb__nodemask_init(tvdb_nodemask_t *m) {
    memset(m, 0, sizeof(*m));
}

static int tvdb__nodemask_alloc(tvdb_nodemask_t *m, int32_t log2dim,
                                tvdb_allocator_t *a) {
    m->log2dim = log2dim;
    m->bitsize = 1 << (3 * log2dim);
    return tvdb__bitset_alloc(&m->bits, (size_t)m->bitsize, a);
}

static void tvdb__nodemask_destroy(tvdb_nodemask_t *m) {
    tvdb__bitset_destroy(&m->bits);
    m->log2dim = 0;
    m->bitsize = 0;
}

static int tvdb__nodemask_is_on(const tvdb_nodemask_t *m, int32_t i) {
    return tvdb__bitset_test(&m->bits, (size_t)i);
}

static size_t tvdb__nodemask_count_on(const tvdb_nodemask_t *m) {
    return tvdb__bitset_count_on(&m->bits);
}

static size_t tvdb__nodemask_mem_usage(const tvdb_nodemask_t *m) {
    return m->bits.num_bytes;
}

/* ========================================================================== */
/*  Endian detection & byte swap                                              */
/* ========================================================================== */

int tvdb_is_big_endian(void) {
    union { uint32_t i; char c[4]; } u;
    u.i = 0x01020304;
    return u.c[0] == 1;
}

static void tvdb__swap2(void *val) {
    uint8_t *p = (uint8_t *)val;
    uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
}

static void tvdb__swap4(void *val) {
    uint8_t *p = (uint8_t *)val;
    uint8_t t;
    t = p[0]; p[0] = p[3]; p[3] = t;
    t = p[1]; p[1] = p[2]; p[2] = t;
}

static void tvdb__swap8(void *val) {
    uint8_t *p = (uint8_t *)val;
    uint8_t t;
    t = p[0]; p[0] = p[7]; p[7] = t;
    t = p[1]; p[1] = p[6]; p[6] = t;
    t = p[2]; p[2] = p[5]; p[5] = t;
    t = p[3]; p[3] = p[4]; p[4] = t;
}

/* ========================================================================== */
/*  Half-float conversion (LE)                                                */
/* ========================================================================== */

static float tvdb__half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)(h & 0x3ffu);
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            /* denormalized */
            exp = 1;
            while (!(mant & 0x400u)) { mant <<= 1; exp++; }
            mant &= 0x3ffu;
            f = (sign << 31) | ((127 - 15 + 1 - exp) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000u | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float result;
    memcpy(&result, &f, 4);
    return result;
}

static uint16_t tvdb__float_to_half(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t sign = (u >> 31) & 1u;
    int32_t  exp  = (int32_t)((u >> 23) & 0xffu) - 127 + 15;
    uint32_t mant = u & 0x7fffffu;
    uint16_t h;
    if (exp <= 0) {
        if (exp < -10) { h = (uint16_t)(sign << 15); }
        else {
            mant |= 0x800000u;
            uint32_t shift = (uint32_t)(1 - exp);
            h = (uint16_t)((sign << 15) | (mant >> (13 + shift)));
        }
    } else if (exp >= 31) {
        h = (uint16_t)((sign << 15) | 0x7c00u); /* Inf */
    } else {
        h = (uint16_t)((sign << 15) | ((uint32_t)exp << 10) | (mant >> 13));
    }
    return h;
}

#if defined(TINYVDB_SIMD) && defined(__F16C__) && defined(__AVX2__)
  #include <immintrin.h>
  #define TVDB_IO_HAS_F16C 1
#else
  #define TVDB_IO_HAS_F16C 0
#endif

/* Promote an array of half-float values to float (expands buffer).
   `halfs` has `count` uint16_t values; `floats` must hold `count` floats.
   Buffers must NOT alias: callers always pass distinct buffers. */
static void tvdb__promote_half_to_float(const uint8_t *halfs, uint8_t *floats,
                                        size_t count) {
#if TVDB_IO_HAS_F16C
    /* 8-wide F16C path: VCVTPH2PS converts 8 halves to 8 floats per insn. */
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m128i h = _mm_loadu_si128((const __m128i *)(halfs + i * 2));
        __m256  f = _mm256_cvtph_ps(h);
        _mm256_storeu_ps((float *)(floats + i * 4), f);
    }
    for (; i < count; ++i) {
        uint16_t h;
        memcpy(&h, halfs + i * 2, 2);
        float fv = tvdb__half_to_float(h);
        memcpy(floats + i * 4, &fv, 4);
    }
#else
    /* Scalar: process backwards to allow in-place if floats == halfs. */
    for (size_t i = count; i > 0; i--) {
        uint16_t h;
        memcpy(&h, halfs + (i - 1) * 2, 2);
        float fv = tvdb__half_to_float(h);
        memcpy(floats + (i - 1) * 4, &fv, 4);
    }
#endif
}

/* Demote an array of float values to half-float.
   `floats` has `count` float values; `halfs` must hold `count` uint16_t. */
static void tvdb__demote_float_to_half(const uint8_t *floats, uint8_t *halfs,
                                       size_t count) {
#if TVDB_IO_HAS_F16C
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256  f = _mm256_loadu_ps((const float *)(floats + i * 4));
        __m128i h = _mm256_cvtps_ph(f, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
        _mm_storeu_si128((__m128i *)(halfs + i * 2), h);
    }
    for (; i < count; ++i) {
        float fv;
        memcpy(&fv, floats + i * 4, 4);
        uint16_t h = tvdb__float_to_half(fv);
        memcpy(halfs + i * 2, &h, 2);
    }
#else
    for (size_t i = 0; i < count; i++) {
        float fv;
        memcpy(&fv, floats + i * 4, 4);
        uint16_t h = tvdb__float_to_half(fv);
        memcpy(halfs + i * 2, &h, 2);
    }
#endif
}

/* ========================================================================== */
/*  Value type size                                                           */
/* ========================================================================== */

size_t tvdb_value_type_size(tvdb_value_type_t type) {
    switch (type) {
        case TVDB_VALUE_NULL:   return 0;
        case TVDB_VALUE_BOOL:   return 1;
        case TVDB_VALUE_INT32:  return 4;
        case TVDB_VALUE_INT64:  return 8;
        case TVDB_VALUE_FLOAT:  return 4;
        case TVDB_VALUE_DOUBLE: return 8;
        case TVDB_VALUE_HALF:   return 2;
        case TVDB_VALUE_VEC3I:  return 12;
        case TVDB_VALUE_VEC3F:  return 12;
        case TVDB_VALUE_VEC3D:  return 24;
        case TVDB_VALUE_STRING: return 0;
        default:                return 0;
    }
}

/* ========================================================================== */
/*  Stream reader                                                             */
/* ========================================================================== */

typedef struct tvdb__sr {
    const uint8_t *data;
    uint64_t       length;
    uint64_t       pos;
    int            swap_endian;
} tvdb__sr_t;

static void tvdb__sr_init(tvdb__sr_t *sr, const uint8_t *data, uint64_t length,
                          int swap_endian) {
    sr->data = data;
    sr->length = length;
    sr->pos = 0;
    sr->swap_endian = swap_endian;
}

static int tvdb__sr_seek_set(tvdb__sr_t *sr, uint64_t offset) {
    if (offset > sr->length) return 0;
    sr->pos = offset;
    return 1;
}

static int tvdb__sr_seek_cur(tvdb__sr_t *sr, int64_t offset) {
    int64_t new_pos = (int64_t)sr->pos + offset;
    if (new_pos < 0 || (uint64_t)new_pos > sr->length) return 0;
    sr->pos = (uint64_t)new_pos;
    return 1;
}

static int tvdb__sr_read(tvdb__sr_t *sr, size_t n, uint8_t *dst) {
    if (sr->pos + n > sr->length) return 0;
    memcpy(dst, sr->data + sr->pos, n);
    sr->pos += n;
    return 1;
}

static int tvdb__sr_read_u8(tvdb__sr_t *sr, uint8_t *out) {
    if (sr->pos + 1 > sr->length) return 0;
    *out = sr->data[sr->pos++];
    return 1;
}

static int tvdb__sr_read_i8(tvdb__sr_t *sr, int8_t *out) {
    return tvdb__sr_read_u8(sr, (uint8_t *)out);
}

static int tvdb__sr_read_u16(tvdb__sr_t *sr, uint16_t *out) {
    if (sr->pos + 2 > sr->length) return 0;
    memcpy(out, sr->data + sr->pos, 2);
    if (sr->swap_endian) tvdb__swap2(out);
    sr->pos += 2;
    return 1;
}

static int tvdb__sr_read_u32(tvdb__sr_t *sr, uint32_t *out) {
    if (sr->pos + 4 > sr->length) return 0;
    memcpy(out, sr->data + sr->pos, 4);
    if (sr->swap_endian) tvdb__swap4(out);
    sr->pos += 4;
    return 1;
}

static int tvdb__sr_read_i32(tvdb__sr_t *sr, int32_t *out) {
    return tvdb__sr_read_u32(sr, (uint32_t *)out);
}

static int tvdb__sr_read_u64(tvdb__sr_t *sr, uint64_t *out) {
    if (sr->pos + 8 > sr->length) return 0;
    memcpy(out, sr->data + sr->pos, 8);
    if (sr->swap_endian) tvdb__swap8(out);
    sr->pos += 8;
    return 1;
}

static int tvdb__sr_read_i64(tvdb__sr_t *sr, int64_t *out) {
    return tvdb__sr_read_u64(sr, (uint64_t *)out);
}

static int tvdb__sr_read_f32(tvdb__sr_t *sr, float *out) {
    uint32_t v;
    if (!tvdb__sr_read_u32(sr, &v)) return 0;
    memcpy(out, &v, 4);
    return 1;
}

static int tvdb__sr_read_f64(tvdb__sr_t *sr, double *out) {
    uint64_t v;
    if (!tvdb__sr_read_u64(sr, &v)) return 0;
    memcpy(out, &v, 8);
    return 1;
}

static int tvdb__sr_read_vec3d(tvdb__sr_t *sr, double out[3]) {
    return tvdb__sr_read_f64(sr, &out[0])
        && tvdb__sr_read_f64(sr, &out[1])
        && tvdb__sr_read_f64(sr, &out[2]);
}

/* Read a length-prefixed string (uint32_t length + chars, not null-terminated
   in file). Returns allocated null-terminated string. */
/* Strings returned here are released with strlen()+1, so an embedded NUL
   would make the free size disagree with the allocation size: reject it.
   Returns TVDB_ERROR_INVALID_DATA for truncation/NUL, OUT_OF_MEMORY for OOM. */
static tvdb_status_t tvdb__sr_read_cstring(tvdb__sr_t *sr, tvdb_allocator_t *a,
                                           char **out) {
    uint32_t len = 0;
    *out = NULL;
    if (!tvdb__sr_read_u32(sr, &len)) return TVDB_ERROR_INVALID_DATA;
    if ((uint64_t)len > sr->length - sr->pos) return TVDB_ERROR_INVALID_DATA;
    if (len && memchr(sr->data + sr->pos, 0, len)) return TVDB_ERROR_INVALID_DATA;
    *out = tvdb__strndup(a, (const char *)(sr->data + sr->pos), len);
    if (!*out) return TVDB_ERROR_OUT_OF_MEMORY;
    sr->pos += len;
    return TVDB_OK;
}


/* Read a typed value from the stream. The type must be set in out->type. */
static int tvdb__sr_read_value(tvdb__sr_t *sr, tvdb_value_type_t type,
                               tvdb_value_t *out) {
    out->type = type;
    switch (type) {
        case TVDB_VALUE_BOOL: {
            uint8_t v;
            if (!tvdb__sr_read_u8(sr, &v)) return 0;
            out->u.b = v ? 1 : 0;
            return 1;
        }
        case TVDB_VALUE_INT32:
            return tvdb__sr_read_i32(sr, &out->u.i32);
        case TVDB_VALUE_INT64:
            return tvdb__sr_read_i64(sr, &out->u.i64);
        case TVDB_VALUE_FLOAT:
            return tvdb__sr_read_f32(sr, &out->u.f);
        case TVDB_VALUE_DOUBLE:
            return tvdb__sr_read_f64(sr, &out->u.d);
        case TVDB_VALUE_HALF: {
            uint16_t h;
            if (!tvdb__sr_read_u16(sr, &h)) return 0;
            out->u.f = tvdb__half_to_float(h);
            out->type = TVDB_VALUE_FLOAT; /* promote to float */
            return 1;
        }
        case TVDB_VALUE_VEC3I:
            return tvdb__sr_read_i32(sr, &out->u.vec3i[0]) &&
                   tvdb__sr_read_i32(sr, &out->u.vec3i[1]) &&
                   tvdb__sr_read_i32(sr, &out->u.vec3i[2]);
        case TVDB_VALUE_VEC3F:
            return tvdb__sr_read_f32(sr, &out->u.vec3f[0]) &&
                   tvdb__sr_read_f32(sr, &out->u.vec3f[1]) &&
                   tvdb__sr_read_f32(sr, &out->u.vec3f[2]);
        case TVDB_VALUE_VEC3D:
            return tvdb__sr_read_f64(sr, &out->u.vec3d[0]) &&
                   tvdb__sr_read_f64(sr, &out->u.vec3d[1]) &&
                   tvdb__sr_read_f64(sr, &out->u.vec3d[2]);
        default:
            return 0;
    }
}

/* ========================================================================== */
/*  mmap                                                                      */
/* ========================================================================== */

static void tvdb__mmap_init(tvdb_mmap_t *m) {
    memset(m, 0, sizeof(*m));
#if !defined(_WIN32)
    m->fd_ = -1;
#endif
}

#if !defined(TVDB_NO_MMAP)

#if defined(_WIN32)

static tvdb_status_t tvdb__mmap_open(tvdb_mmap_t *m,
                                     const char *filepath_utf8,
                                     tvdb_error_t *err) {
    tvdb__mmap_init(m);

    /* Convert UTF-8 to wide chars */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, NULL, 0);
    if (wlen <= 0) {
        tvdb__set_error(err, TVDB_ERROR_PATH_CONVERSION,
                        "UTF-8 to wchar conversion failed");
        return TVDB_ERROR_PATH_CONVERSION;
    }
    wchar_t *wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wpath) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "Out of memory");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }
    MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, wpath, wlen);

    /* Prepend \\?\ for long path support if path > MAX_PATH */
    wchar_t *final_path = wpath;
    wchar_t *long_path = NULL;
    if (wlen > MAX_PATH && wcsncmp(wpath, L"\\\\?\\", 4) != 0) {
        size_t plen = (size_t)wlen + 4;
        long_path = (wchar_t *)malloc(sizeof(wchar_t) * plen);
        if (long_path) {
            wcscpy(long_path, L"\\\\?\\");
            wcscat(long_path, wpath);
            final_path = long_path;
        }
    }

    HANDLE hFile = CreateFileW(final_path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                               NULL);
    free(long_path);
    free(wpath);

    if (hFile == INVALID_HANDLE_VALUE) {
        tvdb__set_error(err, TVDB_ERROR_IO, "Failed to open file");
        return TVDB_ERROR_IO;
    }

    LARGE_INTEGER fsize;
    if (!GetFileSizeEx(hFile, &fsize)) {
        CloseHandle(hFile);
        tvdb__set_error(err, TVDB_ERROR_IO, "Failed to get file size");
        return TVDB_ERROR_IO;
    }

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED,
                        "CreateFileMapping failed");
        return TVDB_ERROR_MMAP_FAILED;
    }

    void *addr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!addr) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED, "MapViewOfFile failed");
        return TVDB_ERROR_MMAP_FAILED;
    }

    m->data         = (const uint8_t *)addr;
    m->mapped_len   = (uint64_t)fsize.QuadPart;
    m->file_size    = (uint64_t)fsize.QuadPart;
    m->file_handle_ = hFile;
    m->map_handle_  = hMap;
    m->base_addr_   = addr;
    return TVDB_OK;
}

static void tvdb__mmap_close(tvdb_mmap_t *m) {
    if (m->base_addr_) UnmapViewOfFile(m->base_addr_);
    if (m->map_handle_) CloseHandle(m->map_handle_);
    if (m->file_handle_) CloseHandle(m->file_handle_);
    tvdb__mmap_init(m);
}

#else /* Unix */

static tvdb_status_t tvdb__mmap_open(tvdb_mmap_t *m,
                                     const char *filepath_utf8,
                                     tvdb_error_t *err) {
    tvdb__mmap_init(m);

    int fd = open(filepath_utf8, O_RDONLY);
    if (fd < 0) {
        tvdb__set_error(err, TVDB_ERROR_IO, "Failed to open file");
        return TVDB_ERROR_IO;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        tvdb__set_error(err, TVDB_ERROR_IO, "fstat failed");
        return TVDB_ERROR_IO;
    }

    if (st.st_size == 0) {
        close(fd);
        tvdb__set_error(err, TVDB_ERROR_INVALID_FILE, "File is empty");
        return TVDB_ERROR_INVALID_FILE;
    }

    void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED, "mmap failed");
        return TVDB_ERROR_MMAP_FAILED;
    }

#ifdef MADV_SEQUENTIAL
    madvise(addr, (size_t)st.st_size, MADV_SEQUENTIAL);
#endif

    m->data       = (const uint8_t *)addr;
    m->mapped_len = (uint64_t)st.st_size;
    m->file_size  = (uint64_t)st.st_size;
    m->fd_        = fd;
    m->base_addr_ = addr;
    m->base_len_  = (uint64_t)st.st_size;
    return TVDB_OK;
}

static void tvdb__mmap_close(tvdb_mmap_t *m) {
    if (m->base_addr_) munmap(m->base_addr_, (size_t)m->base_len_);
    if (m->fd_ >= 0) close(m->fd_);
    tvdb__mmap_init(m);
}

#endif /* _WIN32 */

#else /* TVDB_NO_MMAP */

static tvdb_status_t tvdb__mmap_open(tvdb_mmap_t *m, const char *filepath_utf8,
                                     tvdb_error_t *err) {
    (void)m; (void)filepath_utf8;
    tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED, "mmap disabled at compile time");
    return TVDB_ERROR_MMAP_FAILED;
}

static void tvdb__mmap_close(tvdb_mmap_t *m) { (void)m; }

#endif /* TVDB_NO_MMAP */

/* ========================================================================== */
/*  File data open/close (mmap with heap-buffer fallback)                     */
/* ========================================================================== */

static tvdb_status_t tvdb__file_data_open(tvdb_file_data_t *fd,
                                          const char *filepath_utf8,
                                          tvdb_allocator_t *alloc,
                                          tvdb_error_t *err) {
    memset(fd, 0, sizeof(*fd));
    fd->alloc = *alloc;

    /* Try mmap first */
    tvdb_status_t st = tvdb__mmap_open(&fd->mmap, filepath_utf8, NULL);
    if (st == TVDB_OK) {
        fd->data     = fd->mmap.data;
        fd->data_len = fd->mmap.mapped_len;
        fd->source   = TVDB_SOURCE_MMAP;
        return TVDB_OK;
    }

    /* Fallback: read entire file into buffer */
#if defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, NULL, 0);
    if (wlen <= 0) {
        tvdb__set_error(err, TVDB_ERROR_PATH_CONVERSION,
                        "UTF-8 to wchar conversion failed");
        return TVDB_ERROR_PATH_CONVERSION;
    }
    wchar_t *wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, wpath, wlen);

    HANDLE hFile = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (hFile == INVALID_HANDLE_VALUE) {
        tvdb__set_error(err, TVDB_ERROR_IO, "Failed to open file");
        return TVDB_ERROR_IO;
    }
    LARGE_INTEGER fsize;
    GetFileSizeEx(hFile, &fsize);
    uint64_t sz = (uint64_t)fsize.QuadPart;
#else
    FILE *fp = fopen(filepath_utf8, "rb");
    if (!fp) {
        tvdb__set_error(err, TVDB_ERROR_IO, "Failed to open file");
        return TVDB_ERROR_IO;
    }
    fseek(fp, 0, SEEK_END);
    long sz_l = ftell(fp);
    if (sz_l < 0) {
        fclose(fp);
        tvdb__set_error(err, TVDB_ERROR_IO, "ftell failed");
        return TVDB_ERROR_IO;
    }
    uint64_t sz = (uint64_t)sz_l;
    fseek(fp, 0, SEEK_SET);
#endif

    if (sz < 16) {
#if defined(_WIN32)
        CloseHandle(hFile);
#else
        fclose(fp);
#endif
        tvdb__set_error(err, TVDB_ERROR_INVALID_FILE, "File too small");
        return TVDB_ERROR_INVALID_FILE;
    }

    fd->buffer = (uint8_t *)tvdb__alloc(alloc, (size_t)sz);
    if (!fd->buffer) {
#if defined(_WIN32)
        CloseHandle(hFile);
#else
        fclose(fp);
#endif
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "Out of memory");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

#if defined(_WIN32)
    DWORD bytes_read = 0;
    ReadFile(hFile, fd->buffer, (DWORD)sz, &bytes_read, NULL);
    CloseHandle(hFile);
#else
    size_t nread = fread(fd->buffer, 1, (size_t)sz, fp);
    fclose(fp);
    (void)nread;
#endif

    fd->data     = fd->buffer;
    fd->data_len = sz;
    fd->source   = TVDB_SOURCE_BUFFER;
    return TVDB_OK;
}

static void tvdb__file_data_close(tvdb_file_data_t *fd) {
    if (fd->source == TVDB_SOURCE_MMAP) {
        tvdb__mmap_close(&fd->mmap);
    } else if (fd->source == TVDB_SOURCE_BUFFER && fd->buffer) {
        tvdb__free(&fd->alloc, fd->buffer, (size_t)fd->data_len);
    }
    memset(fd, 0, sizeof(*fd));
}

/* ========================================================================== */
/*  Metadata reading                                                          */
/* ========================================================================== */

static void tvdb__metadata_init(tvdb_metadata_t *m, tvdb_allocator_t *a) {
    memset(m, 0, sizeof(*m));
    m->alloc = a;
}

static void tvdb__meta_entry_destroy(tvdb_meta_entry_t *e, tvdb_allocator_t *a) {
    if (e->name) tvdb__free(a, e->name, strlen(e->name) + 1);
    if (e->type_name) tvdb__free(a, e->type_name, strlen(e->type_name) + 1);
    if (e->value.type == TVDB_VALUE_STRING && e->value.u.s.str)
        tvdb__free(a, e->value.u.s.str, e->value.u.s.len + 1);
    if (e->raw_data) tvdb__free(a, e->raw_data, e->raw_data_len);
    memset(e, 0, sizeof(*e));
}

static void tvdb__metadata_destroy(tvdb_metadata_t *m) {
    if (!m->alloc) return;
    for (size_t i = 0; i < m->count; ++i)
        tvdb__meta_entry_destroy(&m->entries[i], m->alloc);
    if (m->entries)
        tvdb__free(m->alloc, m->entries, m->capacity * sizeof(tvdb_meta_entry_t));
    memset(m, 0, sizeof(*m));
}

static int tvdb__metadata_push(tvdb_metadata_t *m, tvdb_meta_entry_t *e) {
    if (m->count >= m->capacity) {
        size_t new_cap = m->capacity ? m->capacity * 2 : 8;
        tvdb_meta_entry_t *new_entries = (tvdb_meta_entry_t *)tvdb__realloc(
            m->alloc, m->entries,
            m->capacity * sizeof(tvdb_meta_entry_t),
            new_cap * sizeof(tvdb_meta_entry_t));
        if (!new_entries) return 0;
        m->entries  = new_entries;
        m->capacity = new_cap;
    }
    m->entries[m->count++] = *e;
    return 1;
}

/* Validate lengths before allocation so truncation and OOM stay distinct. */
static tvdb_status_t tvdb__meta_string(tvdb__sr_t *sr, tvdb_allocator_t *a,
                                      char **out, size_t *length) {
    uint32_t n;
    if (!tvdb__sr_read_u32(sr, &n) || n > sr->length - sr->pos ||
        (uint64_t)n + 1 > SIZE_MAX) return TVDB_ERROR_INVALID_DATA;
    /* Names have no stored allocation length and must be C strings. */
    if (!length && memchr(sr->data + sr->pos, 0, n)) return TVDB_ERROR_INVALID_DATA;
    *out = tvdb__strndup(a, (const char *)sr->data + sr->pos, n);
    if (!*out) return TVDB_ERROR_OUT_OF_MEMORY;
    sr->pos += n;
    if (length) *length = n;
    return TVDB_OK;
}

static tvdb_status_t tvdb__read_meta(tvdb__sr_t *sr, tvdb_metadata_t *meta,
                                     tvdb_allocator_t *alloc,
                                     tvdb_error_t *err) {
    int32_t count = 0;
    if (!tvdb__sr_read_i32(sr, &count) || count < 0 || count > 4096) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Invalid metadata count");
        return TVDB_ERROR_INVALID_DATA;
    }
    for (int32_t i = 0; i < count; ++i) {
        tvdb_meta_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        tvdb_status_t st = tvdb__meta_string(sr, alloc, &entry.name, NULL);
        if (st != TVDB_OK) goto fail;
        st = tvdb__meta_string(sr, alloc, &entry.type_name, NULL);
        if (st != TVDB_OK) goto fail;
        if (strcmp(entry.type_name, "string") == 0) {
            entry.value.type = TVDB_VALUE_STRING;
            size_t length = 0;
            st = tvdb__meta_string(sr, alloc, &entry.value.u.s.str, &length);
            if (st != TVDB_OK) goto fail;
            entry.value.u.s.len = length;
        } else {
            int32_t size;
            st = TVDB_ERROR_INVALID_DATA;
            if (!tvdb__sr_read_i32(sr, &size) || size < 0 ||
                (uint64_t)size > sr->length - sr->pos) goto fail;
            tvdb_value_type_t type = TVDB_VALUE_NULL;
            if (!strcmp(entry.type_name,"bool")) type = TVDB_VALUE_BOOL;
            else if (!strcmp(entry.type_name,"float")) type = TVDB_VALUE_FLOAT;
            else if (!strcmp(entry.type_name,"double")) type = TVDB_VALUE_DOUBLE;
            else if (!strcmp(entry.type_name,"int32")) type = TVDB_VALUE_INT32;
            else if (!strcmp(entry.type_name,"int64")) type = TVDB_VALUE_INT64;
            else if (!strcmp(entry.type_name,"vec3i")) type = TVDB_VALUE_VEC3I;
            else if (!strcmp(entry.type_name,"vec3d")) type = TVDB_VALUE_VEC3D;
            if (type != TVDB_VALUE_NULL) {
                if ((size_t)size != tvdb_value_type_size(type) ||
                    !tvdb__sr_read_value(sr, type, &entry.value)) goto fail;
            } else if (size) {
                entry.raw_data = (uint8_t *)tvdb__alloc(alloc, (size_t)size);
                if (!entry.raw_data) { st = TVDB_ERROR_OUT_OF_MEMORY; goto fail; }
                entry.raw_data_len = (size_t)size;
                if (!tvdb__sr_read(sr, (size_t)size, entry.raw_data)) goto fail;
            }
        }
        if (meta) {
            if (!tvdb__metadata_push(meta, &entry)) {
                st = TVDB_ERROR_OUT_OF_MEMORY;
                goto fail;
            }
        } else tvdb__meta_entry_destroy(&entry, alloc);
        continue;
fail:
        tvdb__meta_entry_destroy(&entry, alloc);
        tvdb__set_error(err, st, st == TVDB_ERROR_OUT_OF_MEMORY ?
                        "Metadata allocation failed" : "Invalid metadata entry");
        return st;
    }
    return TVDB_OK;
}

/* ========================================================================== */
/*  Grid descriptor reading                                                   */
/* ========================================================================== */

static int tvdb__ends_with(const char *str, const char *suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return 0;
    return memcmp(str + slen - xlen, suffix, xlen) == 0;
}

static char *tvdb__strip_suffix(const char *name, char sep,
                                tvdb_allocator_t *a) {
    const char *p = strchr(name, sep);
    if (p) return tvdb__strndup(a, name, (size_t)(p - name));
    return tvdb__strdup(a, name);
}

static tvdb_status_t tvdb__read_grid_descriptor(
    tvdb__sr_t *sr, uint32_t file_version, tvdb_grid_descriptor_t *gd,
    tvdb_allocator_t *alloc, tvdb_error_t *err) {
    memset(gd, 0, sizeof(*gd));

    tvdb_status_t st = tvdb__sr_read_cstring(sr, alloc, &gd->unique_name);
    if (st != TVDB_OK) {
        tvdb__set_error(err, st, st == TVDB_ERROR_OUT_OF_MEMORY ? "OOM" :
                        "Failed to read grid unique name");
        return st;
    }
    gd->grid_name = tvdb__strip_suffix(gd->unique_name, '\x1e', alloc);
    if (!gd->grid_name) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

    st = tvdb__sr_read_cstring(sr, alloc, &gd->grid_type);
    if (st != TVDB_OK) {
        tvdb__set_error(err, st, st == TVDB_ERROR_OUT_OF_MEMORY ? "OOM" :
                        "Failed to read grid type");
        return st;
    }

    /* Strip _HalfFloat suffix */
    static const char *HALF_SUFFIX = "_HalfFloat";
    if (tvdb__ends_with(gd->grid_type, HALF_SUFFIX)) {
        gd->save_float_as_half = 1;
        size_t new_len = strlen(gd->grid_type) - strlen(HALF_SUFFIX);
        char *stripped = tvdb__strndup(alloc, gd->grid_type, new_len);
        if (!stripped) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        tvdb__free(alloc, gd->grid_type, strlen(gd->grid_type) + 1);
        gd->grid_type = stripped;
    }

    /* Instance parent names exist from file version 221 (grid instancing). */
    st = file_version >= TVDB_FILE_VERSION_FLOAT_FRUSTUM_BBOX
             ? tvdb__sr_read_cstring(sr, alloc, &gd->instance_parent_name)
             : TVDB_OK;
    if (st != TVDB_OK) {
        tvdb__set_error(err, st, st == TVDB_ERROR_OUT_OF_MEMORY ? "OOM" :
                        "Failed to read grid instance parent name");
        return st;
    }

    /* Read byte offsets */
    if (!tvdb__sr_read_u64(sr, &gd->grid_byte_offset) ||
        !tvdb__sr_read_u64(sr, &gd->block_byte_offset) ||
        !tvdb__sr_read_u64(sr, &gd->end_byte_offset)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read grid byte offsets");
        return TVDB_ERROR_INVALID_DATA;
    }

    return TVDB_OK;
}

static void tvdb__grid_descriptor_destroy(tvdb_grid_descriptor_t *gd,
                                          tvdb_allocator_t *a) {
    if (gd->grid_name) tvdb__free(a, gd->grid_name,
                                  strlen(gd->grid_name) + 1);
    if (gd->unique_name) tvdb__free(a, gd->unique_name,
                                    strlen(gd->unique_name) + 1);
    if (gd->grid_type) tvdb__free(a, gd->grid_type,
                                  strlen(gd->grid_type) + 1);
    if (gd->instance_parent_name)
        tvdb__free(a, gd->instance_parent_name,
                   strlen(gd->instance_parent_name) + 1);
    memset(gd, 0, sizeof(*gd));
}

/* ========================================================================== */
/*  Grid type string parsing → layout                                         */
/* ========================================================================== */

static tvdb_status_t tvdb__parse_grid_type(const char *grid_type,
                                           tvdb_grid_layout_t *layout,
                                           tvdb_error_t *err) {
    memset(layout, 0, sizeof(*layout));

    /* Expected format: "Tree_<valuetype>_<dim1>_<dim2>_<dim3>"
       e.g., "Tree_float_5_4_3" */
    if (strncmp(grid_type, "Tree_", 5) != 0) {
        tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                        "Unsupported grid type (no Tree_ prefix)");
        return TVDB_ERROR_UNSUPPORTED_GRID_TYPE;
    }

    const char *p = grid_type + 5;

    /* Parse value type */
    tvdb_value_type_t vtype = TVDB_VALUE_FLOAT;
    if (strncmp(p, "float_", 6) == 0) {
        vtype = TVDB_VALUE_FLOAT; p += 6;
    } else if (strncmp(p, "double_", 7) == 0) {
        vtype = TVDB_VALUE_DOUBLE; p += 7;
    } else if (strncmp(p, "int32_", 6) == 0) {
        vtype = TVDB_VALUE_INT32; p += 6;
    } else if (strncmp(p, "int64_", 6) == 0) {
        vtype = TVDB_VALUE_INT64; p += 6;
    } else if (strncmp(p, "bool_", 5) == 0) {
        vtype = TVDB_VALUE_BOOL; p += 5;
    } else if (strncmp(p, "vec3s_", 6) == 0) {
        vtype = TVDB_VALUE_VEC3F; p += 6;
    } else if (strncmp(p, "vec3d_", 6) == 0) {
        vtype = TVDB_VALUE_VEC3D; p += 6;
    } else if (strncmp(p, "vec3i_", 6) == 0) {
        vtype = TVDB_VALUE_VEC3I; p += 6;
    } else if (strncmp(p, "ptidx32_", 8) == 0) {
        /* OpenVDB PointIndexGrid: voxel values are prefix offsets into
           per-leaf point index arrays. */
        vtype = TVDB_VALUE_INT32; p += 8;
    } else if (strncmp(p, "ptdataidx32_", 12) == 0) {
        /* OpenVDB PointDataGrid: per-voxel storage is uint32 index
           offsets into per-leaf attribute arrays. tinyvdb only uses this
           to parse the topology; the attribute payload is skipped at the
           grid level (see tvdb__read_grid). */
        vtype = TVDB_VALUE_INT32; p += 12;
    } else {
        tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                        "Unsupported value type in grid type string");
        return TVDB_ERROR_UNSUPPORTED_GRID_TYPE;
    }

    /* Root node (level 0) */
    layout->levels[0].node_type  = TVDB_NODE_ROOT;
    layout->levels[0].value_type = vtype;
    layout->levels[0].log2dim    = 0;
    layout->num_levels = 1;

    /* Parse dimensions (e.g. "5_4_3") */
    while (*p && layout->num_levels < TVDB_MAX_TREE_DEPTH) {
        int dim = 0;
        while (*p >= '0' && *p <= '9') {
            /* Saturate instead of overflowing; anything > 10 is rejected. */
            if (dim <= 10) dim = dim * 10 + (*p - '0');
            p++;
        }
        if (*p == '_') p++;

        /* Reject log2dim >= 10 to prevent undefined behavior in
           1 << (3 * log2dim). Standard OpenVDB uses 3, 4, 5. */
        if (dim < 0 || dim > 10) {
            tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                            "log2dim out of range (max 10)");
            return TVDB_ERROR_UNSUPPORTED_GRID_TYPE;
        }

        int level = layout->num_levels;
        layout->levels[level].value_type = vtype;
        layout->levels[level].log2dim    = (int32_t)dim;

        /* Last dimension is leaf, others are internal */
        if (*p == '\0') {
            layout->levels[level].node_type = TVDB_NODE_LEAF;
        } else {
            layout->levels[level].node_type = TVDB_NODE_INTERNAL;
        }
        layout->num_levels++;
    }

    if (layout->num_levels < 3) {
        tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_GRID_TYPE,
                        "Grid type has too few levels");
        return TVDB_ERROR_UNSUPPORTED_GRID_TYPE;
    }

    return TVDB_OK;
}

/* ========================================================================== */
/*  Transform reading                                                         */
/* ========================================================================== */

static tvdb_status_t tvdb__read_transform(tvdb__sr_t *sr,
                                          tvdb_transform_t *xform,
                                          tvdb_allocator_t *alloc,
                                          tvdb_error_t *err) {
    memset(xform, 0, sizeof(*xform));
    /* Identity matrix */
    for (int i = 0; i < 4; i++) xform->matrix[i][i] = 1.0;

    char *type = NULL;
    tvdb_status_t tst = tvdb__sr_read_cstring(sr, alloc, &type);
    if (tst != TVDB_OK) {
        tvdb__set_error(err, tst, tst == TVDB_ERROR_OUT_OF_MEMORY ? "OOM" :
                        "Failed to read transform type");
        return tst;
    }
    int ok = 1;

    double dummy[3]; /* for fields we read but don't store */

    if (strcmp(type, "UniformScaleMap") == 0 ||
        strcmp(type, "ScaleMap") == 0) {
        xform->type = (strcmp(type, "UniformScaleMap") == 0)
                        ? TVDB_TRANSFORM_UNIFORM_SCALE
                        : TVDB_TRANSFORM_SCALE;
        /* ScaleMap::read: scale, voxelSize, scaleInv, invScaleSqr,
           invTwiceScale */
        ok = ok && tvdb__sr_read_vec3d(sr, xform->scale_values);
        ok = ok && tvdb__sr_read_vec3d(sr, xform->voxel_size);
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* scaleValuesInverse */
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* invScaleSqr */
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* invTwiceScale */
    } else if (strcmp(type, "UniformScaleTranslateMap") == 0 ||
               strcmp(type, "ScaleTranslateMap") == 0) {
        xform->type = (strcmp(type, "UniformScaleTranslateMap") == 0)
                        ? TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE
                        : TVDB_TRANSFORM_SCALE_TRANSLATE;
        /* ScaleTranslateMap::read: translation, scale, voxelSize, scaleInv,
           invScaleSqr, invTwiceScale */
        ok = ok && tvdb__sr_read_vec3d(sr, xform->translation);
        ok = ok && tvdb__sr_read_vec3d(sr, xform->scale_values);
        ok = ok && tvdb__sr_read_vec3d(sr, xform->voxel_size);
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* scaleValuesInverse */
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* invScaleSqr */
        ok = ok && tvdb__sr_read_vec3d(sr, dummy); /* invTwiceScale */
    } else if (strcmp(type, "TranslationMap") == 0) {
        xform->type = TVDB_TRANSFORM_TRANSLATION;
        ok = ok && tvdb__sr_read_vec3d(sr, xform->translation);
        xform->scale_values[0] = xform->scale_values[1] =
            xform->scale_values[2] = 1.0;
        xform->voxel_size[0] = xform->voxel_size[1] =
            xform->voxel_size[2] = 1.0;
    } else if (strcmp(type, "AffineMap") == 0) {
        xform->type = TVDB_TRANSFORM_AFFINE;
        /* AffineMap::read: 4x4 matrix (16 doubles, row-major) */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                ok = ok && tvdb__sr_read_f64(sr, &xform->matrix[r][c]);
        /* Extract scale and translation from matrix */
        xform->translation[0] = xform->matrix[0][3];
        xform->translation[1] = xform->matrix[1][3];
        xform->translation[2] = xform->matrix[2][3];
    } else {
        /* Unknown transform: try to skip by reading an AffineMap-sized block */
        xform->type = TVDB_TRANSFORM_UNKNOWN;
        tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_TRANSFORM, type);
        tvdb__free(alloc, type, strlen(type) + 1);
        return TVDB_ERROR_UNSUPPORTED_TRANSFORM;
    }

    tvdb__free(alloc, type, strlen(type) + 1);
    if (!ok) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Truncated transform");
        return TVDB_ERROR_INVALID_DATA;
    }
    return TVDB_OK;
}

/* ========================================================================== */
/*  Decompression                                                             */
/* ========================================================================== */

static int tvdb__decompress_zip(uint8_t *dst, size_t *uncompressed_size,
                                const uint8_t *src, size_t src_size) {
    if (*uncompressed_size == src_size) {
        memcpy(dst, src, src_size);
        return 1;
    }
#if defined(TVDB_USE_SYSTEM_ZLIB)
    uLongf sz = (uLongf)*uncompressed_size;
    if (uncompress(dst, &sz, src, (uLong)src_size) != Z_OK) return 0;
    *uncompressed_size = (size_t)sz;
#else
    mz_ulong sz = (mz_ulong)*uncompressed_size;
    if (mz_uncompress(dst, &sz, src, (mz_ulong)src_size) != MZ_OK) return 0;
    *uncompressed_size = (size_t)sz;
#endif
    return 1;
}

/* ========================================================================== */
/*  Inline BLOSC frame decode/encode (no external blosc dependency)           */
/* ========================================================================== */

static int32_t tvdb__blosc_le32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void tvdb__blosc_put_le32(uint8_t *p, int32_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u);        p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);  p[3] = (uint8_t)(u >> 24);
}

static void tvdb__blosc_unshuffle(size_t type_size, size_t block_size,
                                  const uint8_t *src, uint8_t *dst) {
    size_t neblock = block_size / type_size;
    size_t leftover = block_size % type_size;
    for (size_t i = 0; i < neblock; i++)
        for (size_t j = 0; j < type_size; j++)
            dst[i * type_size + j] = src[j * neblock + i];
    if (leftover)
        memcpy(dst + neblock * type_size,
               src + neblock * type_size, leftover);
}

static void tvdb__blosc_shuffle(size_t type_size, size_t block_size,
                                const uint8_t *src, uint8_t *dst) {
    size_t neblock = block_size / type_size;
    size_t leftover = block_size % type_size;
    for (size_t j = 0; j < type_size; j++)
        for (size_t i = 0; i < neblock; i++)
            dst[j * neblock + i] = src[i * type_size + j];
    if (leftover)
        memcpy(dst + neblock * type_size,
               src + neblock * type_size, leftover);
}

/* Decompress a complete BLOSC frame. Supports blosc1 (v2) and blosc2 (v5+)
   headers, LZ4 and ZLIB compressors, byte-shuffle, split and non-split modes.
   Returns 1 on success, 0 on failure. */
static int tvdb__decompress_blosc(uint8_t *dst, size_t dst_cap,
                                  const uint8_t *src, size_t src_size,
                                  tvdb_allocator_t *alloc) {
    if (src_size < 16) return 0;

    /* Parse header */
    uint8_t version   = src[0];
    uint8_t flags     = src[2];
    uint8_t typesize  = src[3];
    int32_t nbytes    = tvdb__blosc_le32(src + 4);
    int32_t blocksize = tvdb__blosc_le32(src + 8);
    int32_t cbytes    = tvdb__blosc_le32(src + 12);

    if (nbytes < 0 || blocksize <= 0 || cbytes < 16) return 0;
    /* The frame must produce exactly the expected payload; a shorter frame
       would leave the tail of dst uninitialized. */
    if ((size_t)nbytes != dst_cap) return 0;
    if ((size_t)cbytes > src_size) return 0;
    if (typesize == 0) typesize = 1;

    int do_shuffle = (flags & 0x01) && (typesize > 1);
    int memcpyed   = (flags & 0x02);
    int do_bitshuffle = (flags & 0x04);
    int compformat = (flags >> 5) & 7;

    /* Determine header length */
    int header_len = (version >= 5) ? 32 : 16;
    if ((size_t)header_len > src_size) return 0;

    /* Bitshuffle not supported in minimal implementation */
    if (do_bitshuffle) return 0;

    /* Memcpy (uncompressed) frame */
    if (memcpyed) {
        if ((size_t)header_len + (size_t)nbytes > src_size) return 0;
        memcpy(dst, src + header_len, (size_t)nbytes);
        return 1;
    }

    /* Validate compressor */
    /* Validate compressor: LZ4(1), ZLIB(3), ZSTD(4) */
    if (compformat != 1 && compformat != 3
#if defined(TVDB_USE_ZSTD)
        && compformat != 4
#endif
    ) {
        return 0;
    }

    int64_t nblocks64 = ((int64_t)nbytes + blocksize - 1) / blocksize;
    if (nblocks64 < 0 || nblocks64 > INT32_MAX) return 0;
    int nblocks = (int)nblocks64;
    /* Blocks never exceed the payload; bound the scratch buffer by it. */
    if (blocksize > nbytes && nbytes > 0) blocksize = nbytes;
    const uint8_t *bstarts = src + header_len;

    /* Validate block offsets fit in src (use size_t to avoid int overflow) */
    if ((size_t)header_len + (size_t)nblocks * 4 > src_size) return 0;

    /* Temp buffer for unshuffle */
    uint8_t *tmp = NULL;
    if (do_shuffle) {
        tmp = (uint8_t *)tvdb__alloc(alloc, (size_t)blocksize);
        if (!tmp) return 0;
    }

    int32_t bytes_written = 0;
    for (int j = 0; j < nblocks; j++) {
        int32_t bsize = blocksize;
        if (j == nblocks - 1 && (nbytes % blocksize) != 0)
            bsize = nbytes % blocksize;

        int32_t block_offset = tvdb__blosc_le32(bstarts + (size_t)j * 4);
        if (block_offset < 0 || (size_t)block_offset >= src_size) goto fail;

        /* Determine splits. The header's dont-split bit is unreliable: the
           compressor's split heuristic (block-size / element-count thresholds)
           varies across c-blosc versions, so derive the choice from the data
           instead. A block is stored as EITHER one stream covering the whole
           block, OR `typesize` independently-compressed shuffle-partition
           streams. If the first stream's framed length (4 + csize0) fills the
           block's whole compressed extent it is unsplit, otherwise it is split
           typesize-ways. */
        int nsplits = 1;
        if (do_shuffle && typesize > 1 && typesize <= 16 &&
            (bsize / typesize) >= 1) {
            /* Compressed extent of this block: [block_offset, next_offset). */
            size_t block_end = src_size;
            if (j + 1 < nblocks) {
                int32_t bnext = tvdb__blosc_le32(bstarts + (size_t)(j + 1) * 4);
                if (bnext >= 0 && (size_t)bnext <= src_size &&
                    (size_t)bnext >= (size_t)block_offset)
                    block_end = (size_t)bnext;
            }
            size_t block_compr = block_end - (size_t)block_offset;
            if (block_compr >= 4) {
                int32_t csize0 = tvdb__blosc_le32(src + block_offset);
                if (csize0 >= 0 &&
                    ((size_t)4 + (size_t)csize0) != block_compr)
                    nsplits = typesize;
            }
        }
        int neblock = bsize / nsplits;

        uint8_t *block_dst = do_shuffle ? tmp : (dst + (size_t)j * blocksize);

        for (int k = 0; k < nsplits; k++) {
            if ((size_t)block_offset + 4 > src_size) goto fail;
            int32_t split_cbytes = tvdb__blosc_le32(src + block_offset);
            block_offset += 4;

            if (split_cbytes < 0 ||
                (size_t)block_offset + (size_t)split_cbytes > src_size)
                goto fail;

            uint8_t *split_dst = block_dst + (size_t)k * (size_t)neblock;

            if (split_cbytes == neblock) {
                /* Stored uncompressed */
                memcpy(split_dst, src + block_offset, (size_t)neblock);
            } else if (compformat == 1) {
                /* LZ4 */
                int dec = LZ4_decompress_safe(
                    (const char *)(src + block_offset),
                    (char *)split_dst,
                    split_cbytes, neblock);
                if (dec < 0 || dec != neblock) goto fail;
            } else if (compformat == 3) {
                /* ZLIB via miniz */
                size_t usz = (size_t)neblock;
                if (!tvdb__decompress_zip(split_dst, &usz,
                                          src + block_offset,
                                          (size_t)split_cbytes))
                    goto fail;
            }
#if defined(TVDB_USE_ZSTD)
            else if (compformat == 4) {
                /* ZSTD */
                size_t dec = ZSTD_decompress(
                    split_dst, (size_t)neblock,
                    src + block_offset, (size_t)split_cbytes);
                if (ZSTD_isError(dec) || dec != (size_t)neblock)
                    goto fail;
            }
#endif
            block_offset += split_cbytes;
        }

        if (do_shuffle) {
            tvdb__blosc_unshuffle((size_t)typesize, (size_t)bsize,
                                 tmp, dst + (size_t)j * blocksize);
        }
        bytes_written += bsize;
    }

    if (tmp) tvdb__free(alloc, tmp, (size_t)blocksize);
    return bytes_written == nbytes ? 1 : 0;

fail:
    if (tmp) tvdb__free(alloc, tmp, (size_t)blocksize);
    return 0;
}

/* Compress data into a BLOSC1 frame using LZ4 + byte-shuffle.
   Returns total compressed size (including header), 0 if compression
   didn't help (caller should store uncompressed), or -1 on error. */
static int tvdb__compress_blosc(const uint8_t *src, size_t src_size,
                                uint8_t *dst, size_t dst_cap,
                                size_t typesize, int clevel,
                                tvdb_allocator_t *alloc) {
    if (src_size == 0) return 0;
    if (typesize == 0) typesize = 1;
    if (clevel < 1) clevel = 1;
    if (clevel > 9) clevel = 9;

    int32_t blocksize = (int32_t)src_size; /* single block for simplicity */
    int nblocks = 1;
    int do_shuffle = (typesize > 1) ? 1 : 0;

    /* Header(16) + block_offsets(nblocks*4) + compressed data */
    size_t overhead = 16 + (size_t)nblocks * 4;
    if (dst_cap < overhead + 4) return -1;

    /* Write header */
    dst[0] = 2;  /* blosc1 format version */
    dst[1] = 1;  /* LZ4 format version */
    /* flags: shuffle(bit0) | dont_split(bit4) | LZ4(bits5-7=1) */
    dst[2] = (uint8_t)((do_shuffle ? 0x01 : 0x00) | 0x10 | (1 << 5));
    dst[3] = (uint8_t)(typesize > 255 ? 255 : typesize);
    tvdb__blosc_put_le32(dst + 4,  (int32_t)src_size);
    tvdb__blosc_put_le32(dst + 8,  blocksize);
    /* cbytes placeholder at dst+12, filled at end */

    size_t out_pos = overhead;

    /* Temp buffer for shuffle */
    uint8_t *shuffled = NULL;
    if (do_shuffle) {
        shuffled = (uint8_t *)tvdb__alloc(alloc, src_size);
        if (!shuffled) return -1;
    }

    for (int j = 0; j < nblocks; j++) {
        size_t bsize = (j == nblocks - 1 && (src_size % (size_t)blocksize) != 0)
                           ? (src_size % (size_t)blocksize)
                           : (size_t)blocksize;

        /* Record block offset */
        tvdb__blosc_put_le32(dst + 16 + j * 4, (int32_t)out_pos);

        const uint8_t *block_src = src + (size_t)j * blocksize;
        if (do_shuffle) {
            tvdb__blosc_shuffle(typesize, bsize, block_src, shuffled);
            block_src = shuffled;
        }

        /* Compress with LZ4 (dont_split=1: single stream per block) */
        int max_out = (int)(dst_cap - out_pos - 4);
        if (max_out < 1) { if (shuffled) tvdb__free(alloc, shuffled, src_size); return -1; }

        int acceleration = 10 - clevel; /* higher clevel = lower accel = better ratio */
        if (acceleration < 1) acceleration = 1;

        int csize = LZ4_compress_fast(
            (const char *)block_src, (char *)(dst + out_pos + 4),
            (int)bsize, max_out, acceleration);

        if (csize <= 0 || (size_t)csize >= bsize) {
            /* Compression didn't help — store uncompressed */
            if (out_pos + 4 + bsize > dst_cap) {
                if (shuffled) tvdb__free(alloc, shuffled, src_size);
                return -1;
            }
            tvdb__blosc_put_le32(dst + out_pos, (int32_t)bsize);
            memcpy(dst + out_pos + 4, block_src, bsize);
            out_pos += 4 + bsize;
        } else {
            tvdb__blosc_put_le32(dst + out_pos, (int32_t)csize);
            out_pos += 4 + (size_t)csize;
        }
    }

    if (shuffled) tvdb__free(alloc, shuffled, src_size);

    /* Backpatch cbytes */
    tvdb__blosc_put_le32(dst + 12, (int32_t)out_pos);
    return (int)out_pos;
}

/* Read compressed data from stream, decompress into dst_data.
   dst_data must be pre-allocated to element_size * count bytes.
   If dst_data is NULL, just seek over the data. */
static tvdb_status_t tvdb__read_and_decompress(
    tvdb__sr_t *sr, uint8_t *dst_data, size_t element_size, size_t count,
    uint32_t compression_mask, tvdb_allocator_t *alloc, tvdb_error_t *err) {

    size_t total_size = tvdb__safe_mul(element_size, count);
    if (element_size > 0 && count > 0 && total_size == 0) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Buffer size overflow");
        return TVDB_ERROR_INVALID_DATA;
    }

    if (compression_mask & (TVDB_COMPRESS_BLOSC | TVDB_COMPRESS_ZIP)) {
        int is_blosc = (compression_mask & TVDB_COMPRESS_BLOSC) != 0;
        int64_t num_compressed;
        if (!tvdb__sr_read_i64(sr, &num_compressed)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Failed to read compressed chunk size");
            return TVDB_ERROR_INVALID_DATA;
        }
        if (num_compressed <= 0) {
            /* OpenVDB: -N means N uncompressed bytes follow, and N must be
               exactly the expected payload size. */
            if (num_compressed == INT64_MIN ||
                (uint64_t)(-num_compressed) != (uint64_t)total_size) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Uncompressed chunk size mismatch");
                return TVDB_ERROR_INVALID_DATA;
            }
            int ok = dst_data ? tvdb__sr_read(sr, total_size, dst_data)
                              : tvdb__sr_seek_cur(sr, (int64_t)total_size);
            if (!ok) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Truncated uncompressed chunk");
                return TVDB_ERROR_INVALID_DATA;
            }
        } else {
            /* Guard a corrupt/oversized compressed length against the bytes
               actually remaining in the stream, before allocating. */
            if ((uint64_t)num_compressed > sr->length - sr->pos) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Compressed size exceeds remaining data");
                return TVDB_ERROR_INVALID_DATA;
            }
            /* The stream is fully in memory; decompress from it directly. */
            const uint8_t *src = sr->data + sr->pos;
            sr->pos += (uint64_t)num_compressed;
            if (dst_data) {
                int ok;
                if (is_blosc) {
                    ok = tvdb__decompress_blosc(dst_data, total_size, src,
                                                (size_t)num_compressed, alloc);
                } else {
                    size_t usz = total_size;
                    ok = tvdb__decompress_zip(dst_data, &usz, src,
                                              (size_t)num_compressed) &&
                         usz == total_size;
                }
                if (!ok) {
                    tvdb__set_error(err, TVDB_ERROR_DECOMPRESSION_FAILED,
                                    is_blosc ? "BLOSC decompression failed"
                                             : "ZIP decompression failed");
                    return TVDB_ERROR_DECOMPRESSION_FAILED;
                }
            }
        }
    } else {
        /* No compression */
        int ok = dst_data ? tvdb__sr_read(sr, total_size, dst_data)
                          : tvdb__sr_seek_cur(sr, (int64_t)total_size);
        if (!ok) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated value buffer");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    /* Endian swap */
    if (dst_data && sr->swap_endian) {
        if (element_size == 2) {
            for (size_t i = 0; i < count; i++)
                tvdb__swap2(dst_data + i * 2);
        } else if (element_size == 4) {
            for (size_t i = 0; i < count; i++)
                tvdb__swap4(dst_data + i * 4);
        } else if (element_size == 8) {
            for (size_t i = 0; i < count; i++)
                tvdb__swap8(dst_data + i * 8);
        }
    }

    return TVDB_OK;
}

/* ========================================================================== */
/*  Mask-compressed value reading                                             */
/* ========================================================================== */

static tvdb_value_t tvdb__negate_value(tvdb_value_t v) {
    tvdb_value_t r = v;
    switch (v.type) {
        case TVDB_VALUE_FLOAT:  r.u.f = -v.u.f; break;
        case TVDB_VALUE_DOUBLE: r.u.d = -v.u.d; break;
        case TVDB_VALUE_INT32:  r.u.i32 = -v.u.i32; break;
        case TVDB_VALUE_INT64:  r.u.i64 = -v.u.i64; break;
        case TVDB_VALUE_VEC3I:
            r.u.vec3i[0] = -v.u.vec3i[0];
            r.u.vec3i[1] = -v.u.vec3i[1];
            r.u.vec3i[2] = -v.u.vec3i[2];
            break;
        case TVDB_VALUE_VEC3F:
            r.u.vec3f[0] = -v.u.vec3f[0];
            r.u.vec3f[1] = -v.u.vec3f[1];
            r.u.vec3f[2] = -v.u.vec3f[2];
            break;
        case TVDB_VALUE_VEC3D:
            r.u.vec3d[0] = -v.u.vec3d[0];
            r.u.vec3d[1] = -v.u.vec3d[1];
            r.u.vec3d[2] = -v.u.vec3d[2];
            break;
        default: break;
    }
    return r;
}

/* Defined below, but referenced by the staging-buffer parameter of
 * tvdb__read_mask_values. */
typedef struct tvdb__deser_params tvdb__deser_params_t;

/* Scratch belongs to one grid load and uses that load's allocator. */
typedef struct tvdb__scratch {
    uint8_t *data;
    size_t size;
} tvdb__scratch_t;

/* Number of scalar components stored as half when a grid is saved with
   _HalfFloat. OpenVDB only converts real-valued types (RealToHalf<T>::isReal):
   float/double map to half, Vec3s/Vec3d to Vec3H. 0 = not converted. */
static size_t tvdb__half_components(tvdb_value_type_t t) {
    switch (t) {
        case TVDB_VALUE_FLOAT: case TVDB_VALUE_DOUBLE: return 1;
        case TVDB_VALUE_VEC3F: case TVDB_VALUE_VEC3D:  return 3;
        default: return 0;
    }
}

/* Expand one stored half value (ncomp halves) to the in-memory value type. */
static void tvdb__half_to_value(const uint8_t *src, uint8_t *dst,
                                tvdb_value_type_t t) {
    size_t n = tvdb__half_components(t);
    for (size_t c = 0; c < n; ++c) {
        uint16_t h;
        memcpy(&h, src + c * 2, 2);
        float f = tvdb__half_to_float(h);
        if (t == TVDB_VALUE_DOUBLE || t == TVDB_VALUE_VEC3D) {
            double d = (double)f;
            memcpy(dst + c * 8, &d, 8);
        } else {
            memcpy(dst + c * 4, &f, 4);
        }
    }
}

static tvdb_status_t tvdb__read_mask_values(
    tvdb__sr_t *sr, uint32_t compression_flags, uint32_t file_version,
    tvdb_value_t background, size_t num_values, tvdb_value_type_t value_type,
    const tvdb_nodemask_t *value_mask, uint8_t *values,
    int half_precision, tvdb__scratch_t *scratch,
    tvdb_allocator_t *alloc, tvdb_error_t *err) {

    int mask_compressed = (compression_flags & TVDB_COMPRESS_ACTIVE_MASK) != 0;
    size_t half_comps = half_precision ? tvdb__half_components(value_type) : 0;
    int is_half = half_comps != 0;
    int8_t per_node_flag = TVDB_NO_MASK_AND_ALL_VALS;

    if (file_version >= TVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        if (!tvdb__sr_read_i8(sr, &per_node_flag) ||
            per_node_flag < TVDB_NO_MASK_OR_INACTIVE_VALS ||
            per_node_flag > TVDB_NO_MASK_AND_ALL_VALS) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Invalid per-node compression flag");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    tvdb_value_t inactive_val1;
    tvdb_value_t inactive_val0;
    memset(&inactive_val1, 0, sizeof(inactive_val1));
    memset(&inactive_val0, 0, sizeof(inactive_val0));
    inactive_val1 = background;
    inactive_val0 = (per_node_flag == TVDB_NO_MASK_OR_INACTIVE_VALS)
                        ? background : tvdb__negate_value(background);

    /* Inactive values are stored at full ValueT width even for half grids
       (OpenVDB readCompressedValues reads sizeof(ValueT)). */
    if (per_node_flag == TVDB_NO_MASK_AND_ONE_INACTIVE_VAL ||
        per_node_flag == TVDB_MASK_AND_ONE_INACTIVE_VAL ||
        per_node_flag == TVDB_MASK_AND_TWO_INACTIVE_VALS) {
        int ok = tvdb__sr_read_value(sr, background.type, &inactive_val0);
        if (ok && per_node_flag == TVDB_MASK_AND_TWO_INACTIVE_VALS)
            ok = tvdb__sr_read_value(sr, background.type, &inactive_val1);
        if (!ok) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated inactive values");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    tvdb_nodemask_t selection_mask;
    tvdb__nodemask_init(&selection_mask);
    if (per_node_flag == TVDB_MASK_AND_NO_INACTIVE_VALS ||
        per_node_flag == TVDB_MASK_AND_ONE_INACTIVE_VAL ||
        per_node_flag == TVDB_MASK_AND_TWO_INACTIVE_VALS) {
        if (!tvdb__nodemask_alloc(&selection_mask, value_mask->log2dim,
                                  alloc)) {
            tvdb__nodemask_destroy(&selection_mask);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                            "OOM in read_mask_values");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        if (!tvdb__sr_read(sr, tvdb__nodemask_mem_usage(&selection_mask),
                           selection_mask.bits.data)) {
            tvdb__nodemask_destroy(&selection_mask);
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated selection mask");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    size_t read_count = num_values;
    if (mask_compressed && per_node_flag != TVDB_NO_MASK_AND_ALL_VALS &&
        file_version >= TVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        read_count = tvdb__nodemask_count_on(value_mask);
        if (read_count > num_values) read_count = num_values;
    }

    size_t vsize = tvdb_value_type_size(value_type);
    size_t file_elem_size = is_half ? 2 * half_comps : vsize;

    size_t tmp_size = read_count * file_elem_size;
    /* Reuse the shared staging buffer instead of allocating and zeroing one
     * per node. It only ever grows, and its contents are fully consumed below
     * before the caller recurses, so sharing across nodes is safe. */
    uint8_t *tmp_buf = NULL;
    if (tmp_size > 0) {
        if (scratch->size < tmp_size) {
            uint8_t *grown = (uint8_t *)tvdb__alloc(alloc, tmp_size);
            if (!grown) {
                tvdb__nodemask_destroy(&selection_mask);
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                                "OOM in read_mask_values");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            if (scratch->data)
                tvdb__free(alloc, scratch->data, scratch->size);
            scratch->data = grown;
            scratch->size = tmp_size;
        }
        tmp_buf = scratch->data;
    }

    /* OpenVDB's HalfReader::read has an `if (count < 1) return;` guard at the
       top, so half-precision writes/reads emit NOTHING at all when the active
       count is zero (e.g. an internal node whose tiles are all inactive in a
       signed-flood-filled level set). The non-half path still runs through
       readData → bloscFromStream, which does read/write the 8-byte blosc size
       header even for count=0. Match that asymmetry here, or we drift 8 bytes
       on every all-inactive half-precision node. */
    tvdb_status_t st = TVDB_OK;
    if (!(is_half && read_count == 0)) {
        st = tvdb__read_and_decompress(
            sr, tmp_buf, is_half ? 2 : file_elem_size,
            is_half ? read_count * half_comps : read_count,
            compression_flags, alloc, err);
    }
    if (st != TVDB_OK) {
        tvdb__nodemask_destroy(&selection_mask);
        return st;
    }

    /* Reconstruct full value buffer if mask compressed */
    if (values && read_count != num_values) {
        size_t temp_idx = 0;
        /* Fast path: float values at full width, i.e. the overwhelmingly
         * common case. It lets the compiler keep the mask test in a register
         * and turn the per-element memcpy into a 4-byte store, instead of a
         * variable-size copy with a bounds check per element. The generic
         * path below still handles every other value type. */
        if (!is_half && vsize == 4 && file_elem_size == 4) {
            const uint8_t *src = tmp_buf;
            uint8_t *dst = values;
            const tvdb_bitset_t *vm = &value_mask->bits;
            /* selection_mask is only allocated for the MASK_AND_* node flags.
             * When it is absent there are no inactive slots to distinguish, so
             * every inactive entry takes inactive_val0. */
            const tvdb_bitset_t *sm = selection_mask.bits.data
                                          ? &selection_mask.bits : NULL;
            float v0, v1;
            memcpy(&v0, &inactive_val0.u, 4);
            memcpy(&v1, &inactive_val1.u, 4);
            for (size_t dest_idx = 0; dest_idx < num_values; dest_idx++) {
                float out;
                if (((vm->data[dest_idx >> 3] >> (dest_idx & 7)) & 1u) &&
                    temp_idx < read_count) {
                    memcpy(&out, src + temp_idx * 4, 4);
                    temp_idx++;
                } else {
                    out = (sm && ((sm->data[dest_idx >> 3] >> (dest_idx & 7)) & 1u))
                              ? v1 : v0;
                }
                memcpy(dst + dest_idx * 4, &out, 4);
            }
        } else {
        for (size_t dest_idx = 0; dest_idx < num_values; dest_idx++) {
            if (tvdb__nodemask_is_on(value_mask, (int32_t)dest_idx) &&
                temp_idx < read_count) {
                if (is_half) {
                    tvdb__half_to_value(tmp_buf + temp_idx * file_elem_size,
                                        values + dest_idx * vsize, value_type);
                } else {
                    memcpy(values + dest_idx * vsize,
                           tmp_buf + temp_idx * file_elem_size, vsize);
                }
                temp_idx++;
            } else {
                if (tvdb__nodemask_is_on(&selection_mask, (int32_t)dest_idx))
                    memcpy(values + dest_idx * vsize, &inactive_val1.u, vsize);
                else
                    memcpy(values + dest_idx * vsize, &inactive_val0.u, vsize);
            }
        }
        }
    } else if (values) {
        if (is_half && value_type == TVDB_VALUE_FLOAT) {
            tvdb__promote_half_to_float(tmp_buf, values, num_values);
        } else if (is_half) {
            for (size_t i = 0; i < num_values; i++)
                tvdb__half_to_value(tmp_buf + i * file_elem_size,
                                    values + i * vsize, value_type);
        } else if (num_values) {
            memcpy(values, tmp_buf, num_values * vsize);
        }
    }

    /* tmp_buf is the shared staging buffer and outlives this call; only the
     * local selection mask needs releasing here. */
    tvdb__nodemask_destroy(&selection_mask);
    return TVDB_OK;
}

/* ========================================================================== */
/*  Tree node management                                                      */
/* ========================================================================== */

static size_t tvdb__tree_alloc_node(tvdb_tree_t *tree) {
    if (tree->num_nodes >= tree->nodes_capacity) {
        size_t new_cap = tree->nodes_capacity ? tree->nodes_capacity * 2 : 64;
        tvdb_tree_node_t *new_nodes = (tvdb_tree_node_t *)tvdb__realloc(
            tree->alloc, tree->nodes,
            tree->nodes_capacity * sizeof(tvdb_tree_node_t),
            new_cap * sizeof(tvdb_tree_node_t));
        if (!new_nodes) return (size_t)-1;
        /* Zero new entries */
        memset(new_nodes + tree->nodes_capacity, 0,
               (new_cap - tree->nodes_capacity) * sizeof(tvdb_tree_node_t));
        tree->nodes          = new_nodes;
        tree->nodes_capacity = new_cap;
    }
    size_t idx = tree->num_nodes++;
    memset(&tree->nodes[idx], 0, sizeof(tvdb_tree_node_t));
    return idx;
}

/* ========================================================================== */
/*  Tree topology reading                                                     */
/* ========================================================================== */

typedef struct tvdb__deser_params {
    uint32_t file_version;
    uint32_t compression_flags;
    int      half_precision;
    tvdb_value_t background;
    tvdb__scratch_t *scratch;
} tvdb__deser_params_t;
/* Forward declaration for recursive calls */
static tvdb_status_t tvdb__read_node_topology(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err);

static tvdb_status_t tvdb__read_root_topology(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_tree_node_t *node = &tree->nodes[node_idx];
    node->type  = TVDB_NODE_ROOT;
    node->level = level;
    tvdb_root_node_t *root = &node->u.root;
    tvdb_allocator_t *a = tree->alloc;
    tvdb_value_type_t vt = tree->layout.levels[level].value_type;

    /* Read background value */
    if (!tvdb__sr_read_value(sr, vt, &root->background)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read root background value");
        return TVDB_ERROR_INVALID_DATA;
    }

    if (!tvdb__sr_read_u32(sr, &root->num_tiles) ||
        !tvdb__sr_read_u32(sr, &root->num_children)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read root tile/child counts");
        return TVDB_ERROR_INVALID_DATA;
    }

    if (root->num_tiles == 0 && root->num_children == 0) {
        /* Empty root - this is valid for some grids */
        return TVDB_OK;
    }

    /* Bound header counts by the bytes that remain before allocating or
       looping: each tile needs an origin, a value and an active byte, each
       child at least its origin. */
    {
        uint64_t remaining = sr->length - sr->pos;
        uint64_t tile_bytes = 12u + (uint64_t)tvdb_value_type_size(vt) + 1u;
        uint64_t tiles_total = (uint64_t)root->num_tiles * tile_bytes;
        if (tiles_total > remaining ||
            (uint64_t)root->num_children * 12u > remaining - tiles_total) {
            root->num_tiles = 0;
            root->num_children = 0;
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Root tile/child counts exceed remaining data");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    /* Read tiles */
    if (root->num_tiles > 0) {
        size_t tile_origins_sz = tvdb__safe_mul(
            (size_t)root->num_tiles * 3, sizeof(int32_t));
        size_t tile_values_sz = tvdb__safe_mul(
            (size_t)root->num_tiles, sizeof(tvdb_value_t));
        size_t tile_active_sz = tvdb__safe_mul(
            (size_t)root->num_tiles, sizeof(int));
        if (!tile_origins_sz || !tile_values_sz || !tile_active_sz) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Tile count overflow");
            return TVDB_ERROR_INVALID_DATA;
        }
        root->tile_origins = (int32_t *)tvdb__alloc(a, tile_origins_sz);
        root->tile_values = (tvdb_value_t *)tvdb__alloc(a, tile_values_sz);
        root->tile_active = (int *)tvdb__alloc(a, tile_active_sz);
        if (!root->tile_origins || !root->tile_values || !root->tile_active) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }

        memset(root->tile_values, 0, tile_values_sz);
        for (uint32_t i = 0; i < root->num_tiles; i++) {
            uint8_t active = 0;
            if (!tvdb__sr_read_i32(sr, &root->tile_origins[(size_t)i * 3 + 0]) ||
                !tvdb__sr_read_i32(sr, &root->tile_origins[(size_t)i * 3 + 1]) ||
                !tvdb__sr_read_i32(sr, &root->tile_origins[(size_t)i * 3 + 2]) ||
                !tvdb__sr_read_value(sr, vt, &root->tile_values[i]) ||
                !tvdb__sr_read_u8(sr, &active)) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Truncated root tile");
                return TVDB_ERROR_INVALID_DATA;
            }
            root->tile_active[i] = active ? 1 : 0;
        }
    }

    /* Read child nodes */
    if (root->num_children > 0) {
        size_t child_orig_sz = tvdb__safe_mul(
            (size_t)root->num_children * 3, sizeof(int32_t));
        size_t child_idx_sz = tvdb__safe_mul(
            (size_t)root->num_children, sizeof(size_t));
        if (!child_orig_sz || !child_idx_sz) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Child count overflow");
            return TVDB_ERROR_INVALID_DATA;
        }
        root->child_origins = (int32_t *)tvdb__alloc(a, child_orig_sz);
        root->child_indices = (size_t *)tvdb__alloc(a, child_idx_sz);
        if (!root->child_origins || !root->child_indices) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }

        /* Propagate the just-read root background to descendant topology
           reads. tvdb__read_mask_values uses params->background to derive
           inactive-tile fill values (±background for signed-flood-filled
           level sets); without this, every internal tile reads back as 0. */
        tvdb__deser_params_t child_params = *params;
        child_params.background = root->background;

        memset(root->child_indices, 0, child_idx_sz);
        for (uint32_t i = 0; i < root->num_children; i++) {
            if (!tvdb__sr_read_i32(sr, &root->child_origins[(size_t)i * 3 + 0]) ||
                !tvdb__sr_read_i32(sr, &root->child_origins[(size_t)i * 3 + 1]) ||
                !tvdb__sr_read_i32(sr, &root->child_origins[(size_t)i * 3 + 2])) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Truncated root child origin");
                return TVDB_ERROR_INVALID_DATA;
            }

            size_t child_idx = tvdb__tree_alloc_node(tree);
            if (child_idx == (size_t)-1) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            /* NOTE: tree->nodes may have been reallocated, re-read root */
            root = &tree->nodes[node_idx].u.root;
            root->child_indices[i] = child_idx;

            tvdb_status_t st = tvdb__read_node_topology(
                tree, sr, child_idx, level + 1, &child_params, err);
            if (st != TVDB_OK) return st;
            /* Re-read root again after potential realloc */
            root = &tree->nodes[node_idx].u.root;
        }
    }

    return TVDB_OK;
}

static tvdb_status_t tvdb__read_internal_topology(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_tree_node_t *node = &tree->nodes[node_idx];
    node->type  = TVDB_NODE_INTERNAL;
    node->level = level;
    tvdb_internal_node_t *inode = &node->u.internal;
    tvdb_allocator_t *a = tree->alloc;
    int32_t log2dim = tree->layout.levels[level].log2dim;
    tvdb_value_type_t vt = tree->layout.levels[level].value_type;

    /* Read child mask and value mask */
    if (!tvdb__nodemask_alloc(&inode->child_mask, log2dim, a) ||
        !tvdb__nodemask_alloc(&inode->value_mask, log2dim, a)) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

    if (!tvdb__sr_read(sr, tvdb__nodemask_mem_usage(&inode->child_mask),
                       inode->child_mask.bits.data) ||
        !tvdb__sr_read(sr, tvdb__nodemask_mem_usage(&inode->value_mask),
                       inode->value_mask.bits.data)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read internal node masks");
        return TVDB_ERROR_INVALID_DATA;
    }

    int32_t bitsize = inode->child_mask.bitsize;
    int old_version = (params->file_version <
                       TVDB_FILE_VERSION_NODE_MASK_COMPRESSION);

    int32_t num_values;
    if (old_version) {
        int32_t child_count = (int32_t)tvdb__nodemask_count_on(&inode->child_mask);
        if (child_count > bitsize) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Child mask count exceeds bitsize");
            return TVDB_ERROR_INVALID_DATA;
        }
        num_values = bitsize - child_count;
    } else {
        num_values = bitsize;
    }

    /* Read tile/inactive values. Storage is always indexed by slot
       (bitsize entries); pre-222 files store only the non-child slots, which
       are expanded in place below. */
    size_t vsize = tvdb_value_type_size(vt);
    inode->values_size = (size_t)bitsize * vsize;
    inode->values = (uint8_t *)tvdb__alloc(a, inode->values_size);
    if (!inode->values) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }
    memset(inode->values, 0, inode->values_size);

    /* BOOL internal nodes use the same generic compressed-values format as
       other types: 1 flag byte + num_values * sizeof(bool) bytes (1 byte
       per value, NOT bit-packed). Only LeafBuffer<bool> is bit-packed. */
    tvdb_status_t mst = tvdb__read_mask_values(
        sr, params->compression_flags, params->file_version,
        params->background, (size_t)num_values, vt,
        &inode->value_mask, inode->values,
        params->half_precision, params->scratch, a, err);
    if (mst != TVDB_OK) return mst;

    if (old_version && num_values < bitsize) {
        /* Spread the compacted values to their slots, back to front so the
           move never overwrites unread input (slot >= compacted index).
           Child slots hold the background, as OpenVDB's do not carry a
           value. */
        int32_t n = num_values;
        for (int32_t i = bitsize - 1; i >= 0; --i) {
            uint8_t *dst = inode->values + (size_t)i * vsize;
            if (tvdb__nodemask_is_on(&inode->child_mask, i)) {
                memcpy(dst, &params->background.u, vsize);
            } else {
                --n;
                if (n != i)
                    memmove(dst, inode->values + (size_t)n * vsize, vsize);
            }
        }
    }

    /* Read child nodes */
    size_t nc = tvdb__nodemask_count_on(&inode->child_mask);
    inode->num_children = nc;
    if (nc > 0) {
        inode->child_indices = (size_t *)tvdb__alloc(a, nc * sizeof(size_t));
        if (!inode->child_indices) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
    }

    size_t child_n = 0;
    for (int32_t i = 0; i < bitsize; i++) {
        if (tvdb__nodemask_is_on(&inode->child_mask, i)) {
            size_t child_idx = tvdb__tree_alloc_node(tree);
            if (child_idx == (size_t)-1) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            /* Re-read after potential realloc */
            inode = &tree->nodes[node_idx].u.internal;
            inode->child_indices[child_n++] = child_idx;

            tvdb_status_t cst = tvdb__read_node_topology(tree, sr, child_idx,
                                                        level + 1, params, err);
            if (cst != TVDB_OK) return cst;
            inode = &tree->nodes[node_idx].u.internal;
        }
    }

    return TVDB_OK;
}

static tvdb_status_t tvdb__read_leaf_topology(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {
    (void)params;

    tvdb_tree_node_t *node = &tree->nodes[node_idx];
    node->type  = TVDB_NODE_LEAF;
    node->level = level;
    tvdb_leaf_node_t *leaf = &node->u.leaf;
    int32_t log2dim = tree->layout.levels[level].log2dim;

    if (!tvdb__nodemask_alloc(&leaf->value_mask, log2dim, tree->alloc)) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

    if (!tvdb__sr_read(sr, tvdb__nodemask_mem_usage(&leaf->value_mask),
                       leaf->value_mask.bits.data)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read leaf value mask");
        return TVDB_ERROR_INVALID_DATA;
    }

    leaf->num_voxels = (uint32_t)(1 << (3 * log2dim));
    return TVDB_OK;
}

static tvdb_status_t tvdb__read_node_topology(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    if (level >= tree->layout.num_levels) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Tree depth exceeds layout levels");
        return TVDB_ERROR_INVALID_DATA;
    }

    tvdb_node_type_t nt = tree->layout.levels[level].node_type;
    switch (nt) {
        case TVDB_NODE_ROOT:
            return tvdb__read_root_topology(tree, sr, node_idx, level,
                                            params, err);
        case TVDB_NODE_INTERNAL:
            return tvdb__read_internal_topology(tree, sr, node_idx, level,
                                                params, err);
        case TVDB_NODE_LEAF:
            return tvdb__read_leaf_topology(tree, sr, node_idx, level,
                                            params, err);
        default:
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Unknown node type");
            return TVDB_ERROR_INVALID_DATA;
    }
}

/* ========================================================================== */
/*  Tree buffer reading                                                       */
/* ========================================================================== */

static tvdb_status_t tvdb__read_node_buffer(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err);

static tvdb_status_t tvdb__read_root_buffer(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_root_node_t *root = &tree->nodes[node_idx].u.root;

    for (uint32_t i = 0; i < root->num_children; i++) {
        tvdb_status_t st = tvdb__read_node_buffer(
            tree, sr, root->child_indices[i], level + 1, params, err);
        if (st != TVDB_OK) return st;
        /* Re-read root after potential side effects */
        root = &tree->nodes[node_idx].u.root;
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__read_internal_buffer(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_internal_node_t *inode = &tree->nodes[node_idx].u.internal;

    size_t child_n = 0;
    for (int32_t i = 0; i < inode->child_mask.bitsize; i++) {
        if (tvdb__nodemask_is_on(&inode->child_mask, i)) {
            TVDB_ASSERT(child_n < inode->num_children);
            tvdb_status_t st = tvdb__read_node_buffer(
                tree, sr, inode->child_indices[child_n], level + 1,
                params, err);
            if (st != TVDB_OK) return st;
            inode = &tree->nodes[node_idx].u.internal;
            child_n++;
        }
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__read_leaf_buffer(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_leaf_node_t *leaf = &tree->nodes[node_idx].u.leaf;
    tvdb_allocator_t *a = tree->alloc;
    tvdb_value_type_t vt = tree->layout.levels[level].value_type;
    size_t vsize = tvdb_value_type_size(vt);

    /* Seek over the value mask (already loaded in topology phase) */
    if (!tvdb__sr_seek_cur(sr,
            (int64_t)tvdb__nodemask_mem_usage(&leaf->value_mask))) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Truncated leaf buffer");
        return TVDB_ERROR_INVALID_DATA;
    }

    /* Handle old version coordinate + buffer count */
    if (params->file_version < TVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        int32_t coord[3];
        int8_t num_buffers;
        if (!tvdb__sr_read_i32(sr, &coord[0]) ||
            !tvdb__sr_read_i32(sr, &coord[1]) ||
            !tvdb__sr_read_i32(sr, &coord[2]) ||
            !tvdb__sr_read_i8(sr, &num_buffers)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated leaf header");
            return TVDB_ERROR_INVALID_DATA;
        }
    }

    /* Use tvdb__read_mask_values which correctly handles per-node flags,
       inactive values, and selection masks in the compressed value stream.
       This is the same format used by both internal and leaf nodes. */
    size_t num_values = leaf->num_voxels;
    leaf->data_size = num_values * vsize;
    leaf->data = (uint8_t *)tvdb__alloc(a, leaf->data_size > 0 ? leaf->data_size : 1);
    if (!leaf->data) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }
    memset(leaf->data, 0, leaf->data_size > 0 ? leaf->data_size : 1);

    /* BOOL leaves: OpenVDB's LeafNode<bool>::readBuffers writes
       value_mask + mOrigin (12 bytes) + bit-packed data buffer.
       Bypass the generic mask_values machinery (no flag byte, no
       inactive-value selection, no compression). The 12-byte origin
       isn't pre-read in the topology pass for new-version files, so
       skip it here. */
    if (vt == TVDB_VALUE_BOOL) {
        if (params->file_version >= TVDB_FILE_VERSION_NODE_MASK_COMPRESSION &&
            !tvdb__sr_seek_cur(sr, 12)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated bool leaf");
            return TVDB_ERROR_INVALID_DATA;
        }
        size_t packed_bytes = (num_values + 7) / 8;
        uint8_t *packed = (uint8_t *)tvdb__alloc(a, packed_bytes);
        if (!packed) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        if (!tvdb__sr_read(sr, packed_bytes, packed)) {
            tvdb__free(a, packed, packed_bytes);
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Truncated bool leaf");
            return TVDB_ERROR_INVALID_DATA;
        }
        for (size_t s = 0; s < num_values; ++s) {
            leaf->data[s] = (packed[s >> 3] >> (s & 7)) & 1u;
        }
        tvdb__free(a, packed, packed_bytes);
        if (tree->is_point_index_grid) {
            /* fall through to point-index payload below */
        } else {
            return TVDB_OK;
        }
    } else {
    tvdb_status_t st = tvdb__read_mask_values(
        sr, params->compression_flags, params->file_version,
        params->background, num_values, vt,
        &leaf->value_mask, leaf->data,
        params->half_precision, params->scratch, a, err);
    if (st != TVDB_OK) return st;
    }

    if (tree->is_point_index_grid) {
        int64_t num_indices_i64 = 0;
        if (!tvdb__sr_read_i64(sr, &num_indices_i64) || num_indices_i64 < 0) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Failed to read PointIndex leaf index count");
            return TVDB_ERROR_INVALID_DATA;
        }
        leaf->num_point_indices = (uint64_t)num_indices_i64;

        if (leaf->num_point_indices > 0) {
            if (leaf->num_point_indices > (uint64_t)(SIZE_MAX / sizeof(int32_t)) ||
                leaf->num_point_indices > (sr->length - sr->pos) / sizeof(int32_t)) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "PointIndex leaf index count too large");
                return TVDB_ERROR_INVALID_DATA;
            }
            size_t bytes = (size_t)leaf->num_point_indices * sizeof(int32_t);
            leaf->point_indices = (int32_t *)tvdb__alloc(a, bytes);
            if (!leaf->point_indices) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            if (!tvdb__sr_read(sr, bytes, (uint8_t *)leaf->point_indices)) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Failed to read PointIndex leaf indices");
                return TVDB_ERROR_INVALID_DATA;
            }
        }

        int64_t aux_bytes_i64 = 0;
        if (!tvdb__sr_read_i64(sr, &aux_bytes_i64) || aux_bytes_i64 < 0) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Failed to read PointIndex leaf aux size");
            return TVDB_ERROR_INVALID_DATA;
        }
        leaf->point_aux_data_size = (uint64_t)aux_bytes_i64;
        if (leaf->point_aux_data_size > 0) {
            if (leaf->point_aux_data_size > (uint64_t)SIZE_MAX ||
                leaf->point_aux_data_size > sr->length - sr->pos) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "PointIndex leaf aux payload too large");
                return TVDB_ERROR_INVALID_DATA;
            }
            size_t aux_size = (size_t)leaf->point_aux_data_size;
            leaf->point_aux_data = (uint8_t *)tvdb__alloc(a, aux_size);
            if (!leaf->point_aux_data) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            if (!tvdb__sr_read(sr, aux_size, leaf->point_aux_data)) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Failed to read PointIndex aux payload");
                return TVDB_ERROR_INVALID_DATA;
            }
        }
    }

    return TVDB_OK;
}

static tvdb_status_t tvdb__read_node_buffer(
    tvdb_tree_t *tree, tvdb__sr_t *sr, size_t node_idx, int level,
    const tvdb__deser_params_t *params, tvdb_error_t *err) {

    tvdb_node_type_t nt = tree->nodes[node_idx].type;
    switch (nt) {
        case TVDB_NODE_ROOT:
            return tvdb__read_root_buffer(tree, sr, node_idx, level,
                                          params, err);
        case TVDB_NODE_INTERNAL:
            return tvdb__read_internal_buffer(tree, sr, node_idx, level,
                                              params, err);
        case TVDB_NODE_LEAF:
            return tvdb__read_leaf_buffer(tree, sr, node_idx, level,
                                          params, err);
        default:
            return TVDB_ERROR_INVALID_DATA;
    }
}

/* ========================================================================== */
/*  Tree cleanup                                                              */
/* ========================================================================== */

static void tvdb__tree_destroy(tvdb_tree_t *tree) {
    if (!tree->alloc) return;
    tvdb_allocator_t *a = tree->alloc;

    for (size_t i = 0; i < tree->num_nodes; i++) {
        tvdb_tree_node_t *n = &tree->nodes[i];
        switch (n->type) {
            case TVDB_NODE_ROOT: {
                tvdb_root_node_t *r = &n->u.root;
                if (r->tile_origins)
                    tvdb__free(a, r->tile_origins,
                               (size_t)r->num_tiles * 3 * sizeof(int32_t));
                if (r->tile_values)
                    tvdb__free(a, r->tile_values,
                               (size_t)r->num_tiles * sizeof(tvdb_value_t));
                if (r->tile_active)
                    tvdb__free(a, r->tile_active,
                               (size_t)r->num_tiles * sizeof(int));
                if (r->child_origins)
                    tvdb__free(a, r->child_origins,
                               (size_t)r->num_children * 3 * sizeof(int32_t));
                if (r->child_indices)
                    tvdb__free(a, r->child_indices,
                               (size_t)r->num_children * sizeof(size_t));
                break;
            }
            case TVDB_NODE_INTERNAL: {
                tvdb_internal_node_t *in = &n->u.internal;
                tvdb__nodemask_destroy(&in->child_mask);
                tvdb__nodemask_destroy(&in->value_mask);
                if (in->values && !n->pooled)
                    tvdb__free(a, in->values, in->values_size);
                if (in->child_indices)
                    tvdb__free(a, in->child_indices,
                               in->num_children * sizeof(size_t));
                break;
            }
            case TVDB_NODE_LEAF: {
                tvdb_leaf_node_t *lf = &n->u.leaf;
                tvdb__nodemask_destroy(&lf->value_mask);
                if (lf->data && !n->pooled)
                    tvdb__free(a, lf->data, lf->data_size);
                if (lf->point_indices)
                    tvdb__free(a, lf->point_indices,
                               (size_t)lf->num_point_indices * sizeof(int32_t));
                if (lf->point_aux_data)
                    tvdb__free(a, lf->point_aux_data,
                               (size_t)lf->point_aux_data_size);
                break;
            }
        }
    }

    if (tree->nodes)
        tvdb__free(a, tree->nodes,
                   tree->nodes_capacity * sizeof(tvdb_tree_node_t));
    if (tree->pool) {
        tvdb_pool_chunk_t *c = tree->pool;
        while (c) {
            tvdb_pool_chunk_t *next = c->next;
            tvdb__free(a, c, sizeof(tvdb_pool_chunk_t) + c->size);
            c = next;
        }
    }
    memset(tree, 0, sizeof(*tree));
}

/* ========================================================================== */
/*  Grid cleanup                                                              */
/* ========================================================================== */

static void tvdb__grid_destroy(tvdb_grid_t *grid, tvdb_allocator_t *a) {
    tvdb__grid_descriptor_destroy(&grid->descriptor, a);
    tvdb__metadata_destroy(&grid->metadata);
    tvdb__tree_destroy(&grid->tree);
    if (grid->point_data_blob)
        tvdb__free(a, grid->point_data_blob, grid->point_data_blob_size);
    memset(grid, 0, sizeof(*grid));
}

void tvdb_grid_destroy(tvdb_grid_t *grid, const tvdb_allocator_t *alloc) {
    if (!grid) return;
    tvdb_allocator_t a = alloc ? *alloc : tvdb__default_allocator();
    tvdb__grid_destroy(grid, &a);
}

/* ========================================================================== */
/*  Read a single grid                                                        */
/* ========================================================================== */

static tvdb_status_t tvdb__read_grid_inner(tvdb__sr_t *sr, tvdb_grid_t *grid,
                                          const tvdb_header_t *header,
                                          tvdb__scratch_t *scratch,
                                          tvdb_allocator_t *alloc,
                                          tvdb_error_t *err);

/* The node-value staging buffer is shared across a whole grid load; release it
 * when the grid finishes, on success and on every failure path alike. */
static tvdb_status_t tvdb__read_grid(tvdb__sr_t *sr, tvdb_grid_t *grid,
                                     const tvdb_header_t *header,
                                     tvdb_allocator_t *alloc,
                                     tvdb_error_t *err) {
    tvdb__scratch_t scratch = { NULL, 0 };
    tvdb_status_t st = tvdb__read_grid_inner(sr, grid, header, &scratch, alloc, err);
    if (scratch.data) tvdb__free(alloc, scratch.data, scratch.size);
    return st;
}

static tvdb_status_t tvdb__read_grid_inner(tvdb__sr_t *sr, tvdb_grid_t *grid,
                                          const tvdb_header_t *header,
                                          tvdb__scratch_t *scratch,
                                          tvdb_allocator_t *alloc,
                                          tvdb_error_t *err) {
    uint32_t file_version = header->file_version;
    uint64_t point_blob_start = 0;

    /* Read per-grid compression (v222+) */
    grid->compression_flags = TVDB_COMPRESS_NONE;
    if (file_version >= TVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        if (!tvdb__sr_read_u32(sr, &grid->compression_flags)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Failed to read grid compression flags");
            return TVDB_ERROR_INVALID_DATA;
        }
    } else if (header->compression_flags) {
        grid->compression_flags = header->compression_flags;
    }

    /* Read grid metadata */
    tvdb__metadata_init(&grid->metadata, alloc);
    tvdb_status_t st = tvdb__read_meta(sr, &grid->metadata, alloc, err);
    if (st != TVDB_OK) return st;

    /* Read transform */
    st = tvdb__read_transform(sr, &grid->transform, alloc, err);
    if (st != TVDB_OK) return st;
    point_blob_start = sr->pos;

    /* Parse grid type into layout */
    st = tvdb__parse_grid_type(grid->descriptor.grid_type,
                               &grid->tree.layout, err);
    if (st != TVDB_OK) return st;

    /* Check for grid instances */
    if (grid->descriptor.instance_parent_name &&
        grid->descriptor.instance_parent_name[0] != '\0') {
        /* Instance grid — skip tree data */
        return TVDB_OK;
    }

    /* Set up tree */
    grid->tree.alloc = alloc;
    grid->tree.nodes = NULL;
    grid->tree.num_nodes = 0;
    grid->tree.nodes_capacity = 0;
    grid->tree.is_point_data_grid =
        (grid->descriptor.grid_type &&
         strstr(grid->descriptor.grid_type, "ptdataidx") != NULL) ? 1 : 0;
    grid->tree.is_point_index_grid =
        (grid->descriptor.grid_type &&
         strstr(grid->descriptor.grid_type, "ptidx") != NULL &&
         strstr(grid->descriptor.grid_type, "ptdataidx") == NULL) ? 1 : 0;

    /* Determine value type (half-float promotion) */
    tvdb_value_type_t vtype = grid->tree.layout.levels[0].value_type;
    if (grid->descriptor.save_float_as_half && vtype == TVDB_VALUE_FLOAT) {
        /* Values are stored as half but we'll read them as half and promote
           later if needed. For now, keep the layout as FLOAT since the tree
           values are stored at the size indicated in the type string. */
    }

    tvdb__deser_params_t params;
    params.scratch = scratch;
    params.file_version      = file_version;
    params.compression_flags = grid->compression_flags;
    params.half_precision    = grid->descriptor.save_float_as_half;
    params.background.type   = vtype;
    memset(&params.background.u, 0, sizeof(params.background.u));

    /* TreeBase: read buffer count */
    {
        int32_t buffer_count;
        if (!tvdb__sr_read_i32(sr, &buffer_count)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                            "Failed to read tree buffer count");
            return TVDB_ERROR_INVALID_DATA;
        }
        /* multi-buffer trees are no longer supported; ignore value */
    }

    /* Allocate root node */
    size_t root_idx = tvdb__tree_alloc_node(&grid->tree);
    if (root_idx == (size_t)-1) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

    /* Read topology */
    st = tvdb__read_node_topology(&grid->tree, sr, root_idx, 0, &params, err);
    if (st != TVDB_OK) return st;

    /* Update background from root */
    params.background = grid->tree.nodes[root_idx].u.root.background;

    /* PointDataGrid: leaf buffers contain serialized AttributeSet blobs that
       tinyvdb does not parse. Topology is already loaded; skip the buffer
       phase and jump to the grid's end marker so subsequent grids still
       read correctly. */
    if (grid->tree.is_point_data_grid) {
        if (grid->descriptor.end_byte_offset > point_blob_start &&
            grid->descriptor.end_byte_offset <= sr->length) {
            size_t blob_size =
                (size_t)(grid->descriptor.end_byte_offset - point_blob_start);
            grid->point_data_blob = (uint8_t *)tvdb__alloc(alloc, blob_size);
            if (!grid->point_data_blob) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            memcpy(grid->point_data_blob, sr->data + point_blob_start, blob_size);
            grid->point_data_blob_size = blob_size;
        }
        if (grid->descriptor.end_byte_offset > 0) {
            tvdb__sr_seek_set(sr, grid->descriptor.end_byte_offset);
        }
        return TVDB_OK;
    }

    /* Read buffers */
    st = tvdb__read_node_buffer(&grid->tree, sr, root_idx, 0, &params, err);
    return st;
}

/* ========================================================================== */
/*  Header parsing                                                            */
/* ========================================================================== */

static tvdb_status_t tvdb__read_header(tvdb__sr_t *sr, tvdb_header_t *header,
                                       tvdb_error_t *err) {
    memset(header, 0, sizeof(*header));

    /* Magic number */
    int64_t magic;
    if (!tvdb__sr_read_i64(sr, &magic)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                        "Failed to read magic number");
        return TVDB_ERROR_INVALID_HEADER;
    }
    if (magic != TVDB_MAGIC) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                        "Invalid VDB magic number");
        return TVDB_ERROR_INVALID_HEADER;
    }

    /* File version */
    if (!tvdb__sr_read_u32(sr, &header->file_version)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                        "Failed to read file version");
        return TVDB_ERROR_INVALID_HEADER;
    }

    if (header->file_version < TVDB_FILE_VERSION_SELECTIVE_COMPRESSION) {
        tvdb__set_error(err, TVDB_ERROR_UNSUPPORTED_VERSION,
                        "VDB file version < 220 not supported");
        return TVDB_ERROR_UNSUPPORTED_VERSION;
    }

    /* Library version (v211+) */
    if (header->file_version >= 211) {
        if (!tvdb__sr_read_u32(sr, &header->major_version) ||
            !tvdb__sr_read_u32(sr, &header->minor_version)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                            "Failed to read library version");
            return TVDB_ERROR_INVALID_HEADER;
        }
    }

    /* Grid offsets flag (v212+) */
    header->has_grid_offsets = 0;
    if (header->file_version >= 212) {
        uint8_t flag = 0;
        if (!tvdb__sr_read_u8(sr, &flag)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                            "Failed to read grid offsets flag");
            return TVDB_ERROR_INVALID_HEADER;
        }
        header->has_grid_offsets = flag ? 1 : 0;
    }

    if (!header->has_grid_offsets) {
        tvdb__set_error(err, TVDB_ERROR_UNIMPLEMENTED,
                        "VDB without grid offsets not supported");
        return TVDB_ERROR_UNIMPLEMENTED;
    }

    /* Global compression flag (v220 to v221) */
    header->compression_flags = TVDB_COMPRESS_NONE;
    if (header->file_version >= TVDB_FILE_VERSION_SELECTIVE_COMPRESSION &&
        header->file_version < TVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        uint8_t is_compressed = 0;
        if (!tvdb__sr_read_u8(sr, &is_compressed)) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                            "Failed to read compression flag");
            return TVDB_ERROR_INVALID_HEADER;
        }
        if (is_compressed) {
            header->compression_flags =
                TVDB_COMPRESS_ZIP | TVDB_COMPRESS_ACTIVE_MASK;
        }
    }

    /* UUID (36 bytes ASCII) */
    uint8_t uuid_bytes[36];
    if (!tvdb__sr_read(sr, 36, uuid_bytes)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_HEADER,
                        "Failed to read UUID");
        return TVDB_ERROR_INVALID_HEADER;
    }
    memcpy(header->uuid, uuid_bytes, 36);
    header->uuid[36] = '\0';

    header->offset_to_data = sr->pos;
    return TVDB_OK;
}

/* ========================================================================== */
/*  Public API implementation                                                 */
/* ========================================================================== */

const char *tvdb_status_string(tvdb_status_t status) {
    switch (status) {
        case TVDB_OK:                          return "OK";
        case TVDB_ERROR_INVALID_FILE:          return "Invalid file";
        case TVDB_ERROR_INVALID_HEADER:        return "Invalid header";
        case TVDB_ERROR_INVALID_DATA:          return "Invalid data";
        case TVDB_ERROR_INVALID_ARGUMENT:      return "Invalid argument";
        case TVDB_ERROR_UNSUPPORTED_VERSION:   return "Unsupported version";
        case TVDB_ERROR_UNSUPPORTED_GRID_TYPE: return "Unsupported grid type";
        case TVDB_ERROR_UNSUPPORTED_COMPRESSION:
            return "Unsupported compression";
        case TVDB_ERROR_UNSUPPORTED_TRANSFORM: return "Unsupported transform";
        case TVDB_ERROR_DECOMPRESSION_FAILED:  return "Decompression failed";
        case TVDB_ERROR_OUT_OF_MEMORY:         return "Out of memory";
        case TVDB_ERROR_IO:                    return "I/O error";
        case TVDB_ERROR_MMAP_FAILED:           return "mmap failed";
        case TVDB_ERROR_PATH_CONVERSION:       return "Path conversion failed";
        case TVDB_ERROR_UNIMPLEMENTED:         return "Unimplemented";
        default:                               return "Unknown error";
    }
}

int tvdb_nodemask_is_on(const tvdb_nodemask_t *m, int32_t i) {
    return tvdb__nodemask_is_on(m, i);
}

size_t tvdb_nodemask_count_on(const tvdb_nodemask_t *m) {
    return tvdb__nodemask_count_on(m);
}

/* Parse header, file metadata and grid descriptors from file->file_data.
   On failure every allocation made so far (including the file data) is
   released through tvdb_file_close, leaving *file zeroed. */
static tvdb_status_t tvdb__file_parse(tvdb_file_t *file, tvdb_error_t *err) {
    int swap_endian = tvdb_is_big_endian();
    tvdb__sr_t sr;
    tvdb__sr_init(&sr, file->file_data.data, file->file_data.data_len,
                  swap_endian);

    tvdb_status_t st = tvdb__read_header(&sr, &file->header, err);
    if (st != TVDB_OK) goto fail;

    /* Read file-level metadata */
    tvdb__metadata_init(&file->file_metadata, &file->alloc);
    st = tvdb__read_meta(&sr, &file->file_metadata, &file->alloc, err);
    if (st != TVDB_OK) goto fail;

    /* Read grid descriptors. A descriptor occupies at least three string
       lengths and three offsets, which bounds the count by the remaining
       bytes before anything is allocated. */
    int32_t grid_count = 0;
    if (!tvdb__sr_read_i32(&sr, &grid_count) || grid_count < 0 ||
        (uint64_t)grid_count > (sr.length - sr.pos) / 36u) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Failed to read grid count");
        st = TVDB_ERROR_INVALID_DATA;
        goto fail;
    }

    if (grid_count > 0) {
        size_t n = (size_t)grid_count;
        file->grids = (tvdb_grid_t *)tvdb__alloc(&file->alloc,
                                                 n * sizeof(tvdb_grid_t));
        if (!file->grids) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            st = TVDB_ERROR_OUT_OF_MEMORY;
            goto fail;
        }
        memset(file->grids, 0, n * sizeof(tvdb_grid_t));
        file->num_grids = n;

        for (size_t i = 0; i < file->num_grids; i++) {
            st = tvdb__read_grid_descriptor(&sr, file->header.file_version,
                                            &file->grids[i].descriptor,
                                            &file->alloc, err);
            if (st != TVDB_OK) goto fail;
            /* Validate grid offsets */
            tvdb_grid_descriptor_t *gd = &file->grids[i].descriptor;
            if (gd->grid_byte_offset > gd->end_byte_offset ||
                gd->end_byte_offset > file->file_data.data_len ||
                !tvdb__sr_seek_set(&sr, gd->end_byte_offset)) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "Grid byte offsets out of range");
                st = TVDB_ERROR_INVALID_DATA;
                goto fail;
            }
        }
    }

    return TVDB_OK;

fail:
    tvdb_file_close(file);
    return st;
}

tvdb_status_t tvdb_file_open(tvdb_file_t *file, const char *filepath_utf8,
                             const tvdb_allocator_t *alloc,
                             tvdb_error_t *err) {
    if (!file || !filepath_utf8) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                        "NULL file or filepath");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    memset(file, 0, sizeof(*file));
    file->alloc = alloc ? *alloc : tvdb__default_allocator();

    /* Open file data (mmap or buffer) */
    tvdb_status_t st = tvdb__file_data_open(&file->file_data, filepath_utf8,
                                            &file->alloc, err);
    if (st != TVDB_OK) return st;

    return tvdb__file_parse(file, err);
}

tvdb_status_t tvdb_file_open_memory(tvdb_file_t *file, const uint8_t *data,
                                    size_t data_len,
                                    const tvdb_allocator_t *alloc,
                                    tvdb_error_t *err) {
    if (!file || !data) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL argument");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    memset(file, 0, sizeof(*file));
    file->alloc = alloc ? *alloc : tvdb__default_allocator();

    file->file_data.data     = data;
    file->file_data.data_len = (uint64_t)data_len;
    file->file_data.source   = TVDB_SOURCE_EXTERNAL;

    return tvdb__file_parse(file, err);
}

void tvdb_file_close(tvdb_file_t *file) {
    if (!file) return;

    for (size_t i = 0; i < file->num_grids; i++) {
        tvdb__grid_destroy(&file->grids[i], &file->alloc);
    }
    if (file->grids) {
        tvdb__free(&file->alloc, file->grids,
                   file->num_grids * sizeof(tvdb_grid_t));
    }

    tvdb__metadata_destroy(&file->file_metadata);
    tvdb__file_data_close(&file->file_data);

    memset(file, 0, sizeof(*file));
}

tvdb_status_t tvdb_read_all_grids(tvdb_file_t *file, tvdb_error_t *err) {
    if (!file) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL file");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    int swap_endian = tvdb_is_big_endian();
    tvdb__sr_t sr;
    tvdb__sr_init(&sr, file->file_data.data, file->file_data.data_len,
                  swap_endian);

    /* A single undecodable grid (e.g. an unsupported value type, or a corrupt
       payload) should not discard the other grids in a multi-grid file -- so
       skip the failed grid and keep loading the rest, remembering the first
       failure to report to the caller at the end. */
    tvdb_status_t first_err = TVDB_OK;
    char          first_err_msg[TVDB_MAX_ERROR_MSG];
    int32_t       first_err_grid = -1;
    first_err_msg[0] = '\0';

    for (size_t i = 0; i < file->num_grids; i++) {
        if (err) err->grid_index = (int32_t)i;

        tvdb_grid_t *current = &file->grids[i];
        tvdb_grid_t loaded;
        memset(&loaded, 0, sizeof(loaded));
        /* Borrow the descriptor while reading into independent storage. Repeated
           reads must not overwrite owning metadata/tree pointers and leak them. */
        loaded.descriptor = current->descriptor;
        tvdb_status_t st;
        if (!file->file_data.data || !loaded.descriptor.grid_byte_offset ||
            loaded.descriptor.grid_byte_offset > loaded.descriptor.end_byte_offset ||
            loaded.descriptor.end_byte_offset > file->file_data.data_len) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Grid has no valid serialized offsets");
            st = TVDB_ERROR_INVALID_DATA;
        } else {
            tvdb__sr_seek_set(&sr, loaded.descriptor.grid_byte_offset);
            st = tvdb__read_grid(&sr, &loaded, &file->header, &file->alloc, err);
        }
        if (st != TVDB_OK) {
            if (first_err == TVDB_OK) {
                first_err = st;
                first_err_grid = (int32_t)i;
                if (err) {
                    memcpy(first_err_msg, err->message, sizeof(first_err_msg));
                    first_err_msg[sizeof(first_err_msg) - 1] = '\0';
                }
            }
            memset(&loaded.descriptor, 0, sizeof(loaded.descriptor));
            tvdb__grid_destroy(&loaded, &file->alloc);
        } else {
            memset(&current->descriptor, 0, sizeof(current->descriptor));
            tvdb__grid_destroy(current, &file->alloc);
            *current = loaded;
        }
    }

    if (first_err != TVDB_OK) {
        /* Restore the first failure (later grids' reads may have overwritten
           err) so the caller still observes that something went wrong. */
        tvdb__set_error(err, first_err, first_err_msg);
        if (err) err->grid_index = first_err_grid;
        return first_err;
    }

    if (err) err->grid_index = -1;
    return TVDB_OK;
}

size_t tvdb_grid_count(const tvdb_file_t *file) {
    return file ? file->num_grids : 0;
}

const char *tvdb_grid_name(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return NULL;
    return file->grids[idx].descriptor.grid_name;
}

const char *tvdb_grid_type_name(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return NULL;
    return file->grids[idx].descriptor.grid_type;
}

int tvdb_grid_is_point_data(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return 0;
    return file->grids[idx].tree.is_point_data_grid ? 1 : 0;
}

int tvdb_grid_is_point_index(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return 0;
    return file->grids[idx].tree.is_point_index_grid ? 1 : 0;
}

const uint8_t *tvdb_grid_point_data_blob(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return NULL;
    return file->grids[idx].point_data_blob;
}

size_t tvdb_grid_point_data_blob_size(const tvdb_file_t *file, size_t idx) {
    if (!file || idx >= file->num_grids) return 0;
    return file->grids[idx].point_data_blob_size;
}

tvdb_status_t tvdb_grid_set_point_data_blob(tvdb_file_t *file, size_t idx,
                                            const uint8_t *data, size_t size,
                                            tvdb_error_t *err) {
    if (!file || idx >= file->num_grids) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                        "grid index out of range");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    tvdb_grid_t *g = &file->grids[idx];
    if (!g->tree.is_point_data_grid) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT,
                        "grid is not PointDataGrid");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    if (g->point_data_blob) {
        tvdb__free(&file->alloc, g->point_data_blob, g->point_data_blob_size);
        g->point_data_blob = NULL;
        g->point_data_blob_size = 0;
    }

    if (!data || size == 0) return TVDB_OK;

    g->point_data_blob = (uint8_t *)tvdb__alloc(&file->alloc, size);
    if (!g->point_data_blob) {
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }
    memcpy(g->point_data_blob, data, size);
    g->point_data_blob_size = size;
    return TVDB_OK;
}

/* ========================================================================== */
/* ========================================================================== */
/*  WRITING IMPLEMENTATION                                                    */
/* ========================================================================== */
/* ========================================================================== */

/* ========================================================================== */
/*  Stream writer (growing buffer)                                            */
/* ========================================================================== */

typedef struct tvdb__sw {
    uint8_t          *data;
    size_t            len;
    size_t            cap;
    tvdb_allocator_t *alloc;
    int               swap_endian;
    int               compression_level; /* 1-9, default 5 */
    int               failed; /* sticky: a write could not grow the buffer */
} tvdb__sw_t;

static void tvdb__sw_init(tvdb__sw_t *sw, tvdb_allocator_t *alloc) {
    memset(sw, 0, sizeof(*sw));
    sw->alloc = alloc;
    sw->swap_endian = tvdb_is_big_endian();
    sw->compression_level = 5;
}

static void tvdb__sw_destroy(tvdb__sw_t *sw) {
    if (sw->data && sw->alloc)
        tvdb__free(sw->alloc, sw->data, sw->cap);
    memset(sw, 0, sizeof(*sw));
}

static int tvdb__sw_ensure(tvdb__sw_t *sw, size_t additional) {
    if (sw->failed) return 0;
    if (additional > SIZE_MAX - sw->len) { sw->failed = 1; return 0; }
    size_t needed = sw->len + additional;
    if (needed <= sw->cap) return 1;
    size_t new_cap = sw->cap ? sw->cap : 4096;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) { new_cap = needed; break; }
        new_cap *= 2;
    }
    uint8_t *nd = (uint8_t *)tvdb__realloc(sw->alloc, sw->data,
                                            sw->cap, new_cap);
    if (!nd) { sw->failed = 1; return 0; }
    sw->data = nd;
    sw->cap = new_cap;
    return 1;
}

static int tvdb__sw_write(tvdb__sw_t *sw, const void *src, size_t n) {
    if (!tvdb__sw_ensure(sw, n)) return 0;
    if (n) memcpy(sw->data + sw->len, src, n);
    sw->len += n;
    return 1;
}

static int tvdb__sw_write_u8(tvdb__sw_t *sw, uint8_t v) {
    return tvdb__sw_write(sw, &v, 1);
}

static int tvdb__sw_write_i8(tvdb__sw_t *sw, int8_t v) {
    return tvdb__sw_write(sw, &v, 1);
}

static int tvdb__sw_write_u32(tvdb__sw_t *sw, uint32_t v) {
    if (sw->swap_endian) tvdb__swap4(&v);
    return tvdb__sw_write(sw, &v, 4);
}

static int tvdb__sw_write_i32(tvdb__sw_t *sw, int32_t v) {
    return tvdb__sw_write_u32(sw, (uint32_t)v);
}

static int tvdb__sw_write_u64(tvdb__sw_t *sw, uint64_t v) {
    if (sw->swap_endian) tvdb__swap8(&v);
    return tvdb__sw_write(sw, &v, 8);
}

static int tvdb__sw_write_i64(tvdb__sw_t *sw, int64_t v) {
    return tvdb__sw_write_u64(sw, (uint64_t)v);
}

static int tvdb__sw_write_f32(tvdb__sw_t *sw, float v) {
    uint32_t u; memcpy(&u, &v, 4);
    return tvdb__sw_write_u32(sw, u);
}

static int tvdb__sw_write_f64(tvdb__sw_t *sw, double v) {
    uint64_t u; memcpy(&u, &v, 8);
    return tvdb__sw_write_u64(sw, u);
}

static int tvdb__sw_write_vec3d(tvdb__sw_t *sw, const double v[3]) {
    return tvdb__sw_write_f64(sw, v[0])
        && tvdb__sw_write_f64(sw, v[1])
        && tvdb__sw_write_f64(sw, v[2]);
}

/* Write a length-prefixed string (uint32 length + chars). */
static int tvdb__sw_write_string(tvdb__sw_t *sw, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    if (!tvdb__sw_write_u32(sw, len)) return 0;
    if (len > 0) return tvdb__sw_write(sw, s, len);
    return 1;
}

/* Write at a specific offset (for backpatching), without moving write pos. */
static void tvdb__sw_patch_u64(tvdb__sw_t *sw, size_t offset, uint64_t v) {
    if (offset + 8 > sw->len) return;
    if (sw->swap_endian) tvdb__swap8(&v);
    memcpy(sw->data + offset, &v, 8);
}

static int tvdb__sw_write_value(tvdb__sw_t *sw, const tvdb_value_t *v) {
    switch (v->type) {
        case TVDB_VALUE_BOOL:   return tvdb__sw_write_u8(sw, (uint8_t)v->u.b);
        case TVDB_VALUE_INT32:  return tvdb__sw_write_i32(sw, v->u.i32);
        case TVDB_VALUE_INT64:  return tvdb__sw_write_i64(sw, v->u.i64);
        case TVDB_VALUE_FLOAT:  return tvdb__sw_write_f32(sw, v->u.f);
        case TVDB_VALUE_DOUBLE: return tvdb__sw_write_f64(sw, v->u.d);
        case TVDB_VALUE_VEC3I:
            return tvdb__sw_write_i32(sw, v->u.vec3i[0]) &&
                   tvdb__sw_write_i32(sw, v->u.vec3i[1]) &&
                   tvdb__sw_write_i32(sw, v->u.vec3i[2]);
        case TVDB_VALUE_VEC3F:
            return tvdb__sw_write_f32(sw, v->u.vec3f[0]) &&
                   tvdb__sw_write_f32(sw, v->u.vec3f[1]) &&
                   tvdb__sw_write_f32(sw, v->u.vec3f[2]);
        case TVDB_VALUE_VEC3D:
            return tvdb__sw_write_f64(sw, v->u.vec3d[0]) &&
                   tvdb__sw_write_f64(sw, v->u.vec3d[1]) &&
                   tvdb__sw_write_f64(sw, v->u.vec3d[2]);
        default: return 0;
    }
}

/* ========================================================================== */
/*  Compression for writing                                                   */
/* ========================================================================== */

static tvdb_status_t tvdb__compress_and_write(
    tvdb__sw_t *sw, const uint8_t *src_data, size_t element_size, size_t count,
    uint32_t compression_mask, int clevel, tvdb_allocator_t *alloc,
    tvdb_error_t *err) {

    size_t total_size = tvdb__safe_mul(element_size, count);
    if (element_size > 0 && count > 0 && total_size == 0) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Buffer size overflow");
        return TVDB_ERROR_INVALID_DATA;
    }

    /* Endian-swap a copy if needed */
    uint8_t *swapped = NULL;
    if (sw->swap_endian && element_size > 1) {
        swapped = (uint8_t *)tvdb__alloc(alloc, total_size);
        if (!swapped) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        memcpy(swapped, src_data, total_size);
        for (size_t i = 0; i < count; i++) {
            if (element_size == 2) tvdb__swap2(swapped + i * 2);
            else if (element_size == 4) tvdb__swap4(swapped + i * 4);
            else if (element_size == 8) tvdb__swap8(swapped + i * 8);
        }
        src_data = swapped;
    }

    if (compression_mask & TVDB_COMPRESS_BLOSC) {
        if (total_size == 0) {
            /* OpenVDB's bloscToStream writes `-Int64(inBytes) = 0` for
               zero-length payloads; tinyvdb used to write -1 here, which
               OpenVDB then misreads as "expected a 0-byte chunk, got 1-byte".
               Keep 0 so tinyvdb-written files round-trip through openvdb. */
            tvdb__sw_write_i64(sw, 0);
            if (swapped) tvdb__free(alloc, swapped, total_size);
            return TVDB_OK;
        }
        /* BLOSC overhead: 16-byte header + nblocks*4 offsets + LZ4 expansion */
        size_t dest_cap = (size_t)LZ4_compressBound((int)total_size) + 16 + 256;
        uint8_t *dest = (uint8_t *)tvdb__alloc(alloc, dest_cap);
        if (!dest) {
            if (swapped) tvdb__free(alloc, swapped, total_size);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        int csize = tvdb__compress_blosc(
            src_data, total_size, dest, dest_cap,
            element_size, clevel > 0 ? clevel : 5, alloc);
        if (csize <= 0 || (size_t)csize >= total_size) {
            /* OpenVDB's sentinel for "uncompressed follows" is
               -Int64(inBytes); tinyvdb used to write -1 which openvdb
               misreads as "expected 1-byte chunk". */
            tvdb__sw_write_i64(sw, -(int64_t)total_size);
            tvdb__sw_write(sw, src_data, total_size);
        } else {
            tvdb__sw_write_i64(sw, (int64_t)csize);
            tvdb__sw_write(sw, dest, (size_t)csize);
        }
        tvdb__free(alloc, dest, dest_cap);
    } else if (compression_mask & TVDB_COMPRESS_ZIP) {
        if (total_size == 0) {
            /* Match openvdb's zip-path zero-length sentinel. */
            tvdb__sw_write_i64(sw, 0);
            if (swapped) tvdb__free(alloc, swapped, total_size);
            return TVDB_OK;
        }
#if defined(TVDB_USE_SYSTEM_ZLIB)
        uLongf dest_len = compressBound((uLong)total_size);
        uint8_t *dest = (uint8_t *)tvdb__alloc(alloc, (size_t)dest_len);
        if (!dest) {
            if (swapped) tvdb__free(alloc, swapped, total_size);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        int zret = compress(dest, &dest_len, src_data, (uLong)total_size);
        if (zret != Z_OK || (size_t)dest_len >= total_size) {
            tvdb__sw_write_i64(sw, -(int64_t)total_size);
            tvdb__sw_write(sw, src_data, total_size);
        } else {
            tvdb__sw_write_i64(sw, (int64_t)dest_len);
            tvdb__sw_write(sw, dest, (size_t)dest_len);
        }
        tvdb__free(alloc, dest, (size_t)compressBound((uLong)total_size));
#else
        mz_ulong dest_len = mz_compressBound((mz_ulong)total_size);
        uint8_t *dest = (uint8_t *)tvdb__alloc(alloc, (size_t)dest_len);
        if (!dest) {
            if (swapped) tvdb__free(alloc, swapped, total_size);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        int zret = mz_compress(dest, &dest_len, src_data, (mz_ulong)total_size);
        if (zret != MZ_OK || (size_t)dest_len >= total_size) {
            tvdb__sw_write_i64(sw, -(int64_t)total_size);
            tvdb__sw_write(sw, src_data, total_size);
        } else {
            tvdb__sw_write_i64(sw, (int64_t)dest_len);
            tvdb__sw_write(sw, dest, (size_t)dest_len);
        }
        tvdb__free(alloc, dest, (size_t)mz_compressBound((mz_ulong)total_size));
#endif
    } else {
        /* No compression */
        tvdb__sw_write(sw, src_data, total_size);
    }

    if (swapped) tvdb__free(alloc, swapped, total_size);
    return TVDB_OK;
}

/* ========================================================================== */
/*  Write mask-compressed values                                              */
/* ========================================================================== */

static int tvdb__values_equal(const uint8_t *a, const uint8_t *b, size_t sz) {
    return memcmp(a, b, sz) == 0;
}

/* Demote one in-memory real value to its stored half components. */
static void tvdb__value_to_half(const uint8_t *src, uint8_t *dst,
                                tvdb_value_type_t t) {
    size_t n = tvdb__half_components(t);
    for (size_t c = 0; c < n; ++c) {
        float f;
        if (t == TVDB_VALUE_DOUBLE || t == TVDB_VALUE_VEC3D) {
            double d;
            memcpy(&d, src + c * 8, 8);
            f = (float)d;
        } else {
            memcpy(&f, src + c * 4, 4);
        }
        uint16_t h = tvdb__float_to_half(f);
        memcpy(dst + c * 2, &h, 2);
    }
}

static tvdb_status_t tvdb__write_mask_values(
    tvdb__sw_t *sw, uint32_t compression_flags, tvdb_value_t background,
    size_t num_values, tvdb_value_type_t value_type,
    const tvdb_nodemask_t *value_mask, const uint8_t *values,
    int half_precision,
    tvdb_allocator_t *alloc, tvdb_error_t *err) {

    size_t vsize = tvdb_value_type_size(value_type);
    size_t half_comps = half_precision ? tvdb__half_components(value_type) : 0;
    int is_half = half_comps != 0;
    size_t file_elem_size = is_half ? 2 * half_comps : vsize;
    int mask_compressed = (compression_flags & TVDB_COMPRESS_ACTIVE_MASK) != 0;

    if (vsize == 0 || vsize > 32 || (num_values && !values)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Unsupported value buffer");
        return TVDB_ERROR_INVALID_DATA;
    }

    /* Classify inactive values exactly like OpenVDB's MaskCompress so the
       reader (ours or OpenVDB's) reconstructs them bit-for-bit. Values are
       compared at full precision; inactive values are stored at full ValueT
       width even for half grids. */
    uint8_t bg_bytes[32], neg_bg_bytes[32];
    memset(bg_bytes, 0, sizeof(bg_bytes));
    memset(neg_bg_bytes, 0, sizeof(neg_bg_bytes));
    memcpy(bg_bytes, &background.u, vsize);
    {
        tvdb_value_t neg_bg = tvdb__negate_value(background);
        memcpy(neg_bg_bytes, &neg_bg.u, vsize);
    }

    uint8_t inactive_val[2][32];
    memcpy(inactive_val[0], bg_bytes, sizeof(bg_bytes));
    memcpy(inactive_val[1], bg_bytes, sizeof(bg_bytes));
    int num_unique = 0;
    if (mask_compressed) {
        for (size_t i = 0; i < num_values && num_unique < 3; i++) {
            if (tvdb__nodemask_is_on(value_mask, (int32_t)i)) continue;
            const uint8_t *v = values + i * vsize;
            int unique = !((num_unique > 0 &&
                            tvdb__values_equal(v, inactive_val[0], vsize)) ||
                           (num_unique > 1 &&
                            tvdb__values_equal(v, inactive_val[1], vsize)));
            if (unique) {
                if (num_unique < 2) memcpy(inactive_val[num_unique], v, vsize);
                ++num_unique;
            }
        }
    }

    int8_t flag = TVDB_NO_MASK_AND_ALL_VALS;
    if (mask_compressed) {
        flag = TVDB_NO_MASK_OR_INACTIVE_VALS;
        if (num_unique == 1) {
            if (!tvdb__values_equal(inactive_val[0], bg_bytes, vsize)) {
                flag = tvdb__values_equal(inactive_val[0], neg_bg_bytes, vsize)
                           ? TVDB_NO_MASK_AND_MINUS_BG
                           : TVDB_NO_MASK_AND_ONE_INACTIVE_VAL;
            }
        } else if (num_unique == 2) {
            int v0_bg = tvdb__values_equal(inactive_val[0], bg_bytes, vsize);
            int v1_bg = tvdb__values_equal(inactive_val[1], bg_bytes, vsize);
            if (!v0_bg && !v1_bg) {
                flag = TVDB_MASK_AND_TWO_INACTIVE_VALS;
            } else if (v1_bg) {
                flag = tvdb__values_equal(inactive_val[0], neg_bg_bytes, vsize)
                           ? TVDB_MASK_AND_NO_INACTIVE_VALS
                           : TVDB_MASK_AND_ONE_INACTIVE_VAL;
            } else {
                /* v0 is the background: swap so val1 is the background. */
                uint8_t tmp[32];
                memcpy(tmp, inactive_val[0], vsize);
                memcpy(inactive_val[0], inactive_val[1], vsize);
                memcpy(inactive_val[1], tmp, vsize);
                flag = tvdb__values_equal(inactive_val[0], neg_bg_bytes, vsize)
                           ? TVDB_MASK_AND_NO_INACTIVE_VALS
                           : TVDB_MASK_AND_ONE_INACTIVE_VAL;
            }
        } else if (num_unique > 2) {
            flag = TVDB_NO_MASK_AND_ALL_VALS;
        }
    }

    tvdb__sw_write_i8(sw, flag);

    if (flag == TVDB_NO_MASK_AND_ONE_INACTIVE_VAL ||
        flag == TVDB_MASK_AND_ONE_INACTIVE_VAL ||
        flag == TVDB_MASK_AND_TWO_INACTIVE_VALS) {
        int count = flag == TVDB_MASK_AND_TWO_INACTIVE_VALS ? 2 : 1;
        for (int k = 0; k < count; ++k) {
            uint8_t out[32];
            memcpy(out, inactive_val[k], vsize);
            if (is_half) {
                /* OpenVDB writes truncateRealToHalf(value) at full width. */
                uint8_t h[6];
                tvdb__value_to_half(inactive_val[k], h, value_type);
                tvdb__half_to_value(h, out, value_type);
            }
            tvdb_value_t v;
            memset(&v, 0, sizeof(v));
            v.type = background.type;
            memcpy(&v.u, out, vsize);
            tvdb__sw_write_value(sw, &v);
        }
    }

    if (flag == TVDB_MASK_AND_NO_INACTIVE_VALS ||
        flag == TVDB_MASK_AND_ONE_INACTIVE_VAL ||
        flag == TVDB_MASK_AND_TWO_INACTIVE_VALS) {
        /* Selection mask: on where the inactive value equals val1. */
        tvdb_nodemask_t sel;
        tvdb__nodemask_init(&sel);
        if (!tvdb__nodemask_alloc(&sel, value_mask->log2dim, alloc)) {
            tvdb__nodemask_destroy(&sel);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < num_values; i++) {
            if (!tvdb__nodemask_is_on(value_mask, (int32_t)i) &&
                tvdb__values_equal(values + i * vsize, inactive_val[1], vsize))
                sel.bits.data[i / 8] |= (uint8_t)(1u << (i % 8));
        }
        tvdb__sw_write(sw, sel.bits.data, sel.bits.num_bytes);
        tvdb__nodemask_destroy(&sel);
    }

    /* Values to store: all of them, or the active ones only. */
    size_t write_count = flag == TVDB_NO_MASK_AND_ALL_VALS
                             ? num_values
                             : tvdb__nodemask_count_on(value_mask);
    if (write_count > num_values) write_count = num_values;

    /* OpenVDB's HalfWriter::write has `if (count < 1) return;`, so half
       grids emit nothing (not even a chunk header) for zero values. */
    if (is_half && write_count == 0) return TVDB_OK;

    const uint8_t *payload = values;
    uint8_t *packed = NULL;
    size_t packed_size = 0;
    if (is_half || write_count != num_values) {
        packed_size = tvdb__safe_mul(write_count, file_elem_size);
        if (write_count && !packed_size) {
            tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Buffer size overflow");
            return TVDB_ERROR_INVALID_DATA;
        }
        if (packed_size) {
            packed = (uint8_t *)tvdb__alloc(alloc, packed_size);
            if (!packed) {
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
        }
        if (is_half && value_type == TVDB_VALUE_FLOAT &&
            write_count == num_values) {
            /* Common case: whole float buffer, vectorized demotion. */
            tvdb__demote_float_to_half(values, packed, num_values);
        } else {
            size_t idx = 0;
            for (size_t i = 0; i < num_values && idx < write_count; i++) {
                if (write_count != num_values &&
                    !tvdb__nodemask_is_on(value_mask, (int32_t)i)) continue;
                if (is_half)
                    tvdb__value_to_half(values + i * vsize,
                                        packed + idx * file_elem_size,
                                        value_type);
                else
                    memcpy(packed + idx * vsize, values + i * vsize, vsize);
                idx++;
            }
        }
        payload = packed;
    }

    tvdb_status_t st = tvdb__compress_and_write(
        sw, payload, is_half ? 2 : vsize,
        is_half ? write_count * half_comps : write_count,
        compression_flags, sw->compression_level, alloc, err);
    if (packed) tvdb__free(alloc, packed, packed_size);
    return st;
}

/* ========================================================================== */
/*  Write tree topology                                                       */
/* ========================================================================== */

static tvdb_status_t tvdb__write_node_topology(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err);

static tvdb_status_t tvdb__write_root_topology(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    uint32_t compression_flags, int half_precision, tvdb_error_t *err) {
    const tvdb_root_node_t *root = &tree->nodes[node_idx].u.root;

    tvdb__sw_write_value(sw, &root->background);
    tvdb__sw_write_u32(sw, root->num_tiles);
    tvdb__sw_write_u32(sw, root->num_children);

    for (uint32_t i = 0; i < root->num_tiles; i++) {
        tvdb__sw_write_i32(sw, root->tile_origins[i * 3 + 0]);
        tvdb__sw_write_i32(sw, root->tile_origins[i * 3 + 1]);
        tvdb__sw_write_i32(sw, root->tile_origins[i * 3 + 2]);
        tvdb__sw_write_value(sw, &root->tile_values[i]);
        tvdb__sw_write_u8(sw, (uint8_t)(root->tile_active[i] ? 1 : 0));
    }

    for (uint32_t i = 0; i < root->num_children; i++) {
        tvdb__sw_write_i32(sw, root->child_origins[i * 3 + 0]);
        tvdb__sw_write_i32(sw, root->child_origins[i * 3 + 1]);
        tvdb__sw_write_i32(sw, root->child_origins[i * 3 + 2]);

        tvdb_status_t st = tvdb__write_node_topology(
            tree, sw, root->child_indices[i], level + 1,
            root->background, compression_flags, half_precision, err);
        if (st != TVDB_OK) return st;
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_internal_topology(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    const tvdb_internal_node_t *inode = &tree->nodes[node_idx].u.internal;
    tvdb_value_type_t vt = tree->layout.levels[level].value_type;

    tvdb__sw_write(sw, inode->child_mask.bits.data,
                   inode->child_mask.bits.num_bytes);
    tvdb__sw_write(sw, inode->value_mask.bits.data,
                   inode->value_mask.bits.num_bytes);

    int32_t num_values = inode->child_mask.bitsize;
    if (inode->values_size < (size_t)num_values * tvdb_value_type_size(vt)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Internal node value buffer smaller than its slots");
        return TVDB_ERROR_INVALID_DATA;
    }
    /* BOOL internal nodes use the generic compressed-values format
       (1 flag byte + 1 byte per value); not bit-packed. */
    tvdb_status_t mst = tvdb__write_mask_values(
        sw, compression_flags, background, (size_t)num_values, vt,
        &inode->value_mask, inode->values, half_precision, sw->alloc, err);
    if (mst != TVDB_OK) return mst;

    tvdb_status_t st = TVDB_OK;
    size_t child_n = 0;
    for (int32_t i = 0; i < inode->child_mask.bitsize; i++) {
        if (tvdb__nodemask_is_on(&inode->child_mask, i)) {
            st = tvdb__write_node_topology(
                tree, sw, inode->child_indices[child_n], level + 1,
                background, compression_flags, half_precision, err);
            if (st != TVDB_OK) return st;
            child_n++;
        }
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_leaf_topology(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx,
    tvdb_error_t *err) {
    (void)err;
    const tvdb_leaf_node_t *leaf = &tree->nodes[node_idx].u.leaf;
    tvdb__sw_write(sw, leaf->value_mask.bits.data,
                   leaf->value_mask.bits.num_bytes);
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_node_topology(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    tvdb_node_type_t nt = tree->nodes[node_idx].type;
    switch (nt) {
        case TVDB_NODE_ROOT:
            return tvdb__write_root_topology(tree, sw, node_idx, level,
                                             compression_flags,
                                             half_precision, err);
        case TVDB_NODE_INTERNAL:
            return tvdb__write_internal_topology(
                tree, sw, node_idx, level, background,
                compression_flags, half_precision, err);
        case TVDB_NODE_LEAF:
            return tvdb__write_leaf_topology(tree, sw, node_idx, err);
        default:
            return TVDB_ERROR_INVALID_DATA;
    }
}

/* ========================================================================== */
/*  Write tree buffers                                                        */
/* ========================================================================== */

static tvdb_status_t tvdb__write_node_buffer(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err);

static tvdb_status_t tvdb__write_root_buffer(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    const tvdb_root_node_t *root = &tree->nodes[node_idx].u.root;
    for (uint32_t i = 0; i < root->num_children; i++) {
        tvdb_status_t st = tvdb__write_node_buffer(
            tree, sw, root->child_indices[i], level + 1,
            background, compression_flags, half_precision, err);
        if (st != TVDB_OK) return st;
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_internal_buffer(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    const tvdb_internal_node_t *inode = &tree->nodes[node_idx].u.internal;
    size_t child_n = 0;
    for (int32_t i = 0; i < inode->child_mask.bitsize; i++) {
        if (tvdb__nodemask_is_on(&inode->child_mask, i)) {
            tvdb_status_t st = tvdb__write_node_buffer(
                tree, sw, inode->child_indices[child_n], level + 1,
                background, compression_flags, half_precision, err);
            if (st != TVDB_OK) return st;
            child_n++;
        }
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_leaf_buffer(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    const tvdb_leaf_node_t *leaf = &tree->nodes[node_idx].u.leaf;
    tvdb_value_type_t vt = tree->layout.levels[level].value_type;
    size_t num_values = leaf->num_voxels;
    if (!leaf->data ||
        leaf->data_size < num_values * tvdb_value_type_size(vt)) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                        "Leaf value buffer smaller than its voxels");
        return TVDB_ERROR_INVALID_DATA;
    }

    tvdb__sw_write(sw, leaf->value_mask.bits.data,
                   leaf->value_mask.bits.num_bytes);

    /* BOOL leaves: OpenVDB's LeafNode<bool>::writeBuffers writes
       value_mask + mOrigin (12 bytes) + bit-packed data buffer.
       The value mask is written above; emit the origin and the packed
       data here. The origin is recoverable from the tree node's origin
       field; if not populated, write zeros (round-trip through tinyvdb
       still works because we recompute coordinates from tree topology). */
    if (vt == TVDB_VALUE_BOOL) {
        const tvdb_tree_node_t *node = &tree->nodes[node_idx];
        int32_t origin[3] = { node->origin[0], node->origin[1], node->origin[2] };
        tvdb__sw_write_i32(sw, origin[0]);
        tvdb__sw_write_i32(sw, origin[1]);
        tvdb__sw_write_i32(sw, origin[2]);
        size_t packed_bytes = (num_values + 7) / 8;
        uint8_t *packed = (uint8_t *)tvdb__alloc(sw->alloc, packed_bytes);
        if (!packed) {
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        memset(packed, 0, packed_bytes);
        for (size_t s = 0; s < num_values; ++s) {
            if (leaf->data[s]) packed[s >> 3] |= (uint8_t)(1u << (s & 7));
        }
        tvdb__sw_write(sw, packed, packed_bytes);
        tvdb__free(sw->alloc, packed, packed_bytes);
    } else {
    tvdb_status_t st = tvdb__write_mask_values(
        sw, compression_flags, background, num_values, vt,
        &leaf->value_mask, leaf->data, half_precision, sw->alloc, err);
    if (st != TVDB_OK) return st;
    }

    if (tree->is_point_index_grid) {
        int64_t num_indices = (int64_t)leaf->num_point_indices;
        tvdb__sw_write_i64(sw, num_indices);
        if (num_indices > 0) {
            if (!leaf->point_indices) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "PointIndex leaf missing index payload");
                return TVDB_ERROR_INVALID_DATA;
            }
            tvdb__sw_write(sw, (const uint8_t *)leaf->point_indices,
                           (size_t)num_indices * sizeof(int32_t));
        }
        int64_t aux_size = (int64_t)leaf->point_aux_data_size;
        tvdb__sw_write_i64(sw, aux_size);
        if (aux_size > 0) {
            if (!leaf->point_aux_data) {
                tvdb__set_error(err, TVDB_ERROR_INVALID_DATA,
                                "PointIndex leaf missing aux payload");
                return TVDB_ERROR_INVALID_DATA;
            }
            tvdb__sw_write(sw, leaf->point_aux_data, (size_t)aux_size);
        }
    }
    return TVDB_OK;
}

static tvdb_status_t tvdb__write_node_buffer(
    const tvdb_tree_t *tree, tvdb__sw_t *sw, size_t node_idx, int level,
    tvdb_value_t background, uint32_t compression_flags,
    int half_precision, tvdb_error_t *err) {
    tvdb_node_type_t nt = tree->nodes[node_idx].type;
    switch (nt) {
        case TVDB_NODE_ROOT:
            return tvdb__write_root_buffer(tree, sw, node_idx, level,
                                           background, compression_flags,
                                           half_precision, err);
        case TVDB_NODE_INTERNAL:
            return tvdb__write_internal_buffer(tree, sw, node_idx, level,
                                               background, compression_flags,
                                               half_precision, err);
        case TVDB_NODE_LEAF:
            return tvdb__write_leaf_buffer(tree, sw, node_idx, level,
                                           background, compression_flags,
                                           half_precision, err);
        default:
            return TVDB_ERROR_INVALID_DATA;
    }
}

/* ========================================================================== */
/*  Write header, metadata, transform, grid                                   */
/* ========================================================================== */

static void tvdb__write_header(tvdb__sw_t *sw, const tvdb_header_t *h) {
    int64_t magic = TVDB_MAGIC;
    tvdb__sw_write_i64(sw, magic);

    uint32_t file_version = h->file_version;
    if (file_version < TVDB_FILE_VERSION_NODE_MASK_COMPRESSION)
        file_version = TVDB_FILE_VERSION_MULTIPASS_IO; /* default to 224 */
    tvdb__sw_write_u32(sw, file_version);

    /* Library version */
    tvdb__sw_write_u32(sw, h->major_version ? h->major_version : 1);
    tvdb__sw_write_u32(sw, h->minor_version);

    /* Has grid offsets: always yes */
    tvdb__sw_write_u8(sw, 1);

    /* No global compression flag for v222+ */

    /* UUID */
    char uuid[37];
    if (h->uuid[0]) {
        memcpy(uuid, h->uuid, 36);
    } else {
        /* Generate a simple placeholder UUID */
        static const char hex[] = "0123456789abcdef";
        /* Use address and counter for entropy */
        static unsigned int counter = 0;
        uintptr_t addr = (uintptr_t)sw->data;
        counter++;
        unsigned int seed = (unsigned int)(addr ^ (counter * 2654435761u));
        for (int i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) uuid[i] = '-';
            else { uuid[i] = hex[seed & 0xf]; seed = seed * 1103515245 + 12345; }
        }
    }
    uuid[36] = '\0';
    tvdb__sw_write(sw, uuid, 36);
}

/* OpenVDB takes a grid's half-float setting from this metadata entry when
   reading, so it must agree with the "_HalfFloat" grid type suffix. */
#define TVDB__META_SAVE_HALF "is_saved_as_half_float"

/* save_half < 0 writes the metadata unchanged (file metadata); otherwise the
   grid's "is_saved_as_half_float" entry is forced to save_half, and added
   when a half grid lacks it. */
static void tvdb__write_meta(tvdb__sw_t *sw, const tvdb_metadata_t *meta,
                             int save_half) {
    int32_t count = meta ? (int32_t)meta->count : 0;
    int has_half_entry = 0;
    for (int32_t i = 0; i < count; i++) {
        const tvdb_meta_entry_t *e = &meta->entries[i];
        if (e->name && strcmp(e->name, TVDB__META_SAVE_HALF) == 0)
            has_half_entry = 1;
    }
    int add_half_entry = save_half > 0 && !has_half_entry;
    tvdb__sw_write_i32(sw, count + (add_half_entry ? 1 : 0));
    if (add_half_entry) {
        tvdb__sw_write_string(sw, TVDB__META_SAVE_HALF);
        tvdb__sw_write_string(sw, "bool");
        tvdb__sw_write_u32(sw, 1);
        tvdb__sw_write_u8(sw, 1);
    }
    for (int32_t i = 0; i < count; i++) {
        const tvdb_meta_entry_t *e = &meta->entries[i];
        if (save_half >= 0 && e->name &&
            strcmp(e->name, TVDB__META_SAVE_HALF) == 0) {
            tvdb__sw_write_string(sw, e->name);
            tvdb__sw_write_string(sw, "bool");
            tvdb__sw_write_u32(sw, 1);
            tvdb__sw_write_u8(sw, (uint8_t)(save_half ? 1 : 0));
            continue;
        }
        tvdb__sw_write_string(sw, e->name);
        tvdb__sw_write_string(sw, e->type_name);

        if (strcmp(e->type_name, "string") == 0) {
            tvdb__sw_write_string(sw, e->value.u.s.str);
        } else if (strcmp(e->type_name, "bool") == 0) {
            tvdb__sw_write_u32(sw, 1);
            tvdb__sw_write_u8(sw, (uint8_t)e->value.u.b);
        } else if (strcmp(e->type_name, "float") == 0) {
            tvdb__sw_write_u32(sw, sizeof(float));
            tvdb__sw_write_f32(sw, e->value.u.f);
        } else if (strcmp(e->type_name, "double") == 0) {
            tvdb__sw_write_u32(sw, sizeof(double));
            tvdb__sw_write_f64(sw, e->value.u.d);
        } else if (strcmp(e->type_name, "int32") == 0) {
            tvdb__sw_write_u32(sw, sizeof(int32_t));
            tvdb__sw_write_i32(sw, e->value.u.i32);
        } else if (strcmp(e->type_name, "int64") == 0) {
            tvdb__sw_write_u32(sw, sizeof(int64_t));
            tvdb__sw_write_i64(sw, e->value.u.i64);
        } else if (strcmp(e->type_name, "vec3i") == 0) {
            tvdb__sw_write_u32(sw, 3 * sizeof(int32_t));
            tvdb__sw_write_i32(sw, e->value.u.vec3i[0]);
            tvdb__sw_write_i32(sw, e->value.u.vec3i[1]);
            tvdb__sw_write_i32(sw, e->value.u.vec3i[2]);
        } else if (strcmp(e->type_name, "vec3d") == 0) {
            tvdb__sw_write_u32(sw, 3 * sizeof(double));
            tvdb__sw_write_f64(sw, e->value.u.vec3d[0]);
            tvdb__sw_write_f64(sw, e->value.u.vec3d[1]);
            tvdb__sw_write_f64(sw, e->value.u.vec3d[2]);
        } else if (e->raw_data) {
            tvdb__sw_write_u32(sw, (uint32_t)e->raw_data_len);
            tvdb__sw_write(sw, e->raw_data, e->raw_data_len);
        } else {
            tvdb__sw_write_u32(sw, 0);
        }
    }
}

static void tvdb__write_transform(tvdb__sw_t *sw, const tvdb_transform_t *x) {
    double inv[3], inv_sq[3], inv_twice[3];
    for (int i = 0; i < 3; i++) {
        double s = x->scale_values[i] != 0.0 ? x->scale_values[i] : 1.0;
        inv[i] = 1.0 / s;
        inv_sq[i] = inv[i] * inv[i];
        inv_twice[i] = 0.5 / s;
    }

    switch (x->type) {
        case TVDB_TRANSFORM_UNIFORM_SCALE:
            tvdb__sw_write_string(sw, "UniformScaleMap");
            tvdb__sw_write_vec3d(sw, x->scale_values);
            tvdb__sw_write_vec3d(sw, x->voxel_size);
            tvdb__sw_write_vec3d(sw, inv);
            tvdb__sw_write_vec3d(sw, inv_sq);
            tvdb__sw_write_vec3d(sw, inv_twice);
            break;
        case TVDB_TRANSFORM_SCALE:
            tvdb__sw_write_string(sw, "ScaleMap");
            tvdb__sw_write_vec3d(sw, x->scale_values);
            tvdb__sw_write_vec3d(sw, x->voxel_size);
            tvdb__sw_write_vec3d(sw, inv);
            tvdb__sw_write_vec3d(sw, inv_sq);
            tvdb__sw_write_vec3d(sw, inv_twice);
            break;
        case TVDB_TRANSFORM_UNIFORM_SCALE_TRANSLATE:
            tvdb__sw_write_string(sw, "UniformScaleTranslateMap");
            tvdb__sw_write_vec3d(sw, x->translation);
            tvdb__sw_write_vec3d(sw, x->scale_values);
            tvdb__sw_write_vec3d(sw, x->voxel_size);
            tvdb__sw_write_vec3d(sw, inv);
            tvdb__sw_write_vec3d(sw, inv_sq);
            tvdb__sw_write_vec3d(sw, inv_twice);
            break;
        case TVDB_TRANSFORM_SCALE_TRANSLATE:
            tvdb__sw_write_string(sw, "ScaleTranslateMap");
            tvdb__sw_write_vec3d(sw, x->translation);
            tvdb__sw_write_vec3d(sw, x->scale_values);
            tvdb__sw_write_vec3d(sw, x->voxel_size);
            tvdb__sw_write_vec3d(sw, inv);
            tvdb__sw_write_vec3d(sw, inv_sq);
            tvdb__sw_write_vec3d(sw, inv_twice);
            break;
        case TVDB_TRANSFORM_TRANSLATION:
            tvdb__sw_write_string(sw, "TranslationMap");
            tvdb__sw_write_vec3d(sw, x->translation);
            break;
        case TVDB_TRANSFORM_AFFINE:
        default:
            tvdb__sw_write_string(sw, "AffineMap");
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    tvdb__sw_write_f64(sw, x->matrix[r][c]);
            break;
    }
}

static tvdb_status_t tvdb__write_grid(
    tvdb__sw_t *sw, const tvdb_grid_t *grid, const tvdb_header_t *header,
    uint32_t compression_flags, tvdb_error_t *err) {
    (void)header;

    /* Grid compression (v222+) */
    tvdb__sw_write_u32(sw, compression_flags);

    /* Grid metadata */
    tvdb__write_meta(sw, &grid->metadata,
                     grid->descriptor.save_float_as_half ? 1 : 0);

    /* Transform */
    tvdb__write_transform(sw, &grid->transform);

    if (grid->tree.is_point_data_grid ||
        (grid->descriptor.grid_type &&
         strstr(grid->descriptor.grid_type, "ptdataidx") != NULL)) {
        if (!grid->point_data_blob || grid->point_data_blob_size == 0) {
            tvdb__set_error(err, TVDB_ERROR_UNIMPLEMENTED,
                            "PointDataGrid write requires point_data_blob payload");
            return TVDB_ERROR_UNIMPLEMENTED;
        }
        tvdb__sw_write(sw, grid->point_data_blob, grid->point_data_blob_size);
        return TVDB_OK;
    }

    /* Instance grids share another grid's tree — no tree to write */
    if (grid->descriptor.instance_parent_name &&
        grid->descriptor.instance_parent_name[0] != '\0') {
        return TVDB_OK;
    }

    /* Tree */
    if (grid->tree.num_nodes == 0) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_DATA, "Grid has no tree nodes");
        return TVDB_ERROR_INVALID_DATA;
    }

    /* TreeBase: buffer count (always 1) */
    tvdb__sw_write_i32(sw, 1);

    tvdb_value_t background = grid->tree.nodes[0].u.root.background;

    int half = grid->descriptor.save_float_as_half;

    /* Write topology */
    tvdb_status_t st = tvdb__write_node_topology(
        &grid->tree, sw, 0, 0, background, compression_flags, half, err);
    if (st != TVDB_OK) return st;

    /* Write buffers */
    st = tvdb__write_node_buffer(
        &grid->tree, sw, 0, 0, background, compression_flags, half, err);
    return st;
}

/* ========================================================================== */
/*  Public write API                                                          */
/* ========================================================================== */

void tvdb_file_init_for_write(tvdb_file_t *file, const tvdb_allocator_t *alloc) {
    if (!file) return;
    memset(file, 0, sizeof(*file));
    file->header.file_version = 224;
    file->header.major_version = 10;
    file->header.minor_version = 0;
    file->header.has_grid_offsets = 1;
    memcpy(file->header.uuid, "00000000-0000-0000-0000-000000000000", 37);
    file->alloc = alloc ? *alloc : tvdb__default_allocator();
}

tvdb_status_t tvdb_write_to_memory(const tvdb_file_t *file,
                                   uint32_t compression_flags,
                                   int compression_level,
                                   uint8_t **out_data, size_t *out_size,
                                   tvdb_error_t *err) {
    if (!file || !out_data || !out_size) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL argument");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    *out_data = NULL;
    *out_size = 0;

    tvdb_allocator_t alloc = file->alloc;
    tvdb__sw_t sw;
    tvdb__sw_init(&sw, &alloc);
    sw.compression_level = (compression_level > 0) ? compression_level : 5;

    /* Write header */
    tvdb__write_header(&sw, &file->header);

    /* File metadata */
    tvdb__write_meta(&sw, &file->file_metadata, -1);

    /* Grid count */
    tvdb__sw_write_i32(&sw, (int32_t)file->num_grids);

    /* Write each grid as a DESCRIPTOR immediately followed by its GRID DATA.
       OpenVDB (and tinyvdb's own reader) interleave descriptors with data:
       [desc0][grid0][desc1][grid1][...]. Emitting all descriptors up front
       instead produces a file that neither tinyvdb nor openvdb can re-read
       for multi-grid archives. */
    for (size_t i = 0; i < file->num_grids; i++) {
        const tvdb_grid_descriptor_t *gd = &file->grids[i].descriptor;

        tvdb__sw_write_string(&sw, gd->unique_name ? gd->unique_name
                                                    : gd->grid_name);

        /* Grid type with optional _HalfFloat suffix */
        if (gd->save_float_as_half && gd->grid_type) {
            size_t tlen = strlen(gd->grid_type);
            char *full_type = (char *)tvdb__alloc(&alloc, tlen + 11);
            if (!full_type) {
                tvdb__sw_destroy(&sw);
                tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
                return TVDB_ERROR_OUT_OF_MEMORY;
            }
            memcpy(full_type, gd->grid_type, tlen);
            memcpy(full_type + tlen, "_HalfFloat", 11);
            tvdb__sw_write_string(&sw, full_type);
            tvdb__free(&alloc, full_type, tlen + 11);
        } else {
            tvdb__sw_write_string(&sw, gd->grid_type);
        }

        tvdb__sw_write_string(&sw, gd->instance_parent_name
                                     ? gd->instance_parent_name : "");

        /* Record position of offset triple for backpatching */
        size_t offsets_pos = sw.len;
        tvdb__sw_write_u64(&sw, 0); /* grid_byte_offset placeholder */
        tvdb__sw_write_u64(&sw, 0); /* block_byte_offset placeholder */
        tvdb__sw_write_u64(&sw, 0); /* end_byte_offset placeholder */

        size_t grid_pos = sw.len;

        tvdb_status_t st = tvdb__write_grid(
            &sw, &file->grids[i], &file->header, compression_flags, err);
        if (st != TVDB_OK) {
            tvdb__sw_destroy(&sw);
            return st;
        }

        size_t end_pos = sw.len;

        tvdb__sw_patch_u64(&sw, offsets_pos + 0, (uint64_t)grid_pos);
        tvdb__sw_patch_u64(&sw, offsets_pos + 8, (uint64_t)grid_pos);
        tvdb__sw_patch_u64(&sw, offsets_pos + 16, (uint64_t)end_pos);
    }

    /* Any write that could not grow the buffer leaves a truncated stream. */
    if (sw.failed || sw.len == 0) {
        tvdb__sw_destroy(&sw);
        tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY,
                        "Out of memory while writing VDB stream");
        return TVDB_ERROR_OUT_OF_MEMORY;
    }

    /* The caller frees *out_data with *out_size, so hand back an allocation
       of exactly that size. */
    if (sw.cap != sw.len) {
        uint8_t *exact = (uint8_t *)tvdb__realloc(&alloc, sw.data, sw.cap,
                                                  sw.len);
        if (!exact) {
            tvdb__sw_destroy(&sw);
            tvdb__set_error(err, TVDB_ERROR_OUT_OF_MEMORY, "OOM");
            return TVDB_ERROR_OUT_OF_MEMORY;
        }
        sw.data = exact;
        sw.cap = sw.len;
    }

    /* Transfer ownership of buffer to caller */
    *out_data = sw.data;
    *out_size = sw.len;
    sw.data = NULL; /* prevent sw_destroy from freeing */
    sw.len = sw.cap = 0;
    tvdb__sw_destroy(&sw);

    return TVDB_OK;
}

tvdb_status_t tvdb_file_save(const tvdb_file_t *file,
                             const char *filepath_utf8,
                             uint32_t compression_flags,
                             int compression_level,
                             int use_mmap,
                             tvdb_error_t *err) {
    if (!file || !filepath_utf8) {
        tvdb__set_error(err, TVDB_ERROR_INVALID_ARGUMENT, "NULL argument");
        return TVDB_ERROR_INVALID_ARGUMENT;
    }

    /* Write to memory first */
    uint8_t *data = NULL;
    size_t data_size = 0;
    tvdb_status_t st = tvdb_write_to_memory(file, compression_flags,
                                            compression_level,
                                            &data, &data_size, err);
    if (st != TVDB_OK) return st;

    tvdb_allocator_t alloc = file->alloc;

#if !defined(TVDB_NO_MMAP) && !defined(_WIN32)
    if (use_mmap) {
        int fd = open(filepath_utf8, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Failed to create file");
            return TVDB_ERROR_IO;
        }
        if (ftruncate(fd, (off_t)data_size) < 0) {
            close(fd);
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "ftruncate failed");
            return TVDB_ERROR_IO;
        }
        void *mapped = mmap(NULL, data_size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            close(fd);
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED, "mmap write failed");
            return TVDB_ERROR_MMAP_FAILED;
        }
        memcpy(mapped, data, data_size);
        munmap(mapped, data_size);
        close(fd);
        tvdb__free(&alloc, data, data_size);
        return TVDB_OK;
    }
#endif

#if !defined(TVDB_NO_MMAP) && defined(_WIN32)
    if (use_mmap) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, NULL, 0);
        wchar_t *wpath = NULL;
        if (wlen > 0) {
            wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
            if (wpath)
                MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, wpath, wlen);
        }
        HANDLE hFile = CreateFileW(
            wpath ? wpath : L"", GENERIC_READ | GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wpath);
        if (hFile == INVALID_HANDLE_VALUE) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Failed to create file");
            return TVDB_ERROR_IO;
        }
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)data_size;
        HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READWRITE,
                                         li.HighPart, li.LowPart, NULL);
        if (!hMap) {
            CloseHandle(hFile);
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED,
                            "CreateFileMapping write failed");
            return TVDB_ERROR_MMAP_FAILED;
        }
        void *mapped = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
        if (!mapped) {
            CloseHandle(hMap);
            CloseHandle(hFile);
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_MMAP_FAILED,
                            "MapViewOfFile write failed");
            return TVDB_ERROR_MMAP_FAILED;
        }
        memcpy(mapped, data, data_size);
        UnmapViewOfFile(mapped);
        CloseHandle(hMap);
        CloseHandle(hFile);
        tvdb__free(&alloc, data, data_size);
        return TVDB_OK;
    }
#endif

    /* Standard file I/O fallback */
    (void)use_mmap;
#if defined(_WIN32)
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, NULL, 0);
        wchar_t *wpath = NULL;
        if (wlen > 0) {
            wpath = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wlen);
            if (wpath)
                MultiByteToWideChar(CP_UTF8, 0, filepath_utf8, -1, wpath, wlen);
        }
        HANDLE hFile = CreateFileW(
            wpath ? wpath : L"", GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(wpath);
        if (hFile == INVALID_HANDLE_VALUE) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Failed to create file");
            return TVDB_ERROR_IO;
        }
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, data, (DWORD)data_size, &written, NULL);
        CloseHandle(hFile);
        if (!ok || (size_t)written != data_size) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Write incomplete");
            return TVDB_ERROR_IO;
        }
    }
#else
    {
        FILE *fp = fopen(filepath_utf8, "wb");
        if (!fp) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Failed to create file");
            return TVDB_ERROR_IO;
        }
        size_t nw = fwrite(data, 1, data_size, fp);
        fclose(fp);
        if (nw != data_size) {
            tvdb__free(&alloc, data, data_size);
            tvdb__set_error(err, TVDB_ERROR_IO, "Write incomplete");
            return TVDB_ERROR_IO;
        }
    }
#endif

    tvdb__free(&alloc, data, data_size);
    return TVDB_OK;
}

#endif /* TINYVDB_IO_IMPLEMENTATION */

#endif /* TINYVDB_IO_H_ */
