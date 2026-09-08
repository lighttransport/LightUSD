// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Internal header shared across render-data split files.
// NOT part of the public API.
//
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "tydra/render-data.hh"
#include "usdSkel.hh"
#include "value-types.hh"
#include "common-macros.inc"

namespace lightusd {

// Forward declarations
class Stage;
class Prim;
class Path;
struct Skeleton;
struct SkelRoot;
struct SkelAnimation;
struct AssetInfo;

namespace tydra {

// -----------------------------------------------------------------------
// SkelRootSkeletonResolver
// Resolves Skeleton prims from SkelRoot ancestor hierarchy.
// Used by both ConvertMesh (render-data-mesh.cc) and
// ConvertToRenderScene (render-data.cc).
// -----------------------------------------------------------------------
class SkelRootSkeletonResolver {
 public:
  using SkelRootToSkeletonMap =
      std::unordered_map<std::string, std::pair<Path, const Skeleton *>,
                         FNV1StringHash>;

  static void BuildMap(const PathPrimMap<Skeleton> &allSkeletons,
                       const PathPrimMap<SkelRoot> &allSkelRoots,
                       SkelRootToSkeletonMap *out_map) {
    if (!out_map) {
      return;
    }

    out_map->clear();
    out_map->reserve(allSkelRoots.size());

    for (const auto &kv : allSkeletons) {
      const std::string &skel_path_str = kv.first;
      const Skeleton *skel_ptr = kv.second;
      Path current_path(skel_path_str, "");

      size_t iter = 0;
      while (current_path.is_valid() && !current_path.is_root_path()
             && !current_path.is_root_prim()) {
        if (iter++ >= kMaxDefaultTraversalLimit) break;
        Path parent_path = current_path.get_parent_prim_path();
        const std::string parent_path_str = parent_path.prim_part();

        if (allSkelRoots.find(parent_path_str) != allSkelRoots.end()) {
          // Deterministic selection: if multiple Skeletons exist under one
          // SkelRoot, keep lexicographically smallest absolute skeleton path.
          auto it = out_map->find(parent_path_str);
          if (it == out_map->end()) {
            out_map->emplace(parent_path_str,
                             std::make_pair(Path(skel_path_str, ""), skel_ptr));
          } else if (skel_path_str < it->second.first.prim_part()) {
            it->second = std::make_pair(Path(skel_path_str, ""), skel_ptr);
          }
          break;
        }

        current_path = parent_path;
      }
    }
  }

  // Find skeleton for mesh by walking up ancestor hierarchy.
  // Returns true if skeleton found, with path and skeleton pointer stored.
  static bool FindByAncestor(const Path &meshPath,
                             const PathPrimMap<Skeleton> &allSkeletons,
                             const PathPrimMap<SkelRoot> &allSkelRoots,
                             const SkelRootToSkeletonMap *skelRootToSkeleton,
                             Path *outSkelPath, const Skeleton **outSkelPtr) {
    if (allSkeletons.empty()) {
      return false;
    }

    // Walk up ancestor chain
    size_t iter = 0;
    Path currentPath = meshPath;
    while (currentPath.is_valid() && !currentPath.is_root_path()
           && !currentPath.is_root_prim()) {
      if (iter++ >= kMaxDefaultTraversalLimit) break;
      Path parentPath = currentPath.get_parent_prim_path();
      std::string parentPathStr = parentPath.prim_part();

      DCOUT("FindSkeletonByAncestor: checking parent " << parentPathStr);

      // Check if parent is a SkelRoot
      auto skelRootIt = allSkelRoots.find(parentPathStr);
      if (skelRootIt != allSkelRoots.end()) {
        DCOUT("Found SkelRoot ancestor: " << parentPathStr);

        if (skelRootToSkeleton) {
          auto mapped = skelRootToSkeleton->find(parentPathStr);
          if (mapped != skelRootToSkeleton->end()) {
            *outSkelPath = mapped->second.first;
            if (outSkelPtr) *outSkelPtr = mapped->second.second;
            DCOUT("Found skeleton under SkelRoot from cache: "
                  << mapped->second.first.prim_part());
            return true;
          }
        }

        // Found SkelRoot ancestor - search its children for Skeleton
        std::string bestSkelPath;
        const Skeleton *bestSkelPtr{nullptr};
        for (const auto &kv : allSkeletons) {
          const std::string &skelPath = kv.first;
          const Skeleton *skelPtr = kv.second;

          if (IsStrictDescendantPath(skelPath, parentPathStr)) {
            if (!bestSkelPtr || skelPath < bestSkelPath) {
              bestSkelPath = skelPath;
              bestSkelPtr = skelPtr;
            }
          }
        }

        if (bestSkelPtr) {
          *outSkelPath = Path(bestSkelPath, "");
          if (outSkelPtr) *outSkelPtr = bestSkelPtr;
          DCOUT("Found skeleton under SkelRoot: " << bestSkelPath);
          return true;
        }
      }

      // Also check if parent itself is a Skeleton
      auto skelIt = allSkeletons.find(parentPathStr);
      if (skelIt != allSkeletons.end()) {
        *outSkelPath = Path(parentPathStr, "");
        if (outSkelPtr) *outSkelPtr = skelIt->second;
        DCOUT("Found skeleton as ancestor: " << parentPathStr);
        return true;
      }

      currentPath = parentPath;
    }

    // Fallback: if only one skeleton in scene, use it
    if (allSkeletons.size() == 1) {
      *outSkelPath = Path(allSkeletons.begin()->first, "");
      if (outSkelPtr) *outSkelPtr = allSkeletons.begin()->second;
      DCOUT("Fallback: using only skeleton in scene: "
            << allSkeletons.begin()->first);
      return true;
    }

    return false;
  }

