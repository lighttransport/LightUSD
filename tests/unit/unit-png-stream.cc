#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "image-loader.hh"
#include "image-writer.hh"
#include "imageio/png-stream.hh"
#include "tydra/texture-util.hh"

#include <cstdint>
#include <vector>

using namespace lightusd;

namespace {

// Build a deterministic test image with `ch` channels (8-bit).
lightusd::Image MakeImage(int w, int h, int ch) {
  lightusd::Image img;
  img.width = w;
  img.height = h;
  img.channels = ch;
  img.bpp = 8;
  img.format = lightusd::Image::PixelFormat::UInt;
  img.data.resize((size_t)w * h * ch);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < ch; ++c) {
        img.data[((size_t)y * w + x) * ch + c] =
            (uint8_t)((x * 7 + y * 13 + c * 53) & 0xFF);
      }
    }
  }
  return img;
}

std::vector<uint8_t> EncodePNG(const lightusd::Image &img) {
  lightusd::image::WriteOption opt;
  opt.format = lightusd::image::WriteImageFormat::PNG;
  auto r = lightusd::image::WriteImageToMemory(img, opt);
  TEST_CHECK(bool(r));
  if (!r) return {};
  return r.value();
}

// Decode `png` and compare RGBA-normalized pixels against `ref`.
void CheckPixelsEqual(const std::vector<uint8_t> &png,
                      const lightusd::Image &ref) {
  auto dec = lightusd::image::LoadImageFromMemory(png.data(), png.size(), "mem");
  TEST_CHECK(bool(dec));
  if (!dec) return;
  const auto &im = dec.value().image;
  TEST_CHECK(im.uri == "mem");
  TEST_CHECK(im.width == ref.width);
  TEST_CHECK(im.height == ref.height);
  // Compare only the channels present in the reference (decoders may expand to
  // RGBA). Sample per-pixel up to ref.channels.
  int rc = ref.channels;
  int dc = im.channels;
  bool ok = true;
  for (int y = 0; y < ref.height && ok; ++y) {
    for (int x = 0; x < ref.width && ok; ++x) {
      for (int c = 0; c < rc; ++c) {
        uint8_t a = ref.data[((size_t)y * ref.width + x) * rc + c];
        uint8_t b = im.data[((size_t)y * im.width + x) * dc + c];
        if (a != b) { ok = false; break; }
      }
    }
  }
  TEST_CHECK(ok);
}

void TranscodeRoundtrip(int w, int h, int ch) {
  lightusd::Image img = MakeImage(w, h, ch);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 0);
  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::TranscodePNG(png.data(), png.size(), out);
  TEST_CHECK(ok);
  if (!ok) return;
  TEST_CHECK(out.size() > 0);
  // Transcoded PNG must decode to the same pixels as the original image.
  CheckPixelsEqual(out, img);
}

}  // namespace

void png_stream_transcode_rgba_test(void) { TranscodeRoundtrip(67, 41, 4); }
void png_stream_transcode_rgb_test(void) { TranscodeRoundtrip(64, 64, 3); }
void png_stream_transcode_gray_test(void) { TranscodeRoundtrip(33, 50, 1); }

// Drive the reader/writer directly (fresh-mode) and confirm a pixel-faithful
// round-trip — this is the path the resize/colorspace pipeline will use.
void png_stream_reader_writer_roundtrip_test(void) {
  lightusd::Image img = MakeImage(40, 24, 4);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 0);

  lightusd::imageio::PngScanlineReader reader;
  TEST_CHECK(reader.Open(png.data(), png.size()));
  lightusd::imageio::PngScanlineWriter writer;
  TEST_CHECK(writer.Begin(reader.info()));

  std::vector<uint8_t> row(reader.info().row_bytes);
  uint32_t rows = 0;
  while (reader.NextRow(row.data())) {
    TEST_CHECK(writer.WriteRow(row.data()));
    ++rows;
  }
  TEST_CHECK(reader.ok());
  TEST_CHECK(rows == img.height);
  std::vector<uint8_t> out;
  TEST_CHECK(writer.Finish(out));
  CheckPixelsEqual(out, img);
}

