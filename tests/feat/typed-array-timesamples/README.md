# TypedArray TimeSamples Feature Test

This test verifies the implementation of `TypedArray<T>` support for TimeSamples.

## Feature Overview

The TypedArray-based TimeSamples implementation provides:

1. **Memory-efficient storage**: Uses flat byte buffer (`_data`) for all binary-serializable types
2. **Backwards compatibility**: Both `std::vector<T>` and `TypedArray<T>` overloads work transparently
3. **Flat binary-storage path**: All binary types (scalars and arrays) share a single `_data` buffer

## Storage Architecture (as of 2026-03-17)

TimeSamples uses two storage backends:

### Backend 1: Flat Binary Storage
For trivially-copyable POD types (int, float, half, matrix, etc.):
- `_data: vector<uint8_t>` — flat byte buffer for ALL binary values
- `_data_offsets: vector<uint32_t>` — per-sample byte offset into `_data`
- `_array_counts: vector<uint32_t>` — per-sample element count (arrays only)

### Backend 2: Generic Value Storage
For non-binary types (string, token, path, bool, etc.):
- `_samples: vector<Sample>` — `{double t, Value value, bool blocked}`

### Auto-detection
`add_sample<T>()` / `add_array_sample<T>()` auto-detect the backend on first call.
No `init()` needed. Use `set_type_id()` for metadata-only cases (all-blocked TimeSamples).

## Supported Types

### Binary-Serializable Types (flat buffer storage)
- Scalars: `int32_t`, `uint32_t`, `int64_t`, `uint64_t`, `half`, `float`, `double`
- Vectors: `half2/3/4`, `float2/3/4`, `double2/3/4`, `int2/3/4`
- Quaternions: `quath`, `quatf`, `quatd`
- Matrices: `matrix2f/d`, `matrix3f/d`, `matrix4f/d`
- Role types: `color3f/d`, `point3f/d`, `normal3f/d`, `vector3f/d`, `texcoord2f/d`, `texcoord3f/d`

### Generic Value Types (Sample-based storage)
- `token[]`, `string[]`, `path[]`, `bool[]`, `AssetPath[]`

## Building

### Using Make (Standalone)

```bash
cd tests/feat/typed-array-timesamples
make
```

### Running the Test

```bash
make test
```

### Cleaning

```bash
make clean
```

## Test Coverage

The test program verifies:

1. **TypedArray storage**: Adding `TypedArray<T>` at multiple time samples
2. **std::vector compatibility**: Ensuring existing vector-based API still works
3. **Scalar values**: Testing binary-serializable scalar value storage
4. **Multiple types**: Testing int32, uint32, int64, uint64, float, and double

## Implementation Details

### Key Files

- `src/timesamples.hh`: TimeSamples class with flat binary storage
- `src/timesamples.cc`: Implementation (sorting, reconstruction, copy/move)
- `src/crate-reader-timesamples.cc`: Crate format TimeSamples reader
- `src/ascii-parser-timesamples.cc`: ASCII format scalar TimeSamples parser
- `src/ascii-parser-timesamples-array.cc`: ASCII format array TimeSamples parser

### Deduplication

In-TimeSamples deduplication has been removed (2026-03-17). The crate reader
already deduplicates at the `ValueRep` level. The ASCII parser's O(n^2) comparison
was expensive and rarely triggered. Memory impact of storing full data is negligible
for typical files.

## See Also

- `src/typed-array.hh` - TypedArray implementation
- `src/timesamples.hh` - TimeSamples flat binary-storage path
- `src/crate-reader-timesamples.cc` - Crate format TimeSamples reader
- Full refactoring history: Git history of `doc/archive/refactor-opportunities.md` (removed; the refactor is complete)
