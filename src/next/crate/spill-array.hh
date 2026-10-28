// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - SpillVec: an append-only column that can move its oldest
// elements to an unlinked spill file once a shared in-memory budget is
// exceeded.
//
// The CrateWriter's batched (low-profile) path appends prim/property info to
// column arrays for the whole scene (FIELDS tokens/value reps, FIELDSET
// indices, per-spec offsets and SPECS records). For multi-gigabyte scenes the
// accumulated columns dominate the writer's peak RSS, and only the section
// builders at the end need them back as one contiguous block. SpillVec keeps
// elements in ~8 MiB chunks; the writer discards fully committed chunks
// strictly from the front by appending their bytes to a file that is unlinked
// at creation (so any exit path removes it), and the section builders
// reconstruct the whole array with one sequential pread.
//
// Order invariant: elements are appended only at the back and evicted only
// from the front, so a column's absolute element identity (field ids, spec
// offsets) never changes; sizes are absolute across evictions.
//
// Spill only changes where bytes live, never their values, so callers that
// read a column through Resolve() see exactly the bytes they appended --
// output files are byte-identical to a fully in-memory write.
//
// The spill file goes to the directory the caller configures through
// CrateWriteOptions::spill_directory. Nothing here reads the environment.

#ifndef LIGHTUSD_NEXT_CRATE_SPILL_ARRAY_HH_
#define LIGHTUSD_NEXT_CRATE_SPILL_ARRAY_HH_

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include "../memory-trim.hh"  // AdviseHugePages
#endif

namespace lightusd {
namespace next {

// Type-erased surface of one spilled column, so the writer can budget, drain
// and diagnose the columns without knowing their element type. Header-only
// interface class: suppress the weak-vtable warning the same way
// src/crate-path-utils/path_interface.hh does.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
#endif
class SpillColumnBase {
 public:
  SpillColumnBase() = default;
  SpillColumnBase(const SpillColumnBase&) = delete;
  SpillColumnBase& operator=(const SpillColumnBase&) = delete;
  virtual ~SpillColumnBase() = default;

  // Enables 8 MiB chunking + front eviction. Off keeps one plain vector.
  void set_spill_enabled(bool on) { spill_enabled_ = on; }
  // Directory for the (unlinked) spill file; empty means "/tmp".
  void set_spill_directory(std::string dir) { spill_dir_ = std::move(dir); }

  virtual uint64_t ResidentBytes() const = 0;
  virtual uint64_t SealedBytes() const = 0;
  virtual uint64_t SpillSealed(uint64_t want_free, std::string* error) = 0;
  virtual bool io_failed() const = 0;

 protected:
  bool spill_enabled_ = false;
  std::string spill_dir_;
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

template <typename T, size_t ChunkBytes = static_cast<size_t>(8) << 20>
class SpillVec final : public SpillColumnBase {
 public:
  // Sealed chunk size: large enough to amortize allocation/touch costs like a
  // vector's geometric growth, small enough that the eviction overshoot and
  // per-chunk copy stay cheap. A template parameter so tests can use tiny
  // chunks and callers can tune the traffic pattern.
  static constexpr size_t kChunkBytes = ChunkBytes;
  static constexpr size_t kChunkElems =
      sizeof(T) >= kChunkBytes ? 1 : kChunkBytes / sizeof(T);

  SpillVec() = default;
  ~SpillVec() override { Release(); }

  size_t size() const { return size_; }
  uint64_t spilled_bytes() const { return spilled_elems_ * sizeof(T); }

  uint64_t ResidentBytes() const override {
    uint64_t bytes = 0;
    for (const auto& c : chunks_) bytes += c.capacity() * sizeof(T);
    return bytes;
  }

  uint64_t SealedBytes() const override {
    if (chunks_.size() <= 1) return 0;
    uint64_t bytes = 0;
    for (size_t i = 0; i + 1 < chunks_.size(); ++i)
      bytes += chunks_[i].capacity() * sizeof(T);
    return bytes;
  }

  bool io_failed() const override {
    return io_failed_.load(std::memory_order_relaxed);
  }

  // MADV_HUGEPAGE the chunk spans (best effort, as the plain vector path did
  // for the whole column).
  void AdviseHugePagesOnChunks() const {
    for (const auto& c : chunks_)
      AdviseHugePages(c.data(), c.capacity() * sizeof(T));
  }

