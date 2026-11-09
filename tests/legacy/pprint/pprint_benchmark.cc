// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment Inc.
//
// Pretty-print performance benchmark
//
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <iomanip>

#include "core/prim.hh"
#include "prim-pprint.hh"
#include "prim-pprint-parallel.hh"
#include "stream-writer.hh"
#include "timesamples-pprint.hh"
#include "lightusd.hh"
#include "usdGeom.hh"  // Geom* (no longer re-exported by lightusd.hh)
#include "value-types.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// ============================================================================
// Configuration
// ============================================================================

struct BenchmarkConfig {
  size_t num_prims = 100;
  size_t array_min_size = 100;
  size_t array_max_size = 1000;
  size_t num_timesample_times = 10;
  size_t timesample_array_size = 100;
  bool use_parallel = false;
  bool verbose = false;

  // Estimated total size in bytes
  size_t estimate_size() const {
    // Rough estimate: each prim with arrays and timesamples
    size_t prim_size = (array_min_size + array_max_size) / 2 * sizeof(float) * 2; // points + normals
    prim_size += num_timesample_times * timesample_array_size * sizeof(float);
    return num_prims * prim_size;
  }

  void print() const {
    std::cout << "Benchmark Configuration:\n";
    std::cout << "  Number of Prims:          " << num_prims << "\n";
    std::cout << "  Array size range:         " << array_min_size << " - " << array_max_size << "\n";
    std::cout << "  Timesample times:         " << num_timesample_times << "\n";
    std::cout << "  Timesample array size:    " << timesample_array_size << "\n";
    std::cout << "  Use parallel printing:    " << (use_parallel ? "yes" : "no") << "\n";
    std::cout << "  Estimated data size:      " << (estimate_size() / (1024 * 1024)) << " MB\n";
    std::cout << "\n";
  }
};

// Preset configurations
BenchmarkConfig get_preset(const std::string& preset) {
  BenchmarkConfig config;

  if (preset == "small") {
    // ~10 MB
    config.num_prims = 50;
    config.array_min_size = 1000;
    config.array_max_size = 5000;
    config.num_timesample_times = 5;
    config.timesample_array_size = 500;
  } else if (preset == "medium") {
    // ~100 MB
    config.num_prims = 200;
    config.array_min_size = 5000;
    config.array_max_size = 20000;
    config.num_timesample_times = 10;
    config.timesample_array_size = 2000;
  } else if (preset == "large") {
    // ~1 GB
    config.num_prims = 500;
    config.array_min_size = 20000;
    config.array_max_size = 50000;
    config.num_timesample_times = 20;
    config.timesample_array_size = 5000;
  } else if (preset == "huge") {
    // ~10 GB
    config.num_prims = 2000;
    config.array_min_size = 50000;
    config.array_max_size = 100000;
    config.num_timesample_times = 50;
    config.timesample_array_size = 10000;
  } else {
    std::cerr << "Unknown preset: " << preset << "\n";
    std::cerr << "Available presets: small, medium, large, huge\n";
    exit(1);
  }

  return config;
}

// ============================================================================
// Data Generation
// ============================================================================

// Generate random float array
std::vector<float> generate_float_array(size_t size, float min_val = -100.0f, float max_val = 100.0f) {
  std::vector<float> arr(size);
  for (size_t i = 0; i < size; i++) {
    float t = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    arr[i] = min_val + t * (max_val - min_val);
  }
  return arr;
}

// Generate random int array
std::vector<int> generate_int_array(size_t size, int min_val = 0, int max_val = 10000) {
  std::vector<int> arr(size);
  for (size_t i = 0; i < size; i++) {
    arr[i] = min_val + (rand() % (max_val - min_val + 1));
  }
  return arr;
}

// Generate synthetic GeomMesh with arrays
GeomMesh generate_mesh(const std::string& name, size_t points_size, size_t indices_size) {
  GeomMesh mesh;
  mesh.name = name;

  // Generate points
  std::vector<lightusd::legacy::value::point3f> points;
  points.reserve(points_size);
  for (size_t i = 0; i < points_size; i++) {
    float x = static_cast<float>(rand() % 1000) / 10.0f;
    float y = static_cast<float>(rand() % 1000) / 10.0f;
    float z = static_cast<float>(rand() % 1000) / 10.0f;
    points.push_back(lightusd::legacy::value::point3f{x, y, z});
  }

  // Set points as attribute
  TypedAttribute<Animatable<std::vector<lightusd::legacy::value::point3f>>> points_attr;
  points_attr.set_value(points);
  mesh.points = points_attr;

  // Generate face vertex indices
  std::vector<int> indices;
  indices.reserve(indices_size);
  for (size_t i = 0; i < indices_size; i++) {
    indices.push_back(static_cast<int>(rand() % points_size));
  }

  TypedAttribute<Animatable<std::vector<int>>> indices_attr;
  indices_attr.set_value(indices);
  mesh.faceVertexIndices = indices_attr;

  // Generate face vertex counts (triangles)
  std::vector<int> counts;
  size_t num_faces = indices_size / 3;
  counts.reserve(num_faces);
  for (size_t i = 0; i < num_faces; i++) {
    counts.push_back(3);
  }

  TypedAttribute<Animatable<std::vector<int>>> counts_attr;
  counts_attr.set_value(counts);
  mesh.faceVertexCounts = counts_attr;

  return mesh;
}

