// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../tydra/asset-import/import.hh"
#include "../layer/layer.hh"
namespace lightusd {
namespace next {
// OBJ is a file-format adapter; output uses the standard Geom/Shade/Skel
// schemas.
bool ConvertOBJPayloadToLayer(const tydra::import_asset::Json& payload,
                              Layer* layer, std::string* err);
std::shared_ptr<Layer> LoadOBJLayerFromMemory(
    const std::string& source, const uint8_t* data, size_t size,
    const tydra::import_asset::Options& options, std::string* warn,
    std::string* err);
std::shared_ptr<Layer> LoadOBJLayerFromFile(
    const std::string& source, const tydra::import_asset::Options& options,
    std::string* warn, std::string* err);
}  // namespace next
}  // namespace lightusd
