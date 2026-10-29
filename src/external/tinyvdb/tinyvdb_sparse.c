#include "tinyvdb_sparse.h"
#include "tinyvdb_checked.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void tvdb_sparse_grid_init(tvdb_sparse_grid* sg) {
  sg->coords = NULL;
  sg->values = NULL;
  sg->count = 0;
  sg->capacity = 0;
  sg->voxel_size = 1.0f;
  sg->ox = sg->oy = sg->oz = 0.0f;
}

void tvdb_sparse_grid_free(tvdb_sparse_grid* sg) {
  // capacity==0 with non-NULL coords is a non-owning view (e.g. tvdb_grid_batch_view):
  // its arrays point into another allocation, so reset the handle without freeing.
  if (sg->capacity > 0) {
    free(sg->coords);
    free(sg->values);
  }
  sg->coords = NULL;
  sg->values = NULL;
  sg->count = 0;
  sg->capacity = 0;
}

bool tvdb_sparse_grid_reserve(tvdb_sparse_grid* sg, size_t capacity) {
  if (!sg) return false;
  if (capacity <= sg->capacity) return true;
  if (sg->capacity == 0 && (sg->coords || sg->values)) return false;
  if (sg->count > sg->capacity || (sg->count && (!sg->coords || !sg->values))) return false;
  size_t coord_bytes, value_bytes;
  if (!tvdb_size_mul(capacity, sizeof(tvdb_vec3i), &coord_bytes) ||
      !tvdb_size_mul(capacity, sizeof(float), &value_bytes)) return false;
  tvdb_vec3i *nc = (tvdb_vec3i*)malloc(coord_bytes);
  float *nv = (float*)malloc(value_bytes);
  if (!nc || !nv) { free(nc); free(nv); return false; }
  if (sg->count) {
    memcpy(nc, sg->coords, sg->count * sizeof(*nc));
    memcpy(nv, sg->values, sg->count * sizeof(*nv));
  }
  free(sg->coords); free(sg->values);
  sg->coords = nc; sg->values = nv; sg->capacity = capacity;
  return true;
}


static bool tvdb_sparse_push(tvdb_sparse_grid* sg, int x, int y, int z, float v) {
  if (sg->count == sg->capacity) {
    if (sg->capacity > SIZE_MAX / 2) return false;
    size_t cap = sg->capacity ? sg->capacity * 2 : 256;
    if (!tvdb_sparse_grid_reserve(sg, cap)) return false;
  }
  sg->coords[sg->count].x = x;
  sg->coords[sg->count].y = y;
  sg->coords[sg->count].z = z;
  sg->values[sg->count] = v;
  ++sg->count;
  return true;
}

static bool tvdb_dense_to_sparse_impl(const tvdb_dense_grid* dense,
                          float background,
                          float tolerance,
                          tvdb_sparse_grid* out) {
  size_t bytes;
  if (!dense || !out || !tvdb_grid_valid(dense->nx,dense->ny,dense->nz,dense->voxel_size,dense->data,sizeof(float)) ||
      !isfinite(tolerance) || tolerance < 0 || !isfinite(background) ||
      !tvdb_grid_bytes(dense->nx,dense->ny,dense->nz,sizeof(float),&bytes)) return false;
  out->count = 0;
  out->voxel_size = dense->voxel_size;
  out->ox = dense->ox; out->oy = dense->oy; out->oz = dense->oz;

  const int nx = dense->nx, ny = dense->ny, nz = dense->nz;
  for (int iz = 0; iz < nz; ++iz) {
    for (int iy = 0; iy < ny; ++iy) {
      for (int ix = 0; ix < nx; ++ix) {
        float v = dense->data[(((size_t)iz * ny + iy) * nx + ix)];
        if (fabsf(v - background) > tolerance) {
          if (!tvdb_sparse_push(out, ix, iy, iz, v)) return false;
        }
      }
    }
  }
  return true;
}

bool tvdb_active_grid_coords(const tvdb_dense_grid* dense,
                             float background,
                             tvdb_sparse_grid* out) {
  return tvdb_dense_to_sparse(dense, background, 0.0f, out);
}

