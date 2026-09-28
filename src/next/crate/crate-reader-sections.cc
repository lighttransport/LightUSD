// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader structural section readers

#include "crate-reader-internal.hh"
#include "lazy-array.hh"
#include "safe-arithmetic.hh"
#include "../strfmt.hh"
#include "../execution.hh"
#include "crate-timing.hh"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

uint64_t CrateValueRepMinPayloadBytes(ValueRep rep, CrateVersion version) {
  // Array element count header: uint32 for crate < 0.7.0, uint64 for >= 0.7.0
  // (pxr crateFile.cpp _Write/_ReadUncompressedArray).
  if (rep.is_array()) return CrateArrayCountHeaderBytes(version);
  switch (rep.type_id()) {
    case CrateTypeId::Bool: return 1;
    case CrateTypeId::UChar: return 1;
    case CrateTypeId::Half: return 2;
    // Half vectors are stored as 2-byte lanes (GfVec{2,3,4}h = 4/6/8 bytes);
    // these entries used to reuse the float-vector sizes (8/12/16), rejecting
    // valid files whose half payload sits near EOF as "truncated".
    case CrateTypeId::Vec2h: return 4;
    case CrateTypeId::Vec3h: return 6;
    case CrateTypeId::Vec4h:
    case CrateTypeId::Quath: return 8;  // 4 half lanes, not 4 float lanes
    case CrateTypeId::Int:
    case CrateTypeId::UInt:
    case CrateTypeId::Float: return 4;
    case CrateTypeId::Int64:
    case CrateTypeId::UInt64:
    case CrateTypeId::Double:
    case CrateTypeId::TimeCode: return 8;
    case CrateTypeId::Vec2i:
    case CrateTypeId::Vec2f: return 8;
    case CrateTypeId::Vec3i:
    case CrateTypeId::Vec3f: return 12;
    case CrateTypeId::Vec4i:
    case CrateTypeId::Vec4f:
    case CrateTypeId::Quatf: return 16;
    case CrateTypeId::Vec2d: return 16;
    case CrateTypeId::Vec3d: return 24;
    case CrateTypeId::Vec4d:
    case CrateTypeId::Quatd:
    case CrateTypeId::Matrix2d: return 32;
    case CrateTypeId::Matrix3d: return 72;
    case CrateTypeId::Matrix4d: return 128;
    case CrateTypeId::Dictionary:
    case CrateTypeId::TokenVector:
    case CrateTypeId::StringVector:
    case CrateTypeId::DoubleVector:
    case CrateTypeId::PathVector:
    case CrateTypeId::VariantSelectionMap:
      return 8;
    case CrateTypeId::PathListOp:
    case CrateTypeId::ReferenceListOp:
    case CrateTypeId::PayloadListOp:
    case CrateTypeId::TokenListOp:
    case CrateTypeId::StringListOp:
      return 1;
    case CrateTypeId::TimeSamples:
      return 32;
    default:
      return 0;
  }
}

bool CrateReader::Impl::ReadBootstrap() {
  // Check magic
  char magic[8];
  if (!reader_->read(magic, 8)) {
    AddError("Failed to read magic bytes");
    return false;
  }
  if (std::memcmp(magic, kCrateMagic, 8) != 0) {
    AddError("Invalid USDC magic bytes");
    return false;
  }

  // Read version
  uint8_t version_bytes[8];
  if (!reader_->read(version_bytes, 8)) {
    AddError("Failed to read version");
    return false;
  }
  version_.major = version_bytes[0];
  version_.minor = version_bytes[1];
  version_.patch = version_bytes[2];

  if (!version_.is_valid()) {
    AddError("Unsupported USDC version: " + version_.to_string());
    return false;
  }
  // AOUSD Core 1.0.1 standardizes Crate through 0.12. Compatibility mode can
  // read additive OpenUSD 0.13/0.14 containers and diagnose any unsupported
  // values, but strict conformance must not imply support for those versions.
  if (version_.major == 0 && version_.minor > 12) {
    const std::string message =
        "USDC version " + version_.to_string() +
        " is newer than the AOUSD Core 1.0.1 Crate 0.12 profile";
    if (options_.strict_aousd_conformance) {
      AddError("Strict AOUSD mode: " + message);
      return false;
    }
    AddWarning(message);
  }

  // Read TOC offset
  int64_t toc_offset;
  if (!reader_->read_i64(toc_offset)) {
    AddError("Failed to read TOC offset");
    return false;
  }

  if (toc_offset < static_cast<int64_t>(kCrateBootstrapSize) ||
      toc_offset >= static_cast<int64_t>(reader_->size())) {
    AddError("Invalid TOC offset");
    return false;
  }

  // Seek to TOC
  if (!reader_->seek(static_cast<size_t>(toc_offset))) {
    AddError("Failed to seek to TOC");
    return false;
  }

  return true;
}

