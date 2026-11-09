// SPDX-License-Identifier: Apache 2.0
// Copyright 2019 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertinament Inc.

#include <algorithm>
#include <atomic>
// #include <cassert>
#include <cctype>  // std::tolower
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include "asset-resolution.hh"

//
#ifndef __wasi__
#include <thread>
#endif
//
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "io-util.hh"
#include "mmap-array-ref.hh"
#include "path-util.hh"
#include "pprinter.hh"
#include "prim-pprint.hh"
#include "prim-pprint-parallel.hh"
#include "str-util.hh"
#include "tiny-container.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdLux.hh"
#include "usdShade.hh"
#include "usda-reader.hh"
#include "value-pprint.hh"
//
#include "common-macros.inc"

namespace lightusd {

#if 1
// For PUSH_ERROR_AND_RETURN
#define PushError(s) _err += s;
// #define PushWarn(s) if (warn) { (*warn) += s; }
#endif

namespace {

// Optimized version: iterative traversal using path components
// Avoids string concatenation and recursion by directly navigating to target
nonstd::optional<const Prim *> GetPrimAtPathIterative(
    const std::vector<Prim> &root_nodes,
    const Path &path) {

  const std::string &target_path = path.full_path_name();

  // Must be absolute path starting with '/'
  if (target_path.empty() || target_path[0] != '/') {
    return nonstd::nullopt;
  }

  // For paths like "/" only (root path), we don't have a prim
  if (target_path.size() == 1) {
    return nonstd::nullopt;
  }

  // Iteratively parse and traverse path components
  // No string allocations - uses compare() with indices
  const Prim *current = nullptr;
  const std::vector<Prim> *current_children = &root_nodes;

  size_t start = 1;  // skip leading '/'
  const size_t len = target_path.size();

  while (start < len) {
    // Find end of current component
    size_t end = start;
    while (end < len && target_path[end] != '/') {
      ++end;
    }

    if (end == start) {
      // Empty component (double slash), skip
      start = end + 1;
      continue;
    }

    // Search for matching child using direct string comparison
    // No string allocation - compare against substring
    const Prim *found = nullptr;
    const size_t component_len = end - start;

    for (const auto &child : *current_children) {
      const std::string &name = child.element_name();
      if (name.size() == component_len &&
          target_path.compare(start, component_len, name) == 0) {
        found = &child;
        break;
      }
    }

    if (!found) {
      return nonstd::nullopt;
    }

    current = found;
    current_children = &current->children();
    start = end + 1;
  }

  return current;
}

}  // namespace

//
// -- Stage
//

struct StageMMapFileOwner {
  explicit StageMMapFileOwner(io::MMapFileHandle &&h) : handle(std::move(h)) {}
  ~StageMMapFileOwner() {
    std::string ignored;
    io::UnmapFile(handle, &ignored);
  }

  StageMMapFileOwner(const StageMMapFileOwner &) = delete;
  StageMMapFileOwner &operator=(const StageMMapFileOwner &) = delete;