void png_stream_reject_nonpng_test(void) {
  const uint8_t junk[16] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4,
                            5,    6,    7,    8,    9, 10, 11, 12};  // JPEG sig
  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::TranscodePNG(junk, sizeof(junk), out);
  TEST_CHECK(!ok);  // must reject -> caller falls back to whole-image codec
}

// Streaming resize must match the whole-image tydra::ResizeImage (same stbir lib
// + same linear filter) to within rounding.
static void ResizeParity(int w, int h, int ch, int tw, int th,
                         bool srgb = false) {
  lightusd::Image img = MakeImage(w, h, ch);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 0);

  // Streaming resize -> decode result.
  std::vector<uint8_t> rpng;
  bool ok = lightusd::imageio::ResizePNG(png.data(), png.size(), (uint32_t)tw,
                                         (uint32_t)th, srgb, rpng);
  TEST_CHECK(ok);
  if (!ok) return;
  auto dec = lightusd::image::LoadImageFromMemory(rpng.data(), rpng.size(), "m");
  TEST_CHECK(bool(dec));
  if (!dec) return;
  const auto &streamed = dec.value().image;
  TEST_CHECK(streamed.width == tw && streamed.height == th);

  // Whole-image reference.
  lightusd::Image ref;
  std::string err;
  bool rok = lightusd::tydra::ResizeImage(
      img, tw, th, &ref,
      srgb ? lightusd::tydra::ResizeFilter::SRGB
           : lightusd::tydra::ResizeFilter::Linear,
      &err);
  TEST_CHECK(rok);
  if (!rok) return;

  // Compare ref channels against the (possibly RGBA-expanded) decoded result.
  int dc = streamed.channels;
  int maxdiff = 0;
  for (int y = 0; y < th; ++y)
    for (int x = 0; x < tw; ++x)
      for (int c = 0; c < ch; ++c) {
        int a = ref.data[((size_t)y * tw + x) * ch + c];
        int b = streamed.data[((size_t)y * tw + x) * dc + c];
        int d = a > b ? a - b : b - a;
        if (d > maxdiff) maxdiff = d;
      }
  TEST_CHECK(maxdiff <= 1);  // streaming vs whole-image: identical within 1 LSB
}

// 16-bit grayscale PNG transcode must be lossless (byte-wise codec). The writer
// (fresh mode) builds the 16-bit PNG so the test needs no external 16-bit codec.
void png_stream_transcode_16bit_test(void) {
  const uint32_t W = 50, H = 37;
  lightusd::imageio::PngImageInfo info;
  info.width = W;
  info.height = H;
  info.bit_depth = 16;
  info.color_type = 0;  // grayscale
  lightusd::imageio::PngScanlineWriter w;
  TEST_CHECK(w.Begin(info));
  std::vector<std::vector<uint8_t>> rows(H, std::vector<uint8_t>((size_t)W * 2));
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint16_t v = (uint16_t)((x * 257 + y * 131) & 0xFFFF);
      rows[y][x * 2] = (uint8_t)(v >> 8);       // big-endian (PNG)
      rows[y][x * 2 + 1] = (uint8_t)(v & 0xFF);
    }
    TEST_CHECK(w.WriteRow(rows[y].data()));
  }
  std::vector<uint8_t> png16;
  TEST_CHECK(w.Finish(png16));

  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::TranscodePNG(png16.data(), png16.size(), out);
  TEST_CHECK(ok);
  if (!ok) return;

  lightusd::imageio::PngScanlineReader r;
  TEST_CHECK(r.Open(out.data(), out.size()));
  TEST_CHECK(r.info().bit_depth == 16 && r.info().color_type == 0 &&
             r.info().width == W && r.info().height == H);
  std::vector<uint8_t> row(r.info().row_bytes);
  bool good = true;
  for (uint32_t y = 0; y < H && good; ++y) {
    if (!r.NextRow(row.data())) { good = false; break; }
    if (row != rows[y]) good = false;  // lossless: identical 16-bit samples
  }
  TEST_CHECK(good);
}

