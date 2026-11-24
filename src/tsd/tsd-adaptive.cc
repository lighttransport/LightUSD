// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// tinysubdiv: adaptive (selective) bilinear refinement. See tinysubdiv.hh.
//
// Each round extracts the marked frontier faces into a compact sub-mesh,
// refines it one level with the bilinear scheme (Refine), and replaces the
// marked faces by their children. Faces are never shared across rounds, so
// T-junctions between levels are harmless.

#include <cstring>
#include <string>
#include <vector>

#include "tinysubdiv.hh"

namespace lightusd {
namespace tsd {

namespace {

void SetErr(std::string *err, const char *msg) {
  if (err) *err = msg;
}

void BuildOffsets(AdaptiveMesh *m) {
  m->face_offsets.resize(m->face_vertex_counts.size());
  uint32_t o = 0;
  for (size_t f = 0; f < m->face_vertex_counts.size(); ++f) {
    m->face_offsets[f] = o;
    o += m->face_vertex_counts[f];
  }
}

// Drop points no face references and renumber indices.
void CompactPoints(AdaptiveMesh *m) {
  const uint32_t np = uint32_t(m->points.size() / 3);
  std::vector<uint32_t> remap(np, UINT32_MAX);
  std::vector<float> pts;
  pts.reserve(m->points.size());
  for (uint32_t &v : m->face_vertex_indices) {
    if (remap[v] == UINT32_MAX) {
      remap[v] = uint32_t(pts.size() / 3);
      pts.insert(pts.end(), &m->points[size_t(v) * 3],
                 &m->points[size_t(v) * 3] + 3);
    }
    v = remap[v];
  }
  m->points.swap(pts);
}

}  // namespace

Result RefineAdaptive(const MeshView &base_mesh, const AdaptiveOptions &options,
                      AdaptiveMarkFn mark, void *mark_user, AdaptiveMesh *out,
                      std::string *err) {
  if (!out || !mark) {
    SetErr(err, "RefineAdaptive: null output or mark callback");
    return Result::InvalidArgument;
  }
  if (options.max_level < 0 || options.max_level > kMaxAdaptiveLevel) {
    SetErr(err, "RefineAdaptive: max_level out of range");
    return Result::InvalidArgument;
  }
  Options ro;
  ro.scheme = Scheme::Bilinear;
  ro.parallel_for = options.parallel_for;
  ro.parallel_for_user = options.parallel_for_user;
  // One bilinear level of the marked subset never exceeds 4 children per face
  // (for quads; n per n-gon), so derive Refine's caps from max_faces instead of
  // its 16M defaults, which large adaptive runs would otherwise hit.
  {
    const uint64_t cap = uint64_t(options.max_faces) * 4u + 64u;
    const uint32_t c32 = cap > 0xffffffffull ? 0xffffffffu : uint32_t(cap);
    ro.max_faces = c32;
    ro.max_vertices = c32;
    ro.max_face_vertex_indices = c32;
  }

  // Level 0: validated passthrough of the base mesh.
  RefinedMesh base;
  ro.level = 0;
  Result r = Refine(base_mesh, ro, &base, err);
  if (r != Result::Success) return r;
  if (base.face_vertex_counts.size() > options.max_faces) {
    SetErr(err, "RefineAdaptive: base mesh exceeds max_faces");
    return Result::LimitExceeded;
  }
  AdaptiveMesh &m = *out;
  m.points = base.points;
  m.face_vertex_counts = base.face_vertex_counts;
  m.face_vertex_indices = base.face_vertex_indices;
  m.face_source = base.face_source;
  m.face_level.assign(m.face_vertex_counts.size(), 0);
  m.face_parent.assign(m.face_vertex_counts.size(), UINT32_MAX);
  BuildOffsets(&m);

  uint32_t first = 0, count = uint32_t(m.face_vertex_counts.size());
  std::vector<uint8_t> sel;
  ro.level = 1;
  for (int32_t level = 0; count > 0; ++level) {
    sel.assign(count, 0);
    if (!mark(mark_user, level, m, first, count, sel.data())) {
      SetErr(err, "RefineAdaptive: aborted by mark callback");
      return Result::InvalidArgument;
    }
    if (level >= options.max_level) break;  // marks at the cap are ignored
    // Extract the marked faces into a compact sub-mesh.
    std::vector<uint32_t> sub_counts, sub_fvi, sub_parent;
    std::vector<float> sub_pts;
    std::vector<uint32_t> remap(m.points.size() / 3, UINT32_MAX);
    for (uint32_t i = 0; i < count; ++i) {
      if (!sel[i]) continue;
      const uint32_t f = first + i;
      sub_parent.push_back(f);
      sub_counts.push_back(m.face_vertex_counts[f]);
      for (uint32_t k = 0; k < m.face_vertex_counts[f]; ++k) {
        const uint32_t v = m.face_vertex_indices[m.face_offsets[f] + k];
        if (remap[v] == UINT32_MAX) {
          remap[v] = uint32_t(sub_pts.size() / 3);
          sub_pts.insert(sub_pts.end(), &m.points[size_t(v) * 3],
                         &m.points[size_t(v) * 3] + 3);
        }
        sub_fvi.push_back(remap[v]);
      }
    }
    if (sub_parent.empty()) break;
    // Every split face yields one child per corner.
    uint64_t leaves = uint64_t(m.face_vertex_counts.size()) - sub_parent.size();
    for (uint32_t c : sub_counts) leaves += c;
    if (leaves > options.max_faces) {
      SetErr(err, "RefineAdaptive: max_faces exceeded");
      return Result::LimitExceeded;
    }
    MeshView sv;
    sv.points = sub_pts.data();
    sv.num_points = uint32_t(sub_pts.size() / 3);
    sv.face_vertex_counts = sub_counts.data();
    sv.num_faces = uint32_t(sub_counts.size());
    sv.face_vertex_indices = sub_fvi.data();
    sv.num_face_vertex_indices = uint32_t(sub_fvi.size());
    RefinedMesh child;
    r = Refine(sv, ro, &child, err);
    if (r != Result::Success) return r;

    // Rebuild: unmarked faces keep their order, children are appended (the
    // next frontier).
    AdaptiveMesh next;
    next.points = m.points;
    const uint32_t nf = uint32_t(m.face_vertex_counts.size());
    for (uint32_t f = 0; f < nf; ++f) {
      if (f >= first && f < first + count && sel[f - first]) continue;
      next.face_vertex_counts.push_back(m.face_vertex_counts[f]);
      next.face_vertex_indices.insert(
          next.face_vertex_indices.end(),
          m.face_vertex_indices.begin() + m.face_offsets[f],
          m.face_vertex_indices.begin() + m.face_offsets[f] +
              m.face_vertex_counts[f]);
      next.face_source.push_back(m.face_source[f]);
      next.face_level.push_back(m.face_level[f]);
      next.face_parent.push_back(m.face_parent[f]);
    }
    const uint32_t new_first = uint32_t(next.face_vertex_counts.size());
    const uint32_t pbase = uint32_t(next.points.size() / 3);
    next.points.insert(next.points.end(), child.points.begin(),
                       child.points.end());
    for (uint32_t v : child.face_vertex_indices)
      next.face_vertex_indices.push_back(pbase + v);
    for (size_t cf = 0; cf < child.face_vertex_counts.size(); ++cf) {
      const uint32_t parent = sub_parent[child.face_source[cf]];
      next.face_vertex_counts.push_back(child.face_vertex_counts[cf]);
      next.face_source.push_back(m.face_source[parent]);
      next.face_level.push_back(uint8_t(m.face_level[parent] + 1));
      next.face_parent.push_back(parent - first);
    }
    CompactPoints(&next);
    BuildOffsets(&next);
    m.points.swap(next.points);
    m.face_vertex_counts.swap(next.face_vertex_counts);
    m.face_vertex_indices.swap(next.face_vertex_indices);
    m.face_offsets.swap(next.face_offsets);
    m.face_source.swap(next.face_source);
    m.face_level.swap(next.face_level);
    m.face_parent.swap(next.face_parent);
    first = new_first;
    count = uint32_t(m.face_vertex_counts.size()) - new_first;
  }
  CompactPoints(&m);
  return Result::Success;
}

}  // namespace tsd
}  // namespace lightusd
