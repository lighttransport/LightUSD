// SPDX-License-Identifier: Apache-2.0
// Analytic nearest-hit and repeated-trace check for resident/streamed TLAS
// slices.
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "lightrt_c_vk.h"
int main() {
  lrt_vk_engine_options options = {};
  options.prefer_discrete = 1;
  options.want_ray_tracing = 1;
  lrt_result error = LRT_RESULT_OK;
  auto* engine = lrt_vk_engine_create(&options, &error);
  if (!engine) return 77;
  if (!(lrt_vk_engine_caps(engine) & LRT_VK_CAP_RAY_QUERY)) {
    lrt_vk_engine_destroy(engine);
    return 77;
  }
  const float vertices[] = {-2, -2, 0, 2, -2, 0, 0, 2, 0};
  const uint32_t indices[] = {0, 1, 2};
  lrt_vk_proto proto = {};
  proto.vertices = vertices;
  proto.nverts = 3;
  proto.indices = indices;
  proto.ntris = 1;
  lrt_vk_instance instances[2] = {};
  for (auto& inst : instances) {
    inst.transform[0] = inst.transform[5] = inst.transform[10] = 1;
    inst.proto = 0;
  }
  instances[1].transform[11] = 1;
  lrt_ray rays[2] = {};
  for (auto& ray : rays) {
    ray.org[2] = 5;
    ray.dir[2] = -1;
    ray.tmin = .001f;
    ray.tmax = 100;
  }
  rays[1].org[0] = 100;
  setenv("LRT_VK_TLAS_SLICE", "1", 1);
  for (const char* stream : {"0", "1"})
    for (const char* compact : {"0", "1"}) {
      setenv("LRT_VK_STREAM_TLAS", stream, 1);
      setenv("LRT_VK_BLAS_COMPACT", compact, 1);
      setenv("LRT_VK_TLAS_COMPACT", compact, 1);
      auto* scene = stream[0] == '1'
                        ? lrt_vk_rtx_scene_build_instanced_streamed(
                              engine, &proto, 1, instances, 2, &error)
                        : lrt_vk_rtx_scene_build_instanced_multi(
                              engine, &proto, 1, instances, 2, &error);
      if (!scene) {
        std::cerr << "build failure " << error << "\n";
        return 1;
      }
      for (int repeat = 0; repeat < 3; ++repeat) {
        lrt_hit_wide hits[2] = {};
        int count =
            lrt_vk_rtx_scene_trace_wide(engine, scene, rays, 2, hits, &error);
        if (count != 1 || hits[0].inst != 1 || hits[0].local != 0 ||
            std::abs(hits[0].t - 4) > 1e-5f || hits[1].inst != UINT32_MAX) {
          std::cerr << "nearest-hit/repeated-trace failure stream=" << stream
                    << " compact=" << compact << " repeat=" << repeat << "\n";
          return 1;
        }
      }
      lrt_vk_rtx_scene_free(engine, scene);
    }
  std::cout << "PASS: streamed/resident slices retain nearest global instance "
               "IDs across repeated traces\n";
  lrt_vk_engine_destroy(engine);
  return 0;
}
