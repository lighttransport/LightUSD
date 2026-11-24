// SPDX-License-Identifier: Apache-2.0
// Opinion-merge pass (ComposeLayer / ComposePrim / CopyLocalOpinions) of
// Compositor, extracted from composition.cc for compile-time parallelism.

#include "composition.hh"
#include "composition-internal.hh"
#include "layer/array-edit.hh"
#include "layer/arc-reference.hh"
#include "layer/listop-field-table.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lightusd {

// AOUSD §6.6.2 / §12.2: dictionary opinions combine recursively. Keys from
// the stronger value keep their values; missing keys (including nested keys)
// are filled from the weaker value.
void MergeWeakerDictionary(Value* stronger, const Value& weaker,
                           std::vector<std::string>* conflicts = nullptr,
                           const std::string& key_prefix = std::string()) {
  MergeWeakerDictionaryValue(stronger, weaker, conflicts, key_prefix);
}

void MergeWeakerPropMeta(PropMeta* stronger, const PropMeta& weaker) {
  if (!stronger) return;
  auto fill = [&](uint32_t bit, auto* destination, const auto& source) {
    if ((weaker.authored & bit) && !(stronger->authored & bit)) {
      *destination = source;
      stronger->authored |= bit;
    }
  };
  fill(PropMeta::kInterpolation, &stronger->interpolation,
       weaker.interpolation);
  fill(PropMeta::kElementSize, &stronger->elementSize, weaker.elementSize);
  fill(PropMeta::kColorSpace, &stronger->colorSpace, weaker.colorSpace);
  fill(PropMeta::kDisplayName, &stronger->displayName, weaker.displayName);
  fill(PropMeta::kDisplayGroup, &stronger->displayGroup, weaker.displayGroup);
  fill(PropMeta::kDoc, &stronger->doc, weaker.doc);
  fill(PropMeta::kComment, &stronger->comment, weaker.comment);
  fill(PropMeta::kHidden, &stronger->hidden, weaker.hidden);
  fill(PropMeta::kRenderType, &stronger->renderType, weaker.renderType);
  fill(PropMeta::kConnectability, &stronger->connectability,
       weaker.connectability);
  fill(PropMeta::kOutputName, &stronger->outputName, weaker.outputName);
  fill(PropMeta::kBindMaterialAs, &stronger->bindMaterialAs,
       weaker.bindMaterialAs);
  fill(PropMeta::kKind, &stronger->kind, weaker.kind);
  fill(PropMeta::kPermission, &stronger->permission, weaker.permission);
  fill(PropMeta::kRelDefault, &stronger->relDefault, weaker.relDefault);
  fill(PropMeta::kWeight, &stronger->weight, weaker.weight);
  fill(PropMeta::kUnauthoredIdx, &stronger->unauthoredValuesIndex,
       weaker.unauthoredValuesIndex);
  fill(PropMeta::kAllowedTokens, &stronger->allowedTokens,
       weaker.allowedTokens);

  auto merge_dictionary = [&](uint32_t bit, Value* destination,
                              const Value& source) {
    if (!(weaker.authored & bit)) return;
    if (!(stronger->authored & bit)) {
      *destination = source;
      stronger->authored |= bit;
    } else {
      MergeWeakerDictionary(destination, source);
    }
  };
  merge_dictionary(PropMeta::kCustomData, &stronger->customData,
                   weaker.customData);
  merge_dictionary(PropMeta::kAssetInfo, &stronger->assetInfo,
                   weaker.assetInfo);
  merge_dictionary(PropMeta::kSdrMetadata, &stronger->sdrMetadata,
                   weaker.sdrMetadata);

  if (weaker.authored & PropMeta::kUnknownMeta) {
    for (const auto& field : weaker.unknownMeta) {
      const bool present = std::find_if(
          stronger->unknownMeta.begin(), stronger->unknownMeta.end(),
          [&](const auto& own) { return own.first == field.first; }) !=
          stronger->unknownMeta.end();
      if (!present) stronger->unknownMeta.push_back(field);
    }
    stronger->authored |= PropMeta::kUnknownMeta;
  }
  MergeWeakerExtensionFields(&stronger->unknownFields, weaker.unknownFields);
}

std::string ResolveWeakerPathExpressionText(const std::string& stronger,
                                            const std::string& weaker) {
  if (stronger.find("%_") == std::string::npos) return stronger;
  const std::string replacement = weaker.empty()
      ? "(/ & ~/)" : ("(" + weaker + ")");
  std::string out = stronger;
  size_t pos = 0;
  while ((pos = out.find("%_", pos)) != std::string::npos) {
    out.replace(pos, 2, replacement);
    pos += replacement.size();
  }
  return out;
}

void ApplyRelationshipEdit(std::vector<Path>* values, const ArcListOp& edit) {
  if (!values) return;
  auto as_path = [](const std::string& s) { return Path(s); };
  auto erase_items = [&](const std::vector<std::string>& items) {
    values->erase(std::remove_if(values->begin(), values->end(),
                                 [&](const Path& p) {
                                   return std::find(items.begin(), items.end(),
                                                    p.str()) != items.end();
                                 }),
                  values->end());
  };
  auto add_unique = [&](const Path& p, bool front) {
    values->erase(std::remove(values->begin(), values->end(), p),
                  values->end());
    if (front) values->insert(values->begin(), p);
    else values->push_back(p);
  };
  erase_items(edit.deleted);
  // Insert prepended items in reverse so their authored order is retained.
  for (auto it = edit.prepended.rbegin(); it != edit.prepended.rend(); ++it) {
    add_unique(as_path(*it), true);
  }
  for (const std::string& item : edit.added) {
    const Path path = as_path(item);
    if (std::find(values->begin(), values->end(), path) == values->end()) {
      values->push_back(path);
    }
  }
  for (const std::string& item : edit.appended) {
    add_unique(as_path(item), false);
  }
  if (!edit.ordered.empty()) {
    std::vector<Path> ordered;
    ordered.reserve(values->size());
    for (const std::string& item : edit.ordered) {
      Path p(item);
      auto it = std::find(values->begin(), values->end(), p);
      if (it != values->end() &&
          std::find(ordered.begin(), ordered.end(), p) == ordered.end()) {
        ordered.push_back(p);
      }
    }
    for (const Path& p : *values) {
      if (std::find(ordered.begin(), ordered.end(), p) == ordered.end()) {
        ordered.push_back(p);
      }
    }
    *values = std::move(ordered);
  }
}

// Clone the prim at `root_path` and its descendants into a fresh layer (paths
// kept; `root_path` prim is the layer root). Used to compose a self-contained
// referenced subtree alone. child_indices on the clones reference the source
// layer and are stale, but composition (flat iteration + path lookups, with
// GraftSubtree validating / falling back to a path scan) does not rely on them
// — the same way Compose()'s own output carries stale child_indices.
// NOTE: the former linear-scan ExtractSubtree() is gone; the subtree is now
// cloned from the per-layer sorted path range (Compositor::ExtractSubtreeIndexed),
// so pulling K prims out of an L-prim library is no longer O(K*L).

// ============================================================
// Compositor
// ============================================================



}  // namespace lightusd
