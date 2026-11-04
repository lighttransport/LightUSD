// SPDX-License-Identifier: Apache-2.0
// Minimal, lazy Ptex container reader used by the renderers. A thin C++
// wrapper over the portable C11 core in src/ptex/lptex.h (see doc/ptex.md).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "io-util.hh"

struct lptex_reader;

namespace lightusd {
namespace ptx {

enum class DataType : uint32_t { UInt8 = 0, UInt16 = 1, Half = 2, Float = 3 };

struct FaceInfo {
  uint8_t ures = 0;  // log2 horizontal resolution
  uint8_t vres = 0;  // log2 vertical resolution
  uint8_t adjEdges = 0;
  uint8_t flags = 0;
  uint32_t adjFaces[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
                          0xffffffffu};
  bool constant() const { return (flags & 1u) != 0; }
  uint32_t width() const { return 1u << ures; }
  uint32_t height() const { return 1u << vres; }
};

struct Info {
  DataType dataType = DataType::UInt8;
  uint32_t meshType = 1;  // 0 triangle, 1 quad
  uint32_t alphaChannel = 0xffffffffu;
  uint16_t channels = 0;
  uint16_t levels = 0;
  uint32_t faces = 0;
  std::vector<FaceInfo> faceInfo;
};

struct FaceImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t channels = 0;
  DataType dataType = DataType::UInt8;
  // Channels are interleaved, v-major, matching the Ptex specification.
  std::vector<uint8_t> data;
};

class Reader {
 public:
  Reader() = default;
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  static bool OpenFile(const std::string& path, Reader* out, std::string* err);
  static bool OpenMemory(const uint8_t* data, size_t size, Reader* out,
                         std::string* err);

  const Info& info() const { return info_; }

  // Decode one face at one mip level. Only the requested face is inflated;
  // source bytes remain owned by the Reader. max_bytes bounds the destination.
  bool ReadFace(uint32_t face, uint32_t level, size_t max_bytes,
                FaceImage* out, std::string* err) const;

 private:
  lptex_reader* reader_ = nullptr;
  std::unique_ptr<io::MMapFileHandle> mapped_file_;
  Info info_;

  bool Attach(const uint8_t* data, size_t size, std::string* err);
  void ResetSource();
};

}  // namespace ptx
}  // namespace lightusd
