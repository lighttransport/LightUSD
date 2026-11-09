// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Explicit instantiations of ListPrims for all prim types.
// Split from scene-access.cc for parallel compilation.
//
// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

// src
#include "common-macros.inc"
#include "prim-pprint.hh"
#include "core/prim.hh"
#include "primvar.hh"
#include "tiny-container.hh"
#include "tiny-format.hh"
#include "tydra/prim-apply.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdPhysics.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "value-pprint.hh"
#include "xform.hh"  // For matrix inverse

// src/tydra
#include "attribute-eval.hh"
#include "scene-access.hh"

#include <unordered_set>


namespace lightusd {
namespace tydra {

#include "tydra/scene-access-traverse-impl.inc"

#define LISTPRIMS_INSTANCIATE(__ty) \
  template bool ListPrims(const lightusd::Stage &stage, PathPrimMap<__ty> &m);

APPLY_FUNC_TO_PRIM_TYPES(LISTPRIMS_INSTANCIATE)

#undef LISTPRIMS_INSTANCIATE

}  // namespace tydra
}  // namespace lightusd