// Generate synthetic Prim with timesamples
Prim generate_prim_with_timesamples(
    const std::string& name,
    size_t array_size,
    size_t num_times,
    size_t timesample_array_size) {

  // Create mesh with arrays
  GeomMesh mesh = generate_mesh(name, array_size, array_size * 3);

  // Create Prim with the mesh and set the name
  Prim prim(name, lightusd::legacy::value::Value(mesh));

  // Note: Timesamples properties removed for simplicity
  // The mesh already contains arrays that test array printing performance
  (void)num_times;  // Suppress unused parameter warning
  (void)timesample_array_size;  // Suppress unused parameter warning

  return prim;
}

// ============================================================================
// Benchmark Functions
// ============================================================================

struct BenchmarkResult {
  double time_ms = 0.0;
  size_t output_size_bytes = 0;
  std::string method_name;

  void print() const {
    std::cout << std::left << std::setw(30) << method_name;
    std::cout << std::right << std::setw(12) << std::fixed << std::setprecision(2) << time_ms << " ms";
    std::cout << std::setw(15) << (output_size_bytes / (1024 * 1024)) << " MB";

    // Calculate throughput
    if (time_ms > 0) {
      double throughput_mb_s = (output_size_bytes / (1024.0 * 1024.0)) / (time_ms / 1000.0);
      std::cout << std::setw(15) << std::fixed << std::setprecision(2) << throughput_mb_s << " MB/s";
    }
    std::cout << "\n";
  }
};

// Benchmark: String-based pprint
BenchmarkResult benchmark_string_pprint(const std::vector<Prim>& prims) {
  BenchmarkResult result;
  result.method_name = "String-based pprint";

  auto start = std::chrono::high_resolution_clock::now();

  std::string output;
  for (const auto& prim : prims) {
    output += prim::print_prim(prim, 0);
  }

  auto end = std::chrono::high_resolution_clock::now();

  result.time_ms = std::chrono::duration<double, std::milli>(end - start).count();
  result.output_size_bytes = output.size();

  return result;
}

// Benchmark: StreamWriter-based pprint
BenchmarkResult benchmark_streamwriter_pprint(const std::vector<Prim>& prims) {
  BenchmarkResult result;
  result.method_name = "StreamWriter pprint";

  auto start = std::chrono::high_resolution_clock::now();

  StreamWriter writer;
  for (const auto& prim : prims) {
    prim::print_prim(writer, prim, 0);
  }
  std::string output = writer.str();

  auto end = std::chrono::high_resolution_clock::now();

  result.time_ms = std::chrono::duration<double, std::milli>(end - start).count();
  result.output_size_bytes = output.size();

  return result;
}

// Benchmark: ChunkedStreamWriter-based pprint
BenchmarkResult benchmark_chunked_pprint(const std::vector<Prim>& prims) {
  BenchmarkResult result;
  result.method_name = "ChunkedStreamWriter pprint";

  auto start = std::chrono::high_resolution_clock::now();

  ChunkedStreamWriter<4096, 16> writer;
  for (const auto& prim : prims) {
    prim::print_prim(writer, prim, 0);
  }
  std::string output = writer.str();

  auto end = std::chrono::high_resolution_clock::now();

  result.time_ms = std::chrono::duration<double, std::milli>(end - start).count();
  result.output_size_bytes = output.size();

  return result;
}

#ifdef LIGHTUSD_ENABLE_THREAD
// Benchmark: Parallel ChunkedStreamWriter pprint
// Commented out: parallel printing API not available
#if 0
BenchmarkResult benchmark_parallel_chunked_pprint(const std::vector<Prim>& prims) {
  BenchmarkResult result;
  result.method_name = "Parallel ChunkedStreamWriter";

  auto start = std::chrono::high_resolution_clock::now();

  // Create vector of Prim pointers
  std::vector<const Prim*> prim_ptrs;
  prim_ptrs.reserve(prims.size());
  for (const auto& prim : prims) {
    prim_ptrs.push_back(&prim);
  }

  ChunkedStreamWriter<4096, 16> writer;
  prim::ParallelPrintConfig config;
  config.enabled = true;
  config.min_prims_for_parallel = 4;

  prim::print_prims_parallel(writer, prim_ptrs, 0, config);
  std::string output = writer.str();

  auto end = std::chrono::high_resolution_clock::now();

  result.time_ms = std::chrono::duration<double, std::milli>(end - start).count();
  result.output_size_bytes = output.size();

  return result;
}
#endif // 0 - parallel printing API not available
#endif

// ============================================================================
// Main
// ============================================================================

