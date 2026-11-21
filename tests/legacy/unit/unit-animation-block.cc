// SPDX-License-Identifier: Apache 2.0
// Unit tests for SdfAnimationBlock (Crate value type 60). AnimationBlock is an
// inline type tag (like ValueBlock/`None`) that requires no crate version bump.

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-animation-block.h"
#include "lightusd.hh"
#include "usdc-writer.hh"

#include <string>
#include <vector>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

static bool LoadUSDA(const std::string &usda, Stage *stage) {
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      stage, &warn, &err);
  if (!ok) {
    TEST_MSG("USDA parse failed: %s", err.c_str());
  }
  return ok;
}

static const char *kAnimBlockUSDA =
    "#usda 1.0\n"
    "\n"
    "def \"Foo\"\n"
    "{\n"
    "    custom double a = AnimationBlock\n"
    "    custom float b = None\n"
    "    custom int c = 42\n"
    "}\n";

void animation_block_usda_roundtrip_test(void) {
  // Parse -> export to USDA -> re-parse. AnimationBlock must survive and stay
  // distinct from the ValueBlock (`None`) and a normal value.
  Stage stage;
  TEST_CHECK(LoadUSDA(kAnimBlockUSDA, &stage));

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("AnimationBlock") != std::string::npos);
  // The `None`-blocked attribute must still print `None`, not AnimationBlock.
  TEST_CHECK(exported.find("b = None") != std::string::npos);

  Stage stage2;
  TEST_CHECK(LoadUSDA(exported, &stage2));
  std::string exported2 = stage2.ExportToString();
  TEST_CHECK(exported2.find("a = AnimationBlock") != std::string::npos);
  TEST_CHECK(exported2.find("b = None") != std::string::npos);
  TEST_CHECK(exported2.find("c = 42") != std::string::npos);
}

void animation_block_crate_roundtrip_test(void) {
  // Parse USDA -> save USDC (crate-60 inline write) -> reload (crate-60 read).
  Stage stage;
  TEST_CHECK(LoadUSDA(kAnimBlockUSDA, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;
  TEST_CHECK(usdc.size() > 10);

  // AnimationBlock requires NO crate version bump (matches OpenUSD): a file
  // containing only an AnimationBlock stays at the base version 0.8.0.
  TEST_CHECK_(usdc[9] == 8, "crate version minor = %d, expected 8 (no bump)",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = lightusd::legacy::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  std::string exported = stage2.ExportToString();
  TEST_CHECK(exported.find("a = AnimationBlock") != std::string::npos);
  TEST_CHECK(exported.find("b = None") != std::string::npos);
  TEST_CHECK(exported.find("c = 42") != std::string::npos);
}
