// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "usd-semantics.hh"
#include "../layer/prim-spec.hh"

#include "../eval/attribute-eval.hh"
#include "../prim/identifier.hh"

namespace lightusd {
namespace next {

bool HasSemanticsLabelsAPI(const UsdPrim& prim,
                           const std::string& instance_name) {
  if (!prim.IsValid() ||
      !IsValidNamespacedIdentifier(instance_name)) return false;
  const std::string schema = "SemanticsLabelsAPI:" + instance_name;
  for (const std::string& applied : prim.GetMeta().apiSchemas()) {
    if (applied == schema) return true;
  }
  return false;
}

bool GetSemanticsLabels(const Stage& stage, const UsdPrim& prim,
                        const std::string& instance_name,
                        std::vector<std::string>* out, double time) {
  if (!out || !HasSemanticsLabelsAPI(prim, instance_name)) return false;
  out->clear();
  AttributeEval eval(&stage);
  eval.SetTime(time);
  const EvalResult value = eval.Eval(prim, "semantics:labels:" + instance_name);
  if (!value.success) return true;
  const std::vector<std::string>* labels = value.value.as_token_array();
  if (!labels) return false;
  *out = *labels;
  return true;
}

}  // namespace next
}  // namespace lightusd
