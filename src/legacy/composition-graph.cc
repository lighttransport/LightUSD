// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// composition-graph.cc - DAG-based USD composition engine implementation
//

#include "composition-graph.hh"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <sstream>

#include "common-macros.inc"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "hash-util.hh"
#include "layer.hh"
#include "namespace-mapping.hh"
#include "core/security-policy.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"

// These are needed for BuildStage (PrimSpec -> Prim reconstruction)
#include "stage.hh"

// VariantSelectPrimSpec (variant content resolution).
#include "composition.hh"

namespace lightusd {
namespace composition_graph {

using security_policy::ValidateAndNormalizeAssetPath;

namespace {

bool IsFileDescriptorLimitError(const std::string &err) {
  return err.find("file descriptor limit reached") != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// ArcType utilities
// ---------------------------------------------------------------------------

const char *ArcTypeName(ArcType t) {
  switch (t) {
    case ArcType::Root: return "Root";
    case ArcType::SubLayer: return "SubLayer";
    case ArcType::Inherit: return "Inherit";
    case ArcType::Variant: return "Variant";
    case ArcType::Relocate: return "Relocate";
    case ArcType::Reference: return "Reference";
    case ArcType::Payload: return "Payload";
    case ArcType::Specialize: return "Specialize";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// PrimIndex methods
// ---------------------------------------------------------------------------

bool PrimIndex::HasAnySpecs() const {
  for (const auto &node : _nodes) {
    if (node.has_specs() && !node.is_culled()) return true;
  }
  return false;
}

bool PrimIndex::IsPayloadLoaded() const {
  for (const auto &node : _nodes) {
    if (node.is_payload_deferred()) return false;
  }
  return true;
}

std::string PrimIndex::DumpToString() const {
  std::ostringstream ss;
  ss << "PrimIndex: " << _prim_path.prim_part() << "\n";
  ss << "  Nodes (" << _nodes.size() << "):\n";
  for (size_t i = 0; i < _nodes.size(); i++) {
    const auto &n = _nodes[i];
    ss << "    [" << i << "] " << ArcTypeName(n.arc_type)
       << " parent=" << n.parent << " depth=" << static_cast<int>(n.depth)
       << " strength=" << n.strength_order;
    if (_path_table && n.site_path_idx < _path_table->size()) {
      ss << " site=" << (*_path_table)[n.site_path_idx];
    }
    if (n.has_specs()) ss << " HAS_SPECS";
    if (n.is_culled()) ss << " CULLED";
    if (n.is_inert()) ss << " INERT";
    if (n.is_payload_deferred()) ss << " PAYLOAD_DEFERRED";
    if (n.is_implied_arc()) ss << " IMPLIED";
    ss << "\n";
  }
  ss << "  Strength order: [";
  for (size_t i = 0; i < _strength_order.size(); i++) {
    if (i > 0) ss << ", ";
    ss << _strength_order[i];
  }
  ss << "]\n";
  return ss.str();
}

// ---------------------------------------------------------------------------
// InstanceKey computation
// ---------------------------------------------------------------------------

InstanceKey ComputeInstanceKey(const PrimIndex &index,
                               const std::vector<LayerStackEntry> &stacks,
                               const std::vector<std::string> &path_table) {
  InstanceKey key;
  if (!index.IsInstanceable()) return key;

  SpookyHash hasher;
  hasher.Init(0xDEADBEEF, 0xCAFEBABE);

  // Hash all non-root nodes' structural identity
  for (uint16_t i = 1; i < index.GetNodeCount(); i++) {
    const auto &node = index.GetNode(i);
    if (node.is_culled()) continue;

    // Arc type
    uint8_t at = static_cast<uint8_t>(node.arc_type);
    hasher.Update(&at, sizeof(at));

    // Layer identifier
    if (node.layer_stack_idx != CompNode::kInvalidIndex &&
        node.layer_stack_idx < stacks.size()) {
      const auto &id = stacks[node.layer_stack_idx].identifier;
      hasher.Update(id.data(), id.size());

      // Layer offset
      const auto &off = stacks[node.layer_stack_idx].offset;
      hasher.Update(&off._offset, sizeof(off._offset));
      hasher.Update(&off._scale, sizeof(off._scale));
    }

    // Site path
    if (node.site_path_idx < path_table.size()) {
      const auto &sp = path_table[node.site_path_idx];
      hasher.Update(sp.data(), sp.size());
    }

    // Sibling number (ordering matters)
    hasher.Update(&node.sibling_num, sizeof(node.sibling_num));
  }

  hasher.Final(&key.hash_lo, &key.hash_hi);
  return key;
}

// ---------------------------------------------------------------------------
// PrimIndex incremental-mutation helpers (friends of PrimIndex)
// ---------------------------------------------------------------------------

CompNode &GetMutableNode(PrimIndex &index LIGHTUSD_LIFETIMEBOUND,
                         uint16_t node_idx) {
  // node_idx is typically from GetStrengthOrder() or DeferredPayloadInfo.
  // The latter can be stale if the deferred-payload index is corrupted,
  // so guard against out-of-bounds access (defense-in-depth).
  if (node_idx >= index._nodes.size()) {
    static thread_local CompNode s_sentinel{};
    return s_sentinel;
  }
  return index._nodes[node_idx];
}

void RecomputeStrengthOrder(PrimIndex &index) {
  index._strength_order.clear();
  for (uint16_t i = 0; i < index.GetNodeCount(); i++) {
    if (!index._nodes[i].is_culled()) {
      index._strength_order.push_back(i);
    }
  }
  std::sort(index._strength_order.begin(), index._strength_order.end(),
            [&index](uint16_t a, uint16_t b) {
              return index._nodes[a].strength_order <
                     index._nodes[b].strength_order;
            });
}

// ---------------------------------------------------------------------------
// CompositionContext -- shared table management
// ---------------------------------------------------------------------------

uint32_t CompositionContext::InternPath(const std::string &path_str) {
  auto it = _path_intern_map.find(path_str);
  if (it != _path_intern_map.end()) return it->second;

  uint32_t idx = static_cast<uint32_t>(_path_table.size());
  _path_table.push_back(path_str);
  _path_intern_map[path_str] = idx;
  return idx;
}

uint16_t CompositionContext::AddLayerStack(const Layer *layer,
                                           const std::string &id,
                                           const LayerOffset &offset) {
  // Check if we already have this layer
  for (size_t i = 0; i < _layer_stacks.size(); i++) {
    if (_layer_stacks[i].layer == layer &&
        _layer_stacks[i].identifier == id) {
      return static_cast<uint16_t>(i);
    }
  }

  if (_layer_stacks.size() >= static_cast<size_t>((std::numeric_limits<uint16_t>::max)())) {
    return CompNode::kInvalidIndex;
  }
  uint16_t idx = static_cast<uint16_t>(_layer_stacks.size());
  LayerStackEntry entry;
  entry.layer = layer;
  entry.identifier = id;
  entry.offset = offset;
  _layer_stacks.push_back(std::move(entry));
  return idx;
}

uint16_t CompositionContext::AddMapExpression(const NamespaceMapping &mapping,
                                              int32_t parent_expr) {
  if (mapping.empty() && parent_expr < 0) {
    return CompNode::kInvalidIndex;  // identity mapping
  }

  if (_map_expressions.size() >= static_cast<size_t>((std::numeric_limits<uint16_t>::max)())) {
    return CompNode::kInvalidIndex;
  }
  uint16_t idx = static_cast<uint16_t>(_map_expressions.size());
  MapExpr expr;
  expr.mapping = mapping;
  expr.parent_expr = parent_expr;
  _map_expressions.push_back(std::move(expr));
  return idx;
}

// ---------------------------------------------------------------------------
// PrimIndexBuilder
// ---------------------------------------------------------------------------

PrimIndexBuilder::PrimIndexBuilder(CompositionContext *ctx,
                                   const Path &prim_path,
                                   const PrimSpec &root_primspec,
                                   uint16_t root_layer_stack_idx)
    : _ctx(ctx),
      _root_primspec(&root_primspec),
      _root_layer_stack_idx(root_layer_stack_idx) {
  _result._prim_path = prim_path;
  _result._path_table = &ctx->_path_table;
  _result._layer_stacks = &ctx->_layer_stacks;
  _result._map_expressions = &ctx->_map_expressions;
}

PrimIndexBuilder::PrimIndexBuilder(CompositionContext *ctx,
                                   const Path &prim_path)
    : _ctx(ctx), _root_primspec(nullptr), _root_layer_stack_idx(0) {
  _result._prim_path = prim_path;
  _result._path_table = &ctx->_path_table;
  _result._layer_stacks = &ctx->_layer_stacks;
  _result._map_expressions = &ctx->_map_expressions;
}

void PrimIndexBuilder::SeedDescendedNodes(const PrimIndex &parent,
                                          const std::string &child_name) {
  // Mirror the parent's node tree one namespace level down: for each parent
  // node that has a child named `child_name`, create a descended node with the
  // same arc type / layer stack but the child's site path. Preserving the
  // parent linkage keeps arc-type/depth/sibling ordering, so the descended
  // index has the same strength order as the parent — i.e. the child inherits
  // the parent's reference/payload/inherit arcs. Each descended node's own arcs
  // are then scanned (and followed), which is what pulls nested references such
  // as a referenced child that itself references another file.
  std::unordered_map<uint16_t, uint16_t> parent_to_child;
  parent_to_child.reserve(parent.GetNodeCount());

  for (uint16_t i = 0; i < parent.GetNodeCount(); i++) {
    const CompNode &pn = parent.GetNode(i);
    // Skip nodes that contribute nothing or are not yet realized.
    if (pn.is_culled() || pn.is_payload_deferred()) continue;
    if (pn.layer_stack_idx == CompNode::kInvalidIndex ||
        pn.layer_stack_idx >= _ctx->_layer_stacks.size()) {
      continue;
    }
    const LayerStackEntry &ls = _ctx->_layer_stacks[pn.layer_stack_idx];
    if (!ls.layer) continue;

    const std::string &pn_site = _ctx->_path_table[pn.site_path_idx];
    const std::string child_site = pn_site + "/" + child_name;

    const PrimSpec *cps = nullptr;
    std::string find_err;
    if (!ls.layer->find_primspec_at(Path(child_site, ""), &cps, &find_err) ||
        !cps) {
      continue;  // this branch of the parent tree has no such child
    }

    uint16_t cparent = CompNode::kInvalidIndex;
    if (pn.parent != CompNode::kInvalidIndex) {
      auto it = parent_to_child.find(pn.parent);
      if (it != parent_to_child.end()) cparent = it->second;
    }

    uint16_t new_idx =
        AddNode(pn.arc_type, cparent, pn.layer_stack_idx, pn.layer_idx,
                _ctx->InternPath(child_site), pn.map_expr_idx);
    if (new_idx == CompNode::kInvalidIndex) continue;

    _result._nodes[new_idx].sibling_num = pn.sibling_num;
    if (cparent != CompNode::kInvalidIndex) AppendChild(cparent, new_idx);

    if (!cps->props().empty() || cps->metas().authored() ||
        !cps->children().empty()) {
      _result._nodes[new_idx].flags =
          _result._nodes[new_idx].flags | NodeFlags::HasSpecs;
    }

    parent_to_child[i] = new_idx;

    CollectVariantOpinions(new_idx);
    ScanArcsAndEnqueueTasks(new_idx, *cps);
  }
}

uint16_t PrimIndexBuilder::AddNode(ArcType arc_type, uint16_t parent_idx,
                                   uint16_t layer_stack_idx,
                                   uint16_t layer_idx,
                                   uint32_t site_path_idx,
                                   uint16_t map_expr_idx) {
  if (_result._nodes.size() >= CompNode::kInvalidIndex) {
    return CompNode::kInvalidIndex;  // per-index overflow
  }
  // Global arc-node budget: bounds total composition work (and the synthesized
  // variant Layer copies) across the whole graph so a cyclic scene cannot grow
  // memory without limit.
  if (_ctx->_max_arc_nodes > 0 &&
      _ctx->_arc_node_count >= _ctx->_max_arc_nodes) {
    return CompNode::kInvalidIndex;
  }
  ++_ctx->_arc_node_count;

  uint16_t idx = static_cast<uint16_t>(_result._nodes.size());
  CompNode node;
  node.parent = parent_idx;
  node.arc_type = arc_type;
  node.depth = (parent_idx != CompNode::kInvalidIndex)
                   ? static_cast<uint8_t>(
                         std::min<uint16_t>(_result._nodes[parent_idx].depth + 1, 255))
                   : 0;
  node.flags = NodeFlags::None;
  node.layer_stack_idx = layer_stack_idx;
  node.layer_idx = layer_idx;
  node.map_expr_idx = map_expr_idx;
  node.site_path_idx = site_path_idx;
  _result._nodes.push_back(node);
  return idx;
}

void PrimIndexBuilder::AppendChild(uint16_t parent_idx, uint16_t child_idx) {
  if (parent_idx == CompNode::kInvalidIndex) return;

  CompNode &parent = _result._nodes[parent_idx];
  if (!parent.has_children()) {
    parent.first_child = child_idx;
  } else {
    // Walk to the last sibling
    uint16_t cur = parent.first_child;
    while (_result._nodes[cur].has_next_sibling()) {
      cur = _result._nodes[cur].next_sibling;
    }
    _result._nodes[cur].next_sibling = child_idx;
  }
  _result._nodes[child_idx].parent = parent_idx;
}

bool PrimIndexBuilder::WouldCreateCycle(const std::string &layer_id,
                                        const std::string &prim_path) const {
  return _arc_stack.count({layer_id, prim_path}) > 0;
}

const PrimSpec *PrimIndexBuilder::GetPrimSpecForNode(uint16_t node_idx) const {
  const CompNode &node = _result._nodes[node_idx];

  if (node.layer_stack_idx == CompNode::kInvalidIndex) return nullptr;
  if (node.layer_stack_idx >= _ctx->_layer_stacks.size()) return nullptr;

  const LayerStackEntry &ls = _ctx->_layer_stacks[node.layer_stack_idx];
  if (!ls.layer) return nullptr;

  const std::string &site_path =
      _ctx->_path_table[node.site_path_idx];

  const PrimSpec *ps = nullptr;
  std::string find_err;
  Path p(site_path, "");
  if (!ls.layer->find_primspec_at(p, &ps, &find_err)) {
    return nullptr;
  }
  return ps;
}

// Default layer loader: resolve + parse the asset fresh and own it in
// ctx->_loaded_layers. This is the legacy CompositionGraph behavior, used when
// no load_layer_fn seam is installed. Returns a borrowed pointer (nullptr on
// failure). Honors the per-PrimSpec current-working-path for resolution.
static const Layer *DefaultLoadAndOwnLayer(CompositionContext *ctx,
                                           const std::string &asset_path,
                                           const std::string &cwp,
                                           std::string *warn,
                                           std::string *err) {
  if (!ctx->_resolver) return nullptr;

  // Resolve-and-parse cache: a big composed tree can request the same asset
  // (or the same missing asset) from many prims; without this each request
  // repeats an expensive filesystem resolve (+ parse) — O(prims) work.
  const std::string cache_key = asset_path + "\n" + cwp;
  {
    auto it = ctx->_default_layer_cache.find(cache_key);
    if (it != ctx->_default_layer_cache.end()) {
      return it.mapped();  // cached hit (Layer*) or miss (nullptr)
    }
  }

  std::string old_cwp = ctx->_resolver->current_working_path();
  if (!cwp.empty()) {
    ctx->_resolver->set_current_working_path(cwp);
  }

  const Layer *result = nullptr;
  std::string resolved_path = ctx->_resolver->resolve(asset_path);
  if (!resolved_path.empty()) {
    Asset asset;
    if (ctx->_resolver->open_asset(resolved_path, asset_path, &asset, warn,
                                   err)) {
      if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
        if (err) {
          *err = fmt::format("Resolved asset exceeds max bytes ({} > {}).",
                             asset.size(),
                             security_policy::GetMaxAssetReadBytes());
        }
      } else {
        Layer layer;
        // Pass the RESOLVED (anchored) path, not the authored asset_path, so the
        // loaded layer's prims are stamped with the resolved directory as their
        // current-working-path. This preserves the parent-directory context for
        // nested relative references/payloads (cross-directory anchoring).
        if (LoadLayerFromMemory(asset.data(), asset.size(), resolved_path, &layer,
                                warn, err)) {
          auto layer_ptr = std::make_unique<Layer>(std::move(layer));
          ctx->_loaded_layers.push_back(std::move(layer_ptr));
          result = ctx->_loaded_layers.back().get();
        }
      }
    }
  }

  if (!old_cwp.empty()) {
    ctx->_resolver->set_current_working_path(old_cwp);
  }
  ctx->_default_layer_cache.insert({cache_key, result});  // cache hit OR miss
  return result;
}

const Layer *PrimIndexBuilder::LoadArcLayer(const std::string &asset_path,
                                            const std::string &cwp,
                                            std::string *warn,
                                            std::string *err) {
  if (_ctx->load_layer_fn) {
    return _ctx->load_layer_fn(_ctx->load_layer_userdata, asset_path, cwp, warn,
                               err);
  }
  return DefaultLoadAndOwnLayer(_ctx, asset_path, cwp, warn, err);
}

void PrimIndexBuilder::ScanArcsAndEnqueueTasks(uint16_t node_idx,
                                               const PrimSpec &ps) {
  const PrimMetas &metas = ps.metas();

  // Inherits
  if (metas.inherits.has_value() && !metas.inherits->empty()) {
    _task_queue.push(
        {TaskType::EvalInherits, node_idx, 0, 0});
  }

  // Variants
  if ((metas.variantSets.has_value() && !metas.variantSets->empty()) ||
      (metas.variants.has_value())) {
    _task_queue.push(
        {TaskType::EvalVariants, node_idx, 0, 0});
  }

  // References
  if (metas.references.has_value() && !metas.references->empty()) {
    _task_queue.push(
        {TaskType::EvalReferences, node_idx, 0, 0});
  }

  // Payloads
  if (metas.payload.has_value() && !metas.payload->empty()) {
    _task_queue.push(
        {TaskType::EvalPayloads, node_idx, 0, 0});
  }

  // Specializes
  if (metas.specializes.has_value() && !metas.specializes->empty()) {
    _task_queue.push(
        {TaskType::EvalSpecializes, node_idx, 0, 0});
  }
}

// ---------------------------------------------------------------------------
// PrimIndexBuilder::Build -- main entry point
// ---------------------------------------------------------------------------

nonstd::expected<PrimIndex, std::string> PrimIndexBuilder::Build() {
  std::string err;

  // Phase 1: Create root node
  if (!EvalRootNode(&err)) {
    return nonstd::make_unexpected(err);
  }

  return FinishBuild();
}

nonstd::expected<PrimIndex, std::string>
PrimIndexBuilder::BuildChildFrom(const PrimIndex &parent,
                                 const std::string &child_name) {
  // Phase 1 (child): seed nodes descended from the parent's nodes.
  SeedDescendedNodes(parent, child_name);
  if (_result._nodes.empty()) {
    return nonstd::make_unexpected("child prim has no contributing nodes");
  }
  return FinishBuild();
}

bool PrimIndexBuilder::ReprocessNode(PrimIndex *index, uint16_t node_idx,
                                     std::string *err) {
  if (!index) {
    if (err) *err = "PrimIndexBuilder::ReprocessNode: index is null";
    return false;
  }
  if (node_idx >= index->GetNodeCount()) {
    if (err) *err = "PrimIndexBuilder::ReprocessNode: node index out of range";
    return false;
  }

  _result = std::move(*index);
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) {
    if (err) *err = "PrimIndexBuilder::ReprocessNode: node has no PrimSpec";
    *index = std::move(_result);
    return false;
  }

  CollectVariantOpinions(node_idx);
  ScanArcsAndEnqueueTasks(node_idx, *ps);
  auto rebuilt = FinishBuild();
  if (!rebuilt) {
    if (err) *err = rebuilt.error();
    *index = std::move(_result);
    return false;
  }

  *index = std::move(*rebuilt);
  return true;
}

bool PrimIndexBuilder::DrainTaskQueue(std::string *err) {
  while (!_task_queue.empty()) {
    CompositionTask task = _task_queue.top();
    _task_queue.pop();

    bool ok = true;
    switch (task.type) {
      case TaskType::EvalRootNode:
        // Already done
        break;
      case TaskType::EvalSubLayers:
        ok = EvalSubLayers(task.node_idx, err);
        break;
      case TaskType::EvalImpliedInherits:
        ok = PropagateImpliedInherits(task.node_idx, err);
        break;
      case TaskType::EvalInherits:
        ok = EvalInherits(task.node_idx, err);
        break;
      case TaskType::EvalVariants:
        ok = EvalVariants(task.node_idx, err);
        break;
      case TaskType::EvalReferences:
        ok = EvalReferences(task.node_idx, err);
        break;
      case TaskType::EvalPayloads:
        ok = EvalPayloads(task.node_idx, err);
        break;
      case TaskType::EvalImpliedSpecializes:
        ok = PropagateImpliedSpecializes(task.node_idx, err);
        break;
      case TaskType::EvalSpecializes:
        ok = EvalSpecializes(task.node_idx, err);
        break;
      case TaskType::EvalRelocates:
        ok = EvalRelocates(task.node_idx, err);
        break;
    }

    if (!ok) {
      return false;
    }
  }

  return true;
}

nonstd::expected<PrimIndex, std::string> PrimIndexBuilder::FinishBuild() {
  std::string err;

  // Phase 2: Process task queue
  if (!DrainTaskQueue(&err)) {
    return nonstd::make_unexpected(err);
  }

  // Phase 3: Resolve deferred variant selections
  ResolveAndApplyVariants(&err);

  // Phase 3b: Compose variant CONTENT. Resolve the selected variant on any node
  // that authors a matching variantSet and repoint it at the materialized
  // result, so the variant's child prims (and their arcs) enter the namespace.
  // A variant child that itself references/payloads is handled by the normal
  // namespace expansion when that child's index is built. A variant can also
  // author arcs on the selected prim itself; ApplyVariantContent() re-scans the
  // resolved PrimSpec, so drain any newly queued tasks before final culling.
  uint32_t variant_passes = 0;
  while (ApplyVariantContent()) {
    if (++variant_passes > _ctx->_options.max_depth) {
      return nonstd::make_unexpected(
          "Variant composition depth limit exceeded");
    }
    if (!DrainTaskQueue(&err)) {
      return nonstd::make_unexpected(err);
    }
  }

  // Phase 4: Check for relocates on root layer and enqueue if needed
  if (_ctx->_root_layer &&
      !_ctx->_root_layer->metas().layerRelocates.empty()) {
    // Relocates are handled at the CompositionGraph level, not per-prim
  }

  // Phase 5: Compute strength order and cull inert nodes
  CullInertNodes();
  ComputeStrengthOrder();

  // Phase 6: Detect instanceable from the composed (strongest) opinion. For a
  // root prim this is the root node (== _root_primspec); for a child prim built
  // by descent there is no single root PrimSpec, so read the strongest node.
  _result._is_instanceable = false;
  for (uint16_t order_idx : _result._strength_order) {
    const PrimSpec *ps = GetPrimSpecForNode(order_idx);
    if (!ps) continue;
    if (ps->metas().has_instanceable()) {
      _result._is_instanceable = ps->metas().get_instanceable();
      break;
    }
  }

  return std::move(_result);
}

// ---------------------------------------------------------------------------
// EvalRootNode
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalRootNode(std::string *err) {
  uint32_t path_idx =
      _ctx->InternPath(_result._prim_path.prim_part());

  uint16_t root_idx =
      AddNode(ArcType::Root, CompNode::kInvalidIndex,
              _root_layer_stack_idx, 0, path_idx,
              CompNode::kInvalidIndex);

  if (root_idx == CompNode::kInvalidIndex) {
    if (err) *err = "Failed to create root node (overflow)";
    return false;
  }

  // Check if root has specs
  if (!_root_primspec->props().empty() ||
      _root_primspec->metas().authored() ||
      !_root_primspec->children().empty()) {
    _result._nodes[root_idx].flags =
        _result._nodes[root_idx].flags | NodeFlags::HasSpecs;
  }

  // Scan for composition arcs and enqueue tasks
  ScanArcsAndEnqueueTasks(root_idx, *_root_primspec);

  return true;
}

// ---------------------------------------------------------------------------
// EvalSubLayers -- sublayers are already composed before we get here
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalSubLayers(uint16_t /*node_idx*/,
                                     std::string * /*err*/) {
  // Sublayers (L phase) are composed before CompositionGraph::Compose()
  // is called, so there's nothing to do here. The root layer already
  // contains the composed sublayer opinions.
  return true;
}

// ---------------------------------------------------------------------------
// EvalInherits
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalInherits(uint16_t node_idx, std::string * /* err */) {
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) return true;

  const auto &inherits_opt = ps->metas().inherits;
  if (!inherits_opt.has_value()) return true;

  uint16_t sibling_count = 0;

  for (const auto &listop_pair : *inherits_opt) {
    const ListEditQual qual = listop_pair.first;
    const auto &paths = listop_pair.second;

    for (const auto &inherit_path : paths) {
      const std::string &path_str = inherit_path.prim_part();

      // Cycle detection
      if (_inherit_visited.count(path_str) > 0) {
        DCOUT("Inherit cycle detected at " << path_str);
        continue;
      }
      _inherit_visited.insert(path_str);

      // Find the target PrimSpec in the root layer
      const PrimSpec *target_ps = nullptr;
      std::string find_err;
      if (_ctx->_root_layer &&
          _ctx->_root_layer->find_primspec_at(inherit_path, &target_ps,
                                                &find_err) &&
          target_ps) {
        // Create namespace mapping: inherit maps target -> this prim
        NamespaceMapping mapping =
            MakeInheritMapping(inherit_path, _result._prim_path);

        uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
        uint32_t site_path_idx = _ctx->InternPath(path_str);

        uint16_t child_idx =
            AddNode(ArcType::Inherit, node_idx,
                    _result._nodes[node_idx].layer_stack_idx, 0,
                    site_path_idx, map_idx);

        if (child_idx != CompNode::kInvalidIndex) {
          AppendChild(node_idx, child_idx);
          _result._nodes[child_idx].sibling_num = sibling_count++;

          // Mark HasSpecs if target has content
          if (!target_ps->props().empty() ||
              target_ps->metas().authored()) {
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
          }

          // Collect variant opinions from inherited content
          CollectVariantOpinions(child_idx);

          // Check for arcs on the inherited prim (recursive)
          ScanArcsAndEnqueueTasks(child_idx, *target_ps);

          // Enqueue implied inherit propagation
          if (target_ps->metas().inherits.has_value() ||
              target_ps->metas().specializes.has_value()) {
            _task_queue.push(
                {TaskType::EvalImpliedInherits, child_idx, 0, 0});
          }
        }
      } else {
        // Target class not found -- silently skip (common for classes
        // defined in other layers)
        (void)qual;
      }

      // NOTE: do NOT erase here. Arcs are processed from a deferred task
      // queue (ScanArcsAndEnqueueTasks only enqueues), so erasing before the
      // child task runs made this cycle guard a no-op and a self/mutual
      // inherit looped to the per-index node cap. Keeping the target site
      // marked for the whole build composes each class at most once (idempotent
      // for USD class composition) and bounds cycles.
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// EvalReferences
// ---------------------------------------------------------------------------

// True if following an arc from `node_idx` to (layer_stack, site_path) would
// revisit a site already present in the ancestor node chain — i.e. a
// composition cycle (A references B references A, or an internal self/ancestor
// reference). Without this, a cyclic internal reference reconstructs referenced
// descendants into ever-deeper composed prims: exponential node/prim blow-up
// (multi-hundred-MB hang) that a depth cap alone cannot tame.
bool PrimIndexBuilder::ArcWouldRevisitSite(uint16_t node_idx,
                                           uint16_t target_ls_idx,
                                           uint32_t target_site_idx) const {
  uint16_t cur = node_idx;
  // Bound the walk by the node count (defensive against a malformed chain).
  for (size_t steps = 0; steps <= _result._nodes.size(); ++steps) {
    if (cur >= _result._nodes.size()) break;
    const CompNode &n = _result._nodes[cur];
    if (n.layer_stack_idx == target_ls_idx &&
        n.site_path_idx == target_site_idx) {
      return true;
    }
    if (n.parent == CompNode::kInvalidIndex || n.parent == cur) break;
    cur = n.parent;
  }
  return false;
}

bool PrimIndexBuilder::EvalReferences(uint16_t node_idx, std::string *err) {
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) return true;

  const auto &refs_opt = ps->metas().references;
  if (!refs_opt.has_value()) return true;

  // Bound the arc chain by the NODE's composition depth, not the builder's
  // `_depth` member: arcs are processed from a deferred task queue
  // (ScanArcsAndEnqueueTasks only enqueues), so `_depth++/--` around the
  // enqueue is restored before the task runs — the member is effectively
  // constant and never bounds a reference cycle (internal or external), which
  // grows nodes to the uint16 cap (~65535) = a multi-hundred-MB DoS.
  // node.depth increments per parent chain and is the true recursion depth.
  if (node_idx < _result._nodes.size() &&
      _result._nodes[node_idx].depth >=
          std::min<uint32_t>(_ctx->_options.max_depth, 255)) {
    // Chain too deep (likely a reference cycle). Stop, don't fail the build.
    return true;
  }

  uint16_t sibling_count = 0;

  for (const auto &listop_pair : *refs_opt) {
    const ListEditQual qual = listop_pair.first;
    const auto &refs = listop_pair.second;
    (void)qual;

    for (const auto &ref : refs) {
      std::string asset_path_str = ref.asset_path.GetAssetPath();

      // Internal reference (empty asset path)
      if (asset_path_str.empty()) {
        if (!ref.prim_path.is_valid()) continue;

        const std::string &target_path = ref.prim_path.prim_part();

        // Find target in root layer
        const PrimSpec *target_ps = nullptr;
        std::string find_err;
        if (_ctx->_root_layer &&
            _ctx->_root_layer->find_primspec_at(ref.prim_path, &target_ps,
                                                  &find_err) &&
            target_ps) {
          NamespaceMapping mapping =
              MakeReferenceMapping(ref.prim_path, _result._prim_path, true);

          uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
          uint32_t site_path_idx = _ctx->InternPath(target_path);

          if (ArcWouldRevisitSite(
                  node_idx, _result._nodes[node_idx].layer_stack_idx,
                  site_path_idx)) {
            continue;  // internal reference cycle
          }

          uint16_t child_idx =
              AddNode(ArcType::Reference, node_idx,
                      _result._nodes[node_idx].layer_stack_idx, 0,
                      site_path_idx, map_idx);

          if (child_idx != CompNode::kInvalidIndex) {
            AppendChild(node_idx, child_idx);
            _result._nodes[child_idx].sibling_num = sibling_count++;

            if (!target_ps->props().empty() ||
                target_ps->metas().authored()) {
              _result._nodes[child_idx].flags =
                  _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
            }

            CollectVariantOpinions(child_idx);
            ScanArcsAndEnqueueTasks(child_idx, *target_ps);
          }
        }
        continue;
      }

      if (!ValidateAndNormalizeAssetPath(
              asset_path_str, &asset_path_str,
              _ctx->_options.allow_parent_relative_paths)) {
        if (_ctx->_options.error_when_asset_not_found) {
          if (err) {
            *err = fmt::format("Unsafe asset path in reference: {}",
                               ref.asset_path.GetAssetPath());
          }
          return false;
        }
        continue;
      }

      // External reference -- cycle detection
      std::string cycle_key = asset_path_str + ":" + ref.prim_path.prim_part();
      if (WouldCreateCycle(asset_path_str, ref.prim_path.prim_part())) {
        DCOUT("Reference cycle detected: " << cycle_key);
        continue;
      }

      _arc_stack.insert({asset_path_str, ref.prim_path.prim_part()});

      // Load the referenced asset via the layer-loading seam (parse-once
      // through pcp::Cache's registry, or parse-fresh by default).
      std::string load_warn, load_err;
      std::string cwp = ps->get_current_working_path();
      const Layer *ref_layer =
          LoadArcLayer(asset_path_str, cwp, &load_warn, &load_err);

      if (ref_layer) {
        const PrimSpec *ref_root_ps = nullptr;
        // Find the target prim in the loaded layer
        Path target_prim_path = ref.prim_path;
        if (!target_prim_path.is_valid() ||
            target_prim_path.prim_part().empty()) {
          // Use defaultPrim
          std::string dp = ref_layer->metas().defaultPrim.str();
          if (!dp.empty()) {
            target_prim_path = Path("/" + dp, "");
          }
        }

        if (target_prim_path.is_valid() &&
            ref_layer->find_primspec_at(target_prim_path, &ref_root_ps,
                                        &load_err) &&
            ref_root_ps) {
          uint16_t ref_ls_idx =
              _ctx->AddLayerStack(ref_layer, asset_path_str, ref.layerOffset);

          NamespaceMapping mapping = MakeReferenceMapping(
              target_prim_path, _result._prim_path, false);
          uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
          uint32_t site_path_idx =
              _ctx->InternPath(target_prim_path.prim_part());

          if (ArcWouldRevisitSite(node_idx, ref_ls_idx, site_path_idx)) {
            _arc_stack.erase({asset_path_str, ref.prim_path.prim_part()});
            continue;  // reference cycle (ancestor site revisited)
          }

          uint16_t child_idx =
              AddNode(ArcType::Reference, node_idx, ref_ls_idx, 0,
                      site_path_idx, map_idx);

          if (child_idx != CompNode::kInvalidIndex) {
            AppendChild(node_idx, child_idx);
            _result._nodes[child_idx].sibling_num = sibling_count++;

            if (!ref_root_ps->props().empty() ||
                ref_root_ps->metas().authored()) {
              _result._nodes[child_idx].flags =
                  _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
            }

            CollectVariantOpinions(child_idx);

            // Recursively scan the referenced prim for more arcs
            _depth++;
            ScanArcsAndEnqueueTasks(child_idx, *ref_root_ps);
            _depth--;

            // Check for implied inherits/specializes
            if (ref_root_ps->metas().inherits.has_value()) {
              _task_queue.push(
                  {TaskType::EvalImpliedInherits, child_idx, 0, 0});
            }
            if (ref_root_ps->metas().specializes.has_value()) {
              _task_queue.push(
                  {TaskType::EvalImpliedSpecializes, child_idx, 0, 0});
            }
          }
        }
      } else if (IsFileDescriptorLimitError(load_err)) {
        if (err) {
          *err = fmt::format("Failed to load referenced asset `{}`: {}",
                             asset_path_str, load_err);
        }
        _arc_stack.erase({asset_path_str, ref.prim_path.prim_part()});
        return false;
      } else if (_ctx->_options.error_when_asset_not_found) {
        if (err) {
          *err = fmt::format("Failed to load referenced asset: {}",
                             asset_path_str);
        }
        _arc_stack.erase({asset_path_str, ref.prim_path.prim_part()});
        return false;
      }

      _arc_stack.erase({asset_path_str, ref.prim_path.prim_part()});
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// EvalPayloads
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalPayloads(uint16_t node_idx, std::string *err) {
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) return true;

  const auto &payload_opt = ps->metas().payload;
  if (!payload_opt.has_value()) return true;

  if (node_idx < _result._nodes.size() &&
      _result._nodes[node_idx].depth >=
          std::min<uint32_t>(_ctx->_options.max_depth, 255)) {
    // Chain too deep (likely a payload cycle). Stop, don't fail the build.
    return true;
  }

  uint16_t sibling_count = 0;

  for (const auto &listop_pair : *payload_opt) {
    const ListEditQual qual = listop_pair.first;
    const auto &payloads = listop_pair.second;
    (void)qual;

    for (const auto &pl : payloads) {
      if (pl.is_none()) continue;

      std::string asset_path_str = pl.asset_path.GetAssetPath();

      // Check load policy
      bool should_load = true;
      if (_ctx->_options.payload_policy_with_prim) {
        should_load = _ctx->_options.payload_policy_with_prim(
            _result._prim_path, pl, *ps);
      } else if (_ctx->_options.payload_policy) {
        should_load =
            _ctx->_options.payload_policy(_result._prim_path, pl);
      }

      // Always create the node (for structural identity in InstanceKey)
      uint32_t site_path_idx =
          _ctx->InternPath(pl.prim_path.prim_part());

      // Create a placeholder namespace mapping
      NamespaceMapping mapping;
      if (pl.prim_path.is_valid() && !pl.prim_path.prim_part().empty()) {
        mapping = MakeReferenceMapping(
            pl.prim_path, _result._prim_path, asset_path_str.empty());
      }
      uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);

      uint16_t child_idx =
          AddNode(ArcType::Payload, node_idx,
                  CompNode::kInvalidIndex,  // layer not yet loaded
                  0, site_path_idx, map_idx);

      if (child_idx == CompNode::kInvalidIndex) continue;

      AppendChild(node_idx, child_idx);
      _result._nodes[child_idx].sibling_num = sibling_count++;

      if (!should_load) {
        // Mark as deferred
        _result._nodes[child_idx].flags =
            _result._nodes[child_idx].flags | NodeFlags::PayloadDeferred;

        // Store deferred info for later loading
        DeferredPayloadInfo info;
        info.prim_path = _result._prim_path;
        info.payload = pl;
        info.node_idx = child_idx;
        info.current_working_path = ps->get_current_working_path();
        info.asset_search_paths = ps->get_asset_search_paths();
        _ctx->_deferred_payloads.push_back(std::move(info));
        continue;
      }

      // Load the payload asset
      if (asset_path_str.empty()) {
        // Internal payload
        if (pl.prim_path.is_valid()) {
          const PrimSpec *target_ps = nullptr;
          std::string find_err;
          if (_ctx->_root_layer &&
              _ctx->_root_layer->find_primspec_at(
                  pl.prim_path, &target_ps, &find_err) &&
              target_ps &&
              // Skip a cyclic internal payload (target site already an
              // ancestor) — otherwise it re-expands referenced content
              // unboundedly.
              !ArcWouldRevisitSite(
                  node_idx, _result._nodes[node_idx].layer_stack_idx,
                  site_path_idx)) {
            _result._nodes[child_idx].layer_stack_idx =
                _result._nodes[node_idx].layer_stack_idx;
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags |
                NodeFlags::PayloadLoaded | NodeFlags::HasSpecs;

            CollectVariantOpinions(child_idx);
            ScanArcsAndEnqueueTasks(child_idx, *target_ps);
          }
        }
      } else {
        if (!ValidateAndNormalizeAssetPath(
                asset_path_str, &asset_path_str,
                _ctx->_options.allow_parent_relative_paths)) {
          if (_ctx->_options.error_when_asset_not_found) {
            if (err) {
              *err = fmt::format("Unsafe asset path in payload: {}",
                                 pl.asset_path.GetAssetPath());
            }
            return false;
          }
          continue;
        }

        // External payload -- similar to references
        if (WouldCreateCycle(asset_path_str, pl.prim_path.prim_part())) {
          DCOUT("Payload cycle detected: " << asset_path_str);
          continue;
        }

        _arc_stack.insert({asset_path_str, pl.prim_path.prim_part()});

        std::string load_warn, load_err;
        std::string cwp = ps->get_current_working_path();
        const Layer *pl_layer =
            LoadArcLayer(asset_path_str, cwp, &load_warn, &load_err);

        if (pl_layer) {
          Path target_path = pl.prim_path;
          if (!target_path.is_valid() || target_path.prim_part().empty()) {
            std::string dp = pl_layer->metas().defaultPrim.str();
            if (!dp.empty()) {
              target_path = Path("/" + dp, "");
            }
          }

          const PrimSpec *pl_root_ps = nullptr;
          if (target_path.is_valid() &&
              pl_layer->find_primspec_at(target_path, &pl_root_ps, &load_err) &&
              pl_root_ps) {
            uint16_t pl_ls_idx =
                _ctx->AddLayerStack(pl_layer, asset_path_str, pl.layerOffset);
            NamespaceMapping loaded_mapping =
                MakeReferenceMapping(target_path, _result._prim_path, false);
            uint16_t loaded_map_idx =
                _ctx->AddMapExpression(loaded_mapping, -1);

            _result._nodes[child_idx].layer_stack_idx = pl_ls_idx;
            _result._nodes[child_idx].site_path_idx =
                _ctx->InternPath(target_path.prim_part());
            _result._nodes[child_idx].map_expr_idx = loaded_map_idx;
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags |
                NodeFlags::PayloadLoaded | NodeFlags::HasSpecs;

            CollectVariantOpinions(child_idx);

            _depth++;
            ScanArcsAndEnqueueTasks(child_idx, *pl_root_ps);
            _depth--;

            // Check for implied arcs
            if (pl_root_ps->metas().inherits.has_value()) {
              _task_queue.push(
                  {TaskType::EvalImpliedInherits, child_idx, 0, 0});
            }
            if (pl_root_ps->metas().specializes.has_value()) {
              _task_queue.push(
                  {TaskType::EvalImpliedSpecializes, child_idx, 0, 0});
            }
          }
        } else if (IsFileDescriptorLimitError(load_err)) {
          if (err) {
            *err = fmt::format("Failed to load payload asset `{}`: {}",
                               asset_path_str, load_err);
          }
          _arc_stack.erase({asset_path_str, pl.prim_path.prim_part()});
          return false;
        }

        _arc_stack.erase({asset_path_str, pl.prim_path.prim_part()});
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// EvalVariants
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalVariants(uint16_t node_idx, std::string *err) {
  (void)err;

  // Variant evaluation is deferred per AOUSD Core Spec 10.3.2.5.
  // Here we only collect variant selection opinions from this node.
  // The actual resolution and application happens after all I, R, P
  // tasks are processed (in ResolveAndApplyVariants).
  CollectVariantOpinions(node_idx);
  return true;
}

void PrimIndexBuilder::CollectVariantOpinions(uint16_t node_idx) {
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) return;

  if (ps->metas().variants.has_value()) {
    const std::string &prim_path = _result._prim_path.prim_part();
    _variant_opinions[prim_path].push_back(*ps->metas().variants);
  }
}

void PrimIndexBuilder::ResolveAndApplyVariants(std::string *err) {
  (void)err;

  // For each prim path with variant opinions, resolve the strongest opinion
  // per variant set name. Strongest = first in the collected vector (collected
  // in LIVRPS order since the task queue processes in that order).
  //
  // Note: In the DAG model, variants don't create new nodes. Instead, the
  // variant selection affects which PrimSpec content is used during value
  // resolution. The variant selection is stored as metadata on the PrimIndex
  // and consulted when resolving values.
  //
  // For now, we just record the resolved selections. The ComposePrimSpecFromIndex
  // function will apply them during Stage building.
}

// ---------------------------------------------------------------------------
// ApplyVariantContent -- compose the selected variant's content into the index
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::ApplyVariantContent() {
  // 1. Compose the variant selection: strongest opinion per variant set. Node 0
  //    is the root (strongest); arc nodes are appended in decreasing strength,
  //    so a first-seen-wins scan over node index gives the composed selection.
  std::map<std::string, std::string> selection;
  for (uint16_t i = 0; i < static_cast<uint16_t>(_result._nodes.size()); i++) {
    if (_result._nodes[i].is_culled()) continue;
    const PrimSpec *ps = GetPrimSpecForNode(i);
    if (!ps || !ps->metas().variants.has_value()) continue;
    for (const auto &kv : ps->metas().variants.value()) {
      selection.emplace(kv.first, kv.second);  // strongest (lower index) wins
    }
  }
  if (selection.empty()) return false;

  bool changed = false;

  // 2. Resolve each node that authors matching variantSet CONTENT.
  const size_t node_count = _result._nodes.size();
  for (uint16_t i = 0; i < static_cast<uint16_t>(node_count); i++) {
    CompNode &node = _result._nodes[i];
    if (node.is_culled()) continue;

    const PrimSpec *ps = GetPrimSpecForNode(i);
    if (!ps || ps->variantSets().empty()) continue;

    // Does this node author content for any selected variant set?
    bool match = false;
    for (const auto &vs : ps->variantSets()) {
      if (selection.count(vs.first)) {
        match = true;
        break;
      }
    }
    if (!match) continue;

    // Resolve. VariantSelectPrimSpec gates on the PrimSpec carrying BOTH the
    // `variants` selection and the `variantSets` listop; the selection here is
    // composed from other (stronger) nodes, so inject it onto a copy.
    PrimSpec src_ps = *ps;
    src_ps.metas().variants = selection;
    PrimSpec resolved;
    std::string vw, ve;
    if (!VariantSelectPrimSpec(resolved, src_ps, selection, &vw, &ve)) continue;

    // Materialize the resolved PrimSpec in a synthetic layer owned by the
    // context, and repoint the node at it. For root prims, preserve the source
    // layer's sibling prims so internal references authored inside the selected
    // variant can still resolve against that layer. Its
    // children (the variant's child prims) are then reachable by
    // find_primspec_at and so by the namespace expansion / value composition.
    std::string nm = ps->name();
    if (nm.empty()) nm = "_variant";
    resolved.name() = nm;

    const std::string original_site = _ctx->_path_table[node.site_path_idx];
    const bool root_site = (original_site == "/" + nm);
    const Layer *src_layer = nullptr;
    if (root_site && node.layer_stack_idx != CompNode::kInvalidIndex &&
        node.layer_stack_idx < _ctx->_layer_stacks.size()) {
      src_layer = _ctx->_layer_stacks[node.layer_stack_idx].layer;
    }

    auto synth = src_layer ? std::make_unique<Layer>(*src_layer)
                           : std::make_unique<Layer>();
    if (synth->has_primspec(nm)) {
      synth->replace_primspec(nm, resolved);
    } else {
      synth->add_primspec(nm, resolved);
    }
    const Layer *synth_ptr = synth.get();
    _ctx->_loaded_layers.push_back(std::move(synth));

    uint16_t ls =
        _ctx->AddLayerStack(synth_ptr, "<variant>", LayerOffset());
    node.layer_stack_idx = ls;
    node.site_path_idx = _ctx->InternPath("/" + nm);

    const PrimSpec &rps = synth_ptr->primspecs().at(nm);
    if (!rps.props().empty() || rps.metas().authored() ||
        !rps.children().empty()) {
      node.flags = node.flags | NodeFlags::HasSpecs;
    }
    ScanArcsAndEnqueueTasks(i, rps);
    changed = true;
  }

  return changed;
}

// ---------------------------------------------------------------------------
// EvalSpecializes
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalSpecializes(uint16_t node_idx, std::string * /* err */) {
  const PrimSpec *ps = GetPrimSpecForNode(node_idx);
  if (!ps) return true;

  const auto &spec_opt = ps->metas().specializes;
  if (!spec_opt.has_value()) return true;

  uint16_t sibling_count = 0;

  for (const auto &listop_pair : *spec_opt) {
    const auto &paths = listop_pair.second;

    for (const auto &spec_path : paths) {
      const std::string &path_str = spec_path.prim_part();

      // Cycle detection
      if (_specialize_visited.count(path_str) > 0) {
        DCOUT("Specialize cycle detected at " << path_str);
        continue;
      }
      _specialize_visited.insert(path_str);

      // Find the target PrimSpec in the root layer
      const PrimSpec *target_ps = nullptr;
      std::string find_err;
      if (_ctx->_root_layer &&
          _ctx->_root_layer->find_primspec_at(spec_path, &target_ps,
                                                &find_err) &&
          target_ps) {
        NamespaceMapping mapping =
            MakeInheritMapping(spec_path, _result._prim_path);
        uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
        uint32_t site_path_idx = _ctx->InternPath(path_str);

        // Specializes are added at the ROOT level for globally-weak semantics
        // (AOUSD Core Spec 10.4.1)
        uint16_t root_idx = 0;  // Root is always node 0

        uint16_t child_idx =
            AddNode(ArcType::Specialize, root_idx,
                    _result._nodes[node_idx].layer_stack_idx, 0,
                    site_path_idx, map_idx);

        if (child_idx != CompNode::kInvalidIndex) {
          AppendChild(root_idx, child_idx);
          _result._nodes[child_idx].sibling_num = sibling_count++;

          // If this specialize came from a non-root node, mark as implied
          if (node_idx != 0) {
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags | NodeFlags::IsImpliedArc;
          }
          _result._nodes[child_idx].origin = node_idx;

          if (!target_ps->props().empty() ||
              target_ps->metas().authored()) {
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
          }

          // Don't recursively scan specializes -- they are globally weak
          // and should not introduce further arcs at this position
        }
      }

      // NOTE: do NOT erase — see EvalInherits. A persistent per-build visited
      // set composes each specialize target once and stops self/mutual
      // specialize cycles (the erase-before-deferred-task made it a no-op).
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// EvalRelocates
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::EvalRelocates(uint16_t /*node_idx*/,
                                     std::string * /*err*/) {
  // Relocates are handled at the CompositionGraph level as a post-pass
  // (similar to existing lightusd approach). Individual PrimIndex nodes
  // don't need relocate evaluation -- the relocates are applied to the
  // final composed namespace.
  return true;
}

// ---------------------------------------------------------------------------
// Implied arc propagation
// ---------------------------------------------------------------------------

bool PrimIndexBuilder::PropagateImpliedInherits(uint16_t source_node,
                                                std::string *err) {
  (void)err;

  // When a referenced/payload layer contains inherits, those class
  // hierarchies should be propagated to the referencing layer stack.
  //
  // Find the source node's PrimSpec and check for inherits.
  const PrimSpec *ps = GetPrimSpecForNode(source_node);
  if (!ps) return true;

  const auto &inherits_opt = ps->metas().inherits;
  if (!inherits_opt.has_value()) return true;

  for (const auto &listop_pair : *inherits_opt) {
    const auto &paths = listop_pair.second;

    for (const auto &inherit_path : paths) {
      // Check if the class prim exists in the ROOT layer
      const PrimSpec *class_ps = nullptr;
      std::string find_err;
      if (_ctx->_root_layer &&
          _ctx->_root_layer->find_primspec_at(inherit_path, &class_ps,
                                                &find_err) &&
          class_ps) {
        // The class exists in the root layer -- add an implied inherit node
        NamespaceMapping mapping =
            MakeInheritMapping(inherit_path, _result._prim_path);
        uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
        uint32_t site_path_idx =
            _ctx->InternPath(inherit_path.prim_part());

        // Add as child of the source node's parent (the reference/payload node)
        uint16_t parent_idx = _result._nodes[source_node].parent;
        if (parent_idx == CompNode::kInvalidIndex) {
          parent_idx = 0;  // fallback to root
        }

        uint16_t child_idx =
            AddNode(ArcType::Inherit, parent_idx,
                    _result._nodes[0].layer_stack_idx,  // root layer
                    0, site_path_idx, map_idx);

        if (child_idx != CompNode::kInvalidIndex) {
          AppendChild(parent_idx, child_idx);
          _result._nodes[child_idx].flags =
              _result._nodes[child_idx].flags | NodeFlags::IsImpliedArc;
          _result._nodes[child_idx].origin = source_node;

          if (!class_ps->props().empty() || class_ps->metas().authored()) {
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
          }
        }
      }
    }
  }
  return true;
}

bool PrimIndexBuilder::PropagateImpliedSpecializes(uint16_t source_node,
                                                   std::string *err) {
  (void)err;

  // Specializes from referenced/payload layers are propagated to
  // the root node for globally-weak semantics.
  const PrimSpec *ps = GetPrimSpecForNode(source_node);
  if (!ps) return true;

  const auto &spec_opt = ps->metas().specializes;
  if (!spec_opt.has_value()) return true;

  for (const auto &listop_pair : *spec_opt) {
    const auto &paths = listop_pair.second;

    for (const auto &spec_path : paths) {
      const PrimSpec *class_ps = nullptr;
      std::string find_err;
      if (_ctx->_root_layer &&
          _ctx->_root_layer->find_primspec_at(spec_path, &class_ps,
                                                &find_err) &&
          class_ps) {
        NamespaceMapping mapping =
            MakeInheritMapping(spec_path, _result._prim_path);
        uint16_t map_idx = _ctx->AddMapExpression(mapping, -1);
        uint32_t site_path_idx =
            _ctx->InternPath(spec_path.prim_part());

        // Always add to root for globally-weak semantics
        uint16_t child_idx =
            AddNode(ArcType::Specialize, 0,  // root
                    _result._nodes[0].layer_stack_idx, 0,
                    site_path_idx, map_idx);

        if (child_idx != CompNode::kInvalidIndex) {
          AppendChild(0, child_idx);
          _result._nodes[child_idx].flags =
              _result._nodes[child_idx].flags | NodeFlags::IsImpliedArc;
          _result._nodes[child_idx].origin = source_node;

          if (!class_ps->props().empty() || class_ps->metas().authored()) {
            _result._nodes[child_idx].flags =
                _result._nodes[child_idx].flags | NodeFlags::HasSpecs;
          }
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Strength order computation
// ---------------------------------------------------------------------------

void PrimIndexBuilder::ComputeStrengthOrder() {
  _result._strength_order.clear();

  // Collect all non-culled node indices
  // pxr's strength ordering is a PRE-ORDER traversal of the prim-index
  // tree: a node is stronger than everything in its subtree, and siblings
  // are already linked strongest-first (first_child/next_sibling). The old
  // flat `arc_type*10000 + depth*100 + sibling` formula both flattened
  // nesting (a reference's nested inherit outranked the reference) and
  // could invert child order across subtrees. Specializes remain globally
  // weakest: collect them during the walk and append at the end (direct
  // before implied), per Spec 10.4.1.
  std::vector<uint16_t> specializes;
  std::vector<uint16_t> stack;
  if (!_result._nodes.empty()) stack.push_back(0);
  while (!stack.empty()) {
    const uint16_t idx = stack.back();
    stack.pop_back();
    const CompNode &node = _result._nodes[idx];
    if (!node.is_culled()) {
      if (node.arc_type == ArcType::Specialize) {
        specializes.push_back(idx);
      } else {
        _result._strength_order.push_back(idx);
      }
    }
    // Push children in REVERSE sibling order so the strongest child pops
    // first (pre-order).
    std::vector<uint16_t> kids;
    for (uint16_t c = node.first_child; c != CompNode::kInvalidIndex;
         c = _result._nodes[c].next_sibling) {
      kids.push_back(c);
    }
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      stack.push_back(*it);
    }
  }
  std::stable_sort(specializes.begin(), specializes.end(),
                   [this](uint16_t a, uint16_t b) {
                     // direct specializes before implied ones
                     return !_result._nodes[a].is_implied_arc() &&
                            _result._nodes[b].is_implied_arc();
                   });
  for (uint16_t sp : specializes) _result._strength_order.push_back(sp);

  // Record each node's rank so incremental consumers keep a consistent view.
  for (size_t r = 0; r < _result._strength_order.size(); r++) {
    _result._nodes[_result._strength_order[r]].strength_order =
        static_cast<int32_t>(r);
  }
}

// ---------------------------------------------------------------------------
// Node culling
// ---------------------------------------------------------------------------

void PrimIndexBuilder::CullInertNodes() {
  // Mark nodes without specs and without children-with-specs as inert
  for (auto &node : _result._nodes) {
    if (!node.has_specs() && !node.has_children()) {
      node.flags = node.flags | NodeFlags::Inert;
    }
  }
}

// ---------------------------------------------------------------------------
// CompositionGraph::BuildPrimIndex
// ---------------------------------------------------------------------------

void CompositionGraph::DetectAndRegisterInstance(
    const std::string &prim_path, const std::shared_ptr<PrimIndex> &index) {
  if (!_ctx._options.detect_instances || !index->IsInstanceable()) return;

  InstanceKey key =
      ComputeInstanceKey(*index, _ctx._layer_stacks, _ctx._path_table);
  if (!key.is_valid()) return;

  auto it = _instance_key_to_prototype.find(key);
  if (it != _instance_key_to_prototype.end()) {
    _instance_to_prototype[prim_path] = static_cast<int>(it->second);
  } else {
    size_t proto_idx = _prototypes.size();
    _prototypes.push_back(index);
    _instance_key_to_prototype[key] = proto_idx;
    _instance_to_prototype[prim_path] = static_cast<int>(proto_idx);
  }
}

void CompositionGraph::EraseDescendantPrimIndices(
    const std::string &prim_path) {
  const std::string prefix = prim_path + "/";
  for (auto it = _prim_indices.begin(); it != _prim_indices.end();) {
    if (it->first.rfind(prefix, 0) == 0) {
      it = _prim_indices.erase(it);
    } else {
      ++it;
    }
  }
}

void CompositionGraph::RebuildInstanceRegistry() {
  _instance_key_to_prototype.clear();
  _prototypes.clear();
  _instance_to_prototype.clear();
  for (const auto &pair : _prim_indices) {
    DetectAndRegisterInstance(pair.first, pair.second);
  }
}

std::vector<std::string> CompositionGraph::GatherComposedChildNames(
    const PrimIndex &index) const {
  std::vector<std::string> names;
  std::set<std::string> seen;
  // Strongest-first so the composed child order follows opinion strength.
  for (uint16_t order_idx : index.GetStrengthOrder()) {
    const CompNode &n = index.GetNode(order_idx);
    if (n.is_culled() || n.is_payload_deferred()) continue;
    // NOTE: do NOT skip on !has_specs() here. A node may contribute namespace
    // children even when it authors no props/metas of its own (e.g. a reference
    // target that is just a parent of geometry). has_specs() is set from
    // props/metas only on arc nodes, so requiring it would drop such children.
    if (n.layer_stack_idx == CompNode::kInvalidIndex ||
        n.layer_stack_idx >= _ctx._layer_stacks.size()) {
      continue;
    }
    const LayerStackEntry &ls = _ctx._layer_stacks[n.layer_stack_idx];
    if (!ls.layer) continue;
    const std::string &site = _ctx._path_table[n.site_path_idx];
    const PrimSpec *ps = nullptr;
    std::string fe;
    if (!ls.layer->find_primspec_at(Path(site, ""), &ps, &fe) || !ps) continue;
    for (const auto &c : ps->children()) {
      if (seen.insert(c.name()).second) names.push_back(c.name());
    }
  }
  return names;
}

// Namespace depth = number of '/'-separated components in an absolute prim
// path ("/A/B/C" -> 3). Used to bound worklist growth from reference cycles
// that generate ever-longer unique paths.
static uint32_t NamespaceDepth(const std::string &abs_path) {
  uint32_t d = 0;
  for (char c : abs_path) {
    if (c == '/') ++d;
  }
  return d;  // leading '/' counts the first component boundary
}

// True if the composed index for `prim_path` contains a composition arc
// (reference/payload/inherit/specialize) whose TARGET SITE is an ancestor
// (or self) of `prim_path` — a namespace cycle (e.g. a prim that specializes
// its own parent). Descending into such a prim's children re-expands the
// ancestor's subtree, producing ever-longer unique paths (/A/B/A/B/...): an
// unbounded worklist / OOM. We register the prim once but do not descend.
// Content signature of a composed prim: an order-independent hash of the
// (layer_stack, site) of its ARC nodes (node 0, the local root, is excluded so
// the signature is position-independent). Two prims with the same signature
// pull in identical composed content. Used to detect a composition cycle: if a
// prim's signature equals one of its NAMESPACE ANCESTORS on the build path,
// descending would regenerate the same subtree forever (e.g. a variant/ref that
// re-selects the same content one level deeper: /P/a/b/a/b/...). Distinct
// instances of the same asset are siblings (never ancestors), so this does not
// false-positive on instancing. A signature of 0 means "no arc content" (a
// plain local prim) and never triggers the check.
uint64_t CompositionGraph::ComposedContentSignature(
    const PrimIndex &index) const {
  // Collect (layer_stack, site) of arc nodes, sort for order-independence.
  std::vector<uint64_t> keys;
  const uint16_t nc = index.GetNodeCount();
  for (uint16_t i = 1; i < nc; ++i) {  // skip node 0 (local root)
    const CompNode &n = index.GetNode(i);
    if (n.is_culled()) continue;
    switch (n.arc_type) {
      case ArcType::Reference:
      case ArcType::Payload:
      case ArcType::Inherit:
      case ArcType::Specialize:
      case ArcType::Variant:
        break;
      default:
        continue;
    }
    keys.push_back((uint64_t(n.layer_stack_idx) << 32) ^ n.site_path_idx);
  }
  if (keys.empty()) return 0;
  std::sort(keys.begin(), keys.end());
  // FNV-1a over the sorted keys.
  uint64_t h = 1469598103934665603ull;
  for (uint64_t k : keys) {
    for (int b = 0; b < 8; ++b) {
      h ^= (k >> (b * 8)) & 0xff;
      h *= 1099511628211ull;
    }
  }
  return h ? h : 1;  // never return 0 for non-empty content
}

bool CompositionGraph::IndexArcTargetsAncestor(const PrimIndex &index,
                                               const std::string &prim_path) const {
  const uint16_t nc = index.GetNodeCount();
  for (uint16_t i = 0; i < nc; ++i) {
    const CompNode &n = index.GetNode(i);
    switch (n.arc_type) {
      case ArcType::Reference:
      case ArcType::Payload:
      case ArcType::Inherit:
      case ArcType::Specialize:
        break;
      default:
        continue;
    }
    if (n.site_path_idx >= _ctx._path_table.size()) continue;
    const std::string &site = _ctx._path_table[n.site_path_idx];
    if (site.empty() || site == "/") continue;
    // site is an ancestor-or-self of prim_path?
    if (prim_path == site) return true;
    if (prim_path.size() > site.size() &&
        prim_path.compare(0, site.size(), site) == 0 &&
        prim_path[site.size()] == '/') {
      return true;
    }
  }
  return false;
}

bool CompositionGraph::BuildPrimIndex(const std::string &prim_path,
                                      const PrimSpec &primspec,
                                      uint16_t root_layer_stack_idx,
                                      std::string *warn, std::string *err) {
  (void)warn;

  // Iterative pre-order DFS (explicit heap worklist) so deeply nested prim
  // hierarchies cannot overflow the call stack, and so instance-prototype
  // selection ("first matching index wins") sees prims in a stable order.
  //
  // Unlike the old walk (which recursed only over the root-layer PrimSpec's
  // children), each prim's children are its COMPOSED children -- the union of
  // children across every node of its index, including those introduced by
  // references/payloads. A child is built by descending the parent index one
  // namespace level (BuildChildFrom), which both reconstructs referenced
  // descendants and follows their own (nested) arcs.
  // Persistent immutable chain of composed-content signatures along the build
  // path from the root prim. Shared across sibling branches (cheap to extend);
  // membership check is O(namespace depth). Detects composition cycles in
  // O(cycle length) instead of relying on the size backstops.
  struct SigNode {
    uint64_t sig;
    std::shared_ptr<const SigNode> parent;
  };
  auto sig_on_path = [](const std::shared_ptr<const SigNode> &tail,
                        uint64_t sig) {
    for (const SigNode *n = tail.get(); n; n = n->parent.get()) {
      if (n->sig == sig) return true;
    }
    return false;
  };

  struct Item {
    std::string path;
    std::shared_ptr<PrimIndex> parent;  // null for the root prim
    std::string child_name;             // unused for the root prim
    std::shared_ptr<const SigNode> ancestor_sigs;  // build-path signatures
  };
  std::vector<Item> stack;
  stack.push_back({prim_path, nullptr, std::string(), nullptr});

  while (!stack.empty()) {
    Item item = std::move(stack.back());
    stack.pop_back();

    if (_prim_indices.count(item.path)) continue;  // already built

    std::shared_ptr<PrimIndex> index;
    if (!item.parent) {
      // Root prim: build from its root-layer PrimSpec.
      PrimIndexBuilder builder(&_ctx, Path(item.path, ""), primspec,
                               root_layer_stack_idx);
      auto result = builder.Build();
      if (!result) {
        if (err) *err = result.error();
        return false;
      }
      index = std::make_shared<PrimIndex>(std::move(*result));
    } else {
      // Child prim: build by descending the parent index.
      PrimIndexBuilder builder(&_ctx, Path(item.path, ""));
      auto result = builder.BuildChildFrom(*item.parent, item.child_name);
      if (!result) continue;  // no composable opinions for this child; skip
      index = std::make_shared<PrimIndex>(std::move(*result));
    }

    DetectAndRegisterInstance(item.path, index);
    _prim_indices[item.path] = index;

    // Backstop: bound the total number of composed prims (a cycle can fan out
    // under the per-path depth cap).
    if (_ctx._options.max_composed_prims > 0 &&
        _prim_indices.size() > _ctx._options.max_composed_prims) {
      if (err) *err = "Composed prim count exceeded max_composed_prims";
      return false;
    }

    // Composition cycle: this prim composes an arc back to one of its own
    // ancestors (IndexArcTargetsAncestor), OR its composed arc content is
    // identical to a namespace ancestor's (content signature) — a
    // variant/reference that re-selects the same content one level deeper,
    // which would regenerate the same subtree forever. Register the prim
    // (done above) but don't descend.
    if (IndexArcTargetsAncestor(*index, item.path)) {
      continue;
    }
    const uint64_t sig = ComposedContentSignature(*index);
    if (sig != 0 && sig_on_path(item.ancestor_sigs, sig)) {
      continue;  // content cycle: identical composed arcs as an ancestor
    }
    std::shared_ptr<const SigNode> child_sigs =
        (sig != 0) ? std::make_shared<const SigNode>(SigNode{sig, item.ancestor_sigs})
                   : item.ancestor_sigs;

    // Enqueue composed children in reverse for left-to-right pre-order. Reject
    // paths deeper than the namespace-depth cap.
    const std::vector<std::string> child_names =
        GatherComposedChildNames(*index);
    for (auto it = child_names.rbegin(); it != child_names.rend(); ++it) {
      const std::string child_path = item.path + "/" + *it;
      if (NamespaceDepth(child_path) > _ctx._options.max_namespace_depth) {
        continue;  // too deep: likely a reference cycle; stop descending
      }
      stack.push_back({child_path, index, *it, child_sigs});
    }
  }

  return true;
}

bool CompositionGraph::RebuildDescendantPrimIndices(
    const std::string &prim_path, const std::shared_ptr<PrimIndex> &parent,
    std::string *err) {
  if (!parent) return true;

  struct Item {
    std::string path;
    std::shared_ptr<PrimIndex> parent;
    std::string child_name;
  };

  std::vector<Item> stack;
  const uint32_t max_ns_depth = _ctx._options.max_namespace_depth;
  const std::vector<std::string> child_names = GatherComposedChildNames(*parent);
  for (auto it = child_names.rbegin(); it != child_names.rend(); ++it) {
    const std::string child_path = prim_path + "/" + *it;
    if (NamespaceDepth(child_path) > max_ns_depth) continue;
    stack.push_back({child_path, parent, *it});
  }

  while (!stack.empty()) {
    Item item = std::move(stack.back());
    stack.pop_back();

    if (_ctx._options.max_composed_prims > 0 &&
        _prim_indices.size() > _ctx._options.max_composed_prims) {
      if (err) *err = "Composed prim count exceeded max_composed_prims";
      return false;
    }

    PrimIndexBuilder builder(&_ctx, Path(item.path, ""));
    auto result = builder.BuildChildFrom(*item.parent, item.child_name);
    if (!result) continue;

    auto index = std::make_shared<PrimIndex>(std::move(*result));
    _prim_indices[item.path] = index;

    if (IndexArcTargetsAncestor(*index, item.path)) continue;  // namespace cycle

    const std::vector<std::string> nested = GatherComposedChildNames(*index);
    for (auto it = nested.rbegin(); it != nested.rend(); ++it) {
      const std::string child_path = item.path + "/" + *it;
      if (NamespaceDepth(child_path) > max_ns_depth) continue;
      stack.push_back({child_path, index, *it});
    }
  }

  (void)err;
  return true;
}

// ---------------------------------------------------------------------------
// CompositionGraph::Compose -- main entry point
// ---------------------------------------------------------------------------

nonstd::expected<CompositionGraph, std::string> CompositionGraph::Compose(
    AssetResolutionResolver &resolver, const Layer &root_layer,
    const CompositionGraphOptions &options) {
  CompositionGraph graph;
  graph._ctx._resolver = &resolver;
  graph._ctx._options = options;
  graph._ctx._max_arc_nodes = options.max_arc_nodes;

  // Route referenced/payload layer loads through the optional parse-once seam.
  graph._ctx.load_layer_fn = options.load_layer_fn;
  graph._ctx.load_layer_userdata = options.load_layer_userdata;

  // Store a copy of the root layer so it outlives the graph
  // (caller's Layer may go out of scope after Compose returns)
  auto root_copy = std::make_unique<Layer>(root_layer);
  graph._ctx._root_layer = root_copy.get();
  graph._ctx._loaded_layers.push_back(std::move(root_copy));

  // Register the root layer in the layer stack table
  uint16_t root_ls_idx =
      graph._ctx.AddLayerStack(graph._ctx._root_layer, "<root>", LayerOffset());

  // Build PrimIndex for each root-level PrimSpec
  std::string warn, err;
  for (const auto &pair : root_layer.primspecs()) {
    const std::string &name = pair.first;
    const PrimSpec &ps = pair.second;

    std::string prim_path = "/" + name;
    if (!graph.BuildPrimIndex(prim_path, ps, root_ls_idx, &warn, &err)) {
      return nonstd::make_unexpected(err);
    }
  }

  return std::move(graph);
}

// ---------------------------------------------------------------------------
// ComposePrimSpecFromIndex (free function)
// ---------------------------------------------------------------------------

bool ComposePrimSpecFromIndex(const std::vector<LayerStackEntry> &layer_stacks,
                              const std::vector<std::string> &path_table,
                              const PrimIndex &index, PrimSpec *out,
                              std::string *warn, std::string *err) {
  if (!out) return false;
  (void)warn;

  const auto &strength_order = index.GetStrengthOrder();
  bool first = true;

  for (uint16_t order_idx : strength_order) {
    const CompNode &node = index.GetNode(order_idx);
    if (node.is_culled() || node.is_inert()) continue;
    if (!node.has_specs()) continue;
    if (node.is_payload_deferred()) continue;

    // Look up the PrimSpec this node points to
    if (node.layer_stack_idx == CompNode::kInvalidIndex) continue;
    if (node.layer_stack_idx >= layer_stacks.size()) continue;

    const LayerStackEntry &ls = layer_stacks[node.layer_stack_idx];
    if (!ls.layer) continue;

    const std::string &site_path = path_table[node.site_path_idx];
    const PrimSpec *ps = nullptr;
    std::string find_err;
    Path p(site_path, "");
    if (!ls.layer->find_primspec_at(p, &ps, &find_err) || !ps) continue;

    if (first) {
      // Initialize output from strongest opinion
      *out = *ps;
      first = false;
    } else {
      // Merge weaker opinion into existing (stronger wins)
      // Use the same semantics as InheritPrimSpec: weaker fills gaps
      std::string merge_warn, merge_err;
      // Merge metadata: weaker fills gaps
      out->metas().update_from(ps->metas(), false);

      // Merge properties: weaker fills gaps
      for (const auto &prop_pair : ps->props()) {
        if (out->props().find(prop_pair.first) == out->props().end()) {
          out->props()[prop_pair.first] = prop_pair.second;
        }
      }

      // Specifier resolution (AOUSD 12.2.1)
      if (out->specifier() == Specifier::Over &&
          (ps->specifier() == Specifier::Def ||
           ps->specifier() == Specifier::Class)) {
        out->specifier() = ps->specifier();
      }

      // TypeName from defining specs only (AOUSD 12.2.2)
      if (out->typeName().empty() && !ps->typeName().empty() &&
          (ps->specifier() == Specifier::Def ||
           ps->specifier() == Specifier::Class)) {
        out->typeName() = ps->typeName();
      }
    }
  }

  if (first) {
    // No opinions found at all
    if (err) *err = "No specs found for prim " + index.GetPath().prim_part();
    return false;
  }

  // Children are NOT this function's output: callers compose each child from
  // its own PrimIndex (which resolves the child's arcs) and append it. The
  // strongest opinion was copied wholesale above, so it carried that layer's
  // inline children along uncomposed -- leaving them would emit every child
  // twice, the uncomposed copy first. GetPrimAtPath takes the first name match,
  // so it would return the copy and silently ignore the child's own
  // references/payloads/variants -- e.g. an empty `def Xform "Foo" {}` shadowing
  // the payload-backed composition of /Foo.
  out->children().clear();

  // Set the correct name
  std::string path_str = index.GetPath().prim_part();
  size_t last_slash = path_str.rfind('/');
  if (last_slash != std::string::npos) {
    out->name() = path_str.substr(last_slash + 1);
  }

  return true;
}

// ---------------------------------------------------------------------------
// CompositionGraph::BuildStage
// ---------------------------------------------------------------------------

bool CompositionGraph::BuildStage(Stage *stage, std::string *warn,
                                  std::string *err) const {
  if (!stage) {
    if (err) *err = "stage is nullptr";
    return false;
  }

  // Use the existing LayerToStage pipeline by first composing PrimSpecs
  // from the DAG, building a Layer, then converting to Stage.
  //
  // This ensures we produce the exact same output as the existing pipeline
  // and reuse all the ReconstructPrim infrastructure.

  Layer composed_layer;

  // Copy stage metadata from root layer
  if (_ctx._root_layer) {
    composed_layer.metas() = _ctx._root_layer->metas();
  }

  // Build a parent_path -> direct-children index once, so child lookup during
  // recursive composition is O(1) per parent instead of scanning all of
  // _prim_indices for every prim (previously O(N^2) overall, with a substr
  // allocation per probe; now O(1) via string_view key). The parent of
  // "/A/B/C" is "/A/B"; root-level prims ("/Foo") map to parent "" and are
  // composed by the loop below directly. _prim_indices is unordered, so the
  // per-parent child order here matches the previous full-scan order.
  std::unordered_map<std::string_view,
                     std::vector<std::pair<std::string, const PrimIndex *>>>
      children_by_parent;
  children_by_parent.reserve(_prim_indices.size());
  for (const auto &pair : _prim_indices) {
    const std::string &p = pair.first;
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) continue;
    children_by_parent[std::string_view(p.data(), slash)].push_back({p, pair.second.get()});
  }

  // Recursively compose the direct children of parent_path into parent_ps.
  std::function<bool(const std::string &, PrimSpec &)> compose_children =
      [&](const std::string &parent_path, PrimSpec &parent_ps) -> bool {
    auto cit = children_by_parent.find(parent_path);
    if (cit == children_by_parent.end()) return true;
    for (const auto &child : cit->second) {
      PrimSpec child_ps;
      if (ComposePrimSpecFromIndex(_ctx._layer_stacks, _ctx._path_table,
                                   *child.second, &child_ps, warn, err)) {
        compose_children(child.first, child_ps);
        parent_ps.children().push_back(std::move(child_ps));
      }
    }
    return true;
  };

  // For each root-level prim, compose a PrimSpec from its PrimIndex
  for (const auto &pair : _prim_indices) {
    const std::string &path_str = pair.first;
    const PrimIndex &index = *pair.second;

    // Only process root-level prims here
    // (children are handled recursively by compose_children)
    if (std::count(path_str.begin(), path_str.end(), '/') != 1) continue;

    PrimSpec composed_ps;
    if (!ComposePrimSpecFromIndex(_ctx._layer_stacks, _ctx._path_table, index,
                                  &composed_ps, warn, err)) {
      // Skip prims that couldn't be composed (might just have no specs)
      continue;
    }

    compose_children(path_str, composed_ps);
    composed_layer.add_primspec(composed_ps.name(), composed_ps);
  }

  // Use existing LayerToStage for the final conversion
  // Verify no duplicate sibling names in the composed layer (test-gate gap:
  // every inline child was a duplicate prim on both composition paths, and 171
  // ctests + the whole roundtrip corpus missed it for the entire life of the
  // bug because the roundtrip compares flattened output where identical twins
  // collapse harmlessly, and nothing asserted sibling uniqueness).
  if (!ValidateNoDuplicateSiblingNames(composed_layer, warn, err)) {
    return false;
  }
  return LayerToStage(std::move(composed_layer), stage, warn, err);
}

bool ValidateNoDuplicateSiblingNames(const Layer &layer, std::string *warn,
                                     std::string *err) {
  (void)warn;
  std::function<bool(const PrimSpec &, const std::string &)> check =
      [&](const PrimSpec &ps, const std::string &parent_path) -> bool {
    std::unordered_set<std::string> seen;
    for (const PrimSpec &child : ps.children()) {
      if (!seen.insert(child.name()).second) {
        if (err)
          *err = "Duplicate sibling prim '" + child.name() + "' under '" +
                 parent_path + "'";
        return false;
      }
      if (!check(child,
                 parent_path.empty() ? child.name()
                                     : parent_path + "/" + child.name())) {
        return false;
      }
    }
    return true;
  };
  for (const auto &prim_pair : layer.primspecs()) {
    if (!check(prim_pair.second, prim_pair.first)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Query API
// ---------------------------------------------------------------------------

const PrimIndex *CompositionGraph::GetPrimIndex(const Path &prim_path) const {
  auto it = _prim_indices.find(prim_path.prim_part());
  if (it == _prim_indices.end()) return nullptr;
  return it->second.get();
}

std::vector<Path> CompositionGraph::GetAllPrimPaths() const {
  std::vector<Path> result;
  result.reserve(_prim_indices.size());
  for (const auto &pair : _prim_indices) {
    result.emplace_back(pair.first, "");
  }
  return result;
}

size_t CompositionGraph::GetPrototypeCount() const {
  return _prototypes.size();
}

std::vector<Path> CompositionGraph::GetInstancesForPrototype(
    size_t prototype_idx) const {
  std::vector<Path> result;
  for (const auto &pair : _instance_to_prototype) {
    if (pair.second == static_cast<int>(prototype_idx)) {
      result.emplace_back(pair.first, "");
    }
  }
  return result;
}

const PrimIndex *CompositionGraph::GetPrototypePrimIndex(
    size_t prototype_idx) const {
  if (prototype_idx >= _prototypes.size()) return nullptr;
  return _prototypes[prototype_idx].get();
}

// ---------------------------------------------------------------------------
// Lazy payload API
// ---------------------------------------------------------------------------

nonstd::expected<bool, std::string> CompositionGraph::LoadPayload(
    const Path &prim_path, AssetResolutionResolver &resolver) {
  // Find the deferred payload for this prim
  DeferredPayloadInfo *info = nullptr;
  for (auto &dp : _ctx._deferred_payloads) {
    if (dp.prim_path.prim_part() == prim_path.prim_part()) {
      info = &dp;
      break;
    }
  }

  if (!info) {
    return nonstd::make_unexpected(
        "No deferred payload found for " + prim_path.prim_part());
  }

  // Find the PrimIndex
  auto it = _prim_indices.find(prim_path.prim_part());
  if (it == _prim_indices.end()) {
    return nonstd::make_unexpected(
        "PrimIndex not found for " + prim_path.prim_part());
  }

  PrimIndex &index = *it->second;
  CompNode &node = index._nodes[info->node_idx];

  if (!node.is_payload_deferred()) {
    return nonstd::make_unexpected("Payload is not in deferred state");
  }

  // Load the payload asset
  std::string asset_path_str = info->payload.asset_path.GetAssetPath();
  if (asset_path_str.empty()) {
    return nonstd::make_unexpected("Empty asset path in deferred payload");
  }
  if (!ValidateAndNormalizeAssetPath(asset_path_str, &asset_path_str,
                                     _ctx._options.allow_parent_relative_paths)) {
    return nonstd::make_unexpected("Unsafe asset path in deferred payload");
  }

  std::string old_cwp = resolver.current_working_path();
  if (!info->current_working_path.empty()) {
    resolver.set_current_working_path(info->current_working_path);
  }

  std::string resolved_path = resolver.resolve(asset_path_str);
  if (resolved_path.empty()) {
    if (!old_cwp.empty()) resolver.set_current_working_path(old_cwp);
    return nonstd::make_unexpected(
        "Failed to resolve payload asset: " + asset_path_str);
  }

  Layer pl_layer;
  std::string load_warn, load_err;
  Asset asset;
  if (!resolver.open_asset(resolved_path, asset_path_str, &asset,
                           &load_warn, &load_err)) {
    if (!old_cwp.empty()) resolver.set_current_working_path(old_cwp);
    return nonstd::make_unexpected("Failed to open payload asset: " + load_err);
  }

  if (asset.size() > security_policy::GetMaxAssetReadBytes()) {
    if (!old_cwp.empty()) resolver.set_current_working_path(old_cwp);
    return nonstd::make_unexpected(
        fmt::format("Resolved asset exceeds max bytes ({} > {}).",
                    asset.size(), security_policy::GetMaxAssetReadBytes()));
  }

  if (!LoadLayerFromMemory(asset.data(), asset.size(), resolved_path,
                            &pl_layer, &load_warn, &load_err)) {
    if (!old_cwp.empty()) resolver.set_current_working_path(old_cwp);
    return nonstd::make_unexpected("Failed to parse payload: " + load_err);
  }

  if (!old_cwp.empty()) resolver.set_current_working_path(old_cwp);

  // Store the loaded layer
  auto layer_ptr = std::make_unique<Layer>(std::move(pl_layer));
  const Layer *layer_raw = layer_ptr.get();
  _ctx._loaded_layers.push_back(std::move(layer_ptr));

  Path target_path = info->payload.prim_path;
  if (!target_path.is_valid() || target_path.prim_part().empty()) {
    std::string dp = layer_raw->metas().defaultPrim.str();
    if (!dp.empty()) target_path = Path("/" + dp, "");
  }
  const PrimSpec *pl_root_ps = nullptr;
  std::string find_err;
  if (!target_path.is_valid() ||
      !layer_raw->find_primspec_at(target_path, &pl_root_ps, &find_err) ||
      !pl_root_ps) {
    return nonstd::make_unexpected(
        "Payload target prim not found in " + asset_path_str);
  }
  NamespaceMapping mapping =
      MakeReferenceMapping(target_path, prim_path, false);
  uint16_t map_idx = _ctx.AddMapExpression(mapping, -1);
  uint16_t ls_idx =
      _ctx.AddLayerStack(layer_raw, resolved_path, info->payload.layerOffset);

  // Update the node
  node.layer_stack_idx = ls_idx;
  node.site_path_idx = _ctx.InternPath(target_path.prim_part());
  node.map_expr_idx = map_idx;
  node.flags = (node.flags & ~NodeFlags::PayloadDeferred &
                ~NodeFlags::Inert & ~NodeFlags::Culled) |
               NodeFlags::PayloadLoaded | NodeFlags::HasSpecs;

  {
    PrimIndexBuilder reprocessor(&_ctx, prim_path);
    std::string reprocess_err;
    if (!reprocessor.ReprocessNode(&index, info->node_idx, &reprocess_err)) {
      return nonstd::make_unexpected(reprocess_err);
    }
  }
  EraseDescendantPrimIndices(prim_path.prim_part());
  std::string rebuild_err;
  if (!RebuildDescendantPrimIndices(prim_path.prim_part(), it->second,
                                    &rebuild_err)) {
    return nonstd::make_unexpected(rebuild_err);
  }
  RebuildInstanceRegistry();

  return true;
}

nonstd::expected<bool, std::string> CompositionGraph::UnloadPayload(
    const Path &prim_path) {
  auto it = _prim_indices.find(prim_path.prim_part());
  if (it == _prim_indices.end()) {
    return nonstd::make_unexpected(
        "PrimIndex not found for " + prim_path.prim_part());
  }

  PrimIndex &index = *it->second;

  // Find payload nodes and mark them as deferred
  bool found = false;
  for (auto &node : index._nodes) {
    if (node.arc_type == ArcType::Payload && node.is_payload_loaded()) {
      node.flags = (node.flags & ~NodeFlags::PayloadLoaded &
                    ~NodeFlags::HasSpecs) |
                   NodeFlags::PayloadDeferred;
      node.layer_stack_idx = CompNode::kInvalidIndex;
      found = true;
    }
  }

  if (!found) {
    return nonstd::make_unexpected(
        "No loaded payloads found for " + prim_path.prim_part());
  }

  RecomputeStrengthOrder(index);
  EraseDescendantPrimIndices(prim_path.prim_part());
  std::string rebuild_err;
  if (!RebuildDescendantPrimIndices(prim_path.prim_part(), it->second,
                                    &rebuild_err)) {
    return nonstd::make_unexpected(rebuild_err);
  }
  RebuildInstanceRegistry();

  return true;
}

std::vector<Path> CompositionGraph::GetDeferredPayloadPaths() const {
  std::vector<Path> result;
  for (const auto &dp : _ctx._deferred_payloads) {
    auto it = _prim_indices.find(dp.prim_path.prim_part());
    if (it == _prim_indices.end()) continue;
    const PrimIndex &index = *it->second;
    if (dp.node_idx >= index.GetNodeCount()) continue;
    if (index.GetNode(dp.node_idx).is_payload_deferred()) {
      result.push_back(dp.prim_path);
    }
  }
  return result;
}

bool CompositionGraph::HasDeferredPayload(const Path &prim_path) const {
  for (const auto &dp : _ctx._deferred_payloads) {
    if (dp.prim_path.prim_part() != prim_path.prim_part()) continue;
    auto it = _prim_indices.find(dp.prim_path.prim_part());
    if (it == _prim_indices.end()) continue;
    const PrimIndex &index = *it->second;
    if (dp.node_idx >= index.GetNodeCount()) continue;
    if (index.GetNode(dp.node_idx).is_payload_deferred()) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

size_t CompositionGraph::EstimateMemoryUsage() const {
  size_t total = 0;

  // Path table
  for (const auto &p : _ctx._path_table) {
    total += p.size() + sizeof(std::string);
  }
  total += _ctx._path_intern_map.size() *
           (sizeof(std::string) + sizeof(uint32_t) + 64);

  // Layer stacks
  total += _ctx._layer_stacks.size() * sizeof(LayerStackEntry);

  // Map expressions
  total += _ctx._map_expressions.size() * sizeof(MapExpr);

  // PrimIndices
  for (const auto &pair : _prim_indices) {
    total += pair.first.size();
    total += pair.second->_nodes.size() * sizeof(CompNode);
    total += pair.second->_strength_order.size() * sizeof(uint16_t);
  }

  // Loaded layers (approximate -- don't count the layer content itself
  // as that would double-count)
  total += _ctx._loaded_layers.size() * sizeof(std::unique_ptr<Layer>);

  return total;
}

std::string CompositionGraph::DumpToString() const {
  std::ostringstream ss;
  ss << "CompositionGraph:\n";
  ss << "  PrimIndices: " << _prim_indices.size() << "\n";
  ss << "  Path table: " << _ctx._path_table.size() << " entries\n";
  ss << "  Layer stacks: " << _ctx._layer_stacks.size() << " entries\n";
  ss << "  Map expressions: " << _ctx._map_expressions.size() << " entries\n";
  ss << "  Loaded layers: " << _ctx._loaded_layers.size() << "\n";
  ss << "  Prototypes: " << _prototypes.size() << "\n";
  ss << "  Deferred payloads: " << _ctx._deferred_payloads.size() << "\n";
  ss << "  Est. memory: " << EstimateMemoryUsage() << " bytes\n";
  ss << "\n";

  // Sort prim paths for deterministic output
  std::vector<std::string> sorted_paths;
  for (const auto &pair : _prim_indices) {
    sorted_paths.push_back(pair.first);
  }
  std::sort(sorted_paths.begin(), sorted_paths.end());

  for (const auto &path : sorted_paths) {
    auto it = _prim_indices.find(path);
    if (it != _prim_indices.end()) {
      ss << it->second->DumpToString();
    }
  }

  return ss.str();
}

}  // namespace composition_graph
}  // namespace lightusd