void print_usage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  --preset <name>           Use preset configuration (small/medium/large/huge)\n";
  std::cout << "  --num-prims <n>          Number of Prims to generate (default: 100)\n";
  std::cout << "  --array-min <n>          Minimum array size (default: 100)\n";
  std::cout << "  --array-max <n>          Maximum array size (default: 1000)\n";
  std::cout << "  --num-times <n>          Number of timesample times (default: 10)\n";
  std::cout << "  --ts-array-size <n>      Timesample array size (default: 100)\n";
  std::cout << "  --parallel               Enable parallel printing benchmark\n";
  std::cout << "  --verbose                Verbose output\n";
  std::cout << "  --help                   Show this help message\n";
  std::cout << "\nPresets:\n";
  std::cout << "  small:   ~10 MB output\n";
  std::cout << "  medium:  ~100 MB output\n";
  std::cout << "  large:   ~1 GB output\n";
  std::cout << "  huge:    ~10 GB output\n";
  std::cout << "\n";
}

int main(int argc, char** argv) {
  BenchmarkConfig config;
  // bool use_preset = false;  // Currently unused

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--preset" && i + 1 < argc) {
      config = get_preset(argv[++i]);
      // use_preset = true;
    } else if (arg == "--num-prims" && i + 1 < argc) {
      config.num_prims = static_cast<size_t>(std::atoll(argv[++i]));
    } else if (arg == "--array-min" && i + 1 < argc) {
      config.array_min_size = static_cast<size_t>(std::atoll(argv[++i]));
    } else if (arg == "--array-max" && i + 1 < argc) {
      config.array_max_size = static_cast<size_t>(std::atoll(argv[++i]));
    } else if (arg == "--num-times" && i + 1 < argc) {
      config.num_timesample_times = static_cast<size_t>(std::atoll(argv[++i]));
    } else if (arg == "--ts-array-size" && i + 1 < argc) {
      config.timesample_array_size = static_cast<size_t>(std::atoll(argv[++i]));
    } else if (arg == "--parallel") {
      config.use_parallel = true;
    } else if (arg == "--verbose") {
      config.verbose = true;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  // Print configuration
  std::cout << "========================================\n";
  std::cout << "LightUSD Pretty-Print Benchmark\n";
  std::cout << "========================================\n\n";

  config.print();

  // Generate test data
  std::cout << "Generating test data...\n";
  std::vector<Prim> prims;
  prims.reserve(config.num_prims);

  srand(42); // Fixed seed for reproducibility

  for (size_t i = 0; i < config.num_prims; i++) {
    size_t array_size = config.array_min_size +
                       (rand() % (config.array_max_size - config.array_min_size + 1));

    std::string prim_name = "Mesh_" + std::to_string(i);
    Prim prim = generate_prim_with_timesamples(
      prim_name,
      array_size,
      config.num_timesample_times,
      config.timesample_array_size
    );
    prims.push_back(std::move(prim));

    if (config.verbose && (i % 10 == 0)) {
      std::cout << "  Generated " << (i + 1) << " / " << config.num_prims << " prims\r" << std::flush;
    }
  }

  if (config.verbose) {
    std::cout << "\n";
  }
  std::cout << "Test data generated: " << prims.size() << " prims\n\n";

  // Run benchmarks
  std::cout << "Running benchmarks...\n\n";
  std::cout << std::left << std::setw(30) << "Method";
  std::cout << std::right << std::setw(12) << "Time";
  std::cout << std::setw(15) << "Output Size";
  std::cout << std::setw(15) << "Throughput";
  std::cout << "\n";
  std::cout << std::string(72, '-') << "\n";

  std::vector<BenchmarkResult> results;

  // Benchmark 1: String-based
  {
    BenchmarkResult result = benchmark_string_pprint(prims);
    result.print();
    results.push_back(result);
  }

  // Benchmark 2: StreamWriter
  {
    BenchmarkResult result = benchmark_streamwriter_pprint(prims);
    result.print();
    results.push_back(result);
  }

  // Benchmark 3: ChunkedStreamWriter
  {
    BenchmarkResult result = benchmark_chunked_pprint(prims);
    result.print();
    results.push_back(result);
  }

#ifdef LIGHTUSD_ENABLE_THREAD
  // Benchmark 4: Parallel ChunkedStreamWriter
  if (config.use_parallel && prims.size() >= 4) {
    // Note: Parallel print_prims_parallel not available in current build
    // Uncomment when parallel printing API is available
    // BenchmarkResult result = benchmark_parallel_chunked_pprint(prims);
    // result.print();
    // results.push_back(result);
    std::cout << "Note: Parallel benchmark skipped (API not available)\n\n";
  }
#endif

  std::cout << std::string(72, '-') << "\n";

  // Print speedup comparison
  if (results.size() > 1) {
    std::cout << "\nSpeedup vs. String-based:\n";
    double baseline_time = results[0].time_ms;
    for (size_t i = 1; i < results.size(); i++) {
      double speedup = baseline_time / results[i].time_ms;
      std::cout << "  " << std::left << std::setw(30) << results[i].method_name;
      std::cout << std::right << std::fixed << std::setprecision(2) << speedup << "x\n";
    }
  }

  std::cout << "\nBenchmark completed.\n";

  return 0;
}
