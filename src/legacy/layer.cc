// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 Light Transport Entertainment Inc.

#include "layer.hh"
#include "core/prim-spec.hh"  // PrimSpec
#include "core/layer-types.hh"  // LayerMetas
#include "path-util.hh"   // For Path
#include "str-util.hh"    // For split function
#include "tiny-container.hh"
#include "common-macros.inc"
#include "tiny-format.hh"

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <memory>
#include <mutex>
#endif

namespace lightusd {

namespace {

// Generic iterative tree search using explicit stack
// Returns true if any PrimSpec satisfies the predicate
template <typename Predicate>
bool HasPrimSpecWithCondition(const PrimSpec &root, Predicate pred,
                              const uint32_t max_depth) {
  // Check root first
  if (pred(root)) {
    return true;
  }

  // Use explicit stack for DFS traversal (StackVector for stack allocation)
  StackVector<std::pair<const PrimSpec *, size_t>, 4> stack;
  stack.reserve(64);

  if ((max_depth > 0) && !root.children().empty()) {
    stack.emplace_back(&root, 0);
  }

  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= kMaxDefaultTraversalLimit) {
      break;
    }
    auto &top = stack.back();
    const PrimSpec *current = top.first;
    size_t &child_idx = top.second;

    if (child_idx >= current->children().size()) {
      stack.pop_back();
      continue;
    }

    const PrimSpec &child = current->children()[child_idx];
    ++child_idx;

    if (pred(child)) {
      return true;
    }

    // The stack size is the child's depth: the root frame is present while
    // visiting depth-one children. Do not descend beyond the caller's limit.
    if ((stack.size() < max_depth) && !child.children().empty()) {
      stack.emplace_back(&child, 0);
    }
  }

  return false;
}

// Iterative predicates for each Has* function
bool HasReferencesIterative(const PrimSpec &primspec,
                            const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    return ps.metas().references.has_value();
  }, max_depth);
}

bool HasPayloadIterative(const PrimSpec &primspec, const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    return ps.metas().payload.has_value();
  }, max_depth);
}

bool HasVariantIterative(const PrimSpec &primspec, const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    // TODO: Also check if PrimSpec::variantSets is empty?
    return ps.metas().variants.has_value() && ps.metas().variantSets.has_value();
  }, max_depth);
}

bool HasInheritsIterative(const PrimSpec &primspec, const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    return ps.metas().inherits.has_value();
  }, max_depth);
}

bool HasSpecializesIterative(const PrimSpec &primspec,
                             const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    return ps.metas().specializes.has_value();
  }, max_depth);
}

bool HasOverIterative(const PrimSpec &primspec, const uint32_t max_depth) {
  return HasPrimSpecWithCondition(primspec, [](const PrimSpec &ps) {
    return ps.specifier() == Specifier::Over;
  }, max_depth);
}

