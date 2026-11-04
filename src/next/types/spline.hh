// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - typed AOUSD spline (TsSpline) support.
//
// PrimSpec stores authored splines as raw USDA text (`spline_sources_`),
// which composition and the USDA writer pass through verbatim. This module
// provides the typed view over that text on demand:
//   - ParseSplineText / FormatSplineText: USDA `{ ... }` block codec
//     (AOUSD Core Spec 7.4.2.4 grammar).
//   - EncodeSplineBinary / DecodeSplineBinary: OpenUSD ts-binary blob codec
//     (Crate value type 59, byte-compatible with pxr/base/ts/binary.cpp;
//     the surrounding Crate framing is handled by the crate reader/writer).
//   - EvaluateSplineData: time evaluation (AOUSD 12.5.3) via the shared
//     spline evaluator.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

struct SplineKnot {
  double time{0.0};
  double value{0.0};
  double pre_value{0.0};  // dual-valued knots: value approaching from before
  bool dual{false};

  // Tangents in slope form (width unused for hermite).
  double pre_tan_width{0.0};
  double pre_tan_slope{0.0};
  double post_tan_width{0.0};
  double post_tan_slope{0.0};

  // Interpolation of the segment AFTER this knot:
  // 0=none, 1=held, 2=linear, 3=curve.
  int interp{3};

  // TsTangentAlgorithm: 0=None, 1=Custom, 2=AutoEase. A non-None algorithm
  // requires ts-binary version 2 (Crate >= 0.13.0).
  int pre_algo{0};
  int post_algo{0};

  // Authored USDA dictionary for this knot, including its outer braces.
  // Kept as source text so unknown dictionary value types remain lossless.
  std::string custom_data_source;
};

struct SplineData {
  int curve_type{0};  // 0=bezier, 1=hermite

  // Scalar wire type of knot values in the crate blob:
  // 1=double, 2=float, 3=half (0=unspecified reads as double).
  int value_desc{1};

  // Extrapolation: 0=none, 1=held, 2=linear, 3=sloped(x),
  // 4=loop repeat, 5=loop reset, 6=loop oscillate.
  int pre_extrap{1};
  int post_extrap{1};
  double pre_slope{0.0};
  double post_slope{0.0};

  // Inner-loop parameters (AOUSD 7.4.2.4.5). Active when has_loop.
  bool has_loop{false};
  double loop_start{0.0};
  double loop_end{0.0};
  int loop_pre{0};
  int loop_post{0};
  double loop_offset{0.0};

  std::vector<SplineKnot> knots;  // sorted by time when authored validly
};

/// Parse the USDA spline value block (the `{ ... }` text captured by the
/// parser, outer braces included). Does not set `value_desc` — the wire type
/// comes from the attribute's declared type, not the text.
bool ParseSplineText(const std::string& text, SplineData* out,
                     std::string* err);

/// Format as a USDA `{ ... }` block. `indent` prefixes the inner lines; the
/// closing brace is indented one level less (callers embed the result after
/// `name.spline = `).
std::string FormatSplineText(const SplineData& sd, const std::string& indent);

/// ts-binary format version required to encode `sd`: 2 when any knot carries
/// a non-None tangent algorithm (needs Crate 0.13.0), else 1 (Crate 0.12.0).
uint8_t SplineBinaryVersion(const SplineData& sd);

/// Encode/decode the OpenUSD ts-binary blob (Crate type 59 payload without
/// the `uint64 blobSize` / customData framing).
bool EncodeSplineBinary(const SplineData& sd, std::vector<uint8_t>* out,
                        std::string* err);
bool DecodeSplineBinary(const uint8_t* data, size_t size, SplineData* out,
                        std::string* err);

/// Evaluate the spline at `time`. Returns false when the spline yields no
/// value there (empty spline, or a `none` segment / extrapolation).
bool EvaluateSplineData(const SplineData& sd, double time, double* out);

/// One (time, value) vertex of a piecewise-linear approximation.
using SplineSample = std::array<double, 2>;
/// A run of vertices joined by straight segments.
using SplinePolyline = std::vector<SplineSample>;

/// Piecewise-linear approximation of `sd` over the closed interval
/// [t_min, t_max] (AOUSD Core 1.1 "Piecewise-Linear Approximation", optional
/// feature; the analogue of OpenUSD TsSpline::Sample with unit scales).
///
/// Writes polylines in increasing time order. A new polyline starts at every
/// value discontinuity (dual-valued knot, held segment end, loop reset) and
/// after every value-block gap (`none` segment or extrapolation), so regions
/// without a value produce no vertices. Inner loops and looping/linear/sloped/
/// held extrapolation are covered. Within each polyline the vertical
/// deviation from EvaluateSplineData is at most `tolerance` at the sampler's
/// probe points (adaptive bisection; segments are cubic pieces, so the probe
/// bound tracks the true bound closely). Vertex placement is
/// implementation-defined, as the spec allows.
///
/// Returns false (with `err`) on an invalid interval or tolerance, or when the
/// interval spans more loop copies / samples than the internal caps.
bool SampleSplineData(const SplineData& sd, double t_min, double t_max,
                      double tolerance, std::vector<SplinePolyline>* out,
                      std::string* err);

}  // namespace next
}  // namespace lightusd
