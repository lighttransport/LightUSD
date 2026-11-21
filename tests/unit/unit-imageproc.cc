#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "imageproc/simd.hh"

#include <cmath>
#include <cstdint>
#include <vector>
#include <cstring>
#include <string>
#include "image-loader.hh"

using namespace lightusd;

namespace {

std::vector<float> MakeRGB(size_t n) {
  std::vector<float> v(n * 3);
  for (size_t i = 0; i < n * 3; ++i) {
    v[i] = float((i * 2654435761u) & 0xFFFF) / 65535.0f;  // deterministic [0,1]
  }
  return v;
}

}  // namespace

void imageproc_mat3_identity_test(void) {
  const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::vector<float> in = MakeRGB(1000);
  std::vector<float> out(in.size(), -1.0f);
  imageproc::Mat3MulRGBf(in.data(), out.data(), 1000, I);
  bool ok = true;
  for (size_t i = 0; i < in.size(); ++i)
    if (out[i] != in[i]) { ok = false; break; }  // exact: multiply by 1/0 only
  TEST_CHECK(ok);
}

void imageproc_mat3_swap_test(void) {
  // Swap R<->B (and scale G by 2). All exactly representable -> bit-exact.
  const float M[9] = {0, 0, 1, 0, 2, 0, 1, 0, 0};
  const size_t n = 257;
  std::vector<float> in = MakeRGB(n);
  std::vector<float> out(in.size(), -1.0f);
  imageproc::Mat3MulRGBf(in.data(), out.data(), n, M);
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    if (out[3 * i + 0] != in[3 * i + 2]) { ok = false; break; }
    if (out[3 * i + 1] != in[3 * i + 1] * 2.0f) { ok = false; break; }
    if (out[3 * i + 2] != in[3 * i + 0]) { ok = false; break; }
  }
  TEST_CHECK(ok);
}

void imageproc_mat3_parity_test(void) {
  // A representative linear DisplayP3->sRGB-ish matrix vs a scalar reference.
  const float M[9] = {1.2249f,  -0.2247f, 0.0000f, -0.0420f, 1.0419f,
                      0.0000f,  -0.0197f, -0.0786f, 1.0979f};
  const size_t n = 4096;
  std::vector<float> in = MakeRGB(n);
  std::vector<float> out(in.size());
  imageproc::Mat3MulRGBf(in.data(), out.data(), n, M);
  bool ok = true;
  for (size_t i = 0; i < n; ++i) {
    float r = in[3 * i + 0], g = in[3 * i + 1], b = in[3 * i + 2];
    float e0 = M[0] * r + M[1] * g + M[2] * b;
    float e1 = M[3] * r + M[4] * g + M[5] * b;
    float e2 = M[6] * r + M[7] * g + M[8] * b;
    if (std::fabs(out[3 * i + 0] - e0) > 1e-5f ||
        std::fabs(out[3 * i + 1] - e1) > 1e-5f ||
        std::fabs(out[3 * i + 2] - e2) > 1e-5f) {
      ok = false;
      break;
    }
  }
  TEST_CHECK(ok);
}

void imageproc_pack_channels_test(void) {
  const size_t n = 333;
  // Three inputs: a single-channel gloss, an RGB map, and a 2-ch GA map.
  std::vector<uint8_t> gloss(n), rgb(n * 3), ga(n * 2);
  for (size_t i = 0; i < n; ++i) {
    gloss[i] = uint8_t((i * 7) & 0xFF);
    rgb[i * 3 + 0] = uint8_t(i & 0xFF);
    rgb[i * 3 + 1] = uint8_t((i * 3) & 0xFF);
    rgb[i * 3 + 2] = uint8_t((i * 5) & 0xFF);
    ga[i * 2 + 0] = uint8_t((i * 11) & 0xFF);
    ga[i * 2 + 1] = uint8_t((i * 13) & 0xFF);
  }
  // out.R = gloss.R, out.G = rgb.G, out.B = ga.A(ch1), out.A = constant 200.
  imageproc::PackSource srcs[4];
  srcs[0].in = gloss.data(); srcs[0].in_stride = 1; srcs[0].channel = 0;
  srcs[1].in = rgb.data();   srcs[1].in_stride = 3; srcs[1].channel = 1;
  srcs[2].in = ga.data();    srcs[2].in_stride = 2; srcs[2].channel = 1;
  srcs[3].in = nullptr;      srcs[3].constant = 200;

  std::vector<uint8_t> out(n * 4, 0xEE);
  imageproc::PackChannels8(out.data(), n, 4, srcs);

  bool ok = true;
  for (size_t x = 0; x < n; ++x) {
    if (out[x * 4 + 0] != gloss[x]) { ok = false; break; }
    if (out[x * 4 + 1] != rgb[x * 3 + 1]) { ok = false; break; }
    if (out[x * 4 + 2] != ga[x * 2 + 1]) { ok = false; break; }
    if (out[x * 4 + 3] != 200) { ok = false; break; }
  }
  TEST_CHECK(ok);  // dispatched kernel matches the scalar gather exactly
}

void imageproc_simd_level_test(void) {
  imageproc::SimdLevel lvl = imageproc::ActiveSimdLevel();
  const char *name = imageproc::ToString(lvl);
  TEST_CHECK(name != nullptr);
  // In-place aliasing must also work (in == out buffer).
  const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::vector<float> buf = MakeRGB(64);
  std::vector<float> ref = buf;
  imageproc::Mat3MulRGBf(buf.data(), buf.data(), 64, I);
  bool ok = true;
  for (size_t i = 0; i < buf.size(); ++i)
    if (buf[i] != ref[i]) { ok = false; break; }
  TEST_CHECK(ok);
}


void imageproc_hdr_flat_complete_test(void) {
  const std::string header="#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 32 +X 64\n";
  std::vector<uint8_t> bytes(header.begin(),header.end());
  for(size_t i=0;i<64*32;++i){
    bytes.push_back(128);bytes.push_back(255);bytes.push_back(64);bytes.push_back(i%3?129:0);
  }
  auto decoded=lightusd::image::LoadImageFromMemory(bytes.data(),bytes.size(),"flat.hdr");
  TEST_ASSERT(decoded.has_value());
  const auto& image=decoded.value().image;
  TEST_CHECK(image.width==64 && image.height==32 && image.channels==4 && image.bpp==32);
  TEST_ASSERT(image.data.size()==64*32*4*sizeof(float));
  for(size_t i=0;i<64*32;++i){
    float values[4];std::memcpy(values,image.data.data()+i*4*sizeof(float),sizeof(values));
    TEST_CHECK(values[0]==(i%3?1.f:0.f));
    TEST_CHECK(values[1]==(i%3?255.f/128.f:0.f));
    TEST_CHECK(values[2]==(i%3?.5f:0.f));
    TEST_CHECK(values[3]==1.f);
  }
  for(size_t missing:{size_t(1),size_t(4),size_t(16)})
    TEST_CHECK(!lightusd::image::LoadImageFromMemory(bytes.data(),bytes.size()-missing,"truncated.hdr"));
}
