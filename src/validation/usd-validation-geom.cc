// SPDX-License-Identifier: Apache-2.0
// Geom / topology / skeleton validation rules, extracted from
// usd-validation.cc for compile-time parallelism. Part of the
// validation_detail namespace shared through usd-validation-internal.hh.

#include "usd-validation-internal.hh"
#include "core/safe-arithmetic.hh"
#include "schema/geom-point-instancer.hh"
#include "schema/schema-registry.hh"
#include "schema/usd-skel.hh"
#include "types/value-view.hh"
#include "c/lightusd-core-c.h"  // lu_half_to_float

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace validation_detail {

// ---------------------------------------------------------------------------
// geom rules
// ---------------------------------------------------------------------------

// Ancestor-derived context threaded through the recursive prim walk.

void ValidateGeomCommonProperties(const PrimSpec &ps,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  static const std::set<std::string> kVisibility = {"inherited", "invisible"};
  static const std::set<std::string> kPurpose = {"default", "render", "proxy",
                                                 "guide"};
  static const std::set<std::string> kOrientation = {"rightHanded",
                                                     "leftHanded"};

  ValidateTokenSetProperty(ps, "visibility", kVisibility,
                           "geom.gprim.visibility", prim_location, result);
  ValidateTokenSetProperty(ps, "purpose", kPurpose, "geom.gprim.purpose",
                           prim_location, result);
  ValidateTokenSetProperty(ps, "orientation", kOrientation,
                           "geom.gprim.orientation", prim_location, result);

  const std::string location = MakePropertyLocation(prim_location, "extent");
  const auto check_extent = [&](const Value *value, std::optional<double> time) {
    std::vector<std::array<float, 3>> extent;
    if (!ValueToExtent(value, &extent)) return;
    const auto report = [&](const char *message) {
      AddError(result, "geom.gprim.extent", location,
               std::string(message) + (time ? " at time " + std::to_string(*time) : ""));
    };
    if (extent.size() != 2) {
      report("extent must contain exactly 2 points");
      return;
    }
    const auto finite_point = [](const std::array<float, 3> &p) {
      return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
    };
    if (!finite_point(extent[0]) || !finite_point(extent[1])) {
      report("extent bounds must be finite");
    }
    for (size_t axis = 0; axis < 3; axis++) {
      if (extent[0][axis] > extent[1][axis]) {
        report("extent minimum must be <= maximum on every axis");
        break;
      }
    }
  };
  check_extent(GetAttrValue(ps, "extent"), std::nullopt);
  if (const auto *samples = GetTimeSamplesOf(ps, "extent")) {
    for (const auto &sample : *samples) {
      check_extent(ps.time_sample_value(sample.second), sample.first);
    }
  }

  size_t point_count = 0;
  const std::string &type_name = ps.type_name();
  if (type_name != "Points" && type_name != "PointInstancer" &&
      type_name != "BasisCurves" && type_name != "NurbsCurves" &&
      type_name != "HermiteCurves" && type_name != "NurbsPatch" &&
      GetPoint3ArrayLen(ps, "points", &point_count)) {
    ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
    ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                                "geom.gprim.arraySize", prim_location,
                                "points", result);
  }
}

void ValidateGeomPrimitive(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  static const std::set<std::string> kAxis = {"X", "Y", "Z"};
  const std::string &type_name = ps.type_name();

  if (type_name == "Sphere") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cube") {
    ValidatePositiveNumericProperty(ps, "size", "geom.primitive.size",
                                    prim_location, result);
  } else if (type_name == "Cone" || type_name == "Cylinder" ||
             type_name == "Capsule") {
    ValidatePositiveNumericProperty(ps, "radius", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Cylinder_1" || type_name == "Capsule_1") {
    ValidatePositiveNumericProperty(ps, "radiusTop", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "radiusBottom", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "height", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  } else if (type_name == "Plane") {
    ValidatePositiveNumericProperty(ps, "width", "geom.primitive.size",
                                    prim_location, result);
    ValidatePositiveNumericProperty(ps, "length", "geom.primitive.size",
                                    prim_location, result);
    ValidateTokenSetProperty(ps, "axis", kAxis, "geom.primitive.axis",
                             prim_location, result);
  }
}

void ValidateGeomPoints(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t point_count = 0;
  if (!GetPoint3ArrayLen(ps, "points", &point_count)) {
    return;
  }
  const auto *point_samples = GetTimeSamplesOf(ps, "points");
  // The all-time-samples pass resolves both properties at the same time.
  // A default point count is only a valid reference for static topology.
  const bool static_points = !point_samples || point_samples->empty();

  ValidateArrayLengthProperty(ps, "ids", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result, static_points);
  for (const char *name : {"widths", "normals"}) {
    const auto *meta = ps.property_meta(name);
    const bool authored = meta && (meta->authored & PropMeta::kInterpolation);
    const bool constant = authored && meta->interpolation == "constant";
    const bool allow_implicit_singleton = !authored;
    ValidateArrayLengthProperty(ps, name, constant ? 1 : point_count,
                                allow_implicit_singleton,
                                "geom.points.size", prim_location,
                                constant ? "constant interpolation" : "points",
                                result, constant || static_points);
  }
  ValidateArrayLengthProperty(ps, "velocities", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result, static_points);
  ValidateArrayLengthProperty(ps, "accelerations", point_count, false,
                              "geom.points.size", prim_location, "points",
                              result, static_points);

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.points.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }
  }
}

static bool BasisCurvesVaryingSize(const PrimSpec &ps,
                                   const std::vector<int32_t> &counts,
                                   size_t *size) {
  std::string type = "cubic", basis = "bezier", wrap = "nonperiodic";
  GetTokenProperty(ps, "type", &type);
  GetTokenProperty(ps, "basis", &basis);
  GetTokenProperty(ps, "wrap", &wrap);
  if ((type != "linear" && type != "cubic") ||
      (type == "cubic" && basis != "bezier" && basis != "bspline" &&
       basis != "catmullRom") ||
      (wrap != "periodic" && wrap != "nonperiodic" && wrap != "pinned")) {
    return false;
  }
  const bool periodic = wrap == "periodic";
  const size_t step = basis == "bezier" ? 3 : 1;
  *size = 0;
  for (int32_t count : counts) {
    if (count <= 0) return false;
    const size_t n = static_cast<size_t>(count);
    size_t varying = n + (periodic ? 1 : 0);
    if (type == "cubic") {
      // A short nonperiodic or pinned curve uses the single-segment minimum.
      varying = periodic ? n / step : (n > 4 ? (n - 4) / step : 0) + 2;
    }
    if (varying > std::numeric_limits<size_t>::max() - *size) return false;
    *size += varying;
  }
  return true;
}

static void ValidateCubicBasisCounts(const PrimSpec &ps,
                                     const std::vector<int32_t> &counts,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  std::string type = "cubic", basis = "bezier", wrap = "nonperiodic";
  GetTokenProperty(ps, "type", &type);
  GetTokenProperty(ps, "basis", &basis);
  GetTokenProperty(ps, "wrap", &wrap);
  if (type != "cubic" ||
      (basis != "bezier" && basis != "bspline" && basis != "catmullRom") ||
      (wrap != "periodic" && wrap != "nonperiodic" && wrap != "pinned")) {
    return;
  }
  const int step = basis == "bezier" ? 3 : 1;
  for (size_t i = 0; i < counts.size(); ++i) {
    const int32_t count = counts[i];
    if (count <= 0) continue;  // Already checked by the generic curves rule.
    const bool valid = wrap == "periodic" ? count % step == 0
        : wrap == "pinned" && basis != "bezier" ? count >= 2
        : count >= 4 && (count - 4) % step == 0;
    if (!valid) {
      AddError(result, "geom.curves.topology",
               MakePropertyLocation(prim_location, "curveVertexCounts"),
               "Cubic curve " + std::to_string(i) + " has " + std::to_string(count) +
                   " vertices, which do not form complete " + basis + " segments for " + wrap);
    }
  }
}

// Evaluate homogeneous coordinates before projecting to UV. Bound the
// quadratic recurrence so a high authored order cannot dominate validation.
static bool EvaluateTrimEndpoint(const double *points, size_t vertices,
                                 const double *knots, size_t order, double t,
                                 double *uv) {
  constexpr size_t kMaxEvaluatedOrder = 32;
  if (t < knots[order - 1] || t > knots[vertices]) return false;
  const double *point = nullptr;
  std::array<std::array<double, 3>, kMaxEvaluatedOrder> scratch;
  if (t == knots[0] && t == knots[order - 1]) {
    point = points;
  } else if (t == knots[vertices + order - 1] && t == knots[vertices]) {
    point = points + 3 * (vertices - 1);
  } else {
    if (order > kMaxEvaluatedOrder) return false;
    const size_t span = t == knots[vertices] ? vertices - 1 :
        static_cast<size_t>(std::upper_bound(knots + order - 1,
                                            knots + vertices, t) - knots - 1);
    const size_t base = span + 1 - order;
    for (size_t j = 0; j < order; ++j)
      std::copy_n(points + 3 * (base + j), 3, scratch[j].begin());
    for (size_t level = 1; level < order; ++level) {
      for (size_t j = order - 1; j >= level; --j) {
        const double lo = knots[base + j];
        const double hi = knots[span + j + 1 - level];
        const double interval = hi - lo;
        if (!std::isfinite(interval)) return false;
        const double blend = interval > 0 ? (t - lo) / interval : 0.0;
        for (size_t axis = 0; axis < 3; ++axis)
          scratch[j][axis] = (1.0 - blend) * scratch[j - 1][axis] +
                             blend * scratch[j][axis];
      }
    }
    point = scratch[order - 1].data();
  }
  if (!std::isfinite(point[2]) || point[2] == 0) return false;
  uv[0] = point[0] / point[2];
  uv[1] = point[1] / point[2];
  return std::isfinite(uv[0]) && std::isfinite(uv[1]);
}