bool tvdb_sparse_to_dense(const tvdb_sparse_grid* sparse,
                          float background,
                          tvdb_dense_grid* out) {
  size_t bytes;
  if (!sparse || !out || !tvdb_grid_valid(out->nx,out->ny,out->nz,out->voxel_size,out->data,sizeof(float)) ||
      sparse->count > INT_MAX || !isfinite(background) ||
      (sparse->count && (!sparse->coords || !sparse->values)) ||
      !tvdb_grid_bytes(out->nx,out->ny,out->nz,sizeof(float),&bytes)) return false;
  size_t n = bytes / sizeof(float);
  /* No full-size temporary. The grid used to be built in a scratch buffer and
     memcpy'd in, to keep the failure contract that `out` is untouched when this
     returns false. Nothing below can fail -- an out-of-range coordinate is skipped,
     not an error -- and the only allocation is gone, so the contract holds without
     a second 4N-byte buffer. Duplicate coordinates keep the previous
     first-in-input-wins order because the scatter below is unchanged. */
  #pragma omp parallel for schedule(static)
  for (long long ii = 0; ii < (long long)n; ++ii)
    out->data[ii] = background;
  /* Map sparse -> dense; assumes sparse.coords are in the same voxel-index
     frame as the dense grid (caller is responsible for any origin offsets). */
  for (size_t k = sparse->count; k-- > 0;) {
    int x = sparse->coords[k].x;
    int y = sparse->coords[k].y;
    int z = sparse->coords[k].z;
    if (x < 0 || y < 0 || z < 0) continue;
    if (x >= out->nx || y >= out->ny || z >= out->nz) continue;
    out->data[(((size_t)z * out->ny + y) * out->nx + x)] = sparse->values[k];
  }
  return true;
}

// -------------------------------------------------------------------------
// Hashed lookup over sparse coords (linear probing). Used by CSG / morphology.
// -------------------------------------------------------------------------

typedef struct {
  uint64_t key;
  uint32_t idx_plus_one;  // 0 = empty
} tvdb_hash_entry;


static uint64_t tvdb_pack_ijk(int x, int y, int z) {
  // Treat as 21-bit signed-shifted unsigned. Sufficient for grids up to
  // +-1M voxels per axis; see the note above the function on what happens
  // beyond that.
  uint64_t ux = (uint64_t)((int64_t)x + (1LL << 20)) & ((1ULL << 21) - 1);
  uint64_t uy = (uint64_t)((int64_t)y + (1LL << 20)) & ((1ULL << 21) - 1);
  uint64_t uz = (uint64_t)((int64_t)z + (1LL << 20)) & ((1ULL << 21) - 1);
  return (ux << 42) | (uy << 21) | uz;
}

/* Note on the 21-bit packing below: coordinates outside +-2^20 share a packed key.
   That is a hash-quality issue, not a correctness one -- every probe in the
   library (tvdb_hash_get, leaf_hash_lookup, tvdb_index_map_probe and the GLSL and
   CUDA tvdb_map_probe) compares all three full int32 coordinates, so two voxels
   with the same packed key land in different buckets of the same chain and are
   still told apart. The tests pin this: test_hardening drives coords at 2^21 and
   INT32_MAX and expects them resolved correctly. Widening the key would only
   matter if some future path trusted the packed key alone. */

static uint64_t tvdb_mix64(uint64_t k) {
  k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33; return k;
}

static bool tvdb_coord_equal(tvdb_vec3i coord, int x, int y, int z) {
  return coord.x == x && coord.y == y && coord.z == z;
}

/* `first_out`, when non-NULL, receives a bit per input entry: set when that entry
 * is the first occurrence of its coordinate, which is exactly the test the build
 * loop already makes. Callers that walk the input and must skip duplicates were
 * re-probing the table once per entry to rediscover this, which is a random
 * access per voxel for information the builder already had. K bits costs
 * count/8 bytes against the 32 bytes/entry the table itself occupies.
 * The bitmap is calloc'd here and must be freed by the caller. */