// Optimized iterative path lookup starting from a single root PrimSpec
// Uses direct path component matching without string allocation
nonstd::optional<const PrimSpec *> GetPrimSpecAtPathFromRoot(
    const PrimSpec &root, const Path &path) {
  const std::string &target_path = path.full_path_name();

  // Must be absolute path starting with '/'
  if (target_path.empty() || target_path[0] != '/') {
    return nonstd::nullopt;
  }

  // Root path "/" has no primspec
  if (target_path.size() == 1) {
    return nonstd::nullopt;
  }

  // Parse first component to check if it matches root
  size_t start = 1;  // skip leading '/'
  const size_t len = target_path.size();

  size_t end = start;
  while (end < len && target_path[end] != '/') {
    ++end;
  }

  // Check if first component matches root name
  const size_t first_len = end - start;
  const std::string &root_name = root.name();
  if (root_name.size() != first_len ||
      target_path.compare(start, first_len, root_name) != 0) {
    return nonstd::nullopt;
  }

  // If path is just the root, return it
  if (end >= len) {
    return &root;
  }

  // Navigate down the tree
  const PrimSpec *current = &root;
  start = end + 1;

  while (start < len) {
    // Find end of current component
    end = start;
    while (end < len && target_path[end] != '/') {
      ++end;
    }

    if (end == start) {
      // Empty component (double slash), skip
      start = end + 1;
      continue;
    }

    // Search for matching child
    const PrimSpec *found = nullptr;
    const size_t component_len = end - start;

    for (const auto &child : current->children()) {
      const std::string &name = child.name();
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
    start = end + 1;
  }

  return current;
}

// Estimate PrimSpec memory usage using iterative DFS traversal
static size_t EstimatePrimSpecMemory(const PrimSpec& root) {
  size_t total = 0;

  StackVector<const PrimSpec *, 4> stack;
  stack.reserve(64);

  stack.push_back(&root);

  while (!stack.empty()) {
    const PrimSpec *current = stack.back();
    stack.pop_back();

    total += sizeof(PrimSpec);

    // String members
    total += current->name().capacity();
    total += current->typeName().capacity();

    // Properties map
    for (const auto& prop_pair : current->props()) {
      total += prop_pair.first.capacity();
      total += prop_pair.second.estimate_memory_usage();
    }

    // Children
    for (const auto& child : current->children()) {
      stack.push_back(&child);
    }

    // VariantSets
    for (const auto& vs_pair : current->variantSets()) {
      total += vs_pair.first.capacity();
      total += sizeof(VariantSetSpec);
      total += vs_pair.second.name.capacity();
      for (const auto& v_pair : vs_pair.second.variantSet) {
        total += v_pair.first.capacity();
        stack.push_back(&v_pair.second);
      }
    }

    // Limit check
    if (stack.size() > kMaxDefaultTraversalLimit) {
      break;
    }
  }

  return total;
}

}  // namespace

// LayerImpl - internal implementation
struct LayerImpl {
  std::string _name;  // layer name ~= USD filename

  // key = prim name
  std::unordered_map<std::string, PrimSpec> _prim_specs;
  LayerMetas _metas;

  // Cached primspec path.
  // key : full_path_name (e.g. "/A/B/C")
  mutable std::unordered_map<std::string, const PrimSpec *> _primspec_path_cache;
  mutable bool _dirty{true};

#if defined(LIGHTUSD_ENABLE_THREAD)
  // Guards the derived/cached state on this LayerImpl so the `const` accessors
  // that lazily fill it are safe under concurrent readers (e.g. pcp::Cache
  // parallel builds share a Layer): the lookup cache above (find_primspec_at)
  // and the composition flags below (check_/has_unresolved_*). shared_ptr keeps
  // LayerImpl implicitly copyable/movable; the immutable PrimSpec tree
  // (_prim_specs) is read lock-free during those computations. None of the
  // guarded methods call each other, so a plain (non-recursive) mutex is safe.
  //
  // CONTRACT: this mutex only makes CONCURRENT CONST READS of a shared Layer
  // safe. A Layer shared across threads for composition must NOT be copied,
  // copy-assigned, mutated (add/replace/del primspec), or destroyed during that
  // window: reset_lookup_cache() REPLACES this shared_ptr on copy, and the
  // lockers bind to `*_cache_mu` rather than holding a ref, so a concurrent
  // copy/destroy could free the mutex out from under a holder. We intentionally
  // do NOT copy the shared_ptr per lock (that would add an atomic refcount
  // bounce to the hot find_primspec_at path); the engine never copies/mutates a
  // shared layer mid-build, so the contract holds. See doc/datarace.md.
  std::shared_ptr<std::mutex> _cache_mu{std::make_shared<std::mutex>()};
#endif

  // Reset the derived path->PrimSpec lookup cache. Its values are
  // `const PrimSpec*` pointing INTO _prim_specs, so they must never survive a
  // copy: a copied layer has to drop them (and take a fresh mutex) and rebuild
  // lazily against its OWN _prim_specs tree. Without this, find_primspec_at()
  // on a copied Layer returned the SOURCE layer's PrimSpec pointers -- dangling
  // once the source is destroyed, and a shared mutex across two layers.
  void reset_lookup_cache() {
    _primspec_path_cache.clear();
    _dirty = true;
#if defined(LIGHTUSD_ENABLE_THREAD)
    _cache_mu = std::make_shared<std::mutex>();
#endif
  }

