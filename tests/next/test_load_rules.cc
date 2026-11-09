// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Unit tests for pcp::LoadRules (UsdStageLoadRules semantics port).

#include <cassert>
#include <iostream>

#include "pcp/load-rules.hh"

using namespace lightusd::next::pcp;
using R = LoadRules::Rule;
using E = LoadRules::Effect;

int main() {
  // Empty rules: every path is Default (caller falls back to base policy).
  {
    LoadRules lr;
    assert(lr.empty());
    assert(lr.GetEffect("/World") == E::Default);
  }

  // LoadWithDescendants: the path and everything under it is All.
  {
    LoadRules lr;
    lr.LoadWithDescendants("/World/sets");
    assert(lr.GetEffect("/World/sets") == E::All);
    assert(lr.GetEffect("/World/sets/kitchen") == E::All);
    assert(lr.GetEffect("/World/sets/kitchen/table") == E::All);
    // A sibling/ancestor with no rule: /World is an ancestor of a loaded prim
    // -> Only (part of the ancestor chain); an unrelated path -> Default.
    assert(lr.GetEffect("/World") == E::Only);
    assert(lr.GetEffect("/Other") == E::Default);
  }

  // LoadWithoutDescendants: the path is Only, descendants are None.
  {
    LoadRules lr;
    lr.LoadWithoutDescendants("/World/sets");
    assert(lr.GetEffect("/World/sets") == E::Only);
    assert(lr.GetEffect("/World/sets/kitchen") == E::None);
  }

  // OpenUSD's worked example: LoadWithDescendants('/World/sets') then
  // LoadWithoutDescendants('/World/sets/kitchen') -> everything under
  // /World/sets loads EXCEPT under /World/sets/kitchen.
  {
    LoadRules lr;
    lr.LoadWithDescendants("/World/sets");
    lr.LoadWithoutDescendants("/World/sets/kitchen");
    assert(lr.GetEffect("/World/sets") == E::All);
    assert(lr.GetEffect("/World/sets/bedroom") == E::All);
    assert(lr.GetEffect("/World/sets/kitchen") == E::Only);
    assert(lr.GetEffect("/World/sets/kitchen/table") == E::None);
  }

  // Unload under a loaded subtree carves a hole.
  {
    LoadRules lr;
    lr.LoadWithDescendants("/World");
    lr.Unload("/World/sets/kitchen");
    assert(lr.GetEffect("/World") == E::All);
    assert(lr.GetEffect("/World/sets/kitchen") == E::None);
    assert(lr.GetEffect("/World/sets/kitchen/table") == E::None);
    // An ancestor of the unloaded hole is still loaded.
    assert(lr.GetEffect("/World/sets") == E::All);
  }

  // Load under an unloaded subtree: the ancestor chain to the loaded prim is
  // Only; the loaded prim is All.
  {
    LoadRules lr;
    lr.Unload("/A");
    lr.LoadWithDescendants("/A/B/C");
    assert(lr.GetEffect("/A") == E::Only);      // ancestor of loaded C
    assert(lr.GetEffect("/A/B") == E::Only);    // ancestor of loaded C
    assert(lr.GetEffect("/A/B/C") == E::All);
    assert(lr.GetEffect("/A/X") == E::None);    // under the None, not on the chain
  }

  // ReplaceSubtree drops subsumed rules: Load(/A/B) after Unload(/A/B/C)
  // removes the inner rule.
  {
    LoadRules lr;
    lr.Unload("/A/B/C");
    lr.LoadWithDescendants("/A/B");
    assert(lr.GetEffect("/A/B/C") == E::All);   // inner Unload was replaced
    assert(lr.rules().size() == 1);
  }

  std::cout << "All next/pcp LoadRules tests passed." << std::endl;
  return 0;
}