static void ValidateNurbsTrimTopology(const PrimSpec &ps,
                                      const std::string &prim_location,
                                      USDValidationResult *result) {
  const char *rule = "geom.nurbsPatch.trimTopology";
  const auto sum = [&](const char *name, std::vector<int32_t> &counts)
      -> std::optional<size_t> {
    if (!GetIntArrayProperty(ps, name, &counts)) return std::nullopt;
    size_t total = 0;
    for (int32_t count : counts) {
      if (count <= 0 || !safe::add(total, count, &total)) {
        AddError(result, rule, MakePropertyLocation(prim_location, name),
                 "NURBS trim counts and orders must be positive and have a representable sum");
        return std::nullopt;
      }
    }
    return total;
  };
  const auto count_matches = [&](const char *name, size_t expected) {
    size_t count = 0;
    if (GetArrayLengthProperty(ps, name, &count) && count != expected) {
      AddError(result, rule, MakePropertyLocation(prim_location, name),
               "NURBS trim array must contain " + std::to_string(expected) + " elements");
    }
  };
  std::vector<int32_t> loop_counts, curve_vertices, curve_orders;
  const auto curves = sum("trimCurve:counts", loop_counts);
  if (curves) {
    count_matches("trimCurve:orders", *curves);
    count_matches("trimCurve:vertexCounts", *curves);
    count_matches("trimCurve:ranges", *curves);
  }
  const auto vertices = sum("trimCurve:vertexCounts", curve_vertices);
  const auto orders = sum("trimCurve:orders", curve_orders);
  if (vertices) count_matches("trimCurve:points", *vertices);
  if (vertices && orders) {
    if (curve_vertices.size() == curve_orders.size()) {
      for (size_t curve = 0; curve < curve_orders.size(); ++curve) {
        if (curve_vertices[curve] < curve_orders[curve]) {
          AddError(result, rule,
                   MakePropertyLocation(prim_location, "trimCurve:vertexCounts"),
                   "NURBS trim curve " + std::to_string(curve) +
                       " must have at least as many vertices as its order");
        }
      }
    }
    size_t knots = 0;
    if (safe::add(*vertices, *orders, &knots)) {
      count_matches("trimCurve:knots", knots);
      std::vector<double> values;
      if (curve_vertices.size() == curve_orders.size() &&
          GetDoubleArrayProperty(ps, "trimCurve:knots", &values) &&
          values.size() == knots) {
        const Value *range_value = GetAttrValue(ps, "trimCurve:ranges");
        const auto ranges = range_value && range_value->type_id() == TypeId::Double2
            ? range_value->as_double_array() : ValueArrayView<const double>{};
        const bool have_ranges = ranges && ranges->size() % 2 == 0 &&
                                 ranges->size() / 2 == curve_orders.size();
        const Value *point_value = GetAttrValue(ps, "trimCurve:points");
        const auto points = point_value && point_value->type_id() == TypeId::Double3
            ? point_value->as_double_array() : ValueArrayView<const double>{};
        const bool check_closure = curves && *curves == curve_orders.size() &&
            have_ranges && points && points->size() % 3 == 0 &&
            points->size() / 3 == *vertices;
        using Endpoints = std::array<double, 4>;
        std::vector<std::optional<Endpoints>> endpoints;
        if (check_closure) endpoints.resize(*curves);
        size_t offset = 0;
        size_t point_offset = 0;
        for (size_t curve = 0; curve < curve_orders.size(); ++curve) {
          const size_t count = static_cast<size_t>(curve_vertices[curve]) +
                               static_cast<size_t>(curve_orders[curve]);
          const auto first = values.begin() + static_cast<std::ptrdiff_t>(offset);
          const auto last = first + static_cast<std::ptrdiff_t>(count);
          const bool ordered =
              std::all_of(first, last, [](double knot) { return std::isfinite(knot); }) &&
              std::is_sorted(first, last);
          if (!ordered) {
            AddError(result, rule, MakePropertyLocation(prim_location, "trimCurve:knots"),
                     "NURBS trim curve " + std::to_string(curve) +
                         " knots must be finite and nondecreasing");
          }
          if (have_ranges) {
            const double low = (*ranges)[2 * curve];
            const double high = (*ranges)[2 * curve + 1];
            const auto order = static_cast<size_t>(curve_orders[curve]);
            if (!std::isfinite(low) || !std::isfinite(high) || low >= high ||
                (ordered && (low < values[offset + order - 1] ||
                             high > values[offset + count - 1]))) {
              AddError(result, rule,
                       MakePropertyLocation(prim_location, "trimCurve:ranges"),
                       "NURBS trim curve " + std::to_string(curve) +
                           " range must be finite, increasing, and within its knot bounds");
            } else if (check_closure && ordered &&
                       curve_vertices[curve] >= curve_orders[curve]) {
              const size_t vertex_count = static_cast<size_t>(curve_vertices[curve]);
              const double *control = &(*points)[3 * point_offset];
              const double *curve_knots = values.data() + offset;
              Endpoints uv;
              if (EvaluateTrimEndpoint(control, vertex_count, curve_knots, order, low, uv.data()) &&
                  EvaluateTrimEndpoint(control, vertex_count, curve_knots, order, high, uv.data() + 2)) {
                endpoints[curve] = uv;
              }
            }
          }
          offset += count;
          point_offset += static_cast<size_t>(curve_vertices[curve]);
        }
        if (check_closure) {
          const auto close = [](double a, double b) {
            return std::abs(a - b) <= 1e-6 * std::max({1.0, std::abs(a), std::abs(b)});
          };
          size_t first_curve = 0;
          for (int32_t loop_count : loop_counts) {
            const size_t end_curve = first_curve + static_cast<size_t>(loop_count);
            for (size_t curve = first_curve; curve < end_curve; ++curve) {
              const size_t next = curve + 1 == end_curve ? first_curve : curve + 1;
              if (endpoints[curve] && endpoints[next] &&
                  (!close((*endpoints[curve])[2], (*endpoints[next])[0]) ||
                   !close((*endpoints[curve])[3], (*endpoints[next])[1]))) {
                AddWarning(result, "geom.nurbsPatch.trimClosure",
                           MakePropertyLocation(prim_location, "trimCurve:points"),
                           "NURBS trim curve " + std::to_string(curve) +
                               " does not meet curve " + std::to_string(next));
              }
            }
            first_curve = end_curve;
          }
        }
      }
    } else {
      AddError(result, rule, MakePropertyLocation(prim_location, "trimCurve:knots"),
               "NURBS trim knot count is too large");
    }
  }
}

static void ValidateNurbsPatchTopology(const PrimSpec &ps,
                                       const std::string &prim_location,
                                       USDValidationResult *result) {
  const char *rule = "geom.nurbsPatch.topology";
  const auto check_axis = [&](const char *vertex_name, const char *order_name,
                              const char *knot_name,
                              const char *range_name) -> std::optional<uint64_t> {
    int64_t vertices = 0, order = 0;
    const bool have_vertices = GetIntegerScalarProperty(ps, vertex_name, &vertices);
    const bool have_order = GetIntegerScalarProperty(ps, order_name, &order);
    if (have_order && order <= 0) {
      AddError(result, rule, MakePropertyLocation(prim_location, order_name),
               "NURBS order must be positive");
    }
    if (have_vertices && (vertices <= 0 || (have_order && vertices < order))) {
      AddError(result, rule, MakePropertyLocation(prim_location, vertex_name),
               "NURBS vertex count must be positive and at least the order");
    }
    std::vector<double> knots;
    bool knots_ordered = false;
    if (GetDoubleArrayProperty(ps, knot_name, &knots)) {
      const auto location = MakePropertyLocation(prim_location, knot_name);
      if (have_vertices && have_order && vertices > 0 && order > 0 &&
          knots.size() != static_cast<uint64_t>(vertices) + static_cast<uint64_t>(order)) {
        AddError(result, rule, location, "NURBS knot count must equal vertex count plus order");
      }
      knots_ordered =
          std::all_of(knots.begin(), knots.end(), [](double knot) { return std::isfinite(knot); }) &&
          std::is_sorted(knots.begin(), knots.end());
      if (!knots_ordered) {
        AddError(result, rule, location, "NURBS knots must be finite and nondecreasing");
      }
    }
    if (const Value *value = GetAttrValue(ps, range_name)) {
      if (const double *range = value->as_double2()) {
        const bool have_knot_interval = knots_ordered && have_order &&
            order > 0 && static_cast<uint64_t>(order) <= knots.size();
        if (!std::isfinite(range[0]) || !std::isfinite(range[1]) ||
            range[0] >= range[1] ||
            (have_knot_interval &&
             (range[0] < knots[static_cast<size_t>(order - 1)] ||
              range[1] > knots.back()))) {
          AddError(result, rule, MakePropertyLocation(prim_location, range_name),
                   "NURBS range must be finite, increasing, and within the axis knot bounds");
        }
      }
    }
    if (!have_vertices || vertices <= 0) return std::nullopt;
    return static_cast<uint64_t>(vertices);
  };
  const auto u = check_axis("uVertexCount", "uOrder", "uKnots", "uRange");
  const auto v = check_axis("vVertexCount", "vOrder", "vKnots", "vRange");
  size_t points = 0;
  if (!GetPoint3ArrayLen(ps, "points", &points)) return;
  if (u && v && (points % *u != 0 || points / *u != *v)) {
    AddError(result, rule, MakePropertyLocation(prim_location, "points"),
             "NURBS points must fill the U by V control grid");
  }
  size_t weights = 0;
  if (GetArrayLengthProperty(ps, "pointWeights", &weights) && weights != points) {
    AddError(result, rule, MakePropertyLocation(prim_location, "pointWeights"),
             "NURBS pointWeights must match the point count");
  }
}

