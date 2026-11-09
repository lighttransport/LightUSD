// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../layer/prim-spec.hh"
#include <string>

namespace lightusd {
namespace next {

// Shared composition.cc / composition-variants.cc helpers (previously
// file-local in composition.cc's anonymous namespace).
void MergeWeakerDictionary(Value* stronger, const Value& weaker,
                            std::vector<std::string>* conflicts,
                            const std::string& key_prefix);
void MergeWeakerPropMeta(PropMeta* stronger, const PropMeta& weaker);
std::string ResolveWeakerPathExpressionText(const std::string& stronger,
                                            const std::string& weaker);
void ApplyRelationshipEdit(std::vector<Path>* values, const ArcListOp& edit);
bool PrimHasComposableArcs(const PrimSpec& p);
bool PathInSubtree(const std::string& path, const std::string& root,
                   const std::string& root_slash);

}  // namespace next
}  // namespace lightusd
