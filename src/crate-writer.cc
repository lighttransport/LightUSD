// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Experimental USDC (Crate) File Writer Implementation
#include "crate-writer.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

// XXH3 hash (header-only mode, namespaced to avoid collision with zstd's copy)
#define XXH_INLINE_ALL
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include "external/xxhash.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// Phase 4: Compression support
#include "lz4-compression.hh"
#include "integerCoding.h"
// Direct LZ4 API for OpenUSD compatibility
#include "lz4/lz4.h"

#include "safe-arithmetic.hh"
#include "spline-binary.hh"  // SplineBinaryFormatVersion
#include "array-edit.hh"  // value::ArrayEdit (VtArrayEdit)

// math::is_close — used for exact (eps == 0) floating-point comparison without
// tripping -Wfloat-equal. is_close(a, b, 0) computes fabs(a - b) <= 0, which is
// bit-exact equality for finite values (the int-representability checks below
// guard the operands to a finite range before comparing).
#include "math-util.inc"

// Namespace alias to avoid collision between lightusd::crate and ::crate (path library)
namespace pathlib = ::crate;

// Disable specific clang warnings for this file
// - shadow: if-else chains reuse variable names intentionally
// - sign-conversion: safe narrowing in serialization code
// - old-style-cast: debug print formatting
// - unused-parameter: some functions have consistent API signatures
// - nrvo: several helpers return one of multiple local ValueRep candidates
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wexceptions"
#pragma clang diagnostic ignored "-Wunused-parameter"
#if __has_warning("-Wnrvo")
#pragma clang diagnostic ignored "-Wnrvo"
#endif
#endif

namespace lightusd {
namespace experimental {

CrateWriter::InternSink::~InternSink() = default;

// Out-of-line virtual destructors to anchor vtables in this TU.
IOutputStream::~IOutputStream() = default;
MemoryOutputStream::~MemoryOutputStream() = default;

namespace {

FILE *ProfileOutput() {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  FILE *output = stderr;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  return output;
}

// Magic identifier for USDC files
constexpr char kMagicIdent[] = "PXR-USDC";

/// FileOutputStream — writes to disk via std::fstream
class FileOutputStream : public IOutputStream {
public:
  explicit FileOutputStream(const std::string& filepath) : filepath_(filepath) {}
  bool Open(std::string* err) override {
    file_.open(filepath_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
      if (err) *err = "Failed to open file: " + filepath_;
      return false;
    }
    return true;
  }
  void Close() override { if (file_.is_open()) { file_.flush(); file_.close(); } }
  bool IsOpen() const override { return file_.is_open(); }
  int64_t Tell() override { return static_cast<int64_t>(file_.tellp()); }
  bool Seek(int64_t pos) override { file_.seekp(pos, std::ios::beg); return file_.good(); }
  bool Write(const void* data, size_t size) override {
    file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file_.good();
  }
  bool Flush() override { file_.flush(); return file_.good(); }
private:
  std::string filepath_;
  std::fstream file_;
};

// Section names
constexpr char kTokensSection[] = "TOKENS";
constexpr char kStringsSection[] = "STRINGS";
constexpr char kFieldsSection[] = "FIELDS";
constexpr char kFieldSetsSection[] = "FIELDSETS";
constexpr char kPathsSection[] = "PATHS";
constexpr char kSpecsSection[] = "SPECS";


} // anonymous namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

CrateWriter::CrateWriter(const std::string& filepath)
    : filepath_(filepath),
      stream_(std::unique_ptr<IOutputStream>(new FileOutputStream(filepath))) {
}

CrateWriter::CrateWriter(std::unique_ptr<IOutputStream> stream)
    : stream_(std::move(stream)) {
}

CrateWriter::~CrateWriter() {
  Close();
}

// ============================================================================
// NanAwareHash implementation
//
// This hash is used by the storage deduplicator, not by semantic USD value
// comparison. Authored values that differ only in an IEEE-754 representation
// detail (including the sign of zero or a NaN payload) must not share an
// already-written Crate payload.
// ============================================================================

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wused-but-marked-unused"
#endif
size_t CrateWriter::NanAwareHash::hash_buffer(const void *data,
                                               size_t byte_count,
                                               size_t element_size,
                                               bool is_float) {
  if (!is_float) {
    // Non-float: hash raw bytes directly with XXH3
    return static_cast<size_t>(XXH_INLINE_XXH3_64bits(data, byte_count));
  }
  (void)element_size;
  return static_cast<size_t>(XXH_INLINE_XXH3_64bits(data, byte_count));
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

bool CrateWriter::NanAwareHash::buffers_equal(const void *a, const void *b,
                                               size_t byte_count,
                                               size_t element_size,
                                               bool is_float) {
  (void)element_size;
  (void)is_float;
  return byte_count == 0 || std::memcmp(a, b, byte_count) == 0;
}

// ============================================================================
// Public API
// ============================================================================

bool CrateWriter::Open(std::string* err) {
  if (is_open_) {
    if (err) *err = "File already open";
    return false;
  }

  // Open the output stream
  if (!stream_) {
    if (err) *err = "No output stream configured";
    return false;
  }
  if (!stream_->Open(err)) {
    return false;
  }

  is_open_ = true;

  // Reserve space for bootstrap header (we'll write it at the end)
  // Bootstrap is 72 bytes: 8 (ident) + 8 (version) + 8 (toc_offset) + 48 (reserved)
  char zeros[72] = {0};
  if (!WriteBytes(zeros, sizeof(zeros))) {
    if (err) *err = "Failed to write bootstrap placeholder";
    Close();
    return false;
  }

  value_data_start_offset_ = Tell();
  value_data_end_offset_ = value_data_start_offset_;

  // Reserve token index 0 with a sentinel that cannot be a valid path
  // element. OpenUSD does the same (crateFile.cpp line 2594-2601, github
  // issue #811). The compressed path format uses negative token indices
  // for property path elements; -0 == 0 would otherwise make a property
  // at index 0 indistinguishable from a prim element, so ";-)" must sit
  // at index 0 before any caller has a chance to register a real token.
  // Previously this was deferred to Finalize(), but by then AddSpec had
  // already pushed field-name tokens into tokens_ at index 0.
  GetOrCreateToken(";-)");

  return true;
}

bool CrateWriter::AddSpec(const Path& path,
                           SpecType spec_type,
                           crate::FieldValuePairVector fields,
                           std::string* err) {
  if (!is_open_) {
    if (err) *err = "File not open";
    return false;
  }

  if (is_finalized_) {
    if (err) *err = "File already finalized";
    return false;
  }

  // NOTE: duplicate-spec-path detection (USD Crate requires each path to
  // appear only once; first AddSpec wins, silently) happens in Finalize's
  // sorted rebuild: the spec sort tiebreaks equal paths on insertion order,
  // making duplicates adjacent with the first-added spec first, so dedup is
  // a free adjacent compare there. A prior implementation kept a
  // Path-keyed hash set here — one Path hash + equality per AddSpec, which
  // was a top-5 profile entry on spec-dense scenes.

  // Estimate memory usage for this spec
  // Path + field names + approximate field value sizes
  // (prim+prop part sizes; avoids allocating a full_path_name() string)
  int64_t estimated_memory =
      int64_t(path.prim_part().size() + path.prop_part().size() + 1);
  for (const auto& field : fields) {
    estimated_memory += int64_t(field.first.size());  // Field name
    estimated_memory += 64;  // Approximate field value overhead
  }

  // Check memory limit
  if (WouldExceedMemoryLimit(estimated_memory)) {
    if (err) {
      *err = "Adding spec would exceed memory limit of " +
             std::to_string(options_.max_memory_bytes / (1024*1024)) + " MB. " +
             "Current usage: " +
             std::to_string(memory_used_estimate_.load(std::memory_order_relaxed) /
                            (1024 * 1024)) +
             " MB";
    }
    return false;
  }

  // Create spec data. We'll fill in the actual crate::Spec later during
  // Finalize; for now, just accumulate the data (moved, not copied — field
  // values can carry large arrays).
  SpecData spec_data;
  spec_data.path = path;
  spec_data.spec_type = spec_type;  // Store the spec type
  spec_data.fields = std::move(fields);

  // Pre-register tokens from field names (token indices are assigned in
  // first-seen order, so this must stay before Finalize). Under a per-thread
  // spec sink (parallel stage->specs conversion) registration is skipped
  // here and replayed serially in DFS spec order after assembly — same
  // first-seen order, byte-identical token numbering.
  spec_data.fields_at_addspec = static_cast<uint32_t>(spec_data.fields.size());
  if (!tls_spec_sink()) {
    for (const auto& field : spec_data.fields) {
      GetOrCreateToken(field.first);
    }
  }

  active_spec_buffer().push_back(std::move(spec_data));
  memory_used_estimate_ += estimated_memory;

  // NOTE: no GetOrCreatePath(path) here — Finalize() clears and rebuilds the
  // path table from the sorted specs, so pre-registering paths was pure
  // wasted work (hash + ancestor walk per spec).

  return true;
}

namespace {

// Flattened crate-path sort key; plain byte comparison of two keys reproduces
// pathlib::CompareParsedPaths ordering (see the sort in Finalize).
std::string BuildCratePathSortKey(const lightusd::Path& path) {
  const std::string& prim = path.prim_part();
  const std::string& prop = path.prop_part();
  std::string key;
  key.reserve(prim.size() + prop.size() + 2);
  const bool is_abs = !prim.empty() && (prim[0] == '/');
  key.push_back(is_abs ? '\x01' : '\x7e');
  // Join prim elements with '\x02', skipping empty segments (mirrors
  // ParsePath). Root "/" contributes a single empty element, i.e. nothing
  // beyond the marker — which sorts before every non-root absolute path.
  size_t start = is_abs ? 1 : 0;
  bool first = true;
  while (start < prim.size()) {
    size_t end = prim.find('/', start);
    if (end == std::string::npos) {
      end = prim.size();
    }
    if (end > start) {
      if (!first) {
        key.push_back('\x02');
      }
      key.append(prim, start, end - start);
      first = false;
    }
    start = end + 1;
  }
  if (!prop.empty()) {
    key.push_back('\x01');
    key.append(prop);
  }
  return key;
}

// Round-13: byte-identical parallel sort of a Schwartzian `order` index array
// by precomputed `keys`, tiebreaking on original index. Total order (the
// tiebreak makes every pair strictly comparable), so any correct sort yields
// the exact same result as the serial std::sort it replaces. P sorted chunks
// (parallel) + pairwise double-buffered merge rounds. Falls back to std::sort
// when threads are unavailable or n is small.
inline void SortOrderByKeys(std::vector<uint32_t>& order,
                            const std::vector<std::string>& keys,
                            size_t nthreads) {
  auto less = [&keys](uint32_t a, uint32_t b) {
    const int c = keys[a].compare(keys[b]);
    return c != 0 ? c < 0 : a < b;
  };
  const size_t n = order.size();
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Round P down to a power of two so the pairwise tree merge consumes runs
  // cleanly; need enough work per chunk to pay for the threads.
  size_t P = 1;
  while (P * 2 <= nthreads && P * 2 <= (n / 8192 + 1)) P *= 2;
  if (P >= 2) {
    std::vector<size_t> bnd(P + 1);
    const size_t chunk = (n + P - 1) / P;
    for (size_t t = 0; t <= P; t++) bnd[t] = std::min(n, t * chunk);
    // Parallel per-chunk sort.
    {
      std::vector<std::thread> ths;
      ths.reserve(P - 1);
      auto sort_chunk = [&](size_t t) {
        std::sort(order.begin() + std::ptrdiff_t(bnd[t]),
                  order.begin() + std::ptrdiff_t(bnd[t + 1]), less);
      };
      for (size_t t = 1; t < P; t++) ths.emplace_back(sort_chunk, t);
      sort_chunk(0);
      for (auto& th : ths) th.join();
    }
    // Pairwise tree merge with double buffering. `bnd` holds current run
    // boundaries; each round halves the run count.
    std::vector<uint32_t> scratch(n);
    uint32_t* src = order.data();
    uint32_t* dst = scratch.data();
    size_t runs = P;
    while (runs > 1) {
      const size_t pairs = runs / 2;
      std::vector<std::thread> ths;
      ths.reserve(pairs - 1);
      auto merge_pair = [&](size_t p) {
        const size_t lo = bnd[2 * p], mid = bnd[2 * p + 1], hi = bnd[2 * p + 2];
        std::merge(src + lo, src + mid, src + mid, src + hi, dst + lo, less);
      };
      for (size_t p = 1; p < pairs; p++) ths.emplace_back(merge_pair, p);
      if (pairs > 0) merge_pair(0);
      for (auto& th : ths) th.join();
      // Odd trailing run: copy straight through.
      if (runs & 1) {
        const size_t lo = bnd[runs - 1], hi = bnd[runs];
        std::copy(src + lo, src + hi, dst + lo);
      }
      std::vector<size_t> nb(pairs + (runs & 1) + 1);
      for (size_t p = 0; p <= pairs; p++) nb[p] = bnd[2 * p];
      if (runs & 1) nb[pairs] = bnd[runs - 1];
      nb.back() = n;
      bnd = std::move(nb);
      runs = bnd.size() - 1;
      std::swap(src, dst);
    }
    if (src != order.data()) std::copy(src, src + n, order.data());
    return;
  }
#endif
  (void)nthreads;
  (void)n;
  std::sort(order.begin(), order.end(), less);
}

}  // namespace

// Scoped ns accumulator for the LIGHTUSD_CRATE_PROFILE Finalize report.
// Zero-cost (one branch) when disabled.
namespace {
#if defined(LIGHTUSD_ENABLE_THREAD)
// Writer worker-thread budget (same cap as the two-pass field packing).
size_t WriterParallelThreads() {
  const unsigned hw = std::thread::hardware_concurrency();
  return (std::max<size_t>)(
      1, (std::min<size_t>)(static_cast<size_t>(hw ? hw : 1), size_t(16)));
}

// Blocking parallel-for over [0, n) in static contiguous ranges;
// fn(begin, end) must only write disjoint slots.
template <typename Fn>
void ParallelForRanges(size_t n, size_t nthreads, Fn&& fn) {
  if (n == 0) return;
  const size_t nt = (std::min)(nthreads, n);
  if (nt <= 1) {
    fn(size_t(0), n);
    return;
  }
  std::vector<std::thread> ths;
  const size_t chunk = (n + nt - 1) / nt;
  ths.reserve(nt);
  for (size_t t = 0; t < nt; t++) {
    const size_t b = t * chunk;
    const size_t e = (std::min)(n, b + chunk);
    if (b >= e) break;
    ths.emplace_back([&fn, b, e]() { fn(b, e); });
  }
  for (auto& th : ths) {
    th.join();
  }
}
#endif  // LIGHTUSD_ENABLE_THREAD

struct ProfScope {
  bool on;
  uint64_t* acc;
  std::chrono::steady_clock::time_point t0;
  ProfScope(bool enabled, uint64_t* accumulator) : on(enabled), acc(accumulator) {
    if (on) t0 = std::chrono::steady_clock::now();
  }
  ~ProfScope() {
    if (on) {
      *acc += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - t0)
              .count());
    }
  }
};
}  // namespace

bool CrateWriter::Finalize(std::string* err) {
  if (!is_open_) {
    if (err) *err = "File not open";
    return false;
  }

  if (is_finalized_) {
    if (err) *err = "File already finalized";
    return false;
  }

  // Finalize phase profiling (LIGHTUSD_CRATE_PROFILE=1): stderr report of the
  // sort / path-rebuild / field-pack split, with PackValue further split into
  // inline/dedup/write/other. Timer overhead inflates the profiled run; use
  // the shares, not the absolute wall.
  profile_finalize_ = (std::getenv("LIGHTUSD_CRATE_PROFILE") != nullptr);
  const auto prof_now = []() { return std::chrono::steady_clock::now(); };
  const auto prof_ns = [](std::chrono::steady_clock::time_point a,
                          std::chrono::steady_clock::time_point b) -> uint64_t {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
  };
  const auto prof_t0 = prof_now();
  auto prof_t_sort = prof_t0, prof_t_paths = prof_t0, prof_t_fields = prof_t0;

  // ========================================================================
  // Step 1: Process all specs and build internal tables
  // ========================================================================

  // Reserve token index 0 with a sentinel that can't be a valid path element.
  // OpenUSD does the same (crateFile.cpp line 2594-2601, github issue #811).
  // The compressed path format uses negative token indices for property path
  // elements.  Because -0 == 0, a property at index 0 would be misread as a
  // prim element.  Inserting ";-)" here before any other token ensures no real
  // element gets index 0.
  if (tokens_.empty()) {
    GetOrCreateToken(";-)");
  }

  // Phase 5: Sort specs for better compression and correct hierarchy
  // CRITICAL: Sort specs using the same USD path comparison algorithm
  // that will be used in WritePathsSection (SortSimplePaths).
  // This ensures path indices assigned here match the path tree encoding.
  //
  // PseudoRoot ("/") MUST be first (required by USD spec)
  //
  // Schwartzian transform with FLATTENED string keys: the crate path order
  // (absolute-first, element-wise lexicographic with shorter-prefix-first,
  // properties before children, property names bytewise) is encoded into a
  // single byte string per path — '\x01' marks absolute (relative gets
  // '\x7e' so it sorts after every absolute path), elements join with
  // '\x02' and the property appends after '\x01' ('\x01' < '\x02' puts
  // /A.prop before /A/B). Plain std::string comparison then reproduces
  // pathlib::CompareParsedPaths exactly, without re-parsing per comparison.
  {
    const size_t n = spec_data_.size();
    // Thread budget for the parallel key-build + merge sort (round 13). The
    // serial path is byte-identical (same total order); both fall back to
    // serial when nthreads==1 or n is small.
    size_t nthreads = 1;
#if defined(LIGHTUSD_ENABLE_THREAD)
    {
      const unsigned hw = std::thread::hardware_concurrency();
      nthreads = (std::max<size_t>)(1, (std::min<size_t>)(hw ? hw : 1, 16));
    }
#endif
    std::vector<std::string> keys(n);
    // Parallel Schwartzian key build (BuildCratePathSortKey is pure; keys[i]
    // slots are written disjointly).
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (nthreads > 1 && n >= 8192) {
      const size_t chunk = (n + nthreads - 1) / nthreads;
      std::vector<std::thread> ths;
      ths.reserve(nthreads - 1);
      auto build = [&](size_t b, size_t e) {
        for (size_t i = b; i < e; i++)
          keys[i] = BuildCratePathSortKey(spec_data_[i].path);
      };
      for (size_t t = 1; t < nthreads; t++) {
        const size_t b = t * chunk, e = (std::min)(b + chunk, n);
        if (b >= e) break;
        ths.emplace_back(build, b, e);
      }
      build(0, (std::min)(chunk, n));
      for (auto& th : ths) th.join();
    } else
#endif
    {
      for (size_t i = 0; i < n; i++)
        keys[i] = BuildCratePathSortKey(spec_data_[i].path);
    }
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; i++) {
      order[i] = uint32_t(i);
    }
    // Tiebreak equal keys on insertion order: keeps first-AddSpec-wins
    // duplicate handling exact (dedup happens in the rebuild below) and
    // makes the order among equal-key paths (e.g. variant-bearing specs,
    // whose key ignores the variant part) deterministic and stable.
    // Byte-identical parallel merge sort (serial std::sort fallback inside).
    SortOrderByKeys(order, keys, nthreads);
    std::vector<SpecData> sorted_specs;
    sorted_specs.reserve(n);
    for (size_t i = 0; i < n; i++) {
      sorted_specs.push_back(std::move(spec_data_[order[i]]));
    }
    spec_data_ = std::move(sorted_specs);
  }
  if (profile_finalize_) prof_t_sort = prof_now();

