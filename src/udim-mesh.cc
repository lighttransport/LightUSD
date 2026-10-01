// SPDX-License-Identifier: Apache-2.0
#if defined(__clang__)
// Exact equality is required for authored time keys and discrete opinions.
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#include "udim-mesh.hh"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
#include "external/mapbox/earcut/earcut.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "safe-arithmetic.hh"

namespace lightusd {
namespace udim {
namespace {
using Weights = std::array<double, 3>;
using Polygon = std::vector<Weights>;
bool Fail(std::string* e, const std::string& s) {
  if (e) *e = "UDIM dense mesh: " + s;
  return false;
}
double UV(const Weights& w, const std::array<uint32_t, 3>& corners,
          const UVSet& set, size_t component) {
  double r = 0;
  for (size_t k = 0; k < 3; ++k)
    r += w[k] * set.values[size_t(corners[k]) * 2 + component];
  return r;
}
Polygon Clip(const Polygon& input, const std::array<uint32_t, 3>& corners,
             const UVSet& set, size_t component, double plane, bool lower) {
  Polygon out;
  for (size_t i = 0; i < input.size(); ++i) {
    const Weights& a = input[i];
    const Weights& b = input[(i + 1) % input.size()];
    const double da = UV(a, corners, set, component) - plane;
    const double db = UV(b, corners, set, component) - plane;
    const bool in_a = lower ? da <= 0 : da >= 0;
    const bool in_b = lower ? db <= 0 : db >= 0;
    if (in_a) out.push_back(a);
    if (in_a != in_b) {
      const double t = da / (da - db);
      Weights p;
      for (size_t k = 0; k < 3; ++k) p[k] = a[k] + t * (b[k] - a[k]);
      out.push_back(p);
    }
  }
  return out;
}
bool Active(const UVSet& set, size_t face) {
  return set.active_faces.empty() || set.active_faces[face] != 0;
}
uint32_t Owner(const UVSet& set, size_t offset, size_t n) {
  double u = 0, v = 0;
  for (size_t k = 0; k < n; ++k) {
    u += set.values[(offset + k) * 2];
    v += set.values[(offset + k) * 2 + 1];
  }
  return TileAt(float(u / double(n)), float(v / double(n)));
}
bool Crosses(const UVSet& set, size_t offset, size_t n) {
  for (size_t c = 0; c < 2; ++c) {
    double lo = set.values[offset * 2 + c], hi = lo;
    for (size_t k = 1; k < n; ++k) {
      lo = std::min(lo, set.values[(offset + k) * 2 + c]);
      hi = std::max(hi, set.values[(offset + k) * 2 + c]);
    }
    // Vertices exactly on a tile edge belong to the face interior's tile.
    if (std::ceil(hi - 1e-9) - std::floor(lo + 1e-9) > 1) return true;
  }
  return false;
}
}  // namespace
bool RemapDenseMesh(const std::vector<double>& points,
                    const std::vector<int32_t>& counts,
                    const std::vector<int32_t>& indices,
                    const std::vector<UVSet>& sets, const Options& options,
                    MeshRemap* output, std::string* error) {
  if (!output || sets.empty() || points.size() % 3)
    return Fail(error, "invalid geometry input");
  size_t total = 0;
  for (int32_t n : counts) {
    if (n < 3 || n > 16384 || !safe::add(total, size_t(n), &total))
      return Fail(error, "invalid face size");
  }
  if (total != indices.size() ||
      total > uint32_t(std::numeric_limits<int32_t>::max()))
    return Fail(error, "corner count mismatch");
  for (int32_t p : indices)
    if (p < 0 || size_t(p) >= points.size() / 3)
      return Fail(error, "point index outside array");
  for (double p : points)
    if (!std::isfinite(p)) return Fail(error, "non-finite point");
  for (const auto& set : sets) {
    if (set.values.size() != total * 2 ||
        (!set.active_faces.empty() && set.active_faces.size() != counts.size()))
      return Fail(error, "UV cardinality mismatch");
    for (size_t i = 0; i < set.values.size(); ++i) {
      const double v = set.values[i];
      if (!std::isfinite(v) || v < 0 || v > (i % 2 ? 900 : 10))
        return Fail(error, "UV outside supported UDIM domain");
    }
  }
  bool crossing = false;
  size_t offset = 0;
  for (size_t face = 0; face < counts.size(); ++face) {
    for (const auto& set : sets)
      if (Active(set, face) && Crosses(set, offset, size_t(counts[face]))) {
        if (options.cross_tile == CrossTilePolicy::Reject)
          return Fail(error,
                      "face " + std::to_string(face) +
                          " crosses tile boundaries (select split or grid)");
        crossing = true;
      }
    offset += size_t(counts[face]);
  }
  MeshRemap result;
  size_t initial_bytes;
  if (!safe::mul(total, sets.size() * 2 * sizeof(double) + sizeof(Stencil) + 16,
                 &initial_bytes) ||
      initial_bytes > options.memory_budget_bytes)
    return Fail(error, "remap exceeds working-memory limit");
  if (!crossing) {
    result.counts = counts;
    result.indices = indices;
    for (const auto& set : sets) {
      auto& data = result.uv_values[set.name];
      data.reserve(total * 2);
      offset = 0;
      for (size_t face = 0; face < counts.size(); ++face) {
        const uint32_t owner = Owner(set, offset, size_t(counts[face]));
        for (size_t k = 0; k < size_t(counts[face]); ++k) {
          auto uv =
              set.layout.remap(float(set.values[(offset + k) * 2]),
                               float(set.values[(offset + k) * 2 + 1]), owner);
          data.push_back(double(uv[0]));
          data.push_back(double(uv[1]));
        }
        offset += size_t(counts[face]);
      }
    }
    *output = std::move(result);
    return true;
  }
  result.topology_changed = true;
  const size_t bytes_per_corner =
      sizeof(Stencil) + 16 + sets.size() * 2 * sizeof(double);
  const size_t max_corners = options.memory_budget_bytes / bytes_per_corner / 2;
  offset = 0;
  for (size_t face = 0; face < counts.size(); ++face) {
    const size_t n = size_t(counts[face]);
    std::array<double, 3> normal{{0, 0, 0}};
    for (size_t k = 0; k < n; ++k) {
      const size_t a = size_t(indices[offset + k]) * 3,
                   b = size_t(indices[offset + (k + 1) % n]) * 3;
      normal[0] +=
          (points[a + 1] - points[b + 1]) * (points[a + 2] + points[b + 2]);
      normal[1] += (points[a + 2] - points[b + 2]) * (points[a] + points[b]);
      normal[2] += (points[a] - points[b]) * (points[a + 1] + points[b + 1]);
    }
    size_t drop = 0;
    for (size_t c = 1; c < 3; ++c)
      if (std::fabs(normal[c]) > std::fabs(normal[drop])) drop = c;
    std::vector<std::vector<std::array<double, 2>>> polygon(1);
    polygon[0].reserve(n);
    for (size_t k = 0; k < n; ++k) {
      const size_t p = size_t(indices[offset + k]) * 3;
      polygon[0].push_back(
          {{points[p + (drop + 1) % 3], points[p + (drop + 2) % 3]}});
    }
    const auto triangles = mapbox::earcut<uint32_t>(polygon);
    if (triangles.size() != (n - 2) * 3)
      return Fail(error, "cannot triangulate face " + std::to_string(face));
    for (size_t tri = 0; tri < triangles.size(); tri += 3) {
      std::array<uint32_t, 3> corners{{uint32_t(offset) + triangles[tri],
                                       uint32_t(offset) + triangles[tri + 1],
                                       uint32_t(offset) + triangles[tri + 2]}};
      // Preserve authored winding after projected triangulation.
      if (normal[drop] < 0) std::swap(corners[1], corners[2]);
      std::array<uint32_t, 3> source_points{{uint32_t(indices[corners[0]]),
                                             uint32_t(indices[corners[1]]),
                                             uint32_t(indices[corners[2]])}};
      std::vector<Polygon> fragments{
          Polygon{Weights{{1, 0, 0}}, Weights{{0, 1, 0}}, Weights{{0, 0, 1}}}};
      for (const auto& set : sets) {
        if (!Active(set, face)) continue;
        for (size_t c = 0; c < 2; ++c) {
          double lo = set.values[size_t(corners[0]) * 2 + c], hi = lo;
          for (size_t k = 1; k < 3; ++k) {
            lo = std::min(lo, set.values[size_t(corners[k]) * 2 + c]);
            hi = std::max(hi, set.values[size_t(corners[k]) * 2 + c]);
          }
          for (int boundary = int(std::floor(lo)) + 1;
               double(boundary) < hi - 1e-9; ++boundary) {
            std::vector<Polygon> next;
            for (const auto& fragment : fragments) {
              double a = UV(fragment[0], corners, set, c), b = a;
              for (const auto& w : fragment) {
                a = std::min(a, UV(w, corners, set, c));
                b = std::max(b, UV(w, corners, set, c));
              }
              if (a >= double(boundary) - 1e-9 || b <= double(boundary) + 1e-9)
                next.push_back(fragment);
              else {
                auto left = Clip(fragment, corners, set, c, boundary, true);
                auto right = Clip(fragment, corners, set, c, boundary, false);
                if (left.size() >= 3) next.push_back(std::move(left));
                if (right.size() >= 3) next.push_back(std::move(right));
              }
              if (next.size() > max_corners / 3)
                return Fail(error, "clipping exceeds working-memory limit");
            }
            fragments = std::move(next);
          }
        }
      }
      for (const auto& fragment : fragments) {
        for (size_t k = 1; k + 1 < fragment.size(); ++k) {
          if (result.vertices.size() > max_corners ||
              max_corners - result.vertices.size() < 3 ||
              result.vertices.size() >
                  size_t(std::numeric_limits<int32_t>::max()) - 3)
            return Fail(error, "generated geometry exceeds limit");
          const Weights triangle[] = {fragment[0], fragment[k],
                                      fragment[k + 1]};
          // Discard zero-area pieces introduced by clipping at an existing
          // edge.
          const double area = (triangle[1][1] - triangle[0][1]) *
                                  (triangle[2][2] - triangle[0][2]) -
                              (triangle[1][2] - triangle[0][2]) *
                                  (triangle[2][1] - triangle[0][1]);
          if (std::fabs(area) <= 1e-14) continue;
          result.counts.push_back(3);
          result.face_sources.push_back(uint32_t(face));
          for (const auto& w : triangle) {
            result.indices.push_back(int32_t(result.vertices.size()));
            result.vertices.push_back(
                {source_points, corners, w, uint32_t(face)});
          }
          for (const auto& set : sets) {
            Weights center;
            for (size_t j = 0; j < 3; ++j)
              center[j] =
                  (triangle[0][j] + triangle[1][j] + triangle[2][j]) / 3;
            const uint32_t owner = TileAt(float(UV(center, corners, set, 0)),
                                          float(UV(center, corners, set, 1)));
            auto& uv = result.uv_values[set.name];
            for (const auto& w : triangle) {
              auto mapped =
                  set.layout.remap(float(UV(w, corners, set, 0)),
                                   float(UV(w, corners, set, 1)), owner);
              uv.push_back(double(mapped[0]));
              uv.push_back(double(mapped[1]));
            }
          }
        }
      }
    }
    offset += n;
  }
  *output = std::move(result);
  return true;
}
bool RemapNumeric(const std::vector<double>& input, size_t components,
                  const std::string& interpolation, bool integral,
                  const MeshRemap& remap, size_t budget,
                  std::vector<double>* output, std::string* error) {
  if (!output || !components || input.size() % components)
    return Fail(error, "invalid primvar components");
  if (interpolation == "constant") {
    *output = input;
    return true;
  }
  const bool uniform = interpolation == "uniform";
  const bool vertex = interpolation == "vertex" || interpolation == "varying";
  if (!uniform && !vertex && interpolation != "faceVarying")
    return Fail(error, "unsupported interpolation: " + interpolation);
  const size_t count =
      uniform ? remap.face_sources.size() : remap.vertices.size();
  size_t size;
  if (!safe::mul(count, components, &size) || size > budget / sizeof(double))
    return Fail(error, "primvar remap exceeds memory limit");
  std::vector<double> result(size, 0);
  for (size_t i = 0; i < count; ++i) {
    for (size_t c = 0; c < components; ++c) {
      if (uniform) {
        const size_t source = size_t(remap.face_sources[i]) * components + c;
        if (source >= input.size())
          return Fail(error, "uniform primvar index outside array");
        result[i * components + c] = input[source];
      } else {
        const Stencil& stencil = remap.vertices[i];
        const auto& sources = vertex ? stencil.points : stencil.corners;
        bool have_integer = false;
        double integer_value = 0;
        for (size_t k = 0; k < 3; ++k) {
          const size_t source = size_t(sources[k]) * components + c;
          if (source >= input.size())
            return Fail(error, "primvar index outside array");
          if (integral && stencil.weights[k] > 1e-12) {
            if (have_integer && integer_value != input[source])
              return Fail(error, "cannot interpolate discrete vertex data");
            integer_value = input[source];
            have_integer = true;
          }
          result[i * components + c] += stencil.weights[k] * input[source];
        }
      }
    }
  }
  *output = std::move(result);
  return true;
}
}  // namespace udim
}  // namespace lightusd
