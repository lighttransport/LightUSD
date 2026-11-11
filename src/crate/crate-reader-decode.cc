// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - USDC Crate Reader decode/allocation helpers

#include "listop-wire.hh"
#include "crate-reader-internal.hh"

#include <algorithm>
#include "layer/int64-listop.hh"
#include "core/safe-arithmetic.hh"
#include "execution.hh"
#include "writer/dtoa.hh"
#include "writer/value-printer.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

namespace lightusd {

bool CrateReader::Impl::ResolveFieldset(uint32_t fieldset_index,
                                         std::vector<std::pair<std::string_view, Value>>& out,
                                         std::vector<DecodedExtensionField>* extensions) {
  out.clear();

  if (fieldset_index >= fieldset_indices_.size()) {
    return false;
  }

  size_t start = fieldset_index;
  while (start < fieldset_indices_.size()) {
    uint32_t field_idx = fieldset_indices_[start];
    if (field_idx == 0xFFFFFFFF) break;

    if (field_idx < fields_.size()) {
      const CrateField& field = fields_[field_idx];

      if (field.token_index.value < tokens_.size()) {
        // The name views the token blob (stable for the whole build): the
        // fieldset is decoded millions of times, and an owning string per
        // field was pure allocation churn.
        const std::string_view name = tokens_.view(field.token_index.value);
        // variantSelection has a dedicated raw decoder in BuildStage because
        // it is not representable by Value. Do not report the expected
        // generic-decoder miss as an unsupported field in strict mode.
        if (name == "variantSelection" &&
            field.value_rep.type_id() == CrateTypeId::VariantSelectionMap) {
          ++start;
          continue;
        }
        // Structured integer list-ops are retained by the metadata decoder,
        // not flattened into a Value (which would lose authored sublists).
        if (field.value_rep.type_id() == CrateTypeId::Int64ListOp) {
          if (!extensions) { ++start; continue; }  // structural pre-pass
          DecodedExtensionField extension;
          if (DecodeInt64ListOpField(std::string(name), field.value_rep,
                                                   &extension)) {
            extensions->push_back(std::move(extension));
            out.emplace_back(name, Value());  // retain authored field order
          } else AddWarning("Failed to decode Int64 list-op field '" + std::string(name) + "'");
          ++start;
          continue;
        }
        Value value;
        if (UnpackValue(field.value_rep, value)) {
          out.emplace_back(name, std::move(value));
        } else {
          // Preserve field identity in diagnostics even when the value codec is
          // unsupported (notably OpenUSD UnregisteredValue extension fields).
          // Strict AOUSD mode promotes this warning and rejects the load.
          AddWarning("Failed to decode field '" + std::string(name) +
                     "'; field ignored");
        }
      }
    }
    start++;
  }

  return true;
}

CrateTypeId CrateReader::Impl::FieldType(uint32_t fieldset_index, std::string_view name) const {
  for (size_t index = fieldset_index; index < fieldset_indices_.size(); ++index) {
    const uint32_t field_index = fieldset_indices_[index];
    if (field_index == 0xffffffffu) break;
    if (field_index >= fields_.size()) continue;
    const auto& field = fields_[field_index];
    if (field.token_index.value < tokens_.size() && tokens_.view(field.token_index.value) == name)
      return field.value_rep.type_id();
  }
  return CrateTypeId::Invalid;
}

bool CrateReader::Impl::ResolveFieldsetRaw(
    uint32_t fieldset_index,
    std::vector<std::pair<std::string_view, ValueRep>>& out) {
  out.clear();
  if (fieldset_index >= fieldset_indices_.size()) return false;

  size_t start = fieldset_index;
  while (start < fieldset_indices_.size()) {
    uint32_t field_idx = fieldset_indices_[start];
    if (field_idx == 0xFFFFFFFF) break;
    if (field_idx < fields_.size()) {
      const CrateField& field = fields_[field_idx];
      if (field.token_index.value < tokens_.size()) {
        out.emplace_back(tokens_.view(field.token_index.value),
                         field.value_rep);
      }
    }
    start++;
  }
  return true;
}

bool CrateReader::Impl::DecodePathTargets(ValueRep rep,
                                          std::vector<std::string>& out) {
  return DecodePathTargets(rep, out, /*with_markers=*/false);
}

bool CrateReader::Impl::DecodePathTargets(ValueRep rep,
                                          std::vector<std::string>& out,
                                          bool with_markers) {
  out.clear();
  // Reads one or more [u64 count][u32 path_index * count] runs of path indices
  // (uncompressed, per pxrUSD). PathVector has a single run; PathListOp is
  // prefixed by a 1-byte ListOpHeader selecting which sublists are present. We
  // flatten every present sublist's targets (sufficient for round-tripping
  // relationship/connection targets; list-edit semantics are not preserved).
  CrateTypeId tid = rep.type_id();
  if (tid != CrateTypeId::PathVector && tid != CrateTypeId::PathListOp) {
    return false;
  }
  if (rep.payload() == 0) return true;  // empty
  if (!SeekToPayload(reader(), rep)) return false;

  auto read_run = [&](bool keep, uint64_t n) -> bool {
    if (n > options_.max_array_elements) return false;
    // Each element is 4 input bytes but produces a whole reconstructed path
    // STRING (up to max_path_depth components). Require the file to actually
    // hold the indices, and charge the produced strings against the allocation
    // budget -- otherwise a ~1 MB crate drives `out` into the multi-GB range.
    if (n > 0 && !reader()->has_elements(static_cast<size_t>(n), 4)) return false;
    if (keep && !CheckElementAllocation(n, sizeof(std::string), "Path list-op")) {
      return false;
    }
    // Preflight the whole run before creating any strings. A copied cursor
    // avoids temporary index storage and charges the budget once per run.
    StreamReader scan = *reader();
    size_t string_bytes = 0;
    for (uint64_t i = 0; i < n; ++i) {
      uint32_t idx = 0;
      if (!scan.read_u32(idx) || idx >= paths_.size()) return false;
      if (keep && !safe::add(string_bytes,
                     static_cast<uint64_t>(paths_.view(idx).size()),
                     &string_bytes)) return false;
    }
    if (keep && !CheckByteAllocation(string_bytes, "Path list-op targets")) return false;
    for (uint64_t i = 0; i < n; ++i) {
      uint32_t idx = 0;
      if (!reader()->read_u32(idx)) return false;
      if (!keep) continue;
      // paths_ renders a property path as ".<primpath>/<prop>"; convert to the
      // canonical USD form "<primpath>.<prop>" so targets re-intern correctly
      // (and survive repeated round-trips). Prim targets pass through as-is.
      const std::string_view p = paths_.view(idx);
      if (!p.empty() && p[0] == '.') {
        const std::string_view body = p.substr(1);  // "/a/b/prop"
        const size_t slash = body.rfind('/');
        if (slash != std::string_view::npos) {
          std::string target(body);
          target[slash] = '.';                      // "/a/b.prop"
          out.push_back(std::move(target));
        } else {
          out.emplace_back(body);
        }
      } else {
        out.emplace_back(p);                        // prim path "/a/b"
      }
    }
    return true;
  };

  if (tid == CrateTypeId::PathVector) {
    uint64_t count = 0;
    return reader()->read_u64(count) && read_run(true, count);
  }

  // PathListOp: [u8 header][present sublists...]. ListOpHeader bits (pxrUSD):
  //   0x02 explicit, 0x04 added, 0x08 deleted, 0x10 ordered,
  //   0x20 prepended, 0x40 appended. Read order matches the legacy reader:
  //   explicit, added, prepended, appended, deleted, ordered.
  // With `with_markers`, a non-explicit listop's sublists are delimited by
  // "\x01P"/"\x01A"/"\x01D"/"\x01O" marker entries so the caller can
  // reconstruct the authored list-op edits.
  uint8_t bits = 0;
  if (!DecodeCrateListOpWire(*reader(), options_.max_array_elements, bits,
      [&](size_t run, uint64_t count) {
        const bool mark = with_markers && !(CrateListOpSemanticHeader(bits) & 1u);
        if (mark && run != 0) out.push_back(kCrateListOpRunMarkers[run]);
        return read_run(CrateListOpRetainsRun(bits, run), count);
      })) return false;
  // Preserve an authored empty op's mode. No marker would be interpreted as
  // a bare explicit list, incorrectly turning header 0x00 into an explicit clear.
  if (with_markers && out.empty()) {
    out.push_back(kCrateListOpRunMarkers[(CrateListOpSemanticHeader(bits) & 1u) ? 0 : 1]);
  }
  return true;
}

bool CrateReader::Impl::DecodeReferenceListOp(ValueRep rep, bool is_payload,
                                              std::vector<std::string>& out, int depth) {
  out.clear();
  const CrateTypeId tid = rep.type_id();
  if (tid != CrateTypeId::ReferenceListOp && tid != CrateTypeId::PayloadListOp) {
    return false;
  }
  if (rep.payload() == 0) return true;  // empty listop
  if (!SeekToPayload(reader(), rep)) return false;

  // Payload items carry a LayerOffset only from crate 0.8.0 on.
  const bool payload_has_offset =
      !is_payload || (version_.minor >= 8);

  // Read one SdfReference/SdfPayload item; when `keep`, append its arc string.
  auto read_item = [&](bool keep) -> bool {
    uint32_t asset_idx = 0, path_idx = 0;
    if (!reader()->read_u32(asset_idx) || !reader()->read_u32(path_idx)) {
      return false;
    }
    if (asset_idx >= string_indices_.size() || path_idx >= paths_.size() ||
        string_indices_[asset_idx] >= tokens_.size()) {
      return false;
    }
    double offset = 0.0, scale = 1.0;
    if (payload_has_offset) {
      if (!reader()->read_f64(offset) || !reader()->read_f64(scale)) {
        return false;
      }
      if (!std::isfinite(offset) || !std::isfinite(scale)) return false;
    }
    std::string custom_data;
    if (!is_payload) {
      // A reference embeds the ordinary recursive dictionary layout directly
      // after its offset. Reuse the bounded value decoder, preserving nested
      // values and the nesting budget even for discarded list-op buckets.
      const size_t dict_start = reader()->position();
      uint64_t count = 0;
      if (!reader()->read_u64(count) ||
          !CheckElementAllocation(count, sizeof(std::pair<std::string, Value>),
                                  "Reference customData")) return false;
      if (!reader()->seek(dict_start)) return false;
      Value dictionary;
      // Uncached: the caller reads on from where this dictionary ends, and a
      // cache hit would not move the stream (the rep is a stream position).
      if (!DecodeDictionaryUncached(ValueRep::Make(CrateTypeId::Dictionary,
                                                   dict_start, false, false),
                                    dictionary, depth + 1)) return false;
      if (keep && count != 0) {
        PrintOptions print;
        print.float_precision = 9;
        print.double_precision = 17;
        print.sort_dictionary_keys = true;
        custom_data = PrintValue(dictionary, print);
      }
    }
    if (!keep) return true;

    std::string asset;
    if (!GetString(asset_idx, asset)) return false;
    const std::string prim = paths_.str(path_idx);
    // Internal arcs (no asset) render as "</Prim>", matching the usda parser.
    std::string arc;
    if (!asset.empty()) arc = "@" + asset + "@";
    if (!prim.empty() && prim != "/") arc += "<" + prim + ">";
    if (offset != 0.0 || scale != 1.0) {
      arc += "?layerOffset=" + dtos(offset) + ":" +
             dtos(scale);
    }
    if (!custom_data.empty()) arc += '\x1f' + custom_data;
    out.push_back(std::move(arc));
    return true;
  };

  auto read_run = [&](bool keep, uint64_t n) -> bool {
    if (n > options_.max_array_elements) return false;
    // Each item produces a whole arc STRING from a few input bytes; charge the
    // per-item container cost against the allocation budget (read_item itself
    // is bounds-checked against the file for its own reads).
    if (!CheckElementAllocation(n, sizeof(std::string), "Reference list-op")) {
      return false;
    }
    for (uint64_t i = 0; i < n; ++i) {
      if (!read_item(keep)) return false;
    }
    return true;
  };

  // ListOpHeader bits / read order match DecodePathTargets. Non-explicit
  // sublists are marker-delimited ("\x01" "P"/"A"/"D"/"O") so BuildStage can
  // reconstruct the authored list-op edits.
  uint8_t bits = 0;
  if (!DecodeCrateListOpWire(*reader(), options_.max_array_elements, bits,
      [&](size_t run, uint64_t count) {
        const bool mark = !(CrateListOpSemanticHeader(bits) & 1u);
        const bool keep = CrateListOpRetainsRun(bits, run);
        if (mark && keep && run != 0) out.push_back(kCrateListOpRunMarkers[run]);
        return read_run(keep, count);
      })) return false;
  if (out.empty()) {
    out.push_back(kCrateListOpRunMarkers[(CrateListOpSemanticHeader(bits) & 1u) ? 0 : 1]);
  }
  return true;
}

bool CrateReader::Impl::DecodeVariantSelectionMap(
    ValueRep rep, std::vector<std::pair<std::string, std::string>>& out) {
  out.clear();
  if (rep.type_id() != CrateTypeId::VariantSelectionMap) return false;
  if (rep.payload() == 0) return true;  // empty map
  if (!SeekToPayload(reader(), rep)) return false;
  uint64_t count = 0;
  if (!reader()->read_u64(count)) return false;
  if (count > options_.max_array_elements) return false;
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t k = 0, v = 0;
    if (!reader()->read_u32(k) || !reader()->read_u32(v)) return false;
    std::string key, val;
    if (!GetString(k, key) || !GetString(v, val)) return false;
    // AOUSD Core 1.1: duplicate keys make the map invalid; a permissive
    // reader continues with the LAST-encountered value for the key.
    auto dup = std::find_if(out.begin(), out.end(),
                            [&](const std::pair<std::string, std::string>& e) {
                              return e.first == key;
                            });
    if (dup != out.end()) {
      AddWarning("Duplicate variant selection key `" + key +
                 "` in crate; keeping the last value");
      dup->second = std::move(val);
      continue;
    }
    out.emplace_back(std::move(key), std::move(val));
  }
  return true;
}

