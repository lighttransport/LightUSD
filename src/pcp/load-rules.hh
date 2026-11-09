// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Payload load rules
//
// An exception-free port of pxr UsdStageLoadRules: per-subtree rules that
// govern whether payloads are included, resolved with ancestor/descendant
// semantics. The pcp Cache layers these on top of its base payload policy
// (CompositionOptions::load_payloads / payload_policy): a path with no
// governing rule falls back to the base policy (Effect::Default).
//
// Rules (one per prim path):
//   All  - include payloads on this prim and all descendants
//   Only - include the payload on this prim but not its descendants
//   None - exclude payloads on this prim and all descendants
//
// Standalone, C++14. Rule sets are small (a handful of user Load/Unload calls),
// so lookups are linear scans for clarity.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {
namespace pcp {

class LoadRules {
 public:
  enum class Rule { All, Only, None };

  // The effective decision for a path. `Default` means no rule governs the
  // path, so the caller should consult its base policy.
  enum class Effect { All, Only, None, Default };

  LoadRules() = default;

  bool empty() const { return rules_.empty(); }
  void Clear() { rules_.clear(); }
  const std::vector<std::pair<std::string, Rule>> &rules() const {
    return rules_;
  }

  // Load `path` and all descendants (replaces any rule at/under `path`).
  void LoadWithDescendants(const std::string &path) {
    ReplaceSubtree(path, Rule::All);
  }
  // Load `path` but not its descendants.
  void LoadWithoutDescendants(const std::string &path) {
    ReplaceSubtree(path, Rule::Only);
  }
  // Unload `path` and all descendants.
  void Unload(const std::string &path) { ReplaceSubtree(path, Rule::None); }

  // Add/replace a single literal rule at `path` (no subtree clearing).
  void AddRule(const std::string &path, Rule rule);

  // Replace the whole rule set.
  void SetRules(std::vector<std::pair<std::string, Rule>> rules) {
    rules_ = std::move(rules);
  }

  // The effective rule for `path` (UsdStageLoadRules::GetEffectiveRuleForPath
  // semantics, with `Default` returned where OpenUSD would imply AllRule purely
  // from the ABSENCE of any governing rule).
  Effect GetEffect(const std::string &path) const;

 private:
  // Drop every rule at `path` or under it, then add (path, rule).
  void ReplaceSubtree(const std::string &path, Rule rule);

  std::vector<std::pair<std::string, Rule>> rules_;
};

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
