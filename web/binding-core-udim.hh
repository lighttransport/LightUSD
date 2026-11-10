// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

#include "layer/layer.hh"
namespace lightusd::web_core {
std::string UDIMLayerJSON(::lightusd::Layer&, bool apply, const std::string& input);
}
