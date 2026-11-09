// SPDX-License-Identifier: Apache-2.0
// Disk-backed original-triangle recipes preserve instancing and permit regional
// refinement without composing the source again.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "scene-preview-internal.hh"
namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
struct Vertex {
  Sample value;
  float uv[2] = {};
};
struct Triangle {
  Vertex vertex[3];
  uint32_t material = 0, face = 0;
};
struct Chunk {
  uint64_t offset = 0, count = 0;
  double bounds[6] = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
};
struct Link {
  uint64_t child = 0;
  double matrix[16] = {};
};
std::string File(const Build& b, uint64_t id, const char* extension) {
  std::ostringstream name;
  name << std::hex << id;
  return ((b.source_directory.empty() ? b.directory / "prototypes"
                                      : b.source_directory) /
          (name.str() + extension))
      .string();
}
void Include(std::map<uint64_t, std::array<double, 6>>& bounds, uint64_t id,
             const double* p) {
  auto it = bounds.find(id);
  if (it == bounds.end())
    it = bounds
             .emplace(id, std::array<double, 6>{1e300, 1e300, 1e300, -1e300,
                                                -1e300, -1e300})
             .first;
  for (size_t k = 0; k < 3; ++k) {
    it->second[k] = std::min(it->second[k], p[k]);
    it->second[3 + k] = std::max(it->second[3 + k], p[k]);
  }
}
float Attr(const FloatChunked& data, Interpolation mode, uint32_t vertex,
           uint32_t corner, uint32_t face, size_t components, size_t channel,
           float fallback) {
  size_t i = 0;
  if (mode == Interpolation::Vertex || mode == Interpolation::Varying)
    i = vertex;
  else if (mode == Interpolation::FaceVarying)
    i = corner;
  else if (mode == Interpolation::Uniform)
    i = face;
  return i < data.size() / components ? data[i * components + channel]
                                      : fallback;
}
void Normalize(float* n) {
  const double len = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] +
                               double(n[2]) * n[2]);
  if (len > 1e-20)
    for (int k = 0; k < 3; ++k) n[k] = float(n[k] / len);
}
struct Placement {
  std::array<double, 16> matrix;
  double inverse[16] = {};
  bool invertible;
  explicit Placement(const double* m) : invertible(Inverse(m, inverse)) {
    std::copy(m, m + 16, matrix.begin());
  }
  Vertex operator()(const Vertex& vertex) const {
    Vertex out = vertex;
    for (int k = 0; k < 3; ++k) {
      out.value.p[k] = matrix[k] * vertex.value.p[0] +
                       matrix[4 + k] * vertex.value.p[1] +
                       matrix[8 + k] * vertex.value.p[2] + matrix[12 + k];
      if (invertible)
        out.value.n[k] = float(inverse[k * 4] * vertex.value.n[0] +
                               inverse[k * 4 + 1] * vertex.value.n[1] +
                               inverse[k * 4 + 2] * vertex.value.n[2]);
    }
    return out;
  }
};
Vertex Interpolate(const Vertex& a, const Vertex& b, double t) {
  Vertex out;
  for (int k = 0; k < 3; ++k) {
    out.value.p[k] = a.value.p[k] + t * (b.value.p[k] - a.value.p[k]);
    out.value.n[k] = float(a.value.n[k] + t * (b.value.n[k] - a.value.n[k]));
    out.value.color[k] =
        float(a.value.color[k] + t * (b.value.color[k] - a.value.color[k]));
  }
  out.value.opacity =
      float(a.value.opacity + t * (b.value.opacity - a.value.opacity));
  for (int k = 0; k < 2; ++k)
    out.uv[k] = float(a.uv[k] + t * (b.uv[k] - a.uv[k]));
  return out;
}
}  // namespace
bool Build::source_mesh(
    const usd::Stage& stage, const usd::UsdPrim& prim, const RenderMesh& mesh,
    const double* world,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors) {
  std::vector<std::pair<std::string, std::set<int32_t>>> subsets;
  usd::AttributeEval eval(&stage);
  eval.SetTime(opts.time_code);
  for (const auto& child : prim.GetChildren())
    if (child.GetTypeName() == "GeomSubset") {
      auto values = eval.Eval(child, "indices");
      if (!values.success) continue;
      const auto indices = values.value.view().as_int_array();
      std::set<int32_t> faces;
      for (size_t i = 0; i < indices.size(); ++i) faces.insert(indices[i]);
      subsets.emplace_back(binding(child), std::move(faces));
    }
  struct Destination {
    uint64_t id;
    std::ofstream file;
    std::unique_ptr<Placement> placement;
    Chunk chunk;
  };
  std::vector<Destination> destinations;
  auto targets = ancestors;
  bool physical = true;
  for (const auto& a : ancestors)
    if (point_prototypes.count(a.first)) physical = false;
  if (source_capture && physical) targets.emplace_back("", Identity());
  for (const auto& ancestor : targets) {
    Destination d;
    d.id = ancestor.first.empty() ? 0 : Hash(ancestor.first);
    double matrix[16];
    Multiply(ancestor.second.data(), world, matrix);
    d.placement = std::make_unique<Placement>(matrix);
    d.file.open(File(*this, d.id, ".triangles"),
                std::ios::binary | std::ios::app);
    if (!d.file) {
      result.error = "cannot open source triangle spill";
      return false;
    }
    d.chunk.offset = static_cast<uint64_t>(d.file.tellp());
    destinations.push_back(std::move(d));
  }
  size_t face = 0;
  const std::string default_material = binding(prim);
  for (size_t ti = 0; ti < mesh.triangulated_indices.size() / 3; ++ti) {
    while (face + 1 < mesh.face_triangle_offsets.size() &&
           mesh.face_triangle_offsets[face + 1] <= ti)
      ++face;
    std::string material = default_material;
    for (const auto& subset : subsets)
      if (subset.second.count(static_cast<int32_t>(face))) {
        material = subset.first;
        break;
      }
    auto id = source_material_ids.find(material);
    if (id == source_material_ids.end()) {
      if (source_material_paths.size() >= UINT32_MAX) {
        result.error = "too many source materials";
        return false;
      }
      id = source_material_ids
               .emplace(material,
                        static_cast<uint32_t>(source_material_paths.size()))
               .first;
      source_material_paths.push_back(material);
    }
    Triangle triangle;
    triangle.material = id->second;
    triangle.face = static_cast<uint32_t>(face);
    uint32_t vi[3], ci[3];
    double p[3][3];
    for (size_t c = 0; c < 3; ++c) {
      vi[c] = mesh.triangulated_indices[ti * 3 + c];
      ci[c] = mesh.triangulated_face_vertex_indices.empty()
                  ? vi[c]
                  : mesh.triangulated_face_vertex_indices[ti * 3 + c];
      if (vi[c] >= mesh.point_count()) {
        result.error = "source triangle index out of bounds";
        return false;
      }
      for (size_t k = 0; k < 3; ++k)
        p[c][k] = mesh.points[size_t(vi[c]) * 3 + k];
    }
    double a[3], b[3];
    for (int k = 0; k < 3; ++k) {
      a[k] = p[1][k] - p[0][k];
      b[k] = p[2][k] - p[0][k];
    }
    const double n[] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                        a[0] * b[1] - a[1] * b[0]};
    const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!std::isfinite(length) || length < 1e-20) continue;
    const float normal[] = {float(n[0] / length), float(n[1] / length),
                            float(n[2] / length)};
    for (size_t c = 0; c < 3; ++c) {
      auto& vertex = triangle.vertex[c];
      for (size_t k = 0; k < 3; ++k) {
        vertex.value.p[k] = p[c][k];
        vertex.value.n[k] = Attr(mesh.normals, mesh.normals_interp, vi[c],
                                 ci[c], uint32_t(face), 3, k, normal[k]);
        vertex.value.color[k] = Attr(mesh.colors, mesh.colors_interp, vi[c],
                                     ci[c], uint32_t(face), 3, k, 1);
      }
      for (size_t k = 0; k < 2; ++k)
        vertex.uv[k] = Attr(mesh.texcoords_0, mesh.texcoords_0_interp, vi[c],
                            ci[c], uint32_t(face), 2, k, 0);
      vertex.value.opacity = Attr(mesh.opacities, mesh.opacities_interp, vi[c],
                                  ci[c], uint32_t(face), 1, 0, 1);
    }
    for (auto& destination : destinations) {
      Triangle local = triangle;
      for (size_t c = 0; c < 3; ++c) {
        local.vertex[c] = (*destination.placement)(triangle.vertex[c]);
        Include(source_bounds, destination.id, local.vertex[c].value.p);
        for (size_t k = 0; k < 3; ++k) {
          destination.chunk.bounds[k] =
              std::min(destination.chunk.bounds[k], local.vertex[c].value.p[k]);
          destination.chunk.bounds[k + 3] = std::max(
              destination.chunk.bounds[k + 3], local.vertex[c].value.p[k]);
        }
      }
      ++destination.chunk.count;
      destination.file.write(reinterpret_cast<const char*>(&local),
                             sizeof(local));
      if (!destination.file) {
        result.error = "source triangle spill write failed";
        return false;
      }
    }
    if ((ti & 4095) == 0 && !check("source-triangles", ti)) return false;
  }
  for (auto& d : destinations)
    if (d.chunk.count) {
      std::ofstream chunks(File(*this, d.id, ".chunks"),
                           std::ios::binary | std::ios::app);
      chunks.write(reinterpret_cast<const char*>(&d.chunk), sizeof(Chunk));
      if (!chunks) {
        result.error = "cannot write source spatial chunks";
        return false;
      }
    }
  return true;
}
bool Build::source_placement(
    const std::string& key, const double* world,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors) {
  const uint64_t id = Hash(key);
  if (ancestors.empty() && !source_capture) return source_replay(id, world);
  auto targets = ancestors;
  bool physical = true;
  for (const auto& a : ancestors)
    if (point_prototypes.count(a.first)) physical = false;
  if (source_capture && physical) targets.emplace_back("", Identity());
  for (const auto& parent : targets) {
    Link link;
    link.child = id;
    Multiply(parent.second.data(), world, link.matrix);
    const uint64_t parent_id = parent.first.empty() ? 0 : Hash(parent.first);
    std::ofstream output(File(*this, parent_id, ".links"),
                         std::ios::binary | std::ios::app);
    output.write(reinterpret_cast<const char*>(&link), sizeof(link));
    if (!output) {
      result.error = "source placement spill failed";
      return false;
    }
    const auto bounds = source_bounds.find(id);
    if (bounds != source_bounds.end()) {
      const Placement placement(link.matrix);
      for (int c = 0; c < 8; ++c) {
        Vertex v;
        for (size_t k = 0; k < 3; ++k)
          v.value.p[k] = bounds->second[k + ((c & (1 << k)) ? 3 : 0)];
        v = placement(v);
        Include(source_bounds, parent_id, v.value.p);
      }
    }
  }
  for (const auto& parent : ancestors)
    if (point_prototypes.count(parent.first)) return true;
  return !opts.region_enabled || source_replay(id, world);
}
bool Build::source_replay(uint64_t id, const double* world, size_t depth) {
  if (depth > 1024) {
    result.error = "source prototype nesting exceeded";
    return false;
  }
  const bool gpu_geometry = geometry_backend();
  if (!result.error.empty()) return false;
  const Placement placement(world);
  auto intersects = [&](const double* bounds) {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (int c = 0; c < 8; ++c) {
      Vertex v;
      for (size_t k = 0; k < 3; ++k)
        v.value.p[k] = bounds[k + ((c & (1 << k)) ? 3 : 0)];
      v = placement(v);
      for (size_t k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], v.value.p[k]);
        hi[k] = std::max(hi[k], v.value.p[k]);
      }
    }
    for (size_t k = 0; k < 3; ++k)
      if (hi[k] < opts.region_min[k] || lo[k] > opts.region_max[k])
        return false;
    return true;
  };
  const auto bounds = source_bounds.find(id);
  if (bounds != source_bounds.end() && !intersects(bounds->second.data()))
    return true;
  const auto triangle_file = File(*this, id, ".triangles");
  std::ifstream triangles(triangle_file, std::ios::binary);
  Triangle triangle;
  uint64_t count = 0, sample_count = 0;
  RenderMesh shade_mesh;
  std::ifstream chunks(File(*this, id, ".chunks"), std::ios::binary);
  Chunk chunk;
  uint64_t remaining = 0, visited_chunks = 0;
  const bool chunked = chunks.is_open();
  auto next_triangle = [&]() {
    if (chunked) {
      while (!remaining) {
        if (!chunks.read(reinterpret_cast<char*>(&chunk), sizeof(chunk)))
          return false;
        if ((++visited_chunks & 4095) == 0 &&
            !check("source-index-chunks", visited_chunks))
          return false;
        if (!intersects(chunk.bounds)) continue;
        triangles.seekg(static_cast<std::streamoff>(chunk.offset));
        remaining = chunk.count;
      }
      --remaining;
    }
    return bool(
        triangles.read(reinterpret_cast<char*>(&triangle), sizeof(triangle)));
  };
  while (next_triangle()) {
    if (triangle.material >= source_material_paths.size()) {
      result.error = "invalid source material index";
      return false;
    }
    std::vector<Vertex> polygon;
    for (const auto& vertex : triangle.vertex)
      polygon.push_back(placement(vertex));
    for (size_t axis = 0; axis < 3; ++axis)
      for (int side = 0; side < 2; ++side) {
        std::vector<Vertex> clipped;
        auto distance = [&](const Vertex& v) {
          return side ? opts.region_max[axis] - v.value.p[axis]
                      : v.value.p[axis] - opts.region_min[axis];
        };
        for (size_t i = 0; i < polygon.size(); ++i) {
          const auto& a = polygon[i];
          const auto& b = polygon[(i + 1) % polygon.size()];
          const double da = distance(a), db = distance(b);
          if (da >= 0) clipped.push_back(a);
          if ((da >= 0) != (db >= 0))
            clipped.push_back(Interpolate(a, b, da / (da - db)));
        }
        polygon.swap(clipped);
      }
    for (size_t patch = 1; patch + 1 < polygon.size(); ++patch) {
      const Vertex vertices[] = {polygon[0], polygon[patch],
                                 polygon[patch + 1]};
      double a[3], b[3], n[3];
      for (int k = 0; k < 3; ++k) {
        a[k] = vertices[1].value.p[k] - vertices[0].value.p[k];
        b[k] = vertices[2].value.p[k] - vertices[0].value.p[k];
      }
      n[0] = a[1] * b[2] - a[2] * b[1];
      n[1] = a[2] * b[0] - a[0] * b[2];
      n[2] = a[0] * b[1] - a[1] * b[0];
      const double area =
          .5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (!std::isfinite(area) || area < 1e-24) continue;
      const size_t samples = static_cast<size_t>(std::max(
          1., std::min(65536., std::ceil(area / (cell_size * cell_size)))));
      if (gpu_geometry) {
        Sample points[3];
        float uv[6] = {};
        double origin[3];
        std::copy(vertices[0].value.p, vertices[0].value.p + 3, origin);
        for (size_t c = 0; c < 3; ++c) {
          points[c] = vertices[c].value;
          for (size_t k = 0; k < 3; ++k) points[c].p[k] -= origin[k];
          for (size_t k = 0; k < 2; ++k) uv[c * 2 + k] = vertices[c].uv[k];
        }
        const std::vector<std::pair<std::string, std::array<double, 16>>>
            ancestors;
        if (!queue_geometry(
                points, uv, samples,
                float(std::sqrt(area / (3.141592653589793 * samples)) * 1.5),
                source_material_paths[triangle.material], triangle.face,
                ancestors, origin))
          return false;
        continue;
      }
      for (size_t j = 0; j < samples; ++j) {
        const double u = std::sqrt((j + .5) / samples),
                     v = std::fmod((j + .5) * .6180339887498949, 1.);
        const double w[] = {1 - u, u * (1 - v), u * v};
        Sample sample;
        float uv[2] = {}, display[3] = {};
        sample.opacity = 0;
        for (size_t k = 0; k < 3; ++k) {
          sample.p[k] = 0;
          sample.n[k] = 0;
          for (size_t c = 0; c < 3; ++c) {
            sample.p[k] += w[c] * vertices[c].value.p[k];
            sample.n[k] += float(w[c] * vertices[c].value.n[k]);
            display[k] += float(w[c] * vertices[c].value.color[k]);
          }
        }
        for (size_t c = 0; c < 3; ++c) {
          for (size_t k = 0; k < 2; ++k)
            uv[k] += float(w[c] * vertices[c].uv[k]);
          sample.opacity += float(w[c] * vertices[c].value.opacity);
        }
        Normalize(sample.n);
        const auto color = shade(source_material_paths[triangle.material],
                                 shade_mesh, triangle.face, uv);
        for (size_t k = 0; k < 3; ++k) sample.color[k] = color[k] * display[k];
        sample.opacity *= color[3];
        sample.radius =
            float(std::sqrt(area / (3.141592653589793 * samples)) * 1.5);
        if (!insert(sample)) return false;
        if ((++sample_count & 4095) == 0 &&
            !check("source-samples", sample_count))
          return false;
      }
    }
    if ((++count & 4095) == 0 && !check("source-replay", count)) return false;
  }
  if (!result.error.empty()) return false;
  if ((chunked && (remaining || chunks.gcount() != 0 || !chunks.eof())) ||
      (!chunked && (triangles.gcount() != 0 ||
                    (triangles.is_open() && !triangles.eof())))) {
    result.error = "truncated source triangle spill";
    return false;
  }
  std::ifstream links(File(*this, id, ".links"), std::ios::binary);
  Link link;
  uint64_t visited_links = 0;
  while (links.read(reinterpret_cast<char*>(&link), sizeof(link))) {
    if ((++visited_links & 4095) == 0 &&
        !check("source-index-links", visited_links))
      return false;
    double placed[16];
    Multiply(world, link.matrix, placed);
    if (!source_replay(link.child, placed, depth + 1)) return false;
  }
  if (links.gcount() != 0 || (links.is_open() && !links.eof())) {
    result.error = "truncated source placement spill";
    return false;
  }
  return check("source-region", count);
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