bool CrateReader::Impl::DecodeInt64ListOpField(
    std::string name, ValueRep rep, DecodedExtensionField* out) {
  auto fail = [&]() { AddError("Invalid or over-budget Int64 list-op metadata"); return false; };
  if (rep.type_id() != CrateTypeId::Int64ListOp || rep.is_array() ||
      rep.is_array_edit() || rep.is_inlined() || rep.is_compressed()) return fail();
  if (!CheckByteAllocation(sizeof(Int64ListOpStack) + sizeof(Int64ListOp),
                           "Int64 list-op metadata")) return false;
  Int64ListOp op;
  op.bits = 0;
  if (rep.payload()) {
    if (!SeekToPayload(reader(), rep) ||
        !DecodeCrateListOpWire(*reader(), options_.max_array_elements, op.bits,
          [&](size_t run, uint64_t n) {
            if (n > (std::numeric_limits<size_t>::max)() ||
                !reader()->has_elements(static_cast<size_t>(n), 8) ||
                !CheckElementAllocation(n, 96, "Int64 list-op")) return false;
            if (!CrateListOpRetainsRun(op.bits, run)) {
              for (uint64_t i = 0; i < n; ++i) {
                int64_t discarded = 0;
                if (!reader()->read_i64(discarded)) return false;
              }
              return true;
            }
            std::vector<int64_t> values(static_cast<size_t>(n));
            for (int64_t& value : values)
              if (!reader()->read_i64(value)) return false;
            op.items[run] = Value::MakeInt64Array(values);
            return true;
          })) return fail();
  }
  op.bits = CrateListOpSemanticHeader(op.bits);
  *out = MakeInt64ListOpField(std::move(name), std::move(op));
  return true;
}

