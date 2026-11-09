// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// resize-image.hh
//
// The 8-bit image resize entry point, split out of tydra/texture-util.hh.
// texture-util.hh pulls image-writer.hh (PngEncoder) which reaches the legacy
// value-type layer; next-side consumers only need this small API, so including
// it directly keeps the legacy headers out of the next build.
//
#pragma once

#include <string>

#include "image-types.hh"

namespace lightusd {
namespace tydra {

enum class ResizeFilter { Auto, Linear, SRGB };

///
/// Resize an 8-bit `Image` to (dstWidth, dstHeight).
///
/// Only 8-bit per channel images are supported (bpp == 8), 1-4 channels.
/// The output preserves channels/format/colorspace of the source.
///
bool ResizeImage(const Image &src, int dstWidth, int dstHeight, Image *dst,
                 ResizeFilter filter, std::string *err);

}  // namespace tydra
}  // namespace lightusd
