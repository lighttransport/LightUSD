// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// spline-eval.hh - Time-value cubic spline evaluator
//
// Implements AOUSD Core Spec sections 7.4.2.4 and 12.5.3:
//   - Bezier and Hermite cubic curve evaluation
//   - Per-segment interpolation: none/held/linear/curve
//   - Extrapolation: none/held/linear/sloped/looprepeat/loopreset/looposcillate
//   - Anti-regression for Bezier time curves (12.5.3.5)
//   - Dual-valued knots for discontinuity (12.5.3.3)
//
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "next/c/lightusd-core-c.h"  // lu_cubic_bezier / lu_hermite_*

namespace lightusd {

// AOUSD Core Spec 7.4.2.4.1
enum class SplineCurveType {
  Bezier,
  Hermite,
};

// AOUSD Core Spec 7.4.2.4.2
enum class SplineInterpolationMode {
  None,    // value-block: no value in this segment
  Held,    // constant: hold previous knot's value
  Linear,  // linearly interpolate between knot values
  Curve,   // cubic interpolation (bezier or hermite)
};

// AOUSD Core Spec 7.4.2.4.4
enum class SplineExtrapolationMode {
  None,           // no value outside knot range
  Held,           // hold edge knot value (default)
  Linear,         // project from edge tangent
  Sloped,         // project with specified slope
  LoopRepeat,     // repeat with value offset
  LoopReset,      // repeat exactly, discontinuous joins
  LoopOscillate,  // repeat with time reversal alternating
};

// AOUSD Core Spec 7.4.2.4.3
template <typename T>
struct SplineKnot {
  double time{0.0};

  T value{};     // value at knot time
  T preValue{};  // value approaching from before (for dual-valued knots)
  bool hasDualValue{false};  // true if preValue differs from value

  // Tangent definition (slope form): slope = height / length
  double preTangentSlope{0.0};
  double preTangentWidth{0.0};
  double postTangentSlope{0.0};
  double postTangentWidth{0.0};

  // How the segment AFTER this knot is interpolated
  SplineInterpolationMode nextInterpolationMode{SplineInterpolationMode::Held};
};

// AOUSD Core Spec 7.4.2.4.5 (inner loop parameters)
struct SplineLoopParams {
  double protoStart{0.0};
  double protoEnd{0.0};
  int numPreLoops{0};
  int numPostLoops{0};
  double valueOffset{0.0};
};

// AOUSD Core Spec 7.4.2.4
template <typename T>
struct Spline {
  SplineCurveType curveType{SplineCurveType::Bezier};

  SplineExtrapolationMode preExtrapolation{SplineExtrapolationMode::Held};
  double preExtrapolationSlope{0.0};

  SplineExtrapolationMode postExtrapolation{SplineExtrapolationMode::Held};
  double postExtrapolationSlope{0.0};

  SplineLoopParams loopParams;

