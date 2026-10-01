// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "udim-layer.hh"
namespace lightusd::udim {
struct GeometryEdits {
  std::vector<int32_t> skin_joints;
  struct Shape {
    std::string path;
    std::vector<std::pair<std::string, std::string>> channels;
  };
  std::vector<Shape> shapes;
};
bool PrepareGeometry(LayerAccess&, const std::string&, const Options&,
                     GeometryEdits*, std::string*);
bool RefineGeometry(LayerAccess&, const std::string&, const Options&,
                    std::vector<uint32_t>*, std::string*);
bool FinishGeometry(LayerAccess&, const std::string&, const GeometryEdits&,
                    const Options&, std::string*);
}  // namespace lightusd::udim
