#pragma once
/* Float NanoVDB buffer sampler for GPU kernels, and the host-side reference
 * that tests it.
 *
 * The buffer is the layout tinyvdb's writer (tinyvdb_to_nanovdb.c) emits and
 * NanoVDB itself uses: GridData (672 bytes) -> TreeData (64) -> RootData ->
 * Upper/Lower internal nodes -> Leaf, little endian. Internal-node table slots
 * are 8-byte unions holding either the child offset (int64, relative to the
 * node) or the tile value (float at the slot's first four bytes); slot indices
 * are z-fastest.
 *
 * This header has no includes and uses only builtin types on purpose: the same
 * text is compiled by the host, by NVRTC and by hiprtc as part of the GPU
 * compute module's device source, so it cannot depend on libc, stdint or
 * size_t.
 */

#define TVDB_NVDB_GRID_BYTES 672u
#define TVDB_NVDB_TREE_ROOT_OFF 24u
#define TVDB_NVDB_ROOT_BG_OFF 28u
#define TVDB_NVDB_ROOT_SIZE 64u
#define TVDB_NVDB_ROOT_TILE_SIZE 32u
#define TVDB_NVDB_ROOT_TILE_VALUE_OFF 20u
#define TVDB_NVDB_ROOT_TILE_CHILD_OFF 8u
#define TVDB_NVDB_UP_VALUE_MASK_OFF 32u
#define TVDB_NVDB_UP_CHILD_MASK_OFF 4128u
#define TVDB_NVDB_UP_TABLE_OFF 8256u
#define TVDB_NVDB_LO_VALUE_MASK_OFF 32u
#define TVDB_NVDB_LO_CHILD_MASK_OFF 544u
#define TVDB_NVDB_LO_TABLE_OFF 1088u
#define TVDB_NVDB_LEAF_VALUE_MASK_OFF 16u
#define TVDB_NVDB_LEAF_TABLE_OFF 96u
#define TVDB_NVDB_TABLE_STRIDE 8u   /* internal nodes: int64 child / float tile */
#define TVDB_NVDB_LEAF_STRIDE 4u

/* Kernels call these, so they must be device-visible under NVRTC/hiprtc; the
 * host compiles the very same functions for the CPU reference test. */
#if defined(__CUDACC__) || defined(__HIPCC__) || defined(__HIP_DEVICE_COMPILE__)
#define TVDB_NVDB_INLINE __host__ __device__ inline
#else
#define TVDB_NVDB_INLINE static inline
#endif

typedef unsigned long long tvdb_nvdb_u64;

typedef struct {
  const unsigned char* base;
  tvdb_nvdb_u64 size; /* bytes */
} tvdb_nvdb_grid;

/* Little-endian reads assembled from bytes: no alignment requirement, which
 * matters for the 3D texture-style copies some callers keep. */
TVDB_NVDB_INLINE unsigned int tvdb_nvdb_u32(const unsigned char* p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
         ((unsigned int)p[3] << 24);
}
TVDB_NVDB_INLINE tvdb_nvdb_u64 tvdb_nvdb_u64_at(const unsigned char* p) {
  return (tvdb_nvdb_u64)tvdb_nvdb_u32(p) | ((tvdb_nvdb_u64)tvdb_nvdb_u32(p + 4) << 32);
}
TVDB_NVDB_INLINE long long tvdb_nvdb_i64(const unsigned char* p) {
  return (long long)tvdb_nvdb_u64_at(p);
}
TVDB_NVDB_INLINE float tvdb_nvdb_f32(const unsigned char* p) {
  union {
    unsigned int u;
    float f;
  } c;
  c.u = tvdb_nvdb_u32(p);
  return c.f;
}

/* Slot index of an index-space coordinate inside a node whose children span
 * 2^(12 - log2dim) voxels (5-4-3: upper log2dim 5, lower 4, leaf 3). Nodes are
 * aligned to their span and carry their *active* box in bbox_min/max, so the
 * slot comes from the coordinate's own low bits, z fastest (NanoVDB order). */
TVDB_NVDB_INLINE unsigned int tvdb_nvdb_slot(int x, int y, int z, int log2dim) {
  /* 5-4-3: upper children span 128 voxels (mask 4095, shift 7), lower 8
   * (mask 127, shift 3), leaf 1 (mask 7, shift 0). */
  const unsigned int mask = log2dim == 5 ? 4095u : log2dim == 4 ? 127u : 7u;
  const int sh = log2dim == 5 ? 7 : log2dim == 4 ? 3 : 0;
  const unsigned int ix = ((unsigned int)x & mask) >> sh;
  const unsigned int iy = ((unsigned int)y & mask) >> sh;
  const unsigned int iz = ((unsigned int)z & mask) >> sh;
  return (ix << (2 * log2dim)) | (iy << log2dim) | iz;
}
TVDB_NVDB_INLINE int tvdb_nvdb_mask_bit(const unsigned char* mask, unsigned int bit) {
  return (mask[bit >> 3] >> (bit & 7u)) & 1u;
}
/* NanoVDB root bbox key: 21 bits per axis, z lowest. */
TVDB_NVDB_INLINE tvdb_nvdb_u64 tvdb_nvdb_key(int x, int y, int z) {
  return ((tvdb_nvdb_u64)((unsigned int)x >> 12) << 42) |
         ((tvdb_nvdb_u64)((unsigned int)y >> 12) << 21) | (tvdb_nvdb_u64)((unsigned int)z >> 12);
}
TVDB_NVDB_INLINE int tvdb_nvdb_has(const tvdb_nvdb_grid* g, tvdb_nvdb_u64 off, tvdb_nvdb_u64 n) {
  return (g->size >= off && g->size - off >= n) ? 1 : 0;
}