  // Cached flags for composition.
  // true by default even PrimSpec tree does not contain any `references`, `payload`, etc.
  mutable bool _has_unresolved_references{true};
  mutable bool _has_unresolved_payload{true};
  mutable bool _has_unresolved_variant{true};
  mutable bool _has_unresolved_inherits{true};
  mutable bool _has_unresolved_specializes{true};
  mutable bool _has_over_primspec{true};
  mutable bool _has_class_primspec{true};

  //
  // Record AssetResolution state(search paths, current working directory)
  // when this layer is opened by compostion(`references`, `payload`, `subLayers`)
  //
  mutable std::string _current_working_path;
  mutable std::vector<std::string> _asset_search_paths;
  mutable void *_asset_resolution_userdata{nullptr};
};

// Layer implementation

Layer::Layer() : _impl(std::make_unique<LayerImpl>()) {}

Layer::~Layer() = default;

Layer::Layer(const Layer& other)
    : _impl(other._impl ? std::make_unique<LayerImpl>(*other._impl)
                        : std::make_unique<LayerImpl>()) {
  // The implicit LayerImpl copy duplicated the source's lookup cache (pointers
  // into the SOURCE's _prim_specs) and shared its mutex; reset_lookup_cache()
  // drops both (clears the cache, marks dirty, and installs a fresh mutex) so
  // this copy rebuilds against its own tree. (Move ctor below is unaffected:
  // moving the node-based _prim_specs keeps element addresses stable, so its
  // cache stays valid.)
  if (_impl) {
    _impl->reset_lookup_cache();
  }
}

Layer& Layer::operator=(const Layer& other) {
  if (this != &other) {
    // `this` may be a moved-from Layer (null _impl); restore the invariant
    // that a live Layer always owns a LayerImpl before dereferencing it.
    if (!_impl) {
      _impl = std::make_unique<LayerImpl>();
    }
    if (other._impl) {
      *_impl = *other._impl;
      _impl->reset_lookup_cache();  // drop the source's pointer cache + mutex
    } else {
      *_impl = LayerImpl{};
    }
    // See copy ctor: reset the per-layer lookup cache (stale pointers + shared mutex).
    _impl->_dirty = true;
    _impl->_primspec_path_cache.clear();
#if defined(LIGHTUSD_ENABLE_THREAD)
    _impl->_cache_mu = std::make_shared<std::mutex>();
#endif
  }
  return *this;
}

// A moved-from Layer must remain valid and assignable (its _impl is reused as a
// composition output target via operator=), so re-establish an empty LayerImpl
// in the source instead of leaving _impl null.
Layer::Layer(Layer&& other) noexcept : _impl(std::move(other._impl)) {
  other._impl = std::make_unique<LayerImpl>();
}

Layer& Layer::operator=(Layer&& other) noexcept {
  if (this != &other) {
    _impl = std::move(other._impl);
    if (!_impl) {
      _impl = std::make_unique<LayerImpl>();
    }
    other._impl = std::make_unique<LayerImpl>();
  }
  return *this;
}

const std::string Layer::name() const {
  return _impl->_name;
}

void Layer::set_name(const std::string name) {
  _impl->_name = name;
}

void Layer::clear_primspecs() {
  _impl->_prim_specs.clear();
}

bool Layer::has_primspec(const std::string &primname) const {
  return _impl->_prim_specs.count(primname) > 0;
}

const LayerMetas &Layer::metas() const {
  return _impl->_metas;
}

LayerMetas &Layer::metas() {
  return _impl->_metas;
}

const std::unordered_map<std::string, PrimSpec> &Layer::primspecs() const {
  return _impl->_prim_specs;
}

