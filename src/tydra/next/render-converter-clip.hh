// SPDX-License-Identifier: Apache-2.0
// Private clip fallback shared by mesh and curve conversion.
#pragma once
#include "next/eval/value-clip.hh"
#include "render-extract.hh"

namespace lightusd { namespace tydra { namespace next {
bool ReadFloatArrayFromValueClips(
    const ::lightusd::next::UsdPrim& prim, const char* name, double time,
    const ::lightusd::next::ValueClipStageLoader& loader,
    ValueArrayRead<float>* out);
}}}  // namespace lightusd::tydra::next
