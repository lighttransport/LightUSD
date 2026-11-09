// SPDX-License-Identifier: MIT
//
// Built-in .obj import plugIn.
// Import only. Writing scene data as .obj is not supported.
//
// example usage
//
// def "mesh" (
//   prepend references = @bunny.obj@
// )
// {
//    ...
// }

#pragma once

#include <string>

namespace lightusd {
struct GPrim;
class Stage;
class Layer;
namespace tydra {
namespace import_asset {
struct Options;
}
}  // namespace tydra

namespace usdObj {

bool ReadObjFromString(const std::string& str, GPrim* prim,
                       std::string* err = nullptr);
bool ReadObjFromFile(const std::string& filepath, GPrim* prim,
                     std::string* err = nullptr);

// Full-scene import preserves objects, materials and texture bindings.
bool ReadObjStageFromFile(const std::string& filepath, Stage* stage,
                          std::string* warn = nullptr,
                          std::string* err = nullptr);

// source anchors MTL/textures; an injected reader is authoritative.
bool ReadObjStageFromMemory(const std::string& text, const std::string& source,
                            Stage* stage,
                            const tydra::import_asset::Options& options,
                            std::string* warn = nullptr,
                            std::string* err = nullptr);
bool ReadObjLayerFromMemory(const std::string& text, const std::string& source,
                            Layer* layer,
                            const tydra::import_asset::Options& options,
                            std::string* warn = nullptr,
                            std::string* err = nullptr);

}  // namespace usdObj

}  // namespace lightusd