static bool tvdb_hash_build(const tvdb_sparse_grid* g, tvdb_hash_entry** table_out, size_t* mask_out,
                            uint8_t** first_out) {
  size_t cap;
  if (g->count > INT_MAX || (g->count && (!g->coords || !g->values)) ||
      !tvdb_hash_capacity_scaled(g->count, 4, 3, &cap)) return false;
  tvdb_hash_entry* tbl = (tvdb_hash_entry*)calloc(cap, sizeof(tvdb_hash_entry));
  if (!tbl) return false;
  uint8_t* first = NULL;
  if (first_out) {
    *first_out = NULL;
    first = (uint8_t*)calloc((g->count + 7u) / 8u, 1u);
    if (!first) { free(tbl); return false; }
  }
  size_t mask = cap - 1;
  for (size_t i = 0; i < g->count; ++i) {
    uint64_t key = tvdb_pack_ijk(g->coords[i].x, g->coords[i].y, g->coords[i].z);
    size_t h = (size_t)(tvdb_mix64(key) & mask);
    while (tbl[h].idx_plus_one) {
      if (tbl[h].key == key && tvdb_coord_equal(g->coords[tbl[h].idx_plus_one - 1],
          g->coords[i].x, g->coords[i].y, g->coords[i].z)) break;
      h = (h + 1) & mask;
    }
    /* First occurrence wins, which is what the `break` above is reaching for:
     * the active set is sorted, so a repeated coordinate means the later voxel is
     * a duplicate and must not displace the one already indexed. Assigning
     * unconditionally here made it last-wins instead, which disagreed with
     * every GPU lookup -- both the brute-force scan (first match in index order)
     * and the host-built ijk map (which only ever fills an empty slot). The
     * disagreement was invisible until the map paths were compared against this
     * on input containing duplicates. */
    if (tbl[h].idx_plus_one) continue;
    tbl[h].key = key;
    tbl[h].idx_plus_one = (uint32_t)(i + 1);
    if (first) first[i >> 3] |= (uint8_t)(1u << (i & 7u));
  }
  *table_out = tbl; *mask_out = mask;
  if (first_out) *first_out = first;
  return true;
}

static int tvdb_hash_get(const tvdb_hash_entry* tbl, size_t mask,
                         const tvdb_vec3i* coords,
                         int x, int y, int z) {
  uint64_t key = tvdb_pack_ijk(x, y, z);
  size_t h = (size_t)(tvdb_mix64(key) & mask);
  while (tbl[h].idx_plus_one) {
    if (tbl[h].key == key && tvdb_coord_equal(coords[tbl[h].idx_plus_one - 1], x, y, z)) return (int)(tbl[h].idx_plus_one - 1);
    h = (h + 1) & mask;
  }
  return -1;
}

// -------------------------------------------------------------------------
// CSG
// -------------------------------------------------------------------------

static bool tvdb_check_same_frame(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b) {
  return fabsf(a->voxel_size - b->voxel_size) < 1e-6f
      && fabsf(a->ox - b->ox) < 1e-6f
      && fabsf(a->oy - b->oy) < 1e-6f
      && fabsf(a->oz - b->oz) < 1e-6f;
}

static bool tvdb_csg_sparse_impl(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                                 float background, tvdb_sparse_grid* out, int op) {
  // op: 0 = union (min), 1 = intersection (max), 2 = difference (max(a,-b))
  if (!a || !b || !out) return false;
  if (!tvdb_check_same_frame(a, b)) return false;
  out->count = 0;
  out->voxel_size = a->voxel_size;
  out->ox = a->ox; out->oy = a->oy; out->oz = a->oz;

  tvdb_hash_entry *ha = NULL, *hb = NULL;
  size_t ma = 0, mb = 0;
  uint8_t *first_a = NULL, *first_b = NULL;
  if (!tvdb_hash_build(a, &ha, &ma, &first_a)) return false;
  if (!tvdb_hash_build(b, &hb, &mb, &first_b)) { free(ha); free(first_a); return false; }

  // Walk a, looking up b
  for (size_t i = 0; i < a->count; ++i) {
    int x = a->coords[i].x, y = a->coords[i].y, z = a->coords[i].z;
    if (!(first_a[i >> 3] & (uint8_t)(1u << (i & 7u)))) continue;
    int j = tvdb_hash_get(hb, mb, b->coords, x, y, z);
    float va = a->values[i];
    float vb = (j >= 0) ? b->values[j] : background;
    float v = 0.0f;
    if (op == 0)      v = va < vb ? va : vb;
    else if (op == 1) v = va > vb ? va : vb;
    else              v = va > -vb ? va : -vb;
    if (!tvdb_sparse_push(out, x, y, z, v)) { free(ha); free(hb); free(first_a); free(first_b); return false; }
  }
  // All CSG operations span the coordinate union.
  {
    for (size_t i = 0; i < b->count; ++i) {
      int x = b->coords[i].x, y = b->coords[i].y, z = b->coords[i].z;
      if (tvdb_hash_get(ha, ma, a->coords, x, y, z) >= 0 ||
          !(first_b[i >> 3] & (uint8_t)(1u << (i & 7u)))) continue;
      float vb = b->values[i];
      float v = op == 0 ? (background < vb ? background : vb) :
                op == 1 ? (background > vb ? background : vb) :
                          (background > -vb ? background : -vb);
      if (!tvdb_sparse_push(out, x, y, z, v)) { free(ha); free(hb); free(first_a); free(first_b); return false; }
    }
  }
  free(first_a); free(first_b);
  free(ha); free(hb);
  return true;
}

