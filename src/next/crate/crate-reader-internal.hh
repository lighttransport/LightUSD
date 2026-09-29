// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - private CrateReader implementation details.

#pragma once

#include "crate-reader.hh"

#include "crate-data-source.hh"
#include "stream-reader.hh"
#include "../layer/prim-spec.hh"  // PropMeta (property metadata decode)

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#endif

namespace lightusd {
namespace next {

/// Ceiling on recursive value nesting (dictionaries, and UnregisteredValue
/// wrappers around them). Shared by UnpackValue and DecodeDictionary so a
/// wrapper hop cannot launder the counter back to zero.
constexpr int kMaxValueNestDepth = 64;

/// FIELDS prevalidation: minimum number of payload bytes a non-inlined
/// ValueRep of this type must have available at its offset for the decoder
/// not to run off the end of the file. Sizes must match the actual decode
/// reads (crate-reader-unpack.cc) — overstating rejects valid files whose
/// payload sits near EOF. Exposed (non-static) for unit tests.
uint64_t CrateValueRepMinPayloadBytes(ValueRep rep, CrateVersion version);

// TfToken-lite storage for the crate token table. The token section is one
// contiguous run of NUL-separated strings; keeping one blob plus spans avoids a
// per-token std::string allocation while the reader is reconstructing the layer.
class TokenPool {
 public:
  void clear() {
    blob_.clear();
    spans_.clear();
  }
  void reserve(size_t n) { spans_.reserve(n); }
  void push(const char* begin, size_t len) {
    spans_.push_back(Span{static_cast<uint32_t>(blob_.size()),
                          static_cast<uint32_t>(len)});
    blob_.append(begin, len);
  }
  size_t size() const { return spans_.size(); }
  bool empty() const { return spans_.empty(); }
  std::string str(size_t i) const {
    const Span& s = spans_[i];
    return std::string(blob_.data() + s.off, s.len);
  }
  std::string_view view(size_t i) const {
    const Span& s = spans_[i];
    return std::string_view(blob_.data() + s.off, s.len);
  }
  std::vector<std::string> to_vector() const {
    std::vector<std::string> out;
    out.reserve(spans_.size());
    for (size_t i = 0; i < spans_.size(); ++i) out.push_back(str(i));
    return out;
  }

 private:
  struct Span {
    uint32_t off;
    uint32_t len;
  };
  std::string blob_;
  std::vector<Span> spans_;
};

// Arena storage for the crate PATHS table (same idea as TokenPool): one blob
// plus {off,len} spans, so reconstructing millions of path strings does not
// cost one std::string allocation (and later one free) each.
//
// Two fill modes:
//   - serial:   resize(n) then set(i, sv). set() appends to the blob, so blob
//     growth may REALLOCATE -- views returned by view() before a later set()
//     are invalidated.
//   - parallel: resize(n), resize_blob(total) ONCE, then tasks write raw bytes
//     through blob_at(off) into disjoint windows and record spans with
//     place(). The blob never grows after resize_blob(), so views are stable.
class PathPool {
 public:
  void clear() {
    blob_.clear();
    spans_.clear();
  }
  // (Re)size the span table; every slot becomes the empty path. Also drops
  // previously appended blob bytes.
  void resize(size_t n) {
    blob_.clear();
    spans_.assign(n, Span{0, 0});
  }
  size_t size() const { return spans_.size(); }
  std::string_view view(size_t i) const {
    const Span& s = spans_[i];
    return std::string_view(blob_.data() + s.off, s.len);
  }
  std::string str(size_t i) const { return std::string(view(i)); }
  bool empty_at(size_t i) const { return spans_[i].len == 0; }
  void set(size_t i, std::string_view sv) {
    spans_[i] = Span{static_cast<uint64_t>(blob_.size()), sv.size()};
    blob_.append(sv.data(), sv.size());
  }
  void resize_blob(size_t total_bytes) { blob_.resize(total_bytes); }
  void place(size_t i, uint64_t off, size_t len) { spans_[i] = Span{off, len}; }
  char* blob_at(uint64_t off) { return &blob_[0] + off; }
  std::vector<std::string> to_vector() const {
    std::vector<std::string> out;
    out.reserve(spans_.size());
    for (size_t i = 0; i < spans_.size(); ++i) out.emplace_back(view(i));
    return out;
  }

 private:
  struct Span {
    uint64_t off;
    size_t len;
  };
  std::string blob_;
  std::vector<Span> spans_;
};

class CrateReader::Impl {
 public:
  explicit Impl(const CrateReadOptions& options) : options_(options) {}

  CrateReadResult Read(const uint8_t* data, size_t size);
  CrateReadResult ReadOwned(std::string&& owned);
  CrateReadResult ReadFile(const char* filename);

