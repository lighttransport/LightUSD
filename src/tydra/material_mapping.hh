// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "openpbr-params-converter.hh"

namespace lightusd {
namespace tydra {
namespace next {

/// Maps a RenderMaterial to Tydra's backend-neutral real-time PBR block.
inline bool MapRenderMaterialToRealtimePbr(
    const RenderMaterial& mat, tydra::RealtimePbrMaterial* p) {
  return BuildRealtimePbrMaterial(mat, p);
}

inline bool MapRenderMaterialToLightRtOpenPBR(
    const RenderMaterial& mat, tydra::LightRtOpenPBRParams* p) {
  return MapRenderMaterialToRealtimePbr(mat, p);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