  // Capacity hint. Without spill this mirrors std::vector::reserve exactly
  // (one chunk = a plain vector). With spill, chunks are fixed-size and never
  // reallocated, so the hint only pre-creates the first chunk.
  void reserve(size_t n) {
    if (spilled_elems_) return;
    if (!spill_enabled_) {
      EnsureChunk();
      chunks_[0].reserve(n + chunks_[0].size());
      return;
    }
    if (chunks_.empty()) OpenChunk();
  }

  void push_back(const T& v) {
    EnsureRoomForOne();
    chunks_.back().push_back(v);
    ++size_;
  }

  // Range append to the back.
  void append(const T* first, size_t n) {
    if (n == 0) return;
    if (!spill_enabled_) {
      EnsureChunk();
      std::vector<T>& c = chunks_[0];
      c.insert(c.end(), first, first + n);
      size_ = c.size();
      return;
    }
    while (n) {
      EnsureRoomForOne();  // leaves room >= 1 in the open chunk
      std::vector<T>& c = chunks_.back();
      const size_t room = kChunkElems - c.size();
      const size_t step = room < n ? room : n;
      c.insert(c.end(), first, first + step);
      first += step;
      n -= step;
      size_ += step;
    }
  }

  // Apply fn to every element by reference, in append order. Used by the
  // (non-streamed) value-layout patch, which mutates the reps column in place.
  template <class F>
  void ForEachMut(F&& fn) {
    for (auto& c : chunks_)
      for (T& v : c) fn(v);
  }

  // Reset to empty; also drops the spill file (everything here is discarded).
  void clear() noexcept { Release(); }

  // Drop all resident bytes and the spill file. The array is logically empty
  // afterwards; the writer calls this once a column's section payload exists.
  void Release() noexcept {
    chunks_.clear();
    if (spill_fd_ >= 0) {
      ::close(spill_fd_);
      spill_fd_ = -1;
    }
    spill_bytes_ = 0;
    degraded_to_memory_ = false;
    size_ = 0;
    spilled_elems_ = 0;
  }

  // Drop sealed chunks from the front (append their bytes to the spill file)
  // until at least `want_free` resident bytes are freed or no sealed chunk
  // remains. The last chunk is never evicted: it is the one appends go into.
  // Returns the bytes freed. On an I/O failure the array degrades to no-spill
  // (everything stays in memory; no data is lost), raises io_failed() and
  // sets *error.
  uint64_t SpillSealed(uint64_t want_free, std::string* error) override {
    if (!spill_enabled_ || degraded_to_memory_) return 0;
    uint64_t freed = 0;
    while (chunks_.size() > 1 && freed < want_free) {
      if (spill_fd_ < 0 && !OpenSpillFile(error)) return freed;
      const std::vector<T>& c = chunks_.front();
      const size_t bytes = c.size() * sizeof(T);
      if (c.data() && !WriteAll(c.data(), bytes, error)) {
        // Degrade: stop spilling, keep every chunk resident. The file may
        // hold a partial chunk; it is never read for that prefix again
        // (spilled_elems_ is not advanced past a failed write). The fd stays
        // open and the file was unlinked at creation.
        degraded_to_memory_ = true;
        return freed;
      }
      spilled_elems_ += c.size();
      spill_bytes_ += bytes;
      freed += bytes;
      chunks_.pop_front();
    }
    return freed;
  }

  // Whole-array contiguous access for the section builders. When nothing is
  // spilled and the array is a single chunk, *view aliases internal storage
  // and *out stays null (zero-copy, exactly what a plain vector's data() gave
  // before). Otherwise *out receives a private contiguous copy (raw, not
  // zero-filled: the bytes are immediately overwritten by the spill pread and
  // the resident tail) and *view points into it. Returns false (setting
  // *error and io_failed()) only on I/O failure.
  bool Resolve(std::unique_ptr<T[]>* out, const T** view,
               std::string* error) const {
    *view = nullptr;
    out->reset();
    if (!spilled_elems_ && chunks_.size() == 1) {
      *view = chunks_[0].data();
      return true;
    }
    if (spilled_elems_ && spill_fd_ < 0) {
      if (error) *error = "writer spill file is gone";
      io_failed_.store(true, std::memory_order_relaxed);
      return false;
    }
    std::unique_ptr<T[]> buf(new T[size_ ? size_ : 1]);
    T* d = buf.get();
    if (spilled_elems_) {
      const size_t bytes = spilled_elems_ * sizeof(T);
      if (!ReadAll(d, bytes, error)) return false;
      d += spilled_elems_;
    }
    for (const std::vector<T>& c : chunks_) {
      if (c.empty()) continue;
      std::memcpy(d, c.data(), c.size() * sizeof(T));
      d += c.size();
    }
    *view = buf.get();
    *out = std::move(buf);
    return true;
  }

