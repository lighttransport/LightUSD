// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include "lightusd-c.h"

namespace lightusd {
namespace tydra {
namespace next {
struct ConverterConfig;
struct RenderMesh;
}  // namespace next
}  // namespace tydra
}  // namespace lightusd

namespace lusdview {
// Copy the public render API's temporary mesh record into viewer-owned storage.
// Failure leaves out unchanged; the caller retains stage for the whole query.
bool ConvertMeshThroughPublicAPI(
    const lightusd_stage* stage, const std::string& path,
    const lightusd::tydra::next::ConverterConfig& config,
    lightusd::tydra::next::RenderMesh* out, uint8_t proxyMode = 0);
}  // namespace lusdview