static void ValidateNurbsCurveTopology(const PrimSpec &ps,
                                      const std::string &prim_location,
                                      const std::vector<int32_t> &vertices,
                                      USDValidationResult *result) {
  const auto error = [&](const char *name, const std::string &message) {
    AddError(result, "geom.nurbsCurves.topology",
             MakePropertyLocation(prim_location, name), message);
  };
  size_t points = 0, weights = 0;
  if (GetPoint3ArrayLen(ps, "points", &points) &&
      GetArrayLengthProperty(ps, "pointWeights", &weights) && points != weights) {
    error("pointWeights", "NURBS pointWeights must match the point count");
  }
  const Value *range_value = GetAttrValue(ps, "ranges");
  const auto ranges = range_value && range_value->type_id() == TypeId::Double2
      ? range_value->as_double_array() : ValueArrayView<const double>{};
  const bool have_ranges = ranges && ranges->size() % 2 == 0 &&
                           ranges->size() / 2 == vertices.size();
  if (ranges && !have_ranges) {
    error("ranges", "NURBS ranges must contain one interval per curve");
  }
  std::vector<int32_t> orders;
  if (!GetIntArrayProperty(ps, "order", &orders)) return;
  if (orders.size() != vertices.size()) {
    error("order", "NURBS order must contain one value per curve");
    return;
  }
  size_t total = 0;
  bool valid = true;
  for (size_t i = 0; i < orders.size(); ++i) {
    if (orders[i] <= 0 || orders[i] > vertices[i]) {
      error("order", "Curve " + std::to_string(i) +
                         " must have positive order no greater than its vertex count");
      valid = false;
    } else if (!safe::add(total, vertices[i], &total) ||
               !safe::add(total, orders[i], &total)) {
      error("knots", "NURBS knot count is too large");
      return;
    }
  }
  std::vector<double> knots;
  if (!valid || !GetDoubleArrayProperty(ps, "knots", &knots)) return;
  if (knots.size() != total) {
    error("knots", "NURBS knot count must equal the sum of vertex counts and orders");
    return;
  }
  size_t offset = 0;
  for (size_t i = 0; i < orders.size(); ++i) {
    const size_t order = static_cast<size_t>(orders[i]);
    const size_t count = static_cast<size_t>(vertices[i]) + order;
    const auto first = knots.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = first + static_cast<std::ptrdiff_t>(count);
    if (!std::all_of(first, end, [](double knot) { return std::isfinite(knot); }) ||
        !std::is_sorted(first, end)) {
      error("knots", "Curve " + std::to_string(i) + " knots must be finite and nondecreasing");
    } else if (have_ranges) {
      const double low = (*ranges)[2 * i], high = (*ranges)[2 * i + 1];
      if (!std::isfinite(low) || !std::isfinite(high) || low >= high ||
          low < knots[offset + order - 1] || high > knots[offset + count - 1]) {
        error("ranges", "Curve " + std::to_string(i) + " range must be within its knot bounds");
      }
    }
    offset += count;
  }
}

