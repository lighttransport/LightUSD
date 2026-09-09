// SPDX-License-Identifier: Apache-2.0
// Internal render-converter scene-assembly helpers.
#ifndef LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_ASSEMBLY_HH_
#define LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_ASSEMBLY_HH_

#include <cstdint>
#include <string>

namespace lightusd { namespace tydra { namespace next {
class RenderScene;
struct RenderPointInstancer;
void AssignNodeDataId(RenderScene* scene, const std::string& prim_path,
                      int32_t data_id);
void ResolveSkeletalAnimationTargets(RenderScene* scene);
void ResolvePointInstancerPrototypeBindings(RenderScene* scene, RenderPointInstancer* instancer);
void AppendPointInstanceDraws(int32_t instancer_id, RenderPointInstancer* instancer, RenderScene* scene);
}}}  // namespace lightusd::tydra::next
#endif