bool CrateReader::Impl::DecodeTokenListOp(ValueRep rep,
                                          std::vector<std::string>& out) {
  out.clear();
  const CrateTypeId tid = rep.type_id();
  if (tid != CrateTypeId::TokenListOp && tid != CrateTypeId::StringListOp) {
    return false;
  }
  if (rep.payload() == 0) return true;  // empty listop
  if (!SeekToPayload(reader(), rep)) return false;

  const bool is_token = (tid == CrateTypeId::TokenListOp);
  // One [u64 count][u32 idx]* run; collect its tokens when `keep`.
  auto read_run = [&](bool keep, uint64_t n) -> bool {
    if (n > options_.max_array_elements) return false;
    // 4 input bytes per element, one whole token/string appended: require the
    // indices to actually be in the file and charge the output.
    if (n > 0 && !reader()->has_elements(static_cast<size_t>(n), 4)) return false;
    if (keep && !CheckElementAllocation(n, sizeof(std::string),
                                        "Token list-op")) {
      return false;
    }
    for (uint64_t i = 0; i < n; ++i) {
      uint32_t idx = 0;
      if (!reader()->read_u32(idx)) return false;
      std::string s;
      if (is_token) {
        if (idx >= tokens_.size()) return false;
        if (keep) s = tokens_.str(idx);
      } else {
        std::string decoded;
        if (!GetString(idx, decoded)) return false;
        if (keep) s = std::move(decoded);
      }
      if (!keep) continue;
      if (!CheckByteAllocation(s.size(), "Token list-op contents")) return false;
      out.push_back(std::move(s));
    }
    return true;
  };

  // ListOpHeader bits / read order match DecodeReferenceListOp. Non-explicit
  // sublists are marker-delimited ("\x01" "P"/"A"/"D"/"O") so BuildStage can
  // recover the authored qualifier (e.g. `prepend apiSchemas` or
  // `delete apiSchemas` — deleted/ordered items used to be dropped).
  uint8_t bits = 0;
  if (!DecodeCrateListOpWire(*reader(), options_.max_array_elements, bits,
      [&](size_t run, uint64_t count) {
        const bool mark = !(CrateListOpSemanticHeader(bits) & 1u);
        const bool keep = CrateListOpRetainsRun(bits, run);
        if (mark && keep && run != 0) out.push_back(kCrateListOpRunMarkers[run]);
        return read_run(keep, count);
      })) return false;
  if (out.empty()) {
    out.push_back(kCrateListOpRunMarkers[(CrateListOpSemanticHeader(bits) & 1u) ? 0 : 1]);
  }
  return true;
}