  // Verify that the first spec is PseudoRoot (required by USD spec)
  if (!spec_data_.empty()) {
    const auto& first_spec = spec_data_[0];
    bool is_pseudoroot = (first_spec.spec_type == SpecType::PseudoRoot ||
                          (first_spec.path.prim_part() == "/" && first_spec.path.prop_part().empty()));

    if (!is_pseudoroot) {
      if (err) {
        *err = "First spec must be PseudoRoot (path '/'), but got: " +
               first_spec.path.prim_part() +
               (first_spec.path.prop_part().empty() ? "" : "." + first_spec.path.prop_part());
      }
      return false;
    }
  }

  // CRITICAL: Rebuild path deduplication table to match sorted order
  // Path indices must correspond to the sorted spec order.
  // Specs are sorted, so duplicate paths (if any) are adjacent — dedup by
  // comparing with the previous path and assign each spec's path index here
  // (one hash-map insert per unique path; the old flow did a find+insert per
  // spec plus a separate GetOrCreatePath lookup per spec below).
  path_slots_.clear();
  path_slots_used_ = 0;
  GrowPathSlots(spec_data_.size() + 16);
  paths_.clear();
  {
    const crate::PathHasher hasher;
    // Path hashing is a pure function of the (immutable here) spec paths —
    // precompute it in parallel and keep only the order-dependent slot
    // insertion serial.
    std::vector<uint32_t> spec_path_hashes(spec_data_.size());
#if defined(LIGHTUSD_ENABLE_THREAD)
    ParallelForRanges(spec_data_.size(), WriterParallelThreads(),
                      [&](size_t b, size_t e) {
                        for (size_t i = b; i < e; i++) {
                          spec_path_hashes[i] =
                              static_cast<uint32_t>(hasher(spec_data_[i].path));
                        }
                      });
#else
    for (size_t i = 0; i < spec_data_.size(); i++) {
      spec_path_hashes[i] = static_cast<uint32_t>(hasher(spec_data_[i].path));
    }
#endif
    paths_.reserve(spec_data_.size());
    size_t w = 0;
    for (size_t r = 0; r < spec_data_.size(); r++) {
      SpecData& spec_data = spec_data_[r];
      if (!paths_.empty() && paths_.back() == spec_data.path) {
        // Duplicate spec path: keep the first-added spec (the sort above
        // tiebreaks on insertion order), silently drop this one — same
        // behavior as the old AddSpec-time hash-set dedup.
        continue;
      }
      const uint32_t idx = static_cast<uint32_t>(paths_.size());
      // MOVE the path into paths_: nothing reads spec_data.path after this
      // rebuild (the sort keys and the hash precompute above were the last
      // readers), and the duplicate check compares against the moved-in
      // paths_.back(). Saves one Path (two-string) copy per unique spec.
      paths_.push_back(std::move(spec_data.path));
      InsertPathSlot(spec_path_hashes[r], idx);
      spec_data.spec.path_index.value = idx;
      if (w != r) spec_data_[w] = std::move(spec_data);
      w++;
    }
    spec_data_.resize(w);
  }
  if (profile_finalize_) prof_t_paths = prof_now();

  // Build field and fieldset tables.
  //
  // Pass B / serial body: identical shared-state effect sequence either way —
  // field-name intern, TokenVector special case, value pack (from a pass-A
  // plan when one exists, plain PackValue otherwise), field/fieldset dedup.
  auto pack_spec_fields = [&](SpecData& spec_data,
                              std::vector<PackPlan>* plans_for_spec) -> bool {
    std::vector<crate::FieldIndex> field_indices;

    for (size_t fi = 0; fi < spec_data.fields.size(); fi++) {
      const auto& field_pair = spec_data.fields[fi];
      // Create field
      crate::Field field;
      const auto prof_f0 = profile_finalize_
                               ? prof_now()
                               : std::chrono::steady_clock::time_point{};
      field.token_index = GetOrCreateToken(field_pair.first);
      if (profile_finalize_) prof_field_dedup_ns_ += prof_ns(prof_f0, prof_now());

      // USD metadata fields `primChildren` and `properties` store a list of
      // child/property names. On the wire, pxrusd expects these as the
      // dedicated uncompressed `TokenVector` type (CrateDataTypeId 41), not
      // as a `Token[]` array. Large Token[] arrays may be integer-compressed,
      // and retagging those bytes as TokenVector produces scalar ValueReps
      // with the compressed bit set, which OpenUSD rejects.
      const std::string& fname = field_pair.first;
      if ((fname == "primChildren" || fname == "properties" ||
           fname == "variantSetChildren" || fname == "variantChildren") &&
          field_pair.second.as<std::vector<value::token>>()) {
        field.value_rep = PackTokenVectorValueDeduped(
            *field_pair.second.as<std::vector<value::token>>(), err);
      } else if (fname == "subLayers" &&
                 field_pair.second.as<std::vector<std::string>>()) {
        // Must be the dedicated StringVector type or pxr ignores the
        // sublayers entirely (see PackStringVectorValue). Intercepts BEFORE
        // the pass-A plan (like the TokenVector special case above) — a plan
        // for this field is ignored.
        field.value_rep = PackStringVectorValue(
            *field_pair.second.as<std::vector<std::string>>(), err);
      } else if (fname == "subLayerOffsets" &&
                 field_pair.second.as<std::vector<LayerOffset>>()) {
        field.value_rep = PackLayerOffsetVectorValue(
            *field_pair.second.as<std::vector<LayerOffset>>(), err);
      } else if (plans_for_spec) {
        field.value_rep =
            PackValueFromPlan(field_pair.second, (*plans_for_spec)[fi], err);
      } else {
        field.value_rep = PackValue(field_pair.second, err);
      }
      if (err && !err->empty()) {
        return false;
      }

      // Get or create field index
      const auto prof_f1 = profile_finalize_
                               ? prof_now()
                               : std::chrono::steady_clock::time_point{};
      crate::FieldIndex field_idx = GetOrCreateField(field);
      if (profile_finalize_) prof_field_dedup_ns_ += prof_ns(prof_f1, prof_now());
      field_indices.push_back(field_idx);
    }

    // Get or create fieldset
    const auto prof_f2 = profile_finalize_
                             ? prof_now()
                             : std::chrono::steady_clock::time_point{};
    crate::FieldSetIndex fieldset_idx = GetOrCreateFieldSet(field_indices);
    if (profile_finalize_) prof_field_dedup_ns_ += prof_ns(prof_f2, prof_now());

    // (spec.path_index was assigned in the sorted-rebuild loop above.)
    spec_data.spec.fieldset_index = fieldset_idx;
    spec_data.spec.spec_type = spec_data.spec_type;  // Use the stored spec type
    return true;
  };

  // Deferred-interning two-pass (round 14), gated on LIGHTUSD_ENABLE_THREAD:
  // pass A precomputes pure per-field work in parallel (see BuildPackPlan),
  // pass B replays shared-state effects in exact serial order. Windowed so
  // the prebuilt out-of-line buffers never hold more than a slice of the
  // VALUE section at once.
  bool two_pass_pack = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  size_t pack_nthreads = 1;
  {
    const unsigned hw = std::thread::hardware_concurrency();
    pack_nthreads = (std::max<size_t>)(
        1, (std::min<size_t>)(static_cast<size_t>(hw ? hw : 1), size_t(16)));
  }
  two_pass_pack = (pack_nthreads > 1) && (spec_data_.size() >= 1024);
#endif

  if (!two_pass_pack) {
    for (auto& spec_data : spec_data_) {
      if (!pack_spec_fields(spec_data, nullptr)) {
        return false;
      }
    }
  }
#if defined(LIGHTUSD_ENABLE_THREAD)
  else {
    const size_t kWindowSpecs = 65536;
    const size_t total = spec_data_.size();
    std::vector<std::vector<PackPlan>> window_plans;
    for (size_t w0 = 0; w0 < total; w0 += kWindowSpecs) {
      const size_t w1 = (std::min)(total, w0 + kWindowSpecs);
      window_plans.assign(w1 - w0, std::vector<PackPlan>());

      // Pass A: fill plans concurrently. Dynamic chunking — value sizes are
      // heavily skewed (giant mesh arrays next to one-field specs).
      {
        ProfScope prof_pa(profile_finalize_, &prof_passa_ns_);
        PackPlanContext ctx;
        std::atomic<size_t> cursor{w0};
        const size_t kChunk = 64;
        auto worker = [&]() {
          for (;;) {
            const size_t begin = cursor.fetch_add(kChunk);
            if (begin >= w1) break;
            const size_t end = (std::min)(w1, begin + kChunk);
            for (size_t i = begin; i < end; i++) {
              SpecData& sd = spec_data_[i];
              std::vector<PackPlan>& plans = window_plans[i - w0];
              plans.resize(sd.fields.size());
              for (size_t fi = 0; fi < sd.fields.size(); fi++) {
                BuildPackPlan(sd.fields[fi].second, &plans[fi], &ctx);
              }
            }
          }
        };
        std::vector<std::thread> ths;
        ths.reserve(pack_nthreads);
        for (size_t t = 0; t < pack_nthreads; t++) {
          ths.emplace_back(worker);
        }
        for (auto& th : ths) {
          th.join();
        }
      }

      // Pass B: serial replay in exact order.
      {
        ProfScope prof_pb(profile_finalize_, &prof_passb_ns_);
        for (size_t i = w0; i < w1; i++) {
          if (!pack_spec_fields(spec_data_[i], &window_plans[i - w0])) {
            return false;
          }
        }
      }
    }
  }
#endif
  if (profile_finalize_) {
    prof_t_fields = prof_now();
    const double ms = 1e-6;
    fprintf(ProfileOutput(),
            "[crate-writer profile] Finalize: sort %.1fms | path-rebuild %.1fms "
            "| field-pack %.1fms (PackValue: inline %.1fms, dedup %.1fms, "
            "write %.1fms, other %.1fms; field/fieldset dedup %.1fms)\n",
            double(prof_ns(prof_t0, prof_t_sort)) * ms,
            double(prof_ns(prof_t_sort, prof_t_paths)) * ms,
            double(prof_ns(prof_t_paths, prof_t_fields)) * ms,
            double(prof_pack_inline_ns_) * ms, double(prof_pack_dedup_ns_) * ms,
            double(prof_pack_write_ns_) * ms,
            double(prof_pack_total_ns_ -
                   (std::min)(prof_pack_total_ns_,
                              prof_pack_inline_ns_ + prof_pack_dedup_ns_ +
                                  prof_pack_write_ns_)) *
                ms,
            double(prof_field_dedup_ns_) * ms);
    if (prof_passa_ns_ || prof_passb_ns_) {
      fprintf(ProfileOutput(),
              "[crate-writer profile]   two-pass: pass A %.1fms (parallel) | "
              "pass B %.1fms (serial replay)\n",
              double(prof_passa_ns_) * ms, double(prof_passb_ns_) * ms);
    }
  }

