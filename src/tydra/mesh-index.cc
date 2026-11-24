// SPDX-License-Identifier: Apache-2.0
#include "mesh-index.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/safe-arithmetic.hh"

namespace lightusd {
namespace tydra {
namespace {
struct Corner {
  uint32_t point, source_corner, source_face;
};
struct Channel {
  enum class Kind { Float, Int, UInt };
  Kind kind = Kind::Float;
  size_t components = 1;
  Interpolation interpolation = Interpolation::Vertex;
  bool triangle_order = false;
  const FloatChunked* floats = nullptr;
  FloatChunked* output_floats = nullptr;
  const Int32Chunked* ints = nullptr;
  Int32Chunked* output_ints = nullptr;
  const UInt32Chunked* uints = nullptr;
  UInt32Chunked* output_uints = nullptr;
  const UInt32Chunked* indices = nullptr;
  size_t size() const {
    return kind == Kind::Float ? floats->size()
           : kind == Kind::Int ? ints->size()
                               : uints->size();
  }
  bool index(const Corner& corner, size_t render_corner, size_t* result) const {
    size_t i = 0;
    switch (interpolation) {
      case Interpolation::Constant:
        break;
      case Interpolation::Uniform:
        i = corner.source_face;
        break;
      case Interpolation::FaceVarying:
        i = triangle_order ? render_corner : corner.source_corner;
        break;
      case Interpolation::Vertex:
      case Interpolation::Varying:
        i = corner.point;
        break;
      default:
        return false;
    }
    if (indices && !indices->empty()) {
      if (i >= indices->size()) return false;
      i = (*indices)[i];
    }
    if (!components || i >= size() / components) return false;
    *result = i * components;
    return true;
  }
  bool equal(const Corner& a, size_t ai, const Corner& b, size_t bi,
             float epsilon) const {
    size_t x = 0, y = 0;
    if (!index(a, ai, &x) || !index(b, bi, &y)) return false;
    for (size_t c = 0; c < components; c++) {
      if (kind == Kind::Float) {
        const float av = (*floats)[x + c], bv = (*floats)[y + c];
        if (!std::isfinite(av) || !std::isfinite(bv) ||
            std::fabs(av - bv) > epsilon)
          return false;
      } else if (kind == Kind::Int) {
        if ((*ints)[x + c] != (*ints)[y + c]) return false;
      } else if ((*uints)[x + c] != (*uints)[y + c])
        return false;
    }
    return true;
  }
  bool append(const Corner& corner, size_t render_corner) const {
    size_t i = 0;
    if (!index(corner, render_corner, &i)) return false;
    for (size_t c = 0; c < components; c++) {
      if (kind == Kind::Float) {
        const float v = (*floats)[i + c];
        if (!std::isfinite(v)) return false;
        output_floats->push_back(v);
      } else if (kind == Kind::Int)
        output_ints->push_back((*ints)[i + c]);
      else
        output_uints->push_back((*uints)[i + c]);
    }
    return kind == Kind::Float ? !output_floats->alloc_failed()
           : kind == Kind::Int ? !output_ints->alloc_failed()
                               : !output_uints->alloc_failed();
  }
};
size_t Components(VertexFormat format) {
  switch (format) {
    case VertexFormat::Float:
    case VertexFormat::Int:
    case VertexFormat::UInt:
      return 1;
    case VertexFormat::Vec2:
    case VertexFormat::IVec2:
    case VertexFormat::UVec2:
      return 2;
    case VertexFormat::Vec3:
    case VertexFormat::IVec3:
    case VertexFormat::UVec3:
      return 3;
    case VertexFormat::Vec4:
    case VertexFormat::IVec4:
    case VertexFormat::UVec4:
      return 4;
    case VertexFormat::Matrix33:
      return 9;
    case VertexFormat::Matrix44:
      return 16;
  }
  return 0;
}
}  // namespace
bool BuildVertexIndices(RenderMesh* mesh, const MeshIndexOptions& options,
                        std::string* error) {
  const auto fail = [&](const char* text) {
    if (error) *error = text;
    return false;
  };
  if (!mesh || !std::isfinite(options.epsilon) || options.epsilon < 0 ||
      !options.max_candidates_per_point || !options.memory_budget_bytes)
    return fail("Invalid mesh index options");
  if (mesh->points.size() % 3) return fail("Incomplete point triples");
  const bool triangles = !mesh->triangulated_indices.empty();
  const auto& indices =
      triangles ? mesh->triangulated_indices : mesh->face_vertex_indices;
  const size_t count = indices.size();
  if (!count) {
    for (uint32_t corners : mesh->face_vertex_counts)
      if (corners) return fail("Face counts without corner indices");
    return true;
  }
  if (count > std::numeric_limits<uint32_t>::max() || (triangles && count % 3))
    return fail("Invalid or excessive corner count");
  size_t components = 3, channel_count = 0;
  const auto add = [&](size_t values) {
    if (values) {
      channel_count++;
      return true;
    }
    return false;
  };
  if (add(mesh->normals.size())) components += 3;
  if (add(mesh->tangents.size())) components += 4;
  if (add(mesh->texcoords_0.size())) components += 2;
  if (add(mesh->texcoords_1.size())) components += 2;
  if (add(mesh->colors.size())) components += 4;
  if (add(mesh->opacities.size())) components++;
  for (const auto& p : mesh->primvars) {
    const size_t n = Components(p.format);
    if (!n) return fail("Invalid primvar format");
    if (!safe::add(components, n, &components))
      return fail("Attribute size overflow");
    channel_count++;
  }
  size_t per_vertex = 0;
  if (!safe::mul(components, size_t(4), &per_vertex))
    return fail("Attribute size overflow");
  if (mesh->skin) {
    size_t bytes = 0;
    if (!safe::mul(size_t(mesh->skin->influences_per_vertex), size_t(6),
                   &bytes) ||
        !safe::add(per_vertex, bytes, &per_vertex))
      return fail("Skin size overflow");
    channel_count += 2;
  }
  for (const auto& shape : mesh->blend_shapes) {
    size_t n = shape.normal_offsets.empty() ? 3 : 6, extra = 0;
    if (!safe::mul(shape.inbetweens.size(), size_t(3), &extra) ||
        !safe::add(n, extra, &n) || !safe::mul(n, size_t(4), &extra) ||
        !safe::add(per_vertex, extra, &per_vertex) ||
        !safe::add(per_vertex, size_t(4), &per_vertex))
      return fail("Morph size overflow");
    channel_count += 3 + shape.inbetweens.size();
  }
  // Include worst-case output, temporary lookup/source maps, source-corner
  // tables and ChunkedArray allocation slack. Refuse before any large copy.
  size_t estimate = 0, scratch = 0, slack = 0;
  if (!safe::add(per_vertex, size_t(256), &per_vertex) ||
      !safe::mul(count, per_vertex, &estimate) ||
      !safe::mul(mesh->face_vertex_indices.size(), size_t(12), &scratch) ||
      !safe::add(estimate, scratch, &estimate) ||
      !safe::mul(channel_count + 8, size_t(128) * 1024, &slack) ||
      !safe::add(estimate, slack, &estimate) ||
      !safe::add(estimate, mesh->memory_usage(), &estimate) ||
      estimate > options.memory_budget_bytes)
    return fail("Vertex indexing exceeds memory budget");
  for (const auto& shape : mesh->blend_shapes) {
    size_t sparse_bytes = 0;
    if (!safe::mul(shape.point_indices.size(), size_t(64), &sparse_bytes) ||
        !safe::add(estimate, sparse_bytes, &estimate) ||
        estimate > options.memory_budget_bytes)
      return fail("Sparse morph indexing exceeds memory budget");
  }
  if (triangles && !mesh->face_triangle_offsets.empty()) {
    const auto& ranges = mesh->face_triangle_offsets;
    if (ranges.size() != mesh->face_count() + 1 || ranges.front() != 0 ||
        ranges.back() != count / 3)
      return fail("Invalid triangle face mapping");
    for (size_t i = 1; i < ranges.size(); i++)
      if (ranges[i] < ranges[i - 1] || ranges[i] > count / 3)
        return fail("Invalid triangle face mapping");
  }
  for (const auto& subset : mesh->material_subsets) {
    const size_t limit = triangles && !mesh->face_triangle_offsets.empty()
                             ? count / 3
                         : mesh->face_count() ? mesh->face_count()
                                              : count / 3;
    if (subset.face_start > limit ||
        subset.face_count > limit - subset.face_start)
      return fail("Invalid material subset");
  }
  std::vector<uint32_t> original_face(mesh->face_vertex_indices.size());
  size_t offset = 0;
  for (size_t f = 0; f < mesh->face_vertex_counts.size(); f++) {
    const size_t n = mesh->face_vertex_counts[f];
    if (n < 3 || n > original_face.size() - offset)
      return fail("Invalid face counts");
    for (size_t i = 0; i < n; i++)
      original_face[offset + i] = static_cast<uint32_t>(f);
    offset += n;
  }
  if (offset != original_face.size())
    return fail("Face counts do not consume original indices");
  if (!triangles && original_face.size() != count)
    return fail("Invalid polygon corner count");
  if (triangles && !mesh->triangulated_face_vertex_indices.empty() &&
      mesh->triangulated_face_vertex_indices.size() != count)
    return fail("Invalid triangle corner mapping");
  std::vector<Corner> corners;
  corners.reserve(count);
  for (size_t i = 0; i < count; i++) {
    if (indices[i] >= mesh->point_count())
      return fail("Point index out of range");
    const uint32_t c =
        triangles && !mesh->triangulated_face_vertex_indices.empty()
            ? mesh->triangulated_face_vertex_indices[i]
            : static_cast<uint32_t>(i);
    if (!original_face.empty() && c >= original_face.size())
      return fail("Source corner out of range");
    corners.push_back({indices[i], c,
                       original_face.empty() ? static_cast<uint32_t>(i / 3)
                                             : original_face[c]});
  }
  if (triangles && !original_face.empty())
    for (size_t i = 0; i < count; i += 3)
      if (corners[i].source_face != corners[i + 1].source_face ||
          corners[i].source_face != corners[i + 2].source_face)
        return fail("Triangle corners refer to different source faces");
  RenderMesh result = *mesh;
  result.points = FloatChunked{};
  result.face_vertex_indices = UInt32Chunked{};
  result.triangulated_indices = UInt32Chunked{};
  std::vector<Channel> channels;
  const auto floating = [&](const FloatChunked& source, FloatChunked& target,
                            size_t width, Interpolation interpolation,
                            bool triangle_order = false) {
    if (source.empty()) return true;
    if (source.size() % width) return false;
    Channel c;
    c.components = width;
    c.interpolation = interpolation;
    c.triangle_order = triangle_order;
    c.floats = &source;
    c.output_floats = &target;
    target = FloatChunked{};
    channels.push_back(c);
    return true;
  };
  const auto inferred_triangle_order = [&](const FloatChunked& values,
                                           size_t width, Interpolation interp) {
    return triangles && interp == Interpolation::FaceVarying &&
           values.size() / width == count && count != original_face.size();
  };
  size_t color_elements =
      mesh->colors_interp == Interpolation::Constant      ? 1
      : mesh->colors_interp == Interpolation::Uniform     ? mesh->face_count()
      : mesh->colors_interp == Interpolation::FaceVarying ? original_face.size()
                                                          : mesh->point_count();
  const size_t color_width =
      color_elements && mesh->colors.size() / color_elements == 4 ? 4 : 3;
  if (!floating(
          mesh->normals, result.normals, 3, mesh->normals_interp,
          inferred_triangle_order(mesh->normals, 3, mesh->normals_interp)) ||
      !floating(mesh->tangents, result.tangents, 4, mesh->tangents_interp,
                mesh->tangents_triangulated ||
                    inferred_triangle_order(mesh->tangents, 4,
                                            mesh->tangents_interp)) ||
      !floating(mesh->texcoords_0, result.texcoords_0, 2,
                mesh->texcoords_0_interp,
                inferred_triangle_order(mesh->texcoords_0, 2,
                                        mesh->texcoords_0_interp)) ||
      !floating(mesh->texcoords_1, result.texcoords_1, 2,
                mesh->texcoords_1_interp,
                inferred_triangle_order(mesh->texcoords_1, 2,
                                        mesh->texcoords_1_interp)) ||
      !floating(mesh->colors, result.colors, color_width, mesh->colors_interp,
                inferred_triangle_order(mesh->colors, color_width,
                                        mesh->colors_interp)) ||
      !floating(
          mesh->opacities, result.opacities, 1, mesh->opacities_interp,
          inferred_triangle_order(mesh->opacities, 1, mesh->opacities_interp)))
    return fail("Incomplete vertex attribute");
  for (size_t i = 0; i < mesh->primvars.size(); i++) {
    const auto& source = mesh->primvars[i];
    auto& target = result.primvars[i];
    Channel c;
    c.components = Components(source.format);
    c.interpolation = source.interpolation;
    c.indices = &source.indices;
    switch (source.format) {
      case VertexFormat::Int:
      case VertexFormat::IVec2:
      case VertexFormat::IVec3:
      case VertexFormat::IVec4:
        c.kind = Channel::Kind::Int;
        c.ints = &source.int_data;
        c.output_ints = &target.int_data;
        target.int_data = Int32Chunked{};
        break;
      case VertexFormat::UInt:
      case VertexFormat::UVec2:
      case VertexFormat::UVec3:
      case VertexFormat::UVec4:
        c.kind = Channel::Kind::UInt;
        c.uints = &source.uint_data;
        c.output_uints = &target.uint_data;
        target.uint_data = UInt32Chunked{};
        break;
      default:
        c.floats = &source.float_data;
        c.output_floats = &target.float_data;
        target.float_data = FloatChunked{};
        break;
    }
    if (c.size() == 0 && !source.indices.empty())
      return fail("Indexed primvar has no values");
    if (c.size() % c.components) return fail("Incomplete custom primvar");
    target.indices = UInt32Chunked{};
    target.interpolation = Interpolation::Vertex;
    if (c.size()) channels.push_back(c);
  }
  std::unordered_map<uint32_t, std::vector<uint32_t>> buckets;
  std::vector<size_t> unique_sources;
  unique_sources.reserve(count);
  UInt32Chunked output_indices;
  for (size_t i = 0; i < count; i++) {
    const auto& corner = corners[i];
    for (const auto& c : channels) {
      size_t index = 0;
      if (!c.index(corner, i, &index))
        return fail("Vertex attribute index out of range");
    }
    uint32_t id = std::numeric_limits<uint32_t>::max();
    auto& bucket = buckets[corner.point];
    if (options.deduplicate)
      for (uint32_t candidate : bucket) {
        const size_t source = unique_sources[candidate];
        bool equal = true;
        for (const auto& c : channels)
          if (!c.equal(corner, i, corners[source], source, options.epsilon)) {
            equal = false;
            break;
          }
        if (equal) {
          id = candidate;
          break;
        }
      }
    if (id == std::numeric_limits<uint32_t>::max()) {
      id = static_cast<uint32_t>(unique_sources.size());
      unique_sources.push_back(i);
      if (options.deduplicate &&
          bucket.size() < options.max_candidates_per_point)
        bucket.push_back(id);
      for (size_t c = 0; c < 3; c++) {
        const float value = mesh->points[size_t(corner.point) * 3 + c];
        if (!std::isfinite(value)) return fail("Non-finite position");
        result.points.push_back(value);
      }
      if (result.points.alloc_failed()) return fail("Point allocation failure");
      for (const auto& c : channels)
        if (!c.append(corner, i))
          return fail("Invalid attribute or allocation failure");
    }
    output_indices.push_back(id);
  }
  if (result.has_alloc_failure() || output_indices.alloc_failed())
    return fail("Vertex indexing allocation failure");
  result.normals_interp = result.tangents_interp = result.texcoords_0_interp =
      result.texcoords_1_interp = result.colors_interp =
          result.opacities_interp = Interpolation::Vertex;
  result.tangents_triangulated = false;
  if (triangles) {
    result.face_vertex_counts = UInt32Chunked{};
    for (size_t i = 0; i < count / 3; i++)
      result.face_vertex_counts.push_back(3);
    result.face_vertex_indices = output_indices;
    result.triangulated_indices = std::move(output_indices);
    result.triangulated_face_vertex_indices = UInt32Chunked{};
    for (size_t i = 0; i < count; i++)
      result.triangulated_face_vertex_indices.push_back(
          static_cast<uint32_t>(i));
    if (mesh->face_triangle_offsets.empty() &&
        !mesh->material_subsets.empty()) {
      result.material_subsets.clear();
      for (const auto& subset : mesh->material_subsets) {
        const size_t limit =
            mesh->face_count() ? mesh->face_count() : count / 3;
        if (subset.face_start > limit ||
            subset.face_count > limit - subset.face_start)
          return fail("Invalid material subset");
        size_t start = 0;
        bool active = false;
        for (size_t f = 0; f <= count / 3; f++) {
          const size_t source = f < count / 3 ? corners[f * 3].source_face : 0;
          const bool member = f < count / 3 && source >= subset.face_start &&
                              source - subset.face_start < subset.face_count;
          if (member && !active) {
            start = f;
            active = true;
          }
          if (!member && active) {
            result.material_subsets.push_back(
                {static_cast<uint32_t>(start), static_cast<uint32_t>(f - start),
                 subset.material_id, subset.name});
            active = false;
          }
        }
      }
    }
    if (!mesh->subdivision_face_source.empty()) {
      result.subdivision_face_source.clear();
      for (size_t f = 0; f < count / 3; f++) {
        const auto source = corners[f * 3].source_face;
        if (source >= mesh->subdivision_face_source.size())
          return fail("Invalid subdivision ancestry");
        result.subdivision_face_source.push_back(
            mesh->subdivision_face_source[source]);
      }
    }
    result.face_triangle_offsets.resize(count / 3 + 1);
    for (size_t i = 0; i < result.face_triangle_offsets.size(); i++)
      result.face_triangle_offsets[i] = static_cast<uint32_t>(i);
    result.hole_faces.clear();
    result.sanitize_face_remap.clear();
    result.sanitize_dropped_faces = 0;
    result.is_triangulated = true;
    result.left_handed = false;
  } else
    result.face_vertex_indices = std::move(output_indices);
  if (mesh->skin) {
    const auto& source = *mesh->skin;
    const size_t width = source.influences_per_vertex;
    if (source.joint_indices.size() != source.joint_weights.size() ||
        (!width && !source.joint_weights.empty()) ||
        (width && (source.joint_weights.size() % width ||
                   source.joint_weights.size() / width != mesh->point_count())))
      return fail("Invalid skin data");
    result.skin = std::make_shared<RenderMesh::SkinBinding>(source);
    result.skin->joint_indices = UInt16Chunked{};
    result.skin->joint_weights = FloatChunked{};
    for (size_t index : unique_sources)
      for (size_t c = 0; c < width; c++) {
        const size_t i = size_t(corners[index].point) * width + c;
        const float weight = source.joint_weights[i];
        if (!std::isfinite(weight)) return fail("Non-finite skin weight");
        result.skin->joint_indices.push_back(source.joint_indices[i]);
        result.skin->joint_weights.push_back(weight);
      }
  }
  for (size_t b = 0; b < mesh->blend_shapes.size(); b++) {
    const auto& source = mesh->blend_shapes[b];
    auto& target = result.blend_shapes[b];
    const bool sparse = !source.point_indices.empty();
    const size_t elements =
        sparse ? source.point_indices.size() : mesh->point_count();
    if (source.point_offsets.size() / 3 != elements ||
        source.point_offsets.size() % 3 ||
        (!source.normal_offsets.empty() &&
         (source.normal_offsets.size() / 3 != elements ||
          source.normal_offsets.size() % 3)))
      return fail("Invalid morph offsets");
    for (const auto& inbetween : source.inbetweens)
      if (inbetween.point_offsets.size() != source.point_offsets.size())
        return fail("Invalid morph inbetween");
    std::unordered_map<uint32_t, size_t> sparse_map;
    if (sparse)
      for (size_t i = 0; i < source.point_indices.size(); i++) {
        if (source.point_indices[i] >= mesh->point_count() ||
            !sparse_map.emplace(source.point_indices[i], i).second)
          return fail("Invalid sparse morph index");
      }
    std::vector<uint32_t>().swap(target.point_indices);
    target.point_offsets = FloatChunked{};
    target.normal_offsets = FloatChunked{};
    for (auto& inbetween : target.inbetweens)
      inbetween.point_offsets = FloatChunked{};
    for (size_t v = 0; v < unique_sources.size(); v++) {
      const auto point = corners[unique_sources[v]].point;
      size_t slot = point;
      if (sparse) {
        const auto it = sparse_map.find(point);
        if (it == sparse_map.end()) continue;
        slot = it->second;
        target.point_indices.push_back(static_cast<uint32_t>(v));
      }
      for (size_t c = 0; c < 3; c++) {
        const float offset = source.point_offsets[slot * 3 + c];
        if (!std::isfinite(offset)) return fail("Non-finite morph offset");
        target.point_offsets.push_back(offset);
        if (!source.normal_offsets.empty()) {
          const float n = source.normal_offsets[slot * 3 + c];
          if (!std::isfinite(n)) return fail("Non-finite morph normal");
          target.normal_offsets.push_back(n);
        }
        for (size_t j = 0; j < source.inbetweens.size(); j++) {
          const float x = source.inbetweens[j].point_offsets[slot * 3 + c];
          if (!std::isfinite(x)) return fail("Non-finite morph inbetween");
          target.inbetweens[j].point_offsets.push_back(x);
        }
      }
    }
  }
  if (result.has_alloc_failure())
    return fail("Vertex indexing allocation failure");
  result.compact();
  if (result.has_alloc_failure())
    return fail("Vertex indexing compaction failure");
  *mesh = std::move(result);
  return true;
}
}  // namespace tydra
}  // namespace lightusd