// 16-bit resize: a constant image must resize to the same constant (any linear
// filter preserves constants) — this also exercises the BE<->native byte swap.
void png_stream_resize_16bit_test(void) {
  const uint32_t W = 40, H = 30;
  lightusd::imageio::PngImageInfo info;
  info.width = W;
  info.height = H;
  info.bit_depth = 16;
  info.color_type = 0;  // grayscale
  lightusd::imageio::PngScanlineWriter w;
  TEST_CHECK(w.Begin(info));
  std::vector<uint8_t> row((size_t)W * 2);
  for (uint32_t x = 0; x < W; ++x) { row[x * 2] = 0xAB; row[x * 2 + 1] = 0xCD; }
  for (uint32_t y = 0; y < H; ++y) TEST_CHECK(w.WriteRow(row.data()));
  std::vector<uint8_t> png;
  TEST_CHECK(w.Finish(png));

  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::ResizePNG(png.data(), png.size(), 20, 15,
                                         /*srgb=*/false, out);
  TEST_CHECK(ok);
  if (!ok) return;
  lightusd::imageio::PngScanlineReader r;
  TEST_CHECK(r.Open(out.data(), out.size()));
  TEST_CHECK(r.info().bit_depth == 16 && r.info().color_type == 0 &&
             r.info().width == 20 && r.info().height == 15);
  std::vector<uint8_t> orow(r.info().row_bytes);
  bool good = true;
  for (uint32_t y = 0; y < 15 && good; ++y) {
    if (!r.NextRow(orow.data())) { good = false; break; }
    for (uint32_t x = 0; x < 20; ++x)
      if (orow[x * 2] != 0xAB || orow[x * 2 + 1] != 0xCD) { good = false; break; }
  }
  TEST_CHECK(good);
}

// 16-bit grayscale sRGB->linear colorspace conversion must apply the 16-bit LUT.
void png_stream_colorspace_16bit_test(void) {
  const uint32_t W = 40, H = 24;
  lightusd::imageio::PngImageInfo info;
  info.width = W;
  info.height = H;
  info.bit_depth = 16;
  info.color_type = 0;
  lightusd::imageio::PngScanlineWriter w;
  TEST_CHECK(w.Begin(info));
  std::vector<std::vector<uint8_t>> rows(H, std::vector<uint8_t>((size_t)W * 2));
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      uint16_t v = (uint16_t)((x * 1619 + y * 4099) & 0xFFFF);
      rows[y][x * 2] = (uint8_t)(v >> 8);
      rows[y][x * 2 + 1] = (uint8_t)(v & 0xFF);
    }
    TEST_CHECK(w.WriteRow(rows[y].data()));
  }
  std::vector<uint8_t> png;
  TEST_CHECK(w.Finish(png));

  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::ConvertColorspacePNG(
      png.data(), png.size(), lightusd::imageio::ColorspaceXform::SrgbToLinear,
      out);
  TEST_CHECK(ok);
  if (!ok) return;

  std::vector<uint16_t> lut(65536);
  for (int i = 0; i < 65536; ++i) {
    float c = float(i) / 65535.0f;
    float o = (c <= 0.04045f) ? (c / 12.92f)
                              : std::pow((c + 0.055f) / 1.055f, 2.4f);
    int v = int(o * 65535.0f + 0.5f);
    lut[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
  }
  lightusd::imageio::PngScanlineReader r;
  TEST_CHECK(r.Open(out.data(), out.size()));
  TEST_CHECK(r.info().bit_depth == 16 && r.info().color_type == 0);
  std::vector<uint8_t> orow(r.info().row_bytes);
  bool good = true;
  for (uint32_t y = 0; y < H && good; ++y) {
    if (!r.NextRow(orow.data())) { good = false; break; }
    for (uint32_t x = 0; x < W; ++x) {
      uint16_t inv = (uint16_t)((rows[y][x * 2] << 8) | rows[y][x * 2 + 1]);
      uint16_t outv = (uint16_t)((orow[x * 2] << 8) | orow[x * 2 + 1]);
      if (outv != lut[inv]) { good = false; break; }
    }
  }
  TEST_CHECK(good);
}

// A PNG declaring absurd dimensions must be rejected (not overflow/crash) so the
// caller falls back to the whole-image codec.
void png_stream_reject_huge_dims_test(void) {
  lightusd::Image img = MakeImage(16, 16, 3);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 24);
  if (png.size() <= 24) return;
  // Patch IHDR width (offset 16, big-endian) to ~2^31 (the reader doesn't
  // validate the IHDR CRC, so this reaches the dimension guard).
  png[16] = 0x7F; png[17] = 0xFF; png[18] = 0xFF; png[19] = 0xFF;
  std::vector<uint8_t> out;
  TEST_CHECK(!lightusd::imageio::TranscodePNG(png.data(), png.size(), out));
  TEST_CHECK(
      !lightusd::imageio::ResizePNG(png.data(), png.size(), 8, 8, false, out));
  lightusd::imageio::PngScanlineReader r;
  TEST_CHECK(!r.Open(png.data(), png.size()));
}