bool CrateReader::Impl::ReadTOC() {
  uint64_t num_sections;
  if (!reader_->read_u64(num_sections)) {
    AddError("Failed to read section count");
    return false;
  }

  if (num_sections > 100) {
    AddError("Too many sections in TOC");
    return false;
  }

  toc_.sections.resize(static_cast<size_t>(num_sections));

  for (size_t i = 0; i < num_sections; i++) {
    CrateSection& s = toc_.sections[i];
    if (!reader_->read(s.name, 16)) {
      AddError("Failed to read section name");
      return false;
    }
    if (!reader_->read_i64(s.start)) {
      AddError("Failed to read section start");
      return false;
    }
    if (!reader_->read_i64(s.size)) {
      AddError("Failed to read section size");
      return false;
    }

    // Overflow-safe: s.start + s.size as int64 can wrap. Check each term
    // against the file size with subtraction that cannot overflow.
    const size_t fsize = reader_->size();
    if (s.start < 0 || s.size < 0 ||
        static_cast<size_t>(s.start) > fsize ||
        static_cast<size_t>(s.size) > fsize - static_cast<size_t>(s.start)) {
      AddError("Invalid section bounds: " + s.name_str());
      return false;
    }
  }

  // Structural consistency, reported as WARNINGS rather than errors.
  //
  // Each section above is individually in-bounds and every subsequent read is
  // bounds-checked, so a duplicate or overlapping TOC is not memory-unsafe --
  // rejecting such a file would refuse content pxr may well still read. But it
  // does mean the file presents mutually inconsistent structural tables (and
  // toc_.find() silently takes the FIRST match), which is worth surfacing.
  // num_sections is capped at 100 above, so the pairwise scan is trivial.
  for (size_t i = 0; i < toc_.sections.size(); ++i) {
    for (size_t j = i + 1; j < toc_.sections.size(); ++j) {
      if (toc_.sections[i].name_str() == toc_.sections[j].name_str()) {
        AddWarning("Duplicate TOC section '" + toc_.sections[i].name_str() +
                   "'; the first one is used");
      }
    }
  }

  {
    std::vector<size_t> order(toc_.sections.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) {
      return toc_.sections[a].start < toc_.sections[b].start;
    });
    for (size_t k = 1; k < order.size(); ++k) {
      const CrateSection& prev = toc_.sections[order[k - 1]];
      const CrateSection& cur = toc_.sections[order[k]];
      if (prev.size == 0) continue;
      // Both are already validated as non-negative and in-bounds.
      const uint64_t prev_end = static_cast<uint64_t>(prev.start) +
                                static_cast<uint64_t>(prev.size);
      if (static_cast<uint64_t>(cur.start) < prev_end) {
        AddWarning("Overlapping TOC sections '" + prev.name_str() + "' and '" +
                   cur.name_str() + "'");
      }
    }
  }

  return true;
}

bool CrateReader::Impl::ReadTokens() {
  const CrateSection* section = toc_.find("TOKENS");
  if (!section) {
    AddError("Missing TOKENS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to TOKENS");
    return false;
  }

  uint64_t num_tokens;
  if (!reader_->read_u64(num_tokens)) {
    AddError("Failed to read token count");
    return false;
  }

  if (num_tokens > options_.max_tokens) {
    AddError("Too many tokens");
    return false;
  }

  uint64_t uncompressed_size, compressed_size;
  if (!reader_->read_u64(uncompressed_size) || !reader_->read_u64(compressed_size)) {
    AddError("Failed to read token compression info");
    return false;
  }
  if (!CheckByteAllocation(compressed_size, "Compressed token table") ||
      !CheckByteAllocation(uncompressed_size, "Uncompressed token table")) {
    return false;
  }

  // Use the check-before-resize read overload so a bogus compressed_size
  // cannot trigger a huge allocation before the bounds check.
  std::vector<uint8_t> compressed;
  if (!reader_->read(compressed, static_cast<size_t>(compressed_size))) {
    AddError("Failed to read compressed tokens");
    return false;
  }

  DecompressResult dr = DecompressCrateBlob(compressed.data(), compressed.size(),
                                            static_cast<size_t>(uncompressed_size));
  if (!dr.success) {
    AddError("Failed to decompress tokens: " + dr.error);
    return false;
  }

  // TokenPool addresses tokens with uint32 (offset,len) spans; a token blob
  // beyond 4 GiB would silently truncate. Reject it (absurd for real USDC; this
  // only guards hostile/corrupt input).
  if (dr.data.size() > static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
    AddError("Token blob exceeds 4 GiB pooled-storage limit");
    return false;
  }

  tokens_.reserve(static_cast<size_t>(num_tokens));
  const char* ptr = reinterpret_cast<const char*>(dr.data.data());
  const char* end = ptr + dr.data.size();

  while (ptr < end && tokens_.size() < num_tokens) {
    // Bounded scan: a malformed blob whose last token lacks the NUL terminator
    // must not strlen past the decompressed buffer.
    const char* nul = static_cast<const char*>(
        std::memchr(ptr, '\0', static_cast<size_t>(end - ptr)));
    if (!nul) {
      AddError("Token table not NUL-terminated");
      return false;
    }
    tokens_.push(ptr, static_cast<size_t>(nul - ptr));
    ptr = nul + 1;
  }

  if (tokens_.size() != num_tokens) {
    AddWarning("Token count mismatch");
  }

  return true;
}

bool CrateReader::Impl::ReadStrings() {
  const CrateSection* section = toc_.find("STRINGS");
  if (!section) {
    // Required, like the other five structural sections (TOKENS / FIELDS /
    // FIELDSETS / SPECS / PATHS all error here). This one silently accepted
    // its absence, so a crate missing STRINGS loaded "successfully" with an
    // empty string table -- every GetString() then fails and the affected
    // opinions are quietly dropped. OpenUSD rejects the same file outright
    // ("Crate file missing STRINGS section"), and all 240 crate files in
    // tests/ and models/ carry the section, so requiring it costs nothing and
    // removes a silent-data-loss path.
    AddError("Missing STRINGS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to STRINGS");
    return false;
  }

  uint64_t num_strings;
  if (!reader_->read_u64(num_strings)) {
    AddError("Failed to read string count");
    return false;
  }

  if (num_strings > options_.max_strings) {
    AddError("Too many strings");
    return false;
  }
  if (!CheckElementAllocation(num_strings, sizeof(uint32_t), "String table")) {
    return false;
  }

  string_indices_.resize(static_cast<size_t>(num_strings));
  for (size_t i = 0; i < num_strings; i++) {
    uint32_t idx;
    if (!reader_->read_u32(idx)) {
      AddError("Failed to read string index");
      return false;
    }
    if (idx >= tokens_.size()) {
      AddError("String token index out of range");
      return false;
    }
    string_indices_[i] = idx;
  }

  return true;
}

