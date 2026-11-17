// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// tinysubdiv (src/tsd) feature test: analytic, golden and hardening tests.
// Runs in normal CI; no OpenSubdiv required.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "tsd/tinysubdiv.hh"
#include "tsd/tsd-internal.hh"

#include "subdiv-mesh-corpus.hh"

namespace {

int g_failures = 0;
int g_checks = 0;
const char *g_current_test = "";

#define CHECK(cond)                                                       \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__,        \
              g_current_test, #cond);                                     \
    }                                                                     \
  } while (0)

#define CHECK_MSG(cond, msg)                                              \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      fprintf(stderr, "FAIL %s:%d [%s]: %s (%s)\n", __FILE__, __LINE__,   \
              g_current_test, #cond, std::string(msg).c_str());           \
    }                                                                     \
  } while (0)

#define TEST(fn)                       \
  do {                                 \
    g_current_test = #fn;              \
    fn();                              \
  } while (0)

using lightusd::tsd::BoundaryInterpolation;
using lightusd::tsd::CreasingMethod;
using lightusd::tsd::FVarChannelView;
using lightusd::tsd::FVarLinearInterpolation;
using lightusd::tsd::MeshView;
using lightusd::tsd::Options;
using lightusd::tsd::RefinedMesh;
using lightusd::tsd::Result;
using lightusd::tsd::Scheme;
using lightusd::tsd::TriangleSubdivision;
using lightusd::tsd::VertexPrimvarView;

MeshView ToView(const corpus::Mesh &m) {
  MeshView v;
  v.points = m.points.data();
  v.num_points = uint32_t(m.points.size() / 3);
  v.face_vertex_counts = m.face_vertex_counts.data();
  v.num_faces = uint32_t(m.face_vertex_counts.size());
  v.face_vertex_indices = m.face_vertex_indices.data();
  v.num_face_vertex_indices = uint32_t(m.face_vertex_indices.size());
  if (!m.corner_indices.empty()) {
    v.corner_indices = m.corner_indices.data();
    v.num_corners = uint32_t(m.corner_indices.size());
    v.corner_sharpnesses = m.corner_sharpnesses.data();
  }
  if (!m.crease_lengths.empty()) {
    v.crease_indices = m.crease_indices.data();
    v.num_crease_indices = uint32_t(m.crease_indices.size());
    v.crease_lengths = m.crease_lengths.data();
    v.num_crease_lengths = uint32_t(m.crease_lengths.size());
    v.crease_sharpnesses = m.crease_sharpnesses.data();
    v.num_crease_sharpnesses = uint32_t(m.crease_sharpnesses.size());
  }
  if (!m.hole_indices.empty()) {
    v.hole_indices = m.hole_indices.data();
    v.num_holes = uint32_t(m.hole_indices.size());
  }
  return v;
}

// ---------------------------------------------------------------------------
// Validation / hardening
// ---------------------------------------------------------------------------

void test_scheme_none_rejected() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.scheme = Scheme::None;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK(r == Result::UnsupportedScheme);
  CHECK(!err.empty());
}

void test_null_output_rejected() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  std::string err;
  CHECK(Refine(ToView(cube), opts, nullptr, &err) ==
        Result::InvalidArgument);
}

void test_empty_mesh_rejected() {
  MeshView v;
  Options opts;
  RefinedMesh out;
  CHECK(Refine(v, opts, &out, nullptr) == Result::InvalidArgument);
}

void test_bad_level_rejected() {
  corpus::Mesh cube = corpus::Cube();
  RefinedMesh out;
  Options opts;
  opts.level = -1;
  CHECK(Refine(ToView(cube), opts, &out, nullptr) ==
        Result::InvalidArgument);
  opts.level = lightusd::tsd::kMaxLevel + 1;
  CHECK(Refine(ToView(cube), opts, &out, nullptr) ==
        Result::InvalidArgument);
}

void test_out_of_range_index_rejected() {
  corpus::Mesh cube = corpus::Cube();
  cube.face_vertex_indices[5] = 1000;  // > num_points
  Options opts;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::InvalidTopology);
}

void test_count_mismatch_rejected() {
  corpus::Mesh cube = corpus::Cube();
  cube.face_vertex_counts[0] = 5;  // sum != num indices
  Options opts;
  RefinedMesh out;
  CHECK(Refine(ToView(cube), opts, &out, nullptr) ==
        Result::InvalidTopology);
}

void test_bad_face_degree_rejected() {
  corpus::Mesh m = corpus::SingleQuad();
  m.face_vertex_counts = {2, 2};  // degree < 3 (sum still 4)
  Options opts;
  RefinedMesh out;
  CHECK(Refine(ToView(m), opts, &out, nullptr) == Result::InvalidTopology);
}

void test_crease_sharpness_count_rejected() {
  corpus::Mesh cube = corpus::Cube();
  cube.crease_indices = {0, 1, 3};  // one chain of 3 verts = 2 edges
  cube.crease_lengths = {3};
  cube.crease_sharpnesses = {1.0f, 2.0f, 3.0f};  // neither 1 nor 2 entries
  Options opts;
  RefinedMesh out;
  CHECK(Refine(ToView(cube), opts, &out, nullptr) ==
        Result::InvalidTopology);
}

void test_hole_index_out_of_range_rejected() {
  corpus::Mesh cube = corpus::Cube();
  cube.hole_indices = {6};  // cube has faces 0..5
  Options opts;
  RefinedMesh out;
  CHECK(Refine(ToView(cube), opts, &out, nullptr) ==
        Result::InvalidTopology);
}

// ---------------------------------------------------------------------------
// Level-0 passthrough
// ---------------------------------------------------------------------------

void test_level0_passthrough() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.level = 0;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.points == cube.points);
  CHECK(out.face_vertex_counts == cube.face_vertex_counts);
  CHECK(out.face_vertex_indices == cube.face_vertex_indices);
  CHECK(out.face_source.size() == cube.face_vertex_counts.size());
  for (size_t f = 0; f < out.face_source.size(); f++) {
    CHECK(out.face_source[f] == f);
  }
}

void test_level0_fvar_expansion() {
  corpus::Mesh quad = corpus::SingleQuad();
  // 4 corners indexing 2 distinct UV values.
  const float uv_values[] = {0.0f, 0.0f, 1.0f, 1.0f};
  const uint32_t uv_indices[] = {0, 1, 1, 0};
  lightusd::tsd::FVarChannelView ch;
  ch.values = uv_values;
  ch.num_values = 2;
  ch.indices = uv_indices;
  ch.stride = 2;

  Options opts;
  opts.level = 0;
  RefinedMesh out;
  std::string err;
  Result r =
      Refine(ToView(quad), &ch, 1, nullptr, 0, opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.fvar.size() == 1);
  const std::vector<float> expect = {0.0f, 0.0f, 1.0f, 1.0f,
                                     1.0f, 1.0f, 0.0f, 0.0f};
  CHECK(out.fvar[0] == expect);
}

// ---------------------------------------------------------------------------
// Refinement: analytic checks
// ---------------------------------------------------------------------------

void test_bilinear_quad_level1_exact() {
  corpus::Mesh quad = corpus::SingleQuad();
  Options opts;
  opts.scheme = Scheme::Bilinear;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(quad), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  // 4 verts + 4 edge midpoints + 1 centroid; 4 quads.
  CHECK(out.points.size() == 9 * 3);
  CHECK(out.face_vertex_counts.size() == 4);
  for (uint32_t c : out.face_vertex_counts) {
    CHECK(c == 4);
  }
  // Original vertices pass through exactly.
  for (size_t i = 0; i < 12; i++) {
    CHECK(out.points[i] == quad.points[i]);
  }
  // Centroid (last point).
  for (int c = 0; c < 3; c++) {
    const float expect = 0.25f * (quad.points[0 + c] + quad.points[3 + c] +
                                  quad.points[6 + c] + quad.points[9 + c]);
    CHECK(std::fabs(out.points[8 * 3 + size_t(c)] - expect) < 1e-6f);
  }
}

void test_catmark_cube_level1_smooth_vertex() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  // V + E + F = 8 + 12 + 6 = 26 points; 6*4 = 24 quads.
  CHECK(out.points.size() == 26 * 3);
  CHECK(out.face_vertex_counts.size() == 24);

  // Hand-derive the smooth vertex rule for vertex 0 (valence 3):
  // V' = (n-2)/n*V + sum(other endpoints)/n^2 + sum(child face points)/n^2.
  // Child face points (centroids) occupy out.points[(8+12+f)*3].
  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(cube.face_vertex_counts.data(), 6,
                      cube.face_vertex_indices.data(), 24, 8, &topo,
                      nullptr) == Result::Success);
  const uint32_t v = 0;
  const float n = 3.0f;
  for (int c = 0; c < 3; c++) {
    float acc = (n - 2.0f) / n * cube.points[v * 3 + size_t(c)];
    for (uint32_t i = topo.vert_edge_offsets[v];
         i < topo.vert_edge_offsets[v + 1]; i++) {
      const uint32_t e = topo.vert_edges[i];
      const uint32_t other = (topo.edge_verts[2 * e] == v)
                                 ? topo.edge_verts[2 * e + 1]
                                 : topo.edge_verts[2 * e];
      acc += cube.points[other * 3 + size_t(c)] / (n * n);
    }
    for (uint32_t i = topo.vert_face_offsets[v];
         i < topo.vert_face_offsets[v + 1]; i++) {
      const uint32_t f = topo.vert_faces[i];
      acc += out.points[(8 + 12 + f) * 3 + size_t(c)] / (n * n);
    }
    CHECK(std::fabs(out.points[v * 3 + size_t(c)] - acc) < 1e-6f);
  }
}

void test_infinite_corner_pinned() {
  corpus::Mesh cube = corpus::Cube();
  cube.corner_indices = {3};
  cube.corner_sharpnesses = {10.0f};
  Options opts;
  opts.level = 3;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  for (int c = 0; c < 3; c++) {
    CHECK(out.points[3 * 3 + size_t(c)] == cube.points[3 * 3 + size_t(c)]);
  }
}

void test_infinite_crease_midpoints() {
  // Crease ring at sharpness 10: at level 1 the crease edge children are
  // exact midpoints and ring vertices follow (6V + ea + eb) / 8.
  corpus::Mesh cube = corpus::CreasedCube(10.0f, "creased_inf");
  Options opts;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);

  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(cube.face_vertex_counts.data(), 6,
                      cube.face_vertex_indices.data(), 24, 8, &topo,
                      nullptr) == Result::Success);
  // Ring edges: (2,3), (3,5), (5,4), (4,2). Edge child ids are 8 + e.
  const uint32_t ring[4][2] = {{2, 3}, {3, 5}, {5, 4}, {4, 2}};
  for (const auto &rv : ring) {
    // Locate the edge id.
    uint32_t edge = 0xFFFFFFFFu;
    for (uint32_t e = 0; e < topo.num_edges; e++) {
      const uint32_t a = topo.edge_verts[2 * e];
      const uint32_t b = topo.edge_verts[2 * e + 1];
      if ((a == rv[0] && b == rv[1]) || (a == rv[1] && b == rv[0])) {
        edge = e;
        break;
      }
    }
    CHECK(edge != 0xFFFFFFFFu);
    for (int c = 0; c < 3; c++) {
      const float mid = 0.5f * (cube.points[rv[0] * 3 + size_t(c)] +
                                cube.points[rv[1] * 3 + size_t(c)]);
      CHECK(std::fabs(out.points[(8 + edge) * 3 + size_t(c)] - mid) < 1e-6f);
    }
  }
  // Ring vertex 3 has crease neighbors 2 and 5 on the ring.
  for (int c = 0; c < 3; c++) {
    const float expect =
        (6.0f * cube.points[3 * 3 + size_t(c)] +
         cube.points[2 * 3 + size_t(c)] + cube.points[5 * 3 + size_t(c)]) /
        8.0f;
    CHECK(std::fabs(out.points[3 * 3 + size_t(c)] - expect) < 1e-6f);
  }
}

void test_partition_of_unity() {
  // A constant "vertex" primvar must stay exactly constant through smooth
  // refinement (weights sum to 1), and a constant "varying" one likewise.
  corpus::Mesh cube = corpus::CreasedCube(1.5f, "creased");
  std::vector<float> ones(8, 1.0f);
  lightusd::tsd::VertexPrimvarView pv[2];
  pv[0].values = ones.data();
  pv[0].stride = 1;
  pv[0].varying = false;
  pv[1].values = ones.data();
  pv[1].stride = 1;
  pv[1].varying = true;

  Options opts;
  opts.level = 3;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), nullptr, 0, pv, 2, opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.vertex_primvars.size() == 2);
  for (const auto &channel : out.vertex_primvars) {
    CHECK(channel.size() == out.points.size() / 3);
    for (float v : channel) {
      CHECK(std::fabs(v - 1.0f) < 1e-6f);
    }
  }
}

void test_face_source_cube() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.level = 2;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.face_source.size() == 96);  // 6 * 4 * 4
  uint32_t counts[6] = {0, 0, 0, 0, 0, 0};
  for (uint32_t s : out.face_source) {
    CHECK(s < 6);
    counts[s]++;
  }
  for (uint32_t c : counts) {
    CHECK(c == 16);
  }
}

void test_holes_filtered() {
  corpus::Mesh m = corpus::CubeWithHoles();  // holes: faces 1, 4
  Options opts;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(m), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.face_vertex_counts.size() == 16);  // (6 - 2) * 4
  for (uint32_t s : out.face_source) {
    CHECK(s != 1 && s != 4);
  }

  // With remove_holes=false all 24 children remain.
  opts.remove_holes = false;
  RefinedMesh out2;
  r = Refine(ToView(m), opts, &out2, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out2.face_vertex_counts.size() == 24);
}