  // ========================================================================
  // Path-index remap after field packing
  // ========================================================================
  // During field packing, ListOp<Reference> / ListOp<Payload> /
  // ListOp<Path> / connection-target / pathvector value data wrote raw
  // PathIndex bytes referencing positions in paths_ at the time of the
  // write. Historically we re-sorted paths_ here for tree-encoding ordering
  // and remapped only the spec path_indexes — that left those embedded
  // value-data indices pointing at the wrong paths after the sort, so e.g.
  // `references = @./a.usda@</A>` came back as `</x>` (whatever happened
  // to land at the original index after re-sorting).
  //
  // Fix: skip the re-sort. WritePathsSection sorts paths internally for
  // tree encoding and stores `encoded_path_indices[i] = preassigned`, so
  // the on-disk tree still maps correctly to the original PathIndex values
  // — and value-data path indices stay valid.

  // ========================================================================
  // Step 2: Write all structural sections
  // ========================================================================

  // Note: value_data_end_offset_ is already updated by WriteValueData()
  // during PackValue() calls above. Do NOT reset it here.

  // ========================================================================
  // Pre-register path element tokens before writing TOKENS section.
  // WritePathsSection uses GetOrCreateToken during tree building.
  // WritePathsSection rewrites the root path's element name ("/") to the
  // empty string before tokenizing, so we must ensure "" is in the pool;
  // otherwise the root row references a token that only gets appended
  // after the TOKENS section has already been serialized — which pxrusd
  // rejects with "Corrupt path element token index in crate file".
  GetOrCreateToken("");
  for (const auto& path : paths_) {
    std::string elem = path.element_name();
    // Keep in lockstep with WritePathsSection: a trailing variant selection is
    // tokenized as the bare `{set=sel}` group, not the whole slash-segment.
    // Registering the unstripped form here would leave the stripped one to be
    // appended AFTER the TOKENS section is serialized -- the exact "Corrupt
    // path element token index" failure this loop exists to prevent.
    if (!elem.empty() && elem.back() == '}') {
      size_t open = elem.find_last_of('{');
      if (open != std::string::npos) {
        elem = elem.substr(open);
      }
    }
    if (!elem.empty() && elem != "/") {
      GetOrCreateToken(elem);
    }
    if (!path.prop_part().empty()) {
      GetOrCreateToken(path.prop_part());
    }
  }

  // Seek to the end of value data section before writing structural sections
  // (WriteValueData() seeks back after writing, so file position is not at the end)
  if (!Seek(value_data_end_offset_)) {
    if (err) *err = "Failed to seek to end of value data section";
    return false;
  }

  // Write sections in order
  auto prof_t_sec = prof_now();
  const auto prof_section = [&](const char* name) {
    if (!profile_finalize_) return;
    const auto now = prof_now();
    fprintf(ProfileOutput(),
            "[crate-writer profile]   section %s: %.1fms\n", name,
            double(prof_ns(prof_t_sec, now)) * 1e-6);
    prof_t_sec = now;
  };
  if (!WriteTokensSection(err)) return false;
  prof_section("TOKENS");
  if (!WriteStringsSection(err)) return false;
  prof_section("STRINGS");
  if (!WriteFieldsSection(err)) return false;
  prof_section("FIELDS");
  if (!WriteFieldSetsSection(err)) return false;
  prof_section("FIELDSETS");
  if (!WritePathsSection(err)) return false;
  prof_section("PATHS");
  if (!WriteSpecsSection(err)) return false;
  prof_section("SPECS");

#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    // All sections that consume spec data are written — release the heavy
    // per-spec payload (fields hold the CrateValues, i.e. the attribute
    // arrays) in parallel now instead of serially in the writer destructor.
    // Element cleanup is independent; the destructor then frees only empty
    // shells. Not byte-observable (nothing reads spec fields/paths after
    // SPECS), but it moves ~all of the teardown wall onto worker threads.
    ParallelForRanges(spec_data_.size(), WriterParallelThreads(),
                      [&](size_t b, size_t e) {
                        for (size_t i = b; i < e; i++) {
                          crate::FieldValuePairVector().swap(
                              spec_data_[i].fields);
                          spec_data_[i].path = Path();
                        }
                      });
    prof_section("spec-data release");
  }
#endif

  // ========================================================================
  // Step 3: Write Table of Contents
  // ========================================================================

  if (!WriteTableOfContents(err)) return false;

  // ========================================================================
  // Step 4: Write Bootstrap header
  // ========================================================================

  if (!WriteBootStrap(err)) return false;

  is_finalized_ = true;

  return true;
}

void CrateWriter::Close() {
  if (stream_ && stream_->IsOpen()) {
    stream_->Flush();
    stream_->Close();
  }
  is_open_ = false;
}

// ============================================================================
// TimeSamples Value Conversion (Phase 5)
// ============================================================================


// ============================================================================
// Array Deduplication (Phase 5)
// ============================================================================

/// Helper to serialize array to bytes for deduplication
template<typename T>
std::vector<char> SerializeArrayToBytes(const std::vector<T>& arr) {
  std::vector<char> bytes;
  size_t total_size;
  if (!safe::mul(arr.size(), sizeof(T), &total_size)) {
    return {};  // overflow
  }
  bytes.resize(total_size);
  std::memcpy(bytes.data(), arr.data(), total_size);
  return bytes;
}

// ============================================================================
// Compression (Phase 4)
// ============================================================================

bool CrateWriter::CompressData(const char* input, size_t inputSize,
                                std::vector<char>* compressed, std::string* err) {
  if (!compressed) {
    if (err) *err = "CompressData: compressed output buffer is null";
    return false;
  }

  // IMPORTANT: USD crate format uses OpenUSD's TfFastCompression format:
  // - 1 byte: chunk count (0 for single chunk, N for multiple chunks)
  // - If chunk count == 0: raw LZ4 compressed data
  // - If chunk count > 0: for each chunk: int32_t size + LZ4 compressed data
  //
  // For simplicity, we always use single-chunk mode (chunk count = 0)
  // See: pxr/base/tf/fastCompression.cpp in OpenUSD

  // LZ4_compressBound takes int inputSize; reject sizes that overflow int.
  if (inputSize > static_cast<size_t>((std::numeric_limits<int>::max)())) {
    if (err) *err = "Input size too large for LZ4 compression: " + std::to_string(inputSize);
    return false;
  }

  // Get maximum compressed size
  int maxCompressedSize = LZ4_compressBound(static_cast<int>(inputSize));
  if (maxCompressedSize <= 0) {
    if (err) *err = "Input size too large for LZ4 compression: " + std::to_string(inputSize);
    return false;
  }

  // Allocate buffer: 1 byte for chunk count + compressed data
  compressed->resize(1 + static_cast<size_t>(maxCompressedSize));

  // Write chunk count byte (0 = single chunk)
  (*compressed)[0] = 0;

  // Compress with LZ4 (compatible with OpenUSD TfFastCompression)
  int compressedSize = LZ4_compress_default(
      input,
      compressed->data() + 1,  // Skip the chunk count byte
      static_cast<int>(inputSize),
      maxCompressedSize);

  if (compressedSize <= 0) {
    if (err) *err = "LZ4 compression failed with error code: " + std::to_string(compressedSize);
    return false;
  }

  // Resize to actual size: 1 byte chunk count + compressed data
  compressed->resize(1 + static_cast<size_t>(compressedSize));

  return true;
}

// ============================================================================
// Section Writing
// ============================================================================

