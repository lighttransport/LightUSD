// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Deferred geometry worker scheduling and deterministic ordered merge.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common-macros.inc"
#include "tiny-format.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/task-arena.hh"

namespace lightusd {
namespace tydra {

namespace {

// Exact failure-text prefix used by the inline MeshVisitor path for each
// geometry kind ("Mesh conversion failed: ...", "cylinder conversion failed:
// ...", etc.), so deferred-mode errors are byte-identical.
std::string DeferredFailureKindName(const MeshWorkItem &item) {
  switch (item.kind) {
    case MeshWorkItem::Kind::Mesh:
      return "Mesh";
    case MeshWorkItem::Kind::Cube:
      return "Cube";
    case MeshWorkItem::Kind::Sphere:
      return "Sphere";
    case MeshWorkItem::Kind::Cylinder:
      return "cylinder";
    case MeshWorkItem::Kind::Cone:
      return "cone";
    case MeshWorkItem::Kind::Capsule:
      return "capsule";
    case MeshWorkItem::Kind::Plane:
      return "plane";
  }
  return "mesh";
}

std::string DeferredFailureMessage(const MeshWorkItem &item,
                                   const std::string &converter_err) {
  std::string msg = fmt::format("{} conversion failed: {}",
                                DeferredFailureKindName(item),
                                item.abs_path.full_path_name());
  msg += "\n" + converter_err + "\n";
  return msg;
}

// Resolve the effective worker count for parallel per-mesh geometry
// conversion from the config knob (0 = auto, 1 = serial, N = cap).
size_t ResolveGeometryWorkerCountImpl(int config_threads) {
#if !defined(LIGHTUSD_ENABLE_THREAD)
  // Threads are compiled out (default; required for WASM without pthreads).
  (void)config_threads;
  return 1;
#else
  if (config_threads == 1) {
    return 1;
  }

  size_t n = 0;
  if (config_threads > 1) {
    n = size_t(config_threads);
  } else {
    const unsigned hw = std::thread::hardware_concurrency();
    n = std::max<size_t>(1, std::min<size_t>(hw ? hw : 4,
                                              tydra::kMaxTaskArenaThreads));
  }

  // Parallel conversion is only worthwhile with more than one worker.
  return (n > 1) ? n : 1;
#endif
}

}  // namespace

size_t ResolveGeometryWorkerCount(int config_threads) {
  return ResolveGeometryWorkerCountImpl(config_threads);
}

//
// Convert deferred geometry work items.
//
// Phase 1 (parallel): items with convert_serially == false run on the
//   TaskArena. Each worker installs a thread-local DiagCapture so all
//   diagnostics land in per-slot buffers instead of shared strings; results
//   write into caller-owned slots (outputs/ok), so scheduling order cannot
//   affect scene content.
//
// Phase 2 (serial merge): items are visited in original traversal order.
//   Serial-tagged items convert here on the calling thread (keeping skeleton
//   registration deterministic); every item is appended to meshes/meshMap
//   exactly like the inline visitor path, followed by progress reporting and
//   streaming emission. Slot diagnostics funnel in order, so warning/error
//   text is also deterministic.
//
bool RenderSceneConverter::ConvertDeferredMeshes(
    const RenderSceneConverterEnv &env,
    const std::vector<MeshWorkItem> &items, size_t num_workers,
    MeshVisitorEnv *menv, std::string *err) {
  if (!menv || !menv->converter) {
    PUSH_ERROR_AND_RETURN("invalid MeshVisitorEnv for deferred conversion");
  }

  if (items.empty()) {
    return true;
  }

  std::vector<RenderMesh> outputs(items.size());
  std::vector<uint8_t> ok(items.size(), 0);
  std::vector<std::string> slot_warn(items.size());
  std::string slot_err;  // first worker failure aborts the merge anyway

  if (num_workers > 1 && items.size() > 1) {
    tydra::TaskArena arena(num_workers);

    // First failure wins: workers keep converting the current wave, but no
    // further items are claimed once failed_ is set.
    std::atomic<bool> failed{false};

    arena.Run(items.size(), [&](size_t i) {
      if (items[i].convert_serially) {
        return;  // handled by the serial merge phase
      }
      if (failed.load(std::memory_order_relaxed)) {
        return;
      }

      DiagCapture cap;
      cap.warn = &slot_warn[i];
      cap.err = &slot_err;
      DiagCapture *prev = SetDiagCapture(&cap);
      const bool converted =
          ConvertMeshWorkItem(env, items[i], &outputs[i]);
      SetDiagCapture(prev);

      if (converted) {
        ok[i] = 1;
      } else {
        failed.store(true, std::memory_order_relaxed);
      }
    });

    if (failed.load()) {
      if (err) {
        // Mirror the inline visitor's failure text for the first failed item.
        for (size_t i = 0; i < items.size(); i++) {
          if (!ok[i] && !items[i].convert_serially) {
            (*err) += DeferredFailureMessage(items[i], slot_err);
            break;
          }
        }
      }
      // Funnel diagnostics of successful items up to the failure so state
      // matches what a serial run would have produced.
      for (size_t i = 0; i < items.size() && !ok[i]; i++) {
        _warn += slot_warn[i];
        slot_warn[i].clear();
      }
      return false;
    }
  } else {
    // Fully serial fallback (single worker requested or a single item).
    for (size_t i = 0; i < items.size(); i++) {
      if (items[i].convert_serially) {
        continue;
      }
      DiagCapture cap;
      cap.warn = &slot_warn[i];
      cap.err = &slot_err;
      DiagCapture *prev = SetDiagCapture(&cap);
      const bool converted =
          ConvertMeshWorkItem(env, items[i], &outputs[i]);
      SetDiagCapture(prev);
      if (converted) {
        ok[i] = 1;
      } else {
        if (err) {
          (*err) += DeferredFailureMessage(items[i], slot_err);
        }
        for (size_t j = 0; j <= i; j++) {
          _warn += slot_warn[j];
        }
        return false;
      }
    }
  }

  //
  // Ordered merge: identical bookkeeping to the inline MeshVisitor path.
  //
  for (size_t i = 0; i < items.size(); i++) {
    const MeshWorkItem &item = items[i];

    // Serial-tagged items (skinned meshes) convert here, on this thread, at
    // their exact position in traversal order -- so skeleton registration
    // and mesh ids match the fully-serial path.
    if (!ok[i]) {
      DiagCapture cap;
      cap.err = &slot_err;
      cap.warn = &slot_warn[i];
      DiagCapture *prev = SetDiagCapture(&cap);
      const bool converted = ConvertMeshWorkItem(env, item, &outputs[i]);
      SetDiagCapture(prev);

      if (!converted) {
        if (err) {
          // Errors were captured into slot_err by this item's DiagCapture.
          (*err) += DeferredFailureMessage(item, slot_err);
        }
        for (size_t j = 0; j <= i; j++) {
          _warn += slot_warn[j];
        }
        return false;
      }
      ok[i] = 1;
    }

    // Funnel this item's warnings before appending it (order preserved).
    _warn += slot_warn[i];

    uint64_t mesh_id = uint64_t(meshes.size());
    if (mesh_id >= uint64_t((std::numeric_limits<int32_t>::max)())) {
      PUSH_ERROR_AND_RETURN("Mesh index too large.\n");
    }
    meshMap.add(item.abs_path.full_path_name(), mesh_id);
    meshes.emplace_back(std::move(outputs[i]));

    menv->meshes_processed++;
    std::string msg = std::string("Converting ") + item.type_name + " " +
                      std::to_string(menv->meshes_processed) + "/" +
                      std::to_string(menv->meshes_total);
    if (!ReportMeshProgress(menv->meshes_processed, menv->meshes_total,
                            item.abs_path.full_path_name(), msg)) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
    if (!EmitMesh(size_t(mesh_id), item.abs_path.full_path_name())) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
  }

  return true;
}


}  // namespace tydra
}  // namespace lightusd
