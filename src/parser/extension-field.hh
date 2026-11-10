// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include "layer/prim-spec.hh"

namespace lightusd {
// Decode an unregistered metadata opinion while retaining its exact source
// spelling for lossless crate/USDA rewriting.
DecodedExtensionField ParseExtensionField(std::string name, std::string source);
}