void ValidateGeomCurves(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kBasisCurveTypes = {"linear", "cubic"};
  static const std::set<std::string> kBasis = {"bezier", "bspline",
                                               "catmullRom"};
  static const std::set<std::string> kWrap = {"nonperiodic", "periodic",
                                              "pinned"};
  static const std::set<std::string> kForm = {"open", "closed", "periodic"};

  if (ps.type_name() == "BasisCurves") {
    ValidateTokenSetProperty(ps, "type", kBasisCurveTypes,
                             "geom.curves.tokens", prim_location, result);
    ValidateTokenSetProperty(ps, "basis", kBasis, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "wrap", kWrap, "geom.curves.tokens",
                             prim_location, result);
  } else if (ps.type_name() == "NurbsPatch") {
    ValidateNurbsPatchTopology(ps, prim_location, result);
    ValidateNurbsTrimTopology(ps, prim_location, result);
    ValidateTokenSetProperty(ps, "uForm", kForm, "geom.curves.tokens",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "vForm", kForm, "geom.curves.tokens",
                             prim_location, result);
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
  if (have_points && ps.type_name() == "HermiteCurves") {
    size_t tangents = 0;
    if (GetPoint3ArrayLen(ps, "tangents", &tangents) && tangents != point_count) {
      AddError(result, "geom.curves.topology",
               MakePropertyLocation(prim_location, "tangents"),
               "Hermite tangents must match the point count");
    }
  }
  std::vector<int32_t> counts;
  const bool have_counts =
      GetIntArrayProperty(ps, "curveVertexCounts", &counts);
  if (have_counts) {
    if (ps.type_name() == "BasisCurves") {
      ValidateCubicBasisCounts(ps, counts, prim_location, result);
    } else if (ps.type_name() == "NurbsCurves") {
      ValidateNurbsCurveTopology(ps, prim_location, counts, result);
    }
    size_t total = 0;
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.curves.topology",
                 MakePropertyLocation(prim_location, "curveVertexCounts"),
                 "curveVertexCounts[" + std::to_string(i) +
                     "] must be positive");
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
    if (have_points && total != point_count) {
      AddError(result, "geom.curves.topology",
               MakePropertyLocation(prim_location, "curveVertexCounts"),
               "sum(curveVertexCounts) is " + std::to_string(total) +
                   " but points has " + std::to_string(point_count) +
                   " elements");
    }
  }

  std::vector<float> widths;
  if (GetFloatArrayProperty(ps, "widths", &widths)) {
    for (size_t i = 0; i < widths.size(); i++) {
      if (!std::isfinite(widths[i]) || widths[i] < 0.0f) {
        AddError(result, "geom.curves.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "widths[" + std::to_string(i) +
                     "] must be finite and non-negative");
      }
    }

    const PropMeta *width_meta = ps.property_meta("widths");
    if (ps.type_name() == "BasisCurves" && width_meta &&
        width_meta->interpolation == "varying") {
      size_t expected = 0;
      if (have_counts && BasisCurvesVaryingSize(ps, counts, &expected) &&
          widths.size() != expected) {
        AddError(result, "geom.curves.widths",
                 MakePropertyLocation(prim_location, "widths"),
                 "varying widths length must match the curve topology (" +
                     std::to_string(expected) + ")");
      }
      return;
    }

    const bool length_ok = widths.size() == 1 ||
                           (have_points && widths.size() == point_count) ||
                           (have_counts && widths.size() == counts.size());
    if (!length_ok) {
      AddError(result, "geom.curves.widths",
               MakePropertyLocation(prim_location, "widths"),
               "widths length must be 1, match points, or match curve count");
    }
  }
}

// ---------------------------------------------------------------------------
// UsdVol / UsdRender structural checks (PRODUCT PARITY, not AOUSD Core):
// vol.volume.fieldRel / vol.fieldAsset.* / render.*
// ---------------------------------------------------------------------------

void ValidateVolume(const PrimSpec &ps, const UsdPrim &prim,
                    const std::string &prim_location,
                    const PrimTypeByPath &prim_types, USDValidationResult *result) {
  // Volume binds its fields through `field:<name>` RELATIONSHIPS; an
  // attribute in that namespace is an authoring error, and a bound field
  // relationship without a target renders nothing. Attributes live in the
  // property slots; relationships (including declared-only ones) live in the
  // prim's relationship map, so both containers are walked.
  for (const PropSlot &slot : ps.properties().slots()) {
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (prop_name.compare(0, 6, "field:") != 0) continue;
    if (slot.is_relationship()) continue;  // targets checked below
    AddError(result, "vol.volume.fieldRel",
             MakePropertyLocation(prim_location, prop_name),
             "Volume `" + prop_name +
                 "` must be a relationship targeting a field prim");
  }
  for (const std::string &rel_name : ps.relationship_names()) {
    if (rel_name.compare(0, 6, "field:") != 0) continue;
    std::vector<Path> targets;
    prim.GetForwardedRelationshipTargets(rel_name, &targets);
    if (targets.empty()) {
      const auto edit = ps.relationship_edits().find(rel_name);
      const auto *direct = ps.relationship(rel_name);
      if (edit != ps.relationship_edits().end() && edit->second.authored &&
          edit->second.is_explicit && direct && direct->empty()) {
        // UsdVolVolume::BlockFieldRelationship authors an explicit empty list.
        continue;
      }
      AddWarning(result, "vol.volume.fieldRel",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name +
                     "` has no target");
      continue;
    }
    // UsdVolVolume::GetFieldPaths ignores a field relationship unless it has
    // exactly one target.
    if (targets.size() != 1) {
      AddWarning(result, "vol.volume.fieldTargetCount",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name + "` has " +
                     std::to_string(targets.size()) +
                     " targets; UsdVol uses a field only with exactly one");
      continue;
    }
    // Same-layer targets must be volume fields (VolumeFieldBase).
    const Path &target_path = targets.front();
    const std::string &target = target_path.str();
    if (target_path.has_property() || target_path.is_root()) {
      AddWarning(result, "vol.volume.fieldTargetType",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name + "` resolves to `" +
                     target + "`, which is not a field prim path");
      continue;
    }
    const std::string *type = FindPrimType(prim_types, target);
    if (!type || type->empty()) continue;
    const SchemaRegistry &registry = GetSchemaRegistry();
    if (registry.HasParentEntry(*type) &&
        !registry.InheritsFrom(*type, "VolumeFieldBase")) {
      AddWarning(result, "vol.volume.fieldTargetType",
                 MakePropertyLocation(prim_location, rel_name),
                 "Volume field relationship `" + rel_name + "` targets `" + target +
                     "`, a " + *type + ", not a volume field (VolumeFieldBase)");
    }
  }
}

void ValidateFieldAsset(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  static const std::set<std::string> kVdbDataTypes = {
      "half",  "float",  "double", "int",     "uint",     "int64",
      "half2", "float2", "double2", "int2",   "half3",    "float3",
      "double3", "int3", "matrix3d", "matrix4d", "quatd", "bool",
      "mask",  "string"};
  static const std::set<std::string> kField3dDataTypes = {
      "half", "float", "double", "half3", "float3", "double3"};
  static const std::set<std::string> kFieldClass = {"levelSet", "fogVolume",
                                                    "staggered", "unknown"};
  static const std::set<std::string> kVectorRole = {"None", "Point", "Normal",
                                                    "Vector", "Color"};

  const auto has_value = [&](const char *name, const auto &usable) {
    if (const Value *value = GetAttrValue(ps, name); value && usable(*value)) {
      return true;
    }
    if (const auto *samples = GetTimeSamplesOf(ps, name)) {
      for (const auto &sample : *samples) {
        const Value *value = ps.time_sample_value(sample.second);
        if (value && !value->is_block() && !value->is_empty() && usable(*value)) {
          return true;
        }
      }
    }
    return false;
  };
  if (!HasAttributeProp(ps, "filePath")) {
    AddWarning(result, "vol.fieldAsset.filePath",
               MakePropertyLocation(prim_location, "filePath"),
               ps.type_name() + " should author filePath");
  } else if (!has_value("filePath", [](const Value &value) {
               const auto path = value.as_asset_path();
               return path && !path->empty();
             })) {
    AddWarning(result, "vol.fieldAsset.filePath",
               MakePropertyLocation(prim_location, "filePath"),
               ps.type_name() + " filePath should be a non-empty asset path");
  }
  // A missing fieldDataType can be supplied by another layer. Check
  // presence independently of the token legality diagnostic below.
  if (!has_value("fieldDataType", [](const Value &) { return true; })) {
    AddWarning(result, "vol.fieldAsset.dataType",
               MakePropertyLocation(prim_location, "fieldDataType"),
               ps.type_name() + " does not author a fieldDataType value "
               "(pxr: a missing value is an error on the composed stage)");
  }
  if (ps.type_name() == "OpenVDBAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kVdbDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
    ValidateTokenSetProperty(ps, "fieldClass", kFieldClass,
                             "vol.fieldAsset.fieldClass", prim_location,
                             result);
  } else if (ps.type_name() == "Field3DAsset") {
    ValidateTokenSetProperty(ps, "fieldDataType", kField3dDataTypes,
                             "vol.fieldAsset.dataType", prim_location, result);
  }
  ValidateTokenSetProperty(ps, "vectorDataRoleHint", kVectorRole,
                           "vol.fieldAsset.vectorDataRoleHint", prim_location,
                           result);
  const auto check_index = [&](const Value *value, std::optional<double> time) {
    const int32_t *index = value ? value->as_int() : nullptr;
    if (index && *index < 0) {
      AddError(result, "vol.fieldAsset.fieldIndex",
               MakePropertyLocation(prim_location, "fieldIndex"),
               time ? "fieldIndex must be non-negative at time " + std::to_string(*time)
                    : "fieldIndex must be non-negative");
    }
  };
  check_index(GetAttrValue(ps, "fieldIndex"), std::nullopt);
  if (const auto *samples = GetTimeSamplesOf(ps, "fieldIndex")) {
    for (const auto &sample : *samples) {
      check_index(ps.time_sample_value(sample.second), sample.first);
    }
  }
}

// Decode a numeric array Value (half/float/double scalars, vectors or quats)
// into floats. *lanes = components per element. False for other types.
static bool DecodeParticleFloats(const Value &v, std::vector<float> *out,
                                 size_t *lanes) {
  if (!v.is_array() || v.is_block()) return false;
  const size_t n = v.array_size();
  size_t bytes = 0;
  const uint8_t *data = v.view().raw_bytes(&bytes);
  if (!data && n) return false;
  size_t lanes_t;
  bool is_half = false;
  switch (v.type_id()) {
    case TypeId::Half: is_half = true; lanes_t = 1; break;
    case TypeId::Float: case TypeId::Double: lanes_t = 1; break;
    case TypeId::Half2: is_half = true; lanes_t = 2; break;
    case TypeId::Float2: case TypeId::Double2: lanes_t = 2; break;
    case TypeId::Half3: case TypeId::Point3h: case TypeId::Vector3h:
      is_half = true; lanes_t = 3; break;
    case TypeId::Float3: case TypeId::Double3: case TypeId::Point3f:
    case TypeId::Point3d: case TypeId::Vector3f: case TypeId::Vector3d:
      lanes_t = 3; break;
    case TypeId::Half4: case TypeId::Quath: is_half = true; lanes_t = 4; break;
    case TypeId::Float4: case TypeId::Double4: case TypeId::Quatf:
    case TypeId::Quatd: lanes_t = 4; break;
    default: return false;
  }
  *lanes = lanes_t;
  if (n == 0) { out->clear(); return true; }
  // Element width from storage (half arrays may be stored widened).
  const size_t esize = bytes / (n * lanes_t);
  if (esize != 2 && esize != 4 && esize != 8) return false;
  if (esize == 2 && !is_half) return false;
  const size_t count = n * lanes_t;
  out->resize(count);
  for (size_t i = 0; i < count; i++) {
    const uint8_t *e = data + i * esize;
    if (esize == 2) {
      uint16_t h; std::memcpy(&h, e, 2); (*out)[i] = lu_half_to_float(h);
    } else if (esize == 4) {
      std::memcpy(&(*out)[i], e, 4);
    } else {
      double d; std::memcpy(&d, e, 8); (*out)[i] = float(d);
    }
  }
  return true;
}

// Per-value checks of one particle array value (default or one time sample).
static void ValidateParticleValues(const std::string &prop, const Value &v,
                                   const std::string &where_suffix,
                                   const std::string &location,
                                   USDValidationResult *result) {
  std::vector<float> f;
  size_t lanes = 1;
  if (!DecodeParticleFloats(v, &f, &lanes) || lanes == 0) return;
  const size_t n = f.size() / lanes;
  size_t nonfinite = 0, bad = 0, zero = 0;
  for (size_t i = 0; i < n; i++) {
    const float *e = &f[i * lanes];
    bool finite = true;
    for (size_t k = 0; k < lanes; k++) finite = finite && std::isfinite(e[k]);
    if (!finite) { nonfinite++; continue; }
    if (prop.rfind("orientations", 0) == 0 && lanes == 4) {
      const double len = std::sqrt(double(e[0]) * e[0] + double(e[1]) * e[1] +
                                   double(e[2]) * e[2] + double(e[3]) * e[3]);
      if (len < 1e-12) zero++;
      else if (std::fabs(len - 1.0) > 1e-3) bad++;
    } else if (prop.rfind("scales", 0) == 0) {
      for (size_t k = 0; k < lanes; k++) if (!(e[k] > 0.0f)) { bad++; break; }
    } else if (prop.rfind("opacities", 0) == 0) {
      if (e[0] < 0.0f || e[0] > 1.0f) bad++;
    }
  }
  const std::string loc = MakePropertyLocation(location, prop);
  const std::string tag = where_suffix.empty() ? "" : " (" + where_suffix + ")";
  if (nonfinite) {
    AddError(result, "vol.particleField.nonFinite", loc,
             prop + tag + " has " + std::to_string(nonfinite) +
                 " elements with NaN/Inf components");
  }
  if (zero) {
    AddError(result, "vol.particleField.orientation", loc,
             prop + tag + " has " + std::to_string(zero) +
                 " zero-length quaternions (no rotation is defined)");
  }
  if (bad) {
    if (prop.rfind("orientations", 0) == 0) {
      AddWarning(result, "vol.particleField.orientation", loc,
                 prop + tag + " has " + std::to_string(bad) +
                     " non-unit quaternions; renderers normalize them");
    } else if (prop.rfind("scales", 0) == 0) {
      AddWarning(result, "vol.particleField.scale", loc,
                 prop + tag + " has " + std::to_string(bad) +
                     " particles with a non-positive scale (never visible)");
    } else {
      AddWarning(result, "vol.particleField.opacity", loc,
                 prop + tag + " has " + std::to_string(bad) +
                     " opacities outside [0, 1]; renderers clamp them");
    }
  }
}

void ValidateParticleField(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  const bool gaussian = ps.type_name() == "ParticleField3DGaussianSplat";
  if (gaussian) {
    ValidateTokenSetProperty(ps, "projectionModeHint",
                             {"perspective", "tangential"},
                             "vol.particleField.projectionModeHint",
                             prim_location, result);
    ValidateTokenSetProperty(ps, "sortingModeHint",
                             {"zDepth", "cameraDistance", "rayHitDistance"},
                             "vol.particleField.sortingModeHint",
                             prim_location, result);
  }

  const bool has_position_api =
      gaussian || HasAppliedSchema(applied_schemas,
                                   "ParticleFieldPositionAttributeAPI");
  const bool has_kernel_api =
      gaussian ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianEllipsoidAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelGaussianSurfletAPI") ||
      HasAppliedSchema(applied_schemas,
                       "ParticleFieldKernelConstantSurfletAPI");
  if (!has_position_api) {
    AddWarning(result, "vol.particleField.position", prim_location,
               "ParticleField has no applied position provider API");
  }
  if (!has_kernel_api) {
    AddWarning(result, "vol.particleField.kernel", prim_location,
               "ParticleField has no applied kernel API");
  }

  // Value checks on defaults and every time sample, and per-sample particle
  // counts: motion-blurred splats (Cycles) need a constant count per prim.
  static const char *kValueProps[] = {"positions",    "positionsh",
                                      "orientations", "orientationsh",
                                      "scales",       "scalesh",
                                      "opacities",    "opacitiesh"};
  for (const char *prop : kValueProps) {
    if (const Value *v = GetAttrValue(ps, prop)) {
      ValidateParticleValues(prop, *v, "", prim_location, result);
    }
    const auto *samples = GetTimeSamplesOf(ps, prop);
    if (!samples) continue;
    size_t first_count = 0;
    bool have_first = false, varying = false;
    for (const auto &smp : *samples) {
      const Value *sv = ps.time_sample_value(smp.second);
      if (!sv || sv->is_block() || !sv->is_array()) continue;
      ValidateParticleValues(prop, *sv, "time " + std::to_string(smp.first),
                             prim_location, result);
      if (!have_first) { first_count = sv->array_size(); have_first = true; }
      else if (sv->array_size() != first_count) varying = true;
    }
    if (varying) {
      AddWarning(result, "vol.particleField.sampleCount",
                 MakePropertyLocation(prim_location, prop),
                 std::string(prop) + " time samples have differing element "
                 "counts; motion blur interpolates only equal-count samples");
    }
  }

  size_t particle_count = 0;
  bool have_positions = GetArrayLengthProperty(ps, "positions", &particle_count);
  if (!have_positions) {
    have_positions =
        GetArrayLengthProperty(ps, "positionsh", &particle_count);
  }
  if (!have_positions) return;

  // OpenUSD discards a short auxiliary array and truncates a long one. Report
  // the authored mismatch without treating that defined fallback behavior as
  // a fatal validation error.
  for (const char *prop_name :
       {"orientations", "orientationsh", "scales", "scalesh", "opacities",
        "opacitiesh"}) {
    size_t value_count = 0;
    if (GetArrayLengthProperty(ps, prop_name, &value_count) &&
        value_count != particle_count) {
      AddWarning(result, "vol.particleField.arraySize",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has " +
                     std::to_string(value_count) + " elements for " +
                     std::to_string(particle_count) +
                     " particles; OpenUSD will " +
                     (value_count < particle_count ? "discard" : "truncate") +
                     " it");
    }
  }

  // Spherical harmonics: (degree + 1)^2 coefficients per particle.
  int64_t degree = 3;
  if (const Value *v = GetAttrValue(ps, "radiance:sphericalHarmonicsDegree")) {
    if (const int32_t *i = v->as_int()) degree = *i;
  }
  if (degree > 3) {
    AddWarning(result, "vol.particleField.shDegree",
               MakePropertyLocation(prim_location, "radiance:sphericalHarmonicsDegree"),
               "spherical harmonics degree " + std::to_string(degree) +
                   " exceeds 3; renderers evaluate at most degree 3");
  }
  if (degree < 0) {
    AddWarning(result, "vol.particleField.shDegree",
               MakePropertyLocation(prim_location, "radiance:sphericalHarmonicsDegree"),
               "spherical harmonics degree must not be negative");
    return;
  }
  const uint64_t bands = static_cast<uint64_t>(degree) + 1;
  size_t expected_coefficients = 0;
  const bool count_fits = safe::mul3(particle_count, bands, bands,
                                     &expected_coefficients);
  for (const char *prop_name : {"radiance:sphericalHarmonicsCoefficients",
                                "radiance:sphericalHarmonicsCoefficientsh"}) {
    size_t value_count = 0;
    if (GetArrayLengthProperty(ps, prop_name, &value_count) &&
        (!count_fits || value_count != expected_coefficients)) {
      AddWarning(result, "vol.particleField.shCount",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) + " has " + std::to_string(value_count) +
                     " coefficients; degree " + std::to_string(degree) + " with " +
                     std::to_string(particle_count) + " particles needs " +
                     (count_fits ? std::to_string(expected_coefficients)
                                 : "more coefficients than can be represented"));
    }
  }
}

