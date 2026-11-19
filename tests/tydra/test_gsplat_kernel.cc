// SPDX-License-Identifier: Apache-2.0
// Validates the Cycles-ported Gaussian splat kernel (src/tydra/gsplat-kernel.hh)
// against independent references: brute-force ray scans, the explicit
// covariance (Sigma^-1 = R S^-2 R^T) form, Monte Carlo transmittance and SH
// orthonormality.
#include "tydra/gsplat-kernel.hh"

#include <cmath>
#include <cstdio>
#include <random>

using namespace lightusd::gsplat;

static int g_fail = 0;
#define CHECK(c, ...)                                   \
  do {                                                  \
    if (!(c)) {                                         \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                \
      std::fprintf(stderr, "\n");                       \
      ++g_fail;                                         \
    }                                                   \
  } while (0)

static Quat RandQuat(std::mt19937& r) {
  std::normal_distribution<float> n;
  Quat q{n(r), n(r), n(r), n(r)};
  return q;  // deliberately unnormalized: the kernel must normalize
}

// Independent reference: e = (x-mu)^T R S^-2 R^T (x-mu).
static float CovarianceResponse(const GSplat& g, Vec3 x) {
  Vec3 R[3];
  QuatToRows(g.rotation, R);
  const Vec3 d = Sub(x, g.mu);
  double e = 0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      const float* ri = &R[i].x;
      const float* rj = &R[j].x;
      const float* sc = &g.scale.x;
      for (int k = 0; k < 3; ++k) s += ri[k] * rj[k] / (double(sc[k]) * sc[k]);
      e += (&d.x)[i] * s * (&d.x)[j];
    }
  return float(g.opacity * std::exp(-0.5 * e));
}

static void TestRadius() {
  CHECK(GSplatBoundingRadius(0.005f, {1, 1, 1}) == 0.f, "cull below alpha_min");
  const float r = GSplatBoundingRadius(1.f, {0.1f, 2.f, 0.3f});
  CHECK(std::fabs(r - std::sqrt(2.f * std::log(100.f)) * 2.f) < 1e-5f, "radius %f", r);
}

static void TestPeakAgainstReferences() {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1, 1), us(0.05f, 1.f);
  float worst = 0;
  for (int it = 0; it < 2000; ++it) {
    GSplat g{{u(rng), u(rng), u(rng)}, RandQuat(rng),
             {us(rng), us(rng), us(rng)}, 0.2f + 0.8f * (u(rng) * .5f + .5f)};
    const Vec3 ro = {u(rng) * 5, u(rng) * 5, 5 + u(rng)};
    Vec3 rd = Sub(Add(g.mu, {u(rng) * .5f, u(rng) * .5f, u(rng) * .5f}), ro);
    rd = Mul(rd, 1.f / std::sqrt(Dot(rd, rd)));
    GSplatPeak p;
    if (!GSplatPeakResponse(g, ro, rd, 0.f, 1e30f, &p)) continue;
    // Covariance form at the peak.
    const float ref = CovarianceResponse(g, p.x);
    CHECK(std::fabs(ref - p.alpha) < 1e-4f, "cov %f vs %f", ref, p.alpha);
    // Brute-force: no sample along the ray exceeds the peak.
    float best = 0;
    for (int s = 0; s <= 4000; ++s) {
      const float t = p.t - 2.f + 4.f * s / 4000.f;
      best = std::fmax(best, CovarianceResponse(g, Add(ro, Mul(rd, t))));
    }
    worst = std::fmax(worst, best - p.alpha);
  }
  CHECK(worst < 2e-4f, "brute-force max exceeds peak by %g", worst);
}