bool CrateWriter::WriteTokensSection(std::string* err) {
  int64_t section_start = Tell();

  // Write token count
  uint64_t token_count = static_cast<uint64_t>(tokens_.size());

  // Write directly as bytes instead of using Write() template
  if (!stream_->Write(reinterpret_cast<const char*>(&token_count), sizeof(token_count))) {
    if (err) *err = "Failed to write token count bytes";
    return false;
  }
  stream_->Flush();

  // Build token blob (null-terminated strings)
  std::ostringstream blob;
  for (const auto& token : tokens_) {
    blob << token;
    blob.put('\0');
  }

  std::string token_blob = blob.str();

  // Phase 4: Compress the blob if compression is enabled
  std::vector<char> compressed_blob;
  if (!CompressData(token_blob.data(), token_blob.size(), &compressed_blob, err)) {
    if (err) *err = "Failed to compress token blob: " + *err;
    return false;
  }


  // Write in compressed format (version 0.4.0+):
  // - uncompressedSize (uint64_t)
  // - compressedSize (uint64_t)
  // - compressed data
  uint64_t uncompressed_size = static_cast<uint64_t>(token_blob.size());
  uint64_t compressed_size = static_cast<uint64_t>(compressed_blob.size());

  if (!Write(uncompressed_size)) {
    if (err) *err = "Failed to write token blob uncompressed size";
    return false;
  }

  if (!Write(compressed_size)) {
    if (err) *err = "Failed to write token blob compressed size";
    return false;
  }

  if (!WriteBytes(compressed_blob.data(), compressed_blob.size())) {
    if (err) *err = "Failed to write compressed token blob";
    return false;
  }

  int64_t section_end = Tell();

  // Record section in TOC
  crate::Section section(kTokensSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WriteStringsSection(std::string* err) {
  int64_t section_start = Tell();

  // Strings section is just a vector of TokenIndex
  // Each string maps to a token index

  uint64_t string_count = static_cast<uint64_t>(strings_.size());

  if (!Write(string_count)) {
    if (err) *err = "Failed to write string count";
    return false;
  }

  for (const auto& str : strings_) {
    // Find the token index for this string
    auto it = token_to_index_.find(str);
    if (it == token_to_index_.end()) {
      if (err) *err = "String not found in token table: " + str;
      return false;
    }

    if (!Write(it->second)) {
      if (err) *err = "Failed to write string token index";
      return false;
    }
  }

  int64_t section_end = Tell();

  crate::Section section(kStringsSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WriteFieldsSection(std::string* err) {
  int64_t section_start = Tell();

  // IMPORTANT: Format for v0.4.0+ (we target v0.7.0+):
  // 1. uint64_t numFields
  // 2. uint64_t tokenIndicesCompressedSize
  // 3. Compressed token indices (using Usd_IntegerCompression)
  // 4. uint64_t repsSize
  // 5. Compressed value reps (using TfFastCompression)
  //
  // This differs from older versions which mixed token indices and value reps.
  // See: pxr/usd/sdf/crateFile.cpp _ReadFields() and _WriteFields()

  size_t num_fields = fields_.size();

  // Write field count
  uint64_t field_count = static_cast<uint64_t>(num_fields);
  if (!Write(field_count)) {
    if (err) *err = "Failed to write field count";
    return false;
  }

  // Separate token indices from value reps
  std::vector<uint32_t> token_indices;
  std::vector<uint64_t> value_reps;
  token_indices.reserve(num_fields);
  value_reps.reserve(num_fields);

  for (const auto& field : fields_) {
    token_indices.push_back(field.token_index.value);
    value_reps.push_back(field.value_rep.GetData());
  }

  // Compress token indices using Usd_IntegerCompression
  size_t token_indices_compressed_buffer_size =
      Usd_IntegerCompression::GetCompressedBufferSize(num_fields);
  std::vector<char> compressed_token_indices(token_indices_compressed_buffer_size);

  std::string compress_err;
  size_t token_indices_compressed_size = Usd_IntegerCompression::CompressToBuffer(
      token_indices.data(), num_fields,
      compressed_token_indices.data(), &compress_err);

  if (token_indices_compressed_size == 0) {
    if (err) *err = "Failed to compress token indices: " + compress_err;
    return false;
  }

  compressed_token_indices.resize(token_indices_compressed_size);

  // Write tokenIndicesCompressedSize
  uint64_t token_indices_size = static_cast<uint64_t>(token_indices_compressed_size);
  if (!Write(token_indices_size)) {
    if (err) *err = "Failed to write token indices compressed size";
    return false;
  }

  // Write compressed token indices
  if (!WriteBytes(compressed_token_indices.data(), token_indices_compressed_size)) {
    if (err) *err = "Failed to write compressed token indices";
    return false;
  }

  // Compress value reps using TfFastCompression (our CompressData)
  const char* reps_data = reinterpret_cast<const char*>(value_reps.data());
  size_t reps_data_size;
  if (!safe::mul(value_reps.size(), sizeof(uint64_t), &reps_data_size)) {
    if (err) *err = "Integer overflow: value_reps.size() * sizeof(uint64_t)";
    return false;
  }

  std::vector<char> compressed_reps;
  if (!CompressData(reps_data, reps_data_size, &compressed_reps, err)) {
    if (err) *err = "Failed to compress value reps: " + *err;
    return false;
  }

  // Write repsSize
  uint64_t reps_size = static_cast<uint64_t>(compressed_reps.size());
  if (!Write(reps_size)) {
    if (err) *err = "Failed to write value reps size";
    return false;
  }

  // Write compressed value reps
  if (!WriteBytes(compressed_reps.data(), compressed_reps.size())) {
    if (err) *err = "Failed to write compressed value reps";
    return false;
  }

  int64_t section_end = Tell();

  crate::Section section(kFieldsSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WriteFieldSetsSection(std::string* err) {
  int64_t section_start = Tell();

  // IMPORTANT: Format for v0.4.0+ (we target v0.7.0+):
  // 1. uint64_t numFieldSetVals (total number of FieldIndex values)
  // 2. uint64_t compressedSize (size of compressed data)
  // 3. Compressed field index array (using Usd_IntegerCompression)
  //
  // Fieldsets are stored as a flat array of FieldIndex values.
  // Each fieldset is terminated by a default-constructed FieldIndex.
  //
  // See: pxr/usd/sdf/crateFile.cpp _WriteFieldSets()


  // Flatten all fieldsets into a single array with terminators
  std::vector<uint32_t> fieldset_vals;
  for (size_t i = 0; i < fieldsets_.size(); ++i) {
    const auto& fieldset = fieldsets_[i];
    for (const auto& field_idx : fieldset) {
      fieldset_vals.push_back(field_idx.value);
    }
    // Write terminator (default FieldIndex() has value ~0u)
    fieldset_vals.push_back(~0u);
  }


  size_t num_vals = fieldset_vals.size();

  // Write total number of field index values
  uint64_t val_count = static_cast<uint64_t>(num_vals);
  if (!Write(val_count)) {
    if (err) *err = "Failed to write fieldset value count";
    return false;
  }

  // Compress fieldset values using Usd_IntegerCompression
  size_t buffer_size = Usd_IntegerCompression::GetCompressedBufferSize(num_vals);
  std::vector<char> compressed(buffer_size);

  std::string compress_err;
  size_t compressed_size = Usd_IntegerCompression::CompressToBuffer(
      fieldset_vals.data(), num_vals, compressed.data(), &compress_err);

  if (compressed_size == 0) {
    if (err) *err = "Failed to compress fieldset values: " + compress_err;
    return false;
  }

  // Write compressed size
  uint64_t size = static_cast<uint64_t>(compressed_size);
  if (!Write(size)) {
    if (err) *err = "Failed to write compressed size";
    return false;
  }

  // Write compressed data
  if (!WriteBytes(compressed.data(), compressed_size)) {
    if (err) *err = "Failed to write compressed fieldset data";
    return false;
  }

  int64_t section_end = Tell();

  crate::Section section(kFieldSetsSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WritePathsSection(std::string* err) {
  int64_t section_start = Tell();

  // IMPORTANT: Format for v0.4.0+ (we target v0.7.0+):
  // 1. uint64_t numPaths
  // 2. For each of three arrays (pathIndexes, elementTokenIndexes, jumps):
  //    - uint64_t arrayCompressedSize
  //    - Compressed array data (using Usd_IntegerCompression)
  //
  // See: pxr/usd/sdf/crateFile.cpp _WriteCompressedPathData()

  // Gather encodable paths in paths_ order — the vector position IS the
  // pre-assigned PathIndex (Finalize's rebuild and GetOrCreatePath both keep
  // that invariant), so no hash-map iteration and no Path copies here.
  //
  // "Simple" paths (valid + absolute, no variant part, and every name char
  // sorting after the separators '.' 0x2E and '/' 0x2F — true for all valid
  // USD identifiers/namespaced names: alnum, '_', ':') get a fast tree build:
  // for them, USD path order equals plain byte order of the full path names,
  // subtrees are contiguous byte-order ranges, and parent/child tests reduce
  // to prefix-length compares. Anything exotic falls back to the legacy
  // Path-based ordering and probing.
  std::vector<uint32_t> ids;
  ids.reserve(paths_.size());
  bool all_simple = true;
  for (size_t i = 0; i < paths_.size(); i++) {
    const Path& p = paths_[i];
    // Skip empty/invalid paths
    if (p.prim_part().empty() && p.prop_part().empty()) continue;
    ids.push_back(static_cast<uint32_t>(i));
    if (all_simple) {
      if (!p.is_valid() || !p.is_absolute_path() ||
          !p.variant_part_raw().empty() ||
          !p.variant_selection_raw().empty()) {
        all_simple = false;
      } else {
        const lightusd::tstring_view prim = p.prim_part();
        const char* pd = prim.c_str();
        for (size_t ci = 0; ci < prim.size(); ci++) {
          const char c = pd[ci];
          // Variant-selection elements are embedded in prim_part as
          // `{set=sel}` (variant_part_raw() is EMPTY for them — the guard
          // above does not catch this spelling). '{' passes the > '/' name
          // test, but the fast tree build derives parents via
          // find_last_of('/'), which is WRONG for `/Host{v=a}` (its parent
          // is /Host, not /). Classify as non-simple so variant paths take
          // the legacy Path-based ordering, which handles them correctly.
          if ((c != '/' && static_cast<unsigned char>(c) <= '/') ||
              c == '{') {
            all_simple = false;
            break;
          }
        }
        if (all_simple) {
          const lightusd::tstring_view prop = p.prop_part();
          const char* qd = prop.c_str();
          for (size_t ci = 0; ci < prop.size(); ci++) {
            if (static_cast<unsigned char>(qd[ci]) <= '/') {
              all_simple = false;
              break;
            }
          }
        }
      }
    }
  }

  const size_t num_encoded_paths = ids.size();
  if (num_encoded_paths == 0) {
    if (err) *err = "No paths to encode";
    return false;
  }

  // Full path names in ids order (both modes need them). Pure per-slot
  // string builds — parallel.
  std::vector<std::string> fulls(num_encoded_paths);
#if defined(LIGHTUSD_ENABLE_THREAD)
  ParallelForRanges(num_encoded_paths, WriterParallelThreads(),
                    [&](size_t b, size_t e) {
                      for (size_t k = b; k < e; k++) {
                        fulls[k] = paths_[ids[k]].full_path_name();
                      }
                    });
#else
  for (size_t k = 0; k < num_encoded_paths; k++) {
    fulls[k] = paths_[ids[k]].full_path_name();
  }
#endif

  if (all_simple) {
    // Tree encoding needs sorted USD path order == byte order of full names.
    // Finalize() already built paths_ in that order (specs sorted by the
    // flattened crate sort key, whose ordering matches byte order of the
    // full names; only value-data paths GetOrCreatePath()d during packing
    // can break it) — verify in O(N) and skip the N-log-N string sort in
    // the common case.
    bool presorted = true;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (num_encoded_paths > 1) {
      std::atomic<bool> sorted_flag{true};
      ParallelForRanges(num_encoded_paths - 1, WriterParallelThreads(),
                        [&](size_t b, size_t e) {
                          for (size_t k = b; k < e; k++) {
                            if (!(fulls[k] < fulls[k + 1])) {
                              sorted_flag.store(false, std::memory_order_relaxed);
                              return;
                            }
                          }
                        });
      presorted = sorted_flag.load();
    }
#else
    for (size_t k = 1; k < num_encoded_paths; k++) {
      if (!(fulls[k - 1] < fulls[k])) {
        presorted = false;
        break;
      }
    }
#endif
    if (!presorted) {
      std::vector<uint32_t> order(num_encoded_paths);
      for (size_t k = 0; k < num_encoded_paths; k++) order[k] = static_cast<uint32_t>(k);
      std::sort(order.begin(), order.end(), [&fulls](uint32_t a, uint32_t b) {
        return fulls[a] < fulls[b];
      });
      std::vector<uint32_t> ids2(num_encoded_paths);
      std::vector<std::string> fulls2(num_encoded_paths);
      for (size_t k = 0; k < num_encoded_paths; k++) {
        ids2[k] = ids[order[k]];
        fulls2[k] = std::move(fulls[order[k]]);
      }
      ids = std::move(ids2);
      fulls = std::move(fulls2);
    }
  } else {
    // Legacy ordering: Path::operator< (USD path comparison).
    std::vector<uint32_t> order(num_encoded_paths);
    for (size_t k = 0; k < num_encoded_paths; k++) order[k] = static_cast<uint32_t>(k);
    std::sort(order.begin(), order.end(), [this, &ids](uint32_t a, uint32_t b) {
      return paths_[ids[a]] < paths_[ids[b]];
    });
    std::vector<uint32_t> ids2(num_encoded_paths);
    std::vector<std::string> fulls2(num_encoded_paths);
    for (size_t k = 0; k < num_encoded_paths; k++) {
      ids2[k] = ids[order[k]];
      fulls2[k] = std::move(fulls[order[k]]);
    }
    ids = std::move(ids2);
    fulls = std::move(fulls2);
  }

  // Per-node parent info.
  // Fast mode: parent full name == fulls[k].substr(0, parent_len[k]) — no
  // string allocation (0 marks the root node itself).
  // Legacy mode: materialized parent full-name strings via get_parent_path()
  // (variant-aware), as before.
  std::vector<uint32_t> parent_len;
  std::vector<std::string> parent_fulls;
  if (all_simple) {
    parent_len.resize(num_encoded_paths);
    for (size_t k = 0; k < num_encoded_paths; k++) {
      const Path& p = paths_[ids[k]];
      if (!p.prop_part().empty()) {
        // "/A/B.prop" -> "/A/B"
        parent_len[k] =
            static_cast<uint32_t>(fulls[k].size() - p.prop_part().size() - 1);
      } else if (fulls[k].size() == 1) {
        parent_len[k] = 0;  // root "/"
      } else {
        const size_t pos = fulls[k].find_last_of('/');
        parent_len[k] = static_cast<uint32_t>(pos == 0 ? 1 : pos);
      }
    }
  } else {
    parent_fulls.resize(num_encoded_paths);
    for (size_t k = 0; k < num_encoded_paths; k++) {
      parent_fulls[k] = paths_[ids[k]].get_parent_path().full_path_name();
    }
  }

  // Per-node signed element token indices, precomputed in parallel: Finalize
  // pre-registers every path element name / prop part / "" before the TOKENS
  // section is serialized, so these are read-only lookups. If anything is
  // unexpectedly missing, fall back to interning inside the tree recursion
  // (the historical behavior — interning order must be the DFS build order).
  std::vector<int32_t> node_elem_tokens(num_encoded_paths);
  bool elem_tokens_ok = true;
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    std::atomic<bool> all_found{true};
    ParallelForRanges(
        num_encoded_paths, WriterParallelThreads(), [&](size_t b, size_t e) {
          for (size_t k = b; k < e; k++) {
            const Path& p = paths_[ids[k]];
            const bool is_prop = p.is_prim_property_path();
            std::string elem = is_prop ? p.prop_part() : p.element_name();
            if (elem == "/") elem.clear();
            // Same variant-selection stripping as the serial tree build and
            // the pre-registration loop (pxr rejects unstripped elements).
            if (!is_prop && !elem.empty() && elem.back() == '}') {
              size_t open = elem.find_last_of('{');
              if (open != std::string::npos) {
                elem = elem.substr(open);
              }
            }
            auto it = token_to_index_.find(elem);
            if (it == token_to_index_.end()) {
              all_found.store(false, std::memory_order_relaxed);
              return;
            }
            const int32_t tok = static_cast<int32_t>(it->second.value);
            node_elem_tokens[k] = is_prop ? -tok : tok;
          }
        });
    elem_tokens_ok = all_found.load();
  }
#else
  elem_tokens_ok = false;
#endif

  // Build the three compressed arrays directly from sorted paths
  // (matches OpenUSD's _BuildCompressedPathDataRecursive)
  std::vector<uint32_t> encoded_path_indices(num_encoded_paths);
  std::vector<int32_t> element_token_indices(num_encoded_paths);
  std::vector<int32_t> jump_indices(num_encoded_paths);

  // Fill with invalid sentinel
  for (auto& idx : encoded_path_indices) idx = crate::PathIndex().value;

  // Mode-specific tree probes. All take positions into ids/fulls.
  std::function<bool(uint32_t)> isRootNode;
  // End (exclusive) of the subtree rooted at sidx, scanning within
  // [sidx, eidx).
  std::function<uint32_t(uint32_t, uint32_t)> getNextSubtree;
  // Is node c a DIRECT child of node p?
  std::function<bool(uint32_t, uint32_t)> isDirectChildOf;
  // Do nodes a and b share the same parent?
  std::function<bool(uint32_t, uint32_t)> haveSameParent;

  if (all_simple) {
    isRootNode = [&parent_len](uint32_t k) { return parent_len[k] == 0; };
    // Byte-sorted simple paths make every subtree a contiguous range whose
    // members extend the root's full name with '.' or '/' (the two smallest
    // chars that can follow, given the name-char restriction checked above),
    // so the end is found by binary search instead of a linear has_prefix
    // scan per node.
    getNextSubtree = [&fulls](uint32_t sidx, uint32_t eidx) -> uint32_t {
      if (sidx >= eidx) return eidx;
      const std::string& p = fulls[sidx];
      if (p.size() == 1) return eidx;  // root: everything below is in-tree
      uint32_t lo = sidx + 1, hi = eidx;
      while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const std::string& c = fulls[mid];
        const bool in_subtree =
            c.size() > p.size() &&
            memcmp(c.data(), p.data(), p.size()) == 0 &&
            (c[p.size()] == '/' || c[p.size()] == '.');
        if (in_subtree) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      return lo;
    };
    isDirectChildOf = [&fulls, &parent_len](uint32_t c, uint32_t p) {
      return parent_len[c] == fulls[p].size() &&
             memcmp(fulls[c].data(), fulls[p].data(), parent_len[c]) == 0;
    };
    haveSameParent = [&fulls, &parent_len](uint32_t a, uint32_t b) {
      return parent_len[a] == parent_len[b] &&
             memcmp(fulls[a].data(), fulls[b].data(), parent_len[a]) == 0;
    };
  } else {
    isRootNode = [this, &ids](uint32_t k) {
      return paths_[ids[k]].is_root_path();
    };
    getNextSubtree = [this, &ids](uint32_t sidx, uint32_t eidx) -> uint32_t {
      if (sidx >= eidx) return eidx;
      for (uint32_t i = sidx; i < eidx; i++) {
        if (!paths_[ids[i]].has_prefix(paths_[ids[sidx]])) return i;
      }
      return eidx;
    };
    isDirectChildOf = [&fulls, &parent_fulls](uint32_t c, uint32_t p) {
      return parent_fulls[c] == fulls[p];
    };
    haveSameParent = [&parent_fulls](uint32_t a, uint32_t b) {
      return parent_fulls[a] == parent_fulls[b];
    };
  }

  // Stack-overflow backstop for the recursive builder. ConvertPrimIterative is
  // the authoritative depth gate (rejects prim nesting > kMaxPrimNestingDepth
  // with a clear message before this pass runs); allow a small margin here for
  // the extra path level that prim-property / root paths add, so this backstop
  // is never the first thing a too-deep stage hits.
  constexpr uint32_t kMaxPathTreeDepth = kMaxPrimNestingDepth + 8;

  std::function<bool(uint32_t&, uint32_t, uint32_t, uint32_t, uint32_t&)>
  buildPathTree = [&](uint32_t& currentIdx, uint32_t startIdx, uint32_t endIdx,
                      uint32_t depth, uint32_t& nextIdxOut) -> bool {
    if (depth > kMaxPathTreeDepth) {
      if (err) *err = "Path tree too deep (>" + std::to_string(kMaxPathTreeDepth) + " levels)";
      return false;
    }
    if (currentIdx >= num_encoded_paths || startIdx > endIdx) return false;

    for (uint32_t pIdx = startIdx, nextIdx = pIdx; pIdx < endIdx; pIdx = nextIdx) {
      uint32_t nextSubtreeIdx = getNextSubtree(pIdx, endIdx);
      nextIdx = pIdx + 1;

      bool has_child = false;
      bool has_sibling = false;

      if (nextIdx != nextSubtreeIdx && nextIdx < num_encoded_paths) {
        if (isRootNode(pIdx)) {
          has_child = true;
        } else if (isDirectChildOf(nextIdx, pIdx)) {
          has_child = true;
        }
      }

      if (nextSubtreeIdx != endIdx && nextSubtreeIdx < num_encoded_paths) {
        if (!isRootNode(pIdx) && haveSameParent(nextSubtreeIdx, pIdx)) {
          has_sibling = true;
        }
      }

      uint32_t thisIdx = currentIdx++;
      encoded_path_indices[thisIdx] = ids[pIdx];
      if (elem_tokens_ok) {
        element_token_indices[thisIdx] = node_elem_tokens[pIdx];
      } else {
        const Path& p = paths_[ids[pIdx]];
        const bool is_prop = p.is_prim_property_path();
        std::string elem = is_prop ? p.prop_part() : p.element_name();
        if (elem == "/") elem.clear();
        // A variant selection is its OWN path element on the crate wire:
        // strip `Prim{set=sel}` to the bare `{set=sel}` group or OpenUSD's
        // reader rejects the file ("Invalid prim name"). Keep in lockstep
        // with the pre-registration loop and the pass-A precompute.
        if (!is_prop && !elem.empty() && elem.back() == '}') {
          size_t open = elem.find_last_of('{');
          if (open != std::string::npos) {
            elem = elem.substr(open);
          }
        }
        element_token_indices[thisIdx] =
            static_cast<int32_t>(GetOrCreateToken(elem).value);
        if (is_prop) {
          element_token_indices[thisIdx] = -element_token_indices[thisIdx];
        }
      }

      if (has_child) {
        uint32_t childNextOut = 0;
        if (!buildPathTree(currentIdx, nextIdx, endIdx, depth + 1, childNextOut))
          return false;
        nextIdx = childNextOut;
      }

      if (has_sibling && has_child) {
        jump_indices[thisIdx] = static_cast<int32_t>(currentIdx - thisIdx);
      } else if (has_sibling) {
        jump_indices[thisIdx] = 0;
      } else if (has_child) {
        jump_indices[thisIdx] = -1;
      } else {
        jump_indices[thisIdx] = -2;
      }

      if (!has_sibling) {
        nextIdxOut = nextIdx;
        return true;
      }
    }

    nextIdxOut = endIdx;
    return true;
  };

  {
    uint32_t currentIdx = 0;
    uint32_t nextIdx = 0;
    if (!buildPathTree(currentIdx, 0, static_cast<uint32_t>(num_encoded_paths), 0, nextIdx)) {
      if (err) *err = "Failed to build path indices from sorted paths";
      return false;
    }
  }

  // Verify all indices were filled
  for (size_t i = 0; i < encoded_path_indices.size(); i++) {
    if (encoded_path_indices[i] == crate::PathIndex().value) {
      // Dump sorted paths for debugging
      std::string dbg = "path index " + std::to_string(i) + " not filled. Sorted paths:\n";
      for (size_t j = 0; j < num_encoded_paths; j++) {
        dbg += "  [" + std::to_string(j) + "] " + fulls[j]
             + " idx=" + std::to_string(ids[j])
             + (j == i ? " <-- UNFILLED" : "") + "\n";
      }
      if (err) *err = "Internal error: " + dbg;
      return false;
    }
  }

  // Write PATHS section:
  // 1. uint64_t numPaths (total paths — reader allocates _paths of this size)
  uint64_t num_paths = static_cast<uint64_t>(paths_.size());
  if (!Write(num_paths)) {
    if (err) *err = "Failed to write numPaths";
    return false;
  }

  // 2. uint64_t numEncodedPaths (may be <= numPaths; excludes empty/inactive)
  uint64_t num_enc = static_cast<uint64_t>(num_encoded_paths);
  if (!Write(num_enc)) {
    if (err) *err = "Failed to write numEncodedPaths";
    return false;
  }

  // 3-5. Compressed pathIndexes / elementTokenIndexes / jumps. The three
  // compressions are independent pure computations — run them concurrently,
  // then write the (order-sensitive) size+bytes pairs sequentially.
  {
    struct PathsComp {
      const char* name;
      // Exactly one is set: the int32 and uint32 CompressToBuffer overloads
      // encode DIFFERENTLY, so the original per-array overload must be kept.
      const uint32_t* u32 = nullptr;
      const int32_t* i32 = nullptr;
      std::vector<char> comp;
      size_t csz = 0;
      std::string cerr;
    };
    PathsComp comps[3];
    comps[0].name = "pathIndexes";
    comps[0].u32 = encoded_path_indices.data();
    comps[1].name = "elementTokenIndexes";
    comps[1].i32 = element_token_indices.data();
    comps[2].name = "jumps";
    comps[2].i32 = jump_indices.data();
    auto compress_one = [&](PathsComp& c) {
      const size_t buf_size =
          Usd_IntegerCompression::GetCompressedBufferSize(num_encoded_paths);
      c.comp.resize(buf_size);
      c.csz = c.u32 ? Usd_IntegerCompression::CompressToBuffer(
                          c.u32, num_encoded_paths, c.comp.data(), &c.cerr)
                    : Usd_IntegerCompression::CompressToBuffer(
                          c.i32, num_encoded_paths, c.comp.data(), &c.cerr);
    };
#if defined(LIGHTUSD_ENABLE_THREAD)
    {
      std::thread t1([&]() { compress_one(comps[1]); });
      std::thread t2([&]() { compress_one(comps[2]); });
      compress_one(comps[0]);
      t1.join();
      t2.join();
    }
#else
    for (auto& c : comps) {
      compress_one(c);
    }
#endif
    for (auto& c : comps) {
      if (c.csz == 0) {
        if (err) *err = std::string("Compress ") + c.name + " failed: " + c.cerr;
        return false;
      }
      const uint64_t sz = static_cast<uint64_t>(c.csz);
      if (!Write(sz) || !WriteBytes(c.comp.data(), c.csz)) {
        if (err) *err = std::string("Failed to write ") + c.name;
        return false;
      }
    }
  }

  int64_t section_end = Tell();

  crate::Section section(kPathsSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WriteSpecsSection(std::string* err) {
  int64_t section_start = Tell();

  // IMPORTANT: Format for v0.4.0+ (we target v0.7.0+):
  // 1. uint64_t numSpecs
  // 2. pathIndexes (compressed with Usd_IntegerCompression):
  //    - uint64_t pathIndexesSize
  //    - Compressed pathIndexes array
  // 3. fieldSetIndexes (compressed with Usd_IntegerCompression):
  //    - uint64_t fieldSetIndexesSize
  //    - Compressed fieldSetIndexes array
  // 4. specTypes (compressed with Usd_IntegerCompression):
  //    - uint64_t specTypesSize
  //    - Compressed specTypes array
  //
  // See: pxr/usd/sdf/crateFile.cpp _WriteSpecs()

  size_t num_specs = spec_data_.size();

  // Write spec count
  uint64_t spec_count = static_cast<uint64_t>(num_specs);
  if (!Write(spec_count)) {
    if (err) *err = "Failed to write spec count";
    return false;
  }

  // IMPORTANT: Build mapping from fieldset number to offset in flat array
  // The reader expects fieldset_index to be the OFFSET in the flat fieldset array,
  // not the fieldset number. We need to convert from fieldset_number -> offset.
  std::vector<uint32_t> fieldset_number_to_offset;
  fieldset_number_to_offset.resize(fieldsets_.size());

  uint32_t current_offset = 0;
  for (size_t i = 0; i < fieldsets_.size(); ++i) {
    fieldset_number_to_offset[i] = current_offset;
    // Each fieldset takes (num_fields + 1) slots (fields + sentinel)
    const uint32_t slot_size = static_cast<uint32_t>(fieldsets_[i].size() + 1);
      if (current_offset > (std::numeric_limits<uint32_t>::max)() - slot_size) {
      if (err) *err = "Fieldset offset overflow (>4B entries).";
      return false;
    }
    current_offset += slot_size;
  }

  // Separate pathIndexes, fieldSetIndexes, specTypes
  std::vector<uint32_t> path_indexes;
  std::vector<uint32_t> fieldset_indexes;
  std::vector<uint32_t> spec_types;

  path_indexes.reserve(num_specs);
  fieldset_indexes.reserve(num_specs);
  spec_types.reserve(num_specs);

  for (size_t i = 0; i < spec_data_.size(); ++i) {
    const auto& spec_data = spec_data_[i];
    path_indexes.push_back(spec_data.spec.path_index.value);

    // Convert fieldset number to offset in flat array
    uint32_t fieldset_number = spec_data.spec.fieldset_index.value;
    uint32_t fieldset_offset = fieldset_number_to_offset[fieldset_number];
    fieldset_indexes.push_back(fieldset_offset);

    spec_types.push_back(static_cast<uint32_t>(spec_data.spec.spec_type));
  }

  // The three column compressions are independent pure computations — run
  // them concurrently, then write the (order-sensitive) size+bytes pairs
  // sequentially.
  struct SpecsComp {
    const char* name;
    const std::vector<uint32_t>* data;
    std::vector<char> comp;
    size_t csz = 0;
    std::string cerr;
  };
  SpecsComp comps[3] = {
      {"pathIndexes", &path_indexes, {}, 0, {}},
      {"fieldSetIndexes", &fieldset_indexes, {}, 0, {}},
      {"specTypes", &spec_types, {}, 0, {}},
  };
  auto compress_one = [](SpecsComp& c) {
    const size_t buffer_size =
        Usd_IntegerCompression::GetCompressedBufferSize(c.data->size());
    c.comp.resize(buffer_size);
    c.csz = Usd_IntegerCompression::CompressToBuffer(
        c.data->data(), c.data->size(), c.comp.data(), &c.cerr);
  };
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    std::thread t1([&]() { compress_one(comps[1]); });
    std::thread t2([&]() { compress_one(comps[2]); });
    compress_one(comps[0]);
    t1.join();
    t2.join();
  }
#else
  for (auto& c : comps) {
    compress_one(c);
  }
#endif
  for (auto& c : comps) {
    if (c.csz == 0) {
      if (err) *err = std::string("Failed to compress ") + c.name + ": " + c.cerr;
      return false;
    }
    const uint64_t size = static_cast<uint64_t>(c.csz);
    if (!Write(size)) {
      if (err) *err = std::string("Failed to write ") + c.name + " size";
      return false;
    }
    if (!WriteBytes(c.comp.data(), c.csz)) {
      if (err) *err = std::string("Failed to write compressed ") + c.name;
      return false;
    }
  }

  int64_t section_end = Tell();

  crate::Section section(kSpecsSection, section_start, section_end - section_start);
  toc_.sections.push_back(section);

  return true;
}

bool CrateWriter::WriteTableOfContents(std::string* err) {
  int64_t toc_offset = Tell();

  // Write section count
  uint64_t section_count = static_cast<uint64_t>(toc_.sections.size());
  if (!Write(section_count)) {
    if (err) *err = "Failed to write section count";
    return false;
  }

  // Write sections
  for (const auto& section : toc_.sections) {
    // Write section name (null-terminated, max 15 chars)
    char name_buf[crate::kSectionNameMaxLength + 1] = {0};
    strncpy(name_buf, section.name, crate::kSectionNameMaxLength);
    if (!WriteBytes(name_buf, sizeof(name_buf))) {
      if (err) *err = "Failed to write section name";
      return false;
    }

    // Write section start and size
    if (!Write(section.start)) {
      if (err) *err = "Failed to write section start";
      return false;
    }
    if (!Write(section.size)) {
      if (err) *err = "Failed to write section size";
      return false;
    }
  }

  // Store TOC offset for bootstrap
  // We need to save this before writing bootstrap
  int64_t saved_toc_offset = toc_offset;


  // IMPORTANT: Flush before seeking to beginning
  // We need to flush all buffered writes before seeking backwards
  stream_->Flush();

  // Seek to beginning to write bootstrap (no need to close/reopen)
  if (!Seek(0)) {
    if (err) *err = "Failed to seek to beginning for bootstrap write";
    return false;
  }

  // Build bootstrap header
  BootStrap boot;
  memset(&boot, 0, sizeof(boot));

  memcpy(boot.ident, kMagicIdent, 8);
  // Emit max(configured version, version required by written values). The
  // latter is raised via RequestCrateVersionUpgrade() when e.g. an
  // SdfPathExpression (>=0.10.0) or TsSpline (>=0.12.0) value is written, so
  // the header truthfully declares the minimum reader version.
  uint8_t ev_major = options_.version_major;
  uint8_t ev_minor = options_.version_minor;
  uint8_t ev_patch = options_.version_patch;
  const bool req_higher =
      (required_version_major_ > ev_major) ||
      (required_version_major_ == ev_major &&
       required_version_minor_ > ev_minor) ||
      (required_version_major_ == ev_major &&
       required_version_minor_ == ev_minor &&
       required_version_patch_ > ev_patch);
  if (req_higher) {
    ev_major = required_version_major_;
    ev_minor = required_version_minor_;
    ev_patch = required_version_patch_;
  }
  boot.version[0] = ev_major;
  boot.version[1] = ev_minor;
  boot.version[2] = ev_patch;
  boot.toc_offset = saved_toc_offset;

  // Write bootstrap header
  if (!WriteBytes(&boot, sizeof(boot))) {
    if (err) *err = "Failed to write bootstrap";
    return false;
  }

  stream_->Flush();

  return true;
}

bool CrateWriter::WriteBootStrap(std::string* /* err */) {
  // Bootstrap is already written in WriteTableOfContents
  // This is just a placeholder for consistency
  return true;
}

// ============================================================================
// Value Encoding
// ============================================================================

crate::ValueRep CrateWriter::PackValue(const crate::CrateValue& value, std::string* err) {
  ProfScope prof_total(profile_finalize_, &prof_pack_total_ns_);
  crate::ValueRep rep;

  // VtArrayEdit (crate >= 0.14.0): a ValueRep with the IsArrayEdit bit set, the
  // array element type in the type byte, and a payload referencing the
  // (valuesRep, indexesRep, isDense) tuple (payload 0 == identity edit).
  if (auto* ae = value.as<value::ArrayEdit>()) {
    RequestCrateVersionUpgrade(0, 14, 0);  // VtArrayEdit requires crate 0.14.0
    crate::ValueRep aerep;
    aerep.SetIsArrayEdit();
    if (ae->ops.empty()) {
      // Identity edit: no out-of-line data. The element type comes from the
      // model (no literals are packed to derive it from).
      aerep.SetType(ae->element_type_id);
      aerep.SetPayload(0);
      return crate::ValueRep(aerep.GetData());
    }
    last_array_edit_elem_type_ = 0;
    bool is_compressed = false;
    int64_t offset = WriteValueData(value, &is_compressed, err);
    if (offset < 0 || (err && !err->empty())) {
      return crate::ValueRep();
    }
    // Adopt the element type from the packed literals array (robust even for
    // ref-only edits with empty literals); fall back to the model's id.
    aerep.SetType(last_array_edit_elem_type_ ? last_array_edit_elem_type_
                                             : ae->element_type_id);
    aerep.SetPayload(static_cast<uint64_t>(offset));
    return crate::ValueRep(aerep.GetData());
  }

  if (value.as<Reference>()) {
    if (err) {
      *err = "Standalone Reference values are not representable in Crate; use ReferenceListOp.";
    }
    return crate::ValueRep(rep.GetData());
  }

  // Try to inline the value
  {
    ProfScope prof_inline(profile_finalize_, &prof_pack_inline_ns_);
    if (!value.IsUnregisteredValue() && TryInlineValue(value, &rep)) {
      return crate::ValueRep(rep.GetData());
    }
  }

  bool dedup_candidate = false;
  std::vector<char> dedup_bytes;
  size_t dedup_element_size = 1;
  bool dedup_is_float = false;
  uint32_t dedup_wire_tag = 0;
  size_t dedup_hash = 0;

  {
    ProfScope prof_dedup(profile_finalize_, &prof_pack_dedup_ns_);
    if (options_.enable_deduplication &&
        ComputeValueDedupDescriptor(value, &dedup_bytes, &dedup_element_size,
                                    &dedup_is_float, &dedup_wire_tag)) {
      dedup_hash = NanAwareHash::combine(
          NanAwareHash::hash_buffer(dedup_bytes.data(), dedup_bytes.size(),
                                    dedup_element_size, dedup_is_float),
          dedup_wire_tag);
      if (LookupDeduplicatedValue(dedup_bytes, dedup_element_size,
                                  dedup_is_float, dedup_wire_tag, &rep)) {
        return crate::ValueRep(rep.GetData());
      }
      dedup_candidate = true;
    }
  }

  // Value cannot be inlined, write to value data section
  bool is_compressed = false;
  int64_t offset;
  {
    ProfScope prof_write(profile_finalize_, &prof_pack_write_ns_);
    offset = WriteValueData(value, &is_compressed, err);
  }
  if (offset < 0 || (err && !err->empty())) {
    return crate::ValueRep();
  }

  // Create ValueRep with offset and proper type
  SetOutOfLineRepType(value, &rep);

  rep.SetPayload(static_cast<uint64_t>(offset));
  if (is_compressed) {
    rep.SetIsCompressed();
  }

  if (dedup_candidate) {
    RetainDeduplicatedValue(dedup_hash, std::move(dedup_bytes),
                            dedup_element_size, dedup_is_float,
                            dedup_wire_tag, rep);
  }

  return rep;
}

// Determine the ValueRep type/flags for out-of-line values (shared by
// PackValue and the two-pass BuildPackPlan; see the header note about the
// version-bumping branches).
void CrateWriter::SetOutOfLineRepType(const crate::CrateValue& value,
                                      crate::ValueRep* rep_out) {
  crate::ValueRep& rep = *rep_out;

  // Macro to reduce repetitive scalar type dispatch
#define PACK_SCALAR_TYPE(CppType, CrateTypeId) \
  if (value.as<CppType>()) { \
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CrateTypeId)); \
  } else

  // Macro to reduce repetitive array type dispatch
#define PACK_ARRAY_TYPE(ElemType, CrateTypeId) \
  if (value.as<std::vector<ElemType>>()) { \
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CrateTypeId)); \
    rep.SetIsArray(); \
  } else

  // Scalar types
  if (value.IsUnregisteredValue()) {
    rep.SetType(static_cast<int32_t>(
        crate::CrateDataTypeId::CRATE_DATA_TYPE_UNREGISTERED_VALUE));
  } else
  PACK_SCALAR_TYPE(double, CRATE_DATA_TYPE_DOUBLE)
  PACK_SCALAR_TYPE(value::timecode, CRATE_DATA_TYPE_TIME_CODE)
  PACK_SCALAR_TYPE(int64_t, CRATE_DATA_TYPE_INT64)
  PACK_SCALAR_TYPE(uint64_t, CRATE_DATA_TYPE_UINT64)
  PACK_SCALAR_TYPE(value::float2, CRATE_DATA_TYPE_VEC2F)
  PACK_SCALAR_TYPE(value::double2, CRATE_DATA_TYPE_VEC2D)
  PACK_SCALAR_TYPE(value::int2, CRATE_DATA_TYPE_VEC2I)
  PACK_SCALAR_TYPE(value::float3, CRATE_DATA_TYPE_VEC3F)
  PACK_SCALAR_TYPE(value::double3, CRATE_DATA_TYPE_VEC3D)
  PACK_SCALAR_TYPE(value::int3, CRATE_DATA_TYPE_VEC3I)
  PACK_SCALAR_TYPE(value::half2, CRATE_DATA_TYPE_VEC2H)
  PACK_SCALAR_TYPE(value::half3, CRATE_DATA_TYPE_VEC3H)
  PACK_SCALAR_TYPE(value::half4, CRATE_DATA_TYPE_VEC4H)
  PACK_SCALAR_TYPE(value::float4, CRATE_DATA_TYPE_VEC4F)
  PACK_SCALAR_TYPE(value::double4, CRATE_DATA_TYPE_VEC4D)
  PACK_SCALAR_TYPE(value::int4, CRATE_DATA_TYPE_VEC4I)
  PACK_SCALAR_TYPE(value::matrix2d, CRATE_DATA_TYPE_MATRIX2D)
  PACK_SCALAR_TYPE(value::matrix3d, CRATE_DATA_TYPE_MATRIX3D)
  PACK_SCALAR_TYPE(value::matrix4d, CRATE_DATA_TYPE_MATRIX4D)
  PACK_SCALAR_TYPE(value::quath, CRATE_DATA_TYPE_QUATH)
  PACK_SCALAR_TYPE(value::quatf, CRATE_DATA_TYPE_QUATF)
  PACK_SCALAR_TYPE(value::quatd, CRATE_DATA_TYPE_QUATD)
  // Array types - element type ID + IsArray flag (bit 63)
  PACK_ARRAY_TYPE(bool, CRATE_DATA_TYPE_BOOL)
  PACK_ARRAY_TYPE(uint8_t, CRATE_DATA_TYPE_UCHAR)
  PACK_ARRAY_TYPE(int32_t, CRATE_DATA_TYPE_INT)
  PACK_ARRAY_TYPE(uint32_t, CRATE_DATA_TYPE_UINT)
  PACK_ARRAY_TYPE(int64_t, CRATE_DATA_TYPE_INT64)
  PACK_ARRAY_TYPE(uint64_t, CRATE_DATA_TYPE_UINT64)
  PACK_ARRAY_TYPE(value::half, CRATE_DATA_TYPE_HALF)
  PACK_ARRAY_TYPE(float, CRATE_DATA_TYPE_FLOAT)
  PACK_ARRAY_TYPE(double, CRATE_DATA_TYPE_DOUBLE)
  PACK_ARRAY_TYPE(value::timecode, CRATE_DATA_TYPE_TIME_CODE)
  PACK_ARRAY_TYPE(value::float2, CRATE_DATA_TYPE_VEC2F)
  PACK_ARRAY_TYPE(value::float3, CRATE_DATA_TYPE_VEC3F)
  PACK_ARRAY_TYPE(value::float4, CRATE_DATA_TYPE_VEC4F)
  PACK_ARRAY_TYPE(value::half2, CRATE_DATA_TYPE_VEC2H)
  PACK_ARRAY_TYPE(value::half3, CRATE_DATA_TYPE_VEC3H)
  PACK_ARRAY_TYPE(value::half4, CRATE_DATA_TYPE_VEC4H)
  PACK_ARRAY_TYPE(value::double2, CRATE_DATA_TYPE_VEC2D)
  PACK_ARRAY_TYPE(value::double3, CRATE_DATA_TYPE_VEC3D)
  PACK_ARRAY_TYPE(value::double4, CRATE_DATA_TYPE_VEC4D)
  PACK_ARRAY_TYPE(value::int2, CRATE_DATA_TYPE_VEC2I)
  PACK_ARRAY_TYPE(value::int3, CRATE_DATA_TYPE_VEC3I)
  PACK_ARRAY_TYPE(value::int4, CRATE_DATA_TYPE_VEC4I)
  PACK_ARRAY_TYPE(value::matrix2d, CRATE_DATA_TYPE_MATRIX2D)
  PACK_ARRAY_TYPE(value::matrix3d, CRATE_DATA_TYPE_MATRIX3D)
  PACK_ARRAY_TYPE(value::matrix4d, CRATE_DATA_TYPE_MATRIX4D)
  PACK_ARRAY_TYPE(value::quath, CRATE_DATA_TYPE_QUATH)
  PACK_ARRAY_TYPE(value::quatf, CRATE_DATA_TYPE_QUATF)
  PACK_ARRAY_TYPE(value::quatd, CRATE_DATA_TYPE_QUATD)
  PACK_ARRAY_TYPE(value::AssetPath, CRATE_DATA_TYPE_ASSET_PATH)
  PACK_ARRAY_TYPE(value::PathExpression, CRATE_DATA_TYPE_PATH_EXPRESSION)
  PACK_ARRAY_TYPE(std::string, CRATE_DATA_TYPE_STRING)
  PACK_ARRAY_TYPE(value::token, CRATE_DATA_TYPE_TOKEN)

#undef PACK_SCALAR_TYPE
#undef PACK_ARRAY_TYPE

  // PathVector is a special type (type code 40) that doesn't use the array flag
  if (value.as<std::vector<Path>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PATH_VECTOR));
  }
  // SdfRelocates (Crate type 58): a list of (source, target) path pairs. Not an
  // array type (the vector itself is the value). crate >= 0.11.0.
  else if (value.as<std::vector<std::pair<Path, Path>>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_RELOCATES));
    RequestCrateVersionUpgrade(0, 11, 0);  // SdfRelocates requires crate 0.11.0
  }
  // Dictionary type
  else if (value.as<value::dict>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_DICTIONARY));
  }
  // Phase 2: CustomDataType (serializes like dictionary)
  else if (value.as<CustomDataType>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_DICTIONARY));
  }
  // Phase 2: ListOp types
  else if (value.as<ListOp<value::token>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_TOKEN_LIST_OP));
  } else if (value.as<ListOp<std::string>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_STRING_LIST_OP));
  } else if (value.as<ListOp<Path>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PATH_LIST_OP));
  } else if (value.as<ListOp<int32_t>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INT_LIST_OP));
  } else if (value.as<ListOp<uint32_t>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT_LIST_OP));
  } else if (value.as<ListOp<int64_t>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INT64_LIST_OP));
  } else if (value.as<ListOp<uint64_t>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_UINT64_LIST_OP));
  }
  // Phase 2: Payload and list-op types. Crate has no standalone Reference
  // type; references are represented by ReferenceListOp.
  else if (value.as<Payload>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PAYLOAD));
  } else if (value.as<ListOp<Reference>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_REFERENCE_LIST_OP));
  } else if (value.as<ListOp<Payload>>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PAYLOAD_LIST_OP));
  }
  // Phase 2: VariantSelectionMap
  else if (value.as<VariantSelectionMap>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_VARIANT_SELECTION_MAP));
  }
  // Phase 3: TimeSamples
  else if (value.as<value::TimeSamples>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_TIME_SAMPLES));
  }
  // Phase 3b: Spline (TsSpline, Crate type 59)
  else if (auto* spline_data = value.as<primvar::PrimVar::SplineData>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_SPLINE));
    // A spline with tangent algorithms (binary version 2) requires crate
    // 0.13.0; a plain spline (version 1) only requires 0.12.0.
    if (SplineBinaryFormatVersion(*spline_data) >= 2) {
      RequestCrateVersionUpgrade(0, 13, 0);
    } else {
      RequestCrateVersionUpgrade(0, 12, 0);
    }
  }
  // Phase 3c: scalar SdfPathExpression (Crate type 57). Non-inlined: stored as
  // a StringIndex at an offset (OpenUSD cannot decode an inlined PathExpression).
  else if (value.as<value::PathExpression>()) {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_PATH_EXPRESSION));
    RequestCrateVersionUpgrade(0, 10, 0);  // SdfPathExpression requires crate 0.10.0
  }
  // Unknown/unsupported type
  else {
    rep.SetType(static_cast<int32_t>(crate::CrateDataTypeId::CRATE_DATA_TYPE_INVALID));
  }
}

