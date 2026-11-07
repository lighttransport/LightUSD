// SPDX-License-Identifier: Apache-2.0
// C++ wrapper over the vendored C11 Ptex library (external/textools/ptex, tptex).
// All container parsing and decoding lives there; this file only adapts
// ownership (mmap handle, std::vector images) and turns status codes into
// messages. Readers are opened TPTEX_OPEN_NO_LAZY, so ReadFace() is read-only
// and may be called concurrently on one Reader.
#include "ptx-loader.hh"

#include <cstdint>

#include "external/textools/ptex/include/tptex.h"
#ifndef TPTEX_NO_FILEIO
#include "external/textools/ptex/include/tptex_file.h"
#endif

namespace lightusd {
namespace ptx {
namespace {

bool Fail(std::string* err, const char* msg) {
  if (err) *err = msg;
  return false;
}
bool Fail(std::string* err, tptex_status st) {
  return Fail(err, tptex_status_str(st));
}

tptex_options Options() {
  tptex_options o;
  tptex_options_init(&o);
  o.flags = TPTEX_OPEN_NO_LAZY;
  return o;
}

bool CopyInfo(tptex_texture* tex, Info* out) {
  const tptex_info* in = tptex_get_info(tex);
  *out = Info();
  out->dataType = static_cast<DataType>(in->data_type);
  out->meshType = static_cast<uint32_t>(in->mesh_type);
  out->alphaChannel = static_cast<uint32_t>(in->alpha_channel);  // -1 -> 0xffffffff
  out->channels = static_cast<uint16_t>(in->num_channels);
  out->levels = static_cast<uint16_t>(in->num_levels);
  out->faces = static_cast<uint32_t>(in->num_faces);
  out->faceInfo.resize(out->faces);
  for (uint32_t i = 0; i < out->faces; ++i) {
    tptex_face_info s;
    if (tptex_get_face_info(tex, static_cast<int>(i), &s) != TPTEX_OK) return false;
    FaceInfo& d = out->faceInfo[i];
    d.ures = static_cast<uint8_t>(s.res.ulog2);
    d.vres = static_cast<uint8_t>(s.res.vlog2);
    d.adjEdges = s.adjedges;
    d.flags = s.flags;
    for (int e = 0; e < 4; ++e)
      d.adjFaces[e] = s.adjfaces[e] < 0 ? 0xffffffffu : static_cast<uint32_t>(s.adjfaces[e]);
  }
  return true;
}

}  // namespace

Reader::~Reader() { ResetSource(); }

void Reader::ResetSource() {
  tptex_close(reader_);
  reader_ = nullptr;
  if (mapped_file_) {
    std::string ignored;
    io::UnmapFile(*mapped_file_, &ignored);
    mapped_file_.reset();
  }
  info_ = Info();
}

bool Reader::Attach(const uint8_t* data, size_t size, std::string* err) {
  const tptex_options opt = Options();
  const tptex_status st = tptex_open_memory(data, size, &opt, &reader_);
  if (st != TPTEX_OK) return Fail(err, st);
  if (!CopyInfo(reader_, &info_)) return Fail(err, TPTEX_E_CORRUPT);
  return true;
}

bool Reader::OpenFile(const std::string& path, Reader* out, std::string* err) {
  if (!out) return Fail(err, "Ptex output reader is null");
  out->ResetSource();

  // PTEX containers can be several GiB. Mapping keeps the compressed source
  // out of the heap and lets the kernel fault in only the metadata and face
  // blocks that are actually touched.
  auto mapped = std::make_unique<io::MMapFileHandle>();
  std::string mmapError;
  if (io::MMapFile(path, mapped.get(), false, &mmapError)) {
    if (mapped->size > 0 && mapped->size <= (16ull << 30)) {
      const uint8_t* data = mapped->addr;
      const size_t size = static_cast<size_t>(mapped->size);
      out->mapped_file_ = std::move(mapped);
      if (out->Attach(data, size, err)) return true;
      out->ResetSource();
      return false;
    }
    std::string ignored;
    io::UnmapFile(*mapped, &ignored);
  }

#ifndef TPTEX_NO_FILEIO
  // Portability fallback (filesystems that refuse mmap, restricted builds):
  // the C library reads blocks on demand from the file.
  const tptex_options opt = Options();
  const tptex_status st = tptex_open_file(path.c_str(), &opt, &out->reader_);
  if (st != TPTEX_OK) return Fail(err, st);
  if (!CopyInfo(out->reader_, &out->info_)) {
    out->ResetSource();
    return Fail(err, TPTEX_E_CORRUPT);
  }
  return true;
#else
  return Fail(err, "cannot open Ptex file");
#endif
}

bool Reader::OpenMemory(const uint8_t* data, size_t size, Reader* out,
                        std::string* err) {
  if (!out || (!data && size)) return Fail(err, TPTEX_E_INVALID_ARG);
  out->ResetSource();
  if (out->Attach(data, size, err)) return true;
  out->ResetSource();
  return false;
}

bool Reader::ReadFace(uint32_t face, uint32_t level, size_t max_bytes,
                      FaceImage* out, std::string* err) const {
  if (!out || !reader_ || face >= info_.faces || level >= info_.levels)
    return Fail(err, "invalid Ptex face or level");
  // Level L is the face at half resolution L times (each axis clamped at 1
  // texel). Stored reductions are used when present; others are generated.
  const FaceInfo& fi = info_.faceInfo[face];
  const int ul = fi.ures > level ? static_cast<int>(fi.ures - level) : 0;
  const int vl = fi.vres > level ? static_cast<int>(fi.vres - level) : 0;
  const uint32_t w = 1u << ul, h = 1u << vl;
  const size_t pixel = tptex_pixel_size(reader_);
  const uint64_t bytes64 = static_cast<uint64_t>(w) * h * pixel;
  if (bytes64 > static_cast<uint64_t>(max_bytes) ||
      bytes64 > static_cast<uint64_t>(SIZE_MAX))
    return Fail(err, "Ptex face exceeds the decode budget");
  const size_t bytes = static_cast<size_t>(bytes64);
  out->width = w;
  out->height = h;
  out->channels = info_.channels;
  out->dataType = info_.dataType;
  out->data.resize(bytes);
  const tptex_status st = tptex_get_data(reader_, static_cast<int>(face),
                                         tptex_make_res(ul, vl), out->data.data(), 0);
  if (st != TPTEX_OK) {
    out->data.clear();
    return Fail(err, st);
  }
  return true;
}

}  // namespace ptx
}  // namespace lightusd
