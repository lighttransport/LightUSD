// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "usd-semantics.hh"
#include "layer/prim-spec.hh"

#include "eval/attribute-eval.hh"
#include "prim/identifier.hh"
#include "layer/property-index.hh"

#include <algorithm>

namespace lightusd {

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
  auto labels = value.value.as_token_array();
  if (!labels) return false;
  out->assign(labels->begin(), labels->end());
  return true;
}

namespace {

std::vector<std::string> SortedUnique(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void LabelsOf(const Stage& stage, const UsdPrim& prim, const std::string& taxonomy, double t0,
              bool interval, double t1, std::vector<std::string>* out) {
  if (!HasSemanticsLabelsAPI(prim, taxonomy)) return;
  std::vector<double> times = {t0};
  if (interval) {
    const PropNameId id = GetPropNameTable().find("semantics:labels:" + taxonomy);
    if (id.is_valid()) {
      if (const auto* samples = prim.GetTimeSamples(id)) {
        for (const auto& s : *samples)
          if (s.first >= t0 && s.first <= t1) times.push_back(s.first);
      }
    }
  }
  for (double t : times) {
    std::vector<std::string> labels;
    if (GetSemanticsLabels(stage, prim, taxonomy, &labels, t))
      out->insert(out->end(), labels.begin(), labels.end());
  }
}

}  // namespace

std::vector<std::string> ComputeInheritedTaxonomies(const Stage& stage, const UsdPrim& prim) {
  std::vector<std::string> out;
  static const std::string prefix = "SemanticsLabelsAPI:";
  for (UsdPrim p = prim; p.IsValid(); p = p.GetParent()) {
    for (const std::string& applied : p.GetMeta().apiSchemas()) {
      if (applied.compare(0, prefix.size(), prefix) == 0 && applied.size() > prefix.size())
        out.push_back(applied.substr(prefix.size()));
    }
  }
  (void)stage;
  return SortedUnique(std::move(out));
}

std::vector<std::string> ComputeUniqueSemanticsLabels(const Stage& stage, const UsdPrim& prim,
                                                      const std::string& taxonomy,
                                                      bool inherited, double t0, bool interval,
                                                      double t1) {
  std::vector<std::string> out;
  for (UsdPrim p = prim; p.IsValid(); p = p.GetParent()) {
    LabelsOf(stage, p, taxonomy, t0, interval, t1, &out);
    if (!inherited) break;
  }
  return SortedUnique(std::move(out));
}

}  // namespace lightusd