void test_caps_enforced() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.level = 4;
  opts.max_faces = 100;  // cube L4 would need 6*4^4 = 1536 faces
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::LimitExceeded);

  opts = Options();
  opts.level = 4;
  opts.max_face_vertex_indices = 100;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::LimitExceeded);
}

void test_base_caps_enforced_before_topology() {
  corpus::Mesh cube = corpus::Cube();
  RefinedMesh out;
  std::string err;

  Options opts;
  opts.max_vertices = 7;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::LimitExceeded);

  opts = Options();
  opts.max_faces = 5;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::LimitExceeded);

  opts = Options();
  opts.max_face_vertex_indices = 23;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::LimitExceeded);
}

void test_fvar_split_caps_enforced() {
  corpus::Mesh cube = corpus::Cube();
  std::vector<float> vals(cube.face_vertex_indices.size() * 2);
  for (size_t i = 0; i < cube.face_vertex_indices.size(); i++) {
    vals[2 * i + 0] = float(i);
    vals[2 * i + 1] = float(i + 100);
  }

  lightusd::tsd::FVarChannelView ch;
  ch.values = vals.data();
  ch.num_values = uint32_t(cube.face_vertex_indices.size());
  ch.indices = nullptr;
  ch.stride = 2;
  ch.interpolation = lightusd::tsd::FVarLinearInterpolation::None;

  Options opts;
  opts.max_vertices = 30;  // base and geometry child fit; fvar split child does not.
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(cube), &ch, 1, nullptr, 0, opts, &out, &err) ==
        Result::LimitExceeded);
}

// ---------------------------------------------------------------------------
// FaceVarying smooth modes
// ---------------------------------------------------------------------------

void test_fvar_continuous_equals_vertex_refinement() {
  // A fvar channel that is continuous everywhere (indexed by vertex) on a
  // closed mesh must refine identically to a "vertex" primvar under any
  // smooth fvar mode.
  corpus::Mesh cube = corpus::Cube();
  std::vector<float> vals(8 * 2);
  for (size_t v = 0; v < 8; v++) {
    vals[2 * v] = cube.points[3 * v];
    vals[2 * v + 1] = cube.points[3 * v + 1];
  }

  lightusd::tsd::FVarChannelView ch;
  ch.values = vals.data();
  ch.num_values = 8;
  ch.indices = cube.face_vertex_indices.data();  // continuous: vertex ids
  ch.stride = 2;
  ch.interpolation = lightusd::tsd::FVarLinearInterpolation::CornersPlus1;

  lightusd::tsd::VertexPrimvarView pv;
  pv.values = vals.data();
  pv.stride = 2;
  pv.varying = false;

  Options opts;
  opts.level = 2;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(cube), &ch, 1, &pv, 1, opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(out.fvar.size() == 1);
  CHECK(out.vertex_primvars.size() == 1);

  // Expand the vertex primvar per corner and compare.
  const std::vector<float> &vpv = out.vertex_primvars[0];
  const std::vector<float> &fv = out.fvar[0];
  CHECK(fv.size() == out.face_vertex_indices.size() * 2);
  for (size_t i = 0; i < out.face_vertex_indices.size(); i++) {
    const uint32_t v = out.face_vertex_indices[i];
    for (size_t c = 0; c < 2; c++) {
      CHECK(std::fabs(fv[2 * i + c] - vpv[2 * v + c]) < 1e-6f);
    }
  }
}

void test_fvar_island_constants_preserved() {
  // Constant values within each UV island must stay exactly constant
  // through smooth refinement (partition of unity on the split mesh).
  corpus::Mesh m = corpus::Cube();
  // Two islands of 3 faces; one constant value id per island.
  const uint32_t face_island[6] = {0, 0, 0, 1, 1, 1};
  std::vector<float> vals = {5.0f, -1.0f, 9.0f, 4.0f};  // 2 values, stride 2
  std::vector<uint32_t> indices(24);
  for (size_t f = 0; f < 6; f++) {
    for (size_t k = 0; k < 4; k++) {
      indices[f * 4 + k] = face_island[f];
    }
  }

  lightusd::tsd::FVarChannelView ch;
  ch.values = vals.data();
  ch.num_values = 2;
  ch.indices = indices.data();
  ch.stride = 2;
  ch.interpolation = lightusd::tsd::FVarLinearInterpolation::None;

  Options opts;
  opts.level = 2;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(m), &ch, 1, nullptr, 0, opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);
  size_t corner = 0;
  bool all_match = true;
  for (size_t f = 0; f < out.face_vertex_counts.size(); f++) {
    const uint32_t island = face_island[out.face_source[f]];
    for (uint32_t k = 0; k < out.face_vertex_counts[f]; k++, corner++) {
      all_match &=
          std::fabs(out.fvar[0][2 * corner] - vals[2 * island]) < 1e-6f;
      all_match &=
          std::fabs(out.fvar[0][2 * corner + 1] - vals[2 * island + 1]) <
          1e-6f;
    }
  }
  CHECK(all_match);
}

void test_fvar_modes_differ_at_seams() {
  // Sanity: "all" (linear) and "none" (smooth) must produce different
  // refined values for a channel with seams.
  corpus::Mesh m = corpus::UVSeamGrid();
  lightusd::tsd::FVarChannelView ch;
  ch.values = m.fvar_uv.data();
  ch.num_values = uint32_t(m.fvar_uv.size() / 2);
  ch.indices = m.fvar_indices.data();
  ch.stride = 2;

  Options opts;
  opts.level = 2;
  RefinedMesh out_all;
  RefinedMesh out_none;
  std::string err;
  ch.interpolation = lightusd::tsd::FVarLinearInterpolation::All;
  CHECK(Refine(ToView(m), &ch, 1, nullptr, 0, opts, &out_all, &err) ==
        Result::Success);
  ch.interpolation = lightusd::tsd::FVarLinearInterpolation::None;
  CHECK(Refine(ToView(m), &ch, 1, nullptr, 0, opts, &out_none, &err) ==
        Result::Success);
  CHECK(out_all.fvar[0].size() == out_none.fvar[0].size());
  CHECK(out_all.fvar[0] != out_none.fvar[0]);
}


// ---------------------------------------------------------------------------
// Loop scheme
// ---------------------------------------------------------------------------

void test_loop_rejects_non_tris() {
  corpus::Mesh cube = corpus::Cube();
  Options opts;
  opts.scheme = Scheme::Loop;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(cube), opts, &out, &err) == Result::InvalidTopology);
}

void test_loop_regular_vertex_weights() {
  // Interior valence-6 vertex of a tri grid: V' = 5/8 V + 1/16 sum(ring).
  corpus::Mesh m = corpus::TriGrid(4, 4, "tg");
  Options opts;
  opts.scheme = Scheme::Loop;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(m), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);

  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(m.face_vertex_counts.data(),
                      uint32_t(m.face_vertex_counts.size()),
                      m.face_vertex_indices.data(),
                      uint32_t(m.face_vertex_indices.size()),
                      uint32_t(m.points.size() / 3), &topo,
                      nullptr) == Result::Success);
  // Find an interior valence-6 vertex.
  uint32_t v6 = 0xFFFFFFFFu;
  for (uint32_t v = 0; v < topo.num_points; v++) {
    const uint32_t valence =
        topo.vert_edge_offsets[v + 1] - topo.vert_edge_offsets[v];
    if (!topo.vert_is_boundary[v] && valence == 6) {
      v6 = v;
      break;
    }
  }
  CHECK(v6 != 0xFFFFFFFFu);
  for (int c = 0; c < 3; c++) {
    float acc = 0.625f * m.points[v6 * 3 + size_t(c)];
    for (uint32_t i = topo.vert_edge_offsets[v6];
         i < topo.vert_edge_offsets[v6 + 1]; i++) {
      const uint32_t e = topo.vert_edges[i];
      const uint32_t other = (topo.edge_verts[2 * e] == v6)
                                 ? topo.edge_verts[2 * e + 1]
                                 : topo.edge_verts[2 * e];
      acc += 0.0625f * m.points[other * 3 + size_t(c)];
    }
    CHECK(std::fabs(out.points[v6 * 3 + size_t(c)] - acc) < 1e-6f);
  }
  // All-triangle output, 4x face count.
  CHECK(out.face_vertex_counts.size() == m.face_vertex_counts.size() * 4);
  for (uint32_t n : out.face_vertex_counts) {
    CHECK(n == 3);
  }
}

void test_loop_edge_smooth_rule() {
  // Interior edge child: 3/8 (v0+v1) + 1/8 (opposite vertices).
  corpus::Mesh m = corpus::Icosahedron();
  Options opts;
  opts.scheme = Scheme::Loop;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  Result r = Refine(ToView(m), opts, &out, &err);
  CHECK_MSG(r == Result::Success, err);

  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(m.face_vertex_counts.data(), 20,
                      m.face_vertex_indices.data(), 60, 12, &topo,
                      nullptr) == Result::Success);
  const uint32_t e = 0;
  const uint32_t v0 = topo.edge_verts[0];
  const uint32_t v1 = topo.edge_verts[1];
  const uint32_t f0 = topo.edge_faces[0];
  const uint32_t f1 = topo.edge_faces[1];
  auto opposite = [&](uint32_t f) {
    for (uint32_t k = topo.face_offsets[f]; k < topo.face_offsets[f + 1];
         k++) {
      const uint32_t v = m.face_vertex_indices[k];
      if (v != v0 && v != v1) {
        return v;
      }
    }
    return 0u;
  };
  const uint32_t o0 = opposite(f0);
  const uint32_t o1 = opposite(f1);
  for (int c = 0; c < 3; c++) {
    const float expect =
        0.375f * (m.points[v0 * 3 + size_t(c)] + m.points[v1 * 3 + size_t(c)]) +
        0.125f * (m.points[o0 * 3 + size_t(c)] + m.points[o1 * 3 + size_t(c)]);
    CHECK(std::fabs(out.points[(12 + e) * 3 + size_t(c)] - expect) < 1e-6f);
  }
}


// ---------------------------------------------------------------------------
// Limit surface
// ---------------------------------------------------------------------------

void test_limit_regular_interior_mask() {
  // Regular interior catmark vertex (valence 4): limit = the classic
  // uniform B-spline mask (16 V + 4 sum(E) + sum(F)) / 36 at the final level.
  corpus::Mesh m = corpus::QuadGrid(4, 4, "qg");
  Options opts;
  opts.level = 1;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(m), opts, &refined, &err) == Result::Success);

  RefinedMesh snapped = refined;
  Result r = SnapToLimit(ToView(m), opts, &snapped, &err);
  CHECK_MSG(r == Result::Success, err);

  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(refined.face_vertex_counts.data(),
                      uint32_t(refined.face_vertex_counts.size()),
                      refined.face_vertex_indices.data(),
                      uint32_t(refined.face_vertex_indices.size()),
                      uint32_t(refined.points.size() / 3), &topo,
                      nullptr) == Result::Success);
  // Find an interior valence-4 vertex of the refined level.
  bool tested = false;
  for (uint32_t v = 0; v < topo.num_points && !tested; v++) {
    const uint32_t valence =
        topo.vert_edge_offsets[v + 1] - topo.vert_edge_offsets[v];
    if (topo.vert_is_boundary[v] || valence != 4) {
      continue;
    }
    tested = true;
    for (int c = 0; c < 3; c++) {
      float acc = 16.0f / 36.0f * refined.points[v * 3 + size_t(c)];
      for (uint32_t i = topo.vert_edge_offsets[v];
           i < topo.vert_edge_offsets[v + 1]; i++) {
        const uint32_t e = topo.vert_edges[i];
        const uint32_t other = (topo.edge_verts[2 * e] == v)
                                   ? topo.edge_verts[2 * e + 1]
                                   : topo.edge_verts[2 * e];
        acc += 4.0f / 36.0f * refined.points[other * 3 + size_t(c)];
      }
      for (uint32_t i = topo.vert_face_offsets[v];
           i < topo.vert_face_offsets[v + 1]; i++) {
        const uint32_t f = topo.vert_faces[i];
        // Diagonal vertex of the quad.
        const uint32_t begin = topo.face_offsets[f];
        uint32_t k = 0;
        while (refined.face_vertex_indices[begin + k] != v) {
          k++;
        }
        const uint32_t diag =
            refined.face_vertex_indices[begin + ((k + 2) % 4)];
        acc += 1.0f / 36.0f * refined.points[diag * 3 + size_t(c)];
      }
      CHECK(std::fabs(snapped.points[v * 3 + size_t(c)] - acc) < 1e-5f);
    }
  }
  CHECK(tested);
}

void test_limit_corner_pinned() {
  corpus::Mesh m = corpus::Cube();
  m.corner_indices = {3};
  m.corner_sharpnesses = {10.0f};
  Options opts;
  opts.level = 2;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(m), opts, &refined, &err) == Result::Success);
  RefinedMesh snapped = refined;
  CHECK(SnapToLimit(ToView(m), opts, &snapped, &err) == Result::Success);
  for (int c = 0; c < 3; c++) {
    CHECK(snapped.points[3 * 3 + size_t(c)] == m.points[3 * 3 + size_t(c)]);
  }
}

void test_limit_bilinear_identity() {
  corpus::Mesh m = corpus::Cube();
  Options opts;
  opts.scheme = Scheme::Bilinear;
  opts.level = 1;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(m), opts, &refined, &err) == Result::Success);
  RefinedMesh snapped = refined;
  CHECK(SnapToLimit(ToView(m), opts, &snapped, &err) == Result::Success);
  CHECK(snapped.points == refined.points);

  Options capped = opts;
  capped.max_vertices = 7;
  CHECK(SnapToLimit(ToView(m), capped, &snapped, &err) ==
        Result::LimitExceeded);
}