  io::MMapFileHandle handle;
};

Stage::Stage() = default;
Stage::~Stage() = default;

Stage::Stage(const Stage &other)
    : _root_nodes(other._root_nodes),
      _root_node_nameSet(other._root_node_nameSet),
      name(other.name),
      default_root_node(other.default_root_node),
      stage_metas(other.stage_metas),
      _err(other._err),
      _warn(other._warn),
      _dirty(other._dirty),
      _prim_id_dirty(other._prim_id_dirty) {
  // mmap unique_ptr members are not copied.
  // mmap data is only valid for the original Stage loaded from USDC.

  // The lazy lookup caches hold `const Prim*` into other's _root_nodes tree;
  // those pointers are invalid for this copy. Force a rebuild against our own
  // tree on first use, and (under threading) use a private cache mutex.
  _dirty = true;
  _prim_id_dirty = true;
  _prim_path_cache.clear();
  _prim_id_cache.clear();
  // Unconditional (see stage.hh): copies must not share the source's mutex.
  _cache_mu = std::make_shared<std::mutex>();
}

Stage &Stage::operator=(const Stage &other) {
  if (this != &other) {
    _root_nodes = other._root_nodes;
    _root_node_nameSet = other._root_node_nameSet;
    name = other.name;
    default_root_node = other.default_root_node;
    stage_metas = other.stage_metas;
    _err = other._err;
    _warn = other._warn;
    _dirty = other._dirty;
    _prim_id_dirty = other._prim_id_dirty;
    // unique_ptr members are not copied (mmap data is not transferable)
    _mmap_table.reset();
    _mmap_source.reset();
    _mmap_file_owner.reset();
    _mmap_buffer_owner.reset();

    // See copy ctor: reset the lazy lookup caches (stale Prim* + shared mutex).
    _dirty = true;
    _prim_id_dirty = true;
    _prim_path_cache.clear();
    _prim_id_cache.clear();
    _cache_mu = std::make_shared<std::mutex>();
  }
  return *this;
}

Stage::Stage(Stage &&other) noexcept
    : _root_nodes(std::move(other._root_nodes)),
      _root_node_nameSet(std::move(other._root_node_nameSet)),
      name(std::move(other.name)),
      default_root_node(std::move(other.default_root_node)),
      stage_metas(std::move(other.stage_metas)),
      _err(std::move(other._err)),
      _warn(std::move(other._warn)),
      _dirty(true),
      _prim_id_dirty(true),
      _mmap_table(std::move(other._mmap_table)),
      _mmap_source(std::move(other._mmap_source)),
      _mmap_file_owner(std::move(other._mmap_file_owner)),
      _mmap_buffer_owner(std::move(other._mmap_buffer_owner)) {
  // Clear caches: the moved-from Stage's cached const Prim* point into this
  // Stage's _root_nodes after the vector move. A moved-from Stage is still
  // valid (C++ contract), so its next lookup must not return stale pointers.
  _prim_path_cache.clear();
  _prim_id_cache.clear();
  _cache_mu = std::make_shared<std::mutex>();
  other._dirty = true;
  other._prim_id_dirty = true;
  other._prim_path_cache.clear();
  other._prim_id_cache.clear();
  other._cache_mu = std::make_shared<std::mutex>();
}

Stage &Stage::operator=(Stage &&other) noexcept {
  if (this != &other) {
    _root_nodes = std::move(other._root_nodes);
    _root_node_nameSet = std::move(other._root_node_nameSet);
    name = std::move(other.name);
    default_root_node = std::move(other.default_root_node);
    stage_metas = std::move(other.stage_metas);
    _err = std::move(other._err);
    _warn = std::move(other._warn);
    _dirty = true;
    _prim_id_dirty = true;
    _mmap_table = std::move(other._mmap_table);
    _mmap_source = std::move(other._mmap_source);
    _mmap_file_owner = std::move(other._mmap_file_owner);
    _mmap_buffer_owner = std::move(other._mmap_buffer_owner);
    _prim_path_cache.clear();
    _prim_id_cache.clear();
    _cache_mu = std::make_shared<std::mutex>();
    // Clear caches in the moved-from Stage too (same rationale as move ctor).
    other._dirty = true;
    other._prim_id_dirty = true;
    other._prim_path_cache.clear();
    other._prim_id_cache.clear();
    other._cache_mu = std::make_shared<std::mutex>();
  }
  return *this;
}

nonstd::expected<const Prim *, std::string> Stage::GetPrimAtPath(
    const Path &path) const {
  DCOUT("GetPrimAtPath : " << path.prim_part() << "(input path: " << path
                           << ")");

  if (!path.is_valid()) {
    DCOUT("Invalid path.");
    return nonstd::make_unexpected("Path is invalid.\n");
  }

  if (path.is_relative_path()) {
    DCOUT("Relative path is todo.");
    return nonstd::make_unexpected("Relative path is TODO.\n");
  }

  if (!path.is_absolute_path()) {
    DCOUT("Not absolute path.");
    return nonstd::make_unexpected(
        "Path is not absolute. Non-absolute Path is TODO.\n");
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::unique_lock<std::mutex> cache_lock(*_cache_mu);
#endif
  if (_dirty) {
    DCOUT("clear cache.");
    // Clear cache.
    _prim_path_cache.clear();

    _dirty = false;
  } else {
    // First find from a cache. The Path is hashed directly via PathHasher
    // (no implicit std::string allocation from tstring_view conversion);
    // the cache owns its own copy of the Path. See concern #1 in review.md.
    auto ret = _prim_path_cache.find(path);
    if (ret != _prim_path_cache.end()) {
      DCOUT("Found cache.");
      return ret->second;
    }
  }
#if defined(LIGHTUSD_ENABLE_THREAD)
  cache_lock.unlock();  // release during the (lock-free) _root_nodes walk
#endif


  // Direct path-based lookup (no brute-force search)
  if (auto pv = GetPrimAtPathIterative(_root_nodes, path)) {
    // Add to cache.
    // Assume pointer address does not change unless dirty state.
#if defined(LIGHTUSD_ENABLE_THREAD)
    cache_lock.lock();  // re-acquire before mutating the cache (released above)
#endif
    // Key on the full Path (matches the find() above; the cache is Path-hashed
    // via PathHasher, so no string allocation).
    _prim_path_cache[path] = pv.value();
    return pv.value();
  }

  DCOUT("Not found.");
  return nonstd::make_unexpected("Cannot find path <" + path.full_path_name() +
                                 "> in the Stage.\n");
}

bool Stage::find_prim_at_path(const Path &path, const Prim *&prim,
                              std::string *err) const {
  nonstd::expected<const Prim *, std::string> ret = GetPrimAtPath(path);
  if (ret) {
    prim = ret.value();
    return true;
  } else {
    if (err) {
      (*err) = ret.error();
    }
    return false;
  }
}

bool Stage::find_prim_at_path(const Path &path, int64_t *prim_id,
                              std::string *err) const {
  if (!prim_id) {
    if (err) {
      (*err) = "`prim_id` argument is nullptr.\n";
    }
    return false;
  }

  nonstd::expected<const Prim *, std::string> ret = GetPrimAtPath(path);
  if (ret) {
    (*prim_id) = ret.value()->prim_id();
    return true;
  } else {
    if (err) {
      (*err) = ret.error();
    }
    return false;
  }
}

// Optimized iterative version using explicit stack
// Avoids recursion to save stack memory for deep hierarchies
static bool FindPrimByPrimIdIterative(uint64_t prim_id,
                               const std::vector<Prim> &root_nodes,
                               const Prim **primFound) {
  if (!primFound) {
    return false;
  }

  // Use explicit stack for DFS traversal (StackVector for stack allocation)
  // Store pointer to prim and current child index
  StackVector<std::pair<const Prim *, size_t>, 4> stack;
  stack.reserve(64);  // Pre-allocate for typical depth

  // Initialize stack with root nodes
  for (const auto &root : root_nodes) {
    if (root.prim_id() == int64_t(prim_id)) {
      (*primFound) = &root;
      return true;
    }
    if (!root.children().empty()) {
      stack.emplace_back(&root, 0);
    }
  }

  // Iterative DFS with iteration ceiling to prevent CPU-exhaustion on
  // maliciously large prim trees.
  static constexpr size_t kMaxIter = 1024 * 1024;  // 1M
  size_t iter = 0;
  while (!stack.empty()) {
    if (++iter > kMaxIter) {
      return false;
    }

    auto &top = stack.back();
    const Prim *current = top.first;
    size_t &child_idx = top.second;

    if (child_idx >= current->children().size()) {
      // All children processed, backtrack
      stack.pop_back();
      continue;
    }

    const Prim &child = current->children()[child_idx];
    ++child_idx;  // Move to next child for when we return

    // Check if this is the target
    if (child.prim_id() == int64_t(prim_id)) {
      (*primFound) = &child;
      return true;
    }

    // Push child to stack if it has children
    if (!child.children().empty()) {
      stack.emplace_back(&child, 0);
    }
  }

  return false;
}

bool Stage::find_prim_by_prim_id(const uint64_t prim_id, const Prim *&prim,
                                 std::string *err) const {
  if (prim_id < 1) {
    if (err) {
      (*err) = "Input prim_id must be 1 or greater.";
    }
    return false;
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::unique_lock<std::mutex> cache_lock(*_cache_mu);
#endif
  if (_prim_id_dirty) {
    DCOUT("clear prim_id cache.");
    // Clear cache.
    _prim_id_cache.clear();

    _prim_id_dirty = false;
  } else {
    // First find from a cache.
    auto ret = _prim_id_cache.find(prim_id);
    if (ret != _prim_id_cache.end()) {
      DCOUT("Found cache.");
      prim = ret->second;
      return true;
    }
  }
#if defined(LIGHTUSD_ENABLE_THREAD)
  cache_lock.unlock();  // release during the (lock-free) _root_nodes walk
#endif

  const Prim *p{nullptr};
  if (FindPrimByPrimIdIterative(prim_id, _root_nodes, &p)) {
#if defined(LIGHTUSD_ENABLE_THREAD)
    cache_lock.lock();
#endif
    _prim_id_cache[prim_id] = p;
    prim = p;
    return true;
  }

  return false;
}

bool Stage::find_prim_by_prim_id(const uint64_t prim_id, Prim *&prim,
                                 std::string *err) {
  if (prim_id < 1) {
    if (err) {
      (*err) = "Input prim_id must be 1 or greater.";
    }
    return false;
  }

  // Walk the prim tree directly (const version shares the same logic via
  // FindPrimByPrimIdIterative which operates on _root_nodes). Setting dirty
  // flags ensures any cache populated by a concurrent const lookup is skipped.
  const Prim *p{nullptr};
  if (FindPrimByPrimIdIterative(prim_id, _root_nodes, &p)) {
    prim = const_cast<Prim *>(p);
    _dirty = true;
    _prim_id_dirty = true;
    return true;
  }
  return false;
}

nonstd::expected<const Prim *, std::string> Stage::GetPrimFromRelativePath(
    const Prim &root, const Path &path) const {
  if (!path.is_valid()) {
    return nonstd::make_unexpected("Path is invalid.\n");
  }

  if (path.is_absolute_path()) {
    return nonstd::make_unexpected(
        "Path is absolute. Path must be relative.\n");
  }

  if (path.is_relative_path()) {
    // ok
  } else {
    return nonstd::make_unexpected("Invalid Path.\n");
  }

  if (!path.is_prim_path()) {
    return nonstd::make_unexpected("Path must be a relative Prim path.\n");
  }

  if (!root.absolute_path().is_valid() ||
      !root.absolute_path().is_absolute_path()) {
    return nonstd::make_unexpected(
        "Root Prim absolute path is not available.\n");
  }

  Path absolute_path;
  std::string resolve_err;
  if (!pathutil::ResolveRelativePath(root.absolute_path(), path,
                                     &absolute_path, &resolve_err)) {
    if (resolve_err.empty()) {
      resolve_err = "Failed to resolve relative Prim path.\n";
    }
    return nonstd::make_unexpected(resolve_err);
  }

  return GetPrimAtPath(absolute_path);
}

bool Stage::find_prim_from_relative_path(const Prim &root,
                                         const Path &relative_path,
                                         const Prim *&prim,
                                         std::string *err) const {
  nonstd::expected<const Prim *, std::string> ret =
      GetPrimFromRelativePath(root, relative_path);
  if (ret) {
    prim = ret.value();
    return true;
  } else {
    if (err) {
      (*err) = ret.error();
    }
    return false;
  }
}

namespace {


}  // namespace

std::string Stage::ExportToString(bool relative_path, bool parallel) const {
  (void)relative_path;
#if !defined(LIGHTUSD_ENABLE_THREAD)
  (void)parallel; // Threading disabled
#endif

  std::stringstream ss;

  ss << "#usda 1.0\n";

  std::stringstream meta_ss;
  meta_ss << print_layer_metas(stage_metas, /* indent */1);
  if (meta_ss.str().size()) {
    ss << "(\n";
    ss << meta_ss.str();
    ss << ")\n";
  }

  ss << "\n";

  if (stage_metas.primChildren.size() == _root_nodes.size()) {
    std::map<std::string, const Prim *> primNameTable;
    for (size_t i = 0; i < _root_nodes.size(); i++) {
      primNameTable.emplace(_root_nodes[i].element_name(), &_root_nodes[i]);
    }

#if defined(LIGHTUSD_ENABLE_THREAD)
    if (parallel) {
      // Parallel printing path
      std::vector<const Prim*> ordered_prims;
      ordered_prims.reserve(stage_metas.primChildren.size());

      for (size_t i = 0; i < stage_metas.primChildren.size(); i++) {
        value::token nameTok = stage_metas.primChildren[i];
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          ordered_prims.push_back(it->second);
        }
      }

      prim::ParallelPrintConfig config;
      ss << prim::print_prims_parallel(ordered_prims, 0, config);
    } else
#endif  // LIGHTUSD_ENABLE_THREAD
    {
      // Sequential printing path (original)
      for (size_t i = 0; i < stage_metas.primChildren.size(); i++) {
        value::token nameTok = stage_metas.primChildren[i];
        DCOUT(fmt::format("primChildren  {}/{} = {}", i,
                          stage_metas.primChildren.size(), nameTok.str()));
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          //PrimPrintRec(ss, *(it->second), 0);
          ss << prim::print_prim(*(it->second), 0);
          if (i != (stage_metas.primChildren.size() - 1)) {
            ss << "\n";
          }
        } else {
          // TODO: Report warning?
        }
      }
    }
  } else {
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (parallel) {
      // Parallel printing path
      std::vector<const Prim*> prims;
      prims.reserve(_root_nodes.size());
      for (size_t i = 0; i < _root_nodes.size(); i++) {
        prims.push_back(&_root_nodes[i]);
      }

      prim::ParallelPrintConfig config;
      ss << prim::print_prims_parallel(prims, 0, config);
    } else
#endif  // LIGHTUSD_ENABLE_THREAD
    {
      // Sequential printing path (original)
      for (size_t i = 0; i < _root_nodes.size(); i++) {
        //PrimPrintRec(ss, _root_nodes[i], 0);
        ss << prim::print_prim(_root_nodes[i], 0);

        if (i != (_root_nodes.size() - 1)) {
          ss << "\n";
        }
      }
    }
  }

  return ss.str();
}

bool Stage::allocate_prim_id(uint64_t *prim_id) const {
  if (!prim_id) {
    return false;
  }

  uint64_t val;
  if (_prim_id_allocator.Allocate(&val)) {
    (*prim_id) = val;
    return true;
  }

  return false;
}

bool Stage::release_prim_id(const uint64_t prim_id) const {
  return _prim_id_allocator.Release(prim_id);
}

bool Stage::has_prim_id(const uint64_t prim_id) const {
  return _prim_id_allocator.Has(prim_id);
}

namespace {

// Iterative version of ComputeAbsPathAndAssignPrimIdRec
// Uses explicit stack to avoid recursion
bool ComputeAbsPathAndAssignPrimIdIterative(const Stage &stage,
                                            std::vector<Prim> &root_prims,
                                            bool assign_prim_id,
                                            bool force_assign_prim_id,
                                            std::string *err) {
  // Stack entry: (prim pointer, parent path, child index)
  struct StackEntry {
    Prim *prim;
    Path parent_path;
    size_t child_idx;

    StackEntry(Prim *p, Path pp) : prim(p), parent_path(std::move(pp)), child_idx(0) {}
  };

  StackVector<StackEntry, 4> stack;
  stack.reserve(64);

  Path root_path("/", "");

  // Process each root prim
  for (Prim &root : root_prims) {
    if (root.element_name().empty()) {
      if (err) {
        (*err) += "Prim's elementName is empty. Prim's parent Path = /\n";
      }
      return false;
    }

    // Compute path and assign ID for root
    Path abs_path = root_path.AppendPrim(root.element_name());
    root.absolute_path() = abs_path;

    if (assign_prim_id) {
      if (force_assign_prim_id || (root.prim_id() < 1)) {
        uint64_t prim_id{0};
        if (!stage.allocate_prim_id(&prim_id)) {
          if (err) {
            (*err) += "Failed to assign unique Prim ID.\n";
          }
          return false;
        }
        root.prim_id() = int64_t(prim_id);
      }
    }

    if (!root.children().empty()) {
      stack.emplace_back(&root, abs_path);
    }

    // Process tree iteratively
    while (!stack.empty()) {
      auto &top = stack.back();
      Prim *current = top.prim;
      size_t &child_idx = top.child_idx;

      if (child_idx >= current->children().size()) {
        stack.pop_back();
        continue;
      }

      Prim &child = current->children()[child_idx];
      ++child_idx;

      if (child.element_name().empty()) {
        if (err) {
          (*err) += "Prim's elementName is empty. Prim's parent Path = " +
                    top.parent_path.full_path_name() + "\n";
        }
        return false;
      }

      Path child_abs_path = top.parent_path.AppendPrim(child.element_name());
      child.absolute_path() = child_abs_path;

      if (assign_prim_id) {
        if (force_assign_prim_id || (child.prim_id() < 1)) {
          uint64_t prim_id{0};
          if (!stage.allocate_prim_id(&prim_id)) {
            if (err) {
              (*err) += "Failed to assign unique Prim ID.\n";
            }
            return false;
          }
          child.prim_id() = int64_t(prim_id);
        }
      }

      if (!child.children().empty()) {
        stack.emplace_back(&child, child_abs_path);
      }
    }
  }

  return true;
}

}  // namespace

bool Stage::compute_absolute_prim_path_and_assign_prim_id(
    bool force_assign_prim_id) {
  if (!ComputeAbsPathAndAssignPrimIdIterative(*this, _root_nodes,
                                              /* assign_prim_id */ true,
                                              force_assign_prim_id, &_err)) {
    return false;
  }

  // TODO: Only set dirty when prim_id changed.
  _prim_id_dirty = true;

  return true;
}

bool Stage::compute_absolute_prim_path() {
  if (!ComputeAbsPathAndAssignPrimIdIterative(*this, _root_nodes,
                                              /* assign_prim_id */ false,
                                              /* force_assign_prim_id */ true,
                                              &_err)) {
    return false;
  }

  return true;
}

bool Stage::add_root_prim(Prim &&prim, bool rename_prim_name) {
  std::string elementName = prim.element_name();

  if (elementName.empty()) {
    if (rename_prim_name) {

      // assign default name `default`
      elementName = "default";

      if (!SetPrimElementName(prim.get_data(), elementName)) {
        PUSH_ERROR_AND_RETURN("Internal error. cannot modify Prim's elementName");
      }
      prim.element_path() = Path(elementName, /* prop_part */"");
    } else {
      PUSH_ERROR_AND_RETURN("Prim has empty elementName.");
    }
  }

  if (_root_nodes.size() != _root_node_nameSet.size()) {
    // Rebuild nameSet
    _root_node_nameSet.clear();
    for (size_t i = 0; i < _root_nodes.size(); i++) {
      if (_root_nodes[i].element_name().empty()) {
        PUSH_ERROR_AND_RETURN("Internal error: Existing root Prim's elementName is empty.");
      }

      if (_root_node_nameSet.count(_root_nodes[i].element_name())) {
        PUSH_ERROR_AND_RETURN("Internal error: Stage contains root Prim with same elementName.");
      }

      _root_node_nameSet.insert(_root_nodes[i].element_name());
    }
  }

  if (_root_node_nameSet.count(elementName)) {
    if (rename_prim_name) {
      std::string unique_name;
      if (!makeUniqueName(_root_node_nameSet, elementName, &unique_name)) {
        PUSH_ERROR_AND_RETURN(fmt::format("Internal error. cannot assign unique name for `{}`.\n", elementName));
      }

      elementName = unique_name;

      // Need to modify both Prim::data::name and Prim::elementPath
      if (!SetPrimElementName(prim.get_data(), elementName)) {
        PUSH_ERROR_AND_RETURN("Internal error. cannot modify Prim's elementName.");
      }
      prim.element_path() = Path(elementName, /* prop_part */"");
    } else {
      PUSH_ERROR_AND_RETURN(fmt::format("Prim name(elementName) {} already exists in children.\n", prim.element_name()));
    }
  }


  _root_node_nameSet.insert(elementName);
  _root_nodes.emplace_back(std::move(prim));

  _dirty = true;

  return true;


}

bool Stage::replace_root_prim(const std::string &prim_name, Prim &&prim) {

  if (prim_name.empty()) {
    PUSH_ERROR_AND_RETURN(fmt::format("prim_name is empty."));
  }

  if (!ValidatePrimElementName(prim_name)) {
    PUSH_ERROR_AND_RETURN(fmt::format("`{}` is not a valid Prim name.", prim_name));
  }

  if (_root_nodes.size() != _root_node_nameSet.size()) {
    // Rebuild nameSet
    _root_node_nameSet.clear();
    for (size_t i = 0; i < _root_nodes.size(); i++) {
      if (_root_nodes[i].element_name().empty()) {
        PUSH_ERROR_AND_RETURN("Internal error: Existing root Prim's elementName is empty.");
      }

      if (_root_node_nameSet.count(_root_nodes[i].element_name())) {
        PUSH_ERROR_AND_RETURN("Internal error: Stage contains root Prim with same elementName.");
      }

      _root_node_nameSet.insert(_root_nodes[i].element_name());
    }
  }

  // Simple linear scan
  auto result = std::find_if(_root_nodes.begin(), _root_nodes.end(), [prim_name](const Prim &p) {
    return (p.element_name() == prim_name);
  });

  if (result != _root_nodes.end()) {

    // Need to modify both Prim::data::name and Prim::elementPath
    if (!SetPrimElementName(prim.get_data(), prim_name)) {
      PUSH_ERROR_AND_RETURN("Internal error. cannot modify Prim's elementName.");
    }
    prim.element_path() = Path(prim_name, /* prop_part */"");

    (*result) = std::move(prim); // replace

  } else {

    // Need to modify both Prim::data::name and Prim::elementPath
    if (!SetPrimElementName(prim.get_data(), prim_name)) {
      PUSH_ERROR_AND_RETURN("Internal error. cannot modify Prim's elementName.");
    }
    prim.element_path() = Path(prim_name, /* prop_part */"");

    _root_node_nameSet.insert(prim_name);
    _root_nodes.emplace_back(std::move(prim)); // add
  }

  _dirty = true;
  _prim_id_dirty = true;

  return true;
}

namespace {

// Split an absolute prim path "/A/B/C" into ["A","B","C"].
bool SplitAbsPrimPath(const Path &path, std::vector<std::string> *out) {
  if (!path.is_valid() || !path.is_absolute_path()) return false;
  if (!out) return false;
  const std::string s = std::string(path.prim_part());
  if (s.empty() || s[0] != '/') return false;
  out->clear();
  size_t i = 1;
  while (i <= s.size()) {
    size_t j = s.find('/', i);
    if (j == std::string::npos) j = s.size();
    if (j > i) out->push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return true;
}

// Descend `roots` following `comps`, returning a mutable pointer to the prim at
// that path (nullptr if any component is missing). Empty `comps` => nullptr
// (caller treats that as the pseudo-root).
Prim *FindMutablePrim(std::vector<Prim> &roots,
                      const std::vector<std::string> &comps) {
  std::vector<Prim> *sib = &roots;
  Prim *cur = nullptr;
  for (const auto &name : comps) {
    Prim *next = nullptr;
    for (auto &p : *sib) {
      if (std::string(p.element_name()) == name) {
        next = &p;
        break;
      }
    }
    if (!next) return nullptr;
    cur = next;
    sib = &next->children();
  }
  return cur;
}

// Locate the mutable sibling vector + index of the prim at `comps`. `out_parent`
// is null when the target is a root prim.
bool FindPrimSlot(std::vector<Prim> &roots,
                  const std::vector<std::string> &comps,
                  std::vector<Prim> **out_siblings, size_t *out_index,
                  Prim **out_parent) {
  if (comps.empty()) return false;
  std::vector<Prim> *sib = &roots;
  Prim *parent = nullptr;
  for (size_t c = 0; c + 1 < comps.size(); c++) {
    Prim *next = nullptr;
    for (auto &p : *sib) {
      if (std::string(p.element_name()) == comps[c]) {
        next = &p;
        break;
      }
    }
    if (!next) return false;
    parent = next;
    sib = &next->children();
  }
  for (size_t i = 0; i < sib->size(); i++) {
    if (std::string((*sib)[i].element_name()) == comps.back()) {
      *out_siblings = sib;
      *out_index = i;
      *out_parent = parent;
      return true;
    }
  }
  return false;
}

// Replace `old_name` with `new_name` in a primChildren ordering list (no-op if
// the list is empty or does not contain `old_name`).
void RenameInPrimChildren(std::vector<value::token> &pc,
                          const std::string &old_name,
                          const std::string &new_name) {
  for (auto &tok : pc) {
    if (tok.str() == old_name) tok = value::token(new_name);
  }
}

void RemoveFromPrimChildren(std::vector<value::token> &pc,
                            const std::string &name) {
  pc.erase(std::remove_if(pc.begin(), pc.end(),
                          [&](const value::token &t) { return t.str() == name; }),
           pc.end());
}

}  // namespace

bool Stage::RenamePrim(const Path &path, const std::string &new_name,
                       std::string *err) {
  if (new_name.empty() || !ValidatePrimElementName(new_name)) {
    if (err) *err = fmt::format("`{}` is not a valid Prim name.", new_name);
    return false;
  }
  std::vector<std::string> comps;
  if (!SplitAbsPrimPath(path, &comps) || comps.empty()) {
    if (err) *err = "RenamePrim: path must be an absolute, non-root prim path.";
    return false;
  }

  std::vector<Prim> *sib = nullptr;
  size_t idx = 0;
  Prim *parent = nullptr;
  if (!FindPrimSlot(_root_nodes, comps, &sib, &idx, &parent)) {
    if (err) *err = fmt::format("RenamePrim: prim not found: {}",
                                std::string(path.prim_part()));
    return false;
  }

  const std::string old_name = std::string((*sib)[idx].element_name());
  if (old_name == new_name) return true;

  for (size_t i = 0; i < sib->size(); i++) {
    if (i != idx && std::string((*sib)[i].element_name()) == new_name) {
      if (err) *err = fmt::format(
          "RenamePrim: a sibling named `{}` already exists.", new_name);
      return false;
    }
  }

  Prim &target = (*sib)[idx];
  if (!SetPrimElementName(target.get_data(), new_name)) {
    if (err) *err = "RenamePrim: internal error setting elementName.";
    return false;
  }
  target.element_path() = Path(new_name, /* prop */ "");
  target.local_path() = Path(new_name, /* prop */ "");

  RenameInPrimChildren(
      parent ? parent->metas().primChildren : stage_metas.primChildren,
      old_name, new_name);

  if (!parent) _root_node_nameSet.clear();
  if (!compute_absolute_prim_path() && err) {
    *err = "RenamePrim: compute_absolute_prim_path failed\n" + _err;
    return false;
  }
  _dirty = true;
  _prim_id_dirty = true;
  return true;
}


bool Stage::RemovePrim(const Path &path, std::string *err) {
  std::vector<std::string> comps;
  if (!SplitAbsPrimPath(path, &comps) || comps.empty()) {
    if (err) *err = "RemovePrim: path must be an absolute, non-root prim path.";
    return false;
  }

  std::vector<Prim> *sib = nullptr;
  size_t idx = 0;
  Prim *parent = nullptr;
  if (!FindPrimSlot(_root_nodes, comps, &sib, &idx, &parent)) {
    if (err) *err = fmt::format("RemovePrim: prim not found: {}",
                                std::string(path.prim_part()));
    return false;
  }

  const std::string prim_name = std::string((*sib)[idx].element_name());
  sib->erase(sib->begin() + static_cast<std::ptrdiff_t>(idx));
  RemoveFromPrimChildren(
      parent ? parent->metas().primChildren : stage_metas.primChildren,
      prim_name);

  if (!parent) _root_node_nameSet.clear();
  _dirty = true;
  _prim_id_dirty = true;
  return true;
}

bool Stage::ReparentPrim(const Path &path, const Path &new_parent_path,
                         const std::string &new_name, std::string *err) {
  std::vector<std::string> comps;
  if (!SplitAbsPrimPath(path, &comps) || comps.empty()) {
    if (err) *err = "ReparentPrim: path must be an absolute, non-root prim path.";
    return false;
  }

  // Resolve destination parent. The pseudo-root `/` is a valid destination.
  const bool destIsRoot = new_parent_path.is_root_path();
  std::vector<std::string> pcomps;
  if (!destIsRoot) {
    if (!SplitAbsPrimPath(new_parent_path, &pcomps) || pcomps.empty()) {
      if (err) *err = "ReparentPrim: new_parent_path must be `/` or an absolute prim path.";
      return false;
    }
  }

  // Cycle guard: destination must not be the prim itself or a descendant.
  if (!destIsRoot && pcomps.size() >= comps.size()) {
    bool under = true;
    for (size_t i = 0; i < comps.size(); i++) {
      if (pcomps[i] != comps[i]) { under = false; break; }
    }
    if (under) {
      if (err) *err = "ReparentPrim: cannot reparent a prim under itself or a descendant.";
      return false;
    }
  }

  std::vector<Prim> *sib = nullptr;
  size_t idx = 0;
  Prim *parent = nullptr;
  if (!FindPrimSlot(_root_nodes, comps, &sib, &idx, &parent)) {
    if (err) *err = fmt::format("ReparentPrim: prim not found: {}",
                                std::string(path.prim_part()));
    return false;
  }

  // Destination children vector (confirm the destination parent exists).
  std::vector<Prim> *dest = nullptr;
  if (destIsRoot) {
    dest = &_root_nodes;
  } else {
    Prim *dparent = FindMutablePrim(_root_nodes, pcomps);
    if (!dparent) {
      if (err) *err = fmt::format("ReparentPrim: destination parent not found: {}",
                                  std::string(new_parent_path.prim_part()));
      return false;
    }
    dest = &dparent->children();
  }

  const std::string old_name = std::string((*sib)[idx].element_name());
  const std::string final_name = new_name.empty() ? old_name : new_name;
  if (!new_name.empty() && !ValidatePrimElementName(new_name)) {
    if (err) *err = fmt::format("`{}` is not a valid Prim name.", new_name);
    return false;
  }

  // Name clash in destination (the moved prim is not in `dest` yet).
  for (const auto &p : *dest) {
    if (std::string(p.element_name()) == final_name) {
      if (err) *err = fmt::format(
          "ReparentPrim: destination already has a child named `{}`.", final_name);
      return false;
    }
  }

  // Detach from source. (Erasing may shift later siblings, so the destination
  // is re-resolved by path afterwards; the cycle guard guarantees the
  // destination is not inside the moved subtree.)
  Prim moved = std::move((*sib)[idx]);
  sib->erase(sib->begin() + static_cast<std::ptrdiff_t>(idx));
  RemoveFromPrimChildren(
      parent ? parent->metas().primChildren : stage_metas.primChildren,
      old_name);

  // Apply optional rename to the detached prim.
  if (!new_name.empty() && new_name != old_name) {
    if (!SetPrimElementName(moved.get_data(), new_name)) {
      if (err) *err = "ReparentPrim: internal error setting elementName.";
      return false;
    }
    moved.element_path() = Path(new_name, "");
    moved.local_path() = Path(new_name, "");
  }

  // Re-resolve destination after the erase, then attach.
  std::vector<value::token> *dest_pc = nullptr;
  if (destIsRoot) {
    dest = &_root_nodes;
    dest_pc = &stage_metas.primChildren;
  } else {
    Prim *dparent = FindMutablePrim(_root_nodes, pcomps);
    if (!dparent) {
      if (err) *err = "ReparentPrim: internal error re-resolving destination.";
      return false;
    }
    dest = &dparent->children();
    dest_pc = &dparent->metas().primChildren;
  }
  // Keep destination primChildren ordering consistent only when it is authored.
  if (!dest_pc->empty()) {
    dest_pc->push_back(value::token(final_name));
  }
  dest->emplace_back(std::move(moved));

  _root_node_nameSet.clear();
  if (!compute_absolute_prim_path() && err) {
    *err = _err;
    return false;
  }
  _dirty = true;
  _prim_id_dirty = true;
  return true;
}

namespace {

// Iterative version of DumpPrimTree using explicit stack
// Avoids recursion to save stack memory for deep hierarchies
std::string DumpPrimTreeIterative(const std::vector<Prim> &root_prims) {
  std::stringstream ss;

  // Stack entries: (prim pointer, depth, child index)
  // child_idx == SIZE_MAX means we haven't processed this node yet
  struct StackEntry {
    const Prim *prim;
    uint32_t depth;
    size_t child_idx;
    StackEntry(const Prim *p, uint32_t d) : prim(p), depth(d), child_idx(SIZE_MAX) {}
  };

  StackVector<StackEntry, 4> stack;
  stack.reserve(64);

  // Push root prims in reverse order to process in forward order
  for (auto it = root_prims.rbegin(); it != root_prims.rend(); ++it) {
    stack.emplace_back(&(*it), 0);
  }

  while (!stack.empty()) {
    StackEntry &entry = stack.back();

    if (entry.child_idx == SIZE_MAX) {
      // First visit: output this node
      ss << pprint::Indent(entry.depth) << "\"" << entry.prim->element_name() << "\" "
         << entry.prim->absolute_path() << "\n";
      ss << pprint::Indent(entry.depth + 1) << fmt::format("prim_id {}", entry.prim->prim_id())
         << "\n";
      entry.child_idx = 0;
    }

    // Process children
    const auto &children = entry.prim->children();
    if (entry.child_idx < children.size()) {
      // Push next child and increment index
      size_t idx = entry.child_idx++;
      stack.emplace_back(&children[idx], entry.depth + 1);
    } else {
      // All children processed, pop this node
      stack.pop_back();
    }
  }

  return ss.str();
}

}  // namespace

std::string Stage::dump_prim_tree() const {
  return DumpPrimTreeIterative(_root_nodes);
}

namespace {

// Helper: estimate deep (allocated) memory of an Animatable's type-erased
// timesamples store.
static size_t EstimateTimeSamplesMemory(const value::TimeSamples *ts) {
  return ts ? ts->estimate_memory_usage() : 0;
}

// Helper: estimate deep memory of TypedAttribute<Animatable<std::vector<E>>>
// Uses get_value_ref() to avoid copying large data.
template <typename E>
static size_t EstimateTypedAttributeVectorMemory(const TypedAttribute<Animatable<std::vector<E>>> &attr) {
  const auto &opt_val = attr.get_value_ref();
  if (!opt_val) {
    return 0;
  }
  size_t total = 0;
  const Animatable<std::vector<E>> &anim = *opt_val;
  if (anim.has_timesamples()) {
    total += EstimateTimeSamplesMemory(anim.get_timesamples_ptr());
  } else if (anim.has_default()) {
    // Use const reference to avoid copying large arrays
    const std::vector<E> &val = anim.get_scalar_ref();
    total += val.capacity() * sizeof(E);
  }
  return total;
}

// Estimate deep memory for GeomMesh predefined attributes
static size_t EstimateGeomMeshDeepMemory(const GeomMesh &mesh) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorMemory(mesh.points);
  total += EstimateTypedAttributeVectorMemory(mesh.normals);
  total += EstimateTypedAttributeVectorMemory(mesh.velocities);
  total += EstimateTypedAttributeVectorMemory(mesh.faceVertexCounts);
  total += EstimateTypedAttributeVectorMemory(mesh.faceVertexIndices);
  return total;
}

// Estimate deep memory for GeomBasisCurves predefined attributes
static size_t EstimateGeomBasisCurvesDeepMemory(const GeomBasisCurves &curves) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorMemory(curves.points);
  total += EstimateTypedAttributeVectorMemory(curves.normals);
  total += EstimateTypedAttributeVectorMemory(curves.velocities);
  total += EstimateTypedAttributeVectorMemory(curves.curveVertexCounts);
  total += EstimateTypedAttributeVectorMemory(curves.widths);
  return total;
}

// Estimate deep memory for GeomPoints predefined attributes
static size_t EstimateGeomPointsDeepMemory(const GeomPoints &pts) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorMemory(pts.points);
  total += EstimateTypedAttributeVectorMemory(pts.normals);
  total += EstimateTypedAttributeVectorMemory(pts.velocities);
  total += EstimateTypedAttributeVectorMemory(pts.widths);
  total += EstimateTypedAttributeVectorMemory(pts.ids);
  return total;
}

