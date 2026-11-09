// SPDX-License-Identifier: Apache 2.0
// USDA roundtrip test executable:
// Parse USDA file, export to string, re-parse, compare via JSON

#include <algorithm>
#include <cctype>
#include <iostream>
#include <fstream>
#include <sstream>

#include "lightusd.hh"
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
    std::cout << "USDA roundtrip test: parse -> export -> re-parse -> JSON compare\n" << std::endl;
    std::cout << "Usage: usda_roundtrip input.usda [--verbose] [--dump-on-fail]\n" << std::endl;
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

  // Step 2: Export to string
  std::string exported = stage1.ExportToString();
  if (exported.empty()) {
    std::cerr << "Error: ExportToString returned empty string for: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 2: Exported to string (" << exported.size() << " bytes)\n";
  }

  // Step 3: Re-parse the exported string
  lightusd::legacy::Stage stage2;
  std::string warn2, err2;

  bool ret2 = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(exported.data()),
      exported.size(),
      "roundtrip.usda",
      &stage2,
      &warn2,
      &err2);

  if (!warn2.empty() && verbose) {
    std::cerr << "WARN (load2): " << warn2 << "\n";
  }
  if (!ret2) {
    std::cerr << "ERR (load2): " << err2 << "\n";
    std::cerr << "Failed to re-parse exported USDA for: " << filepath << "\n";
    if (dump_on_fail) {
      std::cerr << "--- Exported content ---\n" << exported << "\n--- End ---\n";
    }
    return EXIT_FAILURE;
  }

  if (verbose) {
    std::cout << "Step 3: Re-parsed exported string OK\n";
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
    std::cerr << "Error: JSON mismatch after roundtrip for: " << filepath << "\n";
    if (dump_on_fail) {
      std::cerr << "--- Original JSON ---\n" << json1 << "\n";
      std::cerr << "--- Roundtrip JSON ---\n" << json2 << "\n";
      std::cerr << "--- Exported USDA ---\n" << exported << "\n--- End ---\n";
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
    std::cout << "Roundtrip OK: " << filepath << "\n";
  }

  return EXIT_SUCCESS;
}
