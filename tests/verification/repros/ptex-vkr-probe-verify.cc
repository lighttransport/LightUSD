// SPDX-License-Identifier: Apache-2.0
// Vulkan descriptor-path Ptex oracle: verify per-pixel Ptex base-colour samples
// (face, u, v) against OpenPtex.
//
// Input (text), written by tools/lusdrender/check_vkr_ptex.py from two raw-float
// dumps of the renderer (-vkDumpF32 with -rtDebugAov ptex and
// =base):
//   # texture <path to .ptx>
//   # srgb <0|1>
//   <face> <u> <v> <r> <g> <b>        one line per visible Ptex pixel
//
// The expected value re-samples the same (face, u, v) from the OpenPtex-decoded
// level-0 page with the within-face bilinear the shader uses: texel centers at
// (i+0.5)/size, v measured from the bottom (Ptex), taps clamped to the face. It
// does not model cross-face blending (a gutter atlas does not do it), so it is
// exact for the shader and an approximation of a full Ptex filter near edges.
//
// Build (OpenPtex 2.3 or 2.4 sources; the driver compiles them directly):
//   g++ -std=c++17 -O2 -pthread -I<ptex-src>/src/ptex ptex-vkr-probe-verify.cc \
//       <ptex-src>/src/ptex/Ptex{Cache,Filters,Half,MemoryReader,Reader,Utils,Writer,\
//       SeparableFilter,SeparableKernel,TriangleFilter,TriangleKernel}.cpp -lz
// Exit: 0 = pass, 1 = mismatch, 2 = usage/open error.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Ptexture.h"

namespace {
float SrgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <probe.txt> [tolerance]\n", argv[0]);
    return 2;
  }
  const float tolerance = argc > 2 ? std::atof(argv[2]) : 0.01f;
  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::string path;
  int srgb = 1;
  struct Rec { int face; float u, v, rgb[3]; };
  std::vector<Rec> recs;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("# texture ", 0) == 0) { path = line.substr(10); continue; }
    if (line.rfind("# srgb ", 0) == 0) { srgb = std::atoi(line.c_str() + 7); continue; }
    if (line.empty() || line[0] == '#') continue;
    Rec r{};
    std::istringstream ss(line);
    if (ss >> r.face >> r.u >> r.v >> r.rgb[0] >> r.rgb[1] >> r.rgb[2]) recs.push_back(r);
  }
  if (path.empty() || recs.empty()) {
    std::fprintf(stderr, "probe has no texture or no records\n");
    return 2;
  }
#if defined(PtexLibraryMinorVersion) && PtexLibraryMinorVersion >= 4
  Ptex::String err;  // OpenPtex 2.4+
#else
  std::string err;   // OpenPtex <= 2.3