void test_limit_normals_unit_length() {
  corpus::Mesh m = corpus::CreasedCube(2.0f, "creased");
  Options opts;
  opts.level = 2;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(m), opts, &refined, &err) == Result::Success);
  std::vector<float> normals;
  Result r = ComputeLimitNormals(ToView(m), opts, refined, &normals, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(normals.size() == refined.points.size());
  for (size_t v = 0; v < normals.size() / 3; v++) {
    const float len = std::sqrt(normals[3 * v] * normals[3 * v] +
                                normals[3 * v + 1] * normals[3 * v + 1] +
                                normals[3 * v + 2] * normals[3 * v + 2]);
    CHECK(std::fabs(len - 1.0f) < 1e-3f);
  }
}

void test_limit_caps_enforced() {
  corpus::Mesh m = corpus::Cube();
  Options opts;
  opts.level = 4;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(m), opts, &refined, &err) == Result::Success);

  Options capped = opts;
  capped.max_faces = 100;  // cube L4 would need 6*4^4 = 1536 faces
  RefinedMesh snapped = refined;
  CHECK(SnapToLimit(ToView(m), capped, &snapped, &err) ==
        Result::LimitExceeded);

  std::vector<float> normals;
  CHECK(ComputeLimitNormals(ToView(m), capped, refined, &normals, &err) ==
        Result::LimitExceeded);
}

// ---------------------------------------------------------------------------
// Determinism (serial vs parallel)
// ---------------------------------------------------------------------------

void parallel_for_4threads(void *user, uint32_t count,
                           void (*body)(void *, uint32_t), void *body_user) {
  (void)user;
  std::vector<std::thread> threads;
  const uint32_t num_threads = 4;
  for (uint32_t t = 0; t < num_threads; t++) {
    threads.emplace_back([=]() {
      for (uint32_t i = t; i < count; i += num_threads) {
        body(body_user, i);
      }
    });
  }
  for (std::thread &th : threads) {
    th.join();
  }
}

void test_parallel_determinism() {
  corpus::Mesh m = corpus::CreasedCube(2.7f, "creased");
  Options serial;
  serial.level = 3;
  RefinedMesh a;
  std::string err;
  CHECK(Refine(ToView(m), serial, &a, &err) == Result::Success);

  Options parallel = serial;
  parallel.parallel_for = parallel_for_4threads;
  RefinedMesh b;
  CHECK(Refine(ToView(m), parallel, &b, &err) == Result::Success);

  CHECK(a.points.size() == b.points.size());
  CHECK(memcmp(a.points.data(), b.points.data(),
               a.points.size() * sizeof(float)) == 0);
  CHECK(a.face_vertex_indices == b.face_vertex_indices);

  // Serial runs are reproducible.
  RefinedMesh c;
  CHECK(Refine(ToView(m), serial, &c, &err) == Result::Success);
  CHECK(memcmp(a.points.data(), c.points.data(),
               a.points.size() * sizeof(float)) == 0);
}

// ---------------------------------------------------------------------------
// Topology builder (internal)
// ---------------------------------------------------------------------------

void test_topology_cube() {
  corpus::Mesh cube = corpus::Cube();
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(cube.face_vertex_counts.data(),
                           uint32_t(cube.face_vertex_counts.size()),
                           cube.face_vertex_indices.data(),
                           uint32_t(cube.face_vertex_indices.size()),
                           uint32_t(cube.points.size() / 3), &topo, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(topo.num_edges == 12);
  for (uint32_t e = 0; e < topo.num_edges; e++) {
    CHECK(!topo.IsBoundaryEdge(e));
  }
  for (uint32_t v = 0; v < topo.num_points; v++) {
    CHECK(topo.vert_is_boundary[v] == 0);
    // Cube: every vertex has 3 incident faces and 3 incident edges.
    CHECK(topo.vert_face_offsets[v + 1] - topo.vert_face_offsets[v] == 3);
    CHECK(topo.vert_edge_offsets[v + 1] - topo.vert_edge_offsets[v] == 3);
  }
}

void test_topology_single_quad_boundary() {
  corpus::Mesh quad = corpus::SingleQuad();
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(quad.face_vertex_counts.data(), 1,
                           quad.face_vertex_indices.data(), 4, 4, &topo, &err);
  CHECK_MSG(r == Result::Success, err);
  CHECK(topo.num_edges == 4);
  for (uint32_t e = 0; e < 4; e++) {
    CHECK(topo.IsBoundaryEdge(e));
  }
  for (uint32_t v = 0; v < 4; v++) {
    CHECK(topo.vert_is_boundary[v] == 1);
  }
}

void test_topology_mixed_degree() {
  corpus::Mesh m = corpus::MixedDegree();
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(m.face_vertex_counts.data(),
                           uint32_t(m.face_vertex_counts.size()),
                           m.face_vertex_indices.data(),
                           uint32_t(m.face_vertex_indices.size()),
                           uint32_t(m.points.size() / 3), &topo, &err);
  CHECK_MSG(r == Result::Success, err);
  // V - E + F = 2 - boundary_loops for a disk: 8 - E + 4 = 1 => E = 11.
  CHECK(topo.num_edges == 11);
}

void test_topology_non_manifold_rejected() {
  corpus::Mesh m = corpus::NonManifoldFan();
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(m.face_vertex_counts.data(),
                           uint32_t(m.face_vertex_counts.size()),
                           m.face_vertex_indices.data(),
                           uint32_t(m.face_vertex_indices.size()),
                           uint32_t(m.points.size() / 3), &topo, &err);
  CHECK(r == Result::InvalidTopology);
}

void test_topology_inconsistent_winding_rejected() {
  // Two quads sharing edge (1,2) traversed in the SAME direction.
  const uint32_t fvc[] = {4, 4};
  const uint32_t fvi[] = {0, 1, 2, 3, 1, 2, 4, 5};
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(fvc, 2, fvi, 8, 6, &topo, &err);
  CHECK(r == Result::InvalidTopology);
}

void test_topology_degenerate_edge_rejected() {
  const uint32_t fvc[] = {3};
  const uint32_t fvi[] = {0, 0, 1};
  lightusd::tsd::Topology topo;
  std::string err;
  Result r = BuildTopology(fvc, 1, fvi, 3, 2, &topo, &err);
  CHECK(r == Result::InvalidTopology);
}

// ---------------------------------------------------------------------------
// Crease canonicalization (internal)
// ---------------------------------------------------------------------------

void test_crease_canonicalize_per_crease() {
  corpus::Mesh cube = corpus::Cube();
  cube.crease_indices = {0, 1, 3};  // chain: edges (0,1), (1,3)
  cube.crease_lengths = {3};
  cube.crease_sharpnesses = {2.5f};  // per-crease

  lightusd::tsd::CreaseEdges ce;
  std::string err;
  Result r = CanonicalizeCreases(ToView(cube), &ce, &err);
  CHECK(r == Result::Success);
  CHECK(ce.sharpnesses.size() == 2);
  for (float s : ce.sharpnesses) {
    CHECK(std::fabs(s - 2.5f) < 1e-6f);
  }
  // Sorted vertex pairs.
  CHECK(ce.edge_verts[0] < ce.edge_verts[1]);
  CHECK(ce.edge_verts[2] < ce.edge_verts[3]);
}

void test_crease_canonicalize_per_edge_and_dedup() {
  corpus::Mesh cube = corpus::Cube();
  // Two chains; edge (0,1) appears in both with different sharpness.
  cube.crease_indices = {0, 1, 3, 1, 0};
  cube.crease_lengths = {3, 2};
  cube.crease_sharpnesses = {1.0f, 2.0f, 4.0f};  // per-edge (2 + 1)

  lightusd::tsd::CreaseEdges ce;
  std::string err;
  Result r = CanonicalizeCreases(ToView(cube), &ce, &err);
  CHECK(r == Result::Success);
  CHECK(ce.sharpnesses.size() == 2);  // (0,1) deduped, (1,3) kept
  bool found01 = false;
  bool found13 = false;
  for (size_t i = 0; i < ce.sharpnesses.size(); i++) {
    const uint32_t a = ce.edge_verts[2 * i];
    const uint32_t b = ce.edge_verts[2 * i + 1];
    if (a == 0 && b == 1) {
      found01 = true;
      CHECK(std::fabs(ce.sharpnesses[i] - 4.0f) < 1e-6f);  // max(1, 4)
    }
    if (a == 1 && b == 3) {
      found13 = true;
      CHECK(std::fabs(ce.sharpnesses[i] - 2.0f) < 1e-6f);
    }
  }
  CHECK(found01);
  CHECK(found13);
}

void test_crease_sharpness_clamped() {
  corpus::Mesh cube = corpus::Cube();
  cube.crease_indices = {0, 1};
  cube.crease_lengths = {2};
  cube.crease_sharpnesses = {1.0e10f};  // way past infinite

  lightusd::tsd::CreaseEdges ce;
  std::string err;
  CHECK(CanonicalizeCreases(ToView(cube), &ce, &err) == Result::Success);
  CHECK(ce.sharpnesses.size() == 1);
  CHECK(std::fabs(ce.sharpnesses[0] - lightusd::tsd::kInfiniteSharpness) <
        1e-6f);
}

// ---------------------------------------------------------------------------
// Broad invariant sweep + added analytic regressions
//
// Review conclusion (June 2026): the kernels match OpenSubdiv across the full
// swept matrix (tests/feat/subdiv/subdiv-verify-osd.cc). The tests below run in
// normal CI without OpenSubdiv and lock in correctness properties and the
// hand-derived rules the differential test cannot express as invariants.
// ---------------------------------------------------------------------------

std::vector<corpus::Mesh> AllManifoldMeshes() {
  std::vector<corpus::Mesh> m;
  m.push_back(corpus::Cube());
  m.push_back(corpus::SingleQuad());
  m.push_back(corpus::SingleTri());
  m.push_back(corpus::QuadGrid(4, 4, "qg4x4"));
  m.push_back(corpus::QuadGrid(8, 1, "qg_strip"));
  m.push_back(corpus::QuadTorus(8, 6));
  m.push_back(corpus::TriFan(3));
  m.push_back(corpus::TriFan(5));
  m.push_back(corpus::TriFan(8));
  m.push_back(corpus::NGons());
  m.push_back(corpus::MixedDegree());
  m.push_back(corpus::CreasedCube(0.4f, "cc_0_4"));
  m.push_back(corpus::CreasedCube(2.7f, "cc_2_7"));
  m.push_back(corpus::PerEdgeCreasedCube());
  m.push_back(corpus::CorneredGrid());
  m.push_back(corpus::CubeWithHoles());
  m.push_back(corpus::UVSeamGrid());
  m.push_back(corpus::UVCube());
  m.push_back(corpus::UVDartGrid());
  m.push_back(corpus::PartialSeamQuads());
  m.push_back(corpus::Icosahedron());
  m.push_back(corpus::TriGrid(4, 3, "tg4x3"));
  m.push_back(corpus::CreasedTriGrid());
  m.push_back(corpus::UVTriGrid());
  return m;
}

// For each corpus mesh x valid scheme x boundary mode x level: every refined
// position is finite and inside the base AABB (all subdivision position masks
// are convex combinations of control points), output face arity matches the
// scheme, indices and face_source are in range, constant vertex/varying/fvar
// fields stay constant (partition of unity), and serial == parallel output.
void test_invariant_sweep() {
  const std::vector<corpus::Mesh> meshes = AllManifoldMeshes();
  const BoundaryInterpolation boundaries[3] = {
      BoundaryInterpolation::EdgeAndCorner, BoundaryInterpolation::EdgeOnly,
      BoundaryInterpolation::None};
  for (const corpus::Mesh &m : meshes) {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 3 <= m.points.size(); i += 3) {
      for (int c = 0; c < 3; c++) {
        lo[c] = std::min(lo[c], m.points[i + size_t(c)]);
        hi[c] = std::max(hi[c], m.points[i + size_t(c)]);
      }
    }
    const uint32_t nbase = uint32_t(m.face_vertex_counts.size());
    const uint32_t np = uint32_t(m.points.size() / 3);
    const uint32_t ncorner = uint32_t(m.face_vertex_indices.size());

    std::vector<Scheme> schemes = {Scheme::CatmullClark, Scheme::Bilinear};
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) {
      if (c != 3) {
        all_tris = false;
      }
    }
    if (all_tris) {
      schemes.push_back(Scheme::Loop);
    }

    for (Scheme scheme : schemes) {
      for (BoundaryInterpolation boundary : boundaries) {
        for (int level = 1; level <= 3; level++) {
          std::vector<float> ones(np, 1.0f);
          VertexPrimvarView pv[2];
          pv[0].values = ones.data();
          pv[0].stride = 1;
          pv[0].varying = false;
          pv[1].values = ones.data();
          pv[1].stride = 1;
          pv[1].varying = true;
          std::vector<float> cfv(ncorner, 7.0f);
          FVarChannelView fv;
          fv.values = cfv.data();
          fv.num_values = ncorner;
          fv.indices = nullptr;
          fv.stride = 1;
          fv.interpolation = FVarLinearInterpolation::CornersPlus1;

          Options opts;
          opts.scheme = scheme;
          opts.boundary = boundary;
          opts.level = level;
          RefinedMesh out;
          std::string err;
          const Result r = Refine(ToView(m), &fv, 1, pv, 2, opts, &out, &err);
          CHECK_MSG(r == Result::Success, m.name + ": " + err);
          if (r != Result::Success) {
            continue;
          }

          const uint32_t arity = (scheme == Scheme::Loop) ? 3u : 4u;
          const uint32_t npts = uint32_t(out.points.size() / 3);
          bool arity_ok = true, idx_ok = true, src_ok = true;
          bool finite_ok = true, aabb_ok = true;
          for (uint32_t c : out.face_vertex_counts) {
            arity_ok = arity_ok && (c == arity);
          }
          for (uint32_t i : out.face_vertex_indices) {
            idx_ok = idx_ok && (i < npts);
          }
          for (uint32_t s : out.face_source) {
            src_ok = src_ok && (s < nbase);
          }
          for (size_t i = 0; i < out.points.size(); i++) {
            const float x = out.points[i];
            finite_ok = finite_ok && std::isfinite(x);
            const int c = int(i % 3);
            aabb_ok = aabb_ok && (x >= lo[c] - 2e-4f) && (x <= hi[c] + 2e-4f);
          }
          CHECK_MSG(arity_ok, m.name);
          CHECK_MSG(idx_ok, m.name);
          CHECK_MSG(src_ok, m.name);
          CHECK_MSG(finite_ok, m.name);
          CHECK_MSG(aabb_ok, m.name);

          bool pou_ok = (out.vertex_primvars.size() == 2) && out.fvar.size() == 1;
          for (const auto &ch : out.vertex_primvars) {
            for (float v : ch) {
              pou_ok = pou_ok && (std::fabs(v - 1.0f) < 1e-4f);
            }
          }
          if (!out.fvar.empty()) {
            for (float v : out.fvar[0]) {
              pou_ok = pou_ok && (std::fabs(v - 7.0f) < 1e-4f);
            }
          }
          CHECK_MSG(pou_ok, m.name);

          Options par = opts;
          par.parallel_for = parallel_for_4threads;
          RefinedMesh out2;
          CHECK(Refine(ToView(m), &fv, 1, pv, 2, par, &out2, &err) ==
                Result::Success);
          CHECK_MSG(out.points == out2.points, m.name);
          CHECK_MSG(out.fvar == out2.fvar, m.name);
          CHECK_MSG(out.vertex_primvars == out2.vertex_primvars, m.name);
        }
      }
    }
  }
}

