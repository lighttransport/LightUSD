// SPDX-License-Identifier: Apache-2.0
// C++ wrapper over the C11 Ptex core (src/ptex/lptex.h). All container
// parsing and decoding lives in lptex.c; this file only adapts ownership
// (mmap handle, std::vector images) and turns status codes into messages.
#include "ptx-loader.hh"

#include <cstdint>

#include "ptex/lptex.h"

namespace lightusd {
namespace ptx {
namespace {

bool Fail(std::string* err, const char* msg) {
  if (err) *err = msg;
  return false;
}
bool Fail(std::string* err, lptex_status st) {
  return Fail(err, lptex_status_str(st));
}

void CopyInfo(const lptex_info& in, Info* out) {
  *out = Info();
  out->dataType = static_cast<DataType>(in.type);
  out->meshType = in.mesh_type;
  out->alphaChannel = in.alpha_channel;
  out->channels = in.channels;
  out->levels = in.levels;
  out->faces = in.faces;
  out->faceInfo.resize(in.faces);
  for (uint32_t i = 0; i < in.faces; ++i) {
    const lptex_face_info& s = in.face_info[i];
    FaceInfo& d = out->faceInfo[i];
    d.ures = s.ures;
    d.vres = s.vres;
    d.adjEdges = s.adj_edges;
    d.flags = s.flags;
    for (int e = 0; e < 4; ++e) d.adjFaces[e] = s.adj_faces[e];
  }
}

}  // namespace

Reader::~Reader() { ResetSource(); }

void Reader::ResetSource() {
  lptex_close(reader_);
  reader_ = nullptr;
  if (mapped_file_) {
    std::string ignored;
    io::UnmapFile(*mapped_file_, &ignored);
    mapped_file_.reset();
  }
  info_ = Info();
}

bool Reader::Attach(const uint8_t* data, size_t size, std::string* err) {
  const lptex_status st = lptex_open_memory(data, size, nullptr, &reader_);
  if (st != LPTEX_OK) return Fail(err, st);
  CopyInfo(*lptex_reader_info(reader_), &info_);
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

#ifndef LPTEX_NO_FILEIO
  // Portability fallback (filesystems that refuse mmap, restricted builds):
  // the C core reads the file into reader-owned memory.
  const lptex_status st = lptex_open_file(path.c_str(), nullptr, &out->reader_);
  if (st != LPTEX_OK) return Fail(err, st);
  CopyInfo(*lptex_reader_info(out->reader_), &out->info_);
  return true;
#else
  return Fail(err, LPTEX_E_OPEN_FAILED);
#endif
}

bool Reader::OpenMemory(const uint8_t* data, size_t size, Reader* out,
                        std::string* err) {
  if (!out || (!data && size)) return Fail(err, LPTEX_E_INVALID_ARGUMENT);
  out->ResetSource();
  if (out->Attach(data, size, err)) return true;
  out->ResetSource();
  return false;
}

bool Reader::ReadFace(uint32_t face, uint32_t level, size_t max_bytes,
                      FaceImage* out, std::string* err) const {
  if (!out || !reader_) return Fail(err, LPTEX_E_INVALID_FACE_LEVEL);
  uint32_t w = 0, h = 0;
  size_t bytes = 0;
  lptex_status st = lptex_face_dims(reader_, face, level, &w, &h, &bytes);
  if (st != LPTEX_OK) return Fail(err, st);
  if (bytes > max_bytes) return Fail(err, LPTEX_E_DECODE_BUDGET);
  out->width = w;
  out->height = h;
  out->channels = info_.channels;
  out->dataType = info_.dataType;
  out->data.resize(bytes);
  st = lptex_read_face_into(reader_, face, level, max_bytes, out->data.data(),
                            out->data.size());
  if (st != LPTEX_OK) {
    out->data.clear();
    return Fail(err, st);
  }
  return true;
}

}  // namespace ptx
}  // namespace lightusd
