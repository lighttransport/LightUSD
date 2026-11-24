#include <cstdint>

#include "lightusd.hh"
#include "usda-reader.hh"

static void parse_usda(const uint8_t *data, size_t size)
{
  // append magic header
  std::string content("#usda 1.0\n");

  std::vector<uint8_t> buf;
  buf.resize(content.size() + size);
  memcpy(buf.data(), content.data(), content.size());

  if (size > 0 && data) {
    memcpy(buf.data() + content.size(), data, size);
  }

  size_t total_size = content.size() + size;

  lightusd::legacy::StreamReader sr(buf.data(), total_size, /* endianswap */false);

  lightusd::legacy::usda::USDAReaderConfig config;
  config.max_memory_limit_in_mb = 1024 * 4; // 4GB for fuzzer

  lightusd::legacy::usda::USDAReader reader(&sr);
  reader.set_reader_config(config);

  bool ret = reader.Read();
  (void)ret;

  return;
}

extern "C"
int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    parse_usda(data, size);
    return 0;
}