void test_nonmanifold_rejected_all_schemes() {
  corpus::Mesh m = corpus::NonManifoldFan();
  const Scheme schemes[2] = {Scheme::CatmullClark, Scheme::Bilinear};
  for (Scheme s : schemes) {
    Options opts;
    opts.scheme = s;
    opts.level = 1;
    RefinedMesh out;
    std::string err;
    CHECK(Refine(ToView(m), opts, &out, &err) == Result::InvalidTopology);
  }
}

void test_closed_stays_closed() {
  corpus::Mesh meshes[3] = {corpus::Cube(), corpus::QuadTorus(8, 6),
                            corpus::Icosahedron()};
  for (corpus::Mesh &m : meshes) {
    std::vector<Scheme> schemes = {Scheme::CatmullClark, Scheme::Bilinear};
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) {
      all_tris = all_tris && (c == 3);
    }
    if (all_tris) {
      schemes.push_back(Scheme::Loop);
    }
    for (Scheme s : schemes) {
      Options opts;
      opts.scheme = s;
      opts.level = 2;
      RefinedMesh out;
      std::string err;
      CHECK(Refine(ToView(m), opts, &out, &err) == Result::Success);
      lightusd::tsd::Topology topo;
      CHECK(BuildTopology(out.face_vertex_counts.data(),
                          uint32_t(out.face_vertex_counts.size()),
                          out.face_vertex_indices.data(),
                          uint32_t(out.face_vertex_indices.size()),
                          uint32_t(out.points.size() / 3), &topo,
                          nullptr) == Result::Success);
      bool any_boundary = false;
      for (uint32_t e = 0; e < topo.num_edges; e++) {
        any_boundary = any_boundary || topo.IsBoundaryEdge(e);
      }
      CHECK_MSG(!any_boundary, m.name);
    }
  }
}

void test_semisharp_crease_transitional_blend() {
  // A ring crease at sharpness 0.5: at level 1 every crease edge child is the
  // fractional blend (1-s)*smooth + s*midpoint (the 0<s<1 path), not a pure
  // crease midpoint.
  corpus::Mesh m = corpus::CreasedCube(0.5f, "cc_0_5");
  Options opts;
  opts.level = 1;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(m), opts, &out, &err) == Result::Success);

  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(m.face_vertex_counts.data(), 6,
                      m.face_vertex_indices.data(), 24, 8, &topo,
                      nullptr) == Result::Success);
  const uint32_t ring[4][2] = {{2, 3}, {3, 5}, {5, 4}, {4, 2}};
  const float s = 0.5f;
  for (const auto &rv : ring) {
    uint32_t e = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < topo.num_edges; i++) {
      const uint32_t a = topo.edge_verts[2 * i];
      const uint32_t b = topo.edge_verts[2 * i + 1];
      if ((a == rv[0] && b == rv[1]) || (a == rv[1] && b == rv[0])) {
        e = i;
        break;
      }
    }
    CHECK(e != 0xFFFFFFFFu);
    const uint32_t f0 = topo.edge_faces[2 * e];
    const uint32_t f1 = topo.edge_faces[2 * e + 1];
    CHECK(f1 != lightusd::tsd::kInvalidIndex);
    for (int c = 0; c < 3; c++) {
      const float p0 = m.points[rv[0] * 3 + size_t(c)];
      const float p1 = m.points[rv[1] * 3 + size_t(c)];
      const float fc0 = out.points[(8 + 12 + f0) * 3 + size_t(c)];
      const float fc1 = out.points[(8 + 12 + f1) * 3 + size_t(c)];
      const float smooth = 0.25f * (p0 + p1 + fc0 + fc1);
      const float midpoint = 0.5f * (p0 + p1);
      const float expect = (1.0f - s) * smooth + s * midpoint;
      CHECK(std::fabs(out.points[(8 + e) * 3 + size_t(c)] - expect) < 1e-6f);
    }
  }
}

void test_chaikin_formula_and_decay() {
  // ChaikinChildEdgeSharpness matches OpenSubdiv's
  // Sdc::Crease::SubdivideEdgeSharpnessAtVertex: the neighbor average is over
  // SEMI-sharp incident edges only, so infinitely-sharp neighbors are excluded.
  // (Regression: do not "fix" this to include infinite edges -- it would
  // diverge from OSD; the differential test confirms the current behavior.)
  using lightusd::tsd::ChaikinChildEdgeSharpness;
  {
    const float inc[3] = {4.0f, 10.0f, 10.0f};  // semi-sharp set is {4.0} only
    CHECK(std::fabs(ChaikinChildEdgeSharpness(4.0f, 3, inc) - 3.0f) < 1e-6f);
  }
  {
    const float inc[2] = {2.0f, 4.0f};
    // edge 2: avg_others=(6-2)/1=4, blend=0.75*2+0.25*4=2.5, decrement -> 1.5.
    CHECK(std::fabs(ChaikinChildEdgeSharpness(2.0f, 2, inc) - 1.5f) < 1e-6f);
    // edge 4: avg_others=(6-4)/1=2, blend=0.75*4+0.25*2=3.5, decrement -> 2.5.
    CHECK(std::fabs(ChaikinChildEdgeSharpness(4.0f, 2, inc) - 2.5f) < 1e-6f);
  }
  {
    const float inc[3] = {3.0f, 0.0f, 0.0f};  // lone crease: plain decrement
    CHECK(std::fabs(ChaikinChildEdgeSharpness(3.0f, 3, inc) - 2.0f) < 1e-6f);
  }
  // End-to-end: Chaikin and Uniform diverge where a crease vertex joins two
  // semi-sharp edges of different sharpness (both verified against OSD).
  corpus::Mesh m = corpus::PerEdgeCreasedCube();
  Options u;
  u.level = 2;
  u.creasing = CreasingMethod::Uniform;
  Options ch = u;
  ch.creasing = CreasingMethod::Chaikin;
  RefinedMesh a, b;
  std::string err;
  CHECK(Refine(ToView(m), u, &a, &err) == Result::Success);
  CHECK(Refine(ToView(m), ch, &b, &err) == Result::Success);
  CHECK(a.points.size() == b.points.size());
  bool differ = false;
  for (size_t i = 0; i < a.points.size(); i++) {
    differ = differ || (std::fabs(a.points[i] - b.points[i]) > 1e-5f);
  }
  CHECK(differ);
}

void test_boundary_edgeonly_vs_edgeandcorner() {
  // Vertex 0 of a grid is a corner: a boundary vertex with exactly two boundary
  // edges. EdgeAndCorner pins it (stays at base); EdgeOnly applies the boundary
  // crease rule 3/4 V + 1/8 (ea + eb).
  corpus::Mesh m = corpus::QuadGrid(3, 3, "bgrid");
  const uint32_t nf = uint32_t(m.face_vertex_counts.size());
  const uint32_t nfi = uint32_t(m.face_vertex_indices.size());
  const uint32_t nv = uint32_t(m.points.size() / 3);
  lightusd::tsd::Topology topo;
  CHECK(BuildTopology(m.face_vertex_counts.data(), nf,
                      m.face_vertex_indices.data(), nfi, nv, &topo,
                      nullptr) == Result::Success);
  CHECK(topo.vert_is_boundary[0]);

  Options ec;
  ec.level = 2;
  ec.boundary = BoundaryInterpolation::EdgeAndCorner;
  RefinedMesh a;
  std::string err;
  CHECK(Refine(ToView(m), ec, &a, &err) == Result::Success);
  for (int c = 0; c < 3; c++) {
    CHECK(std::fabs(a.points[size_t(c)] - m.points[size_t(c)]) < 1e-6f);
  }

  Options eo = ec;
  eo.boundary = BoundaryInterpolation::EdgeOnly;
  RefinedMesh b;
  CHECK(Refine(ToView(m), eo, &b, &err) == Result::Success);
  bool moved = false;
  for (int c = 0; c < 3; c++) {
    moved = moved || (std::fabs(b.points[size_t(c)] - m.points[size_t(c)]) >
                      1e-6f);
  }
  CHECK(moved);

  Options eo1 = eo;
  eo1.level = 1;
  RefinedMesh b1;
  CHECK(Refine(ToView(m), eo1, &b1, &err) == Result::Success);
  uint32_t nb[2] = {0, 0};
  uint32_t cnt = 0;
  for (uint32_t i = topo.vert_edge_offsets[0]; i < topo.vert_edge_offsets[1];
       i++) {
    const uint32_t e = topo.vert_edges[i];
    if (topo.IsBoundaryEdge(e)) {
      const uint32_t other = (topo.edge_verts[2 * e] == 0)
                                 ? topo.edge_verts[2 * e + 1]
                                 : topo.edge_verts[2 * e];
      if (cnt < 2) {
        nb[cnt] = other;
      }
      cnt++;
    }
  }
  CHECK(cnt == 2);
  for (int c = 0; c < 3; c++) {
    const float expect = 0.75f * m.points[size_t(c)] +
                         0.125f * (m.points[nb[0] * 3 + size_t(c)] +
                                   m.points[nb[1] * 3 + size_t(c)]);
    CHECK(std::fabs(b1.points[size_t(c)] - expect) < 1e-6f);
  }
}

void test_boundary_none_makes_holes() {
  // Open grid: boundary "none" drops boundary-incident faces (holes). Closed
  // mesh: no boundary, so "none" leaves the face count unchanged.
  corpus::Mesh g = corpus::QuadGrid(4, 4, "g");
  Options none;
  none.level = 1;
  none.boundary = BoundaryInterpolation::None;
  Options ec = none;
  ec.boundary = BoundaryInterpolation::EdgeAndCorner;
  RefinedMesh a, b;
  std::string err;
  CHECK(Refine(ToView(g), none, &a, &err) == Result::Success);
  CHECK(Refine(ToView(g), ec, &b, &err) == Result::Success);
  CHECK(a.face_vertex_counts.size() < b.face_vertex_counts.size());
  for (uint32_t s : a.face_source) {
    CHECK(s < g.face_vertex_counts.size());
  }

  corpus::Mesh cube = corpus::Cube();
  RefinedMesh c, d;
  CHECK(Refine(ToView(cube), none, &c, &err) == Result::Success);
  CHECK(Refine(ToView(cube), ec, &d, &err) == Result::Success);
  CHECK(c.face_vertex_counts.size() == d.face_vertex_counts.size());
}

