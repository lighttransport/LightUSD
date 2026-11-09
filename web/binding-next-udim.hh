// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

#include "next/layer/layer.hh"
namespace lightusd::web_next {
std::string UDIMLayerJSON(next::Layer&, bool apply, const std::string& input);
}