static void TestRangeAndDegenerate() {
  GSplat g{{0, 0, 0}, {1, 0, 0, 0}, {1, 1, 1}, 1};
  GSplatPeak p;
  CHECK(!GSplatPeakResponse(g, {0, 0, 5}, {0, 0, 1}, 0, 1e30f, &p), "behind origin");
  CHECK(!GSplatPeakResponse(g, {0, 0, 5}, {0, 0, -1}, 0, 4.f, &p), "beyond tmax");
  CHECK(GSplatPeakResponse(g, {0, 0, 5}, {0, 0, -1}, 0, 6.f, &p) &&
            std::fabs(p.t - 5) < 1e-5f && std::fabs(p.alpha - 1) < 1e-6f,
        "center hit");
  GSplat flat{{0, 0, 0}, {1, 0, 0, 0}, {1, 1, 0}, 1};
  CHECK(!GSplatPeakResponse(flat, {0.3f, 0, 5}, {0, 0.1f, -1}, 0, 1e30f, &p),
        "zero scale rejected (NaN) like Cycles");
}

static void TestStochasticTransmittance() {
  // The fraction of jittered rays accepted must match mean alpha.
  GSplat g{{0, 0, 0}, {0.9f, 0.1f, 0.3f, 0.2f}, {0.4f, 0.2f, 0.8f}, 0.7f};
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-0.3f, 0.3f);
  const int N = 200000;
  double hits = 0, expect = 0;
  for (int i = 0; i < N; ++i) {
    const Vec3 ro = {u(rng), u(rng), 4};
    GSplatPeak p;
    if (!GSplatPeakResponse(g, ro, {0, 0, -1}, 0, 1e30f, &p)) continue;
    expect += p.alpha;
    float t;
    hits += GSplatIntersectStochastic(g, ro, {0, 0, -1}, 0, 1e30f, &t);
  }
  CHECK(std::fabs(hits - expect) / expect < 0.01, "MC %f vs %f", hits, expect);
}

static void TestSH() {
  // Orthonormality of the 16 basis functions (Monte Carlo over the sphere).
  std::mt19937 rng(11);
  std::normal_distribution<float> n;
  double G[16][16] = {};
  const int N = 400000;
  for (int s = 0; s < N; ++s) {
    Vec3 v{n(rng), n(rng), n(rng)};
    v = Mul(v, 1.f / std::sqrt(Dot(v, v)));
    float b[16];
    for (int i = 0; i < 16; ++i) {
      float c[48] = {};
      c[i * 3] = 1;
      b[i] = EvalSH(3, c, v).x;
    }
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j) G[i][j] += b[i] * b[j];
  }
  const double w = 4 * M_PI / N;
  double err = 0;
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j)
      err = std::fmax(err, std::fabs(G[i][j] * w - (i == j)));
  CHECK(err < 0.02, "SH orthonormality error %f", err);

  // DC + 0.5 offset and colour spaces.
  const float dc[3] = {(0.25f - 0.5f) / 0.282094792f, 0, (1.0f - 0.5f) / 0.282094792f};
  const Vec3 lin = GSplatRadiance(0, dc, {0, 0, 1}, GSplatColorSpace::Linear);
  CHECK(std::fabs(lin.x - 0.25f) < 1e-5f && std::fabs(lin.y - 0.5f) < 1e-5f &&
            std::fabs(lin.z - 1.0f) < 1e-5f,
        "linear %f %f %f", lin.x, lin.y, lin.z);
  const Vec3 srgb = GSplatRadiance(0, dc, {0, 0, 1}, GSplatColorSpace::SRGB);
  CHECK(std::fabs(srgb.y - 0.2140411f) < 1e-4f && std::fabs(srgb.z - 1.f) < 1e-5f,
        "srgb %f", srgb.y);
}

