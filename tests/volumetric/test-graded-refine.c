#include "tinyvdb_tet_refine_internal.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static double volume6(const double *p, const uint32_t *t) {
    const double *a = p + 3*t[0], *b = p + 3*t[1];
    const double *c = p + 3*t[2], *d = p + 3*t[3];
    double x1=b[0]-a[0], y1=b[1]-a[1], z1=b[2]-a[2];
    double x2=c[0]-a[0], y2=c[1]-a[1], z2=c[2]-a[2];
    double x3=d[0]-a[0], y3=d[1]-a[1], z3=d[2]-a[2];
    return x1*(y2*z3-z2*y3)-y1*(x2*z3-z2*x3)+z1*(x2*y3-y2*x3);
}
static int face_count(const uint32_t *t, size_t n, uint32_t a, uint32_t b, uint32_t c) {
    size_t i; int count = 0;
    for (i = 0; i < n; ++i) {
        int x=0, y=0, z=0; unsigned j;
        for (j=0; j<4; ++j) { x |= t[4*i+j]==a; y |= t[4*i+j]==b; z |= t[4*i+j]==c; }
        count += x && y && z;
    }
    return count;
}
static int local_size(const double point[3], double *size, void *user) {
    (void)user; *size = point[0] < 2 ? 0.8 : 10.0; return 1;
}
static int invalid_size(const double point[3], double *size, void *user) {
    (void)point; (void)user; *size = NAN; return 1;
}
static int sphere_sdf(const double point[3], float *value, void *user) {
    (void)user; *value = (float)(hypot(hypot(point[0], point[1]), point[2]) - 1.2); return 1;
}
static int fail_sdf(const double point[3], float *value, void *user) {
    (void)point; (void)value; (void)user; return 0;
}
int main(void) {
    const double p[] = {0,0,0, 1,0,0, 0,1,0, 0,0,1, 0,0,-1};
    const uint32_t t[] = {0,1,2,3, 0,2,1,4};
    tvdb_tet_refine_graph *g = NULL, *h = NULL;
    tvdb_error_t error; uint32_t m, n, m2, n2; size_t i;
    const uint32_t *out; const double *pos; double sum = 0;
    assert(tvdb_tet_refine_create(p, 5, t, 2, 20, 20, &g, &error) == TVDB_OK);
    assert(tvdb_tet_refine_create(p, 5, t, 2, 20, 20, &h, &error) == TVDB_OK);
    assert(tvdb_tet_refine_split(g, 0, 1, &m, &error) == TVDB_OK);
    assert(m == 5 && tvdb_tet_refine_tet_count(g) == 4);
    out = tvdb_tet_refine_tets(g);
    assert(face_count(out, 4, 0, m, 2) == 2);
    assert(face_count(out, 4, 1, m, 2) == 2);
    assert(tvdb_tet_refine_split(g, 0, m, &n, &error) == TVDB_OK);
    assert(tvdb_tet_refine_split(h, 0, 1, &m2, &error) == TVDB_OK);
    assert(tvdb_tet_refine_split(h, 0, m2, &n2, &error) == TVDB_OK);
    assert(m == m2 && n == n2 && tvdb_tet_refine_tet_count(g) == 6);
    out = tvdb_tet_refine_tets(g); pos = tvdb_tet_refine_positions(g);
    assert(face_count(out, 6, 0, n, 2) == 2);
    assert(face_count(out, 6, m, n, 2) == 2);
    for (i=0; i<6; ++i) { double v = volume6(pos, out+4*i); assert(v > 0); sum += v; }
    assert(fabs(sum - 2.0) < 1e-12);
    assert(memcmp(out, tvdb_tet_refine_tets(h), 6*4*sizeof(uint32_t)) == 0);
    assert(memcmp(pos, tvdb_tet_refine_positions(h), 7*3*sizeof(double)) == 0);
    tvdb_tet_refine_destroy(g); tvdb_tet_refine_destroy(h);
    /* Repeatedly refine the same edge star. Stale incidence entries must not
       cause duplicate splits, including after the incidence array grows. */
    assert(tvdb_tet_refine_create(p, 5, t, 2, 80, 150, &g, &error) == TVDB_OK);
    m = 1;
    for (i = 0; i < 60; ++i) {
        assert(tvdb_tet_refine_split(g, 0, m, &n, &error) == TVDB_OK);
        m = n;
        assert(tvdb_tet_refine_tet_count(g) == 4 + 2*i);
    }
    out = tvdb_tet_refine_tets(g); pos = tvdb_tet_refine_positions(g); sum = 0;
    for (i = 0; i < tvdb_tet_refine_tet_count(g); ++i) {
        double v = volume6(pos, out + 4*i); assert(v > 0); sum += v;
    }
    assert(fabs(sum - 2.0) < 1e-12);
    tvdb_tet_refine_destroy(g);
    {
      const double points[] = {0,0,0, 1,0,0, 0,1,0, 0,0,1, 0,0,-1,
                               4,0,0, 5,0,0, 4,1,0, 4,0,1};
      const uint32_t cells[] = {0,1,2,3, 0,2,1,4, 5,6,7,8};
      size_t far_count = 0;
      assert(tvdb_tet_refine_create(points, 9, cells, 3, 5000, 10000, &g, &error) == TVDB_OK);
      assert(tvdb_tet_refine_create(points, 9, cells, 3, 5000, 10000, &h, &error) == TVDB_OK);
      assert(tvdb_tet_refine_adapt(g, local_size, NULL, &error) == TVDB_OK);
      assert(tvdb_tet_refine_adapt(h, local_size, NULL, &error) == TVDB_OK);
      size_t count = tvdb_tet_refine_tet_count(g), vertices = tvdb_tet_refine_vertex_count(g);
      out = tvdb_tet_refine_tets(g); pos = tvdb_tet_refine_positions(g); sum = 0;
      assert(count > 3 && count == tvdb_tet_refine_tet_count(h));
      assert(vertices == tvdb_tet_refine_vertex_count(h));
      assert(memcmp(out, tvdb_tet_refine_tets(h), count*4*sizeof(uint32_t)) == 0);
      assert(memcmp(pos, tvdb_tet_refine_positions(h), vertices*3*sizeof(double)) == 0);
      for (i = 0; i < count; ++i) {
        const uint32_t *cell = out + 4*i;
        double v = volume6(pos, cell); assert(v > 0); sum += v;
        if (pos[3*cell[0]] >= 4) ++far_count;
        for (unsigned a = 0; a < 4; ++a) for (unsigned b = a+1; b < 4; ++b) {
          const double *x = pos + 3*cell[a], *y = pos + 3*cell[b];
          double mid[3], target;
          for (unsigned d = 0; d < 3; ++d) mid[d] = 0.5*x[d] + 0.5*y[d];
          local_size(mid, &target, NULL);
          assert(hypot(hypot(x[0]-y[0], x[1]-y[1]), x[2]-y[2]) <= target);
        }
        for (unsigned a = 0; a < 4; ++a) {
          uint32_t f[3]; unsigned nface = 0;
          for (unsigned b = 0; b < 4; ++b) if (a != b) f[nface++] = cell[b];
          if (pos[3*f[0]+2] == 0 && pos[3*f[1]+2] == 0 && pos[3*f[2]+2] == 0 &&
              pos[3*f[0]] < 2)
            assert(face_count(out, count, f[0], f[1], f[2]) == 2);
        }
      }
      assert(far_count == 1 && fabs(sum - 3.0) < 1e-12);
      tvdb_tet_refine_destroy(g); tvdb_tet_refine_destroy(h);
      assert(tvdb_tet_refine_create(points, 9, cells, 3, 9, 3, &g, &error) == TVDB_OK);
      assert(tvdb_tet_refine_adapt(g, invalid_size, NULL, &error) == TVDB_ERROR_INVALID_DATA);
      tvdb_tet_refine_destroy(g);
      assert(tvdb_tet_refine_create(points, 9, cells, 3, 9, 3, &g, &error) == TVDB_OK);
      assert(tvdb_tet_refine_adapt(g, local_size, NULL, &error) == TVDB_ERROR_INVALID_DATA);
      tvdb_tet_refine_destroy(g);
    }
    {
      const double lo[] = {-2,-2,-2}, hi[] = {2,2,2};
      const size_t cells[] = {2,2,2};
      assert(tvdb_tet_refine_box(lo, hi, cells, 10000, 40000, &g, &error) == TVDB_OK);
      assert(tvdb_tet_refine_vertex_count(g) == 27 && tvdb_tet_refine_tet_count(g) == 48);
      pos = tvdb_tet_refine_positions(g); out = tvdb_tet_refine_tets(g); sum = 0;
      for (i = 0; i < 48; ++i) { double v = volume6(pos, out+4*i); assert(v > 0); sum += v; }
      assert(fabs(sum - 384) < 1e-10);
      assert(tvdb_tet_refine_adapt_sdf(g, sphere_sdf, NULL, 0, 1.0, 3.0, 1.0, &error) == TVDB_OK);
      size_t count = tvdb_tet_refine_tet_count(g);
      assert(count > 48);
      pos = tvdb_tet_refine_positions(g); out = tvdb_tet_refine_tets(g); sum = 0;
      for (i = 0; i < count; ++i) {
        const uint32_t *cell = out + 4*i;
        double v = volume6(pos, cell); assert(v > 0); sum += v;
        for (unsigned a = 0; a < 4; ++a) {
          for (unsigned b = a+1; b < 4; ++b) {
            const double *x = pos+3*cell[a], *y = pos+3*cell[b];
            double mid[3]; float field;
            for (unsigned d = 0; d < 3; ++d) mid[d] = 0.5*x[d] + 0.5*y[d];
            sphere_sdf(mid, &field, NULL);
            double target = fmin(3.0, 1.0 + fabs((double)field));
            assert(hypot(hypot(x[0]-y[0], x[1]-y[1]), x[2]-y[2]) <= target);
          }
          uint32_t f[3]; unsigned nface = 0;
          for (unsigned b = 0; b < 4; ++b) if (a != b) f[nface++] = cell[b];
          for (unsigned d = 0; d < 3; ++d)
            if (pos[3*f[0]+d] == 0 && pos[3*f[1]+d] == 0 && pos[3*f[2]+d] == 0)
              assert(face_count(out, count, f[0], f[1], f[2]) == 2);
        }
      }
      assert(fabs(sum - 384) < 1e-9);
      tvdb_tet_refine_destroy(g);
      assert(tvdb_tet_refine_box(lo, hi, cells, 100, 100, &g, &error) == TVDB_OK);
      assert(tvdb_tet_refine_adapt_sdf(g, fail_sdf, NULL, 0, 1, 3, 1, &error) == TVDB_ERROR_INVALID_DATA);
      tvdb_tet_refine_destroy(g);
      assert(tvdb_tet_refine_box(lo, hi, cells, 26, 40000, &g, &error) == TVDB_ERROR_INVALID_ARGUMENT);
      assert(g == NULL);
    }
    assert(tvdb_tet_refine_create(p, 5, t, 2, 5, 3, &g, &error) == TVDB_OK);
    assert(tvdb_tet_refine_split(g, 0, 1, &m, &error) == TVDB_ERROR_INVALID_DATA);
    tvdb_tet_refine_destroy(g);
    { const uint32_t bad[] = {0,1,1,3};
      assert(tvdb_tet_refine_create(p, 5, bad, 1, 6, 2, &g, &error) == TVDB_ERROR_INVALID_DATA);
      assert(g == NULL); }
    puts("graded refinement topology: ok");
    return 0;
}
