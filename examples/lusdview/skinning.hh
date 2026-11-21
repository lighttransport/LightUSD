// SPDX-License-Identifier: Apache-2.0
// lusdview - CPU linear-blend skinning + blendshape deformation.
//
// Tydra bakes a rest-pose mesh plus separate skeleton/animation data; it does
// not deform geometry for skinning, and it does not emit animated blendshape
// weights. This module fills both gaps on the CPU: given a RenderScene already
// converted at time `timecode` (so node transforms/value-clips are resolved)
// and the source Stage, it deforms each skinned / blendshaped RenderMesh's
// points in place so the existing pack + upload path renders the posed mesh.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gpu_scene.hh"
#include "tydra/render-data.hh"

namespace lusdview {









struct RtSkinnedMeshUpload {
  int meshIndex{-1};
  std::vector<DrawVertex> vertices;
};

// Deterministic parallel-for over [0, n): contiguous ranges, one per worker,
// no shared accumulation -- each index runs exactly the serial loop body, so
// the result is BIT-IDENTICAL to serial regardless of thread count. That is
// the contract that lets the per-frame RT deform be threaded without touching
// any of the byte-parity oracles (refit-vs-rebuild, gpu-vs-cpu-bake).
//
// grainMin is the MINIMUM indices per worker, set from the measured cost of
// the loop body. Measured 2026-07-16 (Ryzen/Linux, medians over 120 poses --
// single-shot timings of this path swing 2-4x, do not trust them): the
// 102k-vert LBS deform runs 3.2 ms serial vs 1.67 ms with 7 workers at grain
// 16384, byte-identical; at 1M verts threading is ~3x. Thread wake latency is
// real (~0.3 ms plus outliers) but amortizes once a worker carries a few tens
// of thousands of deform-weight indices; use larger grains for cheaper loop
// bodies (see RecomputeSmoothNormals).
template <typename F>
inline void DeformParallelFor(size_t n, size_t grainMin, F&& fn) {
  const unsigned hw = std::thread::hardware_concurrency();
  const size_t maxWorkers = grainMin ? (n + grainMin - 1) / grainMin : 1;
  const size_t workers = std::min<size_t>(hw ? hw : 1, maxWorkers);
  if (workers <= 1 || n == 0) {
    if (n) fn(size_t(0), n);
    return;
  }
  std::vector<std::thread> ts;
  ts.reserve(workers);
  const size_t chunk = (n + workers - 1) / workers;
  for (size_t w = 0; w < workers; ++w) {
    const size_t b = w * chunk;
    const size_t e = std::min(n, b + chunk);
    if (b >= e) break;
    ts.emplace_back([&fn, b, e]() { fn(b, e); });
  }
  for (std::thread& t : ts) t.join();
}


// One GPU-compute-skinnable mesh's per-frame inputs: the composed skinning
// matrices (geomBind * skinMat * skeletonWorld * inverse(meshWorld), 16 floats
// each, row-major, row-vector p*M — exactly what ApplySkinningToVertices uses)
// plus a conservative posed object-space bound (union of the per-joint
// transformed rest prototype box; the skinned mesh is a convex combination of
// per-joint transforms, so it is contained in that union).
struct RtGpuSkinUpdate {
  int meshIndex{-1};
  int matrixBase{0};  // absolute joint id - matrixBase indexes mats
  int jointCount{0};
  std::vector<float> mats;  // 16 floats per joint
  float aabbMin[3]{0, 0, 0};
  float aabbMax[3]{0, 0, 0};
};





}  // namespace lusdview