  std::vector<SplineKnot<T>> knots;  // must be sorted by time
};

namespace detail {

// Cubic Hermite basis functions
inline double H00(double u) { return lu_hermite_h00(u); }
inline double H10(double u) { return lu_hermite_h10(u); }
inline double H01(double u) { return lu_hermite_h01(u); }
inline double H11(double u) { return lu_hermite_h11(u); }

// Evaluate cubic Bezier at parameter u: B(u) = (1-u)^3*P0 + 3(1-u)^2*u*P1 + 3(1-u)*u^2*P2 + u^3*P3
inline double EvalCubicBezier(double p0, double p1, double p2, double p3, double u) {
  return lu_cubic_bezier(p0, p1, p2, p3, u);
}

// Derivative of cubic Bezier: B'(u) = 3[(1-u)^2(P1-P0) + 2(1-u)u(P2-P1) + u^2(P3-P2)]
inline double EvalCubicBezierDeriv(double p0, double p1, double p2, double p3, double u) {
  return lu_cubic_bezier_deriv(p0, p1, p2, p3, u);
}

// Newton-Raphson + bisection to solve B_time(u) = targetTime for u in [0,1]
// Returns u parameter. Assumes B_time is monotonically increasing after anti-regression.
inline double SolveBezierTimeForU(double t0, double t1, double t2, double t3,
                                  double targetTime, int maxIter = 20) {
  return lu_solve_bezier_time_for_u(t0, t1, t2, t3, targetTime, maxIter);
}

// Anti-regression: ensure Bezier time curve is monotonic (Spec 12.5.3.5).
// Shrink tangent widths proportionally if the time control points are non-monotonic.
inline void AntiRegress(double t0, double &tw0_post, double &tw1_pre, double t3) {
  lu_spline_anti_regress(t0, &tw0_post, &tw1_pre, t3);
}

}  // namespace detail

///
/// Evaluate a time-value spline at a given time.
///
/// @param[in] spline The spline to evaluate
/// @param[in] time The time to evaluate at
/// @param[out] result The evaluated value
/// @return true if a value was produced, false if value-block (no value)
///
template <typename T>
bool EvaluateSpline(const Spline<T> &spline, double time, T *result) {
  if (!result) return false;

  const auto &knots = spline.knots;

  // Empty spline: no value
  if (knots.empty()) return false;

  // Single knot: return value at knot time, extrapolate otherwise
  if (knots.size() == 1) {
    double knotTime = knots[0].time;
    if (std::abs(time - knotTime) < 1e-15) {
      *result = knots[0].value;
      return true;
    }
    // Fall through to extrapolation handling below
  }

  double firstTime = knots.front().time;
  double lastTime = knots.back().time;

  // --- Pre-extrapolation ---
  if (time < firstTime) {
    switch (spline.preExtrapolation) {
      case SplineExtrapolationMode::None:
        return false;
      case SplineExtrapolationMode::Held:
        *result = knots.front().value;
        return true;
      case SplineExtrapolationMode::Linear: {
        // Use the pre-tangent slope of the first knot
        double slope = knots.front().preTangentSlope;
        double dt = time - firstTime;
        *result = static_cast<T>(static_cast<double>(knots.front().value) + slope * dt);
        return true;
      }
      case SplineExtrapolationMode::Sloped: {
        double dt = time - firstTime;
        *result = static_cast<T>(static_cast<double>(knots.front().value) +
                                 spline.preExtrapolationSlope * dt);
        return true;
      }
      case SplineExtrapolationMode::LoopRepeat:
      case SplineExtrapolationMode::LoopReset:
      case SplineExtrapolationMode::LoopOscillate: {
        // Remap time into knot range
        double period = lastTime - firstTime;
        if (period <= 0.0) {
          *result = knots.front().value;
          return true;
        }
        double dt = firstTime - time;
        double cycles = std::ceil(dt / period);
        double remapped = time + cycles * period;

        if (spline.preExtrapolation == SplineExtrapolationMode::LoopOscillate) {
          int cycleInt = static_cast<int>(cycles);
          if (cycleInt % 2 != 0) {
            // Reverse time within this cycle
            remapped = firstTime + (lastTime - remapped);
          }
        }

        T val{};
        if (!EvaluateSpline(spline, remapped, &val)) return false;

        if (spline.preExtrapolation == SplineExtrapolationMode::LoopRepeat) {
          // Apply value offset
          double offset = -cycles * (static_cast<double>(knots.back().value) -
                                     static_cast<double>(knots.front().value));
          *result = static_cast<T>(static_cast<double>(val) + offset);
        } else {
          *result = val;
        }
        return true;
      }
    }
  }

  // --- Post-extrapolation ---
  if (time > lastTime) {
    switch (spline.postExtrapolation) {
      case SplineExtrapolationMode::None:
        return false;
      case SplineExtrapolationMode::Held:
        *result = knots.back().value;
        return true;
      case SplineExtrapolationMode::Linear: {
        double slope = knots.back().postTangentSlope;
        double dt = time - lastTime;
        *result = static_cast<T>(static_cast<double>(knots.back().value) + slope * dt);
        return true;
      }
      case SplineExtrapolationMode::Sloped: {
        double dt = time - lastTime;
        *result = static_cast<T>(static_cast<double>(knots.back().value) +
                                 spline.postExtrapolationSlope * dt);
        return true;
      }
      case SplineExtrapolationMode::LoopRepeat:
      case SplineExtrapolationMode::LoopReset:
      case SplineExtrapolationMode::LoopOscillate: {
        double period = lastTime - firstTime;
        if (period <= 0.0) {
          *result = knots.back().value;
          return true;
        }
        double dt = time - lastTime;
        double cycles = std::ceil(dt / period);
        double remapped = time - cycles * period;

        if (spline.postExtrapolation == SplineExtrapolationMode::LoopOscillate) {
          int cycleInt = static_cast<int>(cycles);
          if (cycleInt % 2 != 0) {
            remapped = firstTime + (lastTime - remapped);
          }
        }

        T val{};
        if (!EvaluateSpline(spline, remapped, &val)) return false;

        if (spline.postExtrapolation == SplineExtrapolationMode::LoopRepeat) {
          double offset = cycles * (static_cast<double>(knots.back().value) -
                                    static_cast<double>(knots.front().value));
          *result = static_cast<T>(static_cast<double>(val) + offset);
        } else {
          *result = val;
        }
        return true;
      }
    }
  }

  // --- Inner evaluation: find segment ---
  // Binary search for the segment [knots[i], knots[i+1]] containing time
  auto it = std::upper_bound(
      knots.begin(), knots.end(), time,
      [](double t, const SplineKnot<T> &k) { return t < k.time; });

  size_t idx;
  if (it == knots.begin()) {
    idx = 0;
  } else if (it == knots.end()) {
    idx = knots.size() - 2;
  } else {
    idx = static_cast<size_t>(std::distance(knots.begin(), it)) - 1;
  }

  // Exact knot hit
  if (std::abs(time - knots[idx].time) < 1e-15) {
    *result = knots[idx].value;
    return true;
  }
  if (idx + 1 < knots.size() && std::abs(time - knots[idx + 1].time) < 1e-15) {
    // At next knot -- use preValue if dual-valued
    *result = knots[idx + 1].hasDualValue ? knots[idx + 1].preValue
                                           : knots[idx + 1].value;
    return true;
  }

  const auto &k0 = knots[idx];
  const auto &k1 = knots[idx + 1];
  double t0 = k0.time;
  double t1 = k1.time;
  double dt = t1 - t0;

  // Per-segment interpolation
  switch (k0.nextInterpolationMode) {
    case SplineInterpolationMode::None:
      return false;  // value-block

    case SplineInterpolationMode::Held:
      *result = k0.value;
      return true;

    case SplineInterpolationMode::Linear: {
      if (dt <= 0.0) {
        *result = k0.value;
        return true;
      }
      double u = (time - t0) / dt;
      double v0 = static_cast<double>(k0.value);
      double v1 = static_cast<double>(k1.hasDualValue ? k1.preValue : k1.value);
      *result = static_cast<T>(v0 + u * (v1 - v0));
      return true;
    }

    case SplineInterpolationMode::Curve: {
      if (dt <= 0.0) {
        *result = k0.value;
        return true;
      }

      double v0 = static_cast<double>(k0.value);
      double v1 = static_cast<double>(k1.hasDualValue ? k1.preValue : k1.value);

      if (spline.curveType == SplineCurveType::Hermite) {
        // Hermite cubic: u = normalized time in [0,1]
        double u = (time - t0) / dt;
        double m0 = k0.postTangentSlope * dt;  // tangent scaled by interval
        double m1 = k1.preTangentSlope * dt;

        double val = detail::H00(u) * v0 +
                     detail::H10(u) * m0 +
                     detail::H01(u) * v1 +
                     detail::H11(u) * m1;
        *result = static_cast<T>(val);
        return true;
      }

      // Bezier cubic: 4 control points in (time, value) space
      double tw0 = k0.postTangentWidth;
      double tw1 = k1.preTangentWidth;

      // Anti-regression (Spec 12.5.3.5)
      detail::AntiRegress(t0, tw0, tw1, t1);

      // Time control points
      double ct0 = t0;
      double ct1 = t0 + tw0;
      double ct2 = t1 - tw1;
      double ct3 = t1;

      // Value control points
      double cv0 = v0;
      double cv1 = v0 + k0.postTangentSlope * tw0;
      double cv2 = v1 - k1.preTangentSlope * tw1;
      double cv3 = v1;

      // Solve for u such that B_time(u) = time
      double u = detail::SolveBezierTimeForU(ct0, ct1, ct2, ct3, time);

      // Evaluate value at u
      double val = detail::EvalCubicBezier(cv0, cv1, cv2, cv3, u);
      *result = static_cast<T>(val);
      return true;
    }
  }

  return false;
}

}  // namespace lightusd