void test_fvar_stride_and_indexing() {
  // Stride-3 and stride-4 fvar channels keep a constant field constant.
  corpus::Mesh m = corpus::UVSeamGrid();
  const uint32_t ncorner = uint32_t(m.face_vertex_indices.size());
  for (uint32_t stride = 3; stride <= 4; stride++) {
    std::vector<float> vals(size_t(ncorner) * stride, 2.5f);
    FVarChannelView fv;
    fv.values = vals.data();
    fv.num_values = ncorner;
    fv.indices = nullptr;
    fv.stride = stride;
    fv.interpolation = FVarLinearInterpolation::CornersPlus1;
    Options opts;
    opts.level = 2;
    RefinedMesh out;
    std::string err;
    CHECK(Refine(ToView(m), &fv, 1, nullptr, 0, opts, &out, &err) ==
          Result::Success);
    CHECK(out.fvar.size() == 1);
    CHECK(out.fvar[0].size() ==
          size_t(out.face_vertex_indices.size()) * stride);
    for (float v : out.fvar[0]) {
      CHECK(std::fabs(v - 2.5f) < 1e-5f);
    }
  }

  // An indexed channel refines identically to the equivalent flattened
  // (identity) channel.
  std::vector<float> identity(size_t(ncorner) * 2);
  for (uint32_t i = 0; i < ncorner; i++) {
    const uint32_t id = m.fvar_indices[i];
    identity[size_t(i) * 2 + 0] = m.fvar_uv[size_t(id) * 2 + 0];
    identity[size_t(i) * 2 + 1] = m.fvar_uv[size_t(id) * 2 + 1];
  }
  for (int mode = 0; mode < 2; mode++) {
    const FVarLinearInterpolation interp =
        mode ? FVarLinearInterpolation::All : FVarLinearInterpolation::None;
    FVarChannelView indexed;
    indexed.values = m.fvar_uv.data();
    indexed.num_values = uint32_t(m.fvar_uv.size() / 2);
    indexed.indices = m.fvar_indices.data();
    indexed.stride = 2;
    indexed.interpolation = interp;
    FVarChannelView flat;
    flat.values = identity.data();
    flat.num_values = ncorner;
    flat.indices = nullptr;
    flat.stride = 2;
    flat.interpolation = interp;
    Options opts;
    opts.level = 2;
    RefinedMesh a, b;
    std::string err;
    CHECK(Refine(ToView(m), &indexed, 1, nullptr, 0, opts, &a, &err) ==
          Result::Success);
    CHECK(Refine(ToView(m), &flat, 1, nullptr, 0, opts, &b, &err) ==
          Result::Success);
    CHECK(a.fvar.size() == 1 && b.fvar.size() == 1);
    CHECK(a.fvar[0].size() == b.fvar[0].size());
    bool eq = a.fvar[0].size() == b.fvar[0].size();
    for (size_t i = 0; i < a.fvar[0].size() && eq; i++) {
      eq = std::fabs(a.fvar[0][i] - b.fvar[0][i]) < 1e-6f;
    }
    CHECK(eq);
  }
}

void test_varying_vs_vertex_primvar() {
  // "varying" primvars use the linear (bilinear) kernel regardless of scheme,
  // so they differ from "vertex" (smooth) primvars at irregular vertices and
  // are identical under CatmullClark and Bilinear.
  corpus::Mesh m = corpus::Cube();
  std::vector<float> data(8);
  for (uint32_t i = 0; i < 8; i++) {
    data[i] = float(i) * 0.37f + 0.1f;
  }
  VertexPrimvarView vtx;
  vtx.values = data.data();
  vtx.stride = 1;
  vtx.varying = false;
  VertexPrimvarView var;
  var.values = data.data();
  var.stride = 1;
  var.varying = true;

  Options cc;
  cc.level = 2;
  RefinedMesh smooth, linear_cc;
  std::string err;
  CHECK(Refine(ToView(m), nullptr, 0, &vtx, 1, cc, &smooth, &err) ==
        Result::Success);
  CHECK(Refine(ToView(m), nullptr, 0, &var, 1, cc, &linear_cc, &err) ==
        Result::Success);
  bool differ = false;
  for (size_t i = 0; i < smooth.vertex_primvars[0].size(); i++) {
    differ = differ || (std::fabs(smooth.vertex_primvars[0][i] -
                                  linear_cc.vertex_primvars[0][i]) > 1e-5f);
  }
  CHECK(differ);

  Options bil = cc;
  bil.scheme = Scheme::Bilinear;
  RefinedMesh linear_bil;
  CHECK(Refine(ToView(m), nullptr, 0, &var, 1, bil, &linear_bil, &err) ==
        Result::Success);
  CHECK(linear_cc.vertex_primvars[0] == linear_bil.vertex_primvars[0]);
}

void test_vertex_primvar_equals_geometry() {
  // Geometry is itself a smooth "vertex" primvar: feeding the control points as
  // a stride-3 vertex primvar reproduces the refined positions exactly,
  // including crease handling.
  corpus::Mesh m = corpus::CreasedCube(1.5f, "cc_1_5g");
  VertexPrimvarView pv;
  pv.values = m.points.data();
  pv.stride = 3;
  pv.varying = false;
  Options opts;
  opts.level = 3;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(m), nullptr, 0, &pv, 1, opts, &out, &err) ==
        Result::Success);
  CHECK(out.vertex_primvars[0].size() == out.points.size());
  bool eq = out.vertex_primvars[0].size() == out.points.size();
  for (size_t i = 0; i < out.points.size() && eq; i++) {
    eq = std::fabs(out.vertex_primvars[0][i] - out.points[i]) < 1e-5f;
  }
  CHECK(eq);
}

void test_trisub_smooth_differs() {
  // triangleSubdivisionRule "smooth" alters Catmull-Clark edge weights on
  // triangle-incident interior edges (verified against OpenSubdiv); confirm it
  // actually changes the result on a tri+quad mesh.
  corpus::Mesh m = corpus::MixedDegree();
  Options cc;
  cc.level = 2;
  cc.triangle_subdivision = TriangleSubdivision::CatmullClark;
  Options sm = cc;
  sm.triangle_subdivision = TriangleSubdivision::Smooth;
  RefinedMesh a, b;
  std::string err;
  CHECK(Refine(ToView(m), cc, &a, &err) == Result::Success);
  CHECK(Refine(ToView(m), sm, &b, &err) == Result::Success);
  CHECK(a.points.size() == b.points.size());
  bool differ = false;
  for (size_t i = 0; i < a.points.size(); i++) {
    differ = differ || (std::fabs(a.points[i] - b.points[i]) > 1e-5f);
  }
  CHECK(differ);
}

void test_limit_loop_and_planar_normals() {
  // Loop limit pipeline: SnapToLimit and ComputeLimitNormals succeed, stay
  // finite, and limit points stay inside the base AABB.
  corpus::Mesh ico = corpus::Icosahedron();
  float lo[3] = {1e30f, 1e30f, 1e30f};
  float hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i + 3 <= ico.points.size(); i += 3) {
    for (int c = 0; c < 3; c++) {
      lo[c] = std::min(lo[c], ico.points[i + size_t(c)]);
      hi[c] = std::max(hi[c], ico.points[i + size_t(c)]);
    }
  }
  Options lopts;
  lopts.scheme = Scheme::Loop;
  lopts.level = 2;
  RefinedMesh refined;
  std::string err;
  CHECK(Refine(ToView(ico), lopts, &refined, &err) == Result::Success);
  RefinedMesh snapped = refined;
  CHECK(SnapToLimit(ToView(ico), lopts, &snapped, &err) == Result::Success);
  for (size_t i = 0; i < snapped.points.size(); i++) {
    CHECK(std::isfinite(snapped.points[i]));
    const int c = int(i % 3);
    CHECK(snapped.points[i] >= lo[c] - 1e-3f &&
          snapped.points[i] <= hi[c] + 1e-3f);
  }
  std::vector<float> normals;
  CHECK(ComputeLimitNormals(ToView(ico), lopts, refined, &normals, &err) ==
        Result::Success);
  CHECK(normals.size() == refined.points.size());
  for (size_t v = 0; v < normals.size() / 3; v++) {
    const float len = std::sqrt(normals[3 * v] * normals[3 * v] +
                                normals[3 * v + 1] * normals[3 * v + 1] +
                                normals[3 * v + 2] * normals[3 * v + 2]);
    CHECK(std::fabs(len - 1.0f) < 1e-3f);
  }

  // On a perfectly planar (z=0) grid, every Catmull-Clark limit normal is +/-z.
  corpus::Mesh grid = corpus::QuadGrid(4, 4, "flatgrid");
  for (size_t i = 0; i < grid.points.size() / 3; i++) {
    grid.points[i * 3 + 2] = 0.0f;
  }
  Options copts;
  copts.level = 2;
  RefinedMesh cr;
  CHECK(Refine(ToView(grid), copts, &cr, &err) == Result::Success);
  std::vector<float> gn;
  CHECK(ComputeLimitNormals(ToView(grid), copts, cr, &gn, &err) ==
        Result::Success);
  for (size_t v = 0; v < gn.size() / 3; v++) {
    CHECK(std::fabs(gn[3 * v]) < 1e-4f);
    CHECK(std::fabs(gn[3 * v + 1]) < 1e-4f);
    CHECK(std::fabs(std::fabs(gn[3 * v + 2]) - 1.0f) < 1e-4f);
  }
}

void test_high_level_stability() {
  // Deep uniform refinement stays valid: finite, in-AABB, expected face growth.
  corpus::Mesh m = corpus::SingleTri();
  float lo[3] = {1e30f, 1e30f, 1e30f};
  float hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i + 3 <= m.points.size(); i += 3) {
    for (int c = 0; c < 3; c++) {
      lo[c] = std::min(lo[c], m.points[i + size_t(c)]);
      hi[c] = std::max(hi[c], m.points[i + size_t(c)]);
    }
  }
  Options opts;
  opts.level = 6;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(m), opts, &out, &err) == Result::Success);
  CHECK(out.face_vertex_counts.size() == 3u * 1024u);  // one 3-gon -> 3*4^5
  for (size_t i = 0; i < out.points.size(); i++) {
    CHECK(std::isfinite(out.points[i]));
    const int c = int(i % 3);
    CHECK(out.points[i] >= lo[c] - 1e-3f && out.points[i] <= hi[c] + 1e-3f);
  }
}

void test_holes_propagate_multilevel() {
  corpus::Mesh m = corpus::CubeWithHoles();  // faces 1, 4 are holes
  Options opts;
  opts.level = 2;
  RefinedMesh out;
  std::string err;
  CHECK(Refine(ToView(m), opts, &out, &err) == Result::Success);
  CHECK(out.face_vertex_counts.size() == (6u - 2u) * 16u);  // 64
  for (uint32_t s : out.face_source) {
    CHECK(s != 1 && s != 4);
  }
}

// ---------------------------------------------------------------------------
// Streaming refinement: RefineStream must be bit-identical to bulk Refine at
// matching canonical child-vertex ids (StreamBatch::vertex_source).
// ---------------------------------------------------------------------------

struct StreamCollect {
  uint32_t npoints = 0;       // bulk point count
  uint32_t pv_stride = 0;     // single vertex primvar stride (0 = none)
  std::vector<float> points;  // global, indexed by canonical id
  std::vector<float> pv;      // global
  std::vector<uint8_t> written;
  std::vector<uint32_t> face_indices;  // canonical, emission order
  std::vector<uint32_t> face_counts;
  std::vector<uint32_t> face_source;
  // Normals (global by canonical id) + a seam check: a canonical id emitted in
  // multiple batches must carry the same normal each time.
  bool has_normals = false;
  bool seam_ok = true;
  std::vector<float> normals;
  std::vector<uint8_t> n_written;
};

bool StreamSinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  StreamCollect *c = static_cast<StreamCollect *>(user);
  for (uint32_t i = 0; i < b->num_vertices; i++) {
    const uint32_t gid = b->vertex_source[i];
    if (gid < c->npoints) {
      c->written[gid] = 1;
      for (int k = 0; k < 3; k++) {
        c->points[size_t(gid) * 3 + size_t(k)] = b->positions[size_t(i) * 3 + size_t(k)];
      }
      if (b->normals && gid < c->n_written.size()) {
        c->has_normals = true;
        if (c->n_written[gid]) {
          for (int k = 0; k < 3; k++) {
            if (c->normals[size_t(gid) * 3 + size_t(k)] !=
                b->normals[size_t(i) * 3 + size_t(k)]) {
              c->seam_ok = false;
            }
          }
        } else {
          c->n_written[gid] = 1;
          for (int k = 0; k < 3; k++) {
            c->normals[size_t(gid) * 3 + size_t(k)] =
                b->normals[size_t(i) * 3 + size_t(k)];
          }
        }
      }
      if (c->pv_stride && b->num_vertex_primvars == 1) {
        for (uint32_t k = 0; k < c->pv_stride; k++) {
          c->pv[size_t(gid) * c->pv_stride + k] =
              b->vertex_primvars[0].values[size_t(i) * c->pv_stride + k];
        }
      }
    }
  }
  const uint32_t arity = b->num_faces ? (b->num_indices / b->num_faces) : 0;
  for (uint32_t f = 0; f < b->num_faces; f++) {
    c->face_counts.push_back(arity);
    c->face_source.push_back(b->face_source[f]);
    for (uint32_t k = 0; k < arity; k++) {
      const uint32_t local = b->indices[size_t(f) * arity + k];
      c->face_indices.push_back(b->vertex_source[local]);
    }
  }
  return true;
}

