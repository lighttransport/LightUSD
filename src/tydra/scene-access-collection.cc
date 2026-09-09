// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "core/path-expression.hh"
#include "core/path-expression-eval.hh"
#include "core/prim.hh"
#include "pprint-enum.hh"
#include "scene-access.hh"
#include "usdPhysics.hh"

#include <deque>
#include <memory>
#include <string>

namespace lightusd {
namespace tydra {

namespace {

// Split a predicate "func(arg)" or "func:arg" or "func" into (func, arg).
void SplitPredicate(const std::string &pred, std::string *func,
                    std::string *arg) {
  std::string s = pred;
  // trim
  auto l = s.find_first_not_of(" \t");
  auto r = s.find_last_not_of(" \t");
  if (l == std::string::npos) {
    *func = "";
    *arg = "";
    return;
  }
  s = s.substr(l, r - l + 1);

  auto paren = s.find('(');
  if (paren != std::string::npos && s.back() == ')') {
    *func = s.substr(0, paren);
    *arg = s.substr(paren + 1, s.size() - paren - 2);
  } else {
    auto colon = s.find(':');
    if (colon != std::string::npos) {
      *func = s.substr(0, colon);
      *arg = s.substr(colon + 1);
    } else {
      *func = s;
      *arg = "";
    }
  }
  // Strip surrounding quotes from arg.
  if (arg->size() >= 2 && ((arg->front() == '"' && arg->back() == '"') ||
                           (arg->front() == '\'' && arg->back() == '\''))) {
    *arg = arg->substr(1, arg->size() - 2);
  }
  // trim func/arg
  auto trim = [](std::string &t) {
    auto a = t.find_first_not_of(" \t");
    auto b = t.find_last_not_of(" \t");
    if (a == std::string::npos) {
      t.clear();
    } else {
      t = t.substr(a, b - a + 1);
    }
  };
  trim(*func);
  trim(*arg);
}

}  // namespace

bool EvalPathExpressionPredicate(const Stage &stage,
                                 const std::string &predicate,
                                 const std::string &prim_path) {
  std::string func, arg;
  SplitPredicate(predicate, &func, &arg);
  if (func.empty()) {
    return false;
  }

  Path p(prim_path, "");
  if (!p.is_valid()) {
    return false;
  }
  auto pret = stage.GetPrimAtPath(p);
  if (!pret) {
    return false;
  }
  const Prim *prim = pret.value();
  if (!prim) {
    return false;
  }

  const auto &metas = prim->metas();

  if (func == "defined") {
    return prim->specifier() == Specifier::Def;
  } else if (func == "abstract") {
    return prim->IsAbstract();
  } else if (func == "model") {
    return prim->IsModel();
  } else if (func == "group") {
    Kind k = metas.get_kind_enum();
    return k == Kind::Group || k == Kind::Assembly;
  } else if (func == "assembly") {
    return metas.get_kind_enum() == Kind::Assembly;
  } else if (func == "component") {
    return metas.get_kind_enum() == Kind::Component;
  } else if (func == "subcomponent") {
    return metas.get_kind_enum() == Kind::Subcomponent;
  } else if (func == "active") {
    return metas.has_active() ? metas.get_active() : true;
  } else if (func == "kind") {
    return metas.get_kind_str() == arg;
  } else if (func == "isa") {
    // Approximation: exact schema type name match.
    return prim->type_name() == arg;
  } else if (func == "hasAPI") {
    const APISchemas sc = metas.get_apiSchemas();
    for (const auto &n : sc.names) {
      if (to_string(n.first) == arg) return true;
      if (!n.second.empty() && n.second == arg) return true;  // instance name
    }
    for (const auto &n : sc.unknownSchemas) {
      if (n.first == arg) return true;
    }
    return false;
  }

  // Unknown predicate: be conservative and do not match.
  return false;
}

namespace {

// Hierarchical prefix membership: is `qpath` equal to or a descendant of `base`?
bool PathIsAtOrUnder(const std::string &qpath, const std::string &base) {
  if (base.empty()) return false;
  if (qpath == base) return true;
  if (qpath.size() > base.size() &&
      qpath.compare(0, base.size(), base) == 0 && qpath[base.size()] == '/') {
    return true;
  }
  return false;
}

}  // namespace

bool IsPathIncluded(const CollectionMembershipQuery &query, const Stage &stage,
                    const Path &abs_path,
                    const CollectionInstance::ExpansionRule expansionRule) {
  (void)expansionRule;  // query carries its own expansion_rule

  if (!query.valid() || !abs_path.is_valid()) {
    return false;
  }

  const std::string qpath = abs_path.prim_part();

  if (query.mode == CollectionMembershipQuery::Mode::Expression) {
    PathExpressionEvalContext ctx;
    ctx.eval_predicate = [&stage](const std::string &pred,
                                  const std::string &prim_path) {
      return EvalPathExpressionPredicate(stage, pred, prim_path);
    };
    // Resolve `%refs` to other collections' membershipExpressions on the Stage.
    // Holds parsed sub-expressions alive for the duration of the match. A deque
    // is used so element addresses stay valid as more refs are resolved during
    // nested evaluation (vector reallocation would dangle the sub-evaluator's
    // reference).
    auto subexprs = std::make_shared<std::deque<ParsedPathExpression>>();
    const std::string expression_owner = query.owner_prim_path;
    ctx.resolve_ref =
        [&stage, subexprs, expression_owner](
            const ExpressionReference &ref) -> const ParsedPathExpression * {
      // Composition resolves `%_` while both opinions are available. A
      // surviving weaker ref therefore means there was no weaker authored
      // expression and correctly evaluates as the empty set.
      if (ref.is_weaker()) return nullptr;
      const std::string owner_path = ref.path.empty() ? expression_owner : ref.path;
      Path owner(owner_path, "");
      if (!owner.is_valid()) return nullptr;
      auto pret = stage.GetPrimAtPath(owner);
      if (!pret || !pret.value()) return nullptr;
      const Collection *coll = nullptr;
      if (!GetCollection(*pret.value(), &coll) || !coll) return nullptr;
      const CollectionInstance *inst = nullptr;
      if (!coll->get_instance(ref.name, &inst) || !inst) return nullptr;
      if (!inst->has_membershipExpression()) return nullptr;
      value::PathExpression expr;
      if (!inst->membershipExpression.get_value(&expr)) return nullptr;
      std::string text = expr.GetText();
      // The evaluator callback does not carry the currently evaluated
      // reference's owner. Qualify same-scope refs before parsing a nested
      // expression so `%:base` inside `%/Sets:alias` remains relative to
      // `/Sets`, not to the original seed collection.
      const std::string qualified = "%" + owner_path + ":";
      size_t pos = 0;
      while ((pos = text.find("%:", pos)) != std::string::npos) {
        text.replace(pos, 2, qualified);
        pos += qualified.size();
      }
      subexprs->push_back(ParsedPathExpression::Parse(text));
      return &subexprs->back();
    };
    return MatchPath(query.membership_expression, qpath, ctx);
  }

  // Relationship mode: excludes take precedence over includes (hierarchical).
  for (const auto &ep : query.excludes) {
    if (PathIsAtOrUnder(qpath, ep.prim_part())) {
      return false;
    }
  }
  if (query.include_root && PathIsAtOrUnder(qpath, query.owner_prim_path)) {
    return true;
  }
  for (const auto &ip : query.includes) {
    if (query.expansion_rule ==
        CollectionInstance::ExpansionRule::ExplicitOnly) {
      if (qpath == ip.prim_part()) return true;
    } else {
      if (PathIsAtOrUnder(qpath, ip.prim_part())) return true;
    }
  }
  return false;
}

CollectionMembershipQuery BuildCollectionMembershipQuery(
    const Stage &stage, const CollectionInstance &seedCollectionInstance,
    const std::string &owner_prim_path) {
  (void)stage;
  CollectionMembershipQuery q;
  q.owner_prim_path = owner_prim_path;

  if (seedCollectionInstance.has_membershipExpression()) {
    value::PathExpression expr;
    if (seedCollectionInstance.membershipExpression.get_value(&expr)) {
      q.membership_expression = ParsedPathExpression::Parse(expr.GetText());
      if (q.membership_expression.valid()) {
        q.mode = CollectionMembershipQuery::Mode::Expression;
        return q;
      }
    }
  }

  // Relationship mode.
  q.mode = CollectionMembershipQuery::Mode::Relationship;

  {
    const auto &er = seedCollectionInstance.expansionRule.get_value();
    q.expansion_rule = er;
  }
  {
    const auto &ir = seedCollectionInstance.includeRoot.get_value();
    if (ir.has_default()) {
      q.include_root = ir.get_scalar_ref();
    }
  }
  if (seedCollectionInstance.includes.authored()) {
    const auto &rel = seedCollectionInstance.includes.relationship();
    if (rel.is_path()) {
      q.includes.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      for (const auto &t : rel.targetPathVector) q.includes.push_back(t);
    }
  }
  if (seedCollectionInstance.excludes.authored()) {
    const auto &rel = seedCollectionInstance.excludes.relationship();
    if (rel.is_path()) {
      q.excludes.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      for (const auto &t : rel.targetPathVector) q.excludes.push_back(t);
    }
  }

  return q;
}

}  // namespace tydra
}  // namespace lightusd
