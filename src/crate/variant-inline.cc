// SPDX-License-Identifier: Apache-2.0
#include "variant-holders.hh"
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace {
bool IsHolder(const std::string& path) {
  return path.find("/{") != std::string::npos;
}

// Copy a path-indexed subset, reconstructing links rather than copying indices
// from the source layer. Retain authored child order when those links exist.
Layer CopySubset(const Layer& source, const std::string& prefix, PrimSpec* self) {
  Layer out;
  std::map<uint32_t, uint32_t> indices;
  if (self) out.add_root(out.add_prim(std::move(*self)));
  for (size_t i = 0; i < source.prim_count(); ++i) {
    const auto* prim = source.prim(static_cast<uint32_t>(i));
    const std::string& path = prim->path().str();
    std::string relative;
    if (!prefix.empty()) {
      if (path.compare(0, prefix.size() + 1, prefix + "/") != 0) continue;
      relative = path.substr(prefix.size());
      if (relative.find("/{") != std::string::npos) continue;
    } else if (IsHolder(path)) continue;
    PrimSpec copy = prim->Clone();
    copy.clear_child_indices();
    if (!prefix.empty()) copy.set_path(Path("/__self__" + relative));
    indices.emplace(static_cast<uint32_t>(i), out.add_prim(std::move(copy)));
  }
  out.build_path_index();
  for (const auto& item : indices) {
    const auto* prim = out.prim(item.second);
    const std::string& path = prim->path().str();
    const size_t slash = path.find_last_of('/');
    const uint32_t parent = slash == 0 ? UINT32_MAX : out.index_at_path(path.substr(0, slash));
    if (parent != UINT32_MAX) out.set_parent(item.second, parent);
    else out.add_root(item.second);
  }
  // The source's sibling order is authored data, independent of storage order.
  for (const auto& item : indices) {
    std::vector<uint32_t> ordered;
    for (uint32_t child : source.prim(item.first)->child_indices()) {
      const auto found = indices.find(child);
      if (found != indices.end()) ordered.push_back(found->second);
    }
    if (!ordered.empty()) out.prim_mutable(item.second)->set_child_indices(std::move(ordered));
  }
  if (self) {
    std::vector<uint32_t> ordered;
    if (const PrimSpec* holder = source.prim_at_path(prefix)) {
      for (uint32_t child : holder->child_indices()) {
        const auto found = indices.find(child);
        if (found != indices.end()) ordered.push_back(found->second);
      }
    }
    if (!ordered.empty()) out.prim_mutable(0)->set_child_indices(std::move(ordered));
  }
  if (prefix.empty()) {
    std::vector<uint32_t> roots;
    for (uint32_t root : source.root_indices()) {
      const auto found = indices.find(root);
      if (found != indices.end()) roots.push_back(found->second);
    }
    out.set_root_indices(std::move(roots));
    out.meta() = source.meta();
    out.set_source_asset_anchor_id(source.source_asset_anchor_id());
  }
  out.finalize();
  return out;
}
}