bool CrateReader::Impl::ReadFields() {
  const CrateSection* section = toc_.find("FIELDS");
  if (!section) {
    AddError("Missing FIELDS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to FIELDS");
    return false;
  }

  uint64_t num_fields;
  if (!reader_->read_u64(num_fields)) {
    AddError("Failed to read field count");
    return false;
  }

  if (num_fields > options_.max_fields) {
    AddError("Too many fields");
    return false;
  }
  if (!CheckElementAllocation(num_fields, sizeof(CrateField), "Field table") ||
      !CheckElementAllocation(num_fields, sizeof(uint32_t),
                              "Field token indices") ||
      !CheckElementAllocation(num_fields, sizeof(uint64_t), "Field value reps")) {
    return false;
  }
  size_t field_index_bytes = 0;
  size_t field_value_rep_bytes = 0;
  if (!safe::mul(num_fields, sizeof(uint32_t), &field_index_bytes) ||
      !safe::mul(num_fields, sizeof(uint64_t), &field_value_rep_bytes)) {
    AddError("Field table byte size overflow");
    return false;
  }

  uint64_t indices_size;
  if (!reader_->read_u64(indices_size)) {
    AddError("Failed to read field indices size");
    return false;
  }
  if (!CheckByteAllocation(indices_size, "Field indices")) return false;

  std::vector<uint8_t> indices_data;
  if (!reader_->read(indices_data, static_cast<size_t>(indices_size))) {
    AddError("Failed to read field indices");
    return false;
  }

  // Try pxrUSD format (n_chunks LZ4 + delta-coded) first, fall back to legacy
  std::vector<uint32_t> token_indices_vec(static_cast<size_t>(num_fields));
  // Include u64 compressed_size prefix (DecompressCompressedU32 expects it)
  size_t indices_with_prefix_size = 0;
  if (!safe::add(size_t(8), indices_data.size(), &indices_with_prefix_size)) {
    AddError("Field indices payload size overflow");
    return false;
  }
  std::vector<uint8_t> indices_with_prefix(indices_with_prefix_size);
  std::memcpy(indices_with_prefix.data(), &indices_size, 8);
  if (!indices_data.empty()) {
    std::memcpy(indices_with_prefix.data() + 8, indices_data.data(),
                indices_data.size());
  }
  DecompressResult dr = DecompressCompressedU32(indices_with_prefix.data(), indices_with_prefix.size(),
                                                 token_indices_vec.data(),
                                                 static_cast<size_t>(num_fields));
  if (!dr.success) {
    // Fall back to legacy format
    dr = DecompressIntegers(indices_with_prefix.data() + 8, indices_with_prefix.size() - 8,
                            static_cast<size_t>(num_fields), false);
    if (!dr.success) {
      AddError("Failed to decompress field indices: " + dr.error);
      return false;
    }
    if (dr.data.size() < field_index_bytes) {
      AddError("Decompressed field indices shorter than expected");
      return false;
    }
    if (field_index_bytes > 0) {
      std::memcpy(token_indices_vec.data(), dr.data.data(), field_index_bytes);
    }
  }

  const uint32_t* token_indices = token_indices_vec.data();
  for (size_t i = 0; i < static_cast<size_t>(num_fields); ++i) {
    if (token_indices[i] >= tokens_.size()) {
      AddError("Field token index out of range");
      return false;
    }
  }

  uint64_t reps_size;
  if (!reader_->read_u64(reps_size)) {
    AddError("Failed to read reps size");
    return false;
  }
  if (!CheckByteAllocation(reps_size, "Field value reps")) return false;

  std::vector<uint64_t> value_reps(static_cast<size_t>(num_fields));

  std::vector<uint8_t> reps_data;
  if (!reader_->read(reps_data, static_cast<size_t>(reps_size))) {
    AddError("Failed to read value reps");
    return false;
  }

  // ValueRep[] is normally TfFastCompression/LZ4-compressed bytes. Do not
  // classify by byte count alone: small OpenUSD files can have compressed size
  // equal to the uncompressed `num_fields * 8` length.
  DecompressResult rdr = DecompressCrateBlob(
      reps_data.data(), reps_data.size(), field_value_rep_bytes);
  if (rdr.success && rdr.data.size() >= field_value_rep_bytes) {
    if (field_value_rep_bytes > 0) {
      std::memcpy(value_reps.data(), rdr.data.data(), field_value_rep_bytes);
    }
  } else if (reps_size == field_value_rep_bytes) {
    if (field_value_rep_bytes > 0) {
      std::memcpy(value_reps.data(), reps_data.data(), field_value_rep_bytes);
    }
  } else {
    AddError("Failed to decompress value reps");
    return false;
  }

  fields_.resize(static_cast<size_t>(num_fields));
  for (size_t i = 0; i < num_fields; i++) {
    fields_[i].token_index.value = token_indices[i];
    fields_[i].value_rep = ValueRep(value_reps[i]);
    const ValueRep rep = fields_[i].value_rep;
    if (!rep.is_inlined()) {
      const int64_t off = rep.payload_as_offset();
      if (off < 0) {
        AddError("Field ValueRep payload offset is negative");
        return false;
      }
      if (rep.payload() != 0 &&
          static_cast<uint64_t>(off) >= static_cast<uint64_t>(reader_->size())) {
        AddError("Field ValueRep payload offset is outside file");
        return false;
      }
      if (rep.payload() != 0) {
        const uint64_t min_bytes = CrateValueRepMinPayloadBytes(rep, version_);
        const uint64_t file_size = static_cast<uint64_t>(reader_->size());
        if (min_bytes > 0 &&
            (min_bytes > file_size ||
             static_cast<uint64_t>(off) > file_size - min_bytes)) {
          AddError("Field ValueRep payload is truncated");
          return false;
        }
      }
      if (rep.is_array() && rep.payload() != 0) {
        const size_t saved = reader_->position();
        if (!reader_->seek(static_cast<size_t>(off))) {
          AddError("Failed to seek to array ValueRep payload");
          return false;
        }
        uint64_t count = 0;
        if (!ReadCrateArrayCount(*reader_, version_, &count)) {
          AddError("Failed to read array ValueRep element count");
          return false;
        }
        if (!reader_->seek(saved)) {
          AddError("Failed to restore FIELDS reader position");
          return false;
        }
        const bool lazy_over_cap_ok =
            options_.lazy_arrays &&
            CrateArrayTypeCanBeLazy(rep.type_id(), rep.is_compressed());
        if (count > options_.max_array_elements && !lazy_over_cap_ok) {
          AddError("Array ValueRep element count exceeds max_array_elements limit");
          return false;
        }
        if (count >
            static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
          AddError("Array ValueRep element count exceeds addressable memory");
          return false;
        }
        if (count >
            static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
          AddError("Array ValueRep element count exceeds Value capacity");
          return false;
        }
        bool valid_lazy_block = false;
        if (lazy_over_cap_ok && source_) {
          LazyArrayRef lr;
          valid_lazy_block =
              ProbeArrayBlock(source_, rep,
                              (std::numeric_limits<size_t>::max)(), &lr) &&
              (rep.payload() == 0 || lr.block_len > 0);
          if (!valid_lazy_block && count > options_.max_array_elements) {
            AddError("Lazy Array ValueRep payload is out of bounds");
            return false;
          }
        }
        const uint64_t stride = CrateArrayElemStride(rep.type_id());
        const uint64_t elem_bytes = stride ? stride : 1;
        if (elem_bytes != 0 &&
            count > (std::numeric_limits<uint64_t>::max)() / elem_bytes) {
          AddError("Array ValueRep payload byte size overflow");
          return false;
        }
        if (!valid_lazy_block &&
            !CheckByteAllocation(count * elem_bytes, "Array ValueRep payload")) {
          return false;
        }
      } else if (rep.payload() != 0) {
        const CrateTypeId tid = rep.type_id();
        const bool count_header =
            tid == CrateTypeId::Dictionary ||
            tid == CrateTypeId::TokenVector ||
            tid == CrateTypeId::StringVector ||
            tid == CrateTypeId::DoubleVector ||
            tid == CrateTypeId::PathVector ||
            tid == CrateTypeId::VariantSelectionMap;
        if (count_header) {
          const size_t saved = reader_->position();
          if (!reader_->seek(static_cast<size_t>(off))) {
            AddError("Failed to seek to counted ValueRep payload");
            return false;
          }
          uint64_t count = 0;
          if (!reader_->read_u64(count)) {
            AddError("Failed to read counted ValueRep payload");
            return false;
          }
          if (!reader_->seek(saved)) {
            AddError("Failed to restore FIELDS reader position");
            return false;
          }
          if (count > options_.max_array_elements) {
            AddError("Counted ValueRep payload exceeds max_array_elements limit");
            return false;
          }
          if (tid == CrateTypeId::Dictionary && count > 0) {
            const uint64_t file_size = static_cast<uint64_t>(reader_->size());
            if (count >
                ((std::numeric_limits<uint64_t>::max)() - 8u) / 20u) {
              AddError("Dictionary ValueRep payload size overflow");
              return false;
            }
            const uint64_t min_payload = 8u + count * 20u;
            if (min_payload > file_size ||
                static_cast<uint64_t>(off) > file_size - min_payload) {
              AddError("Dictionary ValueRep payload is truncated");
              return false;
            }
            if (!reader_->seek(static_cast<size_t>(off + 8))) {
              AddError("Failed to seek to dictionary ValueRep entries");
              return false;
            }
            for (uint64_t entry = 0; entry < count; ++entry) {
              uint32_t key_index = 0;
              if (!reader_->read_u32(key_index)) {
                AddError("Failed to read dictionary key index");
                return false;
              }
              if (key_index >= string_indices_.size()) {
                AddError("Dictionary key string index out of range");
                return false;
              }
              const size_t value_start = reader_->position();
              uint64_t recursive_offset_raw = 0;
              if (!reader_->read_u64(recursive_offset_raw)) {
                AddError("Failed to read dictionary recursive offset");
                return false;
              }
              const int64_t recursive_offset =
                  static_cast<int64_t>(recursive_offset_raw);
              if (recursive_offset < 8) {
                AddError("Dictionary recursive offset is invalid");
                return false;
              }
              const uint64_t value_start_u64 =
                  static_cast<uint64_t>(value_start);
              const uint64_t recursive_offset_u64 =
                  static_cast<uint64_t>(recursive_offset);
              if (recursive_offset_u64 >
                  (std::numeric_limits<uint64_t>::max)() - value_start_u64 ||
                  value_start_u64 + recursive_offset_u64 >
                      file_size - sizeof(uint64_t)) {
                AddError("Dictionary recursive ValueRep is outside file");
                return false;
              }
              const size_t next_entry_pos = static_cast<size_t>(
                  value_start_u64 + recursive_offset_u64 + sizeof(uint64_t));
              if (!reader_->seek(next_entry_pos)) {
                AddError("Failed to seek to next dictionary entry");
                return false;
              }
            }
            if (!reader_->seek(saved)) {
              AddError("Failed to restore FIELDS reader position");
              return false;
            }
          }
        }

        const bool list_op =
            tid == CrateTypeId::PathListOp ||
            tid == CrateTypeId::ReferenceListOp ||
            tid == CrateTypeId::PayloadListOp ||
            tid == CrateTypeId::TokenListOp ||
            tid == CrateTypeId::StringListOp;
        if (list_op) {
          const size_t saved = reader_->position();
          if (!reader_->seek(static_cast<size_t>(off))) {
            AddError("Failed to seek to list-op ValueRep payload");
            return false;
          }
          uint8_t bits = 0;
          if (!reader_->read_u8(bits)) {
            AddError("Failed to read list-op ValueRep header");
            return false;
          }
          if (!reader_->seek(saved)) {
            AddError("Failed to restore FIELDS reader position");
            return false;
          }
          const uint8_t kKnownListOpBits = 0x7e;
          if ((bits & kKnownListOpBits) != 0) {
            const uint64_t file_size = static_cast<uint64_t>(reader_->size());
            if (file_size < 9u || static_cast<uint64_t>(off) > file_size - 9u) {
              AddError("List-op ValueRep payload is truncated");
              return false;
            }
            const uint64_t item_bytes =
                (tid == CrateTypeId::ReferenceListOp ||
                 tid == CrateTypeId::PayloadListOp) ? 8u : 4u;
            uint64_t pos = static_cast<uint64_t>(off + 1);
            const uint8_t order[] = {0x02, 0x04, 0x20, 0x40, 0x08, 0x10};
            for (uint8_t bit : order) {
              if ((bits & bit) == 0) continue;
              if (pos > file_size - sizeof(uint64_t)) {
                AddError("List-op ValueRep run is truncated");
                return false;
              }
              const size_t saved = reader_->position();
              if (!reader_->seek(static_cast<size_t>(pos))) {
                AddError("Failed to seek to list-op ValueRep run");
                return false;
              }
              uint64_t count = 0;
              if (!reader_->read_u64(count)) {
                AddError("Failed to read list-op ValueRep run count");
                return false;
              }
              if (!reader_->seek(saved)) {
                AddError("Failed to restore FIELDS reader position");
                return false;
              }
              if (count > options_.max_array_elements) {
                AddError("List-op ValueRep run count exceeds max_array_elements limit");
                return false;
              }
              if (count >
                  ((std::numeric_limits<uint64_t>::max)() - 8u) / item_bytes) {
                AddError("List-op ValueRep run byte size overflow");
                return false;
              }
              const uint64_t run_bytes = 8u + count * item_bytes;
              if (run_bytes > file_size || pos > file_size - run_bytes) {
                AddError("List-op ValueRep run is truncated");
                return false;
              }
              pos += run_bytes;
            }
          }
        }
      }
    }
  }

  return true;
}

