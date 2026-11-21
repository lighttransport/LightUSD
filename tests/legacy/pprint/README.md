# Pretty-Print Benchmark

Performance benchmark for LightUSD pretty-printing functionality. Measures the performance of different pprint implementations:

- **String-based**: Traditional `std::string` concatenation with `std::stringstream`
- **StreamWriter**: Optimized single-buffer writer
- **ChunkedStreamWriter**: Memory-efficient chunked buffer writer
- **Parallel ChunkedStreamWriter**: Multi-threaded printing with zero-copy concatenation

## Building

### Using CMake (Recommended)

```bash
# From the repository root
cd build
cmake ..
make pprint_benchmark
```

### Using Makefile (Standalone)

```bash
# From this directory
cd tests/pprint

# Make sure lightusd is built first
cd ../../build && make && cd -

# Build benchmark
make
```

## Usage

### Basic Usage

```bash
./pprint_benchmark
```

### Using Presets

```bash
# Small benchmark (~10 MB output)
./pprint_benchmark --preset small

# Medium benchmark (~100 MB output)
./pprint_benchmark --preset medium

# Large benchmark (~1 GB output)
./pprint_benchmark --preset large

# Huge benchmark (~10 GB output - requires significant RAM)
./pprint_benchmark --preset huge
```

### Custom Configuration

```bash
# Custom parameters
./pprint_benchmark \
  --num-prims 500 \
  --array-min 1000 \
  --array-max 10000 \
  --num-times 20 \
  --ts-array-size 5000 \
  --parallel \
  --verbose
```

### Command-Line Options

- `--preset <name>` - Use preset configuration (small/medium/large/huge)
- `--num-prims <n>` - Number of Prims to generate (default: 100)
- `--array-min <n>` - Minimum array size (default: 100)
- `--array-max <n>` - Maximum array size (default: 1000)
- `--num-times <n>` - Number of timesample times (default: 10)
- `--ts-array-size <n>` - Timesample array size (default: 100)
- `--parallel` - Enable parallel printing benchmark
- `--verbose` - Verbose output during data generation
- `--help` - Show help message

## Preset Details

| Preset | Est. Output Size | Prims | Array Range | TS Times | TS Array Size |
|--------|------------------|-------|-------------|----------|---------------|
| small  | ~10 MB          | 50    | 1K - 5K     | 5        | 500           |
| medium | ~100 MB         | 200   | 5K - 20K    | 10       | 2K            |
| large  | ~1 GB           | 500   | 20K - 50K   | 20       | 5K            |
| huge   | ~10 GB          | 2000  | 50K - 100K  | 50       | 10K           |

## Example Output

```
========================================
LightUSD Pretty-Print Benchmark
========================================

Benchmark Configuration:
  Number of Prims:          200
  Array size range:         5000 - 20000
  Timesample times:         10
  Timesample array size:    2000
  Use parallel printing:    yes
  Estimated data size:      95 MB

Generating test data...
Test data generated: 200 prims

Running benchmarks...

Method                              Time    Output Size     Throughput
------------------------------------------------------------------------
String-based pprint              1234.56 ms           98 MB       79.41 MB/s
StreamWriter pprint               987.65 ms           98 MB       99.24 MB/s
ChunkedStreamWriter pprint        876.54 ms           98 MB      111.85 MB/s
Parallel ChunkedStreamWriter      234.12 ms           98 MB      418.58 MB/s
------------------------------------------------------------------------

Speedup vs. String-based:
  StreamWriter pprint                1.25x
  ChunkedStreamWriter pprint         1.41x
  Parallel ChunkedStreamWriter       5.27x

Benchmark completed.
```

## Performance Tips

1. **Parallel Printing**: Use `--parallel` flag for benchmarks with many Prims (>= 4)
2. **Threading**: Make sure LightUSD is compiled with `LIGHTUSD_ENABLE_THREAD=ON`
3. **Memory**: Ensure sufficient RAM for large/huge presets
4. **Reproducibility**: The benchmark uses a fixed random seed (42) for consistent results

## What's Being Measured

The benchmark generates synthetic USD data with:

- **Prims**: GeomMesh primitives with realistic attributes
- **Arrays**: Points, face indices, and other geometry data
- **Timesamples**: Animated attributes with multiple time samples
- **Output**: Complete USDA-formatted text output

Each method processes the same data and produces identical output, allowing for direct performance comparison.

## Interpreting Results

- **Time**: Total wall-clock time to generate the pretty-printed output
- **Output Size**: Size of the generated USDA text in bytes
- **Throughput**: Processing speed in MB/s (output size / time)
- **Speedup**: Performance improvement relative to baseline String-based method

The **ChunkedStreamWriter** methods should show:
- Lower memory usage (chunked allocation vs. large contiguous strings)
- Better performance for large outputs (reduced reallocation overhead)
- Significant speedup with parallel printing (4-8x on multi-core systems)