  std::vector<std::string> tokens() const { return tokens_.to_vector(); }
  // Materialized copy (diagnostics only; paths are stored pooled).
  std::vector<std::string> paths() const { return paths_.to_vector(); }
  const std::vector<CrateField>& fields() const { return fields_; }
  const std::vector<CrateSpec>& specs() const { return specs_; }
  const std::vector<uint32_t>& fieldset_indices() const {
    return fieldset_indices_;
  }

 private:
  CrateReadOptions options_;
  std::unique_ptr<StreamReader> reader_;

  // Per-task decode context for the parallel stage build. Every decode path
  // reaches the byte stream through reader() and reports diagnostics through
  // AddError/AddWarning; a worker task installs its OWN StreamReader cursor (a
  // cheap {data,size,pos} copy over the same immutable buffer -- UnpackValue
  // seeks, so one shared cursor cannot be used concurrently) plus private
  // diagnostic lists, via ScopedThreadDecodeCtx. The lists are merged back in
  // task (= spec/prim range) order, so errors and warnings keep exactly the
  // serial order. No context installed = the members are used directly
  // (identical single-threaded behavior).
  struct ThreadDecodeCtx {
    const Impl* owner;
    StreamReader reader;
    std::vector<CrateError> errors;
    std::vector<std::string> warnings;
    ThreadDecodeCtx(const Impl* o, const StreamReader& r)
        : owner(o), reader(r) {}
  };
#if defined(LIGHTUSD_ENABLE_THREAD)
  static thread_local ThreadDecodeCtx* tls_decode_ctx_;
  class ScopedThreadDecodeCtx {
   public:
    explicit ScopedThreadDecodeCtx(ThreadDecodeCtx* ctx)
        : prev_(tls_decode_ctx_) {
      tls_decode_ctx_ = ctx;
    }
    ~ScopedThreadDecodeCtx() { tls_decode_ctx_ = prev_; }
    ScopedThreadDecodeCtx(const ScopedThreadDecodeCtx&) = delete;
    ScopedThreadDecodeCtx& operator=(const ScopedThreadDecodeCtx&) = delete;

   private:
    ThreadDecodeCtx* prev_;
  };
  ThreadDecodeCtx* decode_ctx() const {
    ThreadDecodeCtx* ctx = tls_decode_ctx_;
    return (ctx && ctx->owner == this) ? ctx : nullptr;
  }
#else
  ThreadDecodeCtx* decode_ctx() const { return nullptr; }
#endif
  StreamReader* reader() {
    if (ThreadDecodeCtx* ctx = decode_ctx()) return &ctx->reader;
    return reader_.get();
  }
  const StreamReader* reader() const {
    if (const ThreadDecodeCtx* ctx = decode_ctx()) return &ctx->reader;
    return reader_.get();
  }
  // Append a finished task context's diagnostics to the result (main thread,
  // in task order).
  void MergeDecodeCtx(ThreadDecodeCtx& ctx);
  // Worker count for the parallel stage build (1 = serial).
  int ResolveBuildThreads() const;
  std::shared_ptr<CrateDataSource> source_;
  CrateReadResult result_;

  CrateVersion version_;
  CrateTOC toc_;
  TokenPool tokens_;
  std::vector<uint32_t> string_indices_;
  std::vector<CrateField> fields_;
  std::vector<uint32_t> fieldset_indices_;
  std::vector<CrateSpec> specs_;
  PathPool paths_;
  // PATHS slot of each path's parent in the path tree (UINT32_MAX for roots,
  // unencoded slots and depth-capped subtrees). Lets the stage build resolve
  // a property spec's owning prim without hashing its path string.
  std::vector<uint32_t> path_parent_;

  CrateReadResult ReadFromString(std::string&& bytes);
  CrateReadResult ParseFromSource();

  bool ReadBootstrap();
  bool ReadTOC();
  bool ReadTokens();
  bool ReadStrings();
  bool ReadFields();
  bool ReadFieldsets();
  bool ReadSpecs();
  bool ReadPaths();
  bool DecodePropMetaField(const std::string& name, ValueRep rep,
                           PropMeta& pm);
  bool BuildStage();

  // `depth` is the nesting depth of the value being unpacked. It MUST be
  // threaded through by every recursive caller: the UnregisteredValue wrapper
  // re-enters UnpackValue, and resetting the counter there let a crafted file
  // with two mutually-referencing dictionaries recurse until the stack was
  // exhausted.
  bool UnpackValue(ValueRep rep, Value& out, int depth = 0);
  // VtArrayEdit rep (crate 0.14): decode the (valuesRep, indexesRep, isDense)
  // tuple into the structured op list PrimSpec carries. Literal element
  // values become canonical usda element text (see layer/array-edit.hh).
  bool UnpackArrayEditData(ValueRep rep, ArrayEditData* out);
  bool UnpackArray(ValueRep rep, Value& out);