void ValidateOpenUsd2608GeomAPIs(
    const PrimSpec &ps, const std::vector<AppliedSchema> &applied_schemas,
    const std::string &prim_location, USDValidationResult *result) {
  if (HasAppliedSchema(applied_schemas, "GeomModelAPI")) {
    ValidateTokenSetProperty(ps, "model:cardVisibility",
                             {"full", "simple", "inherited"},
                             "geom.model.cardVisibility", prim_location,
                             result);
  }
  for (const AppliedSchema &schema : applied_schemas) {
    if (schema.name != "BackPlateAPI" || schema.instance_name.empty()) continue;
    const std::string prop_name = "backPlate:" + schema.instance_name +
                                  ":plateVisibility";
    ValidateTokenSetProperty(ps, prop_name, {"all", "solo", "mute"},
                             "geom.backPlate.visibility", prim_location,
                             result);
  }
}

// pxr usdValidation:AttributeTypeMismatch for schema attributes whose
// DECLARED type must match the registry exactly. Empty means "not declared
// locally" and is fine.
void CheckDeclaredAttrType(const PrimSpec &ps, const char *prop_name,
                           const char *expected,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  const std::string authored = AttrTypeNameOf(ps, prop_name);
  if (!authored.empty() && authored != expected) {
    AddWarning(result, "core.schema.attributeType",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " is declared `" + authored +
                   "` but the schema attribute type is `" + expected + "`");
  }
}

void ValidateRenderPrim(const PrimSpec &ps, const std::string &prim_location,
                        const PrimTypeByPath &prim_types,
                        USDValidationResult *result) {
  static const std::set<std::string> kConformPolicy = {
      "expandAperture", "cropAperture", "adjustApertureWidth",
      "adjustApertureHeight", "adjustPixelAspectRatio"};
  static const std::set<std::string> kSourceType = {"raw", "primvar", "lpe",
                                                    "intrinsic"};
  static const std::set<std::string> kCameraType = {"Camera"};
  static const std::set<std::string> kProductType = {"RenderProduct"};
  static const std::set<std::string> kVarType = {"RenderVar"};
  static const std::set<std::string> kRenderSourceTypes = {
      "RenderSettings", "RenderProduct"};
  static const std::set<std::string> kPassType = {"RenderPass"};
  const std::string &type_name = ps.type_name();
  if (type_name == "RenderSettings" || type_name == "RenderProduct") {
    // Rule ids name the prim type they fire on (render.settings.* vs
    // render.product.*) so id-based filtering points at the right schema.
    const bool is_settings = type_name == "RenderSettings";
    ValidateTokenSetProperty(ps, "aspectRatioConformPolicy", kConformPolicy,
                             is_settings
                                 ? "render.settings.aspectRatioConformPolicy"
                                 : "render.product.aspectRatioConformPolicy",
                             prim_location, result);
    // Per pxr, `products` is a RenderSettings property and `orderedVars` a
    // RenderProduct property; the same name on the OTHER type is an ordinary
    // custom property and must not be kind-checked.
    const char *rels[2] = {"camera",
                           is_settings ? "products" : "orderedVars"};
    for (const char *rel : rels) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result,
                 is_settings ? "render.settings.relationship"
                             : "render.product.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    if (!is_settings) {
      CheckDeclaredAttrType(ps, "productName", "token", prim_location, result);
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "camera",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location, "camera"), prim_types, kCameraType,
        result);
    ValidateRelationshipTargetPrimTypes(
        ps, is_settings ? "products" : "orderedVars",
        is_settings ? "render.settings.targetType"
                    : "render.product.targetType",
        MakePropertyLocation(prim_location,
                             is_settings ? "products" : "orderedVars"),
        prim_types, is_settings ? kProductType : kVarType, result);
  } else if (type_name == "RenderVar") {
    ValidateTokenSetProperty(ps, "sourceType", kSourceType,
                             "render.var.sourceType", prim_location, result);
    CheckDeclaredAttrType(ps, "sourceName", "string", prim_location, result);
  } else if (type_name == "RenderPass") {
    for (const char *rel : {"renderSource", "inputPasses"}) {
      if (HasProperty(ps, rel) && !IsRelationshipProp(ps, rel)) {
        AddError(result, "render.pass.relationship",
                 MakePropertyLocation(prim_location, rel),
                 std::string(rel) + " must be a relationship");
      }
    }
    ValidateRelationshipTargetPrimTypes(
        ps, "renderSource", "render.pass.targetType",
        MakePropertyLocation(prim_location, "renderSource"), prim_types,
        kRenderSourceTypes, result);
    ValidateRelationshipTargetPrimTypes(
        ps, "inputPasses", "render.pass.targetType",
        MakePropertyLocation(prim_location, "inputPasses"), prim_types,
        kPassType, result);
  }
}

void ValidatePointInstancer(const PrimSpec &ps,
                            const std::string &prim_location,
                            USDValidationResult *result) {
  size_t position_count = 0;
  const bool have_positions =
      GetPoint3ArrayLen(ps, "positions", &position_count);

  size_t prototype_count = 0;
  if (!HasProperty(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "PointInstancer must author rel prototypes");
  } else if (!IsRelationshipProp(ps, "prototypes")) {
    AddError(result, "geom.pointInstancer.prototypes",
             MakePropertyLocation(prim_location, "prototypes"),
             "prototypes must be a relationship");
  } else {
    const std::vector<Path> targets = RelTargets(ps, "prototypes");
    prototype_count = targets.size();
    if (targets.empty()) {
      AddError(result, "geom.pointInstancer.prototypes",
               MakePropertyLocation(prim_location, "prototypes"),
               "prototypes relationship must target at least one prototype");
    }
    for (const Path &target : targets) {
      std::string err;
      if (!IsValidScenePathString(target.str(), &err)) {
        AddError(result, "geom.pointInstancer.prototypes",
                 MakePropertyLocation(prim_location, "prototypes"),
                 "prototypes target `" + target.str() +
                     "` is invalid: " + err);
      } else if (target.has_property() || target.is_root()) {
        AddError(result, "geom.pointInstancer.prototypes",
                 MakePropertyLocation(prim_location, "prototypes"),
                 "prototypes target `" + target.str() + "` must name a prim directly");
      }
    }
  }

  std::vector<int32_t> proto_indices;
  if (GetIntArrayProperty(ps, "protoIndices", &proto_indices)) {
    if (have_positions && proto_indices.size() != position_count) {
      AddError(result, "geom.pointInstancer.indices",
               MakePropertyLocation(prim_location, "protoIndices"),
               "protoIndices length must match positions length");
    }
    for (size_t i = 0; i < proto_indices.size(); i++) {
      if (proto_indices[i] < 0 ||
          static_cast<size_t>(proto_indices[i]) >= prototype_count) {
        AddError(result, "geom.pointInstancer.indices",
                 MakePropertyLocation(prim_location, "protoIndices"),
                 "protoIndices[" + std::to_string(i) +
                     "] does not reference an authored prototype");
      }
    }
  }

  if (have_positions) {
    const auto *position_samples = GetTimeSamplesOf(ps, "positions");
    const bool static_positions = !position_samples || position_samples->empty();
    const char *orientation_name =
        UsdGeomPointInstancer::UsesFloatOrientations(ps) ? "orientationsf" : "orientations";
    for (const char *prop_name :
         {orientation_name, "scales", "ids", "velocities",
          "accelerations", "angularVelocities"}) {
      ValidateArrayLengthProperty(ps, prop_name, position_count, false,
                                  "geom.pointInstancer.arraySize",
                                  prim_location, "positions", result, static_positions);
    }
  }

  for (const std::string name : {"orientations", "orientationsf"}) {
    const auto location = MakePropertyLocation(prim_location, name);
    const auto check = [&](const Value *value, std::optional<double> time) {
      if (!value || (value->type_id() != TypeId::Quath &&
                     value->type_id() != TypeId::Quatf)) return;
      ArrayScratch<float> scratch;
      ArrayView<float> lanes;
      if (!GetFloatArrayView(*value, &scratch, &lanes)) return;
      // Half-precision lanes incur larger rounding error near unit length.
      const double tolerance = value->type_id() == TypeId::Quath ? 2e-3 : 1e-5;
      for (size_t i = 0; i + 3 < lanes.size; i += 4) {
        double norm_squared = 0.0;
        for (size_t component = 0; component < 4; ++component) {
          const double lane = lanes[i + component];
          norm_squared += lane * lane;
        }
        if (!std::isfinite(norm_squared) ||
            std::abs(norm_squared - 1.0) > tolerance) {
          AddError(result, "geom.pointInstancer.orientation", location,
                   name + "[" + std::to_string(i / 4) + "] must be a finite unit quaternion" +
                       (time ? " at time " + std::to_string(*time) : ""));
        }
      }
    };
    check(GetAttrValue(ps, name), std::nullopt);
    if (const auto *samples = GetTimeSamplesOf(ps, name)) {
      for (const auto &sample : *samples) {
        check(ps.time_sample_value(sample.second), sample.first);
      }
    }
  }
}

