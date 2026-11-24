// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include "nonstd/expected.hpp"
namespace lightusd {
class Layer;
// Export authored layer metadata, prims, attributes, relationships and variants.
// Numeric attribute values use canonical USD text; typed metadata retains USD
// types. Output and traversal limits match the core WASM layer JSON contract.
nonstd::expected<std::string, std::string> WriteLayerToJSON(const Layer& layer);
}