void CrateWriter::BuildPackPlan(const crate::CrateValue& value, PackPlan* plan,
                                PackPlanContext* ctx) {
  plan->kind = PackPlan::kSerial;
  // Mirror PackValue's pre-inline special cases: standalone Reference (error
  // path), VtArrayEdit (version bump + last_array_edit_elem_type_) and
  // unregistered values (recursive wrapper) all stay serial.
  if (value.as<Reference>() || value.as<value::ArrayEdit>() ||
      value.IsUnregisteredValue()) {
    return;
  }

  // Inline classification: same decision procedure as the serial writer, but
  // interning is only OBSERVED, never performed.
  struct RecordingSink final : public InternSink {
    bool interned = false;
    uint32_t InternToken(const std::string&) override {
      interned = true;
      return 0;
    }
    uint32_t InternString(const std::string&) override {
      interned = true;
      return 0;
    }
  } sink;
  crate::ValueRep inline_rep;
  if (TryInlineValue(value, &inline_rep, sink)) {
    if (sink.interned) {
      // Payload is an intern index: pass B re-runs TryInlineValue with the
      // direct sink (first-branch hit + one intern — the mandatory serial
      // part), avoiding any captured-string lifetime concerns.
      plan->kind = PackPlan::kInlineIntern;
    } else {
      plan->kind = PackPlan::kInlinePure;
      plan->rep = inline_rep;
    }
    return;
  }

  if (!IsPureValueData(value)) {
    return;  // index-embedding / seeking encodings stay serial
  }

  // Dedup descriptor + hash first (parallel; pass B's lookup consumes them).
  if (options_.enable_deduplication &&
      ComputeValueDedupDescriptor(value, &plan->dedup_bytes,
                                  &plan->dedup_element_size,
                                  &plan->dedup_is_float,
                                  &plan->dedup_wire_tag)) {
    plan->dedup_hash = NanAwareHash::combine(
        NanAwareHash::hash_buffer(plan->dedup_bytes.data(),
                                  plan->dedup_bytes.size(),
                                  plan->dedup_element_size,
                                  plan->dedup_is_float),
        plan->dedup_wire_tag);
    plan->dedup_candidate = true;
  }

  // Prebuild only values that will actually need encoding. Instanced scenes
  // are dominated by dedup HITS, whose serial path never encodes at all —
  // blindly prebuilding every duplicate costs more than the parallelism buys
  // (measured on a 4317-mesh scene with 177 unique assets). Skip when the
  // value is already retained (frozen-table probe) and claim within-window
  // firsts so each new unique value is encoded by exactly one thread; the
  // other duplicates dedup-hit in pass B once the first is retained. A plan
  // that skips prebuild keeps `bytes` empty — pass B falls back to the serial
  // WriteValueData on a genuine miss, so hash collisions and dedup-budget
  // refusals stay correct.
  bool prebuild = true;
  if (plan->dedup_candidate) {
    crate::ValueRep probe;
    if (LookupDeduplicatedValueWithHash(
            plan->dedup_hash, plan->dedup_bytes, plan->dedup_element_size,
            plan->dedup_is_float, plan->dedup_wire_tag, &probe)) {
      // Certain, FINAL hit: the table only grows (one entry per unique
      // descriptor, added on miss, never mutated), so pass B would resolve to
      // this same rep — resolve it here and skip the serial lookup entirely.
      plan->rep = probe;
      plan->kind = PackPlan::kOolDedupHit;
      return;
    } else if (ctx) {
      std::lock_guard<std::mutex> lock(ctx->mu);
      prebuild = ctx->claimed.insert(plan->dedup_hash).second;
    }
  }

  if (prebuild) {
    // Prebuild the out-of-line byte image with the REAL encoder, captured
    // into the plan buffer (see tls_value_capture).
    plan->bytes.clear();
    bool is_compressed = false;
    std::string local_err;
    tls_value_capture() = &plan->bytes;
    const int64_t body_result =
        WriteValueBody(value, &is_compressed, &local_err);
    tls_value_capture() = nullptr;
    if (body_result < 0) {
      // Shouldn't happen for pure values (capture writes cannot fail); let
      // the serial pass reproduce whatever the error is.
      plan->bytes.clear();
      plan->kind = PackPlan::kSerial;
      return;
    }
    plan->is_compressed = is_compressed;
  }

  SetOutOfLineRepType(value, &plan->rep);
  plan->kind = PackPlan::kOolPure;
}