// Dispatch to estimate deep memory (heap-allocated data inside typed attributes)
// for known concrete prim types. Returns 0 for unknown types.
static size_t EstimatePrimDataDeepMemory(const Prim &prim) {
  if (auto p = prim.as<GeomMesh>()) {
    return EstimateGeomMeshDeepMemory(*p);
  }
  if (auto p = prim.as<GeomBasisCurves>()) {
    return EstimateGeomBasisCurvesDeepMemory(*p);
  }
  if (auto p = prim.as<GeomPoints>()) {
    return EstimateGeomPointsDeepMemory(*p);
  }
  // Other types have smaller predefined attributes; props map handles extras
  return 0;
}

// Helper: estimate memory for a Property map
static size_t EstimatePropertyMapMemory(const std::map<std::string, Property> &props) {
  size_t total = 0;
  // std::map node overhead: ~48 bytes per node (key+value+pointers+color)
  for (const auto &kv : props) {
    total += 48; // map node overhead
    total += kv.first.capacity();
    total += kv.second.estimate_memory_usage();
  }
  return total;
}

// Helper: try to get the props map from a Prim by dispatching on its concrete type.
// Returns the estimated memory of the props map, or 0 if the type doesn't have props.
static size_t EstimatePrimPropsMemory(const Prim &prim) {
  // Macro to try casting to a concrete type and estimating its props
#define TRY_PROPS(__ty)                                         \
  if (auto pv = prim.as<__ty>()) {                              \
    return EstimatePropertyMapMemory(pv->props);                \
  }

  // GPrim-derived types (usdGeom)
  TRY_PROPS(GPrim)
  TRY_PROPS(Xform)
  TRY_PROPS(GeomMesh)
  TRY_PROPS(GeomBasisCurves)
  TRY_PROPS(GeomNurbsCurves)
  TRY_PROPS(GeomSphere)
  TRY_PROPS(GeomCube)
  TRY_PROPS(GeomCylinder)
  TRY_PROPS(GeomCone)
  TRY_PROPS(GeomCapsule)
  TRY_PROPS(GeomPoints)
  TRY_PROPS(GeomPointInstancer)
  TRY_PROPS(GeomCamera)
  TRY_PROPS(GeomSubset)

  // usdShade types (Material, Shader, NodeGraph share UsdShadePrim::props)
  TRY_PROPS(Material)
  TRY_PROPS(Shader)
  TRY_PROPS(NodeGraph)

  // usdSkel types
  TRY_PROPS(SkelRoot)
  TRY_PROPS(Skeleton)
  TRY_PROPS(SkelAnimation)
  TRY_PROPS(BlendShape)

  // usdLux light types
  TRY_PROPS(SphereLight)
  TRY_PROPS(DomeLight)
  TRY_PROPS(CylinderLight)
  TRY_PROPS(DiskLight)
  TRY_PROPS(RectLight)
  TRY_PROPS(DistantLight)
  TRY_PROPS(GeometryLight)
  TRY_PROPS(PortalLight)

  // Model and Scope (generic/unknown prim types)
  TRY_PROPS(Model)
  TRY_PROPS(Scope)

  // UsdVol / UsdRender placeholder prim types.
  TRY_PROPS(Volume)
  TRY_PROPS(OpenVDBAsset)
  TRY_PROPS(Field3DAsset)
  TRY_PROPS(RenderSettings)
  TRY_PROPS(RenderProduct)
  TRY_PROPS(RenderVar)
  TRY_PROPS(GenerativeProcedural)

#undef TRY_PROPS

  return 0;
}

