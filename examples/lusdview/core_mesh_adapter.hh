// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include "lightusd-c.h"

namespace lightusd {
namespace tydra {
struct ConverterConfig;
struct RenderMesh;
}  // namespace tydra
}  // namespace lightusd

namespace lusdview {
// Copy the public render API's temporary mesh record into viewer-owned storage.
// Failure leaves out unchanged; the caller retains stage for the whole query.
bool ConvertMeshThroughPublicAPI(
    const lightusd_stage* stage, const std::string& path,
    const lightusd::tydra::ConverterConfig& config,
    lightusd::tydra::RenderMesh* out, uint8_t proxyMode = 0);
}  // namespace lusdview
