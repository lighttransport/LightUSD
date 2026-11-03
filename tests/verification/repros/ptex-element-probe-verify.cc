// SPDX-License-Identifier: Apache-2.0
// Element-scale visible-material oracle: verify the CPU preview's per-pixel
// PTex base-colour samples against OpenPtex.
//
// Reads a dump written by `LIGHTUSD_CPU_PTEX_PROBE=<file>` (one line per
// visible pixel: x y tex face mip srgb u v r g b) and re-samples the same
// (face, mip, u, v) with PtexTexture::getData using the same bilinear and
// sRGB->linear convention as lusdview/cpu/ptex-sample.hh.
//
// Build against OpenPtex 2.4.3 (Debug reference used by the decoder report):
//   g++ -std=c++17 -O2 -I<ptex-src>/src/ptex ptex-element-probe-verify.cc \
//       -L<ptex-build>/src/ptex -lPtex -lz -o ptex-element-verify
//   LD_LIBRARY_PATH=<ptex-build>/src/ptex ./ptex-element-verify probe.txt
//
// Exit codes: 0 = pass, 1 = mismatch, 2 = usage/open error.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Ptexture.h"

namespace {

float SrgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f
                       : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct RefTexture {
  PtexTexture* texture{nullptr};
  int channels{0};
};

bool SampleReference(RefTexture* ref, int face, int mip, float srgb, float u,
                     float v, float out[3]) {
  if (!ref || !ref->texture) return false;
  Ptex::FaceInfo fi = ref->texture->getFaceInfo(face);
  Ptex::Res res(std::max(0, fi.res.ulog2 - mip),
                std::max(0, fi.res.vlog2 - mip));
  const int w = 1 << res.ulog2;
  const int h = 1 << res.vlog2;
  const int ch = ref->channels;
  if (w <= 0 || h <= 0 || ch <= 0) return false;
  std::vector<uint8_t> data(size_t(w) * h * ch, 0);
  ref->texture->getData(face, data.data(), w * ch, res);

  const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
  const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
  const int x0 = std::clamp(int(std::floor(fx)), 0, w - 1);
  const int y0 = std::clamp(int(std::floor(fy)), 0, h - 1);
  const int x1 = std::min(x0 + 1, w - 1);
  const int y1 = std::min(y0 + 1, h - 1);
  const float tx = std::clamp(fx - std::floor(fx), 0.0f, 1.0f);
  const float ty = std::clamp(fy - std::floor(fy), 0.0f, 1.0f);
  auto texel = [&](int x, int y, float rgb[3]) {
    const size_t i = (size_t(y) * w + size_t(x)) * ch;
    const float r = float(data[i]) / 255.0f;
    const float g = ch > 1 ? float(data[i + 1]) / 255.0f : r;
    const float b = ch > 2 ? float(data[i + 2]) / 255.0f : r;
    rgb[0] = r; rgb[1] = g; rgb[2] = b;
  };
  float c00[3], c10[3], c01[3], c11[3];
  texel(x0, y0, c00); texel(x1, y0, c10);
  texel(x0, y1, c01); texel(x1, y1, c11);
  for (int c = 0; c < 3; ++c) {
    const float top = c00[c] * (1.0f - tx) + c10[c] * tx;
    const float bot = c01[c] * (1.0f - tx) + c11[c] * tx;
    float value = top * (1.0f - ty) + bot * ty;
    if (srgb > 0.5f) value = SrgbToLinear(value);
    out[c] = value;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <probe.txt> [tolerance]\n", argv[0]);
    return 2;
  }
  const float tolerance = argc > 2 ? std::atof(argv[2]) : 2.5e-3f;
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot open probe %s\n", argv[1]);
    return 2;
  }
  std::map<int, std::string> assets;
  std::map<int, RefTexture> refs;
  struct Record {
    int tex, face, mip, srgb;
    float u, v, rgb[3];
  };
  std::vector<Record> records;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("# texture ", 0) == 0) {
      int id = -1;
      std::string path;
      std::istringstream ss(line.substr(10));
      ss >> id >> path;
      if (id >= 0 && !path.empty()) assets[id] = path;
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    Record r{};
    int px = 0, py = 0;
    std::istringstream ss(line);
    if (!(ss >> px >> py >> r.tex >> r.face >> r.mip >> r.srgb >> r.u >>
          r.v >> r.rgb[0] >> r.rgb[1] >> r.rgb[2])) {
      continue;
    }
    records.push_back(r);
  }
  if (records.empty()) {
    std::fprintf(stderr, "FAIL: probe has no pixel records\n");
    return 1;
  }

  double max_error = 0.0, sum_error = 0.0;
  size_t compared = 0, failed = 0;
  std::map<int, size_t> face_count;
  std::map<int, size_t> tex_count;
  for (const Record& r : records) {
    auto asset = assets.find(r.tex);
    if (asset == assets.end()) continue;
    RefTexture& ref = refs[r.tex];
    if (!ref.texture) {
      Ptex::String err;
      ref.texture = PtexTexture::open(asset->second.c_str(), err, false);
      if (!ref.texture) {
        std::fprintf(stderr, "FAIL: cannot open %s: %s\n",
                     asset->second.c_str(), err.c_str());
        return 1;
      }
      ref.channels = ref.texture->numChannels();
    }
    float expected[3];
    if (!SampleReference(&ref, r.face, r.mip, float(r.srgb), r.u, r.v,
                         expected)) {
      ++failed;
      continue;
    }
    double local = 0.0;
    for (int c = 0; c < 3; ++c) {
      local = std::max(local, std::fabs(double(expected[c]) - r.rgb[c]));
    }
    max_error = std::max(max_error, local);
    sum_error += local;
    ++compared;
    ++face_count[r.face];
    ++tex_count[r.tex];
    if (local > tolerance) {
      if (failed < 5) {
        std::fprintf(stderr,
                     "mismatch tex=%d face=%d mip=%d uv=(%.6f,%.6f) "
                     "probe=(%.6f,%.6f,%.6f) ref=(%.6f,%.6f,%.6f)\n",
                     r.tex, r.face, r.mip, r.u, r.v, r.rgb[0], r.rgb[1],
                     r.rgb[2], expected[0], expected[1], expected[2]);
      }
      ++failed;
    }
  }
  for (auto& entry : refs) {
    if (entry.second.texture) entry.second.texture->release();
  }
  if (!compared) {
    std::fprintf(stderr, "FAIL: no probe record could be verified\n");
    return 1;
  }
  const double mean_error = sum_error / double(compared);
  std::printf("pixels=%zu textures=%zu faces=%zu max_error=%.6g mean=%.6g\n",
              compared, tex_count.size(), face_count.size(), max_error,
              mean_error);
  if (failed > 0) {
    std::fprintf(stderr, "FAIL: %zu/%zu samples exceed tolerance %.6g\n",
                 failed, compared, tolerance);
    return 1;
  }
  if (tex_count.size() < 2 || face_count.size() < 16) {
    std::fprintf(stderr,
                 "FAIL: element coverage too small (need >=2 textures and "
                 ">=16 distinct faces)\n");
    return 1;
  }
  std::printf("PASS: CPU element pixels match the independent Ptex decoder\n");
  return 0;
}
