// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Lazy array reference
//
// A LazyArrayRef points at an (undecoded) array value that still lives as bytes
// inside a retained CrateDataSource buffer. Keeping the reference instead of a
// decoded std::vector lets the read -> compose -> write pipeline move array
// values around without ever materializing their (potentially huge) payloads,
// and lets the writer copy the source block straight through when it is safe.

#pragma once

#include "crate-format.hh"      // ValueRep, CrateTypeId
#include "../types/type-id.hh"  // next::TypeId

#include <atomic>
#include <memory>
#include <cstdint>

namespace lightusd {
namespace next {

class CrateDataSource;
class LazyArraySource {
public:
  LazyArraySource() noexcept : identity_(NextIdentity()) {}
  virtual ~LazyArraySource() = default;
  LazyArraySource(const LazyArraySource&) = delete;
  LazyArraySource& operator=(const LazyArraySource&) = delete;
  LazyArraySource(LazyArraySource&&) = delete;
  LazyArraySource& operator=(LazyArraySource&&) = delete;

  // A source identity remains unique after the source is destroyed. Writers
  // use it for non-owning caches whose entries must not keep a multi-GB crate
  // buffer alive merely to avoid a repeated hash. Pointer addresses are not
  // suitable there: consume_values may release the last shared_ptr, allowing
  // an allocator to reuse the address for a later source.
  uint64_t identity() const noexcept { return identity_; }

  virtual bool MaterializeArray(const struct LazyArrayRef& ref, class Value* out) const = 0;
  virtual const uint8_t* base() const = 0;
  virtual size_t size() const = 0;
  virtual CrateVersion version() const = 0;
  virtual bool is_mmapped() const = 0;
  virtual bool can_borrow() const { return false; }
  virtual void DiscardRange(uint64_t offset, uint64_t length) const {
    (void)offset;
    (void)length;
  }

 private:
  static uint64_t NextIdentity() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  const uint64_t identity_;
};

/// Lightweight descriptor for an array value stored in a retained crate buffer.
/// The on-disk block at `block_offset` is `[count][data...]`; the count header
/// is u64 for crate >= 0.7.0 and u32 for older versions.
struct LazyArrayRef {
  std::shared_ptr<LazyArraySource> source;  // keeps the backing source alive
  ValueRep rep;                             // original on-disk rep (flags+payload)
  uint64_t block_offset = 0;   // absolute offset of the block (0 => empty array)
  uint64_t block_len = 0;      // total block bytes for verbatim copy (0 => unknown)
  uint64_t element_count = 0;  // logical element count
  uint32_t src_elem_stride = 0;  // bytes per element as stored on disk
  CrateTypeId crate_type = CrateTypeId::Invalid;  // exact on-disk type
  TypeId value_type = TypeId::Invalid;  // next TypeId materialize() would produce
  bool is_compressed = false;  // convenience copy of rep.is_compressed()
  // Materialization may occur after the reader has returned. Preserve the
  // read-time element policy instead of using a decoder-wide default.
  uint64_t max_elements = 1024ull * 1024ull * 1024ull;
};

/// Probe only the block header (element count + layout) of an array ValueRep,
/// without materializing the payload. `max_elements` guards against a malformed
/// count. Returns false on a malformed/out-of-bounds header.
bool ProbeArrayBlock(const std::shared_ptr<CrateDataSource>& source, ValueRep rep,
                     size_t max_elements, LazyArrayRef* out);

}  // namespace next
}  // namespace lightusd
