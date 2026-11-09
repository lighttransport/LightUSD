// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate data source (retained USDC buffer)
//
// Owns the raw bytes of one loaded crate (USDC) plus the structural tables
// needed to interpret ValueReps lazily (tokens / string indices). One per
// loaded layer; held via shared_ptr so lazy array values can keep it alive for
// exactly as long as some surviving opinion references into it.

#pragma once

#include "crate-format.hh"      // CrateVersion, ValueRep, CrateTypeId
#include "types/type-id.hh"  // ::lightusd::TypeId
#include "lazy-array.hh"
#include "stream-reader.hh"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {

class StreamReader;

/// Seek a StreamReader to a ValueRep's payload offset.
///
/// payload_as_offset() SIGN-EXTENDS from bit 47, so a 48-bit payload with the
/// top bit set yields a negative int64. On LP64 the cast to size_t made a huge
/// value that seek() happened to reject; on a 32-bit target it TRUNCATES to an
/// arbitrary in-bounds offset, silently redirecting the decode to
/// attacker-chosen bytes. ReadFields already rejects off < 0 for top-level
/// field reps -- this applies the same check everywhere else.
inline bool SeekToPayload(StreamReader* r, ValueRep rep) {
  if (!r) return false;
  const int64_t off = rep.payload_as_offset();
  if (off < 0) return false;
  const uint64_t u = static_cast<uint64_t>(off);
  if (u > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
    return false;
  }
  return r->seek(static_cast<size_t>(u));
}


class Value;
class CrateAllocationBudget;
class CrateSnapshotPager;

/// Owned file-snapshot storage is either new[] or (large files) a populated
/// private anonymous mapping; the deleter remembers which.
struct CrateSnapshotDeleter {
  size_t mapped_bytes = 0;  // 0 = delete[]
  void* mapped_base = nullptr;  // start of the mapping (>= the data pointer's page)
  void operator()(uint8_t* p) const noexcept;
};

class CrateDataSource : public LazyArraySource {
 public:
  /// Adopt the crate bytes by move (no copy) together with the decoded
  /// tokens / string-index tables.
  static std::shared_ptr<CrateDataSource> Adopt(
      std::string&& bytes, CrateVersion version, std::vector<std::string>&& tokens,
      std::vector<uint32_t>&& string_indices);

  /// Adopt just the byte buffer (tables empty). Useful for callers that only
  /// need verbatim block access (e.g. write-time pass-through).
  static std::shared_ptr<CrateDataSource> Adopt(std::string&& bytes,
                                                CrateVersion version);

  /// Memory-map `filename` read-only and adopt the mapping as the backing
  /// (Phase 8.3). Returns nullptr when mmap is unavailable on this platform
  /// (non-posix / WASM) or the mapping fails -- the caller should then fall
  /// back to the owned-buffer Adopt path. The mapping is released (munmap) by
  /// the destructor, i.e. once the last shared_ptr (reader or lazy value)
  /// referencing this source is gone. Tables can be installed later via
  /// set_tables(); version via set_version().
  static std::shared_ptr<CrateDataSource> MmapFile(const std::string& filename);

  /// Copy `filename` into a private owned snapshot (the untrusted-input path:
  /// the bytes never alias the file, so later writes/truncation by another
  /// process cannot change or fault what was loaded).
  ///
  /// One open + fstat + positional reads straight into an uninitialized
  /// buffer: no size-probe reopen and no zero-fill pass before the copy (the
  /// std::string path touched every byte twice). The file must be a regular
  /// file of 1..`max_bytes` bytes (0 = kMaxReadableFileBytes) and must keep
  /// exactly its fstat size while being read; a short read or growth is an
  /// error, never a silently truncated/partial snapshot.
  ///
  /// Returns nullptr with `*unavailable = true` on platforms without POSIX
  /// file descriptors (callers fall back to the stream path); otherwise
  /// nullptr means failure and `*err` says why.
  static std::shared_ptr<CrateDataSource> ReadFileSnapshot(
      const std::string& filename, uint64_t max_bytes, bool* unavailable,
      std::string* err);

  /// Copy `filename` into an unlinked private snapshot file, keep the bytes in
  /// memory only while the structural parse runs, then release them.
  ///
  /// The retained backing becomes the snapshot descriptor: lazy arrays are
  /// materialized by ranged `pread` from the (already captured, immutable)
  /// snapshot instead of holding the whole layer in anonymous memory. This is
  /// the low-RSS form of the untrusted no-mmap policy: source bytes never alias
  /// a file another process can rewrite, and no process RSS is charged for
  /// array data that is never read.
  ///
  /// `*unavailable = true` only where POSIX descriptors are unavailable (the
  /// caller keeps the existing owned-buffer path). Any failure after the
  /// source has been read degrades to the owned snapshot stored in this same
  /// object -- never to a second read of the source and never to mmap.
  /// `temp_directory` empty means `$TMPDIR`, then `/tmp`.
  static std::shared_ptr<CrateDataSource> PagedFileSnapshot(
      const std::string& filename, uint64_t max_bytes,
      const std::string& temp_directory, bool* unavailable, std::string* err);