void ValidateCamera(const PrimSpec &ps, const std::string &prim_location,
                    USDValidationResult *result) {
  static const std::set<std::string> kProjection = {"perspective",
                                                    "orthographic"};
  static const std::set<std::string> kStereoRole = {"mono", "left", "right"};

  ValidateTokenSetProperty(ps, "projection", kProjection,
                           "geom.camera.tokens", prim_location, result);
  ValidateTokenSetProperty(ps, "stereoRole", kStereoRole,
                           "geom.camera.tokens", prim_location, result);

  const auto check_clipping_range = [&](const Value *value,
                                        std::optional<double> time) {
    std::array<float, 2> clipping_range = {};
    if (!value || value->is_block() || !ValueToFloat2(*value, &clipping_range)) {
      return;
    }
    if (!std::isfinite(clipping_range[0]) ||
        !std::isfinite(clipping_range[1]) || clipping_range[0] < 0.0f ||
        clipping_range[1] <= clipping_range[0]) {
      AddError(result, "geom.camera.range",
               MakePropertyLocation(prim_location, "clippingRange"),
               std::string("clippingRange must be finite with near >= 0 and far > near") +
                   (time ? " at time " + std::to_string(*time) : ""));
    }
  };
  check_clipping_range(GetAttrValue(ps, "clippingRange"), std::nullopt);
  if (const auto *samples = GetTimeSamplesOf(ps, "clippingRange")) {
    for (const auto &sample : *samples) {
      check_clipping_range(ps.time_sample_value(sample.second), sample.first);
    }
  }

  for (const std::string name : {"focalLength", "horizontalAperture",
                                "verticalAperture", "fStop"}) {
    const bool allow_zero = name == "fStop";
    const char *rule = "geom.camera.optics";
    const std::string location = MakePropertyLocation(prim_location, name);
    if (IsRelationshipProp(ps, name)) {
      AddError(result, rule, location, name + " must be a numeric attribute");
      continue;
    }
    const auto check_optic = [&](const Value *value, std::optional<double> time) {
      if (!value || value->is_block()) return;
      double scalar = 0.0;
      if (!ValueToDouble(*value, &scalar)) {
        AddError(result, rule, location, name + " must have a numeric scalar value");
      } else if (!std::isfinite(scalar) ||
                 (allow_zero ? scalar < 0.0 : scalar <= 0.0)) {
        AddError(result, rule, location, name +
            (allow_zero ? " must be finite and non-negative" : " must be finite and positive") +
            (time ? " at time " + std::to_string(*time) : ""));
      }
    };
    check_optic(GetAttrValue(ps, name), std::nullopt);
    if (const auto *samples = GetTimeSamplesOf(ps, name)) {
      for (const auto &sample : *samples) {
        check_optic(ps.time_sample_value(sample.second), sample.first);
      }
    }
  }

  double shutter_open = 0.0;
  double shutter_close = 0.0;
  const bool have_open =
      GetNumericScalarProperty(ps, "shutter:open", &shutter_open) ||
      GetNumericScalarProperty(ps, "shutterOpen", &shutter_open);
  const bool have_close =
      GetNumericScalarProperty(ps, "shutter:close", &shutter_close) ||
      GetNumericScalarProperty(ps, "shutterClose", &shutter_close);
  if (have_open && have_close &&
      (!std::isfinite(shutter_open) || !std::isfinite(shutter_close) ||
       shutter_close < shutter_open)) {
    AddError(result, "geom.camera.shutter", prim_location,
             "camera shutter close must be finite and >= shutter open");
  }
}

void ValidateMeshTopology(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result,
                          size_t *out_face_count) {
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  const bool have_counts = GetIntArrayProperty(ps, "faceVertexCounts", &counts);
  const bool have_indices =
      GetIntArrayProperty(ps, "faceVertexIndices", &indices);

  if (out_face_count && have_counts) {
    *out_face_count = counts.size();
  }

  size_t total = 0;
  if (have_counts) {
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] <= 0) {
        AddError(result, "geom.mesh.topology.count",
                 MakePropertyLocation(prim_location, "faceVertexCounts"),
                 "faceVertexCounts[" + std::to_string(i) + "] must be "
                 "positive, but is " + std::to_string(counts[i]));
        continue;
      }
      total += static_cast<size_t>(counts[i]);
    }
  }

  if (have_counts && have_indices && total != indices.size()) {
    AddError(result, "geom.mesh.topology.size",
             MakePropertyLocation(prim_location, "faceVertexIndices"),
             "sum(faceVertexCounts) is " + std::to_string(total) +
                 " but faceVertexIndices has " +
                 std::to_string(indices.size()) + " elements");
  }

  size_t point_count = 0;
  const bool have_points = GetPoint3ArrayLen(ps, "points", &point_count);
  const bool primvar_normals = GetAttrValue(ps, "primvars:normals") != nullptr;
  const char *normals_name = primvar_normals ? "primvars:normals" : "normals";
  size_t normal_count = 0;
  if (GetPoint3ArrayLen(ps, normals_name, &normal_count) &&
      (primvar_normals || normal_count != 0)) {
    const auto *meta = ps.property_meta(normals_name);
    const std::string interpolation = meta && (meta->authored & PropMeta::kInterpolation)
        ? meta->interpolation : (primvar_normals ? "constant" : "vertex");
    const auto location = MakePropertyLocation(prim_location, normals_name);
    std::optional<size_t> expected;
    if (interpolation == "constant") {
      expected = 1;
    } else if (interpolation == "uniform" && have_counts) {
      expected = counts.size();
    } else if (interpolation == "faceVarying" && have_indices) {
      expected = indices.size();
    } else if ((interpolation == "vertex" || interpolation == "varying") && have_points) {
      expected = point_count;
    }
    if (!IsValidPrimvarInterpolation(interpolation)) {
      AddError(result, "geom.mesh.normals", location,
               "invalid normals interpolation `" + interpolation + "`");
    } else {
      bool count_matches = !expected || normal_count == *expected;
      if (primvar_normals && GetAttrValue(ps, "primvars:normals:indices")) {
        std::vector<int32_t> selectors;
        const int element_size = meta && (meta->authored & PropMeta::kElementSize)
            ? meta->elementSize : 1;
        if (element_size <= 0 ||
            !GetIntArrayProperty(ps, "primvars:normals:indices", &selectors)) {
          count_matches = false;
        } else {
          const size_t group = static_cast<size_t>(element_size);
          count_matches = !expected ||
              (*expected % group == 0 && selectors.size() == *expected / group);
          count_matches = count_matches &&
              std::all_of(selectors.begin(), selectors.end(), [&](int32_t selector) {
                return selector >= 0 && static_cast<size_t>(selector) < normal_count / group;
              });
        }
      }
      if (!count_matches)
        AddError(result, "geom.mesh.normals", location,
                 "resolved normals do not match the " + interpolation + " interpolation domain");
    }
  }
  if (have_indices && have_points) {
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 || static_cast<size_t>(indices[i]) >= point_count) {
        AddError(result, "geom.mesh.topology.index",
                 MakePropertyLocation(prim_location, "faceVertexIndices"),
                 "faceVertexIndices[" + std::to_string(i) + "] = " +
                     std::to_string(indices[i]) +
                     " is outside points range [0, " +
                     std::to_string(point_count) + ")");
      }
    }
  }
}

void ValidateGeomSubsetTopology(const PrimSpec &ps,
                                const std::string &prim_location,
                                const AncestorContext &ancestors,
                                USDValidationResult *result) {
  if (ancestors.parent_type != "Mesh" ||
      !ancestors.parent_mesh_face_count_known) {
    return;
  }

  std::string element_type = "face";
  (void)GetTokenProperty(ps, "elementType", &element_type);
  if (element_type != "face") {
    return;
  }

  std::vector<int32_t> indices;
  if (!GetIntArrayProperty(ps, "indices", &indices)) {
    return;
  }

  std::set<int32_t> seen;
  for (size_t i = 0; i < indices.size(); i++) {
    const int32_t index = indices[i];
    if (index < 0 ||
        static_cast<size_t>(index) >= ancestors.parent_mesh_face_count) {
      AddError(result, "geom.subset.indices",
               MakePropertyLocation(prim_location, "indices"),
               "indices[" + std::to_string(i) + "] = " +
                   std::to_string(index) +
                   " is outside parent face range [0, " +
                   std::to_string(ancestors.parent_mesh_face_count) + ")");
      continue;
    }
    if (!seen.insert(index).second) {
      AddWarning(result, "geom.subset.indices",
                 MakePropertyLocation(prim_location, "indices"),
                 "indices contains duplicate element " +
                     std::to_string(index));
    }
  }
}