bool CrateReader::Impl::DecodeDictionary(ValueRep rep, Value& out, int depth) {
  if (rep.type_id() != CrateTypeId::Dictionary) return false;
  if (depth > kMaxValueNestDepth) return false;
  if (rep.payload() == 0) {
    out = Value::MakeDictionary();
    return true;
  }
  if (FindDecoded(rep.raw(), out)) return true;
  if (!DecodeDictionaryUncached(rep, out, depth)) return false;
  StoreDecoded(rep.raw(), out);
  return true;
}

bool CrateReader::Impl::FindDecoded(uint64_t rep, Value& out) {
  DictCacheShard& shard = dict_cache_[(rep ^ (rep >> 17)) % kDictCacheShards];
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(shard.mu);
#endif
  auto it = shard.map.find(rep);
  if (it == shard.map.end()) return false;
  out = it->second;
  return true;
}

void CrateReader::Impl::StoreDecoded(uint64_t rep, const Value& v) {
  DictCacheShard& shard = dict_cache_[(rep ^ (rep >> 17)) % kDictCacheShards];
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::lock_guard<std::mutex> lk(shard.mu);
#endif
  shard.map.emplace(rep, v);
}

bool CrateReader::Impl::DecodeDictionaryUncached(ValueRep rep, Value& out,
                                                 int depth) {
  if (!SeekToPayload(reader(), rep)) return false;
  uint64_t count = 0;
  if (!reader()->read_u64(count)) return false;
  if (count > options_.max_array_elements) return false;

  // pxr WriteMap layout: [u64 count] then per entry [u32 keyStringIdx] followed
  // by a Write(VtValue): an int64 forward offset, the nested value data, then
  // the 8-byte ValueRep (_RecursiveWrite). So entries are *variable*-stride: the
  // ValueRep lives at (valStart + recOffset) and the next entry begins right
  // after it. We must read sequentially and restore the position after each
  // value decode (which seeks to the ValueRep's payload elsewhere).
  Value dv = Value::MakeDictionary();
  auto d = dv.as_dictionary();
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t kidx = 0;
    if (!reader()->read_u32(kidx)) return false;
    std::string key;
    if (!GetString(kidx, key)) return false;

    const size_t val_start = reader()->position();
    uint64_t rec_off_raw = 0;
    if (!reader()->read_u64(rec_off_raw)) return false;
    if (rec_off_raw < 8) {
      AddError("Dictionary recursive offset is too small");
      return false;
    }
    const uint64_t val_start_u64 = static_cast<uint64_t>(val_start);
    const uint64_t file_size = static_cast<uint64_t>(reader()->size());
    if (file_size < sizeof(uint64_t) ||
        rec_off_raw > (std::numeric_limits<uint64_t>::max)() - val_start_u64 ||
        (val_start_u64 + rec_off_raw) > (file_size - sizeof(uint64_t))) {
      AddError("Dictionary recursive ValueRep is outside file");
      return false;
    }
    const size_t rep_pos = static_cast<size_t>(val_start_u64 + rec_off_raw);
    if (!reader()->seek(rep_pos)) return false;
    uint64_t vrep_raw = 0;
    if (!reader()->read_u64(vrep_raw)) return false;
    const size_t next_entry_pos = reader()->position();  // rep_pos + 8

    ValueRep vr(vrep_raw);
    Value cv;
    if (vr.type_id() == CrateTypeId::Dictionary) {
      if (!DecodeDictionary(vr, cv, depth + 1)) return false;
    } else if (vr.type_id() != CrateTypeId::Invalid &&
               !UnpackValue(vr, cv, depth + 1)) {
      return false;
    }
    if (d->find(key)) {
      AddWarning("Crate byte " + std::to_string(val_start) +
                 ": [core.dict.duplicateKey] Duplicate dictionary key `" + key +
                 "`; keeping the last value");
    }
    d->set(std::move(key), std::move(cv));

    if (!reader()->seek(next_entry_pos)) return false;  // resume after this value
  }
  out = std::move(dv);
  return true;
}

