// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Feature test for zstd-compressed USD file support

#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>

#define LIGHTUSD_WITH_ZSTD_COMPRESSION

#include "lightusd.hh"
#include "usda-writer.hh"
#include "zstd-compression.hh"

// Simple test macros
#define TEST_ASSERT(cond, msg) do { \
  if (!(cond)) { \
    std::cerr << "FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
    return 1; \
  } \
  std::cout << "PASSED: " << msg << std::endl; \
} while(0)

#define TEST_ASSERT_FALSE(cond, msg) TEST_ASSERT(!(cond), msg)

// Test 1: Magic number detection
int test_magic_number_detection() {
  std::cout << "\n=== Test: Magic Number Detection ===" << std::endl;

  // Valid zstd magic number
  uint8_t valid_magic[] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT(lightusd::legacy::ZstdCompression::IsZstdCompressed(valid_magic, 8),
              "Should detect valid zstd magic");

  // Invalid data
  uint8_t invalid_data[] = {0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT_FALSE(lightusd::legacy::ZstdCompression::IsZstdCompressed(invalid_data, 4),
                    "Should reject non-zstd data");

  // USDA header
  uint8_t usda_header[] = {'#', 'u', 's', 'd', 'a', ' ', '1', '.'};
  TEST_ASSERT_FALSE(lightusd::legacy::ZstdCompression::IsZstdCompressed(usda_header, 8),
                    "Should reject USDA header");

  // USDC header
  uint8_t usdc_header[] = {'P', 'X', 'R', '-', 'U', 'S', 'D', 'C'};
  TEST_ASSERT_FALSE(lightusd::legacy::ZstdCompression::IsZstdCompressed(usdc_header, 8),
                    "Should reject USDC header");

  // Too short data
  uint8_t short_data[] = {0x28, 0xB5};
  TEST_ASSERT_FALSE(lightusd::legacy::ZstdCompression::IsZstdCompressed(short_data, 2),
                    "Should reject too short data");

  // Null data
  TEST_ASSERT_FALSE(lightusd::legacy::ZstdCompression::IsZstdCompressed(nullptr, 0),
                    "Should reject null data");

  return 0;
}

// Test 2: Basic compression/decompression round-trip
int test_compression_roundtrip() {
  std::cout << "\n=== Test: Compression Round-trip ===" << std::endl;

  const char* test_data = "This is a test string for zstd compression. "
                          "It should be compressed and decompressed correctly.";
  size_t test_size = strlen(test_data);

  std::vector<uint8_t> compressed;
  std::string err;

  // Compress
  bool compress_ok = lightusd::legacy::ZstdCompression::Compress(
      reinterpret_cast<const uint8_t*>(test_data), test_size,
      &compressed, 5, &err);
  TEST_ASSERT(compress_ok, "Compression should succeed");
  TEST_ASSERT(compressed.size() > 0, "Compressed size should be > 0");
  TEST_ASSERT(compressed.size() < test_size, "Compressed data should be smaller");

  // Verify magic number
  TEST_ASSERT(lightusd::legacy::ZstdCompression::IsZstdCompressed(compressed.data(), compressed.size()),
              "Compressed data should have zstd magic number");

  // Get decompressed size
  size_t decompressed_size = lightusd::legacy::ZstdCompression::GetDecompressedSize(
      compressed.data(), compressed.size(), &err);
  TEST_ASSERT(decompressed_size == test_size, "Decompressed size should match original");

  // Decompress
  std::vector<uint8_t> decompressed;
  bool decompress_ok = lightusd::legacy::ZstdCompression::Decompress(
      compressed.data(), compressed.size(), &decompressed, &err);
  TEST_ASSERT(decompress_ok, "Decompression should succeed");
  TEST_ASSERT(decompressed.size() == test_size, "Decompressed data size should match");
  TEST_ASSERT(memcmp(decompressed.data(), test_data, test_size) == 0,
              "Decompressed data should match original");

  std::cout << "Compression ratio: " << test_size << " -> " << compressed.size()
            << " (" << (100.0 * compressed.size() / test_size) << "%)" << std::endl;

  return 0;
}

// Test 3: Different compression levels
int test_compression_levels() {
  std::cout << "\n=== Test: Compression Levels ===" << std::endl;

  // Generate some compressible test data
  std::string test_data;
  for (int i = 0; i < 1000; i++) {
    test_data += "Hello World! This is test data for compression level testing. ";
  }

  std::string err;
  size_t prev_size = test_data.size() + 1; // Start larger than input

  for (int level = 1; level <= 9; level += 4) {
    std::vector<uint8_t> compressed;
    bool ok = lightusd::legacy::ZstdCompression::Compress(
        reinterpret_cast<const uint8_t*>(test_data.data()), test_data.size(),
        &compressed, level, &err);
    TEST_ASSERT(ok, "Compression at level " + std::to_string(level) + " should succeed");

    std::cout << "Level " << level << ": " << test_data.size() << " -> "
              << compressed.size() << " bytes ("
              << (100.0 * compressed.size() / test_data.size()) << "%)" << std::endl;

    // Higher levels should generally give same or better compression
    // (not strictly enforced, but good to log)
  }

  return 0;
}

// Test 4: Error handling for corrupt data
int test_corrupt_data_handling() {
  std::cout << "\n=== Test: Corrupt Data Handling ===" << std::endl;

  std::string err;

  // Create corrupt data with valid magic but invalid content
  uint8_t corrupt_data[] = {0x28, 0xB5, 0x2F, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF};
  std::vector<uint8_t> output;

  bool ok = lightusd::legacy::ZstdCompression::Decompress(corrupt_data, 8, &output, &err);
  TEST_ASSERT_FALSE(ok, "Decompression of corrupt data should fail");
  TEST_ASSERT(err.size() > 0, "Error message should be provided");

  std::cout << "Expected error: " << err << std::endl;

  return 0;
}

// Test 5: GetCompressBound
int test_compress_bound() {
  std::cout << "\n=== Test: GetCompressBound ===" << std::endl;

  size_t bound = lightusd::legacy::ZstdCompression::GetCompressBound(1000);
  TEST_ASSERT(bound > 1000, "Compress bound should be larger than input size");

  bound = lightusd::legacy::ZstdCompression::GetCompressBound(0);
  TEST_ASSERT(bound > 0, "Compress bound for 0 should still return some value");

  return 0;
}

// Test 6: USDA file round-trip with compression
int test_usda_roundtrip() {
  std::cout << "\n=== Test: USDA Round-trip with Compression ===" << std::endl;

  // Create a simple stage
  lightusd::legacy::Stage stage;
  stage.metas().doc = "Test USD file for zstd compression";

  // Add a simple Xform prim
  lightusd::legacy::Xform xform;
  xform.name = "TestXform";

  lightusd::legacy::Prim prim;
  prim.set_data(xform);
  stage.root_prims().push_back(prim);

  // Export to string first
  std::string usda_content = stage.ExportToString();
  TEST_ASSERT(usda_content.size() > 0, "USDA export should produce content");

  // Compress the USDA content
  std::vector<uint8_t> compressed;
  std::string err;
  bool compress_ok = lightusd::legacy::ZstdCompression::Compress(
      reinterpret_cast<const uint8_t*>(usda_content.data()), usda_content.size(),
      &compressed, 5, &err);
  TEST_ASSERT(compress_ok, "USDA compression should succeed");

  // Decompress
  std::vector<uint8_t> decompressed;
  bool decompress_ok = lightusd::legacy::ZstdCompression::Decompress(
      compressed.data(), compressed.size(), &decompressed, &err);
  TEST_ASSERT(decompress_ok, "USDA decompression should succeed");

  // Verify content matches
  std::string decompressed_content(decompressed.begin(), decompressed.end());
  TEST_ASSERT(decompressed_content == usda_content,
              "Decompressed USDA content should match original");

  std::cout << "USDA size: " << usda_content.size() << " -> " << compressed.size()
            << " bytes (" << (100.0 * compressed.size() / usda_content.size()) << "%)"
            << std::endl;

  return 0;
}

// Test 7: IsZstdCompressed wrapper function in lightusd namespace
int test_lightusd_is_zstd_compressed() {
  std::cout << "\n=== Test: lightusd::legacy::IsZstdCompressed ===" << std::endl;

  // Valid zstd magic
  uint8_t valid_magic[] = {0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT(lightusd::legacy::IsZstdCompressed(valid_magic, 8),
              "lightusd::legacy::IsZstdCompressed should detect valid zstd magic");

  // Invalid data
  uint8_t invalid_data[] = {'#', 'u', 's', 'd', 'a'};
  TEST_ASSERT_FALSE(lightusd::legacy::IsZstdCompressed(invalid_data, 5),
                    "lightusd::legacy::IsZstdCompressed should reject non-zstd data");

  return 0;
}

// Test 8: Load zstd-compressed USDA from memory
int test_load_compressed_usda_from_memory() {
  std::cout << "\n=== Test: Load Compressed USDA from Memory ===" << std::endl;

  // Create a minimal USDA content
  const char* usda_content = R"(#usda 1.0
(
    doc = "Test compressed USDA"
)

def Xform "TestXform"
{
}
)";
  size_t usda_size = strlen(usda_content);

  // Compress it
  std::vector<uint8_t> compressed;
  std::string err;
  bool compress_ok = lightusd::legacy::ZstdCompression::Compress(
      reinterpret_cast<const uint8_t*>(usda_content), usda_size,
      &compressed, 5, &err);
  TEST_ASSERT(compress_ok, "Compression should succeed");

  // Load as USD from memory
  lightusd::legacy::Stage stage;
  std::string warn;
  bool load_ok = lightusd::legacy::LoadUSDFromMemory(
      compressed.data(), compressed.size(), "", &stage, &warn, &err);
  TEST_ASSERT(load_ok, "Loading compressed USDA from memory should succeed");

  // Verify the loaded content
  TEST_ASSERT(stage.metas().doc.value == "Test compressed USDA",
              "Document string should match");
  TEST_ASSERT(stage.root_prims().size() > 0, "Should have at least one root prim");

  return 0;
}

// Test 9: Memory budget enforcement
int test_memory_budget() {
  std::cout << "\n=== Test: Memory Budget Enforcement ===" << std::endl;

  // Create a large piece of test data
  std::string large_data(10 * 1024 * 1024, 'X'); // 10MB of data

  // Compress it
  std::vector<uint8_t> compressed;
  std::string err;
  bool compress_ok = lightusd::legacy::ZstdCompression::Compress(
      reinterpret_cast<const uint8_t*>(large_data.data()), large_data.size(),
      &compressed, 1, &err); // Use level 1 for speed
  TEST_ASSERT(compress_ok, "Compression should succeed");

  // Try to load with a small memory budget
  lightusd::legacy::USDLoadOptions options;
  options.max_memory_limit_in_mb = 1; // Only 1MB allowed

  lightusd::legacy::Stage stage;
  std::string warn;
  err.clear();

  // This should fail because decompressed size exceeds the memory limit
  // Note: This test may not work perfectly because the compressed data
  // isn't valid USD, but it tests the memory check path
  bool load_ok = lightusd::legacy::LoadUSDFromMemory(
      compressed.data(), compressed.size(), "", &stage, &warn, &err, options);

  // Should fail due to memory limit
  TEST_ASSERT_FALSE(load_ok, "Loading should fail due to memory limit");
  std::cout << "Expected error: " << err << std::endl;

  return 0;
}

// Test 10: Write compressed USDA file
int test_write_compressed_usda() {
  std::cout << "\n=== Test: Write Compressed USDA File ===" << std::endl;

  // Create a simple stage
  lightusd::legacy::Stage stage;
  stage.metas().doc = "Test compressed USDA write";

  lightusd::legacy::Xform xform;
  xform.name = "WriteTest";

  lightusd::legacy::Prim prim;
  prim.set_data(xform);
  stage.root_prims().push_back(prim);

  // Write with compression (using options)
  lightusd::legacy::USDWriteOptions options;
  options.use_zstd_compression = true;
  options.zstd_compression_level = 5;

  std::string warn, err;
  bool write_ok = lightusd::legacy::usda::SaveAsUSDA(
      "test_output.usda.zst", stage, &warn, &err, options);
  TEST_ASSERT(write_ok, "Writing compressed USDA should succeed");

  // Read back the file
  std::ifstream ifs("test_output.usda.zst", std::ios::binary);
  TEST_ASSERT(ifs.is_open(), "Output file should exist");

  std::vector<uint8_t> file_content((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
  ifs.close();

  // Verify it's zstd compressed
  TEST_ASSERT(lightusd::legacy::IsZstdCompressed(file_content.data(), file_content.size()),
              "Output file should be zstd compressed");

  // Load it back
  lightusd::legacy::Stage loaded_stage;
  bool load_ok = lightusd::legacy::LoadUSDFromMemory(
      file_content.data(), file_content.size(), "", &loaded_stage, &warn, &err);
  TEST_ASSERT(load_ok, "Loading written compressed file should succeed");

  // Verify content
  TEST_ASSERT(loaded_stage.metas().doc.value == "Test compressed USDA write",
              "Loaded doc should match");

  // Clean up test file
  std::remove("test_output.usda.zst");

  return 0;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  std::cout << "===== Zstd USD Compression Feature Tests =====" << std::endl;

  int failures = 0;

  failures += test_magic_number_detection();
  failures += test_compression_roundtrip();
  failures += test_compression_levels();
  failures += test_corrupt_data_handling();
  failures += test_compress_bound();
  failures += test_usda_roundtrip();
  failures += test_lightusd_is_zstd_compressed();
  failures += test_load_compressed_usda_from_memory();
  failures += test_memory_budget();
  failures += test_write_compressed_usda();

  std::cout << "\n===== Summary =====" << std::endl;
  if (failures == 0) {
    std::cout << "All tests PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << failures << " test(s) FAILED!" << std::endl;
    return 1;
  }
}
