// SPDX-License-Identifier: Apache-2.0
// Counted UV atlas API for the independent core geometry feature.
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "binding-geometry.h"
#include "external/xatlas/xatlas.h"
namespace {
using Options = lu_geometry_xatlas_options;
static_assert(sizeof(Options) == 32, "xatlas option ABI");
bool span(const void* ptr, size_t bytes) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
  const size_t heap = emscripten_get_heap_size();
  return ptr && start <= heap && bytes <= heap - start;
}
}  // namespace
extern "C" {
// Result: ten uint32 header words followed by positions, UVs, indices, source
// vertex IDs, chart IDs, atlas IDs. All arrays are 32-bit; callers own the blob
// and release it through lu_feature_free. No handles or borrowed atlas memory.
EMSCRIPTEN_KEEPALIVE uint8_t* lu_geometry_xatlas(
    const float* positions, uint32_t position_values, const uint32_t* indices,
    uint32_t index_count, const lu_geometry_xatlas_options* options) {
  constexpr uint32_t max_values = 16u * 1024u * 1024u;
  if (position_values < 9 || position_values % 3 || index_count < 3 ||
      index_count % 3 || position_values > max_values ||
      index_count > max_values ||
      !span(positions, size_t(position_values) * 4) ||
      !span(indices, size_t(index_count) * 4) ||
      !span(options, sizeof(Options)) ||
      reinterpret_cast<uintptr_t>(positions) % 4 ||
      reinterpret_cast<uintptr_t>(indices) % 4)
    return nullptr;
  Options opts;
  std::memcpy(&opts, options, sizeof(opts));
  if (!opts.iterations || opts.iterations > 100 || opts.resolution > 16384 ||
      opts.padding > 1024 || opts.max_chart_size > 16384 || opts.flags > 15 ||
      !std::isfinite(opts.max_cost) || opts.max_cost < 0 ||
      opts.max_cost > 1e6f || !std::isfinite(opts.seam_weight) ||
      opts.seam_weight < 0 || opts.seam_weight > 1e6f ||
      !std::isfinite(opts.texels_per_unit) || opts.texels_per_unit < 0 ||
      opts.texels_per_unit > 1e6f)
    return nullptr;
  for (uint32_t i = 0; i < position_values; ++i)
    if (!std::isfinite(positions[i])) return nullptr;
  for (uint32_t i = 0; i < index_count; ++i)
    if (indices[i] >= position_values / 3) return nullptr;
  std::unique_ptr<xatlas::Atlas, decltype(&xatlas::Destroy)> atlas(
      xatlas::Create(), xatlas::Destroy);
  if (!atlas) return nullptr;
  // Charting is invariant under a global translation/scale. Normalize in
  // double precision so finite USD coordinates cannot overflow xatlas's float
  // cross products. The output still copies the original authored positions.
  double low[3] = {positions[0], positions[1], positions[2]};
  double high[3] = {positions[0], positions[1], positions[2]};
  for (uint32_t i = 0; i < position_values; ++i) {
    low[i % 3] = std::min(low[i % 3], double(positions[i]));
    high[i % 3] = std::max(high[i % 3], double(positions[i]));
  }
  const double scale =
      std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
  if (!(scale > 0)) return nullptr;
  const double texels = double(opts.texels_per_unit) * scale;
  if (!std::isfinite(texels) || texels > 1e6) return nullptr;
  std::vector<float> normalized(position_values);
  for (uint32_t i = 0; i < position_values; ++i)
    normalized[i] = float((double(positions[i]) - low[i % 3]) / scale);
  xatlas::MeshDecl mesh;
  mesh.vertexPositionData = normalized.data();
  mesh.vertexPositionStride = 12;
  mesh.vertexCount = position_values / 3;
  mesh.indexData = indices;
  mesh.indexCount = index_count;
  mesh.faceCount = index_count / 3;
  mesh.indexFormat = xatlas::IndexFormat::UInt32;
  if (xatlas::AddMesh(atlas.get(), mesh) != xatlas::AddMeshError::Success)
    return nullptr;
  xatlas::ChartOptions chart;
  chart.fixWinding = true;
  chart.maxIterations = opts.iterations;
  chart.maxCost = opts.max_cost;
  chart.textureSeamWeight = opts.seam_weight;
  xatlas::PackOptions pack;
  pack.resolution = opts.resolution;
  pack.padding = opts.padding;
  pack.maxChartSize = opts.max_chart_size;
  pack.texelsPerUnit = float(texels);
  pack.rotateChartsToAxis = (opts.flags & 1) != 0;
  pack.rotateCharts = (opts.flags & 2) != 0;
  pack.blockAlign = (opts.flags & 4) != 0;
  pack.bruteForce = (opts.flags & 8) != 0;
  xatlas::Generate(atlas.get(), chart, pack);
  if (!atlas->meshCount || !atlas->width || !atlas->height || !atlas->meshes ||
      !atlas->meshes[0].vertexArray || !atlas->meshes[0].indexArray)
    return nullptr;
  const auto& result = atlas->meshes[0];
  const uint64_t words =
      10 + uint64_t(result.vertexCount) * 8 + result.indexCount;
  if (words > (uint64_t(512) << 20) / 4) return nullptr;
  for (uint32_t i = 0; i < result.vertexCount; ++i)
    if (result.vertexArray[i].xref >= mesh.vertexCount) return nullptr;
  for (uint32_t i = 0; i < result.indexCount; ++i)
    if (result.indexArray[i] >= result.vertexCount) return nullptr;
  auto* blob = static_cast<uint32_t*>(std::malloc(size_t(words) * 4));
  if (!blob) return nullptr;
  const uint32_t header[10] = {uint32_t(words * 4),
                               atlas->width,
                               atlas->height,
                               atlas->atlasCount,
                               atlas->chartCount,
                               result.vertexCount,
                               result.indexCount,
                               mesh.vertexCount,
                               0,
                               0};
  std::memcpy(blob, header, sizeof(header));
  float* pos = reinterpret_cast<float*>(blob + 10);
  float* uv = pos + size_t(result.vertexCount) * 3;
  auto* idx = reinterpret_cast<uint32_t*>(uv + size_t(result.vertexCount) * 2);
  uint32_t* xref = idx + result.indexCount;
  auto* chart_ids = reinterpret_cast<int32_t*>(xref + result.vertexCount);
  int32_t* atlas_ids = chart_ids + result.vertexCount;
  std::memcpy(idx, result.indexArray, size_t(result.indexCount) * 4);
  for (uint32_t i = 0; i < result.vertexCount; ++i) {
    const auto& vertex = result.vertexArray[i];
    std::memcpy(pos + size_t(i) * 3, positions + size_t(vertex.xref) * 3, 12);
    uv[size_t(i) * 2] = vertex.uv[0] / float(atlas->width);
    uv[size_t(i) * 2 + 1] = vertex.uv[1] / float(atlas->height);
    xref[i] = vertex.xref;
    chart_ids[i] = vertex.chartIndex;
    atlas_ids[i] = vertex.atlasIndex;
  }
  return reinterpret_cast<uint8_t*>(blob);
}
}
