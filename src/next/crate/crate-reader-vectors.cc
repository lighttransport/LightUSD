// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Crate scalar vector value decode helpers.

#include "crate-reader-internal.hh"

#include "safe-arithmetic.hh"

namespace lightusd {
namespace next {

bool CrateReader::Impl::UnpackTokenOrStringVector(ValueRep rep,
                                                  CrateTypeId type_id,
                                                  Value& out) {
  if (rep.payload() == 0) {
    out = Value::MakeTokenArray(std::vector<std::string>{});
    return true;
  }
  if (!SeekToPayload(reader(), rep)) return false;
  uint64_t n = 0;
  if (!reader()->read_u64(n)) return false;
  if (n > options_.max_array_elements) return false;
  // The on-disk payload is n uint32_t indices; require them to physically fit
  // before allocating idxs(n)/data(n).
  if (!reader()->has_elements(static_cast<size_t>(n), sizeof(uint32_t))) {
    return false;
  }
  if (!CheckElementAllocation(n, sizeof(uint32_t), "String vector indices")) return false;
  std::vector<uint32_t> idxs(static_cast<size_t>(n));
  size_t bytes;
  if (!safe::mul(static_cast<size_t>(n), sizeof(uint32_t), &bytes)) return false;
  if (n && !reader()->read(idxs.data(), bytes)) return false;
  if (!CheckStringArrayAllocation(idxs, type_id == CrateTypeId::StringVector)) return false;
  std::vector<std::string> data(static_cast<size_t>(n));
  for (size_t i = 0; i < n; i++) {
    std::string s;
    if (type_id == CrateTypeId::TokenVector) {
      if (idxs[i] >= tokens_.size()) return false;
      s = tokens_.str(idxs[i]);
    } else {
      if (!GetString(idxs[i], s)) return false;
    }
    data[i] = std::move(s);
  }
  out = Value::MakeTokenArray(std::move(data));
  return true;
}

bool CrateReader::Impl::UnpackDoubleVector(ValueRep rep, Value& out) {
  if (rep.payload() == 0) {
    out = Value::MakeDoubleArray(std::vector<double>{});
    return true;
  }
  if (!SeekToPayload(reader(), rep)) return false;
  uint64_t n = 0;
  if (!reader()->read_u64(n)) return false;
  if (n > options_.max_array_elements) return false;
  // n doubles must physically be present in the remaining file before we
  // allocate; bound the count against the file to avoid a malformed-count
  // multi-GB allocation ahead of the read below.
  if (!reader()->has_elements(static_cast<size_t>(n), sizeof(double))) {
    return false;
  }
  std::vector<double> data(static_cast<size_t>(n));
  size_t bytes;
  if (!safe::mul(static_cast<size_t>(n), sizeof(double), &bytes)) return false;
  if (n && !reader()->read(data.data(), bytes)) return false;
  out = Value::MakeDoubleArray(std::move(data));
  return true;
}

}  // namespace next
}  // namespace lightusd
