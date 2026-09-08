// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Collection API and path-expression based light-link resolution.

#include "render-converter-light-linking.hh"
#include "render-data.hh"
#include "core/path-expression-eval.hh"
#include "next/stage/stage.hh"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
using ::lightusd::next::Stage;
using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;
namespace {

bool GetBool(const UsdPrim& prim, const std::string& name, bool* out) {
  const Value* value = prim.GetPropertyValue(name);
  const bool* result = value ? value->as_bool() : nullptr;
  if (!result || !out) return false;
  *out = *result;
  return true;
}

bool GetToken(const UsdPrim& prim, const std::string& name, std::string* out) {
  const Value* value = prim.GetPropertyValue(name);
  if (!value || !out) return false;
  if (const std::string* result = value->as_token()) {
    *out = *result;
    return true;
  }
  if (const std::string* result = value->as_string()) {
    *out = *result;
    return true;
  }
  return false;
}

// Mesh paths sorted lexicographically, built ONCE per ResolveLightLinking call
// and shared by every light and both link kinds.
//
// A prim path and all its descendants form a CONTIGUOUS range in sorted order
// ("/A" then "/A/..." then anything after, since '/' sorts below every
// character that can start a sibling's name), so a collection target resolves
// by two binary searches instead of a full scan with a string prefix compare
// per mesh. The old shape was O(lights * meshes * targets): 500 lights x
// 200k meshes x 10 targets x 2 link kinds is ~2e9 string comparisons.
class MeshPathIndex {
 public:
  explicit MeshPathIndex(const RenderScene& scene) {
    sorted_.reserve(scene.meshes.size());
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
      sorted_.push_back({&scene.meshes[i].prim_path, static_cast<int32_t>(i)});
    }
    std::sort(sorted_.begin(), sorted_.end(),
              [](const Entry& a, const Entry& b) { return *a.path < *b.path; });
    stamp_.assign(scene.meshes.size(), 0);
  }

  size_t mesh_count() const { return sorted_.size(); }

  // Half-open [begin, end) over sorted_ covering the STRICT descendants of
  // `root` (use ExactMatch for `root` itself). Descendants are exactly the
  // entries with the prefix "root/", and since '/' is 0x2F they occupy
  // ["root/", "root0") -- two binary searches, no prefix compare per mesh.
  void DescendantRange(const std::string& root, size_t* begin,
                       size_t* end) const {
    if (root.empty()) { *begin = *end = 0; return; }
    const std::string lo_key = root + "/";
    std::string hi_key = root;
    hi_key += static_cast<char>('/' + 1);  // '0'
    *begin = LowerBound(lo_key);
    *end = LowerBound(hi_key);
    if (*end < *begin) *end = *begin;
  }

  int32_t ExactMatch(const std::string& path) const {
    const size_t i = LowerBound(path);
    if (i < sorted_.size() && *sorted_[i].path == path) {
      return sorted_[i].mesh_index;
    }
    return -1;
  }

  int32_t MeshAt(size_t sorted_pos) const {
    return sorted_[sorted_pos].mesh_index;
  }

  // Generation-stamped marking: no O(meshes) clear between lights.
  void NewGeneration() { ++generation_; }
  void Mark(int32_t mesh_index) {
    stamp_[static_cast<size_t>(mesh_index)] = generation_;
  }
  bool Marked(int32_t mesh_index) const {
    return stamp_[static_cast<size_t>(mesh_index)] == generation_;
  }

 private:
  size_t LowerBound(const std::string& key) const {
    const auto it = std::lower_bound(
        sorted_.begin(), sorted_.end(), key,
        [](const Entry& e, const std::string& v) { return *e.path < v; });
    return static_cast<size_t>(it - sorted_.begin());
  }

  struct Entry {
    const std::string* path;
    int32_t mesh_index;
  };
  std::vector<Entry> sorted_;
  mutable std::vector<uint32_t> stamp_;
  uint32_t generation_ = 0;
};