bool CrateReader::Impl::ReadFieldsets() {
  const CrateSection* section = toc_.find("FIELDSETS");
  if (!section) {
    AddError("Missing FIELDSETS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to FIELDSETS");
    return false;
  }

  uint64_t num_fieldsets;
  if (!reader_->read_u64(num_fieldsets)) {
    AddError("Failed to read fieldset count");
    return false;
  }

  // fieldset_indices_ entries index into fields_; bound the count to avoid a
  // huge allocation from a malformed value.
  if (num_fieldsets > options_.max_fieldset_indices) {
    AddError("Too many fieldset indices");
    return false;
  }
  if (!CheckElementAllocation(num_fieldsets, sizeof(uint32_t),
                              "Fieldset index table")) {
    return false;
  }
  size_t fieldset_index_bytes = 0;
  if (!safe::mul(num_fieldsets, sizeof(uint32_t), &fieldset_index_bytes)) {
    AddError("Fieldset index byte size overflow");
    return false;
  }

  if (section->size < 8) {
    AddError("FIELDSETS section too small");
    return false;
  }
  size_t data_size = static_cast<size_t>(section->size) - 8;
  if (!CheckByteAllocation(static_cast<uint64_t>(data_size), "Fieldset data")) {
    return false;
  }
  std::vector<uint8_t> data;
  if (!reader_->read(data, data_size)) {
    AddError("Failed to read fieldset data");
    return false;
  }

  fieldset_indices_.resize(static_cast<size_t>(num_fieldsets));
  DecompressResult dr = DecompressCompressedU32(data.data(), data.size(),
                                                 fieldset_indices_.data(),
                                                 static_cast<size_t>(num_fieldsets));
  if (!dr.success) {
    // Legacy fallback: try EncodeIntegers (common-prefix, no LZ4)
    dr = DecompressIntegers(data.data(), data.size(),
                            static_cast<size_t>(num_fieldsets), false);
    if (!dr.success) {
      AddError("Failed to decompress fieldsets: " + dr.error);
      return false;
    }
    if (dr.data.size() < fieldset_index_bytes) {
      AddError("Decompressed fieldsets shorter than expected");
      return false;
    }
    if (fieldset_index_bytes > 0) {
      std::memcpy(fieldset_indices_.data(), dr.data.data(), fieldset_index_bytes);
    }
  }

  return true;
}