  ~CrateDataSource();

  /// Whether this source is backed by a memory mapping (vs an owned buffer).
  bool is_mmapped() const override { return mmap_base_ != nullptr; }

  /// Whether this source keeps a private snapshot descriptor for ranged
  /// materialization. True for the untrusted paged path even after the parse
  /// buffer is released.
  bool is_paged() const override { return static_cast<bool>(pager_); }

  /// Drop the anonymized copy used for the structural parse. Lazy array
  /// references created by that parse stay valid: they address the retained
  /// snapshot descriptor instead. No-op for every other backing mode.
  void ReleaseParseBuffer();

  /// Has the mapped file shrunk (or been replaced by a shorter one) since it
  /// was mapped?
  ///
  /// A read-only MAP_PRIVATE mapping is sized once, at open. Every bounds
  /// check in the reader is against THAT size, so if another process truncates
  /// the file while the mapping is alive, touching a page past the new EOF
  /// raises SIGBUS -- which no bounds check can prevent, and which no amount of
  /// validation in this reader can turn into a clean error. The mapping also
  /// outlives the reader: lazy array values hold the source alive, so the
  /// exposure lasts until the last opinion referencing it is dropped.
  ///
  /// This cannot make the mapping safe. It detects the race after the fact --
  /// when we happened not to touch the truncated region -- so a caller gets a
  /// diagnosable warning instead of silently reading a file that changed
  /// underneath it. Untrusted input on a shared filesystem should set
  /// CrateReadOptions::use_mmap = false rather than rely on this.
  ///
  /// Returns false for non-mmap sources and when the size cannot be determined.
  bool MappedFileShrank(size_t* current_size = nullptr) const;
  bool can_borrow() const override {
    // Borrowable while a contiguous buffer is resident: mapped, owned, or the
    // transient parse copy of a paged snapshot. After ReleaseParseBuffer() the
    // paged source has no `base()`, so views must materialize instead.
    return base() != nullptr;
  }

  const uint8_t* base() const override {
    if (mmap_base_) return mmap_base_;
    if (pager_) return paged_parse_.get();
    if (snapshot_) return snapshot_.get();
    return reinterpret_cast<const uint8_t*>(bytes_.data());
  }
  size_t size() const override {
    if (mmap_base_) return mmap_size_;
    if (pager_) return paged_size_;
    if (snapshot_) return snapshot_size_;
    return bytes_.size();
  }
  CrateVersion version() const override { return version_; }
  void DiscardRange(uint64_t offset, uint64_t length) const override;

  /// Read `[offset, offset+length)` from the retained snapshot. Only valid for
  /// paged sources; returns false for every other backing (and on short read).
  bool ReadRange(uint64_t offset, size_t length,
                 std::vector<uint8_t>* out) const;

  /// Set the crate version once it has been parsed from the bootstrap header.
  /// (The buffer is adopted before the header is read.)
  void set_version(CrateVersion v) { version_ = v; }
  void set_max_array_elements(size_t n) { max_array_elements_ = n; }
  void set_allocation_budget(std::shared_ptr<CrateAllocationBudget> budget) {
    allocation_budget_ = std::move(budget);
  }

  /// Install the decoded token / string-index tables (used for token-array
  /// materialization and write-time index remapping).
  void set_tables(std::vector<std::string> tokens,
                  std::vector<uint32_t> string_indices) {
    tokens_ = std::move(tokens);
    string_indices_ = std::move(string_indices);
  }
  const std::vector<std::string>& tokens() const { return tokens_; }
  const std::vector<uint32_t>& string_indices() const { return string_indices_; }

  /// Decode a lazy array reference into a concrete Value. Returns false (and
  /// leaves `*out` empty) for array types that have no concrete Value storage
  /// yet, or on a malformed block.
  bool MaterializeArray(const LazyArrayRef& ref, Value* out) const override;

 private:
  CrateDataSource() = default;
  CrateDataSource(const CrateDataSource&) = delete;
  CrateDataSource& operator=(const CrateDataSource&) = delete;

