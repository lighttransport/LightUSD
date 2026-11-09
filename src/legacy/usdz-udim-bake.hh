// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "udim-bake.hh"
#include "usdz-convert.hh"
namespace lightusd { namespace legacy {
class Layer;
namespace usdz {
udim::Options UDIMOptions(const UsdzConvertOptions&);
bool BakeUDIMInLayer(const UsdzConvertOptions&, Layer*, const udim::Fetch&,
                     const std::function<bool(const std::string&)>& exists,
                     std::map<std::string, std::vector<uint8_t>>* assets,
                     UsdzConvertStats*, std::string* error);
}  // namespace usdz
}  // namespace legacy
}  // namespace lightusd
