// SPDX-License-Identifier: Apache 2.0
// USDC roundtrip test executable:
// Parse USDA file -> write USDC to memory -> re-parse USDC -> compare via JSON

#include <algorithm>
#include <cctype>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>

#include "lightusd.hh"
#include "usdc-writer.hh"
#include "usd-to-json.hh"

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "USDC roundtrip test: parse USDA -> write USDC -> re-parse USDC -> JSON compare\n" << std::endl;
    std::cout << "Usage: usdc_roundtrip input.usda [--verbose] [--dump-on-fail]\n" << std::endl;
    return EXIT_FAILURE;
  }

  bool verbose = false;
  bool dump_on_fail = false;

  for (int i = 2; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--dump-on-fail") {
      dump_on_fail = true;
    }
  }

  std::string filepath = argv[1];
  std::string ext = str_tolower(GetFileExtension(filepath));

  if (ext != "usda") {
    std::cerr << "Error: Only .usda files are supported, got: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  // Step 1: Load original USDA file
  lightusd::legacy::Stage stage1;
  std::string warn1, err1;

  bool ret1 = lightusd::legacy::LoadUSDAFromFile(filepath, &stage1, &warn1, &err1);
  if (!warn1.empty() && verbose) {
    std::cerr << "WARN (load1): " << warn1 << "\n";
  }
  if (!ret1) {
    std::cerr << "ERR (load1): " << err1 << "\n";
    std::cerr << "Failed to load USDA file: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 1: Loaded " << filepath << " OK\n";
  }

  // Step 2: Write to USDC memory buffer
  std::vector<uint8_t> usdc_data;
  std::string warn2, err2;

  bool ret2 = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage1, &usdc_data, &warn2, &err2);
  if (!warn2.empty() && verbose) {
    std::cerr << "WARN (write): " << warn2 << "\n";
  }
  if (!ret2) {
    std::cerr << "ERR (write): " << err2 << "\n";
    std::cerr << "Failed to write USDC for: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 2: Wrote USDC to memory (" << usdc_data.size() << " bytes)\n";
  }

  // Step 3: Re-parse the USDC data
  lightusd::legacy::Stage stage2;
  std::string warn3, err3;

  bool ret3 = lightusd::legacy::LoadUSDCFromMemory(
      usdc_data.data(),
      usdc_data.size(),
      "roundtrip.usdc",
      &stage2,
      &warn3,
      &err3);

  if (!warn3.empty() && verbose) {
    std::cerr << "WARN (load2): " << warn3 << "\n";
  }
  if (!ret3) {
    std::cerr << "ERR (load2): " << err3 << "\n";
    std::cerr << "Failed to re-parse USDC for: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 3: Re-parsed USDC OK\n";
  }

  // Step 4: Convert both stages to JSON and compare
#if defined(LIGHTUSD_WITH_JSON)
  lightusd::legacy::USDToJSONOptions options;

  auto json1_result = lightusd::legacy::ToJSON(stage1, options);
  if (!json1_result) {
    std::cerr << "Error: Failed to convert stage1 to JSON: " << json1_result.error() << "\n";
    return EXIT_FAILURE;
  }

  auto json2_result = lightusd::legacy::ToJSON(stage2, options);
  if (!json2_result) {
    std::cerr << "Error: Failed to convert stage2 to JSON: " << json2_result.error() << "\n";
    return EXIT_FAILURE;
  }

  std::string json1 = json1_result.value();
  std::string json2 = json2_result.value();

  if (json1 != json2) {
    std::cerr << "Error: JSON mismatch after USDC roundtrip for: " << filepath << "\n";
    if (dump_on_fail) {
      std::cerr << "--- Original JSON ---\n" << json1 << "\n";
      std::cerr << "--- Roundtrip JSON ---\n" << json2 << "\n";
    }
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 4: JSON comparison OK\n";
  }
#else
  // Without JSON support, just verify that re-parsing succeeds
  // and both stages have the same number of root prims
  if (stage1.root_prims().size() != stage2.root_prims().size()) {
    std::cerr << "Error: Root prim count mismatch for: " << filepath << "\n";
    std::cerr << "  Original: " << stage1.root_prims().size() << " prims\n";
    std::cerr << "  Roundtrip: " << stage2.root_prims().size() << " prims\n";
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 4: Root prim count comparison OK (JSON disabled)\n";
  }
#endif

  if (verbose) {
    std::cout << "USDC roundtrip OK: " << filepath << "\n";
  }

  return EXIT_SUCCESS;
}