/* The stored float at index space (x,y,z): a leaf voxel, an internal-node tile
 * value, a root tile value or the root background. Mirrors tvdb_tree_get. */
TVDB_NVDB_INLINE float tvdb_nvdb_get(const tvdb_nvdb_grid* g, int x, int y, int z) {
  const unsigned char* b = g->base;
  if (!tvdb_nvdb_has(g, 0, TVDB_NVDB_GRID_BYTES + TVDB_NVDB_TREE_ROOT_OFF + 8u)) return 0.0f;
  const tvdb_nvdb_u64 root =
      TVDB_NVDB_GRID_BYTES + tvdb_nvdb_u64_at(b + TVDB_NVDB_GRID_BYTES + TVDB_NVDB_TREE_ROOT_OFF);
  if (!tvdb_nvdb_has(g, root, TVDB_NVDB_ROOT_SIZE)) return 0.0f;
  const float background = tvdb_nvdb_f32(b + root + TVDB_NVDB_ROOT_BG_OFF);
  const unsigned int tiles = tvdb_nvdb_u32(b + root + 24u);
  const tvdb_nvdb_u64 key = tvdb_nvdb_key(x, y, z);
  tvdb_nvdb_u64 tile = 0;
  for (unsigned int i = 0; i < tiles; ++i) {
    const tvdb_nvdb_u64 at = root + TVDB_NVDB_ROOT_SIZE + (tvdb_nvdb_u64)i * TVDB_NVDB_ROOT_TILE_SIZE;
    if (!tvdb_nvdb_has(g, at, TVDB_NVDB_ROOT_TILE_SIZE)) return background;
    if (tvdb_nvdb_u64_at(b + at) == key) {
      tile = at;
      break;
    }
  }
  if (!tile) return background;
  const long long root_child = tvdb_nvdb_i64(b + tile + TVDB_NVDB_ROOT_TILE_CHILD_OFF);
  if (root_child == 0) return tvdb_nvdb_f32(b + tile + TVDB_NVDB_ROOT_TILE_VALUE_OFF);

  /* Upper internal node: slots span 32 voxels per axis, table at 8256. */
  tvdb_nvdb_u64 node = root + (tvdb_nvdb_u64)root_child;
  if (!tvdb_nvdb_has(g, node, TVDB_NVDB_UP_TABLE_OFF + TVDB_NVDB_TABLE_STRIDE)) return background;
  unsigned int s = tvdb_nvdb_slot(x, y, z, 5);
  if (!tvdb_nvdb_mask_bit(b + node + TVDB_NVDB_UP_CHILD_MASK_OFF, s))
    return tvdb_nvdb_f32(b + node + TVDB_NVDB_UP_TABLE_OFF + (tvdb_nvdb_u64)s * TVDB_NVDB_TABLE_STRIDE);
  node += (tvdb_nvdb_u64)tvdb_nvdb_i64(b + node + TVDB_NVDB_UP_TABLE_OFF +
                                       (tvdb_nvdb_u64)s * TVDB_NVDB_TABLE_STRIDE);

  /* Lower internal node: slots span 8 voxels per axis, table at 1088. */
  if (!tvdb_nvdb_has(g, node, TVDB_NVDB_LO_TABLE_OFF + TVDB_NVDB_TABLE_STRIDE)) return background;
  s = tvdb_nvdb_slot(x, y, z, 4);
  if (!tvdb_nvdb_mask_bit(b + node + TVDB_NVDB_LO_CHILD_MASK_OFF, s))
    return tvdb_nvdb_f32(b + node + TVDB_NVDB_LO_TABLE_OFF + (tvdb_nvdb_u64)s * TVDB_NVDB_TABLE_STRIDE);
  node += (tvdb_nvdb_u64)tvdb_nvdb_i64(b + node + TVDB_NVDB_LO_TABLE_OFF +
                                       (tvdb_nvdb_u64)s * TVDB_NVDB_TABLE_STRIDE);

  /* Leaf: one value per voxel, table at 96. */
  if (!tvdb_nvdb_has(g, node, TVDB_NVDB_LEAF_TABLE_OFF + TVDB_NVDB_LEAF_STRIDE)) return background;
  s = tvdb_nvdb_slot(x, y, z, 3);
  return tvdb_nvdb_f32(b + node + TVDB_NVDB_LEAF_TABLE_OFF + (tvdb_nvdb_u64)s * TVDB_NVDB_LEAF_STRIDE);
}