#endif
  PtexTexture* tex = PtexTexture::open(path.c_str(), err, true);
  if (!tex) {
    std::fprintf(stderr, "OpenPtex cannot open %s: %s\n", path.c_str(), err.c_str());
    return 2;
  }
  const int ch = tex->numChannels();
  if (tex->dataType() != Ptex::dt_uint8) {
    std::fprintf(stderr, "note: data type %d (not uint8); 8-bit atlas quantization applies\n",
                 int(tex->dataType()));
  }
  size_t bad = 0;
  double worst = 0.0;
  size_t worst_i = 0;
  std::vector<std::vector<float>> pages(size_t(tex->numFaces()));
  std::vector<int> pw(size_t(tex->numFaces()), 0), ph(size_t(tex->numFaces()), 0);
  auto page = [&](int face) -> const std::vector<float>* {
    if (face < 0 || face >= tex->numFaces()) return nullptr;
    std::vector<float>& p = pages[size_t(face)];
    if (p.empty()) {
      const Ptex::FaceInfo& fi = tex->getFaceInfo(face);
      const int w = fi.res.u(), h = fi.res.v();
      std::vector<float> data(size_t(w) * h * ch, 0.0f);
      // getData writes the native data type; read native and normalise.
      std::vector<uint8_t> raw(size_t(w) * h * ch * Ptex::DataSize(tex->dataType()));
      tex->getData(face, raw.data(), w * ch * Ptex::DataSize(tex->dataType()), fi.res);
      for (size_t i = 0; i < size_t(w) * h * ch; ++i) {
        if (tex->dataType() == Ptex::dt_uint8) data[i] = raw[i] / 255.0f;
        else if (tex->dataType() == Ptex::dt_float) data[i] = reinterpret_cast<float*>(raw.data())[i];
        else if (tex->dataType() == Ptex::dt_uint16) data[i] = reinterpret_cast<uint16_t*>(raw.data())[i] / 65535.0f;
        else data[i] = 0.0f;
      }
      p = std::move(data);
      pw[size_t(face)] = w;
      ph[size_t(face)] = h;
    }
    return &p;
  };
  // Expected colour of (face, u, v) with the within-face bilinear. `decode` applies
  // the sRGB->linear conversion; used by the main check and the diagnostics below.
  auto expected = [&](int face, float u, float v, bool decode, float out3[3]) {
    const std::vector<float>* p = page(face);
    if (!p) return false;
    const int w = pw[size_t(face)], h = ph[size_t(face)];
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(w) - 0.5f;
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(h) - 0.5f;
    // Taps are floor/floor+1 of the continuous coordinate, each clamped to the
    // face (so a coordinate left of the first center reads texel 0 twice).
    const int ix0 = int(std::floor(fx)), iy0 = int(std::floor(fy));
    const int x0 = std::clamp(ix0, 0, w - 1), x1 = std::clamp(ix0 + 1, 0, w - 1);
    const int y0 = std::clamp(iy0, 0, h - 1), y1 = std::clamp(iy0 + 1, 0, h - 1);
    const float tx = fx - std::floor(fx);
    const float ty = fy - std::floor(fy);
    for (int c = 0; c < 3; ++c) {
      const int cc = std::min(c, ch - 1);
      auto t = [&](int x, int y) { return (*p)[(size_t(y) * w + x) * ch + cc]; };
      const float val = (t(x0, y0) * (1 - tx) + t(x1, y0) * tx) * (1 - ty) +
                        (t(x0, y1) * (1 - tx) + t(x1, y1) * tx) * ty;
      out3[c] = decode ? SrgbToLinear(val) : val;
    }
    return true;
  };
  for (size_t i = 0; i < recs.size(); ++i) {
    const Rec& r = recs[i];
    float exp3[3];
    if (!expected(r.face, r.u, r.v, srgb != 0, exp3)) { ++bad; continue; }
    double e = 0.0;
    for (int c = 0; c < 3; ++c) e = std::max(e, double(std::fabs(exp3[c] - r.rgb[c])));
    if (e > worst) { worst = e; worst_i = i; }
    if (e > tolerance) ++bad;
  }
  if (std::getenv("PTEX_VERIFY_BAD")) {
    size_t shown = 0;
    std::map<std::pair<int, int>, size_t> by_res, total_by_res;
    for (const Rec& r : recs) {
      float e3[3];
      if (!expected(r.face, r.u, r.v, srgb != 0, e3)) continue;
      double e = 0.0;
      for (int c = 0; c < 3; ++c) e = std::max(e, double(std::fabs(e3[c] - r.rgb[c])));
      const std::pair<int, int> res{pw[size_t(r.face)], ph[size_t(r.face)]};
      ++total_by_res[res];
      if (e > tolerance) {
        ++by_res[res];
        if (shown++ < size_t(std::atoi(std::getenv("PTEX_VERIFY_BAD"))))
          std::printf("  BAD face %d (%dx%d) u %.3f v %.3f err %.4f expected %.4f %.4f %.4f actual %.4f %.4f %.4f\n",
                      r.face, res.first, res.second, double(r.u), double(r.v), e, double(e3[0]), double(e3[1]),
                      double(e3[2]), double(r.rgb[0]), double(r.rgb[1]), double(r.rgb[2]));
      }
    }
    for (const auto& kv : total_by_res)
      std::printf("  face res %dx%d: %zu samples, %zu bad\n", kv.first.first, kv.first.second, kv.second, by_res[kv.first]);
  }
  if (std::getenv("PTEX_VERIFY_LIST")) {
    for (size_t i = 0; i < recs.size() && i < size_t(std::atoi(std::getenv("PTEX_VERIFY_LIST"))); i += std::max<size_t>(1, recs.size() / 24)) {
      const Rec& r = recs[i];
      float e3[3];
      if (!expected(r.face, r.u, r.v, srgb != 0, e3)) continue;
      std::printf("  face %5d u %.3f v %.3f  expected %.4f %.4f %.4f  actual %.4f %.4f %.4f  ratio %.3f %.3f %.3f\n", r.face, double(r.u), double(r.v),
                  double(e3[0]), double(e3[1]), double(e3[2]), double(r.rgb[0]), double(r.rgb[1]), double(r.rgb[2]),
                  double(r.rgb[0] / std::max(e3[0], 1e-6f)), double(r.rgb[1] / std::max(e3[1], 1e-6f)), double(r.rgb[2] / std::max(e3[2], 1e-6f)));
    }
  }
  if (std::getenv("PTEX_VERIFY_VARIANTS")) {
    struct V { const char* name; int flipu, flipv, swap, decode; };
    const V vs[] = {{"as-is", 0, 0, 0, 1}, {"flip v", 0, 1, 0, 1}, {"flip u", 1, 0, 0, 1},
                    {"swap uv", 0, 0, 1, 1}, {"swap+flip v", 0, 1, 1, 1}, {"swap+flip u", 1, 0, 1, 1},
                    {"as-is no-srgb", 0, 0, 0, 0}, {"flip v no-srgb", 0, 1, 0, 0}};
    for (const V& vr : vs) {
      double sum = 0.0;
      size_t n = 0;
      for (const Rec& r : recs) {
        float u = vr.swap ? r.v : r.u, v = vr.swap ? r.u : r.v;
        if (vr.flipu) u = 1.0f - u;
        if (vr.flipv) v = 1.0f - v;
        float e3[3];
        if (!expected(r.face, u, v, vr.decode != 0, e3)) continue;
        double e = 0.0;
        for (int c = 0; c < 3; ++c) e = std::max(e, double(std::fabs(e3[c] - r.rgb[c])));
        sum += e; ++n;
      }
      std::printf("  variant %-16s mean max-channel error %.5f\n", vr.name, n ? sum / double(n) : -1.0);
    }
  }
  const Rec& w = recs[worst_i];
  std::printf("%zu samples, %zu outside %.4g, worst %.5f (face %d u %.4f v %.4f rgb %.4f %.4f %.4f)\n",
              recs.size(), bad, double(tolerance), worst, w.face, double(w.u), double(w.v),
              double(w.rgb[0]), double(w.rgb[1]), double(w.rgb[2]));
  tex->release();
  return bad == 0 ? 0 : 1;
}
