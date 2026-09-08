// SPDX-License-Identifier: Apache-2.0
// Internal collection-based light-link resolver.
#ifndef LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_LIGHT_LINKING_HH_
#define LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_LIGHT_LINKING_HH_

namespace lightusd { namespace next { class Stage; } }
namespace lightusd { namespace tydra { namespace next {
class RenderScene;
void ResolveLightLinking(const ::lightusd::next::Stage& stage,
                         RenderScene* scene);
}}}  // namespace lightusd::tydra::next
#endif