// fp16 (half-float) resize: a constant image must resize to the same constant,
// exercising tydra::ResizeImage's STBIR_TYPE_HALF_FLOAT path. 0x3C00 = half 1.0.
void png_stream_resize_fp16_test(void) {
  const int W = 40, H = 30, ch = 3;
  lightusd::Image img;
  img.width = W;
  img.height = H;
  img.channels = ch;
  img.bpp = 16;
  img.format = lightusd::Image::PixelFormat::Float;
  img.data.resize((size_t)W * H * ch * 2);
  for (size_t i = 0; i < (size_t)W * H * ch; ++i) {
    img.data[i * 2] = 0x00;  // half 1.0 = 0x3C00, little-endian
    img.data[i * 2 + 1] = 0x3C;
  }
  lightusd::Image out;
  std::string err;
  bool ok = lightusd::tydra::ResizeImage(
      img, 20, 15, &out, lightusd::tydra::ResizeFilter::Linear, &err);
  TEST_CHECK(ok);
  if (!ok) return;
  TEST_CHECK(out.width == 20 && out.height == 15 && out.bpp == 16 &&
             out.channels == ch &&
             out.format == lightusd::Image::PixelFormat::Float);
  bool good = out.data.size() == (size_t)20 * 15 * ch * 2;
  for (size_t i = 0; good && i < (size_t)20 * 15 * ch; ++i)
    if (out.data[i * 2] != 0x00 || out.data[i * 2 + 1] != 0x3C) good = false;
  TEST_CHECK(good);
}

// fp16 EXR round-trip: encode an fp16 RGBA image as half EXR (no fp32 widening)
// then decode it back as half — the half bit patterns must survive exactly
// (EXR ZIP is lossless; half->half has no conversion).
void png_stream_exr_fp16_roundtrip_test(void) {
  const int W = 24, H = 18, ch = 4;
  lightusd::Image img;
  img.width = W;
  img.height = H;
  img.channels = ch;
  img.bpp = 16;
  img.format = lightusd::Image::PixelFormat::Float;
  img.data.resize((size_t)W * H * ch * 2);
  uint16_t *s = reinterpret_cast<uint16_t *>(img.data.data());
  for (size_t i = 0; i < (size_t)W * H * ch; ++i)
    s[i] = (uint16_t)(0x3000 + (i % 0x200));  // finite positive half normals

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::EXR;
  auto enc = lightusd::image::WriteImageToMemory(img, wopt);
  TEST_CHECK(bool(enc));
  if (!enc) return;

  lightusd::Image dec;
  std::string err;
  bool ok = lightusd::image::DecodeImageEXRHalf(enc.value().data(),
                                                enc.value().size(), "m", &dec,
                                                &err);
  TEST_CHECK(ok);
  if (!ok) return;
  TEST_CHECK(dec.width == W && dec.height == H && dec.bpp == 16 &&
             dec.channels == 4 &&
             dec.format == lightusd::Image::PixelFormat::Float);
  const uint16_t *d = reinterpret_cast<const uint16_t *>(dec.data.data());
  bool good = dec.data.size() == (size_t)W * H * 4 * 2;
  for (size_t i = 0; good && i < (size_t)W * H * ch; ++i)
    if (d[i] != s[i]) good = false;
  TEST_CHECK(good);
}