void test_stream_matches_bulk() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::Cube());
  meshes.push_back(corpus::SingleQuad());
  meshes.push_back(corpus::QuadGrid(3, 3, "sg"));
  meshes.push_back(corpus::MixedDegree());
  meshes.push_back(corpus::CubeWithHoles());
  meshes.push_back(corpus::CreasedCube(2.0f, "scc"));
  meshes.push_back(corpus::PerEdgeCreasedCube());
  meshes.push_back(corpus::CorneredGrid());
  meshes.push_back(corpus::Icosahedron());
  meshes.push_back(corpus::TriGrid(3, 3, "stg"));
  meshes.push_back(corpus::CreasedTriGrid());

  const BoundaryInterpolation boundaries[3] = {
      BoundaryInterpolation::EdgeAndCorner, BoundaryInterpolation::EdgeOnly,
      BoundaryInterpolation::None};
  const uint32_t batch_sizes[3] = {1u, 3u, 1u << 20};

  for (const corpus::Mesh &m : meshes) {
    std::vector<Scheme> schemes = {Scheme::CatmullClark, Scheme::Bilinear};
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) {
      all_tris = all_tris && (c == 3);
    }
    if (all_tris) {
      schemes.push_back(Scheme::Loop);
    }
    // The mesh's own points as a smooth stride-3 vertex primvar.
    std::vector<float> pvdata = m.points;
    VertexPrimvarView pv;
    pv.values = pvdata.data();
    pv.stride = 3;
    pv.varying = false;

    for (Scheme scheme : schemes) {
      for (BoundaryInterpolation boundary : boundaries) {
        for (int level = 1; level <= 3; level++) {
          Options opts;
          opts.scheme = scheme;
          opts.boundary = boundary;
          opts.level = level;
          opts.remove_holes = false;  // compare full topology

          RefinedMesh bulk;
          std::string err;
          const Result rb =
              Refine(ToView(m), nullptr, 0, &pv, 1, opts, &bulk, &err);
          CHECK_MSG(rb == Result::Success, m.name + ": " + err);
          if (rb != Result::Success) {
            continue;
          }

          for (uint32_t bs : batch_sizes) {
            StreamOptions so;
            so.batch_faces = bs;
            so.emit_triangles = false;  // native arity for exact comparison
            so.dedup_within_batch = true;
            so.want_normals = false;

            StreamCollect col;
            col.npoints = uint32_t(bulk.points.size() / 3);
            col.pv_stride = 3;
            col.points.assign(bulk.points.size(), 0.0f);
            col.pv.assign(bulk.vertex_primvars[0].size(), 0.0f);
            col.written.assign(col.npoints, 0);

            const Result rs = RefineStream(ToView(m), nullptr, 0, &pv, 1, opts,
                                           so, StreamSinkFn, &col, &err);
            CHECK_MSG(rs == Result::Success, m.name + ": " + err);
            if (rs != Result::Success) {
              continue;
            }

            const std::string tag =
                m.name + "/L" + std::to_string(level) + "/bs" + std::to_string(bs);
            // Topology: faces and provenance match bulk exactly, in order.
            CHECK_MSG(col.face_counts == bulk.face_vertex_counts, tag);
            CHECK_MSG(col.face_indices == bulk.face_vertex_indices, tag);
            CHECK_MSG(col.face_source == bulk.face_source, tag);
            // Every canonical vertex emitted, values bit-identical.
            bool all_written = true;
            for (uint8_t w : col.written) {
              all_written = all_written && (w != 0);
            }
            CHECK_MSG(all_written, tag);
            CHECK_MSG(col.points == bulk.points, tag);
            CHECK_MSG(col.pv == bulk.vertex_primvars[0], tag);
          }
        }
      }
    }
  }
}

void test_stream_level0_passthrough() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;
  corpus::Mesh m = corpus::Cube();
  Options opts;
  opts.level = 0;
  StreamOptions so;
  so.emit_triangles = false;
  StreamCollect col;
  col.npoints = uint32_t(m.points.size() / 3);
  col.points.assign(m.points.size(), 0.0f);
  col.written.assign(col.npoints, 0);
  std::string err;
  CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so, StreamSinkFn, &col,
                     &err) == Result::Success);
  CHECK(col.face_counts.size() == 6);
  CHECK(col.points == m.points);
}

void test_stream_normals() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::Cube());
  meshes.push_back(corpus::Icosahedron());
  meshes.push_back(corpus::CreasedCube(2.0f, "ncc"));
  meshes.push_back(corpus::QuadGrid(3, 3, "ng"));

  const uint32_t batch_sizes[2] = {1u, 1u << 20};
  for (const corpus::Mesh &m : meshes) {
    std::vector<Scheme> schemes = {Scheme::CatmullClark};
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) {
      all_tris = all_tris && (c == 3);
    }
    if (all_tris) {
      schemes.push_back(Scheme::Loop);
    }
    for (Scheme scheme : schemes) {
      for (uint32_t bs : batch_sizes) {
        Options opts;
        opts.scheme = scheme;
        opts.level = 3;
        opts.remove_holes = false;
        // Size from a bulk refine.
        RefinedMesh bulk;
        std::string err;
        CHECK(Refine(ToView(m), opts, &bulk, &err) == Result::Success);

        StreamOptions so;
        so.batch_faces = bs;
        so.emit_triangles = true;
        so.want_normals = true;
        StreamCollect col;
        col.npoints = uint32_t(bulk.points.size() / 3);
        col.points.assign(bulk.points.size(), 0.0f);
        col.written.assign(col.npoints, 0);
        col.normals.assign(bulk.points.size(), 0.0f);
        col.n_written.assign(col.npoints, 0);
        CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so, StreamSinkFn, &col,
                           &err) == Result::Success);
        const std::string tag = m.name + "/bs" + std::to_string(bs);
        CHECK_MSG(col.has_normals, tag);
        // Seamless: a canonical id duplicated across batches has one normal.
        CHECK_MSG(col.seam_ok, tag);
        // Every emitted normal is unit length.
        bool unit = true;
        for (uint32_t v = 0; v < col.npoints; v++) {
          if (!col.n_written[v]) {
            continue;
          }
          const float len = std::sqrt(col.normals[3 * v] * col.normals[3 * v] +
                                      col.normals[3 * v + 1] * col.normals[3 * v + 1] +
                                      col.normals[3 * v + 2] * col.normals[3 * v + 2]);
          unit = unit && (std::fabs(len - 1.0f) < 1e-3f);
        }
        CHECK_MSG(unit, tag);
      }
    }
  }

  // Planar grid: every streamed limit normal is +/- z (exact).
  corpus::Mesh grid = corpus::QuadGrid(4, 4, "nflat");
  for (size_t i = 0; i < grid.points.size() / 3; i++) {
    grid.points[i * 3 + 2] = 0.0f;
  }
  Options opts;
  opts.level = 2;
  opts.remove_holes = false;
  RefinedMesh bulk;
  std::string err;
  CHECK(Refine(ToView(grid), opts, &bulk, &err) == Result::Success);
  StreamOptions so;
  so.want_normals = true;
  so.emit_triangles = true;
  StreamCollect col;
  col.npoints = uint32_t(bulk.points.size() / 3);
  col.points.assign(bulk.points.size(), 0.0f);
  col.written.assign(col.npoints, 0);
  col.normals.assign(bulk.points.size(), 0.0f);
  col.n_written.assign(col.npoints, 0);
  CHECK(RefineStream(ToView(grid), nullptr, 0, nullptr, 0, opts, so, StreamSinkFn, &col,
                     &err) == Result::Success);
  for (uint32_t v = 0; v < col.npoints; v++) {
    if (!col.n_written[v]) {
      continue;
    }
    CHECK(std::fabs(col.normals[3 * v]) < 1e-4f);
    CHECK(std::fabs(col.normals[3 * v + 1]) < 1e-4f);
    CHECK(std::fabs(std::fabs(col.normals[3 * v + 2]) - 1.0f) < 1e-4f);
  }
}

// ---------------------------------------------------------------------------
// Block + halo: welded blocked output must equal whole-mesh bulk Refine.
// (Owned-face vertices are bit-identical to bulk when the halo gives full
// stencil support, so we match emitted positions to bulk positions exactly --
// this is the halo-radius safety net.)
// ---------------------------------------------------------------------------

struct BlockCollect {
  std::vector<float> positions;       // flat, all emitted vertices (with dup)
  std::vector<uint32_t> face_arity;
  std::vector<uint32_t> face_source;
  std::vector<uint32_t> face_corners;  // flat, global emitted-vertex ids
};

bool BlockSinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  BlockCollect *c = static_cast<BlockCollect *>(user);
  const uint32_t vbase = uint32_t(c->positions.size() / 3);
  for (uint32_t i = 0; i < b->num_vertices * 3; i++) {
    c->positions.push_back(b->positions[i]);
  }
  const uint32_t arity = b->num_faces ? (b->num_indices / b->num_faces) : 0;
  for (uint32_t f = 0; f < b->num_faces; f++) {
    c->face_arity.push_back(arity);
    c->face_source.push_back(b->face_source[f]);
    for (uint32_t k = 0; k < arity; k++) {
      c->face_corners.push_back(vbase + b->indices[size_t(f) * arity + k]);
    }
  }
  return true;
}

std::vector<uint32_t> CanonFace(std::vector<uint32_t> ids, uint32_t src) {
  uint32_t best = 0;
  for (size_t k = 1; k < ids.size(); k++) {
    if (ids[k] < ids[best]) best = k;
  }
  std::vector<uint32_t> out;
  for (size_t k = 0; k < ids.size(); k++) {
    out.push_back(ids[(best + k) % ids.size()]);
  }
  out.push_back(0x80000000u | src);  // tag source distinctly from vertex ids
  return out;
}