void ValidatePrimvars(const PrimSpec &ps, const std::string &prim_location,
                      USDValidationResult *result) {
  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!IsPrimvarPropertyName(prop_name)) {
      continue;
    }
    const std::string prop_location =
        MakePropertyLocation(prim_location, prop_name);

    const PropMeta *meta = ps.property_meta(prop_name);
    if (meta && (meta->authored & PropMeta::kInterpolation) &&
        !IsValidPrimvarInterpolation(meta->interpolation)) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "invalid primvar interpolation `" + meta->interpolation + "`");
    }
    if (meta && (meta->authored & PropMeta::kElementSize) &&
        meta->elementSize <= 0) {
      AddError(result, "geom.primvar.metadata", prop_location,
               "primvar elementSize must be greater than 0");
    }

    const std::string indices_name = prop_name + ":indices";
    if (!HasProperty(ps, indices_name)) {
      continue;
    }
    const std::string indices_location =
        MakePropertyLocation(prim_location, indices_name);
    if (IsRelationshipProp(ps, indices_name)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must be an int[] attribute");
      continue;
    }
    const Value *indices_value = ps.property_value(indices_name);
    if (indices_value && indices_value->is_block()) {
      continue;
    }

    std::vector<int32_t> indices;
    if (!GetIntArrayProperty(ps, indices_name, &indices)) {
      AddError(result, "geom.primvar.indices", indices_location,
               "indexed primvar indices must have int[] values");
      continue;
    }

    size_t value_count = 0;
    const bool have_value_count =
        GetArrayLengthProperty(ps, prop_name, &value_count);
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0 ||
          (have_value_count &&
           static_cast<size_t>(indices[i]) >= value_count)) {
        std::string msg = "indices[" + std::to_string(i) + "] = " +
                          std::to_string(indices[i]);
        if (have_value_count) {
          msg += " is outside primvar value range [0, " +
                 std::to_string(value_count) + ")";
        } else {
          msg += " must be non-negative";
        }
        AddError(result, "geom.primvar.indices", indices_location, msg);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// skel rules
// ---------------------------------------------------------------------------

void ValidateSkeletonTopology(const PrimSpec &ps,
                              const std::string &prim_location,
                              USDValidationResult *result,
                              size_t *out_joint_count) {
  std::vector<std::string> joints;
  if (!GetTokenArrayProperty(ps, "joints", &joints)) {
    AddWarning(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should author uniform token[] joints");
    return;
  }

  if (out_joint_count) {
    *out_joint_count = joints.size();
  }

  if (joints.empty()) {
    AddError(result, "geom.skel.skeleton.joints",
             MakePropertyLocation(prim_location, "joints"),
             "Skeleton joints must not be empty");
    return;
  }

  std::unordered_set<std::string> seen_joints;
  std::vector<std::string> valid_joints;
  valid_joints.reserve(joints.size());
  for (size_t i = 0; i < joints.size(); i++) {
    const std::string &joint = joints[i];
    if (joint.empty() || joint[0] == '/' || EndsWith(joint, "/") ||
        joint.find("//") != std::string::npos) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "joints[" + std::to_string(i) +
                   "] must be a relative joint path, but is `" + joint + "`");
      continue;
    }
    if (!seen_joints.insert(joint).second) {
      AddError(result, "geom.skel.skeleton.joints",
               MakePropertyLocation(prim_location, "joints"),
               "duplicate joint path `" + joint + "`");
      continue;
    }

    valid_joints.push_back(joint);
  }

  // Reuse the schema topology builder. It skips intermediate path components
  // until it finds the nearest listed ancestor.
  std::vector<int> parents;
  BuildSkelTopology(valid_joints, parents, nullptr);
  size_t root_count = 0;
  for (size_t i = 0; i < parents.size(); ++i) {
    if (parents[i] < 0) {
      ++root_count;
    } else if (const size_t parent = static_cast<size_t>(parents[i]); parent >= i) {
      AddError(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "joint `" + valid_joints[i] +
                   "` must appear after its parent joint `" +
                   valid_joints[parent] + "`");
    }
  }

  if (root_count != 1) {
    AddWarning(result, "geom.skel.skeleton.topology",
               MakePropertyLocation(prim_location, "joints"),
               "Skeleton should have one root joint, but has " +
                   std::to_string(root_count));
  }

  if (HasAttributeProp(ps, "jointNames")) {
    size_t count = 0;
    if (GetArrayLengthProperty(ps, "jointNames", &count) &&
        count != joints.size()) {
      AddError(result, "geom.skel.skeleton.jointNames",
               MakePropertyLocation(prim_location, "jointNames"),
               "jointNames has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
  }

  for (const char *prop_name : {"bindTransforms", "restTransforms"}) {
    if (!HasAttributeProp(ps, prop_name)) {
      continue;
    }
    size_t count = 0;
    if (!GetArrayLengthProperty(ps, prop_name, &count)) {
      AddWarning(result, "geom.skel.skeleton.transforms",
                 MakePropertyLocation(prim_location, prop_name),
                 std::string(prop_name) +
                     " should be an authored matrix4d[] value");
      continue;
    }
    if (count != joints.size()) {
      AddError(result, "geom.skel.skeleton.transforms",
               MakePropertyLocation(prim_location, prop_name),
               std::string(prop_name) + " has " + std::to_string(count) +
                   " elements, but joints has " +
                   std::to_string(joints.size()) + " elements");
    }
    ValidateTimeSamplesProp(ps, prop_name, "geom.skel.skeleton.transforms",
                            MakePropertyLocation(prim_location, prop_name),
                            result);
  }
}

void ValidateSkelAnimation(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  std::vector<std::string> joints;
  const bool have_joints = GetTokenArrayProperty(ps, "joints", &joints);
  if (!have_joints) {
    AddWarning(result, "geom.skel.animation.joints",
               MakePropertyLocation(prim_location, "joints"),
               "SkelAnimation should author uniform token[] joints");
  }

  if (have_joints) {
    for (const char *prop_name : {"translations", "rotations", "scales"}) {
      if (!HasAttributeProp(ps, prop_name)) {
        continue;
      }
      ValidateArrayLengthAtAllSamples(
          ps, prop_name, "geom.skel.animation.transforms",
          MakePropertyLocation(prim_location, prop_name), joints.size(),
          "joints", result);
    }
  }

  std::vector<std::string> blend_shapes;
  if (GetTokenArrayProperty(ps, "blendShapes", &blend_shapes)) {
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < blend_shapes.size(); i++) {
      const std::string &name = blend_shapes[i];
      if (name.empty()) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "blendShapes[" + std::to_string(i) + "] must not be empty");
      } else if (!seen.emplace(name).second) {
        AddError(result, "geom.skel.animation.blendShapes",
                 MakePropertyLocation(prim_location, "blendShapes"),
                 "duplicate blendShape token `" + name + "`");
      }
    }

    if (HasAttributeProp(ps, "blendShapeWeights")) {
      ValidateArrayLengthAtAllSamples(
          ps, "blendShapeWeights", "geom.skel.animation.blendShapes",
          MakePropertyLocation(prim_location, "blendShapeWeights"),
          blend_shapes.size(), "blendShapes", result);
    }
  } else if (HasAttributeProp(ps, "blendShapeWeights")) {
    AddWarning(result, "geom.skel.animation.blendShapes",
               MakePropertyLocation(prim_location, "blendShapeWeights"),
               "blendShapeWeights is authored without uniform token[] "
               "blendShapes");
  }
}

void ValidateBlendShape(const PrimSpec &ps, const std::string &prim_location,
                        USDValidationResult *result) {
  size_t offset_count = 0;
  bool have_offsets = false;
  if (!HasAttributeProp(ps, "offsets")) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape must author uniform vector3f[] offsets");
  } else if (!GetArrayLengthProperty(ps, "offsets", &offset_count)) {
    AddError(result, "geom.skel.blendShape.offsets",
             MakePropertyLocation(prim_location, "offsets"),
             "BlendShape offsets must be an authored array value");
  } else {
    have_offsets = true;
    if (offset_count == 0) {
      AddWarning(result, "geom.skel.blendShape.offsets",
                 MakePropertyLocation(prim_location, "offsets"),
                 "BlendShape offsets should not be empty");
    }
  }

  if (HasAttributeProp(ps, "normalOffsets")) {
    const std::string location = MakePropertyLocation(prim_location, "normalOffsets");
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "normalOffsets", "geom.skel.blendShape.normalOffsets", location,
          offset_count, "offsets", result);
    } else {
      ValidateTimeSamplesProp(ps, "normalOffsets",
                              "geom.skel.blendShape.normalOffsets", location, result);
    }
  }

  if (HasAttributeProp(ps, "pointIndices")) {
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(
          ps, "pointIndices", "geom.skel.blendShape.pointIndices",
          MakePropertyLocation(prim_location, "pointIndices"), offset_count,
          "offsets", result);
    }

    std::vector<int32_t> indices;
    if (GetIntArrayProperty(ps, "pointIndices", &indices)) {
      std::unordered_set<int32_t> seen;
      for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] < 0) {
          AddError(result, "geom.skel.blendShape.pointIndices",
                   MakePropertyLocation(prim_location, "pointIndices"),
                   "pointIndices[" + std::to_string(i) +
                       "] must be non-negative");
        } else if (!seen.emplace(indices[i]).second) {
          AddWarning(result, "geom.skel.blendShape.pointIndices",
                     MakePropertyLocation(prim_location, "pointIndices"),
                     "duplicate pointIndices value " +
                         std::to_string(indices[i]));
        }
      }
    }
  }

  for (const PropSlot &slot : ps.properties().slots()) {
    if (slot.is_relationship()) {
      continue;
    }
    const std::string prop_name(GetPropNameTable().get(slot.name_id));
    if (!StartsWith(prop_name, "inbetweens:")) {
      continue;
    }
    const std::string location =
        MakePropertyLocation(prim_location, prop_name);
    // The companion normalOffsets attribute belongs to the inbetween but
    // only the inbetween's position offsets carry weight metadata.
    if (!EndsWith(prop_name, ":normalOffsets")) {
      const PropMeta *meta = ps.property_meta(prop_name);
      if (!meta || !(meta->authored & PropMeta::kWeight)) {
        AddError(result, "geom.skel.blendShape.inbetween", location,
                 "BlendShape inbetween `" + prop_name +
                     "` must author weight metadata");
      } else if (!std::isfinite(meta->weight)) {
        AddError(result, "geom.skel.blendShape.inbetween", location,
                 "BlendShape inbetween `" + prop_name +
                     "` has a non-finite weight");
      }
    }
    if (have_offsets) {
      ValidateArrayLengthAtAllSamples(ps, prop_name,
                                      "geom.skel.blendShape.inbetween",
                                      location, offset_count, "offsets",
                                      result);
    }
  }
}

std::vector<std::string> AllPropertyNames(const PrimSpec &ps);

void ValidateSkelBinding(const PrimSpec &ps,
                         const std::vector<AppliedSchema> &applied_schemas,
                         const std::string &prim_location,
                         const PrimTypeByPath &prim_types,
                         const AncestorContext &ancestors,
                         USDValidationResult *result) {
  bool has_binding_property = false;
  // AllPropertyNames, not slots: a relationship WITH targets (`rel
  // skel:skeleton = </Skel>`) lives only in the relationships map, so the
  // slot-only loop never saw the most common UsdSkelBinding spelling and the
  // missing-SkelBindingAPI check was silent on exactly the prims it existed
  // for (OpenUSD's SkelBindingApiAppliedValidator flags these).
  for (const std::string &name : AllPropertyNames(ps)) {
    if (StartsWith(name, "skel:") || StartsWith(name, "primvars:skel:")) {
      has_binding_property = true;
      break;
    }
  }
  const bool has_binding_api =
      HasAppliedSchema(applied_schemas, "SkelBindingAPI");
  if (has_binding_property && !has_binding_api) {
    AddError(result, "geom.skel.binding.api", prim_location,
             "SkelBinding property is authored without applying "
             "SkelBindingAPI");
  }
  if (has_binding_api && ps.type_name() != "SkelRoot" &&
      !ancestors.has_skel_root_ancestor) {
    AddError(result, "geom.skel.binding.root", prim_location,
             "SkelBindingAPI must be applied on a SkelRoot or beneath one");
  }

  if (HasProperty(ps, "skel:skeleton")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:skeleton", "geom.skel.binding.skeleton",
        MakePropertyLocation(prim_location, "skel:skeleton"), result);
    if (IsRelationshipProp(ps, "skel:skeleton")) {
      const std::vector<Path> targets = RelTargets(ps, "skel:skeleton");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "Skeleton") {
          AddWarning(result, "geom.skel.binding.skeleton",
                     MakePropertyLocation(prim_location, "skel:skeleton"),
                     "skel:skeleton target `" + target.str() +
                         "` has type `" + *target_type +
                         "`, expected Skeleton");
        }
      }
    }
  }

  if (HasProperty(ps, "skel:animationSource")) {
    ValidateRelationshipTargetPaths(
        ps, "skel:animationSource", "geom.skel.binding.animationSource",
        MakePropertyLocation(prim_location, "skel:animationSource"), result);
    if (IsRelationshipProp(ps, "skel:animationSource")) {
      const std::vector<Path> targets =
          RelTargets(ps, "skel:animationSource");
      for (const Path &target : targets) {
        std::string prim_part;
        SplitScenePathString(target.str(), &prim_part, nullptr);
        const std::string *target_type = FindPrimType(prim_types, prim_part);
        if (target_type && *target_type != "SkelAnimation") {
          AddWarning(
              result, "geom.skel.binding.animationSource",
              MakePropertyLocation(prim_location, "skel:animationSource"),
              "skel:animationSource target `" + target.str() +
                  "` has type `" + *target_type + "`, expected SkelAnimation");
        }
      }
    }
  }
}

