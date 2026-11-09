// SPDX-License-Identifier: Apache-2.0
// Internal final material-to-scene fixups.
#ifndef LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_MATERIAL_FINALIZE_HH_
#define LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_MATERIAL_FINALIZE_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
class RenderScene;
struct RenderMaterial;
void RemapMaterialTextureIds(RenderMaterial* material,
                             const std::vector<int32_t>& texture_remap);
void PromoteMaterialUVPrimvars(RenderScene* scene,
                               std::vector<std::string>* warnings);
}}}  // namespace lightusd::tydra::next
#endif
