// SPDX-License-Identifier: Apache 2.
// Copyright 2022 - Present, Light Transport Entertainment, Inc.
//
// Stage: Similar to Scene or Scene graph
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nonstd/expected.hpp"

#include "composition.hh"
#include "core/instance-key.hh"  // InstanceKey, InstanceKeyHasher
#include "core/prim.hh"          // Prim class (transitively: path, prim-enums, prim-metas)
#include "crate-format.hh"       // PathHasher / PathKeyEqual for _prim_path_cache
#include "core/layer-types.hh"   // LayerMetas (aliased as StageMetas)
#include "handle-allocator.hh"   // HandleAllocator

namespace lightusd {

// Forward declarations for mmap zero-copy support
class MMapArrayTable;
class MMapDataSource;
struct StageMMapFileOwner;
namespace io {
struct MMapFileHandle;
}

using StageMetas = LayerMetas;

class PrimRange;

// Similar to UsdStage, but much more something like a Scene(scene graph)
class Stage {
 public:
  // pxrUSD compat API ----------------------------------------
  static Stage CreateInMemory() { return Stage(); }

  // Special member functions
  Stage();
  ~Stage();
  Stage(const Stage&);
  Stage& operator=(const Stage&);
  Stage(Stage&&) noexcept;
  Stage& operator=(Stage&&) noexcept;

  ///
  /// Traverse by depth-first order.
  /// NOTE: Not yet implementd. Use tydra::VisitPrims() for a while.
  ///
  // PrimRange Traverse();

  ///
  /// Get Prim at a Path.
  /// Path must be absolute Path.
  ///
  /// @returns Const pointer to Prim(to avoid a copy). Never returns nullptr
  /// upon success.
  ///
  nonstd::expected<const Prim *, std::string> GetPrimAtPath(
      const Path &path) const;

  ///
  /// pxrUSD Compat API
  ///
  bool Flatten(bool addSourceFileComment = true) const {
    return compose(addSourceFileComment);
  }

  ///
  /// Dump Stage as ASCII(USDA) representation.
  /// @param[in] relative_path (optional) Print Path as relative Path.
  /// @param[in] parallel (optional) Use parallel printing for Prims.
  ///
  std::string ExportToString(bool relative_path = false, bool parallel = false) const;

  // pxrUSD compat API end -------------------------------------

  ///
  /// Get Prim from a children of given root Prim.
  /// Path must be relative Path.
  ///
  /// @returns pointer to Prim(to avoid a copy). Never return nullptr upon
  /// success.
  ///
  nonstd::expected<const Prim *, std::string> GetPrimFromRelativePath(
      const Prim &root, const Path &path) const;

  /// Find(Get) Prim at a Path.
  /// Path must be absolute Path.
  ///
  /// @param[in] path Absolute path(e.g. `/bora/dora`)
  /// @param[out] prim const reference to Prim(if found)
  /// @param[out] err Error message(filled when false is returned)
  ///
  /// @returns true if found a Prim.
  bool find_prim_at_path(const Path &path, const Prim *&prim,
                         std::string *err = nullptr) const;

  /// Find(Get) Prim at a Path and returns its Prim id.
  /// Path must be absolute Path.
  ///
  /// @param[in] path Absolute path(e.g. `/bora/dora`)
  /// @param[out] prim_id Prim's id(should be '1 or greater' upon success)
  /// @param[out] err Error message(filled when false is returned)
  ///
  /// @returns true if found a Prim.
  bool find_prim_at_path(const Path &path, int64_t *prim_id,
                         std::string *err = nullptr) const;

  /// Find(Get) Prim from a relative Path.
  /// Path must be relative Path.
  ///
  /// @param[in] root Find from this Prim
  /// @param[in] relative_path relative path(e.g. `dora/muda`)
  /// @param[out] prim const reference to the pointer to Prim(if found)
  /// @param[out] err Error message(filled when false is returned)
  ///
  /// @returns true if found a Prim.
  bool find_prim_from_relative_path(const Prim &root, const Path &relative_path,
                                    const Prim *&prim, std::string *err) const;

  ///
  /// Find(Get) Prim from Prim ID. Prim with no Prim ID assigned(-1 or 0) are
  /// ignored.
  ///
  /// @param[in] prim_id Prim ID(1 or greater)
  /// @param[out] prim const reference to the pointer to Prim(if found)
  /// @param[out] err Error message(filled when false is returned)
  ///
  /// @returns true if found a Prim.
  bool find_prim_by_prim_id(const uint64_t prim_id, const Prim *&prim,
                            std::string *err = nullptr) const;

