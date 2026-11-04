// SPDX-License-Identifier: Apache-2.0
// Optional independent-decoder check for the CPU Ptex mip sampler.
//
// Build (OpenPtex 2.4.3 source at /tmp/gap-ptex-upstream, Debug library already
// built by the Ptex decoder report):
//   gcc -c -O1 -I<repo>/src/external <repo>/src/external/miniz.c -o miniz.o
//   gcc -std=c11 -c -O2 <repo>/src/ptex/lptex.c <repo>/src/ptex/lptex-sample.c
//       <repo>/src/ptex/lptex-file.c   (and link the three .o files below)
//   g++ -std=c++17 -O2 -I<repo>/src -I<repo>/examples/lusdview //       -I<ptex-src>/src/ptex ptex-mip-compare.cc //       <repo>/src/ptx-loader.cc <repo>/src/io-util.cc miniz.o //       -L<ptex-build>/src/ptex -lPtex -lz -o ptex-mip-ref
//   LD_LIBRARY_PATH=<ptex-build>/src/ptex ./ptex-mip-ref \
//       /mnt/nvme02/data/island/textures/isMountainA/Color/mountain_geo.ptx
//
// The CI-friendly fixture form is RunCpuPtexMipSampler() in
// examples/lusdview/tests/ptex_atlas_test.cc, which compares the same faces
// against tests/usdc/ptex-mip/.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Ptexture.h"
#include "cpu/ptex-sample.hh"

using lusdview::CpuPtexSampler;

static bool CompareFace(CpuPtexSampler* sampler, PtexTexture* ref,
                        uint32_t face, uint32_t mip, std::string* err) {
  const uint8_t* raw = nullptr;
  const auto* page = sampler->RawFace(face, mip, &raw, err);
  if (!page) return false;
  if (page->dataType != lightusd::ptx::DataType::UInt8) return false;
  Ptex::FaceInfo fi = ref->getFaceInfo(int(face));
  Ptex::Res res(std::max<int>(0, fi.res.ulog2 - int(mip)),
                std::max<int>(0, fi.res.vlog2 - int(mip)));
  const uint32_t w = 1u << res.ulog2;
  const uint32_t h = 1u << res.vlog2;
  const int channels = ref->numChannels();
  if (w != page->width || h != page->height || channels != page->channels) {
    std::fprintf(stderr, "face %u mip %u dims ref %ux%u/%d vs %ux%u/%d\n",
                 face, mip, w, h, channels, page->width, page->height,
                 page->channels);
    return false;
  }
  std::vector<uint8_t> decoded(size_t(w) * h * channels, 0);
  ref->getData(int(face), decoded.data(), w * channels, res);
  if (std::memcmp(decoded.data(), raw, decoded.size()) != 0) {
    size_t bad = 0;
    for (size_t i = 0; i < decoded.size(); ++i)
      if (decoded[i] != raw[i]) ++bad;
    std::fprintf(stderr, "face %u mip %u: %zu/%zu bytes differ\n", face, mip,
                 bad, decoded.size());
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const char* path = argv[1];
  std::string err;
  CpuPtexSampler sampler(4ull * 1024ull * 1024ull);
  if (!sampler.OpenFile(path, &err)) {
    std::fprintf(stderr, "sampler open failed: %s\n", err.c_str());
    return 2;
  }
  Ptex::String refErr;
  PtexTexture* ref = PtexTexture::open(path, refErr, false);
  if (!ref) {
    std::fprintf(stderr, "reference open failed: %s\n", refErr.c_str());
    return 2;
  }
  const auto& info = sampler.info();
  std::printf("faces=%u levels=%u channels=%u\n", info.faces, info.levels,
              info.channels);
  uint32_t faces = 0, mips = 0;
  for (uint32_t face = 0; face < std::min<uint32_t>(info.faces, 8); ++face) {
    for (uint32_t mip = 1; mip < info.levels; ++mip) {
      if (!CompareFace(&sampler, ref, face, mip, &err)) {
        std::fprintf(stderr, "FAIL face %u mip %u: %s\n", face, mip,
                     err.c_str());
        ref->release();
        return 1;
      }
      ++mips;
    }
    ++faces;
  }
  ref->release();

  // Mip selection: a 32-texel face maps a 2-texel footprint to mip 1 and an
  // 8-texel footprint to mip 3 (clamped by the file's 4 levels).
  bool mip_ok = true;
  const float inv = 1.0f / 32.0f;
  const struct { float uv; uint32_t expect; } cases[] = {
      {0.0f, 0}, {inv, 0}, {2.0f * inv, 1}, {4.0f * inv, 2}, {8.0f * inv, 3},
  };
  for (const auto& c : cases) {
    const uint32_t got = sampler.ChooseMip(0, c.uv);
    if (got != c.expect) {
      std::fprintf(stderr, "ChooseMip(0, %.4f)=%u expected %u\n", c.uv, got,
                   c.expect);
      mip_ok = false;
    }
  }

  // Streamed residency: a 4 KiB budget holds two 32x16x3 pages (1536 B), so
  // fetching four faces evicts at least one.
  CpuPtexSampler small(4ull * 1024ull);
  if (!small.OpenFile(path, &err)) return 2;
  for (uint32_t face = 0; face < 4 && face < info.faces; ++face) {
    const uint8_t* raw = nullptr;
    small.RawFace(face, 0, &raw, &err);
  }
  const auto& st = small.stats();
  const bool cache_ok = st.evictions > 0 && st.residentBytes <= 4ull * 1024ull &&
                        st.peakResidentBytes <= 4ull * 1024ull;
  std::printf("compared %u faces x %u non-base mips; mip-select=%s "
              "cache evictions=%llu resident=%llu peak=%llu\n",
              faces, mips, mip_ok ? "PASS" : "FAIL",
              (unsigned long long)st.evictions,
              (unsigned long long)st.residentBytes,
              (unsigned long long)st.peakResidentBytes);
  if (!mip_ok || !cache_ok) return 1;
  std::printf("PASS: CPU Ptex mip sampler matches OpenPtex and bounds residency\n");
  return 0;
}
