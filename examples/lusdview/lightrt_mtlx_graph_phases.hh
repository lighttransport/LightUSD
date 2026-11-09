// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "gpu_scene.hh"

namespace lusdview {

// Pack lowered JSON nodes into the bounded backend-neutral graph ABI, route
// OpenPBR outputs, and canonicalize dependencies into dependency-first order.
// This phase is kept in its own translation unit so edits to lowering logic do
// not force the large packing/topological-ordering phase to rebuild.
bool PackMaterialXGraphRuntimePhase(
    const nlohmann::json& root, const nlohmann::json& nodegraph,
    const nlohmann::json& runtimeNodes,
    const std::map<std::string, std::map<std::string, std::string>>& closureLanes,
    MaterialXGraphRuntimeCPU* graph, std::string* err);

}  // namespace lusdview
