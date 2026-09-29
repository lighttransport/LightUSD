// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Performance Benchmarks

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif

#include "next/layer/layer.hh"
#include "next/pcp/prim-index.hh"
#include "next/crate/crate-reader.hh"
#include "next/pipeline/flatten.hh"
#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"

using namespace lightusd::next;

// Timer helper
class Timer {
public:
  void start() {
    start_ = std::chrono::high_resolution_clock::now();
  }

  double elapsed_ms() const {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start_).count();
  }

  double elapsed_us() const {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::micro>(end - start_).count();
  }

private:
  std::chrono::high_resolution_clock::time_point start_;
};

// ============================================================
// Benchmark: Value creation and access
// ============================================================

void benchmark_value_creation(int iterations) {
  printf("\n=== Value Creation Benchmark ===\n");

  Timer timer;

  // Scalar creation
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    Value v(static_cast<float>(i));
    (void)v;
  }
  printf("Float scalar: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);

  // Float3 creation
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    Value v = Value::MakeFloat3(1.0f, 2.0f, 3.0f);
    (void)v;
  }
  printf("Float3: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);

  // Matrix4f creation
  timer.start();
  float mat[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  for (int i = 0; i < iterations; ++i) {
    Value v = Value::MakeMatrix4f(mat);
    (void)v;
  }
  printf("Matrix4f: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);

  // String creation
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    Value v(std::string("test_string_value"));
    (void)v;
  }
  printf("String: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);
}

// ============================================================
// Benchmark: Property lookup
// ============================================================

void benchmark_property_lookup(int iterations) {
  printf("\n=== Property Lookup Benchmark ===\n");

  // Create a PrimSpec with many properties
  PrimSpec prim("TestPrim", "Mesh");

  // Add properties
  for (int i = 0; i < 100; ++i) {
    std::string name = "property_" + std::to_string(i);
    prim.add_property(name, Value(static_cast<float>(i)));
  }
  prim.finalize_properties();

  Timer timer;

  // Lookup by string (first time - interning)
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    int idx = i % 100;
    std::string name = "property_" + std::to_string(idx);
    const Value* v = prim.property_value(name);
    (void)v;
  }
  printf("String lookup: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);

  // Lookup by pre-interned ID
  PropNameId id = GetPropNameTable().find("property_50");
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    const Value* v = prim.property_value(id);
    (void)v;
  }
  printf("ID lookup: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);
}

// ============================================================
// Benchmark: Layer building
// ============================================================

void benchmark_layer_building(int num_prims) {
  printf("\n=== Layer Building Benchmark ===\n");

  Timer timer;
  timer.start();

  Layer layer;
  LayerBuilder builder(layer);

  // Build points array
  std::vector<float> points = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
  std::vector<int32_t> faceVertexCounts = {4};
  std::vector<int32_t> faceVertexIndices = {0,1,2,3};

  for (int i = 0; i < num_prims; ++i) {
    std::string name = "Prim_" + std::to_string(i);
    builder.begin_prim(name, "Mesh");

    // Add some properties
    builder.add_property("points", Value::MakeFloat3Array(points));
    builder.add_property("faceVertexCounts", Value::MakeIntArray(faceVertexCounts));
    builder.add_property("faceVertexIndices", Value::MakeIntArray(faceVertexIndices));

    builder.end_prim();
  }

  builder.finalize();

  double elapsed = timer.elapsed_ms();
  printf("Built %d prims in %.2f ms (%.0f prims/sec)\n",
         num_prims, elapsed, num_prims / elapsed * 1000);

  auto stats = layer.stats();
  printf("  Total prims: %zu\n", stats.prim_count);
  printf("  Total properties: %zu\n", stats.total_properties);
  printf("  Memory usage: %.2f KB\n", stats.memory_bytes / 1024.0);
}

// ============================================================
// Benchmark: Stage traversal
// ============================================================

void benchmark_traversal(int num_prims) {
  printf("\n=== Stage Traversal Benchmark ===\n");

  // Build a stage with prims
  Layer layer;
  LayerBuilder builder(layer);
  for (int i = 0; i < num_prims; ++i) {
    std::string name = "Prim_" + std::to_string(i);
    builder.begin_prim(name, "Xform");
    builder.end_prim();
  }
  builder.finalize();

  Stage stage;
  stage.SetRootLayer(std::move(layer));

  Timer timer;

  // Count traversal
  timer.start();
  int count = 0;
  stage.Traverse([&count](const UsdPrim& /* prim */) {
    ++count;
    return true;
  });
  double elapsed = timer.elapsed_ms();
  printf("Traversed %d prims in %.2f ms (%.0f prims/sec)\n",
         count, elapsed, count / elapsed * 1000);

  // Type-filtered traversal
  timer.start();
  auto xforms = stage.GetPrimsOfType("Xform");
  elapsed = timer.elapsed_ms();
  printf("GetPrimsOfType found %zu in %.2f ms\n", xforms.size(), elapsed);
}

// ============================================================
// Benchmark: Time sample interpolation
// ============================================================

void benchmark_time_samples(int num_samples) {
  printf("\n=== Time Sample Benchmark ===\n");

  Layer layer;
  LayerBuilder builder(layer);
  builder.begin_prim("AnimatedCube", "Mesh");

  // Add time samples
  for (int i = 0; i < num_samples; ++i) {
    double time = static_cast<double>(i);
    builder.add_time_sample("xformOp:translate",
                            time,
                            Value::MakeFloat3(static_cast<float>(i), 0, 0));
  }

  builder.end_prim();
  builder.finalize();

  Stage stage;
  stage.SetRootLayer(std::move(layer));

  UsdPrim prim = stage.GetPrimAtPath("/AnimatedCube");

  Timer timer;
  int iterations = 10000;

  // Time sample query
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    double t = static_cast<double>(i % num_samples);
    const Value* v = prim.GetValueAtTime("xformOp:translate", t);
    (void)v;
  }
  printf("GetValueAtTime: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);

  // Interpolated query
  timer.start();
  for (int i = 0; i < iterations; ++i) {
    double t = static_cast<double>(i % num_samples) + 0.5;
    Value v = prim.GetInterpolatedValue("xformOp:translate", t);
    (void)v;
  }
  printf("GetInterpolatedValue: %.2f us/iter (%.0f ops/sec)\n",
         timer.elapsed_us() / iterations,
         iterations / timer.elapsed_ms() * 1000);
}

// ============================================================
// Benchmark: USDA parsing (synthetic)
// ============================================================

void benchmark_usda_parsing() {
  printf("\n=== USDA Parsing Benchmark ===\n");

  // Generate a synthetic USDA
  std::string usda = "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\n";

  const int num_prims = 1000;
  for (int i = 0; i < num_prims; ++i) {
    usda += "def Mesh \"Mesh_" + std::to_string(i) + "\"\n{\n";
    usda += "    float3[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n";
    usda += "    int[] faceVertexCounts = [4]\n";
    usda += "    int[] faceVertexIndices = [0, 1, 2, 3]\n";
    usda += "}\n\n";
  }

  printf("USDA size: %.2f KB\n", usda.size() / 1024.0);

  Timer timer;
  timer.start();

  LoadResult result = LoadUSDAFromString(usda);

  double elapsed = timer.elapsed_ms();

  if (result.success) {
    printf("Parsed %d prims in %.2f ms (%.0f prims/sec)\n",
           num_prims, elapsed, num_prims / elapsed * 1000);
    printf("  Throughput: %.2f MB/sec\n",
           (usda.size() / 1024.0 / 1024.0) / (elapsed / 1000.0));
  } else {
    printf("Parse failed: %s\n", result.error_summary.c_str());
  }
}

// ============================================================
// Benchmark: USDA writing
// ============================================================

void benchmark_usda_writing(int num_prims) {
  printf("\n=== USDA Writing Benchmark ===\n");

  // Build a stage
  std::vector<float> points = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
  std::vector<int32_t> faceVertexCounts = {4};
  std::vector<int32_t> faceVertexIndices = {0,1,2,3};

  Layer layer;
  LayerBuilder builder(layer);
  for (int i = 0; i < num_prims; ++i) {
    std::string name = "Mesh_" + std::to_string(i);
    builder.begin_prim(name, "Mesh");
    builder.add_property("points", Value::MakeFloat3Array(points));
    builder.add_property("faceVertexCounts", Value::MakeIntArray(faceVertexCounts));
    builder.add_property("faceVertexIndices", Value::MakeIntArray(faceVertexIndices));
    builder.end_prim();
  }
  builder.finalize();

  Stage stage;
  stage.SetRootLayer(std::move(layer));

  Timer timer;
  timer.start();

  std::string usda = WriteUSDAToString(stage);

  double elapsed = timer.elapsed_ms();
  printf("Wrote %d prims in %.2f ms (%.0f prims/sec)\n",
         num_prims, elapsed, num_prims / elapsed * 1000);
  printf("  Output size: %.2f KB\n", usda.size() / 1024.0);
  printf("  Throughput: %.2f MB/sec\n",
         (usda.size() / 1024.0 / 1024.0) / (elapsed / 1000.0));
}

// ============================================================
// Benchmark: USDC writing
// ============================================================

void benchmark_usdc_writing(int num_prims) {
  printf("\n=== USDC Writing Benchmark ===\n");

  std::vector<float> points = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
  std::vector<int32_t> fvc = {4};
  std::vector<int32_t> fvi = {0,1,2,3};

  Layer layer;
  LayerBuilder builder(layer);
  for (int i = 0; i < num_prims; ++i) {
    builder.begin_prim("Mesh_" + std::to_string(i), "Mesh");
    builder.add_property("points", Value::MakeFloat3Array(points));
    builder.add_property("faceVertexCounts", Value::MakeIntArray(fvc));
    builder.add_property("faceVertexIndices", Value::MakeIntArray(fvi));
    builder.end_prim();
  }
  builder.finalize();

  Stage stage;
  stage.SetRootLayer(std::move(layer));

  Timer timer;
  timer.start();
  std::vector<uint8_t> buf;
  auto result = WriteUSDCToMemory(buf, stage);
  double elapsed = timer.elapsed_ms();

  if (result.success) {
    printf("Wrote %d prims in %.2f ms (%.0f prims/sec)\n",
           num_prims, elapsed, num_prims / elapsed * 1000);
    printf("  Output size: %.2f KB\n", buf.size() / 1024.0);
  } else {
    printf("FAIL: %s\n", result.error.c_str());
  }
}

// ============================================================
// Benchmark: USDC reading
// ============================================================

void benchmark_usdc_reading() {
  printf("\n=== USDC Reading Benchmark ===\n");

  std::vector<uint8_t> buf;
  {
    // Build a layer with 1000 prims
    std::vector<float> points = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    std::vector<int32_t> fvc = {4};
    Layer layer;
    LayerBuilder builder(layer);
    for (int i = 0; i < 1000; ++i) {
      builder.begin_prim("Mesh_" + std::to_string(i), "Mesh");
      builder.add_property("points", Value::MakeFloat3Array(points));
      builder.add_property("faceVertexCounts", Value::MakeIntArray(fvc));
      builder.end_prim();
    }
    builder.finalize();
    Stage stage;
    stage.SetRootLayer(std::move(layer));
    WriteUSDCToMemory(buf, stage);
  }

  Timer timer;
  timer.start();
  CrateReader reader;
  auto result = reader.Read(buf.data(), buf.size());
  double elapsed = timer.elapsed_ms();

  if (result.success) {
    printf("Read %zu prims in %.2f ms\n",
           result.stage.GetRootPrims().size(), elapsed);
    printf("  File size: %.2f KB\n", buf.size() / 1024.0);
    printf("  Throughput: %.2f MB/sec\n",
           (buf.size() / 1024.0 / 1024.0) / (elapsed / 1000.0));
  } else {
    printf("FAIL\n");
  }
}

// ============================================================
// Memory observability: stable struct/layer stats
// ============================================================

int print_memstats(int num_prims) {
  if (num_prims < 1) num_prims = 1;
  printf("LightUSD Next - Memory Stats\n");
  printf("sizeof(Value)=%zu\n", sizeof(Value));
  printf("sizeof(PrimSpec)=%zu\n", sizeof(PrimSpec));
  printf("sizeof(PrimSpecMeta)=%zu\n", sizeof(PrimSpecMeta));
  printf("sizeof(PrimSpecMetaExt)=%zu\n", sizeof(PrimSpecMetaExt));
  printf("sizeof(pcp::CompNode)=%zu\n", sizeof(pcp::CompNode));

  Layer layer;
  LayerBuilder builder(layer);
  for (int i = 0; i < num_prims; ++i) {
    builder.begin_prim("Prim_" + std::to_string(i), "Xform");
    if ((i % 16) == 0) {
      builder.add_property("visibility", Value::MakeToken("inherited"));
    }
    builder.end_prim();
  }
  builder.finalize();

  const Layer::Stats stats = layer.stats();
  printf("synthetic_prims=%zu\n", stats.prim_count);
  printf("synthetic_roots=%zu\n", stats.root_count);
  printf("synthetic_properties=%zu\n", stats.total_properties);
  printf("synthetic_time_samples=%zu\n", stats.total_time_samples);
  printf("synthetic_memory_bytes=%zu\n", stats.memory_bytes);
  printf("synthetic_bytes_per_prim=%.2f\n",
         stats.prim_count ? double(stats.memory_bytes) / stats.prim_count : 0.0);
  return 0;
}

uint64_t file_size_bytes(const std::string& filename) {
  std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
  if (!ifs) return 0;
  std::streamoff end = ifs.tellg();
  return end > 0 ? static_cast<uint64_t>(end) : 0;
}

std::string lower_ext(const std::string& filename) {
  size_t dot = filename.find_last_of('.');
  if (dot == std::string::npos) return std::string();
  std::string ext = filename.substr(dot);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  }
  return ext;
}

uint64_t current_rss_bytes() {
#if defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  uint64_t size_pages = 0;
  uint64_t resident_pages = 0;
  statm >> size_pages >> resident_pages;
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 0;
  return resident_pages * static_cast<uint64_t>(page_size);
#else
  return 0;
#endif
}

uint64_t peak_rss_bytes() {
#if defined(__linux__) || defined(__APPLE__)
  struct rusage ru;
  std::memset(&ru, 0, sizeof(ru));
  if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<uint64_t>(ru.ru_maxrss);
#else
  return static_cast<uint64_t>(ru.ru_maxrss) * 1024ull;
#endif
#else
  return 0;
#endif
}

void print_bytes_metric(const char* name, uint64_t bytes) {
  if (bytes == 0) {
    printf("%s=unavailable\n", name);
  } else {
    printf("%s=%llu\n", name, static_cast<unsigned long long>(bytes));
  }
}

int print_memstats_file(const std::string& filename) {
  printf("LightUSD Next - Real File Memory Stats\n");
  printf("file=%s\n", filename.c_str());
  printf("file_size_bytes=%llu\n",
         static_cast<unsigned long long>(file_size_bytes(filename)));
  print_bytes_metric("rss_start_bytes", current_rss_bytes());
  print_bytes_metric("peak_rss_start_bytes", peak_rss_bytes());

  const std::string ext = lower_ext(filename);
  Stage stage;
  bool loaded = false;
  bool load_source_mmap = false;
  Timer timer;
  timer.start();
  if (ext == ".usda" || ext == ".usd") {
    LoadResult lr = LoadUSDAFromFile(filename);
    loaded = lr.success;
    if (loaded) {
      stage = std::move(lr.stage);
    } else {
      printf("load_error=%s\n", lr.error_summary.c_str());
    }
  } else if (ext == ".usdc") {
    USDCLoadOptions opts;
    opts.crate_options.lazy_arrays = true;
    USDCLoadResult lr = LoadUSDCFromFile(filename, opts);
    loaded = lr.success;
    load_source_mmap = lr.source_was_mmap;
    if (loaded) {
      stage = std::move(lr.stage);
    } else {
      printf("load_error=%s\n", lr.error_summary.c_str());
    }
  } else {
    printf("load_error=unsupported extension for memstats-file\n");
    return 1;
  }
  printf("load_success=%d\n", loaded ? 1 : 0);
  printf("load_source_mmap=%d\n", load_source_mmap ? 1 : 0);
  printf("load_ms=%.3f\n", timer.elapsed_ms());
  print_bytes_metric("rss_after_load_bytes", current_rss_bytes());
  print_bytes_metric("peak_rss_after_load_bytes", peak_rss_bytes());
  if (!loaded) return 1;

  const Layer* root = stage.GetRootLayer();
  if (root) {
    const Layer::Stats stats = root->stats();
    printf("stage_prims=%zu\n", stats.prim_count);
    printf("stage_roots=%zu\n", stats.root_count);
    printf("stage_properties=%zu\n", stats.total_properties);
    printf("stage_time_samples=%zu\n", stats.total_time_samples);
    printf("stage_memory_bytes=%zu\n", stats.memory_bytes);
  }

  std::vector<uint8_t> written;
  timer.start();
  USDCWriteResult wr = WriteUSDCToMemory(written, stage);
  printf("write_success=%d\n", wr.success ? 1 : 0);
  printf("write_ms=%.3f\n", timer.elapsed_ms());
  printf("write_output_bytes=%zu\n", written.size());
  if (!wr.success) printf("write_error=%s\n", wr.error.c_str());
  print_bytes_metric("rss_after_write_bytes", current_rss_bytes());
  print_bytes_metric("peak_rss_after_write_bytes", peak_rss_bytes());

  std::vector<uint8_t> flattened;
  pipeline::FlattenStats fstats;
  std::string ferr;
  pipeline::FlattenOptions fopts;
  fopts.read.lazy_arrays = true;
  timer.start();
  bool fok = pipeline::FlattenUSDFileToUSDC(filename, flattened, fopts, &fstats, &ferr);
  printf("flatten_success=%d\n", fok ? 1 : 0);
  printf("flatten_wall_ms=%.3f\n", timer.elapsed_ms());
  printf("flatten_read_ms=%.3f\n", fstats.read_ms);
  printf("flatten_compose_ms=%.3f\n", fstats.compose_ms);
  printf("flatten_write_ms=%.3f\n", fstats.write_ms);
  printf("flatten_input_bytes=%zu\n", fstats.input_bytes);
  printf("flatten_output_bytes=%zu\n", fstats.output_bytes);
  printf("flatten_prims=%zu\n", fstats.prim_count);
  printf("flatten_input_mmap=%d\n", fstats.input_was_mmap ? 1 : 0);
  printf("flatten_arrays_passed_through=%zu\n", fstats.arrays_passed_through);
  printf("flatten_arrays_reencoded=%zu\n", fstats.arrays_reencoded);
  printf("flatten_composition_errors=%zu\n", fstats.composition_errors.size());
  if (!fok) printf("flatten_error=%s\n", ferr.c_str());
  print_bytes_metric("rss_after_flatten_bytes", current_rss_bytes());
  print_bytes_metric("peak_rss_after_flatten_bytes", peak_rss_bytes());
  return fok ? 0 : 1;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "memstats-file") {
    return print_memstats_file(argv[2]);
  }
  if (argc > 1 && std::string(argv[1]) == "memstats") {
    int n = argc > 2 ? std::atoi(argv[2]) : 100000;
    return print_memstats(n);
  }

  int scale = 1;
  if (argc > 1) {
    scale = std::atoi(argv[1]);
    if (scale < 1) scale = 1;
  }

  printf("LightUSD Next - Performance Benchmarks (scale=%d)\n", scale);
  printf("================================================\n");

  benchmark_value_creation(100000 * scale);
  benchmark_property_lookup(100000 * scale);
  benchmark_layer_building(1000 * scale);
  benchmark_traversal(10000 * scale);
  benchmark_time_samples(100 * scale);
  benchmark_usda_parsing();
  benchmark_usda_writing(1000 * scale);
  benchmark_usdc_writing(1000 * scale);
  benchmark_usdc_reading();

  printf("\n=== Summary ===\n");
  printf("All benchmarks completed.\n");

  return 0;
}