  // non-const version
  bool find_prim_by_prim_id(const uint64_t prim_id, Prim *&prim,
                            std::string *err = nullptr);

  ///
  /// @brief Get Root Prims
  ///
  /// @return Const array of Root Prims.
  ///
  const std::vector<Prim> &root_prims() const LIGHTUSD_LIFETIMEBOUND {
    return _root_nodes;
  }

  ///
  /// @brief Reference to Root Prims array
  ///
  /// @return Array of Root Prims.
  /// TODO: Deprecate non-const `root_prims()` API and use `add_root_prim()` instead.
  ///
  std::vector<Prim> &root_prims() LIGHTUSD_LIFETIMEBOUND {
    return _root_nodes;
  }

  ///
  /// Add Prim to root.
  ///
  /// @param[in] prim Prim
  /// @param[in] rename_prim_name Rename Prim's elementName if required(to be unique among root Prims)
  ///
  /// @return true Upon success. false when failed to add Prim to root(e.g. same name exists in the root)
  /// (error message can be retrieved using `get_error()`)
  ///
  bool add_root_prim(Prim &&prim, bool rename_prim_name = true);

  ///
  /// Replace root Prim of elementName `prim_name` with `prim`
  ///
  /// `prim`'s elementName will be modified to `prim_name`.
  ///
  /// If no root prim with `prim_name` exists, `prim` is added to root Prim and rename `prim`'s elementName to `prim_name`.
  ///
  /// @return true Upon succes. false when failed to replace Prim at root(e.g. `prim_name` is empty).
  ///
  bool replace_root_prim(const std::string &prim_name, Prim &&prim);

  ///
  /// Namespace editing (mechanical, layer-level -- mirrors OpenUSD's
  /// SdfBatchNamespaceEdit semantics). These mutate the prim tree in place:
  /// the element name and prim-tree position are authoritative (absolute paths
  /// are derived positionally), so descendants move with their parent. The
  /// `primChildren` ordering metadata (when authored) and the lookup cache are
  /// updated. NOTE: relationship-target / attribute-connection paths that
  /// referenced the affected prim(s) are NOT rewritten (that stage-level fixup,
  /// like UsdNamespaceEditor, is a follow-on); such targets may become dangling.
  ///

  /// Rename the prim at `path` to `new_name`. Fails if `path` is the root, does
  /// not exist, or a sibling already uses `new_name`.
  bool RenamePrim(const Path &path, const std::string &new_name,
                  std::string *err = nullptr);

  /// Remove the prim at `path` (and its subtree). Fails if `path` is the root
  /// or does not exist.
  bool RemovePrim(const Path &path, std::string *err = nullptr);

  /// Move the prim at `path` to be a child of `new_parent_path` (which must
  /// exist and must not be the prim itself or one of its descendants). Fails on
  /// a name clash in the destination. Use `new_name` (non-empty) to rename
  /// during the move.
  bool ReparentPrim(const Path &path, const Path &new_parent_path,
                    const std::string &new_name = "", std::string *err = nullptr);

  ///
  /// @brief Get Stage metadatum
  ///
  /// @return Stage metadatum struct.
  ///
  const StageMetas &metas() const LIGHTUSD_LIFETIMEBOUND {
    return stage_metas;
  }

  StageMetas &metas() LIGHTUSD_LIFETIMEBOUND { return stage_metas; }

  ///
  /// @brief Assign unique Prim id inside this Stage.
  ///
  /// @param[out] prim_id Allocated Primitive ID.
  ///
  /// @return true upon success.
  ///
  bool allocate_prim_id(uint64_t *prim_id) const;

  ///
  /// @brief Release Prim id inside this Stage.
  ///
  /// @param[prim_id] prim_id Primitive ID to release(allocated by
  /// `allocate_prim_id`)
  ///
  /// @return true upon success. false when given `prim_id` is an invalid id.
  ///
  bool release_prim_id(const uint64_t prim_id) const;

  ///
  /// @brief Check if given prim_id exists in this Stage.
  ///
  /// @param[prim_id] prim_id Primitive ID to check.
  ///
  /// @return true if `prim_id` exists in this Stage.
  ///
  bool has_prim_id(const uint64_t prim_id) const;

