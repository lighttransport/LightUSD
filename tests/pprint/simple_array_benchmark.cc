// Simple benchmark for testing array pre-allocation optimization
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

#include "stream-writer.hh"
#include "value-types.hh"
#include "timesamples-pprint.hh"
#include "typed-array.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Helper to measure time in ms
double measure_ms(std::function<void()> fn) {
  auto start = std::chrono::high_resolution_clock::now();
  fn();
  auto end = std::chrono::high_resolution_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// Generate random float array
std::vector<float> generate_float_array(size_t size, unsigned seed = 42) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

  std::vector<float> data(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = dist(rng);
  }
  return data;
}

// Generate random float3 array
std::vector<value::float3> generate_float3_array(size_t size, unsigned seed = 42) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

  std::vector<value::float3> data(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = {{dist(rng), dist(rng), dist(rng)}};
  }
  return data;
}

// Print statistics
void print_stats(const std::string& name, double time_ms, size_t output_size) {
  double throughput = (output_size / (1024.0 * 1024.0)) / (time_ms / 1000.0);
  std::cout << name << ":\n";
  std::cout << "  Time:       " << time_ms << " ms\n";
  std::cout << "  Output:     " << (output_size / 1024.0 / 1024.0) << " MB\n";
  std::cout << "  Throughput: " << throughput << " MB/s\n";
  std::cout << std::endl;
}

int main(int argc, char** argv) {
  // Parse command line
  size_t scalar_size = 100000;  // 100K elements default
  size_t vec3_size = 50000;     // 50K elements default

  if (argc > 1) {
    scalar_size = std::atoi(argv[1]);
  }
  if (argc > 2) {
    vec3_size = std::atoi(argv[2]);
  }

  std::cout << "========================================\n";
  std::cout << "Array Pre-allocation Benchmark\n";
  std::cout << "========================================\n\n";

  std::cout << "Configuration:\n";
  std::cout << "  Scalar array size: " << scalar_size << "\n";
  std::cout << "  Float3 array size: " << vec3_size << "\n\n";

  // Generate test data
  std::cout << "Generating test data...\n";
  auto float_data = generate_float_array(scalar_size);
  auto float3_data = generate_float3_array(vec3_size);
  std::cout << "Data generated.\n\n";

  // Calculate required buffer size for StreamWriter (needs to be larger than output)
  // Each float value averages about 9 bytes (value + ", ")
  size_t estimated_scalar_size = scalar_size * 10;  // 10 bytes per value with safety margin
  size_t estimated_vec3_size = vec3_size * 30;       // 30 bytes per float3 with safety margin
  size_t max_buffer_size = std::max(estimated_scalar_size, estimated_vec3_size) * 2;  // 2x safety margin

  std::cout << "Estimated max buffer size: " << (max_buffer_size / 1024.0 / 1024.0) << " MB\n\n";

  // Test 1: Float array printing
  std::cout << "Test 1: Float Array Printing\n";
  std::cout << "------------------------------\n";
  {
    StreamWriter writer(max_buffer_size);

    double time = measure_ms([&]() {
      writer.write("[");
      for (size_t i = 0; i < float_data.size(); i++) {
        if (i > 0) writer.write(", ");
        writer.write(float_data[i]);
      }
      writer.write("]");
    });

    print_stats("Float array", time, writer.str().size());
  }

  // Test 2: Float3 array printing
  std::cout << "Test 2: Float3 Array Printing\n";
  std::cout << "-------------------------------\n";
  {
    StreamWriter writer(max_buffer_size);

    double time = measure_ms([&]() {
      writer.write("[");
      for (size_t i = 0; i < float3_data.size(); i++) {
        if (i > 0) writer.write(", ");
        writer.write("(");
        writer.write(float3_data[i][0]);
        writer.write(", ");
        writer.write(float3_data[i][1]);
        writer.write(", ");
        writer.write(float3_data[i][2]);
        writer.write(")");
      }
      writer.write("]");
    });

    print_stats("Float3 array", time, writer.str().size());
  }

  // Test 3: Multiple iterations to test consistency
  std::cout << "Test 3: Performance Consistency (10 iterations)\n";
  std::cout << "--------------------------------------------------\n";
  {
    double total_time = 0.0;
    size_t total_size = 0;

    for (int iter = 0; iter < 10; iter++) {
      StreamWriter writer(max_buffer_size);

      double time = measure_ms([&]() {
        writer.write("[");
        for (size_t i = 0; i < float_data.size(); i++) {
          if (i > 0) writer.write(", ");
          writer.write(float_data[i]);
        }
        writer.write("]");
      });

      total_time += time;
      total_size = writer.str().size();
    }

    double avg_time = total_time / 10.0;
    print_stats("Average (10 runs)", avg_time, total_size);
  }

  std::cout << "Benchmark completed.\n";
  std::cout << "\nNote: With pre-allocation optimization enabled, buffer\n";
  std::cout << "reallocation overhead is reduced during array printing.\n";

  return 0;
}