// A fp16 EXR must decode consistently via both paths. LoadImageFromMemory now
// preserves the all-half case instead of widening it unnecessarily.
void png_stream_exr_half_float_consistency_test(void) {
  const int W = 8, H = 6, ch = 4;
  lightusd::Image img;
  img.width = W;
  img.height = H;
  img.channels = ch;
  img.bpp = 16;
  img.format = lightusd::Image::PixelFormat::Float;
  img.data.resize((size_t)W * H * ch * 2);
  uint16_t *s = reinterpret_cast<uint16_t *>(img.data.data());
  for (size_t i = 0; i < (size_t)W * H * ch; ++i)
    s[i] = (i % 2) ? 0x3800 : 0x3C00;  // 0.5 : 1.0

  lightusd::image::WriteOption wopt;
  wopt.format = lightusd::image::WriteImageFormat::EXR;
  auto enc = lightusd::image::WriteImageToMemory(img, wopt);
  TEST_CHECK(bool(enc));
  if (!enc) return;

  // (a) half decode -> 16-bit, 4-channel.
  lightusd::Image half;
  std::string e1;
  bool okh = lightusd::image::DecodeImageEXRHalf(enc.value().data(),
                                                 enc.value().size(), "m", &half,
                                                 &e1);
  TEST_CHECK(okh && half.bpp == 16 && half.channels == 4 && half.width == W);

  // (b) LoadImageFromMemory preserves fp16 for the same input.
  auto dec = lightusd::image::LoadImageFromMemory(enc.value().data(),
                                                  enc.value().size(), "m.exr");
  TEST_CHECK(bool(dec));
  if (!dec) return;
  const auto &f = dec.value().image;
  TEST_CHECK(f.bpp == 16 && f.format == lightusd::Image::PixelFormat::Float &&
             f.channels == 4);
  const uint16_t *fp = reinterpret_cast<const uint16_t *>(f.data.data());
  bool good = f.data.size() == (size_t)W * H * 4 * sizeof(uint16_t);
  for (size_t i = 0; good && i < (size_t)W * H * 4; ++i) {
    const uint16_t expect = (i % 2) ? 0x3800 : 0x3C00;
    if (fp[i] != expect) good = false;
  }
  TEST_CHECK(good);
}

// GetImageInfoFromMemory reports correct dimensions without a full decode.
void image_get_info_test(void) {
  lightusd::Image img = MakeImage(40, 24, 3);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 0);
  auto info =
      lightusd::image::GetImageInfoFromMemory(png.data(), png.size(), "m.png");
  TEST_CHECK(bool(info));
  if (!info) return;
  TEST_CHECK(info.value().width == 40 && info.value().height == 24);
  TEST_CHECK(info.value().channels >= 1 && info.value().channels <= 4);
}

void png_stream_resize_rgb_test(void) { ResizeParity(128, 96, 3, 40, 33); }
void png_stream_resize_rgba_test(void) { ResizeParity(100, 100, 4, 50, 25); }
void png_stream_resize_srgb_test(void) { ResizeParity(128, 96, 3, 40, 33, true); }

void png_stream_colorspace_test(void) {
  const int w = 64, h = 48, ch = 4;  // RGBA: convert RGB, preserve alpha
  lightusd::Image img = MakeImage(w, h, ch);
  std::vector<uint8_t> png = EncodePNG(img);
  TEST_CHECK(png.size() > 0);

  std::vector<uint8_t> out;
  bool ok = lightusd::imageio::ConvertColorspacePNG(
      png.data(), png.size(), lightusd::imageio::ColorspaceXform::SrgbToLinear,
      out);
  TEST_CHECK(ok);
  if (!ok) return;
  auto dec = lightusd::image::LoadImageFromMemory(out.data(), out.size(), "m");
  TEST_CHECK(bool(dec));
  if (!dec) return;
  const auto &res = dec.value().image;
  TEST_CHECK(res.width == w && res.height == h);

  // Reference LUT (same formula as the implementation).
  uint8_t lut[256];
  for (int i = 0; i < 256; ++i) {
    float c = float(i) / 255.0f;
    float o = (c <= 0.04045f) ? (c / 12.92f)
                              : std::pow((c + 0.055f) / 1.055f, 2.4f);
    int v = int(o * 255.0f + 0.5f);
    lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  }
  int dc = res.channels;
  bool good = true;
  for (int p = 0; p < w * h && good; ++p) {
    for (int c = 0; c < 3; ++c) {  // RGB transformed
      if (res.data[p * dc + c] != lut[img.data[p * ch + c]]) { good = false; break; }
    }
    if (dc == 4 && res.data[p * dc + 3] != img.data[p * ch + 3]) good = false;  // alpha preserved
  }
  TEST_CHECK(good);
}