// Helper: estimate memory for a single Prim (not including children)
static size_t EstimateSinglePrimMemory(const Prim &prim) {
  size_t total = sizeof(Prim);

  // Concrete prim data stored in _data (value::Value)
  // This now uses sizeof_stored() for MODEL types to get the correct struct size
  total += prim.data().estimate_memory_usage();

  // String members
  total += prim.element_name().size();
  total += prim.element_path().full_path_name().capacity();
  total += prim.prim_type_name().capacity();
  total += prim.absolute_path().full_path_name().capacity();
  total += prim.local_path().full_path_name().capacity();

  // Properties stored in the concrete prim type's props map
  total += EstimatePrimPropsMemory(prim);

  // Deep memory for predefined typed attributes (points, normals, etc.)
  total += EstimatePrimDataDeepMemory(prim);

  // Variant sets
  for (const auto &vs : prim.variantSets()) {
    total += 48; // map node overhead
    total += vs.first.capacity();
    total += sizeof(VariantSet);
  }

  return total;
}

}  // namespace

size_t Stage::estimate_memory_usage() const {
  size_t total = sizeof(Stage);

  // Stage metadata
  total += sizeof(StageMetas);

  // Iteratively walk the entire Prim tree using a stack (avoid recursion for deep trees)
  struct StackEntry {
    const std::vector<Prim> *siblings;
    size_t index;
  };

  std::vector<StackEntry> stack;
  if (!_root_nodes.empty()) {
    // Account for root vector capacity
    total += _root_nodes.capacity() * sizeof(Prim);
    stack.push_back({&_root_nodes, 0});
  }

  while (!stack.empty()) {
    StackEntry &entry = stack.back();
    if (entry.index >= entry.siblings->size()) {
      stack.pop_back();
      continue;
    }

    const Prim &prim = (*entry.siblings)[entry.index];
    entry.index++;

    // Estimate this prim's memory (excluding children, which we'll traverse)
    total += EstimateSinglePrimMemory(prim);

    // Push children onto the stack
    const auto &children = prim.children();
    if (!children.empty()) {
      // Account for children vector capacity (the Prim objects themselves
      // are accounted for when we visit each child)
      total += children.capacity() * sizeof(Prim);
      stack.push_back({&children, 0});
    }
  }

  // Internal string storage
  total += _warn.capacity();
  total += _err.capacity();

  // Prim ID management
  total += _prim_id_cache.size() * (sizeof(uint64_t) + sizeof(const Prim*)) * 2;

  return total;
}