crate::ValueRep CrateWriter::PackValueFromPlan(const crate::CrateValue& value,
                                               PackPlan& plan,
                                               std::string* err) {
  switch (plan.kind) {
    case PackPlan::kInlinePure:
    case PackPlan::kOolDedupHit:
      return plan.rep;
    case PackPlan::kInlineIntern: {
      crate::ValueRep rep;
      if (TryInlineValue(value, &rep)) {
        return rep;
      }
      return PackValue(value, err);  // defensive; classification is deterministic
    }
    case PackPlan::kOolPure: {
      crate::ValueRep rep = plan.rep;  // type + array flag preset
      if (plan.dedup_candidate &&
          LookupDeduplicatedValueWithHash(
              plan.dedup_hash, plan.dedup_bytes, plan.dedup_element_size,
              plan.dedup_is_float, plan.dedup_wire_tag, &rep)) {
        return rep;
      }
      int64_t value_offset;
      bool is_compressed = plan.is_compressed;
      if (plan.bytes.empty()) {
        // Prebuild was skipped (pass A expected a dedup hit that didn't
        // materialize — hash collision or dedup-budget refusal): encode
        // serially, exactly as PackValue would.
        value_offset = WriteValueData(value, &is_compressed, err);
        if (value_offset < 0 || (err && !err->empty())) {
          return crate::ValueRep();
        }
      } else {
        // Append the prebuilt bytes exactly where WriteValueData would have
        // written them (same prologue/epilogue bookkeeping).
        const int64_t current_pos = Tell();
        if (!Seek(value_data_end_offset_)) {
          if (err) *err = "Failed to seek to value data section";
          return crate::ValueRep();
        }
        value_offset = Tell();
        if (!WriteBytes(plan.bytes.data(), plan.bytes.size())) {
          if (err) *err = "Failed to write prebuilt value data";
          return crate::ValueRep();
        }
        value_data_end_offset_ = Tell();
        if (!Seek(current_pos)) {
          if (err) *err = "Failed to seek back after writing value";
          return crate::ValueRep();
        }
      }
      rep.SetPayload(static_cast<uint64_t>(value_offset));
      if (is_compressed) {
        rep.SetIsCompressed();
      }
      if (plan.dedup_candidate) {
        RetainDeduplicatedValue(plan.dedup_hash, std::move(plan.dedup_bytes),
                                plan.dedup_element_size, plan.dedup_is_float,
                                plan.dedup_wire_tag, rep);
      }
      return rep;
    }
    case PackPlan::kSerial:
      return PackValue(value, err);
  }
  return PackValue(value, err);
}


