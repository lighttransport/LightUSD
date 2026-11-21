// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <string>

#include "render-data.hh"
namespace lightusd {
namespace tydra {
// Export render-ready data (not the original authored Stage). Mesh instances
// retain world transforms. Texture assets remain external. Outputs change only
// on success; diagnostics report approximations to unsupported shader models.
bool export_to_usda(const RenderScene& scene, std::string& output,
                    std::string* warning, std::string* error,
                    size_t max_output_bytes = 512u * 1024u * 1024u);
}  // namespace tydra
}  // namespace lightusd