  ///
  /// @brief Commit Stage state.
  ///
  /// Call this function after you finished adding Prims manually(through
  /// `root_prims()`) to Stage.
  ///
  /// (No need to call this if you just use ether USDA/USDC/USDZ reader).
  ///
  /// - Compute absolute path and set it to Prim::abs_path for each Prim
  /// currently added to this Stage.
  /// - Assign unique ID to Prim
  ///
  /// @param[in] force_assign_prim_id true Overwrite `prim_id` of each Prim.
  /// false only assign Prim id when `prim_id` is -1(preserve user-assgiend
  /// prim_id). Setting `false` is not recommended since prim_id may not be
  /// unique over Prims in Stage.
  /// @return false when the Stage contains any invalid Prim
  ///
  /// TODO: Deprecate this API an use `commit()`
  bool compute_absolute_prim_path_and_assign_prim_id(
      bool force_assign_prim_id = true);

  ///
  /// @brief Commit Stage state.
  ///
  bool commit() {
    // Currently we always allocate Prim ID.
    return compute_absolute_prim_path_and_assign_prim_id(true);
  }

  ///
  /// Compute absolute Prim path for Prims in this Stage.
  ///
  bool compute_absolute_prim_path();

  ///
  /// Dump Prim tree info(mainly for debugging).
  ///
  std::string dump_prim_tree() const;

  ///
  /// Compose scene(Not implemented yet).
  ///
  bool compose(bool addSourceFileComment = true) const;

  const std::string &get_warning() const LIGHTUSD_LIFETIMEBOUND {
    return _warn;
  }

  const std::string &get_error() const LIGHTUSD_LIFETIMEBOUND {
    return _err;
  }

  ///
  /// Estimate memory usage of this Stage in bytes
  ///
  /// @return Estimated memory usage in bytes
  ///
  size_t estimate_memory_usage() const;

  /// Detailed memory usage report: allocated (capacity) vs actual (size).
  struct MemoryUsageDetail {
    size_t allocated_bytes{0};  // capacity-based (what the allocator gave us)
    size_t actual_bytes{0};     // size-based (what we're actually using)
  };
  MemoryUsageDetail estimate_memory_usage_detail() const;

  //
  // AOUSD Core Spec 11.3.3: Scene Graph Instancing
  //

  ///
  /// Detect instanceable prims and build a prototype registry.
  ///
  /// Scans all prims for `instanceable = true` metadata AND at least one
  /// composition arc (references, payload, inherits, specializes, variantSets).
  /// Groups instances by their InstanceKey (128-bit hash of composition arcs).
  /// Prims sharing the same InstanceKey share a prototype.
  ///
  /// @return Number of unique prototypes found
  ///
  size_t BuildInstancePrototypes();

  ///
  /// Get the prototype index for a given prim path.
  /// @return prototype index (>= 0) if the prim is an instance, -1 otherwise
  ///
  int GetPrototypeIndex(const Path &path) const {
    auto it = _instance_to_prototype.find(path.prim_part());
    if (it != _instance_to_prototype.end()) {
      return it->second;
    }
    return -1;
  }

  ///
  /// Check if a prim path is registered as an instance.
  ///
  bool IsInstancePrim(const Path &path) const {
    return _instance_to_prototype.count(path.prim_part()) > 0;
  }

  ///
  /// Get the prototype index for a given instance prim path.
  /// Alias for GetPrototypeIndex().
  /// @return prototype index (>= 0) if the prim is an instance, -1 otherwise
  ///
  int GetPrototypeIndexForInstance(const Path &path) const {
    return GetPrototypeIndex(path);
  }

  ///
  /// Get all instance paths that share a given prototype index.
  ///
  std::vector<Path> GetInstancesForPrototype(int prototype_index) const {
    std::vector<Path> result;
    for (const auto &entry : _instance_to_prototype) {
      if (entry.second == prototype_index) {
        result.push_back(Path(entry.first, ""));
      }
    }
    return result;
  }

  ///
  /// Get the source path (first instance) for a prototype.
  /// @return source prim path, or empty string if index is invalid
  ///
  std::string GetPrototypeSourcePath(int prototype_index) const {
    if (prototype_index >= 0 &&
        static_cast<size_t>(prototype_index) < _prototype_source_paths.size()) {
      return _prototype_source_paths[static_cast<size_t>(prototype_index)];
    }
    return {};
  }