static bool tvdb_csg_union_sparse_impl(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                           float background, tvdb_sparse_grid* out) {
  return tvdb_csg_sparse_impl(a, b, background, out, 0);
}
static bool tvdb_csg_intersection_sparse_impl(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                                  float background, tvdb_sparse_grid* out) {
  return tvdb_csg_sparse_impl(a, b, background, out, 1);
}
static bool tvdb_csg_difference_sparse_impl(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
                                float background, tvdb_sparse_grid* out) {
  return tvdb_csg_sparse_impl(a, b, background, out, 2);
}

// -------------------------------------------------------------------------
// Morphology
// -------------------------------------------------------------------------

static bool tvdb_dilate_sparse_step(const tvdb_sparse_grid* in,
                                    float background,
                                    tvdb_sparse_grid* out) {
  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;

  tvdb_hash_entry* hin = NULL; size_t mask = 0; uint8_t* first = NULL;
  if (!tvdb_hash_build(in, &hin, &mask, &first)) return false;

  // For each input voxel, emit (self) plus each missing 6-neighbor.
  // The output value at each voxel = min over (self/contributing neighbors).
  // Use a temporary growing hash for the output.
  //
  // The output holds at most 7 voxels per input voxel. The insert guard below
  // keeps the table under a 0.7 load factor (count * 10 < capacity * 7), so the
  // table must hold 7 * count / 0.7 = 10 * count entries; sizing at 10x the input
  // count (rounded up to a power of two, plus slack) guarantees the worst case of
  // isolated voxels each emitting six new neighbors never trips the guard. The
  // guard is kept so a future connectivity change fails loudly instead of
  // spinning forever on a full table.
  size_t guess;
  if (in->count > INT_MAX / 7 || !tvdb_hash_capacity(in->count, 10, &guess)) { free(first); free(hin); return false; }
  tvdb_hash_entry* hout = (tvdb_hash_entry*)calloc(guess, sizeof(tvdb_hash_entry));
  if (!hout) { free(first); free(hin); return false; }
  size_t hout_mask = guess - 1;

  static const int N[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  for (size_t i = 0; i < in->count; ++i) {
    int x = in->coords[i].x, y = in->coords[i].y, z = in->coords[i].z;
    /* Skip a duplicate coordinate: the first occurrence owns the emission, so the
     * output cannot depend on which copy we happen to be looking at. The builder
     * already recorded which entries those are, so this is a bit test rather than
     * a hash probe per input voxel. */
    if (!(first[i >> 3] & (uint8_t)(1u << (i & 7u)))) continue;
    float v_self = in->values[i];

    // self
    {
      uint64_t key = tvdb_pack_ijk(x, y, z);
      size_t h = (size_t)(tvdb_mix64(key) & hout_mask);
      while (hout[h].idx_plus_one) {
        if (hout[h].key == key && tvdb_coord_equal(out->coords[hout[h].idx_plus_one - 1], x, y, z)) break;
        h = (h + 1) & hout_mask;
      }
      if (hout[h].idx_plus_one == 0) {
        /* Defensive: the 10x sizing bound above makes this unreachable. */
        if (out->count * 10u >= guess * 7u) { free(first); free(hin); free(hout); return false; }
        if (!tvdb_sparse_push(out, x, y, z, v_self)) { free(first); free(hin); free(hout); return false; }
        hout[h].key = key; hout[h].idx_plus_one = (uint32_t)out->count;
      } else {
        size_t idx = hout[h].idx_plus_one - 1;
        if (v_self < out->values[idx]) out->values[idx] = v_self;
      }
    }
    // 6 neighbors
    for (int n = 0; n < 6; ++n) {
      int nx, ny, nz;
      if (!tvdb_coord_offset(x, y, z, N[n][0], N[n][1], N[n][2], &nx, &ny, &nz)) continue;
      // The neighbor's value, based on the dense kernel: if the neighbor is
      // active, its own value is unchanged here (it'll process itself);
      // we only contribute v_self to the neighbor's "min" because the
      // neighbor inherits the inside of `i` shifted by one voxel.
      uint64_t key = tvdb_pack_ijk(nx, ny, nz);
      size_t h = (size_t)(tvdb_mix64(key) & hout_mask);
      while (hout[h].idx_plus_one) {
        if (hout[h].key == key && tvdb_coord_equal(out->coords[hout[h].idx_plus_one - 1], nx, ny, nz)) break;
        h = (h + 1) & hout_mask;
      }
      if (hout[h].idx_plus_one == 0) {
        // neighbor not yet in output
        if (out->count * 10u >= guess * 7u) { free(first); free(hin); free(hout); return false; }
        int j = tvdb_hash_get(hin, mask, in->coords, nx, ny, nz);
        float v_existing = (j >= 0) ? in->values[j] : background;
        float v = v_self < v_existing ? v_self : v_existing;
        if (!tvdb_sparse_push(out, nx, ny, nz, v)) { free(first); free(hin); free(hout); return false; }
        hout[h].key = key; hout[h].idx_plus_one = (uint32_t)out->count;
      } else {
        size_t idx = hout[h].idx_plus_one - 1;
        if (v_self < out->values[idx]) out->values[idx] = v_self;
      }
    }
  }
  free(first);
  free(hin);
  free(hout);
  return true;
}

static bool tvdb_dilate_sparse_impl(const tvdb_sparse_grid* in,
                        float background, int iterations,
                        tvdb_sparse_grid* out) {
  if (!in || !out || iterations <= 0) return false;
  // Copy in -> out as the initial state, then iterate using a scratch.
  tvdb_sparse_grid scratch; tvdb_sparse_grid_init(&scratch);
  if (!tvdb_sparse_grid_reserve(out, in->count)) return false;
  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;
  for (size_t i = 0; i < in->count; ++i) {
    if (!tvdb_sparse_push(out, in->coords[i].x, in->coords[i].y, in->coords[i].z, in->values[i])) {
      tvdb_sparse_grid_free(&scratch);
      return false;
    }
  }
  for (int it = 0; it < iterations; ++it) {
    if (!tvdb_dilate_sparse_step(out, background, &scratch)) {
      tvdb_sparse_grid_free(&scratch); return false;
    }
    // swap out <-> scratch
    tvdb_sparse_grid tmp = *out; *out = scratch; scratch = tmp;
  }
  tvdb_sparse_grid_free(&scratch);
  return true;
}

static bool tvdb_erode_sparse_step(const tvdb_sparse_grid* in,
                                   tvdb_sparse_grid* out) {
  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;

  tvdb_hash_entry* hin = NULL; size_t mask = 0; uint8_t* first = NULL;
  if (!tvdb_hash_build(in, &hin, &mask, &first)) return false;

  static const int N[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  for (size_t i = 0; i < in->count; ++i) {
    int x = in->coords[i].x, y = in->coords[i].y, z = in->coords[i].z;
    if (!(first[i >> 3] & (uint8_t)(1u << (i & 7u)))) continue;
    float v_self = in->values[i];
    float r = v_self;
    bool keep = true;
    for (int n = 0; n < 6; ++n) {
      int nx, ny, nz;
      if (!tvdb_coord_offset(x, y, z, N[n][0], N[n][1], N[n][2], &nx, &ny, &nz)) { keep = false; break; }
      int j = tvdb_hash_get(hin, mask, in->coords, nx, ny, nz);
      if (j < 0) { keep = false; break; }
      float v = in->values[j];
      if (v > r) r = v;
    }
    if (keep) {
      if (!tvdb_sparse_push(out, x, y, z, r)) { free(first); free(hin); return false; }
    }
  }
  free(first);
  free(hin);
  return true;
}

static bool tvdb_erode_sparse_impl(const tvdb_sparse_grid* in, int iterations,
                       tvdb_sparse_grid* out) {
  if (!in || !out || iterations <= 0) return false;
  tvdb_sparse_grid scratch; tvdb_sparse_grid_init(&scratch);
  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;
  for (size_t i = 0; i < in->count; ++i) {
    if (!tvdb_sparse_push(out, in->coords[i].x, in->coords[i].y, in->coords[i].z, in->values[i])) {
      tvdb_sparse_grid_free(&scratch);
      return false;
    }
  }
  for (int it = 0; it < iterations; ++it) {
    if (!tvdb_erode_sparse_step(out, &scratch)) {
      tvdb_sparse_grid_free(&scratch); return false;
    }
    tvdb_sparse_grid tmp = *out; *out = scratch; scratch = tmp;
  }
  tvdb_sparse_grid_free(&scratch);
  return true;
}

// -------------------------------------------------------------------------
// 3D convolution (same-topology)
// -------------------------------------------------------------------------

static bool tvdb_sparse_conv3d_impl(const tvdb_sparse_grid* in,
                        const float* kernel,
                        int kx, int ky, int kz,
                        float pad_value,
                        tvdb_sparse_grid* out) {
  if (!in || !kernel || !out) return false;
  if (kx <= 0 || ky <= 0 || kz <= 0) return false;

  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;
  if (in->count == 0) return true;
  if (!tvdb_sparse_grid_reserve(out, in->count)) return false;

  // Anchor (matches numpy/scipy: floor(k/2) regardless of parity).
  const int ax = kx / 2, ay = ky / 2, az = kz / 2;

  float ksum = 0.0f;
  for (size_t i = 0; i < (size_t)kx * (size_t)ky * (size_t)kz; ++i) ksum += kernel[i];

  // Hash input coords for O(1) neighbor lookup.
  tvdb_hash_entry* tbl = NULL; size_t mask = 0;
  if (!tvdb_hash_build(in, &tbl, &mask, NULL)) return false;

  #pragma omp parallel for schedule(static)
  for (long long i = 0; i < (long long)in->count; ++i) {
    int cx = in->coords[i].x;
    int cy = in->coords[i].y;
    int cz = in->coords[i].z;
    float acc = ksum * pad_value;
    // K[di,dj,dk] convolves over neighbors offset by (di-ax, dj-ay, dk-az).
    for (int dk = 0; dk < kz; ++dk) {
      int64_t oz = (int64_t)cz + (dk - az);
      for (int dj = 0; dj < ky; ++dj) {
        int64_t oy = (int64_t)cy + (dj - ay);
        for (int di = 0; di < kx; ++di) {
          int64_t ox = (int64_t)cx + (di - ax);
          float w = kernel[(((size_t)dk * ky) + dj) * kx + di];
          if (w == 0.0f) continue;
          int j = (ox < INT32_MIN || ox > INT32_MAX || oy < INT32_MIN || oy > INT32_MAX ||
                   oz < INT32_MIN || oz > INT32_MAX) ? -1 :
                  tvdb_hash_get(tbl, mask, in->coords, (int)ox, (int)oy, (int)oz);
          if (j >= 0)
            acc += w * (in->values[j] - pad_value);
        }
      }
    }
    out->coords[i].x = cx;
    out->coords[i].y = cy;
    out->coords[i].z = cz;
    out->values[i] = acc;
  }
  out->count = in->count;
  free(tbl);
  return true;
}

// -------------------------------------------------------------------------
// Multi-channel sparse 3D convolution
// -------------------------------------------------------------------------

static bool tvdb_sparse_conv3d_mc_impl(const tvdb_sparse_grid* in,
                           const float* in_values, int c_in,
                           const float* kernel, int kx, int ky, int kz,
                           int c_out,
                           float pad_value,
                           tvdb_sparse_grid* out,
                           float** out_values_mc) {
  if (!in || !in_values || !kernel || !out || !out_values_mc) return false;
  if (kx <= 0 || ky <= 0 || kz <= 0 || c_in <= 0 || c_out <= 0) return false;

  out->count = 0;
  out->voxel_size = in->voxel_size;
  out->ox = in->ox; out->oy = in->oy; out->oz = in->oz;
  *out_values_mc = NULL;
  if (in->count == 0) return true;
  if (!tvdb_sparse_grid_reserve(out, in->count)) return false;

  *out_values_mc = (float *)calloc(in->count * (size_t)c_out, sizeof(float));
  if (!*out_values_mc) return false;

  const int ax = kx / 2, ay = ky / 2, az = kz / 2;
  const size_t spatial_stride = (size_t)c_out * (size_t)c_in;

  tvdb_hash_entry* tbl = NULL; size_t mask = 0;
  if (!tvdb_hash_build(in, &tbl, &mask, NULL)) {
    free(*out_values_mc); *out_values_mc = NULL;
    return false;
  }

  // Precompute per-position sum over input channels: pad contributes
  // pad_value * sum_ci W[co,ci] per kernel position.
  const size_t npos = (size_t)kx * (size_t)ky * (size_t)kz;
  float* sumW = (float*)malloc(npos * (size_t)c_out * sizeof(float));
  if (!sumW) { free(*out_values_mc); *out_values_mc = NULL; return false; }
  for (size_t p = 0; p < npos; ++p) {
    const float* W = kernel + p * spatial_stride;
    for (int co = 0; co < c_out; ++co) {
      float s = 0.0f;
      for (int ci = 0; ci < c_in; ++ci) s += W[(size_t)co * (size_t)c_in + (size_t)ci];
      sumW[p * (size_t)c_out + co] = s;
    }
  }

  #pragma omp parallel for schedule(static)
  for (long long i = 0; i < (long long)in->count; ++i) {
    int cx = in->coords[i].x;
    int cy = in->coords[i].y;
    int cz = in->coords[i].z;
    out->coords[i].x = cx;
    out->coords[i].y = cy;
    out->coords[i].z = cz;
    float* acc = (*out_values_mc) + (size_t)i * (size_t)c_out;
    for (int co = 0; co < c_out; ++co) {
      float s = 0.0f;
      for (size_t p = 0; p < npos; ++p) s += sumW[p * (size_t)c_out + co];
      acc[co] = s * pad_value;
    }

    for (int dk = 0; dk < kz; ++dk) {
      int64_t oz = (int64_t)cz + (dk - az);
      for (int dj = 0; dj < ky; ++dj) {
        int64_t oy = (int64_t)cy + (dj - ay);
        for (int di = 0; di < kx; ++di) {
          int64_t ox = (int64_t)cx + (di - ax);
          const size_t p = ((size_t)dk * (size_t)ky + (size_t)dj) * (size_t)kx + (size_t)di;
          const float* W = kernel + p * spatial_stride;
          int j = (ox < INT32_MIN || ox > INT32_MAX || oy < INT32_MIN || oy > INT32_MAX ||
                   oz < INT32_MIN || oz > INT32_MAX) ? -1 :
                  tvdb_hash_get(tbl, mask, in->coords, (int)ox, (int)oy, (int)oz);
          if (j >= 0) {
            const float* in_v = in_values + (size_t)j * (size_t)c_in;
            for (int co = 0; co < c_out; ++co) {
              float s = 0.0f;
              for (int ci = 0; ci < c_in; ++ci) {
                s += W[(size_t)co * (size_t)c_in + (size_t)ci] * in_v[ci];
              }
              acc[co] += s - sumW[p * (size_t)c_out + co] * pad_value;
            }
          }
        }
      }
    }
  }
  out->count = in->count;
  free(tbl);
  free(sumW);
  return true;
}

static bool tvdb_sparse_input_valid(const tvdb_sparse_grid* g) {
  size_t bytes;
  return g && isfinite(g->voxel_size) && g->voxel_size > 0 &&
    isfinite(g->ox) && isfinite(g->oy) && isfinite(g->oz) &&
    g->count <= INT_MAX && (!g->count || (g->coords && g->values)) &&
    tvdb_size_mul(g->count,sizeof(tvdb_vec3i),&bytes);
}
static bool tvdb_sparse_output_valid(const tvdb_sparse_grid* g) {
  return g && g->count <= g->capacity &&
    (g->capacity ? (g->coords && g->values) : (!g->coords && !g->values));
}
static bool tvdb_sparse_distinct_storage(const tvdb_sparse_grid* in, const tvdb_sparse_grid* out) {
  size_t ci,vi,co,vo;
  if (in == out) return true;
  if (!tvdb_size_mul(in->count,sizeof(tvdb_vec3i),&ci) ||
      !tvdb_size_mul(in->count,sizeof(float),&vi) ||
      !tvdb_size_mul(out->capacity,sizeof(tvdb_vec3i),&co) ||
      !tvdb_size_mul(out->capacity,sizeof(float),&vo)) return false;
  return !tvdb_buffers_overlap(in->coords,ci,out->coords,co) &&
         !tvdb_buffers_overlap(in->values,vi,out->values,vo) &&
         !tvdb_buffers_overlap(in->coords,ci,out->values,vo) &&
         !tvdb_buffers_overlap(in->values,vi,out->coords,co);
}
static void tvdb_sparse_commit(tvdb_sparse_grid* out, tvdb_sparse_grid* result) {
  tvdb_sparse_grid_free(out); *out = *result;
}

bool tvdb_csg_union_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
 float background, tvdb_sparse_grid* out) {
  if (!tvdb_sparse_input_valid(a) || !tvdb_sparse_input_valid(b) ||
      !tvdb_sparse_output_valid(out) || !isfinite(background) ||
      !tvdb_sparse_distinct_storage(a,out) || !tvdb_sparse_distinct_storage(b,out)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_csg_union_sparse_impl(a,b,background,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_csg_intersection_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
 float background, tvdb_sparse_grid* out) {
  if (!tvdb_sparse_input_valid(a) || !tvdb_sparse_input_valid(b) ||
      !tvdb_sparse_output_valid(out) || !isfinite(background) ||
      !tvdb_sparse_distinct_storage(a,out) || !tvdb_sparse_distinct_storage(b,out)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_csg_intersection_sparse_impl(a,b,background,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_csg_difference_sparse(const tvdb_sparse_grid* a, const tvdb_sparse_grid* b,
 float background, tvdb_sparse_grid* out) {
  if (!tvdb_sparse_input_valid(a) || !tvdb_sparse_input_valid(b) ||
      !tvdb_sparse_output_valid(out) || !isfinite(background) ||
      !tvdb_sparse_distinct_storage(a,out) || !tvdb_sparse_distinct_storage(b,out)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_csg_difference_sparse_impl(a,b,background,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_dilate_sparse(const tvdb_sparse_grid* in, float background, int iterations, tvdb_sparse_grid* out) {
  if (!tvdb_sparse_input_valid(in) || !tvdb_sparse_output_valid(out) ||
      !tvdb_sparse_distinct_storage(in,out) || iterations <= 0) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_dilate_sparse_impl(in,background, iterations,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_erode_sparse(const tvdb_sparse_grid* in, int iterations, tvdb_sparse_grid* out) {
  if (!tvdb_sparse_input_valid(in) || !tvdb_sparse_output_valid(out) ||
      !tvdb_sparse_distinct_storage(in,out) || iterations <= 0) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_erode_sparse_impl(in,iterations,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_sparse_conv3d(const tvdb_sparse_grid* in, const float* kernel,
 int kx,int ky,int kz,float pad_value,tvdb_sparse_grid* out) {
  size_t bytes;
  if (!tvdb_sparse_input_valid(in) || !tvdb_sparse_output_valid(out) ||
      !tvdb_sparse_distinct_storage(in,out) || !kernel ||
      !tvdb_grid_bytes(kx,ky,kz,sizeof(float),&bytes)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  bool ok = tvdb_sparse_conv3d_impl(in,kernel,kx,ky,kz,pad_value,&tmp);
  if (ok) tvdb_sparse_commit(out,&tmp); else tvdb_sparse_grid_free(&tmp);
  return ok;
}

bool tvdb_sparse_conv3d_mc(const tvdb_sparse_grid* in,const float* values,int ci,
 const float* kernel,int kx,int ky,int kz,int co,float pad_value,
 tvdb_sparse_grid* out,float** out_values) {
  size_t kb,ib,ob,width;
  if (!out_values) return false;
  *out_values = NULL;
  if (!tvdb_sparse_input_valid(in) || !tvdb_sparse_output_valid(out) ||
      !tvdb_sparse_distinct_storage(in,out) || !values || !kernel || ci<=0 || co<=0 ||
      !tvdb_grid_bytes(kx,ky,kz,sizeof(float),&kb) ||
      !tvdb_size_mul(kb,(size_t)ci,&kb) || !tvdb_size_mul(kb,(size_t)co,&kb) ||
      !tvdb_size_mul((size_t)ci,sizeof(float),&width) || !tvdb_size_mul(in->count,width,&ib) ||
      !tvdb_size_mul((size_t)co,sizeof(float),&width) || !tvdb_size_mul(in->count,width,&ob)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  float *data = NULL;
  bool ok = tvdb_sparse_conv3d_mc_impl(in,values,ci,kernel,kx,ky,kz,co,pad_value,&tmp,&data);
  if (ok) { tvdb_sparse_commit(out,&tmp); *out_values=data; }
  else { tvdb_sparse_grid_free(&tmp); free(data); }
  return ok;
}

/* Materialization also stages allocation before replacing an owning output. */
bool tvdb_dense_to_sparse(const tvdb_dense_grid* dense,float background,float tolerance,tvdb_sparse_grid* out) {
  if(!tvdb_sparse_output_valid(out)) return false;
  tvdb_sparse_grid tmp; tvdb_sparse_grid_init(&tmp);
  if(!tvdb_dense_to_sparse_impl(dense,background,tolerance,&tmp)) { tvdb_sparse_grid_free(&tmp); return false; }
  tvdb_sparse_commit(out,&tmp); return true;
}