  // Backing is exactly one of: an owned byte string, an owned file snapshot
  // (snapshot_ != nullptr) or a read-only memory mapping (mmap_base_ !=
  // nullptr). base()/size() abstract over all three so every reader stays
  // backing-agnostic.
  std::string bytes_;       // owned mode: the retained crate (never reallocated)
  std::unique_ptr<uint8_t[], CrateSnapshotDeleter> snapshot_;  // snapshot mode: file copy
  size_t snapshot_size_ = 0;
  // Paged mode: the parse copy (released after parsing) plus the retained
  // private snapshot descriptor lazy arrays read through.
  std::unique_ptr<uint8_t[], CrateSnapshotDeleter> paged_parse_;
  std::shared_ptr<CrateSnapshotPager> pager_;
  size_t paged_size_ = 0;
  const uint8_t* mmap_base_ = nullptr;  // mmap mode: mapped region start
  size_t mmap_size_ = 0;                // mmap mode: mapped length (== file size)
#if !defined(LIGHTUSD_NO_MMAP) && !defined(__EMSCRIPTEN__) && \
    !defined(__wasi__) &&                                         \
    (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
  void* mmap_addr_ = nullptr;           // region to munmap in the destructor
  // Path the mapping came from, kept so MappedFileShrank() can re-stat it. A
  // path rather than a retained fd: a scene can reference hundreds of layers
  // and holding an fd per mapped layer risks the descriptor limit.
  std::string mmap_path_;
#endif

  CrateVersion version_{};  // value-initialized to 0.0.0
  std::vector<std::string> tokens_;
  std::vector<uint32_t> string_indices_;
  std::shared_ptr<CrateAllocationBudget> allocation_budget_;
  size_t max_array_elements_ = 1024ull * 1024ull * 1024ull;
};

/// Shared array-block decoder — the single source of truth used by both the
/// crate reader (token/string arrays) and Value::materialize(). `base`/`size`
/// span a retained crate buffer; `rep` must have its array bit set. `version`
/// selects the element-count header width (see CrateArrayCountHeaderBytes).
bool DecodeCrateArray(const uint8_t* base, size_t size, ValueRep rep,
                      CrateVersion version,
                      const std::vector<std::string>& tokens, size_t max_elements,
                      Value* out);

// Internal budget-aware entry point; the existing overload remains available.
bool DecodeCrateArray(const uint8_t* base, size_t size, ValueRep rep,
                      CrateVersion version,
                      const std::vector<std::string>& tokens, size_t max_elements,
                      Value* out, CrateAllocationBudget* budget);

/// Decode an array whose block begins at `rep.payload_as_offset()` but which
/// is presented in a window starting at `base_offset` (a paged source read).
/// `base`/`size` describe that window; `base_offset` is its absolute file
/// offset. The relative payload may be zero here (the window starts exactly at
/// the block header), which the regular entry points reserve for the "empty
/// array" payload encoding.
bool DecodeCrateArrayAt(const uint8_t* base, size_t size, ValueRep rep,
                        uint64_t base_offset, CrateVersion version,
                        const std::vector<std::string>& tokens,
                        size_t max_elements, Value* out,
                        CrateAllocationBudget* budget);

/// Convenience overload assuming the modern (>= 0.7.0) u64 count header.
inline bool DecodeCrateArray(const uint8_t* base, size_t size, ValueRep rep,
                             const std::vector<std::string>& tokens,
                             size_t max_elements, Value* out) {
  return DecodeCrateArray(base, size, rep, CrateVersion{0, 8, 0}, tokens,
                          max_elements, out);
}

/// Bytes per element of an array CrateTypeId as stored on disk (0 if unknown).
uint32_t CrateArrayElemStride(CrateTypeId id);

/// Complete array header width: [u32 rank=1, u32 count] before 0.5,
/// [u32 count] in 0.5/0.6, [u64 count] from 0.7. Element data immediately
/// follows this header. Evidence: pxr crateFile.cpp UnpackArray and _WriteUncompressedArray /
/// _ReadUncompressedArray / _Read+_WritePossiblyCompressedArray all gate on
/// `(ver < CrateFile::Version(0,7,0)) ? <uint32_t> : <uint64_t>`. There is no
/// "packed count" form (a previous lo/hi-split heuristic here mis-read pre-0.7
/// element data by 4 bytes and could silently truncate >= 2^32 counts).
uint32_t CrateArrayCountHeaderBytes(CrateVersion version);

/// Read the version-appropriate element-count header at the current position
/// of `r`, including the legacy rank word. Rejects short reads/unsupported ranks.
bool ReadCrateArrayCount(class StreamReader& r, CrateVersion version,
                         uint64_t* count);

/// ::lightusd::TypeId that materialize() would surface for an array CrateTypeId.
TypeId CrateArrayValueType(CrateTypeId id);

/// Whether an array type can be represented as a lazy byte reference without
/// changing its logical value. Unsupported and swizzled-on-read types must be
/// decoded eagerly, so they keep the normal element-count guard.
bool CrateArrayTypeCanBeLazy(CrateTypeId id, bool compressed);

}  // namespace lightusd
