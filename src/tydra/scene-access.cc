// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

// src
#include "common-macros.inc"
#include "pprint-enum.hh"
#include "core/prim.hh"
#include "primvar.hh"
#include "tiny-container.hh"
#include "tiny-format.hh"
#include "tydra/prim-apply.hh"
#include "usdGeom.hh"
#include "xform.hh"  // For matrix inverse

// src/tydra
#include "scene-access.hh"


#if defined(__clang__)
#pragma clang diagnostic push
#if __has_warning("-Wnrvo")
#pragma clang diagnostic ignored "-Wnrvo"
#endif
#endif

namespace lightusd {
namespace tydra {

#include "tydra/scene-access-traverse-impl.inc"

const Prim *GetParentPrim(const lightusd::Stage &stage,
                          const lightusd::Path &path, std::string *err) {
  if (!path.is_valid()) {
    if (err) {
      (*err) = "Input Path " + lightusd::to_string(path) + " is invalid.\n";
    }
    return nullptr;
  }

  if (path.is_root_path()) {
    if (err) {
      (*err) = "Input Path is root(\"/\").\n";
    }
    return nullptr;
  }

  if (path.is_root_prim()) {
    if (err) {
      (*err) = "Input Path is root Prim, so no parent Prim exists.\n";
    }
    return nullptr;
  }

  if (!path.is_absolute_path()) {
    if (err) {
      (*err) = "Input Path must be absolute path(i.e. starts with \"/\").\n";
    }
    return nullptr;
  }

  lightusd::Path parentPath = path.get_parent_prim_path();

  nonstd::expected<const Prim *, std::string> ret =
      stage.GetPrimAtPath(parentPath);
  if (ret) {
    return ret.value();
  } else {
    if (err) {
      (*err) += "Failed to get parent Prim from Path " +
                lightusd::to_string(path) + ". Reason = " + ret.error() + "\n";
    }
    return nullptr;
  }
}

//
// Template Instanciations
//


namespace {

// Optimized iterative version of VisitPrimsRec
// Handles primChildren ordering and early termination
bool VisitPrimsIterative(const lightusd::Path &start_abs_path,
                         const lightusd::Prim &start_prim, int32_t start_level,
                         VisitPrimFunction visitor_fun, void *userdata,
                         std::string *err,
                         size_t max_iter = kMaxDefaultTraversalLimit) {
  // Stack entry: (prim pointer, ordered children to visit, current child index, level, parent path)
  struct StackEntry {
    const lightusd::Prim *prim;
    std::vector<const lightusd::Prim *> ordered_children;
    size_t child_idx;
    int32_t level;
    lightusd::Path abs_path;

    StackEntry(const lightusd::Prim *p, int32_t lvl, lightusd::Path path)
        : prim(p), child_idx(0), level(lvl), abs_path(std::move(path)) {}
  };

  StackVector<StackEntry, 4> stack;
  stack.reserve(64);

  // Helper to get ordered children list
  auto get_ordered_children = [err](const lightusd::Prim &prim)
      -> std::pair<bool, std::vector<const lightusd::Prim *>> {
    std::vector<const lightusd::Prim *> result;

    if (prim.children().empty()) {
      return {true, result};
    }

    // If primChildren metadata matches children count, use it for ordering
    if (prim.metas().primChildren.size() == prim.children().size()) {
      std::unordered_map<std::string, const lightusd::Prim *, FNV1StringHash>
          primNameTable;
      primNameTable.reserve(prim.children().size());
      for (size_t i = 0; i < prim.children().size(); i++) {
        primNameTable.emplace(prim.children()[i].element_name(),
                              &prim.children()[i]);
      }

      for (size_t i = 0; i < prim.metas().primChildren.size(); i++) {
        value::token nameTok = prim.metas().primChildren[i];
        const auto it = primNameTable.find(nameTok.str());
        if (it != primNameTable.end()) {
          result.push_back(it->second);
        } else {
          if (err) {
            (*err) += fmt::format(
                "Prim name `{}` in `primChildren` metadatum not found in this "
                "Prim's children",
                nameTok.str());
          }
          return {false, {}};
        }
      }
    } else {
      // Use natural order
      for (const auto &child : prim.children()) {
        result.push_back(&child);
      }
    }

    return {true, result};
  };

  // Visit start prim first
  {
    std::string fun_error;
    bool ret = visitor_fun(start_abs_path, start_prim, start_level, userdata, &fun_error);
    if (!ret) {
      if (fun_error.empty()) {
        DCOUT("Early termination requested");
      } else {
        if (err) {
          (*err) += fmt::format(
              "Visit function returned an error for Prim {} (id {}). err = {}",
              start_abs_path.full_path_name(), start_prim.prim_id(), fun_error);
        }
      }
      return false;
    }
  }

  // Get ordered children for start prim
  std::pair<bool, std::vector<const lightusd::Prim *>> start_result =
      get_ordered_children(start_prim);
  if (!start_result.first) {
    return false;
  }

  if (!start_result.second.empty()) {
    StackEntry entry(&start_prim, start_level, start_abs_path);
    entry.ordered_children = std::move(start_result.second);
    stack.push_back(std::move(entry));
  }

  // Iterative traversal
  size_t iter = 0;
  while (!stack.empty()) {
    if (iter++ >= max_iter) {
      if (err) {
        (*err) += "VisitPrims exceeded max iteration limit.\n";
      }
      return false;
    }
    auto &top = stack.back();

    if (top.child_idx >= top.ordered_children.size()) {
      // All children processed, backtrack
      stack.pop_back();
      continue;
    }

    const lightusd::Prim *child = top.ordered_children[top.child_idx];
    ++top.child_idx;

    // Build path for this child
    lightusd::Path child_abs_path = top.abs_path.AppendPrim(child->element_name());
    int32_t child_level = top.level + 1;

    // Call visitor
    std::string fun_error;
    bool ret = visitor_fun(child_abs_path, *child, child_level, userdata, &fun_error);
    if (!ret) {
      if (fun_error.empty()) {
        DCOUT("Early termination requested");
      } else {
        if (err) {
          (*err) += fmt::format(
              "Visit function returned an error for Prim {} (id {}). err = {}",
              child_abs_path.full_path_name(), child->prim_id(), fun_error);
        }
      }
      return false;
    }

    // Get ordered children for this child
    std::pair<bool, std::vector<const lightusd::Prim *>> child_result =
        get_ordered_children(*child);
    if (!child_result.first) {
      return false;
    }

    if (!child_result.second.empty()) {
      StackEntry entry(child, child_level, std::move(child_abs_path));
      entry.ordered_children = std::move(child_result.second);
      stack.push_back(std::move(entry));
    }
  }

  return true;
}


// Scalar-valued attribute.
// TypedAttribute* => Attribute defined in USD schema, so not a custom attr.
}  // namespace

bool VisitPrims(const lightusd::Stage &stage, VisitPrimFunction visitor_fun,
                void *userdata, std::string *err) {
  // if `primChildren` is available, use it
  if (stage.metas().primChildren.size() == stage.root_prims().size()) {
    std::unordered_map<std::string, const Prim *, FNV1StringHash> primNameTable;
    primNameTable.reserve(stage.root_prims().size());
    for (size_t i = 0; i < stage.root_prims().size(); i++) {
      primNameTable.emplace(stage.root_prims()[i].element_name(),
                            &stage.root_prims()[i]);
    }

    for (size_t i = 0; i < stage.metas().primChildren.size(); i++) {
      value::token nameTok = stage.metas().primChildren[i];
      const auto it = primNameTable.find(nameTok.str());
      if (it != primNameTable.end()) {
        const Path root_abs_path("/" + nameTok.str(), "");
        if (!VisitPrimsIterative(root_abs_path, *it->second, 0, visitor_fun,
                                 userdata, err)) {
          return false;
        }
      } else {
        if (err) {
          (*err) += fmt::format(
              "Prim name `{}` in root Layer's `primChildren` metadatum not "
              "found in Layer root.",
              nameTok.str());
        }
        return false;
      }
    }

  } else {
    for (const auto &root : stage.root_prims()) {
      const Path root_abs_path("/" + root.element_name(), /* prop part */ "");
      if (!VisitPrimsIterative(root_abs_path, root, /* root level */ 0,
                               visitor_fun, userdata, err)) {
        return false;
      }
    }
  }

  return true;
}

template <typename T>
bool PrimToPrimSpecImpl(const T &p, PrimSpec &ps, std::string *err);

template <>
bool PrimToPrimSpecImpl(const Model &p, PrimSpec &ps, std::string *err) {
  (void)err;

  ps.name() = p.name;
  ps.specifier() = p.spec;

  ps.props() = p.props;
  ps.metas() = p.meta;

  // TODO: variantSet
  // ps.variantSets

  return true;
}

template <>
bool PrimToPrimSpecImpl(const Xform &p, PrimSpec &ps, std::string *err) {
  (void)err;

  ps.name() = p.name;
  ps.specifier() = p.spec;

  ps.props() = p.props;

  // TODO..
  std::vector<value::token> toks;
  Attribute xformOpOrderAttr;
  xformOpOrderAttr.set_value(std::move(toks));
  ps.props().emplace("xformOpOrder",
                     Property(xformOpOrderAttr, /* custom */ false));

  ps.metas() = p.meta;

  // TODO: variantSet
  // ps.variantSets

  return true;
}

bool PrimToPrimSpec(const Prim &prim, PrimSpec &ps, std::string *err) {
#define TO_PRIMSPEC(__ty)                                   \
  if (prim.as<__ty>()) {                                    \
    return PrimToPrimSpecImpl(*(prim.as<__ty>()), ps, err); \
  } else

  TO_PRIMSPEC(Model) {
    if (err) {
      (*err) +=
          "Unsupported/unimplemented Prim type: " + prim.prim_type_name() +
          "\n";
    }
    return false;
  }

#undef TO_PRIMSPEC
}

std::vector<const GeomSubset *> GetGeomSubsets(
    const lightusd::Stage &stage, const lightusd::Path &prim_path,
    const lightusd::value::token &familyName, bool prim_must_be_geommesh) {
  std::vector<const GeomSubset *> result;

  const Prim *pprim{nullptr};
  if (!stage.find_prim_at_path(prim_path, pprim)) {
    return result;
  }

  if (!pprim) {
    return result;
  }

  if (prim_must_be_geommesh && !pprim->is<GeomMesh>()) {
    return result;
  }

  // Only account for child Prims.
  for (const auto &p : pprim->children()) {
    if (auto pv = p.as<GeomSubset>()) {
      if (familyName.valid()) {
        if (pv->familyName.authored()) {
          if (pv->familyName.get_value().has_value()) {
            const value::token tok = pv->familyName.get_value().value();
            if (familyName.str() == tok.str()) {
              result.push_back(pv);
            }
          } else {
            // connection attr or value block?
            // skip adding this GeomSubset.
          }
        } else {
          result.push_back(pv);
        }
      } else {
        result.push_back(pv);
      }
    }
  }

  return result;
}

std::vector<const GeomSubset *> GetGeomSubsetChildren(
    const lightusd::Prim &prim, const lightusd::value::token &familyName,
    bool prim_must_be_geommesh) {
  std::vector<const GeomSubset *> result;

  if (prim_must_be_geommesh && !prim.is<GeomMesh>()) {
    return result;
  }

  // Only account for child Prims.
  for (const auto &p : prim.children()) {
    if (auto pv = p.as<GeomSubset>()) {
      if (familyName.valid()) {
        if (pv->familyName.authored()) {
          if (pv->familyName.get_value().has_value()) {
            const value::token tok = pv->familyName.get_value().value();
            if (familyName.str() == tok.str()) {
              result.push_back(pv);
            }
          } else {
            // connection attr or value block?
            // skip adding this GeomSubset.
          }
        } else {
          result.push_back(pv);
        }
      } else {
        result.push_back(pv);
      }
    }
  }

  return result;
}


bool GetCollection(const Prim &prim, const Collection **dst) {
  if (!dst) {
    return false;
  }

  auto fn = [dst](const Collection *coll) {
    (*dst) = coll;
    return true;
  };

  bool ret = ApplyToCollection(prim, fn);

  return ret;
}

}  // namespace tydra

}  // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
