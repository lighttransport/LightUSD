#include <cstdint>

#include "lightusd.hh"
#include "usdc-reader.hh"

static void parse_usdc(const uint8_t *data, size_t size)
{
  // append magic header
  std::string content("PXR-USDC");

  std::vector<uint8_t> buf;
  buf.resize(content.size() + size);
  memcpy(buf.data(), content.data(), content.size());

  if (size > 0 && data) {
    memcpy(buf.data() + content.size(), data, size);
  }

  size_t total_size = content.size() + size;


  lightusd::StreamReader sr(buf.data(), total_size, /* endianswap */false);

  lightusd::usdc::USDCReaderConfig config;

  // For fuzzer run
  config.kMaxAllowedMemoryInMB = 1024*4; // 4GB.

  lightusd::usdc::USDCReader reader(&sr, config);

  bool ret = reader.ReadUSDC();
  if (ret) {
    // Crate fields/sections alone do not decode all authored values. Exercise
    // stage reconstruction too, including array storage and property payloads.
    lightusd::Stage stage;
    (void)reader.ReconstructStage(&stage);
  }

  return;
}

extern "C"
int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    parse_usdc(data, size);
    return 0;
}