// The affine world form must reproduce the local form under an arbitrary
// (sheared, non-uniformly scaled) object transform: response at world x
// equals the local response at L^-1 (x - t).
static void TestAffineWorld() {
  std::mt19937 rng(21);
  std::uniform_real_distribution<float> u(-1, 1), us(0.1f, 0.8f);
  // L (column-vector), with shear; and its inverse.
  const double L[9] = {1.5, 0.3, 0.0, -0.2, 0.7, 0.4, 0.1, 0.0, 2.0};
  double det = L[0] * (L[4] * L[8] - L[5] * L[7]) -
               L[1] * (L[3] * L[8] - L[5] * L[6]) +
               L[2] * (L[3] * L[7] - L[4] * L[6]);
  float linv[9];
  linv[0] = float((L[4] * L[8] - L[5] * L[7]) / det);
  linv[1] = float((L[2] * L[7] - L[1] * L[8]) / det);
  linv[2] = float((L[1] * L[5] - L[2] * L[4]) / det);
  linv[3] = float((L[5] * L[6] - L[3] * L[8]) / det);
  linv[4] = float((L[0] * L[8] - L[2] * L[6]) / det);
  linv[5] = float((L[2] * L[3] - L[0] * L[5]) / det);
  linv[6] = float((L[3] * L[7] - L[4] * L[6]) / det);
  linv[7] = float((L[1] * L[6] - L[0] * L[7]) / det);
  linv[8] = float((L[0] * L[4] - L[1] * L[3]) / det);
  float fro = 0;
  for (double v : L) fro += float(v * v);
  fro = std::sqrt(fro);
  const Vec3 trans = {0.3f, -1.f, 2.f};
  auto toWorld = [&](Vec3 p) {
    return Vec3{float(L[0] * p.x + L[1] * p.y + L[2] * p.z) + trans.x,
                float(L[3] * p.x + L[4] * p.y + L[5] * p.z) + trans.y,
                float(L[6] * p.x + L[7] * p.y + L[8] * p.z) + trans.z};
  };
  auto toLocal = [&](Vec3 w) {
    const Vec3 d = Sub(w, trans);
    return MulA(linv, d);
  };
  float worst = 0;
  for (int it = 0; it < 500; ++it) {
    GSplat g{{u(rng), u(rng), u(rng)}, RandQuat(rng),
             {us(rng), us(rng), us(rng)}, 0.9f};
    GSplatWorld w;
    CHECK(MakeGSplatWorld(toWorld(g.mu), g.rotation, g.scale, g.opacity, linv,
                          fro, &w),
          "make world splat");
    const Vec3 ro = {u(rng) * 4, u(rng) * 4, 9};
    Vec3 rd = Sub(Add(w.mu, {u(rng) * .3f, u(rng) * .3f, 0}), ro);
    rd = Mul(rd, 1.f / std::sqrt(Dot(rd, rd)));
    GSplatPeak p;
    if (!GSplatWorldPeak(w, ro, rd, 0, 1e30f, &p)) continue;
    // Reference: local covariance response at the mapped point.
    const float ref = CovarianceResponse(g, toLocal(p.x));
    worst = std::fmax(worst, std::fabs(ref - p.alpha));
    // Peak: nearby points along the world ray respond no more.
    for (float dt : {-1e-2f, 1e-2f})
      CHECK(CovarianceResponse(g, toLocal(Add(ro, Mul(rd, p.t + dt)))) <=
                p.alpha + 1e-5f,
            "world peak not maximal");
    // Bounding sphere contains the alpha_min isosurface along the ray.
    const float rmax = w.radius;
    for (float dt = -3.f; dt <= 3.f; dt += 0.01f) {
      const Vec3 x = Add(ro, Mul(rd, p.t + dt));
      if (CovarianceResponse(g, toLocal(x)) > kAlphaMin * 1.0001f) {
        const Vec3 d = Sub(x, w.mu);
        CHECK(std::sqrt(Dot(d, d)) <= rmax, "bounding sphere too small");
      }
    }
  }
  CHECK(worst < 1e-4f, "affine vs local %g", worst);
  // Seeds decorrelate the stochastic test at a fixed point.
  int differ = 0;
  for (uint32_t s = 1; s < 64; ++s)
    differ += HashPositionToUnit({1, 2, 3}, s) != HashPositionToUnit({1, 2, 3}, 0);
  CHECK(differ > 60, "seeded hash not varying");
}

int main() {
  TestAffineWorld();
  TestRadius();
  TestPeakAgainstReferences();
  TestRangeAndDegenerate();
  TestStochasticTransmittance();
  TestSH();
  if (g_fail) {
    std::fprintf(stderr, "%d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("test_gsplat_kernel: all passed\n");
  return 0;
}