std::unordered_map<std::string, PrimSpec> &Layer::primspecs() {
  return _impl->_prim_specs;
}

bool Layer::add_primspec(const std::string &name, const PrimSpec &ps) {
  if (name.empty()) {
    return false;
  }

  if (!ValidatePrimElementName(name)) {
    return false;
  }

  if (has_primspec(name)) {
    return false;
  }

  _impl->_prim_specs.emplace(name, ps);

  return true;
}

bool Layer::emplace_primspec(const std::string &name, PrimSpec &&ps) {
  if (name.empty()) {
    return false;
  }

  if (!ValidatePrimElementName(name)) {
    return false;
  }

  if (has_primspec(name)) {
    return false;
  }

  _impl->_prim_specs.emplace(name, std::move(ps));

  return true;
}

bool Layer::replace_primspec(const std::string &name, const PrimSpec &ps) {
  if (name.empty()) {
    return false;
  }

  if (!ValidatePrimElementName(name)) {
    return false;
  }

  if (!has_primspec(name)) {
    return false;
  }

  _impl->_prim_specs.at(name) = ps;

  return true;
}

bool Layer::replace_primspec(const std::string &name, PrimSpec &&ps) {
  if (name.empty()) {
    return false;
  }

  if (!ValidatePrimElementName(name)) {
    return false;
  }

  if (!has_primspec(name)) {
    return false;
  }

  _impl->_prim_specs.at(name) = std::move(ps);

  return true;
}

// These getters read flags computed (and cached) by the matching
// check_unresolved_*() methods. Take the same gated lock for a consistent read
// when a parallel build path touches a shared Layer.
bool Layer::has_unresolved_references() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_unresolved_references;
}

bool Layer::has_unresolved_payload() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_unresolved_payload;
}

bool Layer::has_unresolved_variant() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_unresolved_variant;
}

bool Layer::has_over_primspec() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_over_primspec;
}

bool Layer::has_class_primspec() const {
  // _has_class_primspec is fixed at construction (no check_class_primspec()
  // computes it), so this read needs no synchronization.
  return _impl->_has_class_primspec;
}

bool Layer::has_unresolved_inherits() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_unresolved_inherits;
}

bool Layer::has_unresolved_specializes() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  return _impl->_has_unresolved_specializes;
}

void Layer::set_asset_resolution_state(
  const std::string &cwp, const std::vector<std::string> &search_paths, void *userdata) {
  _impl->_current_working_path = cwp;
  _impl->_asset_search_paths = search_paths;
  _impl->_asset_resolution_userdata = userdata;
}

void Layer::get_asset_resolution_state(
  std::string &cwp, std::vector<std::string> &search_paths, void *&userdata) {
  cwp = _impl->_current_working_path;
  search_paths = _impl->_asset_search_paths;
  userdata = _impl->_asset_resolution_userdata;
}

const std::string Layer::get_current_working_path() const {
  return _impl->_current_working_path;
}

const std::vector<std::string> Layer::get_asset_search_paths() const {
  return _impl->_asset_search_paths;
}