void test_stream_blocked_matches_bulk() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::Cube());
  meshes.push_back(corpus::QuadGrid(4, 4, "bg"));
  meshes.push_back(corpus::MixedDegree());
  meshes.push_back(corpus::CreasedCube(2.0f, "bcc"));
  meshes.push_back(corpus::CubeWithHoles());
  meshes.push_back(corpus::CorneredGrid());
  meshes.push_back(corpus::Icosahedron());
  meshes.push_back(corpus::TriGrid(4, 4, "btg"));
  meshes.push_back(corpus::CreasedTriGrid());

  const BoundaryInterpolation boundaries[2] = {
      BoundaryInterpolation::EdgeAndCorner, BoundaryInterpolation::None};
  const uint32_t block_sizes[3] = {1u, 2u, 3u};

  for (const corpus::Mesh &m : meshes) {
    std::vector<Scheme> schemes = {Scheme::CatmullClark, Scheme::Bilinear};
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) {
      all_tris = all_tris && (c == 3);
    }
    if (all_tris) {
      schemes.push_back(Scheme::Loop);
    }
    for (Scheme scheme : schemes) {
      for (BoundaryInterpolation boundary : boundaries) {
        for (int level = 1; level <= 3; level++) {
          Options opts;
          opts.scheme = scheme;
          opts.boundary = boundary;
          opts.level = level;
          opts.remove_holes = true;

          RefinedMesh bulk;
          std::string err;
          CHECK(Refine(ToView(m), opts, &bulk, &err) == Result::Success);

          // Exact position -> bulk vertex id.
          std::map<std::array<float, 3>, uint32_t> posmap;
          for (uint32_t v = 0; v < bulk.points.size() / 3; v++) {
            posmap[{bulk.points[3 * v], bulk.points[3 * v + 1],
                    bulk.points[3 * v + 2]}] = v;
          }
          // Bulk face multiset (canonicalized, with source).
          std::map<std::vector<uint32_t>, int> bulk_faces;
          size_t off = 0;
          for (size_t f = 0; f < bulk.face_vertex_counts.size(); f++) {
            const uint32_t n = bulk.face_vertex_counts[f];
            std::vector<uint32_t> ids(bulk.face_vertex_indices.begin() + off,
                                      bulk.face_vertex_indices.begin() + off + n);
            off += n;
            bulk_faces[CanonFace(ids, bulk.face_source[f])]++;
          }

          for (uint32_t bf : block_sizes) {
           // recursive_blocks is an opt-in memory/time trade; its output must be
           // bit-identical to the default (materializing) block path -> bulk.
           for (int rec = 0; rec < 2; rec++) {
            StreamOptions so;
            so.block_faces = bf;
            so.emit_triangles = false;
            so.want_normals = false;
            so.recursive_blocks = (rec == 1);
            BlockCollect col;
            CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so, BlockSinkFn,
                               &col, &err) == Result::Success);

            const std::string tag = m.name + "/L" + std::to_string(level) +
                                    "/bf" + std::to_string(bf) +
                                    (rec ? "/rec" : "");
            // Every emitted vertex matches a bulk vertex exactly.
            const uint32_t ev = uint32_t(col.positions.size() / 3);
            std::vector<uint32_t> to_bulk(ev, 0xFFFFFFFFu);
            bool all_found = true;
            for (uint32_t e = 0; e < ev; e++) {
              std::array<float, 3> key = {col.positions[3 * e],
                                          col.positions[3 * e + 1],
                                          col.positions[3 * e + 2]};
              auto it = posmap.find(key);
              if (it == posmap.end()) {
                all_found = false;
              } else {
                to_bulk[e] = it->second;
              }
            }
            CHECK_MSG(all_found, tag);

            // Blocked face multiset (mapped into bulk ids) equals bulk's.
            std::map<std::vector<uint32_t>, int> blk_faces;
            size_t coff = 0;
            bool ok = all_found;
            for (size_t f = 0; f < col.face_arity.size(); f++) {
              const uint32_t n = col.face_arity[f];
              std::vector<uint32_t> ids;
              for (uint32_t k = 0; k < n; k++) {
                const uint32_t e = col.face_corners[coff + k];
                if (e >= ev || to_bulk[e] == 0xFFFFFFFFu) {
                  ok = false;
                } else {
                  ids.push_back(to_bulk[e]);
                }
              }
              coff += n;
              if (ids.size() == n) {
                blk_faces[CanonFace(ids, col.face_source[f])]++;
              }
            }
            CHECK_MSG(ok && (blk_faces == bulk_faces), tag);
           }
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Linear faceVarying streaming: per-corner fvar must equal bulk Refine's.
// ---------------------------------------------------------------------------

struct FvarCollect {
  uint32_t stride = 0;
  std::vector<float> fvar;  // concatenated per-corner, emission order
};

bool FvarSinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  FvarCollect *c = static_cast<FvarCollect *>(user);
  if (b->num_fvar == 1) {
    const float *v = b->fvar[0].values;
    const size_t n = size_t(b->num_indices) * c->stride;
    for (size_t i = 0; i < n; i++) {
      c->fvar.push_back(v[i]);
    }
  }
  return true;
}

void test_stream_fvar() {
  using lightusd::tsd::FVarChannelView;
  using lightusd::tsd::FVarLinearInterpolation;
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::UVSeamGrid());
  meshes.push_back(corpus::UVCube());
  meshes.push_back(corpus::UVTriGrid());

  // Non-block streaming carries linear fvar per corner and smooth seam-split fvar
  // through a per-channel split mesh; both are bit-identical to bulk Refine AND
  // emitted in the same per-parent-face/corner order, so the concatenated
  // per-corner fvar equals bulk's exactly. Linear modes are tested under bilinear
  // (where every mode is linear) and the smooth modes under the smooth scheme.
  const FVarLinearInterpolation smooth_modes[5] = {
      FVarLinearInterpolation::CornersPlus1, FVarLinearInterpolation::None,
      FVarLinearInterpolation::CornersOnly, FVarLinearInterpolation::CornersPlus2,
      FVarLinearInterpolation::Boundaries};
  const uint32_t batch_sizes[2] = {3u, 1u << 20};

  for (const corpus::Mesh &m : meshes) {
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) all_tris = all_tris && (c == 3);
    const Scheme smooth = all_tris ? Scheme::Loop : Scheme::CatmullClark;

    struct Case {
      Scheme scheme;
      FVarLinearInterpolation mode;
    };
    std::vector<Case> cases;
    cases.push_back({Scheme::Bilinear, FVarLinearInterpolation::All});  // linear
    cases.push_back({smooth, FVarLinearInterpolation::All});            // linear
    for (FVarLinearInterpolation mode : smooth_modes) {
      cases.push_back({smooth, mode});  // smooth seam-split
    }

    for (const Case &cs : cases) {
      FVarChannelView fv;
      fv.values = m.fvar_uv.data();
      fv.num_values = uint32_t(m.fvar_uv.size() / 2);
      fv.indices = m.fvar_indices.empty() ? nullptr : m.fvar_indices.data();
      fv.stride = 2;
      fv.interpolation = cs.mode;

      for (int level = 1; level <= 3; level++) {
        Options opts;
        opts.scheme = cs.scheme;
        opts.level = level;
        opts.remove_holes = false;

        RefinedMesh bulk;
        std::string err;
        CHECK(Refine(ToView(m), &fv, 1, nullptr, 0, opts, &bulk, &err) ==
              Result::Success);
        CHECK(bulk.fvar.size() == 1);

        for (uint32_t bs : batch_sizes) {
          StreamOptions so;
          so.batch_faces = bs;
          so.emit_triangles = false;  // native faces -> same corner order
          so.want_normals = false;
          FvarCollect col;
          col.stride = 2;
          CHECK(RefineStream(ToView(m), &fv, 1, nullptr, 0, opts, so, FvarSinkFn,
                             &col, &err) == Result::Success);
          const std::string tag = m.name + "/" +
                                  std::to_string(int(cs.mode)) + "/L" +
                                  std::to_string(level) + "/bs" +
                                  std::to_string(bs);
          CHECK_MSG(col.fvar == bulk.fvar[0], tag);  // bit-exact, in order
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Block-mode faceVarying: ALL modes (linear + smooth seam-split) stream in block
// mode, because each block is refined with bulk Refine (which handles every fvar
// mode). Positions are bit-exact to bulk, so match each emitted corner to the
// bulk corners at the same position; the streamed fvar must equal one of those
// bulk fvar tuples (fp noise only). A seam error or short halo diverges by O(1).
// ---------------------------------------------------------------------------

struct BlockFvarCollect {
  // position -> the set of bulk fvar tuples seen at it (seams => multi-valued).
  const std::multimap<std::array<float, 3>, std::array<float, 2>> *bulk = nullptr;
  double maxd = 0.0;
  long miss = 0, tot = 0;
};

bool BlockFvarSinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  BlockFvarCollect *c = static_cast<BlockFvarCollect *>(user);
  if (b->num_fvar != 1) return true;
  uint32_t off = 0;
  for (uint32_t f = 0; f < b->num_faces; f++) {
    const uint32_t n = b->face_vertex_counts ? b->face_vertex_counts[f]
                                             : (b->num_indices / b->num_faces);
    for (uint32_t k = 0; k < n; k++) {
      const uint32_t corner = off + k;
      const uint32_t vid = b->indices[corner];
      std::array<float, 3> p = {b->positions[3 * vid], b->positions[3 * vid + 1],
                                b->positions[3 * vid + 2]};
      std::array<float, 2> uv = {b->fvar[0].values[corner * 2],
                                 b->fvar[0].values[corner * 2 + 1]};
      c->tot++;
      auto range = c->bulk->equal_range(p);
      double best = 1e30;
      for (auto it = range.first; it != range.second; ++it) {
        const double d = std::fabs(double(it->second[0]) - uv[0]) +
                         std::fabs(double(it->second[1]) - uv[1]);
        if (d < best) best = d;
      }
      if (best > 1e29) {
        c->miss++;
      } else if (best > c->maxd) {
        c->maxd = best;
      }
    }
    off += n;
  }
  return true;
}

void test_stream_blocked_fvar() {
  using lightusd::tsd::FVarChannelView;
  using lightusd::tsd::FVarLinearInterpolation;
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::UVSeamGrid());
  meshes.push_back(corpus::UVCube());
  meshes.push_back(corpus::UVTriGrid());

  const FVarLinearInterpolation modes[6] = {
      FVarLinearInterpolation::CornersPlus1,  // smooth (default)
      FVarLinearInterpolation::None,          // smooth everywhere
      FVarLinearInterpolation::CornersOnly,   // smooth
      FVarLinearInterpolation::CornersPlus2,  // smooth
      FVarLinearInterpolation::Boundaries,    // smooth
      FVarLinearInterpolation::All};          // linear
  const uint32_t block_sizes[3] = {1u, 2u, 3u};

  for (const corpus::Mesh &m : meshes) {
    bool all_tris = true;
    for (uint32_t c : m.face_vertex_counts) all_tris = all_tris && (c == 3);
    const Scheme scheme = all_tris ? Scheme::Loop : Scheme::CatmullClark;

    for (int mi = 0; mi < 6; mi++) {
      for (int level = 1; level <= 3; level++) {
        FVarChannelView fv;
        fv.values = m.fvar_uv.data();
        fv.num_values = uint32_t(m.fvar_uv.size() / 2);
        fv.indices = m.fvar_indices.empty() ? nullptr : m.fvar_indices.data();
        fv.stride = 2;
        fv.interpolation = modes[mi];

        Options opts;
        opts.scheme = scheme;
        opts.level = level;
        opts.remove_holes = false;

        RefinedMesh bulk;
        std::string err;
        CHECK(Refine(ToView(m), &fv, 1, nullptr, 0, opts, &bulk, &err) ==
              Result::Success);
        // bulk corner i: position points[fvi[i]], fvar value fvar[0][i*2].
        std::multimap<std::array<float, 3>, std::array<float, 2>> bm;
        for (size_t i = 0; i < bulk.face_vertex_indices.size(); i++) {
          const uint32_t vid = bulk.face_vertex_indices[i];
          bm.insert({{bulk.points[3 * vid], bulk.points[3 * vid + 1],
                      bulk.points[3 * vid + 2]},
                     {bulk.fvar[0][i * 2], bulk.fvar[0][i * 2 + 1]}});
        }

        for (uint32_t bf : block_sizes) {
          StreamOptions so;
          so.block_faces = bf;
          so.want_normals = false;
          so.emit_triangles = false;
          BlockFvarCollect col;
          col.bulk = &bm;
          CHECK(RefineStream(ToView(m), &fv, 1, nullptr, 0, opts, so,
                             BlockFvarSinkFn, &col, &err) == Result::Success);
          const std::string tag = m.name + "/m" + std::to_string(mi) + "/L" +
                                  std::to_string(level) + "/bf" +
                                  std::to_string(bf);
          CHECK_MSG(col.tot > 0, tag);
          CHECK_MSG(col.miss == 0, tag);   // every corner's pos+uv is a bulk pair
          CHECK_MSG(col.maxd < 1e-4, tag);  // fp noise only, no seam divergence
        }
      }
    }
  }

}

// ---------------------------------------------------------------------------
// Level-0 faceVarying: a passthrough emits the base channel verbatim (every
// mode, native or fan-triangulated). Each emitted corner's (position, fvar) must
// be a base/bulk corner pair. (Previously rejected; reuses the block-fvar sink.)
// ---------------------------------------------------------------------------

void test_stream_level0_fvar() {
  using lightusd::tsd::FVarChannelView;
  using lightusd::tsd::FVarLinearInterpolation;
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::UVSeamGrid());
  meshes.push_back(corpus::UVCube());
  meshes.push_back(corpus::UVTriGrid());
  // Mode is irrelevant at level 0 (no refinement); spot-check a smooth, a
  // smooth-corners, and the linear mode.
  const FVarLinearInterpolation modes[3] = {FVarLinearInterpolation::CornersPlus1,
                                            FVarLinearInterpolation::None,
                                            FVarLinearInterpolation::All};

  for (const corpus::Mesh &m : meshes) {
    for (int mi = 0; mi < 3; mi++) {
      FVarChannelView fv;
      fv.values = m.fvar_uv.data();
      fv.num_values = uint32_t(m.fvar_uv.size() / 2);
      fv.indices = m.fvar_indices.empty() ? nullptr : m.fvar_indices.data();
      fv.stride = 2;
      fv.interpolation = modes[mi];

      Options opts;
      opts.scheme = Scheme::CatmullClark;  // level 0 is passthrough; scheme moot
      opts.level = 0;
      opts.remove_holes = false;

      RefinedMesh bulk;
      std::string err;
      CHECK(Refine(ToView(m), &fv, 1, nullptr, 0, opts, &bulk, &err) ==
            Result::Success);
      std::multimap<std::array<float, 3>, std::array<float, 2>> bm;
      for (size_t i = 0; i < bulk.face_vertex_indices.size(); i++) {
        const uint32_t vid = bulk.face_vertex_indices[i];
        bm.insert({{bulk.points[3 * vid], bulk.points[3 * vid + 1],
                    bulk.points[3 * vid + 2]},
                   {bulk.fvar[0][i * 2], bulk.fvar[0][i * 2 + 1]}});
      }

      for (int et = 0; et < 2; et++) {  // native faces and fan-triangulated
        StreamOptions so;
        so.emit_triangles = (et == 1);
        so.want_normals = false;
        BlockFvarCollect col;
        col.bulk = &bm;
        CHECK(RefineStream(ToView(m), &fv, 1, nullptr, 0, opts, so,
                           BlockFvarSinkFn, &col, &err) == Result::Success);
        const std::string tag =
            m.name + "/m" + std::to_string(mi) + "/et" + std::to_string(et);
        CHECK_MSG(col.tot > 0, tag);
        CHECK_MSG(col.miss == 0, tag);
        CHECK_MSG(col.maxd < 1e-6, tag);  // passthrough => exact
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Audit regressions: non-uniform arity reporting + block-mode sink abort.
// ---------------------------------------------------------------------------

struct MixedArityCollect {
  bool saw_counts = false;
  std::vector<uint32_t> arities;    // per emitted face
  std::vector<uint32_t> face_idx;   // flat canonical face vertex ids
};

bool MixedAritySinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  MixedArityCollect *c = static_cast<MixedArityCollect *>(user);
  if (b->face_vertex_counts) c->saw_counts = true;
  size_t off = 0;
  for (uint32_t f = 0; f < b->num_faces; f++) {
    const uint32_t n = b->face_vertex_counts
                           ? b->face_vertex_counts[f]
                           : (b->num_indices / b->num_faces);
    c->arities.push_back(n);
    for (uint32_t k = 0; k < n; k++) {
      c->face_idx.push_back(b->vertex_source[b->indices[off + k]]);
    }
    off += n;
  }
  return true;
}

// Level-0 passthrough of a mixed-degree mesh with native (non-triangulated)
// faces must report per-face arity, not claim uniform arity.
void test_stream_level0_mixed_arity() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;
  corpus::Mesh m = corpus::MixedDegree();  // face_vertex_counts {4,3,3,5}
  Options opts;
  opts.level = 0;
  opts.remove_holes = false;
  StreamOptions so;
  so.emit_triangles = false;
  MixedArityCollect col;
  std::string err;
  CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so,
                     MixedAritySinkFn, &col, &err) == Result::Success);
  CHECK(col.saw_counts);  // non-uniform batch reports face_vertex_counts
  CHECK(col.arities ==
        std::vector<uint32_t>(m.face_vertex_counts.begin(),
                              m.face_vertex_counts.end()));
  CHECK(col.face_idx ==
        std::vector<uint32_t>(m.face_vertex_indices.begin(),
                              m.face_vertex_indices.end()));
}

struct AbortCtx {
  int calls = 0;
};
bool AbortSinkFn(void *user, const lightusd::tsd::StreamBatch *) {
  static_cast<AbortCtx *>(user)->calls++;
  return false;  // abort immediately
}

// Block mode must stop refining further blocks once the sink aborts.
void test_stream_block_abort() {
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;
  corpus::Mesh m = corpus::QuadGrid(6, 6, "abort");  // 36 base faces
  Options opts;
  opts.level = 2;
  StreamOptions so;
  so.block_faces = 4;  // 9 blocks
  AbortCtx ctx;
  std::string err;
  CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so, AbortSinkFn,
                     &ctx, &err) == Result::Success);
  CHECK(ctx.calls == 1);  // stopped after the first batch, not all 9 blocks
}

