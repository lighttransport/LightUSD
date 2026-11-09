// SPDX-License-Identifier: Apache-2.0
#include "lightrt_c_tri.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

struct Coverage {
  int x;
  int y;
  float u;
  float v;
  float w;
};

std::vector<Coverage> RasterizeUnitTriangle(int resolution) {
  std::vector<Coverage> result;
  for (int y = 0; y < resolution; ++y) {
    for (int x = 0; x < resolution; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / resolution;
      const float v = (static_cast<float>(y) + 0.5f) / resolution;
      const float w = 1.0f - u - v;
      if (u < 0.0f || v < 0.0f || w < 0.0f) continue;
      result.push_back({x, y, u, v, w});
    }
  }
  return result;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1.0e-6f; }

}  // namespace

int main() {
  const std::vector<Coverage> coverage = RasterizeUnitTriangle(4);
  if (coverage.size() != 10 || coverage.front().x != 0 ||
      coverage.front().y != 0 || !Near(coverage.front().u, 0.125f) ||
      !Near(coverage.front().v, 0.125f) ||
      !Near(coverage.front().w, 0.75f)) {
    std::fprintf(stderr, "native UV coverage is not deterministic\n");
    return 1;
  }

  // Exercise the same center-sample UV coverage policy for every covered
  // texel, not only the first closest-hit probe below.
  std::vector<lrt_ray> coverage_rays;
  coverage_rays.reserve(coverage.size());
  for (const Coverage& sample : coverage) {
    lrt_ray coverage_ray{};
    coverage_ray.org[0] = (static_cast<float>(sample.x) + 0.5f) / 4.0f;
    coverage_ray.org[1] = (static_cast<float>(sample.y) + 0.5f) / 4.0f;
    coverage_ray.org[2] = 1.0f;
    coverage_ray.dir[2] = -1.0f;
    coverage_ray.tmin = 0.0f;
    coverage_ray.tmax = 10.0f;
    coverage_rays.push_back(coverage_ray);
  }

  const float vertices[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f};
  lrt_tri_build_options options{};
  options.quality = LRT_TRI_BUILD_FAST;
  options.layout = LRT_TRI_LAYOUT_BVH4;
  lrt_result error = LRT_RESULT_OK;
  lrt_tri_scene* scene = lrt_tri_scene_build(vertices, 1, &options, &error);
  if (!scene || error != LRT_RESULT_OK) {
    std::fprintf(stderr, "native LightRT build failed (%d)\n",
                 static_cast<int>(error));
    return 1;
  }

  lrt_ray ray{};
  ray.org[0] = 0.25f;
  ray.org[1] = 0.25f;
  ray.org[2] = 1.0f;
  ray.dir[2] = -1.0f;
  ray.tmin = 0.0f;
  ray.tmax = 10.0f;
  lrt_hit hit{};
  if (!lrt_tri_intersect1(scene, &ray, &hit) || hit.prim_id != 0 ||
      !Near(hit.t, 1.0f) || !Near(hit.u, 0.25f) || !Near(hit.v, 0.25f)) {
    std::fprintf(stderr, "native LightRT closest-hit ABI mismatch\n");
    lrt_tri_scene_free(scene);
    return 1;
  }

  lrt_ray rays[2] = {ray, ray};
  rays[1].org[2] = -1.0f;
  rays[1].dir[2] = -1.0f;
  lrt_hit hits[2]{};
  lrt_tri_intersect1N(scene, rays, hits, 2, LRT_TRI_BATCH_INCOHERENT);
  if (hits[0].prim_id != 0 || hits[1].prim_id != LRT_TRI_NO_HIT) {
    std::fprintf(stderr, "native LightRT batch closest-hit ABI mismatch\n");
    lrt_tri_scene_free(scene);
    return 1;
  }

  std::vector<lrt_hit> coverage_hits(coverage_rays.size());
  lrt_tri_intersect1N(scene, coverage_rays.data(), coverage_hits.data(),
                      coverage_rays.size(), LRT_TRI_BATCH_INCOHERENT);
  for (size_t index = 0; index < coverage.size(); ++index) {
    const Coverage& sample = coverage[index];
    const lrt_hit& hit = coverage_hits[index];
    if (hit.prim_id != 0 || !Near(hit.t, 1.0f) ||
        !Near(hit.u, sample.u) || !Near(hit.v, sample.v) ||
        !Near(1.0f - hit.u - hit.v, sample.w)) {
      std::fprintf(stderr, "native UV coverage barycentrics mismatch\n");
      lrt_tri_scene_free(scene);
      return 1;
    }
  }
  lrt_ray miss_ray = coverage_rays.front();
  miss_ray.org[0] = 0.875f;
  miss_ray.org[1] = 0.875f;
  lrt_hit miss_hit{};
  if (lrt_tri_intersect1(scene, &miss_ray, &miss_hit) ||
      miss_hit.prim_id != LRT_TRI_NO_HIT) {
    std::fprintf(stderr, "native UV coverage miss accounting mismatch\n");
    lrt_tri_scene_free(scene);
    return 1;
  }
  uint8_t occluded[2] = {0, 0};
  lrt_tri_occluded1N(scene, rays, occluded, 2, LRT_TRI_BATCH_INCOHERENT);
  if (occluded[0] != 1 || occluded[1] != 0) {
    std::fprintf(stderr, "native LightRT batch occlusion ABI mismatch\n");
    lrt_tri_scene_free(scene);
    return 1;
  }

  // Exercise the native UV bake's covered-texel visibility pass against a
  // separate blocker surface. Every covered target sample should see the
  // blocker at z=0.5 through the batched any-hit path.
  const float bake_vertices[] = {
      0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.5f};
  lrt_result bake_error = LRT_RESULT_OK;
  lrt_tri_scene* bake_scene =
      lrt_tri_scene_build(bake_vertices, 2, &options, &bake_error);
  if (!bake_scene || bake_error != LRT_RESULT_OK) {
    std::fprintf(stderr, "native LightRT bake scene build failed (%d)\n",
                 static_cast<int>(bake_error));
    lrt_tri_scene_free(scene);
    return 1;
  }
  std::vector<lrt_ray> bake_rays;
  bake_rays.reserve(coverage.size());
  for (const Coverage& sample : coverage) {
    lrt_ray bake_ray{};
    bake_ray.org[0] = (static_cast<float>(sample.x) + 0.5f) / 4.0f;
    bake_ray.org[1] = (static_cast<float>(sample.y) + 0.5f) / 4.0f;
    bake_ray.org[2] = 0.0001f;
    bake_ray.dir[2] = 1.0f;
    bake_ray.tmin = 0.0001f;
    bake_ray.tmax = 10.0f;
    bake_rays.push_back(bake_ray);
  }
  std::vector<uint8_t> baked_visibility(bake_rays.size());
  lrt_tri_occluded1N(bake_scene, bake_rays.data(), baked_visibility.data(),
                     bake_rays.size(), LRT_TRI_BATCH_INCOHERENT);
  for (uint8_t visible : baked_visibility) {
    if (visible != 1) {
      std::fprintf(stderr, "native UV bake visibility mismatch\n");
      lrt_tri_scene_free(bake_scene);
      lrt_tri_scene_free(scene);
      return 1;
    }
  }
  lrt_tri_scene_free(bake_scene);
  lrt_tri_scene_free(scene);
  return 0;
}
