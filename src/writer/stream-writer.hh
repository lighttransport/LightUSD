// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - StreamWriter: a buffered output sink for the USDA writer.
//
// Decouples the writer from std stdio / std::cout so it works where stdio is not
// natively available (WASM/WASI): the destination is a pluggable block-sink. The
// default native backend is C stdio (FILE*). Output is buffered and flushed in
// large blocks for disk throughput; a chunk bigger than the buffer is written
// straight through. A std::string target mode (no buffer copy) is used by the
// parallel subtree workers.
//
// The `operator<<` overloads (const char* / std::string / char) let the existing
// `os << ...` writer code target a StreamWriter with only a type change.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

class StreamWriter {
 public:
  // A block sink writes [data, data+n) to the destination; returns false on
  // error. Called for full buffers and oversized chunks ("blocked" I/O).
  using BlockSink = std::function<bool(const char* data, size_t n)>;

  // Sink-backed: buffer, flushing in `bufcap`-sized blocks.
  explicit StreamWriter(BlockSink sink, size_t bufcap = (1u << 20))
      : sink_(std::move(sink)), buf_(bufcap ? bufcap : 1) {}

  // String-backed fast path: append directly into *out (no buffer, no flush).
  explicit StreamWriter(std::string* out) : str_(out) {}

  ~StreamWriter() { flush(); }

  StreamWriter(const StreamWriter&) = delete;
  StreamWriter& operator=(const StreamWriter&) = delete;

  void write(const char* data, size_t n) {
    if (str_) { str_->append(data, n); total_ += n; return; }
    total_ += n;
    if (n >= buf_.size()) {  // oversized: flush pending + write straight through
      flush_buffer();
      if (ok_ && !sink_(data, n)) ok_ = false;
      return;
    }
    if (len_ + n > buf_.size()) flush_buffer();
    std::memcpy(buf_.data() + len_, data, n);
    len_ += n;
  }
  void write(const std::string& s) { write(s.data(), s.size()); }
  void write(const char* s) { write(s, std::strlen(s)); }
  void put(char c) {
    if (str_) { str_->push_back(c); ++total_; return; }
    if (len_ == buf_.size()) flush_buffer();
    buf_[len_++] = c;
    ++total_;
  }

  StreamWriter& operator<<(const char* s) { write(s); return *this; }
  StreamWriter& operator<<(const std::string& s) { write(s); return *this; }
  StreamWriter& operator<<(char c) { put(c); return *this; }

  // Drain the buffer to the sink. No-op in string mode.
  bool flush() {
    flush_buffer();
    return ok_;
  }
  bool good() const { return ok_; }
  uint64_t bytes_written() const { return total_; }

 private:
  void flush_buffer() {
    if (str_ || len_ == 0) return;
    if (ok_ && !sink_(buf_.data(), len_)) ok_ = false;
    len_ = 0;
  }

  std::string* str_ = nullptr;  // string-target mode (workers)
  BlockSink sink_;              // sink mode
  std::vector<char> buf_;
  size_t len_ = 0;
  uint64_t total_ = 0;
  bool ok_ = true;
};

// --- Default backends -------------------------------------------------------

// Native stdio: write blocks to an existing FILE* via fwrite. This is the
// default backend; on WASM/WASI a host supplies its own BlockSink instead.
inline StreamWriter::BlockSink StdioSink(std::FILE* fp) {
  return [fp](const char* data, size_t n) -> bool {
    return std::fwrite(data, 1, n, fp) == n;
  };
}

// Compatibility backend: write blocks to a std::ostream (keeps the existing
// std::ostream-based WriteUSDA overloads working).
inline StreamWriter::BlockSink OstreamSink(std::ostream& os) {
  return [&os](const char* data, size_t n) -> bool {
    os.write(data, static_cast<std::streamsize>(n));
    return static_cast<bool>(os);
  };
}

}  // namespace next
}  // namespace lightusd