bool CrateReader::Impl::CheckByteAllocation(uint64_t bytes, const char* what) {
  const char* error = allocation_budget_->Charge(bytes);
  if (!error) return true;
  AddError(std::string(what) + error);
  return false;
}

bool CrateReader::Impl::CheckIntegerWorkspace(size_t count,
                                               const uint8_t* data, size_t size) {
  size_t bytes;
  if (!CrateIntegerWorkspaceBytes(count, sizeof(uint32_t), data, size, &bytes)) {
    AddError("Compressed integer workspace size overflow");
    return false;
  }
  return CheckByteAllocation(bytes, "Compressed integer workspace");
}

bool CrateReader::Impl::CheckElementAllocation(uint64_t count, size_t elem_size,
                                               const char* what) {
  if (elem_size == 0) return false;
  const uint64_t max_size =
      static_cast<uint64_t>((std::numeric_limits<size_t>::max)());
  if (count > max_size / elem_size) {
    AddError(std::string(what) + " size exceeds addressable memory");
    return false;
  }
  size_t total = 0;
  if (!safe::mul(count, elem_size, &total)) {
    AddError(std::string(what) + " size exceeds addressable memory");
    return false;
  }
  return CheckByteAllocation(static_cast<uint64_t>(total), what);
}

bool CrateReader::Impl::CheckStringArrayAllocation(
    const std::vector<uint32_t>& indices, bool use_string_indices) {
  size_t bytes = 0;
  for (uint32_t index : indices) {
    if (use_string_indices) {
      if (index >= string_indices_.size()) return false;
      index = string_indices_[index];
    }
    if (index >= tokens_.size()) return false;
    size_t length;
    if (!safe::add(tokens_.view(index).size(),
                   uint64_t(1), &length) ||
        !safe::add(bytes, length, &bytes)) return false;
  }
  return CheckElementAllocation(indices.size(), sizeof(std::string),
                                "String array objects") &&
         CheckByteAllocation(bytes, "String array contents");
}