 private:
  static bool IsStrictDescendantPath(const std::string &descendantPath,
                                     const std::string &ancestorPath) {
    if (ancestorPath.empty() || descendantPath.empty()) {
      return false;
    }

    // All absolute prim paths are expected to start with '/'.
    if (descendantPath[0] != '/' || ancestorPath[0] != '/') {
      return false;
    }

    if (descendantPath.size() <= ancestorPath.size()) {
      return false;
    }

    if (descendantPath.compare(0, ancestorPath.size(), ancestorPath) != 0) {
      return false;
    }

    // Require a path-segment boundary:
    // "/A/SkelRoot/Skel" is a descendant of "/A/SkelRoot", but
    // "/A/SkelRootExtra/Skel" is not.
    return descendantPath[ancestorPath.size()] == '/';
  }
};

// -----------------------------------------------------------------------
// MeshVisitorEnv / MeshVisitor
// Scene traversal visitor for mesh and material conversion.
// Defined in render-data-mesh-visitor.cc, used in render-data.cc.
// -----------------------------------------------------------------------

//
// One deferred geometry conversion. Filled by MeshVisitor when running in
// collect mode (MeshVisitorEnv::work_items != nullptr): material resolution
// still happens inline in traversal order (serial), but the expensive
// geometry conversion is recorded here and executed later -- in parallel on
// a worker pool for non-skinned prims, then merged back in original order.
//
struct MeshWorkItem {
  enum class Kind {
    Mesh,
    Cube,
    Sphere,
    Cylinder,
    Cone,
    Capsule,
    Plane,
  };

  Kind kind{Kind::Mesh};
  Path abs_path;
  const char *type_name{"mesh"};  // lower-case name used by progress messages

  // Stage-owned prim pointer per kind. Valid for the whole conversion; the
  // Stage is immutable during ConvertToRenderScene.
  const void *prim{nullptr};

  // Resolved material binding info (material ids already assigned).
  MaterialPath material_path;
  std::map<std::string, MaterialPath> subset_material_path_map;
  std::vector<const GeomSubset *> material_subsets;
  std::vector<std::pair<std::string, const BlendShape *>> blendshapes;

  // When true the item must be converted on the calling thread during the
  // ordered merge (skinned meshes: skeleton registration has to stay serial
  // and traversal-ordered), or when the whole conversion runs serially.
  bool convert_serially{false};
};

struct MeshVisitorEnv {
  RenderSceneConverter *converter{nullptr};
  const RenderSceneConverterEnv *env{nullptr};

  // Progress tracking for detailed progress reporting
  size_t meshes_processed{0};
  size_t meshes_total{0};
  size_t materials_processed{0};
  size_t materials_total{0};

  // Pre-discovered skeleton/animation prims for ancestor-based discovery
  const PathPrimMap<Skeleton> *allSkeletons{nullptr};
  const PathPrimMap<SkelRoot> *allSkelRoots{nullptr};
  const PathPrimMap<SkelAnimation> *allAnimations{nullptr};

  // When non-null, the visitor runs in COLLECT mode: geometry conversion is
  // deferred. Per-prim inputs are appended here in traversal order; the
  // caller (RenderSceneConverter::ConvertDeferredMeshes) converts and merges
  // them afterwards. Materials are still converted inline (serially) so the
  // materialMap is complete before any deferred geometry conversion starts.
  std::vector<MeshWorkItem> *work_items{nullptr};

  // When true, all collected items are tagged convert_serially regardless of
  // skinning (used when the caller decides to run the merge single-threaded).
  bool force_serial_conversion{false};

  // Conversion timing/counters. Nanoseconds are accumulated here to keep
  // timing code local to the visitor hot path and reported once by the caller.
  uint64_t resolve_material_ns{0};
  uint64_t convert_material_ns{0};
  uint64_t convert_mesh_ns{0};
  uint64_t progress_ns{0};
  size_t material_resolve_calls{0};
  size_t material_resolve_found{0};
  size_t material_cache_hits{0};
  size_t material_cache_misses{0};
};

bool MeshVisitor(const lightusd::Path &abs_path, const lightusd::Prim &prim,
                 const int32_t level, void *userdata, std::string *err);

// -----------------------------------------------------------------------
// ConnectionResolveCache reset
// Defined in render-data-material.cc, called from render-data.cc.
// -----------------------------------------------------------------------
void ResetConnectionResolveCache(const Stage &stage);

// Worker-count policy shared by orchestration and deferred conversion.
size_t ResolveGeometryWorkerCount(int config_threads);

// -----------------------------------------------------------------------
// RawAssetRead
// Shared utility for reading raw assets through the asset resolver.
// Used by render-data-material.cc (ConvertUVTexture) and
// render-data-anim.cc (ConvertDomeLight).
// -----------------------------------------------------------------------
bool RawAssetRead(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver,
    Asset *assetOut,
    std::string &resolvedPathOut,
    void *userdata, std::string *warn,
    std::string *err);

}  // namespace tydra
}  // namespace lightusd
