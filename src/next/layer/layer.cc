// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Layer Implementation

#include "layer.hh"
#include "../prim/identifier.hh"
#include "../../safe-arithmetic.hh"
#include <algorithm>
#include <unordered_set>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#include <vector>
#include "../execution.hh"
#endif

namespace lightusd {
namespace next {

void LayerMeta::FillAbsentStageMetaFrom(const LayerMeta& weaker) {
    if (!rootPrimOrder_set &&
        (weaker.rootPrimOrder_set || !weaker.rootPrimOrder.empty())) {
      rootPrimOrder = weaker.rootPrimOrder;
      rootPrimOrder_set = true;
    }
    if (!defaultPrim_set &&
        (weaker.defaultPrim_set || !weaker.defaultPrim.empty())) {
      defaultPrim = weaker.defaultPrim;
      defaultPrim_set = true;
    }
    if (!doc_set && weaker.doc_set) {
      doc = weaker.doc;
      doc_set = true;
    }
    if (!owner_set && weaker.owner_set) {
      owner = weaker.owner;
      owner_set = true;
    }
    if (!comment_set && weaker.comment_set) {
      comment = weaker.comment;
      comment_set = true;
    }
    if (!playbackMode_set && weaker.playbackMode_set) {
      playbackMode = weaker.playbackMode;
      playbackMode_set = true;
    }
    if (!colorConfiguration_set && weaker.colorConfiguration_set) {
      colorConfiguration = weaker.colorConfiguration;
      colorConfiguration_set = true;
    }
    if (!colorManagementSystem_set && weaker.colorManagementSystem_set) {
      colorManagementSystem = weaker.colorManagementSystem;
      colorManagementSystem_set = true;
    }
    if (!renderSettingsPrimPath_set && weaker.renderSettingsPrimPath_set) {
      renderSettingsPrimPath = weaker.renderSettingsPrimPath;
      renderSettingsPrimPath_set = true;
    }
    if (!upAxis_set && weaker.upAxis_set) {
      upAxis = weaker.upAxis;
      upAxis_set = true;
    }
    if (!metersPerUnit_set && weaker.metersPerUnit_set) {
      metersPerUnit = weaker.metersPerUnit;
      metersPerUnit_set = true;
    }
    if (!timeCodesPerSecond_set && weaker.timeCodesPerSecond_set) {
      timeCodesPerSecond = weaker.timeCodesPerSecond;
      timeCodesPerSecond_set = true;
    }
    if (!framesPerSecond_set && weaker.framesPerSecond_set) {
      framesPerSecond = weaker.framesPerSecond;
      framesPerSecond_set = true;
    }
    if (!kilogramsPerUnit_set && weaker.kilogramsPerUnit_set) {
      kilogramsPerUnit = weaker.kilogramsPerUnit;
      kilogramsPerUnit_set = true;
    }
    if (!startTimeCode_set && weaker.startTimeCode_set) {
      startTimeCode = weaker.startTimeCode;
      startTimeCode_set = true;
    }
    if (!endTimeCode_set && weaker.endTimeCode_set) {
      endTimeCode = weaker.endTimeCode;
      endTimeCode_set = true;
    }
    MergeWeakerRawFields(&unknownMeta, weaker.unknownMeta);
    MergeWeakerExtensionFields(&unknownFields, weaker.unknownFields);
  }

// ============================================================
// Layer
// ============================================================

Layer::Layer() = default;
Layer::~Layer() = default;

Layer::Layer(Layer&&) noexcept = default;
Layer& Layer::operator=(Layer&&) noexcept = default;

void Layer::reserve(size_t count) {
  prims_.reserve(count);
  root_indices_.reserve(count / 4);  // Estimate ~25% are roots
}

uint32_t Layer::add_prim(PrimSpec&& spec) {
  uint32_t index = static_cast<uint32_t>(prims_.size());
  prims_.push_back(std::move(spec));
  return index;
}

void Layer::set_parent(uint32_t child_index, uint32_t parent_index) {
  if (parent_index < prims_.size() && child_index < prims_.size()) {
    prims_[parent_index].add_child_index(child_index);
  }
}

void Layer::add_root(uint32_t index) {
  if (index < prims_.size()) {
    root_indices_.push_back(index);
  }
}

void Layer::add_root_pending(uint32_t fragment_id) {
  root_indices_.push_back(kPendingIndexBit | fragment_id);
}

bool Layer::splice_fragments(const std::vector<Layer*>& fragments,
                             const std::vector<size_t>& insert_before) {
  const size_t nf = fragments.size();
  if (insert_before.size() != nf) return false;
  const size_t nmain = prims_.size();
  size_t total = nmain;
  for (size_t f = 0; f < nf; ++f) {
    const Layer* frag = fragments[f];
    if (!frag || frag->prims_.empty() || frag->root_indices_.empty() ||
        insert_before[f] > nmain ||
        (f > 0 && insert_before[f] < insert_before[f - 1])) {
      return false;
    }
    for (uint32_t r : frag->root_indices_) {
      if (r >= frag->prims_.size()) return false;
    }
    total += frag->prims_.size();
  }
  if (total >= kPendingIndexBit) return false;

  // Final positions: fragment f occupies [frag_off[f], +size) right before
  // main prim insert_before[f]; the main prims keep their relative order.
  std::vector<uint32_t> main_map(nmain);
  std::vector<uint32_t> frag_off(nf);
  {
    size_t next = 0;
    size_t f = 0;
    for (size_t i = 0; i <= nmain; ++i) {
      while (f < nf && insert_before[f] == i) {
        frag_off[f] = static_cast<uint32_t>(next);
        next += fragments[f]->prims_.size();
        ++f;
      }
      if (i < nmain) main_map[i] = static_cast<uint32_t>(next++);
    }
  }
  // Remap one index list: main indices through main_map, each placeholder
  // expanded to its fragment's (rebased) roots.
  auto remap_list = [&](std::vector<uint32_t>* list) -> bool {
    bool has_pending = false;
    for (uint32_t& v : *list) {
      if (v & kPendingIndexBit) {
        has_pending = true;
        continue;
      }
      if (v >= nmain) return false;
      v = main_map[v];
    }
    if (!has_pending) return true;
    std::vector<uint32_t> out;
    out.reserve(list->size() + 4);
    for (uint32_t v : *list) {
      if (!(v & kPendingIndexBit)) {
        out.push_back(v);
        continue;
      }
      const uint32_t id = v & ~kPendingIndexBit;
      if (id >= nf) return false;
      for (uint32_t r : fragments[id]->root_indices_) {
        out.push_back(frag_off[id] + r);
      }
    }
    *list = std::move(out);
    return true;
  };

  // Validate/remap every index before moving anything.
  if (!remap_list(&root_indices_)) return false;
  for (PrimSpec& p : prims_) {
    if (!remap_list(&p.mutable_child_indices())) return false;
  }
  for (size_t f = 0; f < nf; ++f) {
    const size_t fsize = fragments[f]->prims_.size();
    for (PrimSpec& p : fragments[f]->prims_) {
      for (uint32_t& ci : p.mutable_child_indices()) {
        if (ci >= fsize) return false;  // (also rejects nested placeholders)
        ci += frag_off[f];
      }
    }
  }

  // One exact-size allocation, every PrimSpec moved once (a per-fragment
  // append with exact reserves reallocates the whole vector per fragment:
  // quadratic).
  std::vector<PrimSpec> out;
  out.reserve(total);
  size_t f = 0;
  for (size_t i = 0; i <= nmain; ++i) {
    while (f < nf && insert_before[f] == i) {
      for (PrimSpec& p : fragments[f]->prims_) out.push_back(std::move(p));
      fragments[f]->prims_.clear();
      fragments[f]->root_indices_.clear();
      ++f;
    }
    if (i < nmain) out.push_back(std::move(prims_[i]));
  }
  prims_ = std::move(out);
  path_to_index_.clear();
  return true;
}

void Layer::build_path_index() {
  path_to_index_.clear();
  path_to_index_.reserve(prims_.size());
  for (size_t i = 0; i < prims_.size(); ++i) {
    const std::string& path_str = prims_[i].path().str();
    if (!path_str.empty()) {
      path_to_index_[path_str] = static_cast<uint32_t>(i);
    }
  }
}

void Layer::sort_prims_by_path() {
  std::stable_sort(prims_.begin(), prims_.end(),
                   [](const PrimSpec& a, const PrimSpec& b) {
                     return a.path().str() < b.path().str();
                   });

  path_to_index_.clear();
  path_to_index_.reserve(prims_.size());
  for (size_t i = 0; i < prims_.size(); ++i) {
    prims_[i].clear_child_indices();
    const std::string& path_str = prims_[i].path().str();
    if (!path_str.empty()) {
      path_to_index_[path_str] = static_cast<uint32_t>(i);
    }
  }

  root_indices_.clear();
  for (size_t i = 0; i < prims_.size(); ++i) {
    const std::string& path_str = prims_[i].path().str();
    if (path_str.size() < 2 || path_str[0] != '/') continue;
    const size_t slash = path_str.rfind('/');
    if (slash == 0) {
      root_indices_.push_back(static_cast<uint32_t>(i));
      continue;
    }
    const std::string parent_path = path_str.substr(0, slash);
    auto parent_it = path_to_index_.find(parent_path);
    if (parent_it != path_to_index_.end()) {
      prims_[parent_it->second].add_child_index(static_cast<uint32_t>(i));
    }
  }
  apply_namespace_ordering();
}

void Layer::apply_namespace_ordering() {
  auto apply = [](std::vector<uint32_t>* indices,
                  const std::vector<std::string>& order,
                  const std::vector<PrimSpec>& prims) {
    if (!indices || order.empty() || indices->empty()) return;
    std::vector<uint32_t> result;
    std::unordered_set<uint32_t> in_result;
    result.reserve(indices->size());
    in_result.reserve(indices->size());
    for (const std::string& wanted : order) {
      for (uint32_t idx : *indices) {
        if (idx < prims.size() && prims[idx].name() == wanted &&
            !in_result.count(idx)) {
          result.push_back(idx);
          in_result.insert(idx);
          break;
        }
      }
    }
    for (uint32_t idx : *indices) {
      if (!in_result.count(idx)) {
        result.push_back(idx);
        in_result.insert(idx);
      }
    }
    *indices = std::move(result);
  };
  apply(&root_indices_, meta_.rootPrimOrder, prims_);
  for (PrimSpec& prim : prims_) {
    std::vector<uint32_t> children = prim.child_indices();
    // Namespace ordering is a read-only finalization pass.  Bind metadata as
    // const so overload resolution cannot select the mutable cold-field
    // accessor, which materializes a PrimSpecMetaExt for otherwise ordinary
    // prims.
    const PrimSpecMeta& meta = prim.meta();
    apply(&children, meta.primOrder(), prims_);
    prim.clear_child_indices();
    for (uint32_t child : children) prim.add_child_index(child);
  }
}

Layer Layer::Clone() const {
  Layer out;
  out.prims_.reserve(prims_.size());
  for (const PrimSpec& prim : prims_) {
    out.prims_.push_back(prim.Clone());
  }
  out.root_indices_ = root_indices_;
  out.meta_ = meta_;
  out.finalized_ = finalized_;
  out.build_path_index();
  return out;
}

uint32_t Layer::define_prim_at_path(const std::string& path,
                                    const std::string& type_name,
                                    PrimSpecifier specifier) {
  if (path.size() < 2 || path[0] != '/') return UINT32_MAX;
  // Validate the whole path before creating anything, so an invalid path
  // (empty component / trailing slash) does not leave partial ancestors.
  for (size_t i = 1; i < path.size(); ++i) {
    if (path[i] == '/' && (i + 1 == path.size() || path[i + 1] == '/')) {
      return UINT32_MAX;
    }
  }
  // Authoring boundary: every component must be a valid identifier
  // (strict UTF-8 + Unicode XID), so untrusted strings cannot author prims
  // the parser/validator would reject.
  {
    size_t comp = 1;
    while (comp < path.size()) {
      size_t next = path.find('/', comp);
      if (next == std::string::npos) next = path.size();
      if (!IsValidIdentifier(path.substr(comp, next - comp))) {
        return UINT32_MAX;
      }
      comp = next + 1;
    }
  }

  // The incremental updates below rely on the path index being current.
  if (path_to_index_.empty() && !prims_.empty()) {
    build_path_index();
  }

  uint32_t parent_index = UINT32_MAX;
  uint32_t index = UINT32_MAX;
  size_t pos = 1;
  std::string cur_path;
  cur_path.reserve(path.size());

  while (pos <= path.size()) {
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    if (next == pos) return UINT32_MAX;  // empty component ("//" or trailing '/')
    const std::string name = path.substr(pos, next - pos);
    cur_path += '/';
    cur_path += name;
    const bool is_leaf = (next == path.size());

    auto it = path_to_index_.find(cur_path);
    if (it != path_to_index_.end()) {
      index = it->second;
    } else {
      PrimSpec spec(name);  // typeless def for intermediate ancestors
      spec.set_path(Path(cur_path));
      index = add_prim(std::move(spec));  // may reallocate prims_
      path_to_index_[cur_path] = index;
      if (parent_index == UINT32_MAX) {
        add_root(index);
      } else {
        set_parent(index, parent_index);
      }
    }

    if (is_leaf) {
      PrimSpec* leaf = prim(index);
      if (!type_name.empty()) leaf->set_type_name(type_name);
      leaf->set_specifier(specifier);
      return index;
    }

    parent_index = index;
    pos = next + 1;
  }
  return UINT32_MAX;
}

bool Layer::remove_prim_at_path(const std::string& path) {
  if (path_to_index_.empty() && !prims_.empty()) {
    build_path_index();
  }
  auto it = path_to_index_.find(path);
  if (it == path_to_index_.end()) return false;
  const uint32_t index = it->second;

  // Unlink from parent (or the root list).
  size_t slash = path.find_last_of('/');
  if (slash == 0) {
    auto rit = std::find(root_indices_.begin(), root_indices_.end(), index);
    if (rit != root_indices_.end()) root_indices_.erase(rit);
  } else {
    const std::string parent_path = path.substr(0, slash);
    auto pit = path_to_index_.find(parent_path);
    if (pit != path_to_index_.end()) {
      if (PrimSpec* parent = prim(pit->second)) {
        parent->remove_child_index(index);
      }
    }
  }

  // Drop the whole subtree from the path index (iterative DFS; the specs stay
  // allocated but unreachable).
  std::vector<uint32_t> stack{index};
  while (!stack.empty()) {
    uint32_t cur = stack.back();
    stack.pop_back();
    const PrimSpec* p = prim(cur);
    if (!p) continue;
    path_to_index_.erase(p->path().str());
    for (uint32_t child : p->child_indices()) {
      stack.push_back(child);
    }
  }
  return true;
}

bool Layer::rename_prim_at_path(const std::string& path,
                                const std::string& new_name) {
  if (!IsValidIdentifier(new_name)) return false;
  if (path_to_index_.empty() && !prims_.empty()) {
    build_path_index();
  }
  auto it = path_to_index_.find(path);
  if (it == path_to_index_.end()) return false;
  const uint32_t index = it->second;
  const size_t slash = path.find_last_of('/');
  const std::string parent_path = path.substr(0, slash);
  const std::string new_path =
      (slash == 0 ? std::string("/") : parent_path + "/") + new_name;
  if (new_path == path) return true;
  if (path_to_index_.count(new_path)) return false;  // sibling collision

  // Rewrite the renamed prim and every descendant: their paths all share the
  // old prefix, which is replaced by the new one.
  std::vector<uint32_t> stack{index};
  while (!stack.empty()) {
    const uint32_t cur = stack.back();
    stack.pop_back();
    PrimSpec* p = prim(cur);
    if (!p) continue;
    const std::string old_str = p->path().str();
    const std::string new_str = new_path + old_str.substr(path.size());
    path_to_index_.erase(old_str);
    p->set_path(Path(new_str));
    path_to_index_[new_str] = cur;
    for (uint32_t child : p->child_indices()) stack.push_back(child);
  }
  prim(index)->set_name(new_name);
  return true;
}

void Layer::finalize() {
  if (finalized_) return;

  // Build path-to-index map
  build_path_index();

  // Finalize each prim's properties (sort for binary search). Each prim's
  // sort orders only its own slots, so large layers (a composed stage) sort in
  // parallel with identical results. The 64k-prim threshold keeps ordinary
  // layers -- including per-worker layers finalized during a parallel scene
  // load -- on the serial path, avoiding nested thread fan-out.
#if defined(LIGHTUSD_ENABLE_THREAD)
  const unsigned hw = std::thread::hardware_concurrency();
  const size_t workers =
      std::min<size_t>(hw ? hw : 1, static_cast<size_t>(kMaxExecutionThreads));
  if (prims_.size() >= 65536 && workers > 1) {
    std::vector<std::thread> pool;
    pool.reserve(workers);
    const size_t n = prims_.size();
    for (size_t t = 0; t < workers; ++t) {
      const size_t lo = n * t / workers;
      const size_t hi = n * (t + 1) / workers;
      pool.emplace_back([this, lo, hi]() {
        for (size_t i = lo; i < hi; ++i) prims_[i].finalize_properties();
      });
    }
    for (std::thread& th : pool) th.join();
  } else
#endif
  {
    for (auto& prim : prims_) {
      prim.finalize_properties();
    }
  }

  apply_namespace_ordering();

  finalized_ = true;
}

const PrimSpec* Layer::prim(uint32_t index) const {
  if (index >= prims_.size()) return nullptr;
  return &prims_[index];
}

PrimSpec* Layer::prim(uint32_t index) {
  if (index >= prims_.size()) return nullptr;
  return &prims_[index];
}

const PrimSpec* Layer::prim_at_path(const Path& path) const {
  return prim_at_path(path.str());
}

const PrimSpec* Layer::prim_at_path(const std::string& path) const {
  auto it = path_to_index_.find(path);
  if (it == path_to_index_.end()) return nullptr;
  return prim(it->second);
}

PrimSpec* Layer::prim_at_path_mutable(const std::string& path) {
  auto it = path_to_index_.find(path);
  if (it == path_to_index_.end()) return nullptr;
  return prim(it->second);
}

uint32_t Layer::index_at_path(const std::string& path) const {
  auto it = path_to_index_.find(path);
  return it == path_to_index_.end() ? UINT32_MAX : it->second;
}

PrimSpec* Layer::prim_mutable(uint32_t index) {
  if (index >= prims_.size()) return nullptr;
  return &prims_[index];
}

std::vector<const PrimSpec*> Layer::children(uint32_t prim_index) const {
  std::vector<const PrimSpec*> result;
  const PrimSpec* p = prim(prim_index);
  if (!p) return result;

  const auto& indices = p->child_indices();
  result.reserve(indices.size());
  for (uint32_t idx : indices) {
    if (const PrimSpec* child = prim(idx)) {
      result.push_back(child);
    }
  }
  return result;
}

size_t Layer::memory_usage() const {
  size_t size = 0;
  auto add = [&size](size_t bytes) {
    size = safe::saturating_add(size, bytes);
  };
  const auto add_strings = [&add](const std::vector<std::string>& strings) {
    add(safe::saturating_mul(strings.capacity(), sizeof(std::string)));
    for (const std::string& text : strings) add(text.capacity());
  };
  const auto add_value = [&add](const Value& value) {
    add(value.dynamic_string_memory_usage());
    if (value.is_array() && !value.is_lazy()) {
      add(safe::saturating_mul(value.array_size(),
                               GetTypeSize(value.type_id())));
    }
    if (!value.as_dictionary()) return;
    std::vector<const Value*> pending{&value};
    while (!pending.empty()) {
      const Value* current = pending.back();
      pending.pop_back();
      if (current != &value && current->is_array() && !current->is_lazy()) {
        add(safe::saturating_mul(current->array_size(),
                                 GetTypeSize(current->type_id())));
      }
      if (const Dict* dict = current->as_dictionary()) {
        for (const auto& entry : dict->entries()) pending.push_back(&entry.second);
      }
    }
  };
  std::vector<const Layer*> pending_layers{this};
  std::unordered_set<const Layer*> visited_layers;
  while (!pending_layers.empty()) {
    const Layer* layer = pending_layers.back();
    pending_layers.pop_back();
    if (!layer || !visited_layers.insert(layer).second) continue;
    add(sizeof(Layer));

    // Each shared variant content Layer is charged once, even if multiple
    // options reference it or an authored graph points back to an ancestor.
    add(safe::saturating_mul(layer->prims_.capacity(), sizeof(PrimSpec)));
    for (const PrimSpec& prim : layer->prims_) {
      const size_t prim_bytes = prim.memory_usage();
      add(prim_bytes >= sizeof(PrimSpec) ? prim_bytes - sizeof(PrimSpec) : 0);
      std::vector<const VariantSetData*> pending_sets;
      for (const VariantSetData& set : prim.meta().variantSets())
        pending_sets.push_back(&set);
      while (!pending_sets.empty()) {
        const VariantSetData* set = pending_sets.back();
        pending_sets.pop_back();
        for (const VariantData& option : set->variants) {
          if (option.content) pending_layers.push_back(option.content.get());
          for (const VariantSetData& nested : option.variantSets)
            pending_sets.push_back(&nested);
        }
      }
    }

    add(safe::saturating_mul(layer->root_indices_.capacity(), sizeof(uint32_t)));
    for (const auto& kv : layer->path_to_index_) {
      add(kv.first.capacity());
      add(sizeof(uint32_t) + sizeof(void*) * 2);  // Rough hash map overhead
    }

    // Metadata. JSON import/export budgets must include retained dictionaries,
    // numeric arrays, ordering, and decodable extension payloads.
    const LayerMeta& meta = layer->meta_;
    for (const std::string* text : {&meta.defaultPrim, &meta.upAxis,
         &meta.doc, &meta.comment, &meta.owner, &meta.playbackMode,
         &meta.colorConfiguration, &meta.colorManagementSystem,
         &meta.renderSettingsPrimPath}) add(text->capacity());
    add_strings(meta.rootPrimOrder);
    add_strings(meta.subLayers);
    add(safe::saturating_mul(meta.subLayerOffsets.capacity(),
                             sizeof(std::pair<double, double>)));
    add(safe::saturating_mul(meta.relocates.capacity(),
                             sizeof(std::pair<std::string, std::string>)));
    for (const auto& relocate : meta.relocates) {
      add(relocate.first.capacity());
      add(relocate.second.capacity());
    }
    add_value(meta.customLayerData);
    add_value(meta.expressionVariables);
    add(safe::saturating_mul(meta.unknownMeta.capacity(),
                             sizeof(std::pair<std::string, std::string>)));
    for (const auto& entry : meta.unknownMeta) {
      add(entry.first.capacity());
      add(entry.second.capacity());
    }
    add(safe::saturating_mul(meta.unknownFields.capacity(),
                             sizeof(TypedExtensionField)));
    for (const TypedExtensionField& field : meta.unknownFields) {
      add(field.name.capacity());
      add(field.unregistered_source.capacity());
      add_value(field.value);
    }
  }

  return size;
}

Layer::Stats Layer::stats() const {
  Stats s{};
  s.prim_count = prims_.size();
  s.root_count = root_indices_.size();

  for (const auto& prim : prims_) {
    s.total_properties += prim.properties().size();
    // Count time samples (simplified)
  }

  s.memory_bytes = memory_usage();
  return s;
}

// ============================================================
// LayerBuilder
// ============================================================

LayerBuilder::LayerBuilder(Layer& layer)
    : layer_(layer) {}

uint32_t LayerBuilder::FindExistingPrim(const std::string& path_str) {
  // Rebuild when the layer was mutated outside the builder (or on first use),
  // so the map can never disagree with the layer.
  if (!path_index_built_ || layer_.prim_count() != indexed_prim_count_) {
    path_index_.clear();
    path_index_.reserve(layer_.prim_count());
    for (size_t i = 0; i < layer_.prim_count(); ++i) {
      if (const PrimSpec* p = layer_.prim(static_cast<uint32_t>(i))) {
        // emplace, not operator[]: on a duplicate path keep the FIRST, which
        // is what the linear scan this replaces returned.
        path_index_.emplace(p->path().str(), static_cast<uint32_t>(i));
      }
    }
    indexed_prim_count_ = layer_.prim_count();
    path_index_built_ = true;
  }
  auto it = path_index_.find(path_str);
  return it == path_index_.end() ? UINT32_MAX : it->second;
}

uint32_t LayerBuilder::begin_prim(const std::string& name, const std::string& type_name,
                                   PrimSpecifier specifier) {
  PrimSpec spec(name, type_name);
  spec.set_specifier(specifier);

  // Build path based on parent stack
  std::string path_str;
  if (prim_stack_.empty()) {
    path_str = path_prefix_ + "/" + name;  // prefix empty for a real root
  } else {
    // Get parent path
    const PrimSpec* parent = layer_.prim(prim_stack_.back());
    if (parent) {
      path_str = parent->path().str() + "/" + name;
    } else {
      path_str = "/" + name;
    }
  }
  spec.set_path(Path(path_str));

  // Duplicate sibling name: re-open the existing prim instead of appending a
  // second spec with the same path. Two same-path specs corrupt a subsequent
  // crate write (pxr: "invalid specs: spec repeated") — pxr errors on the
  // duplicate at parse time; merging keeps the file loadable while staying
  // single-spec-per-path.
  const uint32_t existing = FindExistingPrim(path_str);
  if (existing != UINT32_MAX) {
    current_index_ = existing;
    if (!type_name.empty() && layer_.prim(existing)->type_name().empty()) {
      layer_.prim_mutable(existing)->set_type_name(type_name);
    }
    prim_stack_.push_back(current_index_);
    return current_index_;
  }

  current_index_ = layer_.add_prim(std::move(spec));

  // Set up parent-child relationship
  if (!prim_stack_.empty()) {
    layer_.set_parent(current_index_, prim_stack_.back());
  } else {
    layer_.add_root(current_index_);
  }

  // Keep the lookup map in step with the layer.
  path_index_.emplace(path_str, current_index_);
  indexed_prim_count_ = layer_.prim_count();

  // Push to stack
  prim_stack_.push_back(current_index_);

  return current_index_;
}

uint32_t LayerBuilder::define_prim(const std::string& name,
                                   const std::string& type_name,
                                   PrimSpecifier specifier, std::string* err) {
  if (!IsValidIdentifier(name)) {
    if (err) *err = "Not a valid prim name: " + name;
    return UINT32_MAX;
  }
  return begin_prim(name, type_name, specifier);
}

void LayerBuilder::end_prim() {
  if (!prim_stack_.empty()) {
    prim_stack_.pop_back();
  }
  current_index_ = prim_stack_.empty() ? UINT32_MAX : prim_stack_.back();
}

PrimSpec* LayerBuilder::current() {
  if (current_index_ == UINT32_MAX) return nullptr;
  return layer_.prim(current_index_);
}

void LayerBuilder::add_property(const std::string& name, Value value, uint16_t flags) {
  if (PrimSpec* p = current()) {
    p->add_property(name, std::move(value), flags);
  }
}

void LayerBuilder::add_time_sample(const std::string& prop_name, double time,
                                   Value value, bool dedup) {
  // Authoring boundary (see PrimSpec::add_property): keep the name a valid
  // (possibly namespaced) identifier so API scenes stay round-trippable.
  if (!IsValidNamespacedIdentifier(prop_name)) return;
  if (PrimSpec* p = current()) {
    PropNameId name_id = GetPropNameTable().intern(prop_name);

    // First, add the time sample (stores the value and records time)
    TypeId type_id = value.type_id();
    bool is_array = value.is_array();
    p->add_time_sample(name_id, time, std::move(value), dedup);

    // Ensure property slot exists with the time-sampled flag set.
    const PropSlot* existing = p->property(name_id);
    if (!existing) {
      // Create property slot for this time-sampled property
      uint16_t flags = PropSlot::kFlagTimeSampled;
      if (is_array) {
        flags |= PropSlot::kFlagArray;
      }
      // Add slot with type info but no default value
      p->add_property_slot(name_id, type_id, flags);
    } else {
      // The attribute was already declared (e.g. a `<type> <name> ( meta )` line
      // before its `.timeSamples`): OR the flag onto the existing slot, else the
      // writer never emits the samples and they are silently lost on roundtrip.
      p->mark_property_time_sampled(name_id);
    }
  }
}

void LayerBuilder::add_relationship(const std::string& name, const Path& target) {
  if (PrimSpec* p = current()) {
    p->add_relationship(name, target);
  }
}

void LayerBuilder::add_connection(const std::string& name, const Path& target) {
  if (PrimSpec* p = current()) {
    const PropNameId name_id = GetPropNameTable().intern(name);
    if (!p->property(name_id)) {
      // A connection-only attribute still needs a property slot. Without it,
      // composition and crate serialization never visit the connection map.
      // Callers with a more specific type may declare the slot first.
      p->add_property_slot(name_id, TypeId::Token,
                           PropSlot::kFlagConnection);
    }
    p->add_connection(name, target);
  }
}

void LayerBuilder::set_active(bool active) {
  if (PrimSpec* p = current()) {
    p->meta().active = active;
  }
}

void LayerBuilder::set_hidden(bool hidden) {
  if (PrimSpec* p = current()) {
    p->meta().hidden = hidden;
  }
}

void LayerBuilder::finalize() {
  layer_.finalize();
}

}  // namespace next
}  // namespace lightusd