void ValidateSkinningPrimvars(const PrimSpec &ps,
                              const std::string &prim_location,
                              std::optional<size_t> bound_joint_count,
                              const SkinningSources &sources,
                              USDValidationResult *result) {
  const PrimSpec &indices_source = sources.indices ? *sources.indices : ps;
  const PrimSpec &weights_source = sources.weights ? *sources.weights : ps;
  const PrimSpec &order_source = sources.joint_order ? *sources.joint_order : ps;
  const bool have_indices_attr =
      HasAttributeProp(indices_source, "primvars:skel:jointIndices");
  const bool have_weights_attr =
      HasAttributeProp(weights_source, "primvars:skel:jointWeights");
  if (!have_indices_attr && !have_weights_attr) {
    return;
  }

  if (!have_indices_attr || !have_weights_attr) {
    AddError(result, "geom.skel.skinning.pair", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "be authored together");
    return;
  }

  const PropMeta *indices_meta = indices_source.property_meta("primvars:skel:jointIndices");
  const PropMeta *weights_meta = weights_source.property_meta("primvars:skel:jointWeights");

  const auto validate_skin_interp = [&](const PropMeta *meta,
                                        const std::string &name) {
    const std::string location = MakePropertyLocation(prim_location, name);
    if (!meta || !(meta->authored & PropMeta::kInterpolation)) {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " must author interpolation metadata");
      return;
    }
    if (meta->interpolation != "vertex" && meta->interpolation != "constant") {
      AddError(result, "geom.skel.skinning.interpolation", location,
               name + " interpolation must be constant or vertex, but is `" +
                   meta->interpolation + "`");
    }
  };
  validate_skin_interp(indices_meta, "primvars:skel:jointIndices");
  validate_skin_interp(weights_meta, "primvars:skel:jointWeights");
  if (indices_meta && weights_meta &&
      (indices_meta->authored & PropMeta::kInterpolation) &&
      (weights_meta->authored & PropMeta::kInterpolation) &&
      indices_meta->interpolation != weights_meta->interpolation) {
    AddError(result, "geom.skel.skinning.interpolation", prim_location,
             "jointIndices and jointWeights must use the same interpolation");
  }

  const bool indices_es_ok = indices_meta &&
                             (indices_meta->authored & PropMeta::kElementSize) &&
                             indices_meta->elementSize > 0;
  const bool weights_es_ok = weights_meta &&
                             (weights_meta->authored & PropMeta::kElementSize) &&
                             weights_meta->elementSize > 0;
  if (!indices_es_ok || !weights_es_ok) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "skinning primvars must author non-zero elementSize metadata");
  } else if (indices_meta->elementSize != weights_meta->elementSize) {
    AddError(result, "geom.skel.skinning.elementSize", prim_location,
             "primvars:skel:jointIndices and primvars:skel:jointWeights must "
             "use the same elementSize");
  }

  std::vector<int32_t> joint_indices;
  std::vector<float> joint_weights;
  if (GetIntArrayProperty(indices_source, "primvars:skel:jointIndices", &joint_indices) &&
      GetFloatArrayProperty(weights_source, "primvars:skel:jointWeights",
                            &joint_weights)) {
    std::vector<int32_t> index_selectors, weight_selectors;
    const char *index_selector_name = "primvars:skel:jointIndices:indices";
    const char *weight_selector_name = "primvars:skel:jointWeights:indices";
    const bool indices_indexed =
        GetIntArrayProperty(indices_source, index_selector_name, &index_selectors);
    const bool weights_indexed =
        GetIntArrayProperty(weights_source, weight_selector_name, &weight_selectors);
    const auto group_count = [&](const char *selector, size_t values,
                                 const PropMeta *metadata, bool size_valid,
                                 bool indexed, std::vector<int32_t> &selectors)
        -> std::optional<size_t> {
      if (!size_valid) return std::nullopt;
      const size_t element_size = static_cast<size_t>(metadata->elementSize);
      if (values % element_size != 0) return std::nullopt;
      const size_t table_groups = values / element_size;
      if (!indexed) return table_groups;
      bool valid = true;
      for (int32_t index : selectors) {
        if (index < 0 || static_cast<size_t>(index) >= table_groups) {
          AddError(result, "geom.skel.skinning.selector",
                   MakePropertyLocation(prim_location, selector),
                   "influence selector " + std::to_string(index) +
                       " is outside the primvar's value table");
          valid = false;
        }
      }
      if (!valid) return std::nullopt;
      std::sort(selectors.begin(), selectors.end());
      return selectors.size();
    };
    const auto index_groups = group_count(index_selector_name, joint_indices.size(),
        indices_meta, indices_es_ok, indices_indexed, index_selectors);
    const auto weight_groups = group_count(weight_selector_name, joint_weights.size(),
        weights_meta, weights_es_ok, weights_indexed, weight_selectors);
    const bool indexed = indices_indexed || weights_indexed;
    const auto selected = [](const std::vector<int32_t> &selectors, size_t group) {
      return std::binary_search(selectors.begin(), selectors.end(), group,
          [](auto a, auto b) { return static_cast<size_t>(a) < static_cast<size_t>(b); });
    };
    if (!indexed && joint_indices.size() != joint_weights.size()) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "jointIndices has " + std::to_string(joint_indices.size()) +
                   " elements, but jointWeights has " +
                   std::to_string(joint_weights.size()) + " elements");
    } else if (indexed && index_groups && weight_groups &&
               *index_groups != *weight_groups) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "indexed jointIndices and jointWeights must resolve to the same number of influence groups");
    }
    if ((indices_es_ok &&
         joint_indices.size() % static_cast<size_t>(indices_meta->elementSize) != 0) ||
        (weights_es_ok &&
         joint_weights.size() % static_cast<size_t>(weights_meta->elementSize) != 0)) {
      AddError(result, "geom.skel.skinning.size", prim_location,
               "jointIndices and jointWeights must contain complete influence groups");
    }
    if (index_groups && weight_groups &&
        (indices_meta->authored & PropMeta::kInterpolation) &&
        (weights_meta->authored & PropMeta::kInterpolation) &&
        indices_meta->interpolation == weights_meta->interpolation) {
      size_t expected_groups = 1;
      const auto &interpolation = indices_meta->interpolation;
      const bool know_group_count = interpolation == "constant" ||
          (interpolation == "vertex" &&
           GetPoint3ArrayLen(ps, "points", &expected_groups));
      if (know_group_count &&
          (*index_groups != expected_groups || *weight_groups != expected_groups)) {
        AddError(result, "geom.skel.skinning.size", prim_location,
                 "skinning requires " + std::to_string(expected_groups) +
                     " complete influence groups for " + interpolation +
                     " interpolation");
      }
    }

    std::vector<std::string> local_joints;
    if (GetTokenArrayProperty(order_source, "skel:joints", &local_joints)) {
      bound_joint_count = local_joints.size();
    }

    for (size_t i = 0; i < joint_indices.size(); i++) {
      if (indices_indexed &&
          (!index_groups ||
           !selected(index_selectors, i / static_cast<size_t>(indices_meta->elementSize)))) {
        continue;
      }
      if (joint_indices[i] < 0) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) +
                     "] must be non-negative");
      } else if (bound_joint_count &&
                 static_cast<size_t>(joint_indices[i]) >= *bound_joint_count) {
        AddError(result, "geom.skel.skinning.index",
                 MakePropertyLocation(prim_location,
                                      "primvars:skel:jointIndices"),
                 "jointIndices[" + std::to_string(i) + "] references joint " +
                     std::to_string(joint_indices[i]) + ", but the joint order has " +
                     std::to_string(*bound_joint_count) + " joints");
      }
    }

    if (weights_meta && (weights_meta->authored & PropMeta::kElementSize)) {
      const size_t element_size =
          weights_meta->elementSize > 0
              ? static_cast<size_t>(weights_meta->elementSize)
              : 0;
      if (element_size != 0 && (joint_weights.size() % element_size) == 0) {
        for (size_t group = 0; group < joint_weights.size() / element_size;
             group++) {
          if (weights_indexed && (!weight_groups || !selected(weight_selectors, group))) {
            continue;
          }
          double sum = 0.0;
          bool all_finite = true;
          for (size_t item = 0; item < element_size; item++) {
            const float weight = joint_weights[group * element_size + item];
            if (!std::isfinite(weight)) {
              all_finite = false;
            }
            sum += static_cast<double>(weight);
          }
          if (!all_finite) {
            AddError(result, "geom.skel.skinning.weight",
                     MakePropertyLocation(prim_location,
                                          "primvars:skel:jointWeights"),
                     "jointWeights group " + std::to_string(group) +
                         " contains a non-finite value");
          } else if (sum <= 0.0) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " has no positive influence");
          } else if (std::fabs(sum - 1.0) > 1.0e-3) {
            AddWarning(result, "geom.skel.skinning.weight",
                       MakePropertyLocation(prim_location,
                                            "primvars:skel:jointWeights"),
                       "jointWeights group " + std::to_string(group) +
                           " sums to " + std::to_string(sum) +
                           " instead of 1");
          }
        }
      }
    }
  }
}


}  // namespace validation_detail
}  // namespace lightusd
