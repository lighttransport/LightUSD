// Simple image writer
// supported file format: PNG(use fpng), JPEG(use stb_image), OpenEXR(use
// tinyexr), TIFF/DNG(use tinydng)
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"
#include "lightusd.hh"

namespace lightusd {
namespace image {

//
// Autodetect = determine file format using filename or Image's pixelformat +
// bpp.
//
enum class WriteImageFormat { Autodetect, BMP, PNG, JPEG, EXR, TIFF, DNG, QOI };

// PNG encoder backend selection.
// Auto  : use fpnge(veluca93) when available(LIGHTUSD_HAVE_FPNGE), otherwise fpng.
// Fpnge : force fpnge. Falls back to fpng when fpnge is not compiled in.
// Fpng  : force fpng(portable).
enum class PngEncoder { Auto, Fpnge, Fpng };

struct WriteOption {
  WriteImageFormat format{WriteImageFormat::Autodetect};
  bool half{false};  // Use half float for EXR

  // When non-zero value is set, prefer this bitdepth than Image's bpp.
  // Can specify 10, 12 and 14 for DNG when writing 16bit input image as 10, 12 and 14bit respectively.
  int bitdepth{
      0};

  // PNG encoder backend(only used for PNG output).
  PngEncoder png_encoder{PngEncoder::Auto};

  // JPEG quality(1-100). Only used for JPEG output.
  int jpeg_quality{90};
};

///
/// @param[in] filename Output filename
/// @param[in] image Image data
/// @param[in] option Image write option(optional)
///
/// @return true upon success. or error message(std::string) when failed.
///
nonstd::expected<bool, std::string> WriteImageToFile(
    const std::string &filename, const Image &image,
    WriteOption option = WriteOption());

///
/// @param[in] image Image data
/// @param[in] option Image write option(optional)
/// @return Serialized image data
///
nonstd::expected<std::vector<uint8_t>, std::string> WriteImageToMemory(
    const Image &image, const WriteOption option = WriteOption());

/// Write multiple TIFF directories (layers/pages) into one baseline TIFF.
/// Other formats are intentionally rejected because their memory writers have
/// no portable multi-image representation.
nonstd::expected<std::vector<uint8_t>, std::string> WriteImageLayersToMemory(
    const std::vector<Image> &images,
    const WriteOption option = WriteOption());

}  // namespace image
}  // namespace lightusd
