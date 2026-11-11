// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdMedia Schema

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <string>

namespace lightusd {

// ============================================================
// Typed prims
// ============================================================

bool IsMediaAudio(const UsdPrim& prim);

// ============================================================
// MediaAudio data
// ============================================================

struct MediaAudioData {
  std::string filePath;   // uniform asset media:filePath
  float gain = 1.0f;
  float pitch = 1.0f;
  float startTime = 0.0f; // media:startTime
  float endTime = -1.0f;  // media:endTime (negative = play to end)
  bool loop = false;      // media:loop
};

bool GetMediaAudioData(const Stage& stage, const UsdPrim& prim,
                        MediaAudioData* out);

} // namespace lightusd