bool EvalNextPathPredicate(const Stage& stage, const std::string& predicate,
                           const std::string& prim_path) {
  std::string func = predicate;
  std::string arg;
  const size_t paren = func.find('(');
  if (paren != std::string::npos && !func.empty() && func.back() == ')') {
    arg = func.substr(paren + 1, func.size() - paren - 2);
    func.resize(paren);
  } else {
    const size_t colon = func.find(':');
    if (colon != std::string::npos) {
      arg = func.substr(colon + 1);
      func.resize(colon);
    }
  }
  auto trim = [](std::string* text) {
    const size_t first = text->find_first_not_of(" \t");
    if (first == std::string::npos) { text->clear(); return; }
    const size_t last = text->find_last_not_of(" \t");
    *text = text->substr(first, last - first + 1);
  };
  trim(&func);
  trim(&arg);
  if (arg.size() >= 2 &&
      ((arg.front() == '"' && arg.back() == '"') ||
       (arg.front() == '\'' && arg.back() == '\''))) {
    arg = arg.substr(1, arg.size() - 2);
  }

  const UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;
  if (func == "defined") return prim.IsDefined();
  if (func == "abstract") return prim.IsAbstract();
  if (func == "active") return prim.IsActive();
  if (func == "isa") return prim.GetTypeName() == arg;
  const std::string& kind = prim.GetMeta().kind();
  if (func == "kind") return kind == arg;
  if (func == "model") return !kind.empty();
  if (func == "group") return kind == "group" || kind == "assembly";
  if (func == "assembly") return kind == "assembly";
  if (func == "component") return kind == "component";
  if (func == "subcomponent") return kind == "subcomponent";
  if (func == "hasAPI") {
    for (const std::string& schema : prim.GetMeta().apiSchemas()) {
      if (schema == arg) return true;
      const size_t separator = schema.find(':');
      if (separator != std::string::npos &&
          schema.substr(separator + 1) == arg) return true;
    }
  }
  return false;
}

bool ResolveExpressionLightLinks(const Stage& stage, const UsdPrim& owner,
                                 const std::string& property_name,
                                 const RenderScene& scene,
                                 std::vector<int32_t>* mesh_indices) {
  const Value* value = owner.GetPropertyValue(property_name);
  if (!value ||
      value->type_id() != ::lightusd::next::TypeId::PathExpression) return false;
  const std::string* text = value->as_string();
  if (!text) return false;
  ParsedPathExpression expression = ParsedPathExpression::Parse(*text);
  if (!expression.valid()) return false;

  PathExpressionEvalContext context;
  context.eval_predicate = [&stage](const std::string& pred,
                                    const std::string& path) {
    return EvalNextPathPredicate(stage, pred, path);
  };
  auto subexpressions = std::make_shared<std::deque<ParsedPathExpression>>();
  const std::string seed_owner = owner.GetPath().str();
  context.resolve_ref =
      [&stage, subexpressions, seed_owner](const ExpressionReference& ref)
          -> const ParsedPathExpression* {
    if (ref.is_weaker()) return nullptr;
    const std::string owner_path = ref.path.empty() ? seed_owner : ref.path;
    const UsdPrim ref_owner = stage.GetPrimAtPath(owner_path);
    if (!ref_owner.IsValid()) return nullptr;
    const Value* ref_value = ref_owner.GetPropertyValue(
        "collection:" + ref.name + ":membershipExpression");
    if (!ref_value || ref_value->type_id() !=
                          ::lightusd::next::TypeId::PathExpression) return nullptr;
    const std::string* ref_text = ref_value->as_string();
    if (!ref_text) return nullptr;
    std::string qualified = *ref_text;
    const std::string replacement = "%" + owner_path + ":";
    size_t pos = 0;
    while ((pos = qualified.find("%:", pos)) != std::string::npos) {
      qualified.replace(pos, 2, replacement);
      pos += replacement.size();
    }
    subexpressions->push_back(ParsedPathExpression::Parse(qualified));
    return subexpressions->back().valid() ? &subexpressions->back() : nullptr;
  };

  mesh_indices->clear();
  for (size_t i = 0; i < scene.meshes.size(); ++i) {
    if (MatchPath(expression, scene.meshes[i].prim_path, context)) {
      mesh_indices->push_back(static_cast<int32_t>(i));
    }
  }
  return true;
}

