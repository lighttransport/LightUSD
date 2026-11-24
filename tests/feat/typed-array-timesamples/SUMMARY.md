# TypedArray TimeSamples Implementation Summary

## Overview

`TypedArray<T>` support for TimeSamples array values. Both `std::vector<T>` and
`TypedArray<T>` overloads write to the same flat binary byte buffer.

## Current Architecture (as of 2026-03-17)

### Storage

All binary-serializable types share a single flat buffer:

```
_times:        vector<double>    — timestamps
_blocked:      Buffer<16>        — per-sample blocked flag (1 byte each)
_data:         vector<uint8_t>   — flat byte buffer for ALL binary values
_data_offsets: vector<uint32_t>  — per-sample byte offset into _data
_array_counts: vector<uint32_t>  — per-sample element count (arrays only)
_element_size: uint32_t          — sizeof(T) for elements
_type_id:      uint32_t
_is_array:     bool
_dirty:        bool
```

Non-binary types (token, string, path, etc.) use `_samples: vector<Sample>`.

### Auto-initialization

`add_sample<T>()` / `add_array_sample<T>()` auto-detect the backend on first call:
- `uses_binary_timesample_scalar_storage_v<T>` → flat binary buffer
- everything else → generic `Value` backend

No `init()` needed. Use `set_type_id(uint32_t)` for metadata-only cases.

### API

```cpp
// Adding samples
template<T> bool add_sample(double t, const T& value, string* err)
template<T> bool add_array_sample(double t, const vector<T>& arr, string* err)
template<T> bool add_array_sample(double t, const TypedArray<T>& arr, string* err)
template<T> bool add_blocked_sample(double t, string* err)
bool add_sample(double t, const Value& v, string* err)   // generic fallback

// Reading samples
const vector<Sample>& get_samples() const               // lazy reconstruct
template<T> bool get(T* dst, double t, InterpolationType) const
template<T> TypedArrayView<const T> get_typed_array_view_at(size_t idx) const
template<T> bool get_vector_at(size_t idx, vector<T>* out) const
```

## Changes Made

### Phase 1: Eliminated init()/ensure flow (2026-03-17)

- `init()` replaced by `set_type_id()` for metadata-only use
- `add_sample<T>()` auto-detects backend on first call
- Removed early `init()` from ASCII parser that caused token[] data-loss bug
- Collapsed 4 `ensure_*` functions into single `validate_type_or_init()`

### Phase 2: Unified binary storage to flat byte buffer (2026-03-17)

Replaced 4 old binary storage mechanisms:
- `_small_values` (uint64 array for sizeof(T) ≤ 8)
- `_values` (Buffer for sizeof(T) > 8)
- `_offsets` (uint64 with dedup/array/buffer flags)
- `_array_values` (vector of unique_ptr<Buffer>)

With 2 new fields:
- `_data` (flat uint8 byte buffer)
- `_data_offsets` (uint32 per-sample byte offset)

Deleted: `StorageDescriptor`, `ScalarStorageDescriptor`, `ArrayStorageDescriptor`,
`UnifiedStorageBackend` enum, `ArrayLayoutKind` enum, offset flag constants.

### Phase 3: Removed in-TimeSamples dedup (2026-03-17)

- Removed `arrays_equal()` and O(n^2) dedup lambda from ASCII parser
- Removed `get_timesamples_dedup_map()` global tracker from crate reader
- Crate reader already deduplicates at ValueRep level

### Phase 4: Cleaned up public API (2026-03-17)

Removed: `get_values()`, `get_offsets()`, `get_small_values()`, `get_array_size()`,
`is_stl_array()`, `is_typed_array()`, `add_value_array_sample()`, `resolve_offset_static()`.

Added: `get_data()`, `get_data_offsets()`, `element_size()`, `BLOCKED_OFFSET`.

## Files Modified

1. `src/timesamples.hh` - Storage redesign, API cleanup
2. `src/timesamples.cc` - Sorting, reconstruction, copy/move, clear
3. `src/ascii-parser-timesamples-array.cc` - Removed early init and dedup
4. `src/ascii-parser-timesamples.cc` - init() → set_type_id()
5. `src/crate-reader-timesamples.cc` - Removed dedup map, init() → set_type_id()
6. `src/usdc-reader.cc` - init() → set_type_id()
7. `src/timesamples-pprint.cc` - Updated for new storage API
8. `src/usd-dump.cc` - Updated for new storage API
9. `tests/unit/unit-timesamples.cc` - Updated for removed methods

## Build Verification

```bash
cd build && make -j16        # zero warnings with -Werror
ctest --output-on-failure    # all 5 tests pass
bash tests/run-usdcat-compare.sh  # 0 USDA failures, 41 pre-existing USDC failures
./build/lusdcat tests/usda/timesamples-array-token-001.usda  # token[] preserved
```

Net result: −1544 lines across 9 files.

## Remaining Feature TODOs

1. **Deferred TimeSamples loading** — OpenUSD reads sample values lazily
2. **SharedTimes** — OpenUSD shares times arrays across TimeSamples in crate files
3. **Type validation for times array** — Validate that times ValueRep is `double[]`

## References

- TypedArray implementation: `src/typed-array.hh`
- TimeSamples storage: `src/timesamples.hh`
- Crate format reader: `src/crate-reader-timesamples.cc`
- Refactoring history: Git history of `doc/archive/refactor-opportunities.md` (removed; the refactor is complete)