  ///
  /// Get the number of unique prototypes.
  ///
  size_t num_prototypes() const { return _prototype_count; }

  ///
  /// Import instance data from CompositionGraph.
  /// Called by CompositionGraph::BuildStage() to transfer pre-computed
  /// instance information.
  ///
  void ImportInstanceData(
      const std::unordered_map<std::string, int> &instance_to_prototype,
      size_t prototype_count);

 private:

  // Instance prototype registry (AOUSD Spec 11.3.3)
  // Maps instanceable prim path -> prototype index
  std::unordered_map<std::string, int> _instance_to_prototype;
  // Maps InstanceKey -> prototype index (for dedup)
  std::unordered_map<InstanceKey, int, InstanceKeyHasher>
      _instance_key_to_prototype;
  // First instance path per prototype (used as source/reference)
  std::vector<std::string> _prototype_source_paths;
  size_t _prototype_count{0};
  // True if instance data was imported from CompositionGraph
  bool _instance_data_imported{false};

  // Root nodes
  std::vector<Prim> _root_nodes;
  std::multiset<std::string> _root_node_nameSet;

  std::string name;       // Scene name
  int64_t default_root_node{-1};  // index to default root node

  StageMetas stage_metas;

  mutable std::string _err;
  mutable std::string _warn;

  // Cached prim path.
  // key : Path (the prim part is used to match; the Path is owned by the
  // cache, avoiding the per-lookup std::string allocation that would result
  // from passing tstring_view as a key to a HashMap<std::string, ...>).
  // See concern #1 in review.md.
  mutable lightusd::HashMap<Path, const Prim *, lightusd::crate::PathHasher,
                            lightusd::crate::PathKeyEqual>
      _prim_path_cache;

  // Cached prim_id -> Prim lookup
  // key : prim_id
  mutable lightusd::HashMap<uint64_t, const Prim *> _prim_id_cache;

  mutable bool _dirty{true}; // True when Stage content changes(addition, deletion, composition/flatten, etc.)

  mutable bool _prim_id_dirty{true}; // True when Prim Id assignent changed(TODO: Unify with `_dirty` flag)

  mutable HandleAllocator<uint64_t> _prim_id_allocator;

  // Guards the lazy lookup caches (_prim_path_cache / _prim_id_cache and their
  // _dirty / _prim_id_dirty flags) so the `const` GetPrimAtPath() /
  // find_prim_by_prim_id() are safe under concurrent readers (e.g. a shared
  // Stage queried from multiple threads). shared_ptr keeps Stage
  // copyable/movable; the _root_nodes Prim tree is read lock-free during the
  // walk. Reset (not shared) in Stage's copy paths.
  //
  // NOTE: this member is *unconditional* (not gated on LIGHTUSD_ENABLE_THREAD)
  // on purpose. LIGHTUSD_ENABLE_THREAD is PRIVATE to the library target, so
  // gating a member of this *public* header would make sizeof(Stage) differ
  // between the library and its consumers (ODR violation / stack corruption).
  // Only the locking in stage.cc is gated; the lone mutex alloc per Stage is
  // negligible since Stages are not created in bulk.
  std::shared_ptr<std::mutex> _cache_mu{std::make_shared<std::mutex>()};

  // mmap zero-copy support (optional, set by USDC reader)
  std::unique_ptr<MMapArrayTable> _mmap_table;
  std::unique_ptr<MMapDataSource> _mmap_source;
  std::unique_ptr<StageMMapFileOwner> _mmap_file_owner;
  std::unique_ptr<std::vector<uint8_t>> _mmap_buffer_owner;

 public:
  void set_mmap_table(MMapArrayTable &&table);
  void set_mmap_source(const MMapDataSource &src);
  bool adopt_mmap_file(io::MMapFileHandle &&handle);
  bool adopt_mmap_buffer(std::vector<uint8_t> &&buffer);
  void clear_mmap_data();
  const MMapArrayTable *mmap_table() const LIGHTUSD_LIFETIMEBOUND {
    return _mmap_table.get();
  }
  const MMapDataSource *mmap_source() const LIGHTUSD_LIFETIMEBOUND {
    return _mmap_source.get();
  }
  bool has_mmap_zero_copy() const;
};

inline std::string to_string(const Stage &stage, bool relative_path = false) {
  return stage.ExportToString(relative_path);
}

}  // namespace lightusd
