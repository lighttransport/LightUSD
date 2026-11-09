// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Explicit instantiations of ListShaders for all shader types.
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

template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPreviewSurface> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdUVTexture> &m);

template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_string> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_int> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_float> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_float2> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_float3> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_float4> &m);
template bool ListShaders(const lightusd::Stage &stage,
                          PathShaderMap<UsdPrimvarReader_matrix> &m);

}  // namespace tydra
}  // namespace lightusd
