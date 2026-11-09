// SPDX-License-Identifier: Apache-2.0
// Stage instance expansion split from render-data.cc.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common-macros.inc"
#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "usdGeom.hh"

namespace lightusd {
namespace tydra {
namespace {
using Clock = std::chrono::steady_clock;
double ElapsedMs(const Clock::time_point &start) {
  return double(std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - start).count()) * 0.001;
}
}  // namespace

double RenderSceneConverter::BuildStageInstances(
    const RenderSceneConverterEnv &env,
    const PathPrimMap<GeomPointInstancer> &point_instancers) {
  const auto phase_start = Clock::now();
    // Flatten the built Node hierarchy into an abs_path -> world matrix map.
    // Used to look up per-prim world transforms for instance expansion
    // (scenegraph instances in 7b, PointInstancer instances in 7c).
    std::unordered_map<std::string, value::matrix4d> path_to_global;
    {
      std::function<void(const Node &)> collect = [&](const Node &n) {
        if (!n.abs_path.empty()) {
          path_to_global[n.abs_path] = n.global_matrix;
        }
        for (const auto &c : n.children) {
          collect(c);
        }
      };
      for (const auto &n : root_nodes) {
        collect(n);
      }
    }

    //
    // 7b. Build instance registry from Stage (AOUSD Spec 11.3.3)
    //
    {
      // BuildInstancePrototypes must be called on the Stage first.
      // It's safe to call multiple times (idempotent after first call).
      Stage &mutable_stage = const_cast<Stage &>(env.stage);
      size_t num_protos = mutable_stage.BuildInstancePrototypes();
      if (num_protos > 0) {
        DCOUT("[Tydra] Found " << num_protos << " instance prototypes");

        // Build prototype_index -> mesh_id mapping from meshMap
        // Instance prims are typically Xform parents of mesh children,
        // so we look up child mesh paths for each prototype source.
        std::unordered_map<int, int32_t> proto_to_mesh;

        // Sorted snapshot of meshMap for O(log M) descendant lookups.
        // (The previous implementation scanned the whole map per instance --
        // O(instances x meshes); StringAndIdMap iteration order is unspecified
        // anyway, so picking the lexicographically smallest descendant path
        // here makes the choice deterministic.)
        std::vector<std::pair<std::string, uint32_t>> sorted_mesh_paths;
        sorted_mesh_paths.reserve(meshMap.size());
        for (auto it = meshMap.s_begin(); it != meshMap.s_end(); ++it) {
          sorted_mesh_paths.emplace_back(it->first, uint32_t(it->second));
        }
        std::sort(sorted_mesh_paths.begin(), sorted_mesh_paths.end());

        // For each instance prim, create a RenderInstance
        for (size_t proto_idx = 0; proto_idx < num_protos; proto_idx++) {
          auto inst_paths = mutable_stage.GetInstancesForPrototype(
              static_cast<int>(proto_idx));
          for (const auto &inst_path : inst_paths) {
            const std::string &path_str = inst_path.prim_part();

            // Find mesh_id for this instance's children (if any)
            int32_t found_mesh_id = -1;
            if (!sorted_mesh_paths.empty()) {
              // Any descendant path sorts within [path + "/", ...) and before
              // the next non-descendant string greater than that prefix.
              const std::string prefix = path_str + "/";
              auto lb = std::lower_bound(
                  sorted_mesh_paths.begin(), sorted_mesh_paths.end(), prefix,
                  [](const std::pair<std::string, uint32_t> &a,
                     const std::string &b) { return a.first < b; });
              if (lb != sorted_mesh_paths.end() &&
                  lb->first.size() > path_str.size() &&
                  lb->first.compare(0, path_str.size(), path_str) == 0 &&
                  lb->first[path_str.size()] == '/') {
                found_mesh_id = static_cast<int32_t>(lb->second);
              }
            }

            RenderInstance rinst;
            rinst.abs_path = path_str;
            rinst.prototype_index = static_cast<int32_t>(proto_idx);
            rinst.mesh_id = found_mesh_id;

            // Populate the instance transform from the instance prim's node.
            auto mit = path_to_global.find(path_str);
            if (mit != path_to_global.end()) {
              rinst.global_matrix = mit->second;
              rinst.local_matrix = mit->second;
            }

            // Extract prim name from path
            size_t last_slash = path_str.rfind('/');
            if (last_slash != std::string::npos) {
              rinst.prim_name = path_str.substr(last_slash + 1);
            }

            instances.emplace_back(std::move(rinst));
          }
        }
        DCOUT("[Tydra] Created " << instances.size() << " render instances");
      }
    }

    //
    // 7c. Expand PointInstancer prims into RenderScene::instances.
    //
    if (env.scene_config.expand_point_instancers &&
        !point_instancers.empty()) {
      for (const auto &kv : point_instancers) {
        const std::string &pi_path = kv.first;
        const GeomPointInstancer *pi = kv.second;
        if (!pi) continue;

        value::matrix4d instancer_world = value::matrix4d::identity();
        auto mit = path_to_global.find(pi_path);
        if (mit != path_to_global.end()) {
          instancer_world = mit->second;
        }

        if (!ExpandPointInstancer(env, pi_path, *pi, instancer_world,
                                  path_to_global)) {
          PushWarn("PointInstancer expansion failed for " + pi_path +
                   "; continuing.\n");
        }
      }
      DCOUT("[Tydra] Total render instances after PointInstancer expansion: "
            << instances.size());
    }

  return ElapsedMs(phase_start);
}

}  // namespace tydra
}  // namespace lightusd