// Resolve one CollectionAPI instance (collection:<name>:*) on a light prim
// to RenderScene mesh indices, mirroring legacy ResolveLightLinking:
// excludes take hierarchical precedence, includeRoot adds the light prim's
// subtree, explicitOnly matches exact paths, expandPrims (default) and
// expandPrimsAndProperties match descendants. Unauthored collections keep
// *links_all = true (light affects everything).
void ResolveLightLinkInstance(const Stage& stage, const UsdPrim& prim,
                              const RenderScene& scene,
                              MeshPathIndex& index,
                              const std::string& instance_name,
                              bool* links_all,
                              std::vector<int32_t>* mesh_indices) {
  const std::string base = "collection:" + instance_name + ":";
  if (prim.HasProperty(base + "membershipExpression")) {
    if (ResolveExpressionLightLinks(stage, prim,
                                    base + "membershipExpression", scene,
                                    mesh_indices)) *links_all = false;
    return;
  }

  const std::vector<::lightusd::next::Path>* includes =
      prim.GetRelationship(base + "includes");
  const std::vector<::lightusd::next::Path>* excludes =
      prim.GetRelationship(base + "excludes");
  if (!includes && !excludes) return;  // unauthored -> links all

  bool include_root = false;
  GetBool(prim, base + "includeRoot", &include_root);
  std::string rule = "expandPrims";
  GetToken(prim, base + "expansionRule", &rule);
  const bool explicit_only = (rule == "explicitOnly");
  const std::string& owner_path = prim.GetPath().str();

  *links_all = false;
  mesh_indices->clear();

  // Mark the excluded set first (excludes take hierarchical precedence), then
  // walk only the meshes the includes actually name. Cost is proportional to
  // the matched subtrees, not to the whole scene.
  index.NewGeneration();
  if (excludes) {
    for (const ::lightusd::next::Path& p : *excludes) {
      const std::string ep = p.str();
      const int32_t exact = index.ExactMatch(ep);
      if (exact >= 0) index.Mark(exact);
      size_t b = 0, e = 0;
      index.DescendantRange(ep, &b, &e);
      for (size_t i = b; i < e; ++i) index.Mark(index.MeshAt(i));
    }
  }

  std::vector<int32_t> candidates;
  auto add_subtree = [&](const std::string& root) {
    const int32_t exact = index.ExactMatch(root);
    if (exact >= 0) candidates.push_back(exact);
    size_t b = 0, e = 0;
    index.DescendantRange(root, &b, &e);
    for (size_t i = b; i < e; ++i) candidates.push_back(index.MeshAt(i));
  };

  if (include_root) add_subtree(owner_path);
  if (includes) {
    for (const ::lightusd::next::Path& p : *includes) {
      const std::string ip = p.str();
      if (explicit_only) {
        const int32_t exact = index.ExactMatch(ip);
        if (exact >= 0) candidates.push_back(exact);
      } else {
        add_subtree(ip);
      }
    }
  }

  // Sorting by mesh index preserves the scene order the previous full scan
  // emitted; unique() collapses overlapping include targets.
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  for (int32_t mi : candidates) {
    if (!index.Marked(mi)) mesh_indices->push_back(mi);
  }
}

void ResolveLightLinkingImpl(const Stage& stage, RenderScene* scene) {
  if (!scene) return;
  if (scene->lights.empty()) return;
  // Built once and reused by every light and both link kinds.
  MeshPathIndex index(*scene);
  for (RenderLight& light : scene->lights) {
    UsdPrim prim = stage.GetPrimAtPath(light.prim_path);
    if (!prim.IsValid()) continue;
    ResolveLightLinkInstance(stage, prim, *scene, index, "lightLink",
                             &light.light_links_all,
                             &light.light_link_mesh_indices);
    ResolveLightLinkInstance(stage, prim, *scene, index, "shadowLink",
                             &light.shadow_links_all,
                             &light.shadow_link_mesh_indices);
  }
}


}  // namespace

void ResolveLightLinking(const Stage& stage, RenderScene* scene) {
  ResolveLightLinkingImpl(stage, scene);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
