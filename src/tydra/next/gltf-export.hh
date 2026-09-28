// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "render-data.hh"
#include "next/resolver/asset-resolver.hh"
#include <string>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
struct GltfExportOptions {
  size_t max_output_bytes = size_t(1) << 30;
  const ::lightusd::next::AssetResolver* resolver = nullptr;
  std::string asset_anchor;
  bool fail_on_loss = false;
};
struct GltfExportResult {
  bool success = false;
  std::string error;
  std::vector<std::string> losses;
  std::vector<uint8_t> glb;
};
// Static, triangulated RenderScene -> self-contained glTF 2.0 binary.
// Unsupported features are listed in losses; fail_on_loss refuses the output.
GltfExportResult ExportGLB(const RenderScene& scene, const GltfExportOptions& options = {});
} } }
