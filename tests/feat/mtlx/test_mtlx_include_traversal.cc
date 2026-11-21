// SPDX-License-Identifier: Apache 2.0
//
// Security regression test for MaterialX <include> path traversal.
//
// ProcessIncludes() in src/usdMtlx.cc resolves the attacker-controlled
// `filename` attribute of <include filename="..."/> against the document's
// base directory. Previously the filename was used verbatim, so an absolute
// path ("/etc/passwd") or a "../" traversal escaped the base directory and
// allowed reading arbitrary local files (path traversal / arbitrary file
// read). The fix routes the include filename through
// security_policy::ValidateAndNormalizeAssetPath(), which rejects absolute
// paths, Windows drive letters and any ".." segment.
//
// This test confirms:
//   1. A "../" traversal include is rejected.
//   2. An absolute-path include is rejected.
//   3. A legitimate, contained relative include is NOT rejected and still
//      loads normally.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#define LIGHTUSD_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define LIGHTUSD_MKDIR(p) mkdir((p), 0755)
#endif

#include "asset-resolution.hh"
#include "lightusd.hh"  // for the complete lightusd::legacy::PrimSpec definition
#include "usdMtlx.hh"

namespace {

// Message fragment emitted by the path-validation guard in ProcessIncludes().
const char *kRejectMsg = "safe relative path";

const char *kTmpRoot = "mtlx_traversal_tmp";
const char *kBaseDir = "mtlx_traversal_tmp/base";

bool WriteFile(const std::string &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    std::cerr << "  Failed to create file: " << path << "\n";
    return false;
  }
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  return ofs.good();
}

bool Contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// Minimal valid MaterialX fragment used for the benign-include case.
const char *kIncludeFragment = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <surfacematerial name="TestMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="TestMaterial_shader" />
  </surfacematerial>
  <open_pbr_surface name="TestMaterial_shader" type="surfaceshader">
    <input name="base_color" type="color3" value="0.8, 0.2, 0.2" />
    <input name="base_weight" type="float" value="1.0" />
  </open_pbr_surface>
</materialx>
)";

bool Setup() {
  LIGHTUSD_MKDIR(kTmpRoot);
  if (LIGHTUSD_MKDIR(kBaseDir) != 0) {
    // EEXIST from a previous run is fine; anything else we still try to write.
  }

  // A "secret" file living OUTSIDE the base dir (in the parent), the target an
  // attacker would try to reach via "../".
  if (!WriteFile(std::string(kTmpRoot) + "/secret_outside.txt",
                 "TOP-SECRET-MARKER\n")) {
    return false;
  }

  // Malicious: parent-directory traversal.
  if (!WriteFile(std::string(kBaseDir) + "/main_traversal.mtlx",
                 R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="../secret_outside.txt"/>
</materialx>
)")) {
    return false;
  }

  // Malicious: absolute path.
  if (!WriteFile(std::string(kBaseDir) + "/main_absolute.mtlx",
                 R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="/etc/passwd"/>
</materialx>
)")) {
    return false;
  }

  // Benign: contained relative include in the same directory.
  if (!WriteFile(std::string(kBaseDir) + "/inc_ok.mtlx", kIncludeFragment)) {
    return false;
  }
  if (!WriteFile(std::string(kBaseDir) + "/main_ok.mtlx",
                 R"(<?xml version="1.0"?>
<materialx version="1.38">
  <include filename="inc_ok.mtlx"/>
</materialx>
)")) {
    return false;
  }

  return true;
}

} // namespace

int main() {
  std::cout << "=== MaterialX <include> path-traversal regression test ===\n\n";

  if (!Setup()) {
    std::cerr << "FATAL: test fixture setup failed\n";
    return 1;
  }

  lightusd::legacy::AssetResolutionResolver resolver;
  resolver.set_search_paths({kBaseDir});

  int failures = 0;

  // --- Test 1: "../" traversal must be rejected -------------------------
  {
    std::cout << "Test 1: parent-directory ('../') include is rejected...\n";
    lightusd::legacy::MtlxModel mtlx;
    std::string warn, err;
    bool ret = lightusd::legacy::ReadMaterialXFromFile(resolver, "main_traversal.mtlx",
                                               &mtlx, &warn, &err);
    if (ret) {
      std::cerr << "  FAIL: traversal include was accepted (expected "
                   "rejection)\n";
      failures++;
    } else if (!Contains(err, kRejectMsg)) {
      std::cerr << "  FAIL: rejected, but not by the path validator. err=\""
                << err << "\"\n";
      failures++;
    } else {
      std::cout << "  PASS: rejected (" << err << ")\n";
    }
  }

  // --- Test 2: absolute path must be rejected ---------------------------
  {
    std::cout << "Test 2: absolute-path include is rejected...\n";
    lightusd::legacy::MtlxModel mtlx;
    std::string warn, err;
    bool ret = lightusd::legacy::ReadMaterialXFromFile(resolver, "main_absolute.mtlx",
                                               &mtlx, &warn, &err);
    if (ret) {
      std::cerr << "  FAIL: absolute-path include was accepted (expected "
                   "rejection)\n";
      failures++;
    } else if (!Contains(err, kRejectMsg)) {
      std::cerr << "  FAIL: rejected, but not by the path validator. err=\""
                << err << "\"\n";
      failures++;
    } else {
      std::cout << "  PASS: rejected (" << err << ")\n";
    }
  }

  // --- Test 3: benign contained relative include still works ------------
  {
    std::cout << "Test 3: contained relative include still loads...\n";
    lightusd::legacy::MtlxModel mtlx;
    std::string warn, err;
    bool ret = lightusd::legacy::ReadMaterialXFromFile(resolver, "main_ok.mtlx", &mtlx,
                                               &warn, &err);
    if (Contains(err, kRejectMsg)) {
      std::cerr << "  FAIL: legitimate relative include was wrongly rejected "
                   "as traversal. err=\""
                << err << "\"\n";
      failures++;
    } else if (!ret) {
      std::cerr << "  FAIL: legitimate include did not load. err=\"" << err
                << "\"\n";
      failures++;
    } else {
      std::cout << "  PASS: loaded (shaders=" << mtlx.shaders.size() << ")\n";
    }
  }

  std::cout << "\n";
  if (failures != 0) {
    std::cout << "RESULT: FAILED (" << failures << " check(s))\n";
    return 1;
  }
  std::cout << "RESULT: PASSED\n";
  return 0;
}
