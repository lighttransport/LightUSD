// SPDX-License-Identifier: Apache-2.0
// Internal point-instancer expanded-mesh helpers.
#ifndef LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_INSTANCE_MESHES_HH_
#define LIGHTUSD_TYDRA_NEXT_RENDER_CONVERTER_INSTANCE_MESHES_HH_

#include <cstddef>
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
class RenderScene;

/// Refresh existing expanded mesh clones for the given source mesh indices,
/// preserving each draw's expanded_mesh_id. Returns false only when a clone
/// could not be rebuilt. `refreshed_paths` receives the clone prim paths.
bool RefreshPointInstanceExpandedMeshes(
    RenderScene* scene, const std::vector<size_t>& changed_meshes,
    std::vector<std::string>* refreshed_paths);

}}}  // namespace lightusd::tydra::next
#endif
