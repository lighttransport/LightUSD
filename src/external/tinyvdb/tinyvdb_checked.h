#pragma once

/* Private checked arithmetic shared by allocation and coordinate paths. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

static inline bool tvdb_size_mul(size_t a, size_t b, size_t *out) {
    if (b && a > SIZE_MAX / b) return false;
    *out = a * b;
    return true;
}

static inline bool tvdb_grid_bytes(int nx, int ny, int nz, size_t element_bytes,
                                   size_t *out) {
    size_t n;
    return nx > 0 && ny > 0 && nz > 0 &&
           tvdb_size_mul((size_t)nx, (size_t)ny, &n) &&
           tvdb_size_mul(n, (size_t)nz, &n) &&
           tvdb_size_mul(n, element_bytes, out);
}

static inline bool tvdb_hash_capacity(size_t count, size_t factor, size_t *out) {
    size_t need;
    if (!tvdb_size_mul(count, factor, &need) || need > SIZE_MAX - 16) return false;
    need += 16;
    size_t cap = 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return false;
        cap *= 2;
    }
    *out = cap;
    return true;
}

/* Same as tvdb_hash_capacity but with a rational load factor num/den (e.g.
 * 4/3 for ~0.75 load). Open addressing stays correct at any load below 1; a
 * higher load shrinks the table and improves probe cache locality. */
static inline bool tvdb_hash_capacity_scaled(size_t count, size_t num, size_t den,
                                             size_t *out) {
    size_t need;
    if (den == 0 || !tvdb_size_mul(count, num, &need)) return false;
    need /= den;
    if (need > SIZE_MAX - 16) return false;
    need += 16;
    size_t cap = 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return false;
        cap *= 2;
    }
    *out = cap;
    return true;
}

static inline bool tvdb_coord_offset(int x, int y, int z, int dx, int dy, int dz,
                                     int *ox, int *oy, int *oz) {
    int64_t a = (int64_t)x + dx, b = (int64_t)y + dy, c = (int64_t)z + dz;
    if (a < INT32_MIN || a > INT32_MAX || b < INT32_MIN || b > INT32_MAX ||
        c < INT32_MIN || c > INT32_MAX) return false;
    *ox = (int)a; *oy = (int)b; *oz = (int)c;
    return true;
}

/* Pointer interval comparisons avoid undefined relational pointer operations. */
static inline bool tvdb_buffers_overlap(const void *a, size_t an, const void *b, size_t bn) {
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!a || !b || !an || !bn) return false;
    return x <= y ? y - x < an : x - y < bn;
}

static inline bool tvdb_grid_valid(int nx, int ny, int nz, double h,
                                    const void *data, size_t width) {
    size_t bytes;
    return data && isfinite(h) && h > 0 &&
           tvdb_grid_bytes(nx, ny, nz, width, &bytes);
}

/* Preserve near-edge fractional coordinates, including quadratic overshoot,
   but never convert a distant or nonfinite coordinate to an integer. */
static inline double tvdb_sample_coord(double x, int dim) {
    if (isnan(x)) return 0.0;
    return x < -1.0 ? -1.0 : (x >= dim ? (double)dim - 1.0 : x);
}

static inline int tvdb_sample_floor(double x, int dim) {
    if (isnan(x)) return 0;
    if (x < 0) return -1;
    if (x >= (double)dim - 1.0) return dim - 1;
    return (int)floor(x);
}
