// SPDX-License-Identifier: Apache-2.0
// Optional standalone reference check. Links OpenPtex only in this executable;
// LightUSD's portable core keeps its dependency-free reader.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Ptexture.h"
#include "ptx-loader.hh"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  constexpr size_t budget = 32u * 1024u * 1024u;
  lightusd::ptx::Reader native;
  std::string error;
  if (!lightusd::ptx::Reader::OpenFile(argv[1], &native, &error)) {
    std::cerr << error << "\n";
    return 2;
  }
  Ptex::String reference_error;
  PtexTexture* reference = PtexTexture::open(argv[1], reference_error, false);
  if (!reference) {
    std::cerr << reference_error.c_str() << "\n";
    return 2;
  }
  const auto& info = native.info();
  bool failed = false, incomplete = false;
  size_t checked = 0;
  if (info.faces != uint32_t(reference->numFaces()) ||
      info.channels != reference->numChannels() ||
      uint32_t(info.dataType) != uint32_t(reference->dataType()))
    failed = true;
  std::cout << "{\"scope\":\"uniform sampled raw faces/mips, no filtering or "
               "colorspace conversion\",\"samples\":[";
  bool first = true;
  std::vector<uint32_t> faces;
  for (uint32_t i = 0; i < 16 && i < info.faces; ++i)
    faces.push_back(uint32_t(uint64_t(i) * (info.faces - 1) /
                             std::max(1u, std::min(16u, info.faces) - 1)));
  for (uint32_t face : faces)
    for (uint32_t level = 0; level < 3 && level < info.levels; ++level) {
      const auto& fi = reference->getFaceInfo(int(face));
      lightusd::ptx::FaceImage decoded;
      const bool decoded_ok =
          native.ReadFace(face, level, budget, &decoded, &error);
      size_t mismatches = 0;
      std::string status = "PASS";
      if (!decoded_ok) {
        status = "UNSUPPORTED";
        incomplete = true;
        std::cerr << "face=" << face << " level=" << level << " " << error
                  << "\n";
      } else {
        const int ul = std::max(0, int(fi.res.ulog2) - int(level));
        const int vl = std::max(0, int(fi.res.vlog2) - int(level));
        Ptex::Res resolution{int8_t(ul), int8_t(vl)};
        const size_t bytes = size_t(resolution.size()) *
                             size_t(reference->numChannels()) *
                             size_t(Ptex::DataSize(reference->dataType()));
        if (bytes > budget) {
          status = "UNSUPPORTED";
          incomplete = true;
        } else {
          std::vector<uint8_t> expected(bytes);
          reference->getData(int(face), expected.data(), 0, resolution);
          if (decoded.width != uint32_t(resolution.u()) ||
              decoded.height != uint32_t(resolution.v()) ||
              decoded.data.size() != bytes)
            mismatches = std::max(decoded.data.size(), bytes) + 1;
          else
            for (size_t i = 0; i < bytes; ++i)
              if (expected[i] != decoded.data[i]) ++mismatches;
          if (mismatches) {
            status = "FAIL";
            failed = true;
          } else
            ++checked;
        }
      }
      if (!first) std::cout << ",";
      first = false;
      std::cout << "{\"face\":" << face << ",\"level\":" << level
                << ",\"status\":\"" << status
                << "\",\"mismatched_bytes\":" << mismatches << "}";
    }
  reference->release();
  std::cout << "],\"checked\":" << checked << ",\"passed\":"
            << (!failed && !incomplete && checked ? "true" : "false") << "}\n";
  return failed ? 1 : (incomplete || !checked ? 2 : 0);
}