bool CrateReader::Impl::ReadSpecs() {
  const CrateSection* section = toc_.find("SPECS");
  if (!section) {
    AddError("Missing SPECS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to SPECS");
    return false;
  }

  uint64_t num_specs;
  if (!reader_->read_u64(num_specs)) {
    AddError("Failed to read spec count");
    return false;
  }

  if (num_specs > options_.max_specs) {
    AddError("Too many specs");
    return false;
  }
  if (!CheckElementAllocation(num_specs, sizeof(CrateSpec), "Spec table") ||
      !CheckElementAllocation(num_specs, sizeof(uint32_t), "Spec path table") ||
      !CheckElementAllocation(num_specs, sizeof(uint32_t), "Spec fieldset table") ||
      !CheckElementAllocation(num_specs, sizeof(uint32_t), "Spec type table")) {
    return false;
  }

  if (section->size < 8) {
    AddError("SPECS section too small");
    return false;
  }

  specs_.resize(static_cast<size_t>(num_specs));

  uint64_t remaining_size = static_cast<uint64_t>(section->size) - 8;

  // Read 3 compressed integer arrays (delta+LZ4 Usd_IntegerCompression format)
  auto read_comp_array = [&](uint32_t* dst, size_t count, uint64_t& remaining) -> bool {
    if (remaining < 8) {
      AddError("Specs section: not enough data for array size");
      return false;
    }
    uint64_t comp_size;
    if (!reader_->read_u64(comp_size)) {
      AddError("Failed to read specs array compressed size");
      return false;
    }
    remaining -= 8;

    if (comp_size > remaining) {
      AddError("Specs compressed size exceeds remaining section data");
      return false;
    }

    // Include u64 compressed_size prefix (DecompressCompressedU32 expects it)
    std::vector<uint8_t> comp_data(8 + static_cast<size_t>(comp_size));
    std::memcpy(comp_data.data(), &comp_size, 8);
    if (!reader_->read(comp_data.data() + 8, static_cast<size_t>(comp_size))) {
      AddError("Failed to read specs compressed data");
      return false;
    }
    remaining -= comp_size;

    DecompressResult dr = DecompressCompressedU32(comp_data.data(), comp_data.size(),
                                                   dst, count);
    if (!dr.success) {
      // Fallback: try legacy EncodeIntegers (common-prefix, no LZ4)
      dr = DecompressIntegers(comp_data.data(), comp_data.size(), count, false);
      if (!dr.success) {
        AddError("Failed to decompress specs array: " + dr.error);
        return false;
      }
      size_t expected_bytes = 0;
      if (!safe::mul(count, sizeof(uint32_t), &expected_bytes)) {
        AddError("Specs array byte size overflow");
        return false;
      }
      if (dr.data.size() < expected_bytes) {
        AddError("Decompressed specs array shorter than expected");
        return false;
      }
      if (expected_bytes > 0) std::memcpy(dst, dr.data.data(), expected_bytes);
    }
    return true;
  };

  std::vector<uint32_t> path_vals(static_cast<size_t>(num_specs));
  std::vector<uint32_t> fieldset_vals(static_cast<size_t>(num_specs));
  std::vector<uint32_t> type_vals(static_cast<size_t>(num_specs));

  if (read_comp_array(path_vals.data(), static_cast<size_t>(num_specs), remaining_size) &&
      read_comp_array(fieldset_vals.data(), static_cast<size_t>(num_specs), remaining_size) &&
      read_comp_array(type_vals.data(), static_cast<size_t>(num_specs), remaining_size)) {
    for (size_t i = 0; i < num_specs; i++) {
      if (fieldset_vals[i] >= fieldset_indices_.size()) {
        AddError("Spec fieldset index out of range");
        return false;
      }
      specs_[i].path_index.value = path_vals[i];
      specs_[i].fieldset_index.value = fieldset_vals[i];
      specs_[i].spec_type = ToSpecType(type_vals[i]);
    }
    return true;
  }

  // Fallback: try legacy single-array format
  if (!reader_->seek(static_cast<size_t>(section->start) + 8)) {
    AddError("Failed to seek to specs data for legacy read");
    return false;
  }
  size_t legacy_size = static_cast<size_t>(section->size) - 8;
  size_t legacy_plain_size = 0;
  size_t legacy_value_count = 0;
  if (!safe::mul(num_specs, size_t(12), &legacy_plain_size) ||
      !safe::mul(num_specs, size_t(3), &legacy_value_count)) {
    AddError("Specs legacy byte size overflow");
    return false;
  }
  std::vector<uint8_t> legacy_data(legacy_size);
  if (!reader_->read(legacy_data.data(), legacy_size)) {
    AddError("Failed to read specs legacy data");
    return false;
  }
  if (legacy_size == legacy_plain_size) {
    const uint8_t* ptr = legacy_data.data();
    for (size_t i = 0; i < num_specs; i++) {
      std::memcpy(&specs_[i].path_index.value, ptr, 4); ptr += 4;
      std::memcpy(&specs_[i].fieldset_index.value, ptr, 4); ptr += 4;
      if (specs_[i].fieldset_index.value >= fieldset_indices_.size()) {
        AddError("Spec fieldset index out of range");
        return false;
      }
      uint32_t spec_type;
      std::memcpy(&spec_type, ptr, 4); ptr += 4;
      specs_[i].spec_type = ToSpecType(spec_type);
    }
  } else {
    DecompressResult dr = DecompressIntegers(legacy_data.data(), legacy_data.size(),
                                              legacy_value_count, false);
    if (dr.success) {
      // memcpy, not reinterpret_cast: dr.data is a vector<uint8_t> whose
      // buffer has no uint32_t alignment guarantee (UB, and a trap on
      // strict-alignment targets). The sibling decode paths above already
      // do it this way.
      size_t need_bytes = 0;
      if (!safe::mul(legacy_value_count, sizeof(uint32_t), &need_bytes) ||
          dr.data.size() < need_bytes) {
        AddError("Decompressed specs shorter than expected");
        return false;
      }
      for (size_t i = 0; i < num_specs; i++) {
        uint32_t triple[3];
        std::memcpy(triple, dr.data.data() + i * 3 * sizeof(uint32_t),
                    sizeof(triple));
        if (triple[1] >= fieldset_indices_.size()) {
          AddError("Spec fieldset index out of range");
          return false;
        }
        specs_[i].path_index.value = triple[0];
        specs_[i].fieldset_index.value = triple[1];
        specs_[i].spec_type = ToSpecType(triple[2]);
      }
    } else {
      AddError("Failed to decompress specs with any format");
      return false;
    }
  }

  return true;
}

