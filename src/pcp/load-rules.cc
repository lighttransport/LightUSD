// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#include "load-rules.hh"

#include <algorithm>

namespace lightusd {
namespace next {
namespace pcp {

namespace {

// True if `base` is `child` or a namespace ancestor of `child`. "/" is an
// ancestor of everything.
bool IsAncestorOrEqual(const std::string &base, const std::string &child) {
  if (base == child) return true;
  if (base == "/") return true;
  return child.size() > base.size() &&
         child.compare(0, base.size(), base) == 0 && child[base.size()] == '/';
}

// True if `desc` is a strict namespace descendant of `base`.
bool IsStrictDescendant(const std::string &desc, const std::string &base) {
  if (base == "/") return desc.size() > 1 && desc[0] == '/';
  return desc.size() > base.size() &&
         desc.compare(0, base.size(), base) == 0 && desc[base.size()] == '/';
}

}  // namespace

void LoadRules::AddRule(const std::string &path, Rule rule) {
  for (auto &r : rules_) {
    if (r.first == path) {
      r.second = rule;
      return;
    }
  }
  rules_.emplace_back(path, rule);
}

void LoadRules::ReplaceSubtree(const std::string &path, Rule rule) {
  // Erase every rule at `path` or under it (they are subsumed by the new rule),
  // then add the new rule.
  rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                              [&](const std::pair<std::string, Rule> &r) {
                                return r.first == path ||
                                       IsStrictDescendant(r.first, path);
                              }),
               rules_.end());
  rules_.emplace_back(path, rule);
}

LoadRules::Effect LoadRules::GetEffect(const std::string &path) const {
  if (rules_.empty()) return Effect::Default;

  // Find the closest ancestor-or-equal rule (the longest prefix match).
  // Mirrors OpenUSD's GetEffectiveRuleForPath using a linear scan of our
  // unsorted rule vector (rulesets are small: a handful of Load/Unload
  // calls).
  const std::pair<std::string, Rule> *best = nullptr;
  for (const auto &r : rules_) {
    if (IsAncestorOrEqual(r.first, path)) {
      if (!best || r.first.size() > best->first.size()) best = &r;
    }
  }

  if (best) {
    // AllRule on the closest ancestor-or-equal: loads this prim and all
    // descendants unconditionally.
    if (best->second == Rule::All) return Effect::All;

    // OnlyRule on `path` itself: loads this prim but not descendants.
    if (best->first == path && best->second == Rule::Only) return Effect::Only;

    // Closest rule is a None ancestor, or an Only ancestor that is NOT
    // `path` itself. Fall through to the descendant check: a descendant
    // with All/Only can still make path an ancestor of loaded content.
  }

  // Check descendants: if any descendant (at any depth, not just direct
  // children) has All or Only, path is an ancestor of loaded content -> Only.
  for (const auto &r : rules_) {
    if (IsStrictDescendant(r.first, path) &&
        (r.second == Rule::All || r.second == Rule::Only)) {
      return Effect::Only;
    }
  }

  if (best) {
    // Governed by an ancestor rule (None, or Only-not-equal) with no loaded
    // descendant -> this prim's payload is excluded.
    return Effect::None;
  }
  // No ancestor rule and no loaded descendant -> defer to the base policy.
  return Effect::Default;
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
