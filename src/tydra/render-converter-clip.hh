// SPDX-License-Identifier: Apache-2.0
// Private clip fallback shared by mesh and curve conversion.
#pragma once
#include "eval/value-clip.hh"
#include "render-extract.hh"

namespace lightusd { namespace tydra {
bool ReadFloatArrayFromValueClips(
    const ::lightusd::UsdPrim& prim, const char* name, double time,
    const ::lightusd::ValueClipStageLoader& loader,
    ValueArrayRead<float>* out);
}}  // namespace lightusd::tydra
