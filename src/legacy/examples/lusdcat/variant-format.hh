#pragma once

#include <sstream>
#include <string>
#include "lightusd.hh"

namespace variant_format {

// Convert Dictionary to YAML format
std::string dictionary_to_yaml(const lightusd::legacy::Dictionary& dict);

// Convert Dictionary to JSON format
std::string dictionary_to_json(const lightusd::legacy::Dictionary& dict);

}  // namespace variant_format