void InlineVariantHolders(Layer* layer) {
  if (!layer) return;
  std::vector<std::string> holders;
  for (const auto& prim : layer->prims()) {
    const std::string& path = prim.path().str();
    if (!path.empty() && path.back() == '}' && IsHolder(path)) holders.push_back(path);
  }
  if (holders.empty()) return;
  // Nested option metadata must be complete before copying its owning holder.
  std::stable_sort(holders.begin(), holders.end(), [](const std::string& a, const std::string& b) {
    return a.size() > b.size();
  });
  for (const std::string& path : holders) {
    const size_t slash = path.find_last_of('/');
    const std::string name = path.substr(slash + 2, path.size() - slash - 3);
    const size_t equal = name.find('=');
    if (equal == std::string::npos || equal + 1 == name.size()) continue;
    const PrimSpec* holder = layer->prim_at_path(path);
    PrimSpec* owner = layer->prim_at_path_mutable(path.substr(0, slash));
    if (!holder || !owner) continue;
    auto& sets = owner->meta().variantSets();
    auto set = std::find_if(sets.begin(), sets.end(), [&](const PrimVariantSetData& data) {
      return data.name == name.substr(0, equal);
    });
    if (set == sets.end()) {
      sets.emplace_back();
      set = sets.end() - 1;
      set->name = name.substr(0, equal);
    }
    auto option = std::find_if(set->variants.begin(), set->variants.end(), [&](const PrimVariantData& data) {
      return data.name == name.substr(equal + 1);
    });
    if (option == set->variants.end()) {
      set->variants.emplace_back();
      option = set->variants.end() - 1;
    }
    PrimVariantData value;
    value.name = name.substr(equal + 1);
    const PrimMetadata& meta = holder->meta();
    value.active = meta.active;
    value.active_authored = meta.active_authored;
    value.hidden = meta.hidden;
    value.hidden_authored = meta.hidden_authored;
    value.doc = meta.doc();
    value.kind = meta.kind();
    value.references = meta.references.get();
    value.payloads = meta.payloads.get();
    value.inherits = meta.inherits.get();
    value.specializes = meta.specializes.get();
    value.variantSelections = meta.variantSelections();
    value.variantSets = meta.variantSets();
    value.variantSetNameEdits = meta.variantSetNameEdits();
    value.unknownMeta = meta.unknownMeta();
    value.unknownFields = meta.unknownFields();
    value.authoredPropertyNames = holder->authored_property_names();

    PrimSpec self = holder->Clone();
    self.set_name("__self__");
    self.set_path(Path("/__self__"));
    self.clear_child_indices();
    auto& sm = self.meta();
    sm.active = true; sm.active_authored = false;
    sm.hidden = false; sm.hidden_authored = false;
    sm.doc().clear(); sm.set_doc_authored(false);
    sm.kind().clear(); sm.setKindAuthored(false);
    sm.references.clear(); sm.payloads.clear(); sm.inherits.clear(); sm.specializes.clear();
    // Keep structured arc edits on the content root until the option model
    // can carry them directly; composition and crate rewriting must retain them.
    sm.variantSelections().clear(); sm.setVariantSelectionsAuthored(false);
    sm.variantSelection.clear(); sm.variantSets().clear();
    sm.variantSetNameEdits() = StringListOps();
    sm.unknownMeta().clear(); sm.unknownFields().clear();
    for (const PropSlot& slot : holder->properties().slots()) {
      const std::string prop(GetPropNameTable().get(slot.name_id));
      if (slot.is_relationship()) continue;
      if (!holder->property_meta(slot.name_id) && !holder->has_time_samples(slot.name_id) &&
                 !holder->connection(prop)) {
        if (const Value* property = holder->property_value(slot.name_id)) {
          value.properties.emplace_back(prop, *property, slot.flags);
          self.remove_property(prop);
        }
      }
    }
    // Relationships live in a separate table and need not have PropSlots.
    for (const std::string& prop : holder->relationship_names()) {
      const PropMeta* property_meta = holder->property_meta(prop);
      if (property_meta && (property_meta->authored || !property_meta->unknownMeta.empty() ||
                            !property_meta->unknownFields.empty())) continue;
      const auto edit = holder->relationship_edits().find(prop);
      if (edit != holder->relationship_edits().end() &&
          (!edit->second.is_explicit || edit->second.is_value_block)) continue;
      if (const auto* targets = holder->relationship(prop)) value.relationships[prop] = *targets;
      const uint16_t flags = holder->relationship_flags(prop);
      if (flags) value.relationshipFlags[prop] = flags;
      self.remove_relationship(prop);
    }
    sm.trim_ext();
    Layer content = CopySubset(*layer, path, &self);
    const PrimSpec* root = content.prim_at_path("/__self__");
    if (content.prim_count() > 1 || (root &&
        (!root->properties().slots().empty() || !root->relationship_names().empty() ||
         !root->type_name().empty() || root->meta().instanceable_authored ||
         root->meta().has_ext())))
      value.content = std::make_shared<Layer>(std::move(content));
    *option = std::move(value);
  }
  *layer = CopySubset(*layer, "", nullptr);
}
}
