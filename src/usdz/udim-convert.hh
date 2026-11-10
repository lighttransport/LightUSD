// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <map>
#include <string>
#include <vector>
#include "layer/layer.hh"
#include "usdz-converter.hh"
namespace lightusd::usdz {
bool ReadNativeAtlasImage(const std::string& path, std::vector<uint8_t>* bytes,
                          std::string* error, size_t limit);

bool BakeUDIMs(Layer&, const UsdzConvertOptions&, const std::string& root_dir,
               std::map<std::string, std::vector<uint8_t>>* assets, std::string* error);
}
