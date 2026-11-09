// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdMedia Schema Implementation

#include "usd-media.hh"

namespace lightusd {
namespace next {

bool IsMediaAudio(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "MediaAudio";
}

bool GetMediaAudioData(const Stage& stage, const UsdPrim& prim,
                        MediaAudioData* out) {
  if (!IsMediaAudio(prim) || !out) return false;

  (void)stage;

  // filePath
  {
    const Value* val = prim.GetPropertyValue("media:filePath");
    if (val) {
      auto s = val->as_string();
      if (s) out->filePath = *s;
    }
  }

  // gain
  {
    const Value* val = prim.GetPropertyValue("media:gain");
    if (val) {
      const float* f = val->as_float();
      if (f) out->gain = *f;
    }
  }

  // pitch
  {
    const Value* val = prim.GetPropertyValue("media:pitch");
    if (val) {
      const float* f = val->as_float();
      if (f) out->pitch = *f;
    }
  }

  // startTime
  {
    const Value* val = prim.GetPropertyValue("media:startTime");
    if (val) {
      const float* f = val->as_float();
      if (f) out->startTime = *f;
    }
  }

  // endTime
  {
    const Value* val = prim.GetPropertyValue("media:endTime");
    if (val) {
      const float* f = val->as_float();
      if (f) out->endTime = *f;
    }
  }

  // loop
  {
    const Value* val = prim.GetPropertyValue("media:loop");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->loop = *b;
    }
  }

  return true;
}

} // namespace next
} // namespace lightusd
