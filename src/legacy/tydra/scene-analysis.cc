// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

// src/tydra
#include "prim-apply.hh"
#include "attribute-eval.hh"
#include "scene-analysis.hh"
#include "../layer.hh"

#include "../common-macros.inc"

namespace lightusd {
namespace tydra {

namespace {


} // namespace

namespace detail {

static bool ComputeGeomBound(const PrimSpec &ps, bool use_extent, Extent &bbox, const double t) {
  // `points`
  if (!ps.props().count("points")) {
    return false;
  }

  (void)use_extent;
  (void)bbox;
  (void)t;

  return false;
}

} // namespace detail


bool ComputeBound(const Layer &layer, const bool use_extent, Extent &bbox, const double t) {

  bool has_bounds = false;

  bbox = Extent();

  for (const auto &ps : layer.primspecs()) {
    if (ComputePrimSpecBound(ps.second, use_extent, bbox, t)) {
      has_bounds = true;
    }
  }

  return has_bounds;
}

bool ComputePrimSpecBound(const PrimSpec &ps, const bool use_extent, Extent &bbox, const double t) {

  if ((ps.typeName() == "GeomMesh") ||
      (ps.typeName() == "GeomPoints")) {
    return detail::ComputeGeomBound(ps, use_extent, bbox, t);
  }
  // TODO: support more types.
  DCOUT("TODO: " << ps.typeName());
  return false;
}

}  // namespace tydra
}  // namespace lightusd
