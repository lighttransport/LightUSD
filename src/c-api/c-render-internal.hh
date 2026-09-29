// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#include "lightusd-render-c.h"

namespace lightusd { namespace tydra { namespace next { class RenderScene; } } }

namespace lightusd_internal {
const lightusd::tydra::next::RenderScene* RenderSceneForExport(
    const lightusd_render_scene* scene);
const std::vector<std::string>* RenderWarningsForExport(
    const lightusd_render_scene* scene);
}  // namespace lightusd_internal