// --- Actual (size-based) helpers for estimate_memory_usage_detail ---

namespace {

static size_t EstimatePropertyMapActualMemory(const std::map<std::string, Property> &props) {
  size_t total = 0;
  for (const auto &kv : props) {
    total += 48; // map node overhead (fixed, same in both modes)
    total += kv.first.size();
    total += kv.second.estimate_actual_usage();
  }
  return total;
}

static size_t EstimatePrimPropsActualMemory(const Prim &prim) {
#define TRY_PROPS_ACTUAL(__ty)                                       \
  if (auto pv = prim.as<__ty>()) {                                   \
    return EstimatePropertyMapActualMemory(pv->props);               \
  }

  TRY_PROPS_ACTUAL(GPrim)
  TRY_PROPS_ACTUAL(Xform)
  TRY_PROPS_ACTUAL(GeomMesh)
  TRY_PROPS_ACTUAL(GeomBasisCurves)
  TRY_PROPS_ACTUAL(GeomNurbsCurves)
  TRY_PROPS_ACTUAL(GeomSphere)
  TRY_PROPS_ACTUAL(GeomCube)
  TRY_PROPS_ACTUAL(GeomCylinder)
  TRY_PROPS_ACTUAL(GeomCone)
  TRY_PROPS_ACTUAL(GeomCapsule)
  TRY_PROPS_ACTUAL(GeomPoints)
  TRY_PROPS_ACTUAL(GeomPointInstancer)
  TRY_PROPS_ACTUAL(GeomCamera)
  TRY_PROPS_ACTUAL(GeomSubset)
  TRY_PROPS_ACTUAL(Material)
  TRY_PROPS_ACTUAL(Shader)
  TRY_PROPS_ACTUAL(NodeGraph)
  TRY_PROPS_ACTUAL(SkelRoot)
  TRY_PROPS_ACTUAL(Skeleton)
  TRY_PROPS_ACTUAL(SkelAnimation)
  TRY_PROPS_ACTUAL(BlendShape)
  TRY_PROPS_ACTUAL(SphereLight)
  TRY_PROPS_ACTUAL(DomeLight)
  TRY_PROPS_ACTUAL(CylinderLight)
  TRY_PROPS_ACTUAL(DiskLight)
  TRY_PROPS_ACTUAL(RectLight)
  TRY_PROPS_ACTUAL(DistantLight)
  TRY_PROPS_ACTUAL(GeometryLight)
  TRY_PROPS_ACTUAL(PortalLight)
  TRY_PROPS_ACTUAL(Model)
  TRY_PROPS_ACTUAL(Scope)

#undef TRY_PROPS_ACTUAL

  return 0;
}

// Deep (size-based) memory of an Animatable's type-erased timesamples store.
static size_t EstimateTimeSamplesActualMemory(const value::TimeSamples *ts) {
  return ts ? ts->estimate_actual_usage() : 0;
}

template <typename E>
static size_t EstimateTypedAttributeVectorActualMemory(const TypedAttribute<Animatable<std::vector<E>>> &attr) {
  const auto &opt_val = attr.get_value_ref();
  if (!opt_val) {
    return 0;
  }
  size_t total = 0;
  const Animatable<std::vector<E>> &anim = *opt_val;
  if (anim.has_timesamples()) {
    total += EstimateTimeSamplesActualMemory(anim.get_timesamples_ptr());
  } else if (anim.has_default()) {
    const std::vector<E> &val = anim.get_scalar_ref();
    total += val.size() * sizeof(E);
  }
  return total;
}

static size_t EstimateGeomMeshDeepActualMemory(const GeomMesh &mesh) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorActualMemory(mesh.points);
  total += EstimateTypedAttributeVectorActualMemory(mesh.normals);
  total += EstimateTypedAttributeVectorActualMemory(mesh.velocities);
  total += EstimateTypedAttributeVectorActualMemory(mesh.faceVertexCounts);
  total += EstimateTypedAttributeVectorActualMemory(mesh.faceVertexIndices);
  return total;
}

