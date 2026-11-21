// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Dependency-free RGB color transforms shared by lightusd legacy and next.
// The transfer-function and chromaticity math follows OpenUSD NanoColor.

#pragma once

#include <cstddef>
#include <string>

namespace lightusd {
namespace color {

enum class ColorSpaceKind {
  Color,
  Data,
  Unknown,
};

struct ColorSpaceDesc {
  std::string name;
  // Row-major RGB-to-CIE-XYZ matrix.
  float rgb_to_xyz[9] = {1.0f, 0.0f, 0.0f,
                         0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 1.0f};
  float gamma = 1.0f;
  float linear_bias = 0.0f;
  ColorSpaceKind kind = ColorSpaceKind::Unknown;
};

struct ColorTransform {
  ColorSpaceDesc source;
  ColorSpaceDesc destination;
  // Row-major linear-source-RGB to linear-destination-RGB matrix.
  float matrix[9] = {1.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 1.0f};
  bool bypass = true;
};

// Return the canonical OpenUSD token for a canonical token or a supported
// MaterialX/legacy alias. Unknown names are returned unchanged.
std::string CanonicalizeToken(const std::string &token);

bool GetBuiltinColorSpace(const std::string &token, ColorSpaceDesc *out);

// Construct a NanoColor-compatible descriptor from xy chromaticities.
bool MakeColorSpaceFromChromaticities(
    const std::string &name, const float red[2], const float green[2],
    const float blue[2], const float white[2], float gamma,
    float linear_bias, ColorSpaceDesc *out);

bool BuildColorTransform(const ColorSpaceDesc &source,
                         const ColorSpaceDesc &destination,
                         ColorTransform *out);

// Apply to packed RGB or RGBA float arrays. Alpha is never modified.
void TransformRGB(const ColorTransform &transform, float rgb[3]);
void TransformRGBSpan(const ColorTransform &transform, float *rgb,
                      size_t rgb_count);
void TransformRGBASpan(const ColorTransform &transform, float *rgba,
                       size_t rgba_count);

bool IsLinear(const ColorSpaceDesc &space);
bool IsData(const ColorSpaceDesc &space);

}  // namespace color
}  // namespace lightusd
