/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * spline.c -- C11 cubic spline segment math (hermite basis, bezier, anti-regress).
 *
 * The pure double helpers from src/spline-eval.hh. The templated evaluator that
 * walks the knot list stays C++ (it is instantiated for float and double and
 * consumes a C++ Spline<T>); these leaf routines are shared so the arithmetic
 * lives once and is available from C.
 */

#include "lightusd-core-c.h"

#include <math.h>

double lu_hermite_h00(double u) { return 2.0 * u * u * u - 3.0 * u * u + 1.0; }
double lu_hermite_h10(double u) { return u * u * u - 2.0 * u * u + u; }
double lu_hermite_h01(double u) { return -2.0 * u * u * u + 3.0 * u * u; }
double lu_hermite_h11(double u) { return u * u * u - u * u; }

/* B(u) = (1-u)^3 p0 + 3(1-u)^2 u p1 + 3(1-u) u^2 p2 + u^3 p3 */
double lu_cubic_bezier(double p0, double p1, double p2, double p3, double u) {
  const double iu = 1.0 - u;
  return iu * iu * iu * p0 + 3.0 * iu * iu * u * p1 + 3.0 * iu * u * u * p2 +
         u * u * u * p3;
}

/* B'(u) = 3[(1-u)^2 (p1-p0) + 2(1-u)u (p2-p1) + u^2 (p3-p2)] */
double lu_cubic_bezier_deriv(double p0, double p1, double p2, double p3,
                             double u) {
  const double iu = 1.0 - u;
  return 3.0 * (iu * iu * (p1 - p0) + 2.0 * iu * u * (p2 - p1) +
                u * u * (p3 - p2));
}

/* Newton-Raphson + bisection for B_time(u) = targetTime, u in [0,1]. */
double lu_solve_bezier_time_for_u(double t0, double t1, double t2, double t3,
                                  double target_time, int max_iter) {
  double u = (t3 > t0) ? (target_time - t0) / (t3 - t0) : 0.5;
  u = fmax(0.0, fmin(1.0, u));

  /* A small time residual does not pin u where dt/du ~ 0 (the vertical
   * tangent at the Keep Ratio limit, where u error ~ cbrt(residual)), so also
   * require the Newton correction to be tiny; Newton converges only linearly
   * there, hence the higher iteration floor. Ordinary segments converge
   * quadratically and stop as early as before. */
  const int iters = max_iter > 100 ? max_iter : 100;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < iters; i++) {
    const double bt = lu_cubic_bezier(t0, t1, t2, t3, u);
    const double err = bt - target_time;
    const double dt = lu_cubic_bezier_deriv(t0, t1, t2, t3, u);
    if (err == 0.0 || hi - lo <= 1e-15) break;
    if (fabs(err) < 1e-12 && fabs(err) <= 1e-14 * fabs(dt)) break;

    if (fabs(dt) > 1e-15) {
      const double u_new = u - err / dt;
      if (u_new >= lo && u_new <= hi) {
        u = u_new;
      } else {
        u = 0.5 * (lo + hi);
      }
    } else {
      u = 0.5 * (lo + hi);
    }

    const double btu = lu_cubic_bezier(t0, t1, t2, t3, u);
    if (btu < target_time) {
      lo = u;
    } else {
      hi = u;
    }
  }
  return fmax(0.0, fmin(1.0, u));
}

/* Anti-regression at evaluation time (AOUSD Core 1.1 "Anti-Regression at
 * Evaluation Time"): the Keep Ratio strategy. Backward-facing tangents are
 * zero-length. With normalized widths a = w0/dt, b = w1/dt the Bezier time
 * curve is monotone iff a + b <= 1 or (1 - a - b)^2 <= a*b; scaling both by s
 * keeps the ratio, and the largest valid s is 1 / (a + b - sqrt(a*b)). */
void lu_spline_anti_regress(double t0, double *tw0_post, double *tw1_pre,
                            double t3) {
  const double dt = t3 - t0;
  if (!(dt > 0.0)) {
    *tw0_post = 0.0;
    *tw1_pre = 0.0;
    return;
  }
  double a = fmax(0.0, *tw0_post) / dt;
  double b = fmax(0.0, *tw1_pre) / dt;
  if (a + b > 1.0) {
    const double c = 1.0 - a - b;
    if (c * c > a * b) {
      const double s = 1.0 / (a + b - sqrt(a * b));
      if (s < 1.0) {
        a *= s;
        b *= s;
      }
    }
  }
  *tw0_post = a * dt;
  *tw1_pre = b * dt;
}


/* ------------------------------------------------------------------
 * AutoEase tangents (ComputeAutoEaseTangents / ts/knotData.cpp)
 *
 * Faithful port of ::lightusd::ComputeAutoEaseTangents. Reads only the authored
 * time/value/pre_value/dual/interp/algo fields, so it is safe to run in place:
 * the tangent fields it writes are never read back by the algorithm.
 * ------------------------------------------------------------------ */

static double lu_ae_blend_slope(const lu_spline_knot *in, size_t i) {
  const double prev_t = in[i - 1].time;
  const double next_t = in[i + 1].time;
  const double t = in[i].time;
  const double prev_v = in[i - 1].value;
  const double knot_v = in[i].value;
  const double next_v =
      in[i + 1].dual ? in[i + 1].pre_value : in[i + 1].value;
  const double prev_slope = (knot_v - prev_v) / (t - prev_t);
  const double next_slope = (next_v - knot_v) / (next_t - t);
  if (prev_slope * next_slope <= 0.0) return 0.0; /* extremum -> flat */
  const double f = (t - prev_t) / (next_t - prev_t);
  const double u = f - 0.5;
  const double g = 0.5 + u * (0.5 + 2.0 * u * u);
  double slope = prev_slope + g * (next_slope - prev_slope);
  if (next_slope > 0.0)
    slope = fmin(fmin(slope, 3.0 * next_slope), 3.0 * prev_slope);
  else
    slope = fmax(fmax(slope, 3.0 * next_slope), 3.0 * prev_slope);
  return slope;
}

void lu_spline_compute_auto_ease(lu_spline_knot *knots, size_t n) {
  if (knots == NULL || n == 0) return;
  const lu_spline_knot *in = knots; /* read-only alias: tangent writes only */
  for (size_t i = 0; i < n; ++i) {
    const int want_pre = (in[i].pre_algo == 2);
    const int want_post = (in[i].post_algo == 2);
    if (!want_pre && !want_post) continue;
    const int has_prev = (i > 0);
    const int has_next = (i + 1 < n);
    /* Discontinuity: endpoint, dual-valued knot, or an adjacent value block
     * (interp 0 == none) forces a flat tangent. */
    const int disc = !has_prev || !has_next || in[i].dual ||
                     in[i].interp == 0 || (has_prev && in[i - 1].interp == 0);
    const double slope = disc ? 0.0 : lu_ae_blend_slope(in, i);
    if (want_pre && has_prev) {
      knots[i].pre_tan_slope = slope;
      knots[i].pre_tan_width = (in[i].time - in[i - 1].time) / 3.0;
    }
    if (want_post && has_next) {
      knots[i].post_tan_slope = slope;
      knots[i].post_tan_width = (in[i + 1].time - in[i].time) / 3.0;
    }
  }
}