bool Layer::find_primspec_at(const Path &path, const PrimSpec **ps,
                             std::string *err) const {

#define PushError(msg) \
  if (err) {           \
    (*err) += msg;     \
    (*err) += "\n";    \
  }
  if (!ps) {
    PUSH_ERROR_AND_RETURN("Invalid PrimSpec dst argument");
  }

  if (!path.is_valid()) {
    DCOUT("Invalid path.");
    PUSH_ERROR_AND_RETURN("Invalid path");
  }

  if (path.is_relative_path()) {
    PUSH_ERROR_AND_RETURN(fmt::format("TODO: Relative path: {}", path.full_path_name()));
  }

  if (!path.is_absolute_path()) {
    PUSH_ERROR_AND_RETURN(fmt::format("Path is not absolute path: {}", path.full_path_name()));
  }

  // Cache read/write is guarded so this `const` method is safe under
  // concurrent readers (the PrimSpec tree itself is immutable during the
  // lookup, so the tree walk below runs lock-free). Lock is a no-op build
  // when threads are disabled.
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::unique_lock<std::mutex> cache_lock(*_impl->_cache_mu);
#endif
  if (_impl->_dirty) {
    DCOUT("clear cache.");
    // Clear cache.
    _impl->_primspec_path_cache.clear();

    _impl->_dirty = false;
  } else {
    // First find from a cache.
    auto ret = _impl->_primspec_path_cache.find(path.full_path_name());
    if (ret != _impl->_primspec_path_cache.end()) {
      DCOUT("Found cache.");
      (*ps) = ret->second;
      return true;
    }
  }
#if defined(LIGHTUSD_ENABLE_THREAD)
  cache_lock.unlock();  // release during the (lock-free) tree walk
#endif

  // Direct path-based lookup (iterative, no string allocation)
  for (const auto &parent : _impl->_prim_specs) {
    if (auto pv = GetPrimSpecAtPathFromRoot(parent.second, path)) {
      (*ps) = pv.value();

      // Add to cache.
      // Assume pointer address does not change unless dirty state changes.
#if defined(LIGHTUSD_ENABLE_THREAD)
      cache_lock.lock();
#endif
      _impl->_primspec_path_cache[path.full_path_name()] = pv.value();
      return true;
    }
  }

  return false;

#undef PushError
}

bool Layer::check_unresolved_references(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasReferencesIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_unresolved_references = ret;
  return ret;
}

bool Layer::check_unresolved_payload(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasPayloadIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_unresolved_payload = ret;
  return ret;
}

bool Layer::check_unresolved_variant(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasVariantIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_unresolved_variant = ret;
  return ret;
}

bool Layer::check_unresolved_inherits(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasInheritsIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_unresolved_inherits = ret;
  return ret;
}

bool Layer::check_unresolved_specializes(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasSpecializesIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_unresolved_specializes = ret;
  return ret;
}

bool Layer::check_over_primspec(const uint32_t max_depth) const {
  bool ret = false;

  for (const auto &item : _impl->_prim_specs) {
    if (HasOverIterative(item.second, max_depth)) {
      ret = true;
      break;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(*_impl->_cache_mu);
#endif
  _impl->_has_over_primspec = ret;
  return ret;
}

size_t Layer::estimate_memory_usage() const {
  size_t total = sizeof(Layer);

  // Layer name
  total += _impl->_name.capacity();

  // PrimSpecs map
  total += _impl->_prim_specs.bucket_count() * sizeof(void*); // Hash table buckets
  for (const auto& prim_pair : _impl->_prim_specs) {
    total += prim_pair.first.capacity(); // key string
    total += EstimatePrimSpecMemory(prim_pair.second); // PrimSpec data
  }

  // LayerMetas
  total += sizeof(LayerMetas);
  // Add metadata strings
  // defaultPrim is a value::token (Token), not optional
  total += _impl->_metas.defaultPrim.str().capacity();

  // comment and doc are StringData structs with a value member
  total += _impl->_metas.comment.value.capacity();
  total += _impl->_metas.doc.value.capacity();

  // SubLayers
  for (const auto& sublayer : _impl->_metas.subLayers) {
    total += sizeof(SubLayer);
    // AssetPath contains strings
    total += sublayer.assetPath.GetAssetPath().capacity();
  }

  // CustomLayerData map
  for (const auto& custom_pair : _impl->_metas.customLayerData) {
    total += custom_pair.first.capacity(); // key string
    total += sizeof(MetaVariable); // Simplified estimate for MetaVariable
  }

  // PrimSpec path cache
  total += _impl->_primspec_path_cache.size() * (sizeof(std::string) + sizeof(const PrimSpec*));
  for (const auto& cache_pair : _impl->_primspec_path_cache) {
    total += cache_pair.first.capacity();
  }

  // Asset resolution state
  total += _impl->_current_working_path.capacity();
  for (const auto& path : _impl->_asset_search_paths) {
    total += path.capacity();
  }

  return total;
}

}  // namespace lightusd