// ============================================================================
// Deduplication
// ============================================================================

crate::TokenIndex CrateWriter::GetOrCreateToken(const std::string& token) {
  return GetOrCreateImpl<std::string, crate::TokenIndex>(token, token_to_index_, tokens_);
}

crate::StringIndex CrateWriter::GetOrCreateString(const std::string& str) {
  auto it = string_to_index_.find(str);
  if (it != string_to_index_.end()) {
    return it->second;
  }

  // Strings map to tokens, so ensure the token exists
  GetOrCreateToken(str);

  // Create new string
  crate::StringIndex idx(static_cast<uint32_t>(strings_.size()));
  strings_.push_back(str);
  string_to_index_[str] = idx;
  return idx;
}

int64_t CrateWriter::FindPathSlot(const Path& path, uint32_t hash) const {
  if (path_slots_.empty()) return -1;
  const size_t mask = path_slots_.size() - 1;
  const crate::PathKeyEqual eq;
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    const auto& slot = path_slots_[i];
    if (slot.second == 0) return -1;
    if (slot.first == hash && eq(paths_[slot.second - 1], path)) {
      return int64_t(slot.second - 1);
    }
  }
}

void CrateWriter::InsertPathSlot(uint32_t hash, uint32_t path_index) {
  if ((path_slots_used_ + 1) * 2 >= path_slots_.size()) {
    GrowPathSlots(path_slots_.size() * 2 + 16);
  }
  const size_t mask = path_slots_.size() - 1;
  size_t i = hash & mask;
  while (path_slots_[i].second != 0) i = (i + 1) & mask;
  path_slots_[i] = {hash, path_index + 1};
  path_slots_used_++;
}

void CrateWriter::GrowPathSlots(size_t want) {
  size_t cap = 32;
  while (cap < want * 2) cap <<= 1;
  std::vector<std::pair<uint32_t, uint32_t>> old = std::move(path_slots_);
  path_slots_.assign(cap, {0u, 0u});
  const size_t mask = cap - 1;
  for (const auto& slot : old) {
    if (slot.second == 0) continue;
    size_t i = slot.first & mask;
    while (path_slots_[i].second != 0) i = (i + 1) & mask;
    path_slots_[i] = slot;
  }
}

crate::PathIndex CrateWriter::GetOrCreatePath(const Path& path) {
  const uint32_t hash = static_cast<uint32_t>(crate::PathHasher()(path));
  {
    const int64_t found = FindPathSlot(path, hash);
    if (found >= 0) {
      return crate::PathIndex(static_cast<uint32_t>(found));
    }
  }

  // IMPORTANT: Ensure all parent paths exist first
  // This is necessary for building a valid path tree where intermediate nodes
  // must have valid path indices. For example, adding "/Material/bora" should
  // also add "/Material" and "/" if they don't exist.
  std::string prim_part = path.prim_part();
  std::string prop_part = path.prop_part();

  // If this is a property path, first ensure the prim path (without property) exists
  if (!prop_part.empty() && !prim_part.empty()) {
    Path prim_only_path(prim_part, "");
    GetOrCreatePath(prim_only_path);
  }
  // Then ensure all parent prim paths exist
  else if (!prim_part.empty() && prim_part != "/") {
    // Build parent path by removing the last element
    size_t last_slash = prim_part.find_last_of('/');
    if (last_slash != std::string::npos && last_slash > 0) {
      std::string parent_prim = prim_part.substr(0, last_slash);
      Path parent_path(parent_prim, "");
      // Recursively ensure parent exists
      GetOrCreatePath(parent_path);
    } else if (last_slash == 0) {
      // Parent is root "/"
      Path root_path("/", "");
      GetOrCreatePath(root_path);
    }
  }

  // Create new path
  crate::PathIndex idx(static_cast<uint32_t>(paths_.size()));
  paths_.push_back(path);
  InsertPathSlot(hash, idx.value);

  // Also register path tokens
  if (!path.prim_part().empty()) {
    // Split prim part into elements and register each
    std::string prim = path.prim_part();
    size_t pos = 0;
    while (pos < prim.size()) {
      size_t next = prim.find('/', pos + 1);
      if (next == std::string::npos) {
        next = prim.size();
      }
      std::string element = prim.substr(pos + 1, next - pos - 1);
      if (!element.empty()) {
        GetOrCreateToken(element);
      }
      pos = next;
    }
  }

  if (!path.prop_part().empty()) {
    GetOrCreateToken(path.prop_part());
  }

  return idx;
}

crate::FieldIndex CrateWriter::GetOrCreateField(const crate::Field& field) {
  return GetOrCreateImpl<crate::Field, crate::FieldIndex>(field, field_to_index_, fields_);
}

crate::FieldSetIndex CrateWriter::GetOrCreateFieldSet(const std::vector<crate::FieldIndex>& fieldset) {
  return GetOrCreateImpl<std::vector<crate::FieldIndex>, crate::FieldSetIndex>(fieldset, fieldset_to_index_, fieldsets_);
}

// ============================================================================
// Compressed Array Helpers
// ============================================================================

int64_t CrateWriter::WriteIndexArray32(
    const uint32_t* data, uint64_t count, const char* typeName, std::string* err) {
  for (uint64_t i = 0; i < count; ++i) {
    if (!Write(data[i])) {
      if (err) { *err = "Failed to write "; *err += typeName; *err += " array element"; }
      return -1;
    }
  }
  return 0;
}

int64_t CrateWriter::WriteCompressedArray32(
    const uint32_t* data, uint64_t count,
    const char* typeName, bool* is_compressed, std::string* err) {
  if (is_compressed) {
    (*is_compressed) = false;
  }

  if (count >= 16 && options_.enable_compression) {
    size_t compBufSize = Usd_IntegerCompression::GetCompressedBufferSize(count);
    std::vector<char> compressed(compBufSize);
    std::string compress_err;
    size_t compSize = Usd_IntegerCompression::CompressToBuffer(
        data, count, compressed.data(), &compress_err);
    if (compSize != 0 && compSize != static_cast<size_t>(~0)) {
      uint64_t cs = static_cast<uint64_t>(compSize);
      if (!Write(cs)) { if (err) { *err = "Failed to write compressed "; *err += typeName; *err += " array size"; } return -1; }
      if (!WriteBytes(compressed.data(), compSize)) { if (err) { *err = "Failed to write compressed "; *err += typeName; *err += " array data"; } return -1; }
      if (is_compressed) {
        (*is_compressed) = true;
      }
      return 0;
    }
  }
  // Fallback: write uncompressed
  for (uint64_t i = 0; i < count; ++i) {
    if (!Write(data[i])) { if (err) { *err = "Failed to write "; *err += typeName; *err += " array element"; } return -1; }
  }
  return 0;
}

int64_t CrateWriter::WriteCompressedArray64(
    const uint64_t* data, uint64_t count,
    const char* typeName, bool* is_compressed, std::string* err) {
  if (is_compressed) {
    (*is_compressed) = false;
  }

  if (count >= 16 && options_.enable_compression) {
    size_t compBufSize = Usd_IntegerCompression64::GetCompressedBufferSize(count);
    std::vector<char> compressed(compBufSize);
    std::string compress_err;
    size_t compSize = Usd_IntegerCompression64::CompressToBuffer(
        data, count, compressed.data(), &compress_err);
    if (compSize != 0 && compSize != static_cast<size_t>(~0)) {
      uint64_t cs = static_cast<uint64_t>(compSize);
      if (!Write(cs)) { if (err) { *err = "Failed to write compressed "; *err += typeName; *err += " array size"; } return -1; }
      if (!WriteBytes(compressed.data(), compSize)) { if (err) { *err = "Failed to write compressed "; *err += typeName; *err += " array data"; } return -1; }
      if (is_compressed) {
        (*is_compressed) = true;
      }
      return 0;
    }
  }
  // Fallback: write uncompressed
  for (uint64_t i = 0; i < count; ++i) {
    if (!Write(data[i])) { if (err) { *err = "Failed to write "; *err += typeName; *err += " array element"; } return -1; }
  }
  return 0;
}

namespace {
// Compress `count` uint32 values into `out` (just the compressed bytes; the
// caller writes the uint64 size prefix). Matches the payload the reader's
// ReadCompressedInts consumes after the size prefix. Returns false on failure.
static bool CompressUInt32ToBuffer(const uint32_t* data, uint64_t count,
                                   std::vector<char>* out) {
  const size_t bufSize =
      Usd_IntegerCompression::GetCompressedBufferSize(static_cast<size_t>(count));
  out->resize(bufSize);
  std::string cerr;
  const size_t n = Usd_IntegerCompression::CompressToBuffer(
      data, static_cast<size_t>(count), out->data(), &cerr);
  if (n == 0 || n == static_cast<size_t>(~0)) {
    return false;
  }
  out->resize(n);
  return true;
}
}  // namespace

