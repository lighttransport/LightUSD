// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "layer/layer.hh"
#include "udim-layer.hh"

namespace lightusd::udim {
bool DescribeCoreLayer(Layer&, std::vector<Site>*, std::string*,
                       size_t budget = size_t(512) << 20,
                       std::vector<uint32_t>* anchors = nullptr);
bool ApplyCorePlans(Layer&, const std::vector<Plan>&, const Options&, std::string*);
std::string CoreLayerJSON(Layer&, bool apply, const std::string& input);
}