  bool UnpackBool(ValueRep rep, Value& out);
  bool UnpackInt(ValueRep rep, Value& out);
  bool UnpackUInt(ValueRep rep, Value& out);
  bool UnpackInt64(ValueRep rep, Value& out);
  bool UnpackUInt64(ValueRep rep, Value& out);
  bool UnpackFloat(ValueRep rep, Value& out);
  bool UnpackDouble(ValueRep rep, Value& out);
  bool UnpackToken(ValueRep rep, Value& out);
  bool UnpackString(ValueRep rep, Value& out);
  bool UnpackAssetPath(ValueRep rep, Value& out);
  bool UnpackVec2f(ValueRep rep, Value& out);
  bool UnpackVec3f(ValueRep rep, Value& out);
  bool UnpackVec4f(ValueRep rep, Value& out);
  bool UnpackVec2d(ValueRep rep, Value& out);
  bool UnpackVec3d(ValueRep rep, Value& out);
  bool UnpackVec4d(ValueRep rep, Value& out);
  bool UnpackQuatf(ValueRep rep, Value& out);
  bool UnpackQuatd(ValueRep rep, Value& out);
  bool UnpackMatrix2d(ValueRep rep, Value& out);
  bool UnpackMatrix3d(ValueRep rep, Value& out);
  bool UnpackMatrix4d(ValueRep rep, Value& out);
  bool UnpackSpecifier(ValueRep rep, Value& out);
  bool UnpackPermission(ValueRep rep, Value& out);
  bool UnpackVariability(ValueRep rep, Value& out);
  bool UnpackTimeSamples(ValueRep rep, Value& out);
  bool DecodeTimeSamples(ValueRep rep,
                         std::vector<std::pair<double, Value>>* out,
                         int depth = 0);
  // Decode a Crate type-59 (TsSpline) field to its USDA text form (the storage
  // PrimSpec uses). Returns false on a malformed blob.
  bool DecodeSplineToText(ValueRep rep, std::string* out);
  bool UnpackTokenOrStringVector(ValueRep rep, CrateTypeId type_id, Value& out);
  bool UnpackDoubleVector(ValueRep rep, Value& out);
  bool UnpackVec2i(ValueRep rep, Value& out);
  bool UnpackVec3i(ValueRep rep, Value& out);
  bool UnpackVec4i(ValueRep rep, Value& out);
  bool UnpackHalf(ValueRep rep, Value& out);
  bool UnpackVec2h(ValueRep rep, Value& out);
  bool UnpackVec3h(ValueRep rep, Value& out);
  bool UnpackVec4h(ValueRep rep, Value& out);
  bool UnpackQuath(ValueRep rep, Value& out);

  // Running total of bytes admitted through CheckByteAllocation, checked
  // against AllocationBudget(). The per-allocation caps alone let N separate
  // allocations each just under the cap sum without bound.
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Atomic: parallel stage-build tasks charge decode allocations concurrently.
  std::atomic<uint64_t> alloc_total_{0};
#else
  uint64_t alloc_total_ = 0;
#endif
  static constexpr uint64_t kU64MaxBytes = ~uint64_t(0);
  uint64_t AllocationBudget() const;

  bool CheckByteAllocation(uint64_t bytes, const char* what);
  bool CheckElementAllocation(uint64_t count, size_t elem_size,
                              const char* what);
  bool GetToken(uint32_t index, std::string& out);
  bool GetString(uint32_t index, std::string& out);
  bool ResolveFieldset(uint32_t fieldset_index,
                       std::vector<std::pair<std::string, Value>>& out);
  bool ResolveFieldsetRaw(uint32_t fieldset_index,
                          std::vector<std::pair<std::string, ValueRep>>& out);
  bool DecodePathTargets(ValueRep rep, std::vector<std::string>& out);
  bool DecodePathTargets(ValueRep rep, std::vector<std::string>& out,
                         bool with_markers);
  bool DecodeReferenceListOp(ValueRep rep, bool is_payload,
                             std::vector<std::string>& out);
  bool DecodeVariantSelectionMap(
      ValueRep rep, std::vector<std::pair<std::string, std::string>>& out);
  bool DecodeTokenListOp(ValueRep rep, std::vector<std::string>& out);
  bool DecodeDictionary(ValueRep rep, Value& out, int depth);

  bool ReportProgress(const char* phase, size_t current = 0,
                      size_t total = 0);
  void AddError(const std::string& msg);
  void AddWarning(const std::string& msg);
};

}  // namespace next
}  // namespace lightusd