bool CrateReader::Impl::GetToken(uint32_t index, std::string& out) {
  if (index >= tokens_.size()) {
    return false;
  }
  out = tokens_.str(index);
  return true;
}

bool CrateReader::Impl::GetString(uint32_t index, std::string& out) {
  if (index >= string_indices_.size()) {
    return false;
  }
  return GetToken(string_indices_[index], out);
}

void CrateReader::Impl::AddError(const std::string& msg) {
  CrateError err;
  const StreamReader* r = reader();
  err.offset = r ? r->position() : 0;
  err.message = msg;
  if (ThreadDecodeCtx* ctx = decode_ctx()) {
    ctx->errors.push_back(std::move(err));
    return;
  }
  result_.errors.push_back(std::move(err));
}

bool CrateReader::Impl::ReportProgress(const char* phase, size_t current,
                                       size_t total) {
  if (!options_.progress_callback) return true;
  if (options_.progress_callback(phase, current, total)) return true;
  AddError(std::string("USDC read cancelled during ") + phase);
  return false;
}

void CrateReader::Impl::AddWarning(const std::string& msg) {
  if (options_.strict_aousd_conformance) {
    AddError("Strict AOUSD mode: " + msg);
    return;
  }
  if (ThreadDecodeCtx* ctx = decode_ctx()) {
    ctx->warnings.push_back(msg);
    return;
  }
  result_.warnings.push_back(msg);
}

void CrateReader::Impl::MergeDecodeCtx(ThreadDecodeCtx& ctx) {
  for (CrateError& e : ctx.errors) result_.errors.push_back(std::move(e));
  for (std::string& w : ctx.warnings) result_.warnings.push_back(std::move(w));
  ctx.errors.clear();
  ctx.warnings.clear();
}

int CrateReader::Impl::ResolveBuildThreads() const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (source_ && source_->size() < options_.min_parallel_parse_bytes) return 1;
  int nt = ClampExecutionThreads(options_.num_threads);
  if (nt <= 0) {
    nt = static_cast<int>(std::thread::hardware_concurrency());
    nt = std::min(nt, 8);
  }
  return nt < 1 ? 1 : nt;
#else
  return 1;
#endif
}


}  // namespace lightusd
