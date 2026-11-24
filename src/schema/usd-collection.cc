// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdCollectionAPI membership (see usd-collection.hh)

#include "usd-collection.hh"

#include <map>
#include <set>
#include <vector>

#include "stage/stage.hh"

namespace lightusd {
namespace {

struct Rules {
  // path -> true (include) / false (exclude); later (more local) writes win.
  std::map<std::string, bool> entries;
  bool explicit_only = false;
  bool authored = false;
};

// Flattens a collection and the collections it includes (UsdCollectionAPI
// ComputeMembershipQuery): nested includes and excludes are added first,
// then this collection's own excludes, so they take precedence.
bool Gather(const Stage& stage, const UsdPrim& owner, const std::string& name,
            std::set<std::string>* visiting, Rules* rules, bool top) {
  const std::string key = owner.GetPath().str() + ".collection:" + name;
  if (!visiting->insert(key).second) return false;  // cycle
  const std::string base = "collection:" + name + ":";
  const std::vector<Path>* includes = owner.GetRelationship(base + "includes");
  const std::vector<Path>* excludes = owner.GetRelationship(base + "excludes");
  bool include_root = false;
  if (const Value* v = owner.GetPropertyValue(base + "includeRoot")) {
    if (const bool* b = v->as_bool()) include_root = *b;
  }
  if (top) {
    std::string rule = "expandPrims";
    if (const Value* v = owner.GetPropertyValue(base + "expansionRule")) {
      const StringView t = v->as_token();
      if (t.size()) rule.assign(t.data(), t.size());
    }
    rules->explicit_only = rule == "explicitOnly";
  }
  if (includes || excludes || include_root) rules->authored = true;
  if (include_root) rules->entries["/"] = true;
  if (includes) {
    for (const Path& p : *includes) {
      const std::string s = p.str();
      const size_t at = s.find(".collection:");
      if (at != std::string::npos) {
        const UsdPrim nested = stage.GetPrimAtPath(s.substr(0, at));
        if (!nested.IsValid() ||
            !Gather(stage, nested, s.substr(at + 12), visiting, rules, false))
          return false;
      } else {
        rules->entries[s] = true;
      }
    }
  }
  if (excludes) {
    for (const Path& p : *excludes) rules->entries[p.str()] = false;
  }
  visiting->erase(key);
  return true;
}

}  // namespace

bool CollectionMembershipQuery::Compute(const Stage& stage, const UsdPrim& owner,
                                        const std::string& name) {
  entries_.clear();
  explicit_only_ = authored_ = false;
  if (!owner.IsValid()) return false;
  Rules rules;
  std::set<std::string> visiting;
  if (!Gather(stage, owner, name, &visiting, &rules, true)) return false;
  entries_ = std::move(rules.entries);
  explicit_only_ = rules.explicit_only;
  authored_ = rules.authored;
  return true;
}

bool CollectionMembershipQuery::IsPathIncluded(const std::string& path) const {
  if (explicit_only_) {
    const auto it = entries_.find(path);
    return it != entries_.end() && it->second;
  }
  // Nearest ancestor-or-self entry decides; walk up the path's prefixes.
  std::string probe = path;
  while (true) {
    const auto it = entries_.find(probe);
    if (it != entries_.end()) return it->second;
    if (probe == "/" || probe.empty()) return false;
    const size_t cut = probe.find_last_of("/.");
    if (cut == std::string::npos) return false;
    probe = cut == 0 ? std::string("/") : probe.substr(0, cut);
  }
}

bool IsPathInCollection(const Stage& stage, const UsdPrim& owner, const std::string& name,
                        const std::string& path, bool* authored) {
  CollectionMembershipQuery query;
  const bool ok = query.Compute(stage, owner, name);
  if (authored) *authored = query.authored();
  return ok && query.IsPathIncluded(path);
}

}  // namespace lightusd