static size_t EstimateGeomBasisCurvesDeepActualMemory(const GeomBasisCurves &curves) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorActualMemory(curves.points);
  total += EstimateTypedAttributeVectorActualMemory(curves.normals);
  total += EstimateTypedAttributeVectorActualMemory(curves.velocities);
  total += EstimateTypedAttributeVectorActualMemory(curves.curveVertexCounts);
  total += EstimateTypedAttributeVectorActualMemory(curves.widths);
  return total;
}

static size_t EstimateGeomPointsDeepActualMemory(const GeomPoints &pts) {
  size_t total = 0;
  total += EstimateTypedAttributeVectorActualMemory(pts.points);
  total += EstimateTypedAttributeVectorActualMemory(pts.normals);
  total += EstimateTypedAttributeVectorActualMemory(pts.velocities);
  total += EstimateTypedAttributeVectorActualMemory(pts.widths);
  total += EstimateTypedAttributeVectorActualMemory(pts.ids);
  return total;
}

static size_t EstimatePrimDataDeepActualMemory(const Prim &prim) {
  if (auto p = prim.as<GeomMesh>()) {
    return EstimateGeomMeshDeepActualMemory(*p);
  }
  if (auto p = prim.as<GeomBasisCurves>()) {
    return EstimateGeomBasisCurvesDeepActualMemory(*p);
  }
  if (auto p = prim.as<GeomPoints>()) {
    return EstimateGeomPointsDeepActualMemory(*p);
  }
  return 0;
}