int64_t CrateWriter::WriteCompressedFloatArray(const float* data, uint64_t count,
                                               bool* is_compressed,
                                               std::string* err) {
  if (is_compressed) {
    (*is_compressed) = false;
  }

  // Opt-in (default off): tagged float-array compression. Requires both the
  // dedicated flag and the general compression flag (the latter gates the
  // integer-stream compressor the 'i'/'t' payloads use).
  if (options_.enable_float_array_compression && options_.enable_compression &&
      count >= crate::kMinCompressedArraySize) {
    // Code 'i': every value is an integer exactly representable as int32 (the
    // reader reconstructs via float(int32)). Negative zero is deliberately not
    // eligible because conversion through int32 would erase its authored sign.
    // The range guard before the cast avoids UB for NaN/Inf/out-of-range
    // values. 2^31 is exactly representable in float; use it as the exclusive
    // upper bound (anything >= it cannot be a valid int32).
    {
      constexpr float kInt32Lo = -2147483648.0f;     // -2^31 == INT32_MIN
      constexpr float kInt32HiExcl = 2147483648.0f;  // 2^31, one past INT32_MAX
      std::vector<int32_t> ints(static_cast<size_t>(count));
      bool all_int = true;
      for (uint64_t i = 0; i < count; ++i) {
        const float v = data[i];
        if (v == 0.0f && std::signbit(v)) { all_int = false; break; }
        if (!(v >= kInt32Lo && v < kInt32HiExcl)) { all_int = false; break; }
        const int32_t iv = static_cast<int32_t>(v);
        if (!math::is_close(static_cast<float>(iv), v, 0.0f)) { all_int = false; break; }
        ints[static_cast<size_t>(i)] = iv;
      }
      std::vector<char> comp;
      if (all_int &&
          CompressUInt32ToBuffer(reinterpret_cast<const uint32_t*>(ints.data()),
                                 count, &comp)) {
        const char code = 'i';
        if (!WriteBytes(&code, 1)) { if (err) *err = "Failed to write float 'i' code"; return -1; }
        if (!Write(static_cast<uint64_t>(comp.size()))) { if (err) *err = "Failed to write float compressed-int size"; return -1; }
        if (!comp.empty() && !WriteBytes(comp.data(), comp.size())) { if (err) *err = "Failed to write float compressed ints"; return -1; }
        if (is_compressed) { (*is_compressed) = true; }
        return 0;
      }
    }
    // Code 't': few distinct values -> lookup table + compressed indices. Keyed
    // on the raw bit pattern so -0.0/NaN/Inf round-trip exactly here.
    {
      std::unordered_map<uint32_t, uint32_t> seen;
      std::vector<float> lut;
      std::vector<uint32_t> indexes(static_cast<size_t>(count));
      // Give up once the LUT would exceed min(count/4, 1024) distinct values —
      // the same profitability bound and 1024 ceiling OpenUSD uses. (We key on
      // the raw bit pattern rather than operator==, so -0.0/NaN round-trip
      // exactly instead of merging/exploding the table.)
      const size_t max_lut = (std::min)(static_cast<size_t>(count / 4),
                                        static_cast<size_t>(1024));
      bool lut_ok = true;
      for (uint64_t i = 0; i < count; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &data[i], sizeof(bits));
        auto it = seen.find(bits);
        if (it != seen.end()) {
          indexes[static_cast<size_t>(i)] = it->second;
          continue;
        }
        if (lut.size() == max_lut) { lut_ok = false; break; }
        const uint32_t idx = static_cast<uint32_t>(lut.size());
        seen.emplace(bits, idx);
        lut.push_back(data[i]);
        indexes[static_cast<size_t>(i)] = idx;
      }
      std::vector<char> comp;
      if (lut_ok && !lut.empty() &&
          CompressUInt32ToBuffer(indexes.data(), count, &comp)) {
        const char code = 't';
        size_t lut_bytes;
        if (!safe::mul(lut.size(), sizeof(float), &lut_bytes)) { if (err) *err = "Overflow: float LUT bytes"; return -1; }
        if (!WriteBytes(&code, 1)) { if (err) *err = "Failed to write float 't' code"; return -1; }
        if (!Write(static_cast<uint32_t>(lut.size()))) { if (err) *err = "Failed to write float LUT size"; return -1; }
        if (!WriteBytes(lut.data(), lut_bytes)) { if (err) *err = "Failed to write float LUT"; return -1; }
        if (!Write(static_cast<uint64_t>(comp.size()))) { if (err) *err = "Failed to write float index size"; return -1; }
        if (!comp.empty() && !WriteBytes(comp.data(), comp.size())) { if (err) *err = "Failed to write float indices"; return -1; }
        if (is_compressed) { (*is_compressed) = true; }
        return 0;
      }
    }
  }

  // Uncompressed fallback: raw little-endian floats (is_compressed stays false).
  size_t byte_count;
  if (!safe::mul(static_cast<size_t>(count), sizeof(float), &byte_count)) {
    if (err) *err = "Integer overflow: count * sizeof(float)";
    return -1;
  }
  if (count > 0 && !WriteBytes(data, byte_count)) {
    if (err) *err = "Failed to write float array data";
    return -1;
  }
  return 0;
}

int64_t CrateWriter::WriteCompressedDoubleArray(const double* data, uint64_t count,
                                                bool* is_compressed,
                                                std::string* err) {
  if (is_compressed) {
    (*is_compressed) = false;
  }

  // Opt-in (default off); see WriteCompressedFloatArray for the gating rationale.
  if (options_.enable_float_array_compression && options_.enable_compression &&
      count >= crate::kMinCompressedArraySize) {
    // Code 'i': integers exactly representable as int32 (reader reconstructs via
    // double(int32)). int32 is always exact in double. Negative zero is not
    // eligible because conversion through int32 would erase its authored sign.
    // The range guard avoids UB for NaN/Inf/out-of-range values.
    {
      constexpr double kInt32Lo = -2147483648.0;     // -2^31 == INT32_MIN
      constexpr double kInt32HiExcl = 2147483648.0;  // 2^31, one past INT32_MAX
      std::vector<int32_t> ints(static_cast<size_t>(count));
      bool all_int = true;
      for (uint64_t i = 0; i < count; ++i) {
        const double v = data[i];
        if (v == 0.0 && std::signbit(v)) { all_int = false; break; }
        if (!(v >= kInt32Lo && v < kInt32HiExcl)) { all_int = false; break; }
        const int32_t iv = static_cast<int32_t>(v);
        if (!math::is_close(static_cast<double>(iv), v, 0.0)) { all_int = false; break; }
        ints[static_cast<size_t>(i)] = iv;
      }
      std::vector<char> comp;
      if (all_int &&
          CompressUInt32ToBuffer(reinterpret_cast<const uint32_t*>(ints.data()),
                                 count, &comp)) {
        const char code = 'i';
        if (!WriteBytes(&code, 1)) { if (err) *err = "Failed to write double 'i' code"; return -1; }
        if (!Write(static_cast<uint64_t>(comp.size()))) { if (err) *err = "Failed to write double compressed-int size"; return -1; }
        if (!comp.empty() && !WriteBytes(comp.data(), comp.size())) { if (err) *err = "Failed to write double compressed ints"; return -1; }
        if (is_compressed) { (*is_compressed) = true; }
        return 0;
      }
    }
    // Code 't': lookup table + compressed indices. Keyed on the raw 64-bit
    // pattern so -0.0/NaN/Inf round-trip exactly.
    {
      std::unordered_map<uint64_t, uint32_t> seen;
      std::vector<double> lut;
      std::vector<uint32_t> indexes(static_cast<size_t>(count));
      // Same min(count/4, 1024) profitability bound as OpenUSD; keyed on the
      // raw 64-bit pattern so -0.0/NaN round-trip exactly.
      const size_t max_lut = (std::min)(static_cast<size_t>(count / 4),
                                        static_cast<size_t>(1024));
      bool lut_ok = true;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t bits;
        std::memcpy(&bits, &data[i], sizeof(bits));
        auto it = seen.find(bits);
        if (it != seen.end()) {
          indexes[static_cast<size_t>(i)] = it->second;
          continue;
        }
        if (lut.size() == max_lut) { lut_ok = false; break; }
        const uint32_t idx = static_cast<uint32_t>(lut.size());
        seen.emplace(bits, idx);
        lut.push_back(data[i]);
        indexes[static_cast<size_t>(i)] = idx;
      }
      std::vector<char> comp;
      if (lut_ok && !lut.empty() &&
          CompressUInt32ToBuffer(indexes.data(), count, &comp)) {
        const char code = 't';
        size_t lut_bytes;
        if (!safe::mul(lut.size(), sizeof(double), &lut_bytes)) { if (err) *err = "Overflow: double LUT bytes"; return -1; }
        if (!WriteBytes(&code, 1)) { if (err) *err = "Failed to write double 't' code"; return -1; }
        if (!Write(static_cast<uint32_t>(lut.size()))) { if (err) *err = "Failed to write double LUT size"; return -1; }
        if (!WriteBytes(lut.data(), lut_bytes)) { if (err) *err = "Failed to write double LUT"; return -1; }
        if (!Write(static_cast<uint64_t>(comp.size()))) { if (err) *err = "Failed to write double index size"; return -1; }
        if (!comp.empty() && !WriteBytes(comp.data(), comp.size())) { if (err) *err = "Failed to write double indices"; return -1; }
        if (is_compressed) { (*is_compressed) = true; }
        return 0;
      }
    }
  }

  // Uncompressed fallback: raw little-endian doubles (is_compressed stays false).
  size_t byte_count;
  if (!safe::mul(static_cast<size_t>(count), sizeof(double), &byte_count)) {
    if (err) *err = "Integer overflow: count * sizeof(double)";
    return -1;
  }
  if (count > 0 && !WriteBytes(data, byte_count)) {
    if (err) *err = "Failed to write double array data";
    return -1;
  }
  return 0;
}

// ============================================================================
// I/O Utilities
// ============================================================================

int64_t CrateWriter::Tell() {
  return stream_->Tell();
}

bool CrateWriter::Seek(int64_t pos) {
  return stream_->Seek(pos);
}

std::vector<char>*& CrateWriter::tls_value_capture() {
  static thread_local std::vector<char>* sink = nullptr;
  return sink;
}

bool CrateWriter::WriteBytes(const void* data, size_t size) {
  // Value-encoding capture (two-pass Finalize pass A): append to the
  // per-thread buffer instead of the stream. No file-size accounting here —
  // the serial replay appends these bytes through the normal path below,
  // which enforces the limit.
  if (std::vector<char>* capture = tls_value_capture()) {
    const char* p = static_cast<const char*>(data);
    capture->insert(capture->end(), p, p + size);
    return true;
  }

  // Check file size limit before writing
  if (WouldExceedFileSizeLimit(static_cast<int64_t>(size))) {
    std::cerr << "ERROR: Writing " << size << " bytes would exceed file size limit of "
              << options_.max_file_size_bytes / (1024*1024) << " MB\n"
              << "  Current file size: " << bytes_written_ << " bytes\n"
              << "  Limit: " << options_.max_file_size_bytes << " bytes\n";
    return false;
  }

  if (stream_->Write(data, size)) {
    bytes_written_ += static_cast<int64_t>(size);
    return true;
  }
  return false;
}

// ============================================================================
// Validation Methods (Phase 5)
// ============================================================================

bool CrateWriter::ValidateStage(const Stage& stage, std::string* err) {
  // Reset validation state
  validation_prim_count_ = 0;
  validation_property_count_ = 0;
  validation_warnings_.clear();
  validation_warnings_count_ = 0;

  if (!options_.enable_validation) {
    return true;  // Validation disabled
  }

  // Check for empty stage
  if (stage.root_prims().empty()) {
    std::string warning = "WARNING: Stage has no root prims";
    if (err) *err = warning;
    validation_warnings_.push_back(warning);
    validation_warnings_count_++;
  }

  // Build a set of root prim names for defaultPrim validation
  std::set<std::string> root_prim_names;

  // Validate each root prim
  for (const auto& prim : stage.root_prims()) {
    validation_prim_count_++;

    // Check prim name is not empty
    if (prim.element_name().empty()) {
      std::string warning = "WARNING: Prim has empty name at index " +
                           std::to_string(validation_prim_count_);
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }

    // Collect prim name for defaultPrim validation
    if (!prim.element_name().empty()) {
      root_prim_names.insert(prim.element_name());
    }

    // Check for invalid path characters in prim name (/, ., and other invalid chars)
    const std::string& prim_name = prim.element_name();
    for (char c : prim_name) {
      if (c == '/' || c == '.' || c == ':' || c == '[' || c == ']' || c == '(' || c == ')') {
        std::string warning = "WARNING: Prim name contains invalid character '" +
                             std::string(1, c) + "' in name: " + prim_name;
        validation_warnings_.push_back(warning);
        validation_warnings_count_++;
        break;  // Only report once per prim
      }
    }
  }

  // Validate stage metadata
  const StageMetas& metas = stage.metas();

  // Validate defaultPrim if specified
  if (!metas.defaultPrim.str().empty()) {
    const std::string& default_prim_name = metas.defaultPrim.str();
    if (root_prim_names.find(default_prim_name) == root_prim_names.end()) {
      std::string warning = "WARNING: defaultPrim '" + default_prim_name +
                           "' does not refer to any root prim";
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }
  }

  // Validate time metadata consistency
  if (metas.startTimeCode.authored() && metas.endTimeCode.authored()) {
    double start = metas.startTimeCode.get_value();
    double end = metas.endTimeCode.get_value();
    if (start > end) {
      std::string warning = "WARNING: startTimeCode (" + std::to_string(start) +
                           ") is greater than endTimeCode (" + std::to_string(end) + ")";
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }
  }

  // Validate framesPerSecond and timeCodesPerSecond are positive
  if (metas.framesPerSecond.authored()) {
    double fps = metas.framesPerSecond.get_value();
    if (fps <= 0.0) {
      std::string warning = "WARNING: framesPerSecond must be positive, got " +
                           std::to_string(fps);
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }
  }

  if (metas.timeCodesPerSecond.authored()) {
    double tcps = metas.timeCodesPerSecond.get_value();
    if (tcps <= 0.0) {
      std::string warning = "WARNING: timeCodesPerSecond must be positive, got " +
                           std::to_string(tcps);
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }
  }

  // Validate metersPerUnit is positive if specified
  if (metas.metersPerUnit.authored()) {
    double mpu = metas.metersPerUnit.get_value();
    if (mpu <= 0.0) {
      std::string warning = "WARNING: metersPerUnit must be positive, got " +
                           std::to_string(mpu);
      validation_warnings_.push_back(warning);
      validation_warnings_count_++;
    }
  }

  return validation_warnings_count_ == 0 || !options_.enable_validation;
}

bool CrateWriter::ValidateLayer(const Layer& layer, std::string* err) {
  // Reset validation state
  validation_prim_count_ = 0;
  validation_property_count_ = 0;
  validation_warnings_.clear();
  validation_warnings_count_ = 0;

  if (!options_.enable_validation) {
    return true;  // Validation disabled
  }


  // Note: Layer is forward declared in crate-writer.hh, so we do minimal validation
  // Actual validation should be done before passing to CrateWriter

  std::string msg = "Layer validation: structure check (detailed validation requires full Layer definition)";

  if (err) *err = msg;
  return true;
}

std::string CrateWriter::GetValidationSummary() const {
  std::string summary = "Validation Summary:\n";
  summary += "  Prims: " + std::to_string(validation_prim_count_) + "\n";
  summary += "  Properties: " + std::to_string(validation_property_count_) + "\n";
  summary += "  Warnings: " + std::to_string(validation_warnings_count_) + "\n";

  if (!validation_warnings_.empty()) {
    summary += "\nWarnings:\n";
    for (size_t i = 0; i < validation_warnings_.size() && i < 10; ++i) {
      summary += "  [" + std::to_string(i + 1) + "] " + validation_warnings_[i] + "\n";
    }
    if (validation_warnings_.size() > 10) {
      summary += "  ... and " + std::to_string(validation_warnings_.size() - 10) + " more warnings\n";
    }
  }

  return summary;
}

} // namespace experimental
} // namespace lightusd

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