// ---------------------------------------------------------------------------
// Block + halo with normals: every owned-face limit normal must equal whole-
// mesh bulk ComputeLimitNormals at the same position. Block mode emits TRUE
// limit normals (a per-block ComputeLimitNormals), so a vertex shared between
// blocks shades identically -> shading is seamless across borders. The
// want_normals block path was previously uncovered (test_stream_blocked_matches
// _bulk asserts positions/topology only). Curved/creased/irregular meshes at
// several levels give the normals something to actually vary over.
// ---------------------------------------------------------------------------

struct NormalCollect {
  std::vector<float> positions;  // flat xyz, all emitted verts (with dup)
  std::vector<float> normals;    // flat xyz, parallel to positions
  bool had_normals = true;
};

bool NormalSinkFn(void *user, const lightusd::tsd::StreamBatch *b) {
  NormalCollect *c = static_cast<NormalCollect *>(user);
  if (!b->normals) {
    c->had_normals = false;
    return true;
  }
  for (uint32_t v = 0; v < b->num_vertices; v++) {
    c->positions.push_back(b->positions[3 * v]);
    c->positions.push_back(b->positions[3 * v + 1]);
    c->positions.push_back(b->positions[3 * v + 2]);
    c->normals.push_back(b->normals[3 * v]);
    c->normals.push_back(b->normals[3 * v + 1]);
    c->normals.push_back(b->normals[3 * v + 2]);
  }
  return true;
}

void test_stream_blocked_normals() {
  using lightusd::tsd::ComputeLimitNormals;
  using lightusd::tsd::RefineStream;
  using lightusd::tsd::StreamOptions;

  std::vector<corpus::Mesh> meshes;
  meshes.push_back(corpus::QuadTorus(10, 8));       // closed, curved, all valence-4
  meshes.push_back(corpus::CreasedCube(2.0f, "bncc"));  // semi-sharp creases
  meshes.push_back(corpus::Icosahedron());          // extraordinary verts (val 5)
  meshes.push_back(corpus::TriGrid(8, 8, "bntg"));  // boundary + curvature

  const uint32_t block_sizes[2] = {1u, 3u};
  // halo_rings 1 == the proven-sufficient floor (a single base-face ring); 0 ==
  // the default (2). Exercising 1 directly guards that the minimal halo still
  // yields bulk-identical positions AND limit normals at every level.
  const uint32_t halo_opts[2] = {0u, 1u};

  for (const corpus::Mesh &m : meshes) {
    std::vector<Scheme> schemes = {Scheme::CatmullClark};
    bool all_tris = true;
    for (uint32_t cnt : m.face_vertex_counts) all_tris = all_tris && (cnt == 3);
    if (all_tris) schemes.push_back(Scheme::Loop);

    for (Scheme scheme : schemes) {
      for (int level = 1; level <= 3; level++) {
        Options opts;
        opts.scheme = scheme;
        opts.level = level;
        opts.remove_holes = true;

        RefinedMesh bulk;
        std::string err;
        CHECK(Refine(ToView(m), opts, &bulk, &err) == Result::Success);
        std::vector<float> bulk_n;
        CHECK(ComputeLimitNormals(ToView(m), opts, bulk, &bulk_n, &err) ==
              Result::Success);
        // Exact position -> bulk limit normal.
        std::map<std::array<float, 3>, std::array<float, 3>> nmap;
        for (uint32_t v = 0; v < bulk.points.size() / 3; v++) {
          nmap[{bulk.points[3 * v], bulk.points[3 * v + 1],
                bulk.points[3 * v + 2]}] = {
              bulk_n[3 * v], bulk_n[3 * v + 1], bulk_n[3 * v + 2]};
        }

        for (uint32_t bf : block_sizes) {
          for (uint32_t hr : halo_opts) {
            StreamOptions so;
            so.block_faces = bf;
            so.halo_rings = hr;
            so.want_normals = true;
            so.emit_triangles = false;
            NormalCollect col;
            CHECK(RefineStream(ToView(m), nullptr, 0, nullptr, 0, opts, so,
                               NormalSinkFn, &col, &err) == Result::Success);

            const std::string tag =
                m.name + "/" + (scheme == Scheme::Loop ? "loop" : "cc") + "/L" +
                std::to_string(level) + "/bf" + std::to_string(bf) + "/hr" +
                std::to_string(hr);
            CHECK_MSG(col.had_normals, tag);
            const uint32_t ev = uint32_t(col.positions.size() / 3);
            CHECK_MSG(ev > 0, tag);
            double maxdiff = 0.0;
            bool all_found = true;
            for (uint32_t e = 0; e < ev; e++) {
              std::array<float, 3> key = {col.positions[3 * e],
                                          col.positions[3 * e + 1],
                                          col.positions[3 * e + 2]};
              auto it = nmap.find(key);
              if (it == nmap.end()) {
                all_found = false;
                continue;
              }
              const double dx = double(col.normals[3 * e]) - it->second[0];
              const double dy = double(col.normals[3 * e + 1]) - it->second[1];
              const double dz = double(col.normals[3 * e + 2]) - it->second[2];
              const double d = std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
              if (d > maxdiff) maxdiff = d;
            }
            CHECK_MSG(all_found, tag);
            // Block normals are the per-block ComputeLimitNormals; with full
            // stencil support they are bit-identical to whole-mesh bulk (fp
            // noise only). A border that lost support would diverge by O(0.1+).
            CHECK_MSG(maxdiff < 1e-4, tag);
          }
        }
      }
    }
  }
}

// ---- Adaptive bilinear refinement -------------------------------------

struct AdaptiveProbe {
  float px, py;     // refine faces containing this point
  int rounds = 0;
  bool abort_at_2 = false;
  uint32_t last_split = UINT32_MAX;
  int bad_parent = 0;
};

bool AdaptiveMarkContains(void *user, int32_t level,
                          const lightusd::tsd::AdaptiveMesh &m, uint32_t first,
                          uint32_t n, uint8_t *refine) {
  AdaptiveProbe *p = static_cast<AdaptiveProbe *>(user);
  p->rounds++;
  if (p->abort_at_2 && level == 2) return false;
  // Children point back at the single face split in the previous round.
  for (uint32_t i = 0; i < n; ++i)
    if (level > 0 && m.face_parent[first + i] != p->last_split) p->bad_parent++;
  p->last_split = UINT32_MAX;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t f = first + i, o = m.face_offsets[f];
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (uint32_t k = 0; k < m.face_vertex_counts[f]; ++k) {
      const float *q = &m.points[size_t(m.face_vertex_indices[o + k]) * 3];
      x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]);
      y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]);
    }
    refine[i] = (p->px >= x0 && p->px < x1 && p->py >= y0 && p->py < y1);
    if (refine[i]) p->last_split = i;
  }
  return true;
}

void test_adaptive_bilinear_quadtree() {
  using namespace lightusd::tsd;
  // Two unit quads side by side: [0,1]x[0,1] and [1,2]x[0,1].
  const float pts[] = {0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0};
  const uint32_t counts[] = {4, 4};
  const uint32_t fvi[] = {0, 1, 4, 3, 1, 2, 5, 4};
  MeshView v;
  v.points = pts; v.num_points = 6;
  v.face_vertex_counts = counts; v.num_faces = 2;
  v.face_vertex_indices = fvi; v.num_face_vertex_indices = 8;
  AdaptiveOptions ao;
  ao.max_level = 6;
  AdaptiveProbe probe{0.3f, 0.7f};
  AdaptiveMesh m;
  std::string err;
  CHECK_MSG(RefineAdaptive(v, ao, AdaptiveMarkContains, &probe, &m, &err) ==
                Result::Success, err);
  // One split per level along the probe path: 2 + 3*6 leaves.
  CHECK(m.face_vertex_counts.size() == 20);
  CHECK(probe.rounds == 7);
  CHECK(probe.bad_parent == 0);
  double area[2] = {0, 0};
  int deepest = 0, contains = 0;
  for (size_t f = 0; f < m.face_vertex_counts.size(); ++f) {
    CHECK(m.face_vertex_counts[f] == 4);
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (uint32_t k = 0; k < 4; ++k) {
      const float *q = &m.points[size_t(m.face_vertex_indices[m.face_offsets[f] + k]) * 3];
      x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]);
      y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]);
    }
    CHECK(m.face_source[f] < 2);
    CHECK(x0 >= float(m.face_source[f]) && x1 <= float(m.face_source[f] + 1));
    // Dyadic: each leaf is exactly 2^-level on a side.
    CHECK(x1 - x0 == std::ldexp(1.0f, -m.face_level[f]));
    CHECK(y1 - y0 == std::ldexp(1.0f, -m.face_level[f]));
    area[m.face_source[f]] += double(x1 - x0) * double(y1 - y0);
    if (m.face_level[f] == 6) {
      deepest++;
      contains += (probe.px >= x0 && probe.px < x1 && probe.py >= y0 && probe.py < y1);
    }
  }
  CHECK(area[0] == 1.0 && area[1] == 1.0);  // leaves partition each base face
  CHECK(deepest == 4 && contains == 1);
  // Every point is referenced (compacted).
  std::vector<char> used(m.points.size() / 3, 0);
  for (uint32_t i : m.face_vertex_indices) used[i] = 1;
  CHECK(std::count(used.begin(), used.end(), 0) == 0);

  // Cap and abort paths.
  ao.max_faces = 10;
  AdaptiveProbe p2{0.3f, 0.7f};
  CHECK(RefineAdaptive(v, ao, AdaptiveMarkContains, &p2, &m, &err) ==
        Result::LimitExceeded);
  ao.max_faces = 1u << 20;
  AdaptiveProbe p3{0.3f, 0.7f};
  p3.abort_at_2 = true;
  CHECK(RefineAdaptive(v, ao, AdaptiveMarkContains, &p3, &m, &err) ==
        Result::InvalidArgument);
  CHECK(RefineAdaptive(v, ao, nullptr, nullptr, &m, &err) ==
        Result::InvalidArgument);
  ao.max_level = kMaxAdaptiveLevel + 1;
  CHECK(RefineAdaptive(v, ao, AdaptiveMarkContains, &p2, &m, &err) ==
        Result::InvalidArgument);
}

}  // namespace

int main() {
  // Validation / hardening
  TEST(test_scheme_none_rejected);
  TEST(test_null_output_rejected);
  TEST(test_empty_mesh_rejected);
  TEST(test_bad_level_rejected);
  TEST(test_out_of_range_index_rejected);
  TEST(test_count_mismatch_rejected);
  TEST(test_bad_face_degree_rejected);
  TEST(test_crease_sharpness_count_rejected);
  TEST(test_hole_index_out_of_range_rejected);

  // Passthrough
  TEST(test_level0_passthrough);
  TEST(test_level0_fvar_expansion);

  // Refinement: analytic
  TEST(test_bilinear_quad_level1_exact);
  TEST(test_adaptive_bilinear_quadtree);
  TEST(test_catmark_cube_level1_smooth_vertex);
  TEST(test_infinite_corner_pinned);
  TEST(test_infinite_crease_midpoints);
  TEST(test_partition_of_unity);
  TEST(test_face_source_cube);
  TEST(test_holes_filtered);
  TEST(test_caps_enforced);
  TEST(test_base_caps_enforced_before_topology);
  TEST(test_fvar_split_caps_enforced);
  TEST(test_parallel_determinism);

  // FaceVarying smooth modes
  TEST(test_fvar_continuous_equals_vertex_refinement);
  TEST(test_fvar_island_constants_preserved);
  TEST(test_fvar_modes_differ_at_seams);

  // Loop scheme
  TEST(test_loop_rejects_non_tris);
  TEST(test_loop_regular_vertex_weights);
  TEST(test_loop_edge_smooth_rule);

  // Limit surface
  TEST(test_limit_regular_interior_mask);
  TEST(test_limit_corner_pinned);
  TEST(test_limit_bilinear_identity);
  TEST(test_limit_normals_unit_length);
  TEST(test_limit_caps_enforced);

  // Topology
  TEST(test_topology_cube);
  TEST(test_topology_single_quad_boundary);
  TEST(test_topology_mixed_degree);
  TEST(test_topology_non_manifold_rejected);
  TEST(test_topology_inconsistent_winding_rejected);
  TEST(test_topology_degenerate_edge_rejected);

  // Crease canonicalization
  TEST(test_crease_canonicalize_per_crease);
  TEST(test_crease_canonicalize_per_edge_and_dedup);
  TEST(test_crease_sharpness_clamped);

  // Broad invariant sweep + added analytic regressions
  TEST(test_invariant_sweep);
  TEST(test_nonmanifold_rejected_all_schemes);
  TEST(test_closed_stays_closed);
  TEST(test_semisharp_crease_transitional_blend);
  TEST(test_chaikin_formula_and_decay);
  TEST(test_boundary_edgeonly_vs_edgeandcorner);
  TEST(test_boundary_none_makes_holes);
  TEST(test_fvar_stride_and_indexing);
  TEST(test_varying_vs_vertex_primvar);
  TEST(test_vertex_primvar_equals_geometry);
  TEST(test_trisub_smooth_differs);
  TEST(test_limit_loop_and_planar_normals);
  TEST(test_high_level_stability);
  TEST(test_holes_propagate_multilevel);

  // Streaming refinement
  TEST(test_stream_matches_bulk);
  TEST(test_stream_level0_passthrough);
  TEST(test_stream_normals);
  TEST(test_stream_blocked_matches_bulk);
  TEST(test_stream_blocked_normals);
  TEST(test_stream_fvar);
  TEST(test_stream_blocked_fvar);
  TEST(test_stream_level0_fvar);
  TEST(test_stream_level0_mixed_arity);
  TEST(test_stream_block_abort);

  printf("feat-subdiv: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
