// SPDX-License-Identifier: Apache-2.0
#include "rt_lod.hh"
#include "static_chunk_cull.hh"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #expr);                                                   \
      ++failures;                                                            \
    }                                                                        \
  } while (false)

void TestStaticChunkProjection() {
  float view[16]{};
  view[0] = view[5] = view[10] = view[15] = 1.0f;
  float proj[16]{};
  proj[5] = -2.0f;
  proj[11] = -1.0f;
  const float center[3] = {100.0f, 0.0f, -10.0f};
  CHECK(std::fabs(lusdview::StaticChunkProjectedRadiusPx(
      center, 1.0f, view, proj, 900) - 100.0f) < 1e-5f);
  proj[5] = 2.0f;
  CHECK(std::fabs(lusdview::StaticChunkProjectedRadiusPx(
      center, 1.0f, view, proj, 1800) - 200.0f) < 1e-5f);
  CHECK(std::isinf(lusdview::StaticChunkProjectedRadiusPx(
      center, 11.0f, view, proj, 900)));
  proj[11] = 0.0f;
  proj[15] = 1.0f;
  CHECK(lusdview::StaticChunkProjectedRadiusPx(
      center, 1.0f, view, proj, 900) == 900.0f);
  proj[5] = 0.0f;
  CHECK(std::isinf(lusdview::StaticChunkProjectedRadiusPx(
      center, 1.0f, view, proj, 900)));
}

void TestSubpixelAggregateBound() {
  lusdview::RtLodGridCell cell{};
  cell.wmn[0] = cell.wmn[1] = -0.2f;
  cell.wmx[0] = cell.wmx[1] = 0.2f;
  cell.wmn[2] = -100.2f;
  cell.wmx[2] = -99.8f;
  cell.maxInstanceRadius = 0.05f;
  lusdview::RtLodCamera cam;
  cam.eye = {0.0f, 0.0f, 0.0f};
  cam.forward = {0.0f, 0.0f, -1.0f};
  cam.nearPlane = 0.1f;
  cam.focalPx = 1000.0f;
  cam.cullPx = 1.0f;
  CHECK(lusdview::IsSubpixelAggregateCell(cell, cam));
  cell.maxInstanceRadius = 0.2f;
  CHECK(!lusdview::IsSubpixelAggregateCell(cell, cam));
  cell.maxInstanceRadius = 0.05f;
  cell.wmn[0] = -1.0f;
  cell.wmx[0] = 1.0f;
  CHECK(!lusdview::IsSubpixelAggregateCell(cell, cam));
}

void TestGridTracksMaximumInstanceRadius() {
  constexpr uint32_t kInstances = 512;
  std::vector<float> xforms(kInstances * 12, 0.0f);
  for (uint32_t i = 0; i < kInstances; ++i) {
    float* x = &xforms[i * 12];
    x[0] = x[5] = x[10] = 1.0f;
    x[3] = static_cast<float>(i * 10);
  }
  const float lo[3] = {-1.0f, -1.0f, -1.0f};
  const float hi[3] = {1.0f, 1.0f, 1.0f};
  lusdview::RtLodProto proto;
  proto.instanceXforms = xforms.data();
  proto.instanceCount = kInstances;
  proto.protoAabbMin = lo;
  proto.protoAabbMax = hi;
  lusdview::RtLodGrid grid;
  lusdview::BuildRtLodGrid(proto, 2, &grid);
  CHECK(grid.valid);
  CHECK(!grid.cells.empty());
  for (const lusdview::RtLodGridCell& cell : grid.cells)
    CHECK(std::fabs(cell.maxInstanceRadius - std::sqrt(3.0f)) < 1e-5f);
}

}  // namespace

int main() {
  TestStaticChunkProjection();
  TestSubpixelAggregateBound();
  TestGridTracksMaximumInstanceRadius();
  return failures == 0 ? 0 : 1;
}
