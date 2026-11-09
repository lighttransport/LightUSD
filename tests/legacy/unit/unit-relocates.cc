// SPDX-License-Identifier: Apache 2.0
// Unit tests for SdfRelocates (Crate value type 58, crate >= 0.11.0).

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-relocates.h"
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

static const char *kRelocatesUSDA =
    "#usda 1.0\n"
    "(\n"
    "    relocates = {\n"
    "        </World/source> : </World/target>,\n"
    "        </World/old/deep> : </World/new>,\n"
    "        </World/dropme> : <>\n"
    "    }\n"
    ")\n"
    "\n"
    "def Xform \"World\"\n"
    "{\n"
    "    def Scope \"target\"\n"
    "    {\n"
    "    }\n"
    "}\n";

// Verify the three authored pairs, including the deletion relocate (empty
// target).
static void CheckRelocates(const Stage &stage) {
  const auto &rel = stage.metas().layerRelocates;
  TEST_CHECK_(rel.size() == 3, "expected 3 relocates, got %zu", rel.size());
  if (rel.size() != 3) return;

  TEST_CHECK(rel[0].first.full_path_name() == "/World/source");
  TEST_CHECK(rel[0].second.full_path_name() == "/World/target");

  TEST_CHECK(rel[1].first.full_path_name() == "/World/old/deep");
  TEST_CHECK(rel[1].second.full_path_name() == "/World/new");

  TEST_CHECK(rel[2].first.full_path_name() == "/World/dropme");
  // Deletion relocate: the target is the empty (invalid) path.
  TEST_CHECK_(!rel[2].second.is_valid(),
              "expected empty target for deletion relocate");
}

void relocates_usda_parse_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kRelocatesUSDA, &stage));
  CheckRelocates(stage);
}

void relocates_usda_roundtrip_test(void) {
  // Parse -> export to USDA -> re-parse; relocates (incl. `<>`) must survive.
  Stage stage;
  TEST_CHECK(LoadUSDA(kRelocatesUSDA, &stage));

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("relocates = {") != std::string::npos);
  // The deletion relocate must serialize as `<>`, not an invalid-path marker.
  TEST_CHECK(exported.find("<>") != std::string::npos);
  TEST_CHECK(exported.find("#INVALID#") == std::string::npos);

  Stage stage2;
  TEST_CHECK(LoadUSDA(exported, &stage2));
  CheckRelocates(stage2);
}

void relocates_crate_roundtrip_test(void) {
  // Parse USDA -> save USDC (crate-58 write) -> reload USDC (crate-58 read).
  Stage stage;
  TEST_CHECK(LoadUSDA(kRelocatesUSDA, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;
  TEST_CHECK(!usdc.empty());

  // Boot header byte 9 = version minor. SdfRelocates must bump the emitted
  // crate version to >= 0.11.0.
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 11, "crate version minor = %d, expected >= 11",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = lightusd::legacy::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  CheckRelocates(stage2);
}