 private:
  void EnsureChunk() {
    if (chunks_.empty()) chunks_.emplace_back();
  }

  void EnsureRoomForOne() {
    if (chunks_.empty() || (spill_enabled_ && chunks_.back().size() >= kChunkElems))
      OpenChunk();
  }

  void OpenChunk() {
    chunks_.emplace_back();
    chunks_.back().reserve(kChunkElems);
  }

  bool OpenSpillFile(std::string* error) const {
    const std::string& dir = spill_dir_.empty() ? std::string("/tmp") : spill_dir_;
    const std::string tmpl = dir + "/lusd-spill-XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    const int fd = ::mkstemp(path.data());
    if (fd < 0) {
      if (error)
        *error = "cannot create writer spill file in " + dir + ": " +
                 std::strerror(errno);
      degraded_to_memory_ = true;
      io_failed_.store(true, std::memory_order_relaxed);
      return false;
    }
    ::unlink(path.data());  // fd stays valid; the path is gone on any exit
    spill_fd_ = fd;
    return true;
  }

  // Sequential append at the tracked end offset (one writer: the batch
  // commit). The lseek keeps the fd offset explicit even if a rebuild read
  // already moved it.
  bool WriteAll(const void* data, size_t bytes, std::string* error) const {
    if (::lseek(spill_fd_, static_cast<off_t>(spill_bytes_), SEEK_SET) < 0) {
      if (error)
        *error = "writer spill seek failed: " + std::string(std::strerror(errno));
      io_failed_.store(true, std::memory_order_relaxed);
      return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (bytes) {
      const ssize_t w = ::write(spill_fd_, p, bytes);
      if (w <= 0) {
        if (w < 0 && errno == EINTR) continue;
        if (error)
          *error = "writer spill write failed: " + std::string(std::strerror(errno));
        io_failed_.store(true, std::memory_order_relaxed);
        return false;
      }
      p += w;
      bytes -= static_cast<size_t>(w);
    }
    return true;
  }

  // Sequential read from the start of the file (one reader per column: the
  // section builder that owns it).
  bool ReadAll(void* dst, size_t bytes, std::string* error) const {
    if (::lseek(spill_fd_, 0, SEEK_SET) != 0) {
      if (error)
        *error = "writer spill seek failed: " + std::string(std::strerror(errno));
      io_failed_.store(true, std::memory_order_relaxed);
      return false;
    }
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (bytes) {
      const ssize_t r = ::read(spill_fd_, p, bytes);
      if (r <= 0) {
        if (r < 0 && errno == EINTR) continue;
        if (error)
          *error = "writer spill read failed: " + std::string(std::strerror(errno));
        io_failed_.store(true, std::memory_order_relaxed);
        return false;
      }
      p += r;
      bytes -= static_cast<size_t>(r);
    }
    return true;
  }

  // Resident storage. Without spill this holds exactly one vector whose
  // behavior (incl. capacity growth) matches the std::vector it replaced.
  std::deque<std::vector<T>> chunks_;
  size_t size_ = 0;
  static_assert(std::is_trivially_copyable<T>::value,
                "SpillVec moves bytes between memory and the spill file");

  mutable bool degraded_to_memory_ = false;  // set from const pread/pwrite helpers
  mutable std::atomic<bool> io_failed_{false};
  size_t spilled_elems_ = 0;          // oldest elements living only in the file
  mutable uint64_t spill_bytes_ = 0;  // file bytes written so far
  mutable int spill_fd_ = -1;
};

}  // namespace next
}  // namespace lightusd

#endif  // LIGHTUSD_NEXT_CRATE_SPILL_ARRAY_HH_
