#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace lightusd {

std::string base122_encode(const std::vector<uint8_t>& data);

// Decode base122 string to binary data
// Returns 0 on success, nonzero on error (invalid char or truncated input)
int base122_decode(const std::string& str, std::vector<uint8_t>& out);


} // namespace lightusd