static size_t EstimateSinglePrimActualMemory(const Prim &prim) {
  size_t total = sizeof(Prim);

  total += prim.data().estimate_actual_usage();

  // Strings: use size() instead of capacity()
  total += prim.element_name().size();
  total += prim.element_path().full_path_name().size();
  total += prim.prim_type_name().size();
  total += prim.absolute_path().full_path_name().size();
  total += prim.local_path().full_path_name().size();

  total += EstimatePrimPropsActualMemory(prim);
  total += EstimatePrimDataDeepActualMemory(prim);

  for (const auto &vs : prim.variantSets()) {
    total += 48;
    total += vs.first.size();
    total += sizeof(VariantSet);
  }

  return total;
}

}  // namespace

Stage::MemoryUsageDetail Stage::estimate_memory_usage_detail() const {
  MemoryUsageDetail detail;

  size_t alloc_base = sizeof(Stage) + sizeof(StageMetas);
  size_t actual_base = alloc_base;

  struct StackEntry {
    const std::vector<Prim> *siblings;
    size_t index;
  };

  std::vector<StackEntry> stack;
  if (!_root_nodes.empty()) {
    alloc_base += _root_nodes.capacity() * sizeof(Prim);
    actual_base += _root_nodes.size() * sizeof(Prim);
    stack.push_back({&_root_nodes, 0});
  }

  size_t alloc_prims = 0;
  size_t actual_prims = 0;

  while (!stack.empty()) {
    StackEntry &entry = stack.back();
    if (entry.index >= entry.siblings->size()) {
      stack.pop_back();
      continue;
    }

    const Prim &prim = (*entry.siblings)[entry.index];
    entry.index++;

    alloc_prims += EstimateSinglePrimMemory(prim);
    actual_prims += EstimateSinglePrimActualMemory(prim);

    const auto &children = prim.children();
    if (!children.empty()) {
      alloc_base += children.capacity() * sizeof(Prim);
      actual_base += children.size() * sizeof(Prim);
      stack.push_back({&children, 0});
    }
  }

  // Internal strings
  alloc_base += _warn.capacity() + _err.capacity();
  actual_base += _warn.size() + _err.size();

  // Prim ID cache
  size_t cache_size = _prim_id_cache.size() * (sizeof(uint64_t) + sizeof(const Prim*)) * 2;
  alloc_base += cache_size;
  actual_base += cache_size;

  detail.allocated_bytes = alloc_base + alloc_prims;
  detail.actual_bytes = actual_base + actual_prims;

  return detail;
}

