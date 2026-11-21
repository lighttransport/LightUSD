// SPDX-License-Identifier: Apache 2.0
// Simple benchmark to compare sequential vs parallel prim printing
//
#include <iostream>
#include <chrono>
#include "stage.hh"
#include "lightusd.hh"
#include "io-util.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <usd_file>\n";
    return 1;
  }

  std::string filename = argv[1];
  std::string warn, err;

  // Load USD file
  Stage stage;
  bool ret = LoadUSDFromFile(filename, &stage, &warn, &err);

  if (!warn.empty()) {
    std::cout << "WARN: " << warn << "\n";
  }

  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return 1;
  }

  std::cout << "Loaded USD file: " << filename << "\n";
  std::cout << "Number of root prims: " << stage.root_prims().size() << "\n\n";

  // Benchmark sequential printing
  {
    auto start = std::chrono::high_resolution_clock::now();
    std::string result = stage.ExportToString(false, false);  // Sequential
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Sequential printing:\n";
    std::cout << "  Time: " << duration.count() << " ms\n";
    std::cout << "  Output size: " << result.size() << " bytes\n\n";
  }

  // Benchmark parallel printing
  {
    auto start = std::chrono::high_resolution_clock::now();
    std::string result = stage.ExportToString(false, true);  // Parallel
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Parallel printing:\n";
    std::cout << "  Time: " << duration.count() << " ms\n";
    std::cout << "  Output size: " << result.size() << " bytes\n\n";
  }

  // Verify both produce the same output
  std::string seq_result = stage.ExportToString(false, false);
  std::string par_result = stage.ExportToString(false, true);

  if (seq_result == par_result) {
    std::cout << "✓ Sequential and parallel outputs match!\n";
  } else {
    std::cout << "✗ WARNING: Sequential and parallel outputs differ!\n";
    std::cout << "  Sequential size: " << seq_result.size() << "\n";
    std::cout << "  Parallel size: " << par_result.size() << "\n";
  }

  return 0;
}
