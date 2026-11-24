// SPDX-License-Identifier: Apache-2.0
#include "binding-core-udim.hh"
#include "udim-core-layer.hh"

namespace lightusd::web_core {
std::string UDIMLayerJSON(::lightusd::Layer& layer, bool apply,
                         const std::string& input) {
  return udim::CoreLayerJSON(layer, apply, input);
}
}