bool CrateReader::Impl::ReadPaths() {
  CratePhaseTimer timer(options_.enable_timing, "next_crate_paths");
  const CrateSection* section = toc_.find("PATHS");
  if (!section) {
    AddError("Missing PATHS section");
    return false;
  }

  if (!reader_->seek(static_cast<size_t>(section->start))) {
    AddError("Failed to seek to PATHS");
    return false;
  }

  uint64_t num_paths;
  if (!reader_->read_u64(num_paths)) {
    AddError("Failed to read path count");
    return false;
  }

  if (num_paths == 0) {
    paths_.resize(1);
    paths_.set(0, "/");
    return true;
  }

  if (num_paths > options_.max_paths) {
    AddError("Too many paths");
    return false;
  }

  // pxrUSD writes two u64 values: [total_path_count] [encoded_tree_node_count]
  uint64_t num_encoded = num_paths;  // default: same as total
  if (!reader_->read_u64(num_encoded)) {
    // Might be a single-count format (no encoded count)
    num_encoded = num_paths;
  }

  if (num_encoded > options_.max_paths) {
    AddError("Too many encoded path nodes");
    return false;
  }

  if (!CheckElementAllocation(num_paths, sizeof(std::string), "Path table")) {
    return false;
  }
  size_t n = static_cast<size_t>(num_encoded);

  // Read 3 compressed integer arrays (delta+LZ4 format). The three blobs are
  // read in file order, then decompressed independently (concurrently for
  // large tables); errors are reported as the sequential read-then-decompress
  // of each array in turn would report them (the first failure wins).
  struct CompArray {
    const char* name = nullptr;
    uint32_t* dst = nullptr;
    std::vector<uint8_t> data;  // u64 compressed_size prefix + payload
    std::string read_error;
    std::string decode_error;
    bool read_ok = false;
  };
  auto read_comp_blob = [&](CompArray& a) -> bool {
    uint64_t comp_size;
    if (!reader_->read_u64(comp_size)) {
      a.read_error = std::string("Failed to read ") + a.name + " compressed size";
      return false;
    }
    // Bound the compressed size against remaining bytes before allocating.
    if (comp_size > reader_->remaining()) {
      a.read_error = std::string(a.name) + " compressed size exceeds remaining data";
      return false;
    }
    // Include u64 compressed_size prefix (DecompressCompressedU32 expects it)
    a.data.resize(8 + static_cast<size_t>(comp_size));
    std::memcpy(a.data.data(), &comp_size, 8);
    if (!reader_->read(a.data.data() + 8, static_cast<size_t>(comp_size))) {
      a.read_error = std::string("Failed to read ") + a.name + " compressed data";
      return false;
    }
    a.read_ok = true;
    return true;
  };
  auto decode_comp_blob = [](CompArray& a, size_t count) {
    const size_t comp_size = a.data.size() - 8;
    DecompressResult dr =
        DecompressCompressedU32(a.data.data(), a.data.size(), a.dst, count);
    if (!dr.success) {
      // Fallback: legacy EncodeIntegers (common-prefix, no LZ4)
      dr = DecompressIntegers(a.data.data() + 8, comp_size, count, false);
      if (!dr.success) {
        a.decode_error = std::string("Failed to decompress ") + a.name + ": " + dr.error;
        return;
      }
      size_t expected_bytes = 0;
      if (!safe::mul(count, sizeof(uint32_t), &expected_bytes)) {
        a.decode_error = std::string(a.name) + " byte size overflow";
        return;
      }
      if (dr.data.size() < expected_bytes) {
        a.decode_error = std::string("Decompressed ") + a.name + " shorter than expected";
        return;
      }
      if (expected_bytes > 0) std::memcpy(a.dst, dr.data.data(), expected_bytes);
    }
    std::vector<uint8_t>().swap(a.data);
  };

  // `num_encoded` is an independent u64 from the file: it was only checked
  // against the flat max_paths cap, NOT against the file-size-relative
  // allocation guard that num_paths goes through. A ~60-byte crate declaring
  // num_paths=1 / num_encoded=10M otherwise allocated ~130 MB across the four
  // buffers below before reading a single byte.
  if (!CheckElementAllocation(n, sizeof(uint32_t) * 3 + 1,
                              "Path decode buffers")) {
    return false;
  }

  std::vector<uint32_t> path_indices(n);
  std::vector<uint32_t> element_tokens(n);
  std::vector<uint32_t> jump_raw(n);  // stored as uint32_t, interpreted as int32_t

  CompArray arrays[3];
  arrays[0].name = "path indices";
  arrays[0].dst = path_indices.data();
  arrays[1].name = "element tokens";
  arrays[1].dst = element_tokens.data();
  arrays[2].name = "jump indices";
  arrays[2].dst = jump_raw.data();
  size_t num_read = 0;
  while (num_read < 3 && read_comp_blob(arrays[num_read])) ++num_read;
  bool decoded = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (num_read > 1 && n >= 65536 && ResolveBuildThreads() > 1) {
    TaskArena arena(num_read);
    arena.Run(num_read, [&](size_t k) { decode_comp_blob(arrays[k], n); });
    decoded = true;
  }
#endif
  if (!decoded) {
    for (size_t k = 0; k < num_read; ++k) decode_comp_blob(arrays[k], n);
  }
  for (size_t k = 0; k < 3; ++k) {
    if (k < num_read && !arrays[k].decode_error.empty()) {
      AddError(arrays[k].decode_error);
      return false;
    }
    if (!arrays[k].read_ok) {
      AddError(arrays[k].read_error);
      return false;
    }
  }

  timer.lap("decompress");
  std::vector<uint8_t> seen_path_slot(static_cast<size_t>(num_paths), uint8_t{0});
  for (size_t i = 0; i < n; ++i) {
    if (path_indices[i] >= num_paths) {
      AddError("Path table index out of range");
      return false;
    }
    if (seen_path_slot[path_indices[i]]) {
      AddError("Duplicate path table index");
      return false;
    }
    seen_path_slot[path_indices[i]] = uint8_t{1};

    int32_t elem_token = static_cast<int32_t>(element_tokens[i]);
    const bool is_prop = elem_token < 0;
    uint32_t token_idx = is_prop
        ? static_cast<uint32_t>(-static_cast<int64_t>(elem_token))
        : static_cast<uint32_t>(elem_token);
    if (token_idx >= tokens_.size()) {
      AddError("Path element token index out of range");
      return false;
    }

    int32_t jump = static_cast<int32_t>(jump_raw[i]);
    if (jump < -2) {
      AddError("Invalid path jump value");
      return false;
    }
    if (jump > 0 && i + static_cast<size_t>(jump) >= n) {
      AddError("Path jump points outside encoded path table");
      return false;
    }
  }

  timer.lap("validate");
  // Reconstruct paths from the compressed tree by navigating jump offsets.
  //
  // Nodes are emitted in pre-order. jump semantics (matching the writer):
  //   jump  > 0 : node has a child (at i+1) AND a sibling (at i+jump)
  //   jump == 0 : node has a sibling only (at i+1), no child
  //   jump == -1: node has a child only (at i+1), no sibling
  //   jump == -2: leaf (no child, no sibling)
  //
  // Three passes:
  //  1. a structural walk (serial) records, per visited node, its parent node
  //     and visit order -- following the jump offset to each sibling is
  //     O(num_nodes). It keeps the exact rules of the former recursive
  //     string-building walk: descend only below max_path_depth, and stop a
  //     sibling chain at an already-visited node (a malformed jump table can
  //     make the child pointer (i+1) and a sibling pointer (i+jump) reach the
  //     same node; `visited` bounds total work to O(n) instead of the
  //     super-linear re-entry a chain of `jump == 1` nodes would cause);
  //  2. path lengths in visit order (parent first) and blob offsets;
  //  3. the fill: each path is written into its own window of one PathPool
  //     blob -- block-copying the parent's already-written path when it lies
  //     earlier in the same task, else walking the ancestor chain backward --
  //     in parallel for large tables. Unvisited nodes keep empty slots.
  //
  // A path is "/" for a top-level node whose element is empty or "/", else
  // "/" + elem under a top-level parent or the root, else parent + "/" + elem;
  // a property node stores "." + that path (its children, in a malformed
  // table, still extend the '.'-less form).
  paths_.resize(static_cast<size_t>(num_paths));  // all slots -> empty path

  constexpr uint32_t kNoParent = UINT32_MAX;
  auto element_view = [&](size_t i) -> std::string_view {
    const int32_t elem_token = static_cast<int32_t>(element_tokens[i]);
    // Promote to int64 before negating (-INT32_MIN is UB).
    const uint32_t token_idx = elem_token < 0
        ? static_cast<uint32_t>(-static_cast<int64_t>(elem_token))
        : static_cast<uint32_t>(elem_token);
    return tokens_.view(token_idx);
  };
  auto is_prop_node = [&](size_t i) -> bool {
    return static_cast<int32_t>(element_tokens[i]) < 0;
  };

  std::vector<uint8_t> visited(n, uint8_t{0});
  std::vector<uint32_t> parent_of(n, kNoParent);
  std::vector<uint32_t> order;  // visited nodes, in visit order
  order.reserve(n);
  {
    struct Frame {
      size_t i;
      uint32_t parent;
      size_t depth;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{0, kNoParent, 0});
    while (!stack.empty()) {
      Frame f = stack.back();
      stack.pop_back();
      size_t i = f.i;
      while (i < n) {
        if (visited[i]) break;  // node already emitted: malformed, stop
        visited[i] = uint8_t{1};
        parent_of[i] = f.parent;
        order.push_back(static_cast<uint32_t>(i));
        const int32_t jump = static_cast<int32_t>(jump_raw[i]);
        const bool has_child = (jump == -1 || jump > 0);
        const bool has_sibling = (jump == 0 || jump > 0);
        const size_t next = has_sibling
            ? i + ((jump > 0) ? static_cast<size_t>(jump) : 1)
            : n;
        if (has_child && f.depth < options_.max_path_depth) {
          // Child subtree first (pre-order), then the rest of this chain.
          if (has_sibling) stack.push_back(Frame{next, f.parent, f.depth});
          stack.push_back(
              Frame{i + 1, static_cast<uint32_t>(i), f.depth + 1});
          break;
        }
        i = next;
      }
    }
  }

  timer.lap("walk");
  // Prim-path length per node (without a property's '.' prefix) and whether
  // the node is the root "/".
  std::vector<uint64_t> plen(n, 0);
  std::vector<uint8_t> rootish(n, uint8_t{0});
  std::vector<uint64_t> node_off(n, 0);
  uint64_t total_bytes = 0;
  path_parent_.assign(static_cast<size_t>(num_paths), UINT32_MAX);
  for (const uint32_t i : order) {
    if (parent_of[i] != kNoParent) {
      path_parent_[path_indices[i]] = path_indices[parent_of[i]];
    }
    const std::string_view elem = element_view(i);
    const uint32_t par = parent_of[i];
    if (par == kNoParent) {
      if (elem.empty() || elem == "/") {
        rootish[i] = uint8_t{1};
        plen[i] = 1;
      } else {
        plen[i] = 1 + elem.size();
      }
    } else if (rootish[par]) {
      plen[i] = 1 + elem.size();
    } else {
      plen[i] = plen[par] + 1 + elem.size();
    }
    node_off[i] = total_bytes;
    total_bytes += plen[i] + (is_prop_node(i) ? 1 : 0);
  }
  paths_.resize_blob(static_cast<size_t>(total_bytes));

  timer.lap("lengths");
  // Write node order[k]'s path for k in [begin, end).
  auto fill_range = [&](size_t begin, size_t end) {
    for (size_t k = begin; k < end; ++k) {
      const uint32_t i = order[k];
      const bool is_prop = is_prop_node(i);
      char* buf = paths_.blob_at(node_off[i]) + (is_prop ? 1 : 0);
      if (is_prop) buf[-1] = '.';
      const uint64_t L = plen[i];
      if (rootish[i]) {
        buf[0] = '/';
      } else {
        const std::string_view elem = element_view(i);
        const uint32_t par = parent_of[i];
        // Parent already written earlier in this range: block-copy it.
        if (par != kNoParent && !rootish[par] && k > begin &&
            node_off[par] >= node_off[order[begin]] &&
            node_off[par] < node_off[i]) {
          const char* pbuf = paths_.blob_at(node_off[par]) +
                             (is_prop_node(par) ? 1 : 0);
          const uint64_t pl = plen[par];
          std::memcpy(buf, pbuf, static_cast<size_t>(pl));
          buf[pl] = '/';
          std::memcpy(buf + pl + 1, elem.data(), elem.size());
        } else {
          // Walk the ancestor chain backward.
          uint64_t pos = L;
          uint32_t j = i;
          for (;;) {
            const std::string_view ej = element_view(j);
            pos -= ej.size();
            std::memcpy(buf + pos, ej.data(), ej.size());
            buf[--pos] = '/';
            const uint32_t pj = parent_of[j];
            if (pj == kNoParent || rootish[pj]) break;
            j = pj;
          }
        }
      }
      paths_.place(path_indices[i], node_off[i], L + (is_prop ? 1 : 0));
    }
  };

  const int nthreads = ResolveBuildThreads();
  bool filled = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (nthreads > 1 && order.size() >= 65536) {
    const size_t task_size = std::max<size_t>(
        16384, order.size() / (static_cast<size_t>(nthreads) * 4));
    const size_t ntasks = (order.size() + task_size - 1) / task_size;
    TaskArena arena(static_cast<size_t>(nthreads));
    arena.Run(ntasks, [&](size_t t) {
      fill_range(t * task_size, std::min(order.size(), (t + 1) * task_size));
    });
    filled = true;
  }
#endif
  if (!filled) fill_range(0, order.size());
  timer.lap("fill");

  for (const CrateSpec& spec : specs_) {
    if (spec.path_index.value >= paths_.size()) {
      AddError("Spec path index out of range");
      return false;
    }
    if (paths_.empty_at(spec.path_index.value)) {
      AddError("Spec path index references an empty path slot");
      return false;
    }
  }

  return true;
}


}  // namespace next
}  // namespace lightusd
