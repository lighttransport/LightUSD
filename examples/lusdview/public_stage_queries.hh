// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include "lightusd-c.h"
namespace lusdview {
struct PublicStageInfo {
  std::string defaultPrim, upAxis, comment, documentation;
  bool startTimeCodeAuthored = false, endTimeCodeAuthored = false;
  double framesPerSecond = 0;
  double metersPerUnit = 0, startTimeCode = 0, endTimeCode = 0, timeCodesPerSecond = 0;
  size_t primCount = 0;
};
// Leaves out unchanged on failure.
bool ReadPublicStageInfo(const lightusd_stage* stage, PublicStageInfo* out);
// Authored depth-first order, including inactive prims. False stops all traversal.
// The caller retains stage and must not mutate it during the visit.
void VisitPublicPrims(const lightusd_stage* stage,
                     const std::function<bool(lightusd_prim)>& visitor);
// First authored/resolved extent, else extentsHint; no geometry expansion or
// time sampling. Reads the first min/max pair. Failure leaves outputs unchanged.
bool ReadPublicPreviewExtent(lightusd_prim prim, float min[3], float max[3]);
// Resolve ancestor animationSource, otherwise first animation under the enclosing
// SkelRoot. Samples named blend weights with linear interpolation; no connections.
std::unordered_map<std::string, float> ReadPublicBlendWeights(
    const lightusd_stage* stage, lightusd_prim mesh, double time);
std::string PublicString(lightusd_sv value);
// Compact inspector display; arrays use only type/count, never their buffers.
std::string PublicDefaultSummary(const lightusd_value_view& value, lightusd_sv text);
}
