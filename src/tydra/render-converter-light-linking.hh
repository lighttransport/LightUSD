// SPDX-License-Identifier: Apache-2.0
// Internal collection-based light-link resolver.
#ifndef LIGHTUSD_TYDRA_RENDER_CONVERTER_LIGHT_LINKING_HH_
#define LIGHTUSD_TYDRA_RENDER_CONVERTER_LIGHT_LINKING_HH_

namespace lightusd {  class Stage;  }
namespace lightusd { namespace tydra {
class RenderScene;
void ResolveLightLinking(const ::lightusd::Stage& stage,
                         RenderScene* scene);
}}  // namespace lightusd::tydra
#endif