void Stage::set_mmap_table(MMapArrayTable &&table) {
  _mmap_table.reset(new MMapArrayTable(std::move(table)));
}

void Stage::set_mmap_source(const MMapDataSource &src) {
  _mmap_file_owner.reset();
  _mmap_buffer_owner.reset();
  _mmap_source.reset(new MMapDataSource(src));
}

bool Stage::adopt_mmap_file(io::MMapFileHandle &&handle) {
  if (!_mmap_source || !handle.addr || handle.size == 0) {
    return false;
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(handle.addr);
  const uintptr_t src = reinterpret_cast<uintptr_t>(_mmap_source->addr());
  if (handle.size > uint64_t((std::numeric_limits<uintptr_t>::max)() - base)) {
    return false;
  }
  const uintptr_t end = base + static_cast<uintptr_t>(handle.size);
  if (src < base || src >= end) {
    return false;
  }

  const uint64_t source_offset = static_cast<uint64_t>(src - base);
  if (_mmap_source->size() > handle.size - source_offset) {
    return false;
  }

  _mmap_buffer_owner.reset();
  _mmap_file_owner.reset(new StageMMapFileOwner(std::move(handle)));
  return true;
}

bool Stage::adopt_mmap_buffer(std::vector<uint8_t> &&buffer) {
  if (!_mmap_source || buffer.empty()) {
    return false;
  }

  const uintptr_t base = reinterpret_cast<uintptr_t>(buffer.data());
  const uintptr_t src = reinterpret_cast<uintptr_t>(_mmap_source->addr());
  const uint64_t buffer_size = static_cast<uint64_t>(buffer.size());
  if (buffer_size > uint64_t((std::numeric_limits<uintptr_t>::max)() - base)) {
    return false;
  }
  const uintptr_t end = base + static_cast<uintptr_t>(buffer_size);
  if (src < base || src >= end) {
    return false;
  }

  const uint64_t source_offset = static_cast<uint64_t>(src - base);
  const uint64_t source_size = _mmap_source->size();
  if (source_size > buffer_size - source_offset) {
    return false;
  }

  _mmap_file_owner.reset();
  _mmap_buffer_owner.reset(new std::vector<uint8_t>(std::move(buffer)));
  _mmap_source.reset(new MMapDataSource(
      _mmap_buffer_owner->data() + static_cast<size_t>(source_offset),
      source_size));
  return true;
}

void Stage::clear_mmap_data() {
  _mmap_table.reset();
  _mmap_source.reset();
  _mmap_file_owner.reset();
  _mmap_buffer_owner.reset();
}

bool Stage::has_mmap_zero_copy() const {
  return _mmap_table && _mmap_source && _mmap_source->is_valid() && !_mmap_table->empty();
}

// AOUSD Core Spec 11.3.3: Build instance prototype registry.
// Scans all prims for instanceable=true AND composition arcs,
// groups by InstanceKey (128-bit hash of composition arc signature).
size_t Stage::BuildInstancePrototypes() {
  _instance_to_prototype.clear();
  _instance_key_to_prototype.clear();
  _prototype_source_paths.clear();
  _prototype_count = 0;

  // If instance data was imported from CompositionGraph, use it directly
  if (_instance_data_imported) {
    return _prototype_count;
  }

  // Recursive helper with depth limit
  std::function<void(const Prim &, const std::string &, int)> scan;
  scan = [&](const Prim &prim, const std::string &parent_path, int depth) {
    if (depth > 4096) return;  // guard against stack overflow
    std::string prim_path = parent_path + "/" + prim.element_name();

    // Per AOUSD Spec 11.3.3: instanceable=true AND at least one composition arc
    if (prim.IsInstance() && prim.HasCompositionArcs()) {
      InstanceKey key;
      if (ComputeInstanceKeyFromPrimMetas(prim.metas(), prim.prim_type_name(),
                                          &key) &&
          key.is_valid()) {
        auto it = _instance_key_to_prototype.find(key);
        if (it == _instance_key_to_prototype.end()) {
          // New prototype
          int idx = static_cast<int>(_prototype_count);
          _instance_key_to_prototype[key] = idx;
          _prototype_source_paths.push_back(prim_path);
          _prototype_count++;
          _instance_to_prototype[prim_path] = idx;
        } else {
          // Existing prototype
          _instance_to_prototype[prim_path] = it->second;
        }
      }
    }

    for (const auto &child : prim.children()) {
      scan(child, prim_path, depth + 1);
    }
  };

  for (const auto &root : _root_nodes) {
    scan(root, "", 0);
  }

  return _prototype_count;
}

void Stage::ImportInstanceData(
    const std::unordered_map<std::string, int> &instance_to_prototype,
    size_t prototype_count) {
  _instance_to_prototype = instance_to_prototype;
  _prototype_count = prototype_count;
  _instance_data_imported = true;

  // Rebuild source paths from imported data
  _prototype_source_paths.resize(prototype_count);
  for (const auto &entry : _instance_to_prototype) {
    int idx = entry.second;
    if (idx >= 0 && static_cast<size_t>(idx) < _prototype_source_paths.size()) {
      // First path encountered for this prototype becomes the source
      if (_prototype_source_paths[static_cast<size_t>(idx)].empty()) {
        _prototype_source_paths[static_cast<size_t>(idx)] = entry.first;
      }
    }
  }
}

}  // namespace lightusd
