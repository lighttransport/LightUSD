#include <cstdint>

#include "lightusd.hh"

static void parse_usdz(const uint8_t *data, size_t size)
{
  lightusd::legacy::Stage stage;
  std::string warn;
  std::string err;
  bool ret = lightusd::legacy::LoadUSDZFromMemory(data, size, "", &stage, &warn, &err);
  (void)ret;

  return;
}

extern "C"
int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    parse_usdz(data, size);
    return 0;
}
