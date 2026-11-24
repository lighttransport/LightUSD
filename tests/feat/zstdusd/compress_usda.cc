// Simple utility to compress a USDA file with zstd
// Build: g++ -std=c++14 -o compress_usda compress_usda.cc -I../../../src -I../../../src/external ../../../src/external/zstd.c -DLIGHTUSD_WITH_ZSTD_COMPRESSION -DZSTD_DISABLE_ASM=1 -w

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

#define LIGHTUSD_WITH_ZSTD_COMPRESSION
#include "zstd-compression.hh"

// Include zstd header for the implementation
#include "external/zstd.h"

// Implement the static members
namespace lightusd { namespace legacy {

constexpr uint8_t ZstdCompression::kZstdMagic[4];

bool ZstdCompression::IsZstdCompressed(const uint8_t *data, size_t length) {
  if (!data || length < 4) {
    return false;
  }
  return (data[0] == kZstdMagic[0] && data[1] == kZstdMagic[1] &&
          data[2] == kZstdMagic[2] && data[3] == kZstdMagic[3]);
}

bool ZstdCompression::Compress(const uint8_t *input, size_t inputSize,
                                std::vector<uint8_t> *output,
                                int compressionLevel, std::string *err) {
  if (!input || inputSize == 0) {
    if (err) *err = "Invalid input data";
    return false;
  }

  if (!output) {
    if (err) *err = "Output buffer is null";
    return false;
  }

  // Clamp compression level to valid range
  if (compressionLevel < 1) compressionLevel = 1;
  if (compressionLevel > ZSTD_maxCLevel()) compressionLevel = ZSTD_maxCLevel();

  // Get maximum compressed size
  size_t maxCompressedSize = ZSTD_compressBound(inputSize);

  // Allocate output buffer
  output->resize(maxCompressedSize);

  // Compress
  size_t result =
      ZSTD_compress(output->data(), maxCompressedSize, input, inputSize,
                    compressionLevel);

  if (ZSTD_isError(result)) {
    if (err) {
      *err = "Zstd compression failed: ";
      *err += ZSTD_getErrorName(result);
    }
    output->clear();
    return false;
  }

  // Shrink to actual compressed size
  output->resize(result);

  return true;
}

size_t ZstdCompression::GetCompressBound(size_t inputSize) {
  return ZSTD_compressBound(inputSize);
}

}  // namespace legacy
}  // namespace lightusd

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input.usda> <output.usda.zst> [compression_level]" << std::endl;
    return 1;
  }

  const char* input_file = argv[1];
  const char* output_file = argv[2];
  int compression_level = 5;
  if (argc >= 4) {
    compression_level = std::atoi(argv[3]);
  }

  // Read input file
  std::ifstream ifs(input_file, std::ios::binary);
  if (!ifs.is_open()) {
    std::cerr << "Failed to open input file: " << input_file << std::endl;
    return 1;
  }

  std::vector<uint8_t> input_data((std::istreambuf_iterator<char>(ifs)),
                                   std::istreambuf_iterator<char>());
  ifs.close();

  std::cout << "Input file: " << input_file << std::endl;
  std::cout << "Input size: " << input_data.size() << " bytes" << std::endl;

  // Compress
  std::vector<uint8_t> compressed;
  std::string err;
  bool ok = lightusd::legacy::ZstdCompression::Compress(
      input_data.data(), input_data.size(), &compressed, compression_level, &err);

  if (!ok) {
    std::cerr << "Compression failed: " << err << std::endl;
    return 1;
  }

  std::cout << "Compressed size: " << compressed.size() << " bytes" << std::endl;
  std::cout << "Compression ratio: " << (100.0 * compressed.size() / input_data.size()) << "%" << std::endl;

  // Write output file
  std::ofstream ofs(output_file, std::ios::binary);
  if (!ofs.is_open()) {
    std::cerr << "Failed to open output file: " << output_file << std::endl;
    return 1;
  }
  ofs.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
  ofs.close();

  std::cout << "Wrote compressed file: " << output_file << std::endl;

  return 0;
}
