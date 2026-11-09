// SPDX-License-Identifier: Apache-2.0
// Standalone OpenPtex control: no LightUSD headers or libraries are used.
// Build against the tested OpenPtex headers/library, then run:
//   ./ptex-constant-reference write /tmp/constant.ptx
//   ./ptex-constant-reference read /tmp/constant.ptx
#include <cstdio>
#include <cstring>

#include "Ptexture.h"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const unsigned char expected[3] = {17, 83, 201};
  Ptex::String error;
  if (std::strcmp(argv[1], "write") == 0) {
    PtexWriter* writer = PtexWriter::open(
        argv[2], Ptex::mt_quad, Ptex::dt_uint8, 3, -1, 1, error, true);
    if (!writer) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    bool ok = writer->writeFace(0, Ptex::FaceInfo(Ptex::Res(0, 0)), expected);
    ok = writer->close(error) && ok;
    writer->release();
    if (!ok) std::fprintf(stderr, "%s\n", error.c_str());
    return ok ? 0 : 1;
  }
  if (std::strcmp(argv[1], "read") != 0) return 2;
  PtexTexture* texture = PtexTexture::open(argv[2], error, false);
  if (!texture) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  if (texture->numFaces() != 1 || texture->numChannels() != 3 ||
      texture->dataType() != Ptex::dt_uint8) {
    texture->release();
    return 1;
  }
  unsigned char actual[3] = {};
  texture->getData(0, actual, 0, Ptex::Res(0, 0));
  texture->release();
  const bool ok = std::memcmp(actual, expected, sizeof(expected)) == 0;
  std::printf("constant RGB: %u %u %u; %s\n", unsigned(actual[0]),
              unsigned(actual[1]), unsigned(actual[2]), ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
