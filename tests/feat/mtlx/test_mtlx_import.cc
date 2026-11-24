// SPDX-License-Identifier: Apache 2.0
// Test for MaterialX import functionality

#include <iostream>
#include <string>

#include "usdMtlx.hh"
#include "lightusd.hh"
#include "value-pprint.hh"

// Simple test MaterialX XML
const char* test_openpbr_mtlx = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <surfacematerial name="TestMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="TestMaterial_shader" />
  </surfacematerial>

  <open_pbr_surface name="TestMaterial_shader" type="surfaceshader">
    <input name="base_color" type="color3" value="0.8, 0.2, 0.2" />
    <input name="base_weight" type="float" value="1.0" />
    <input name="base_metalness" type="float" value="0.8" />
    <input name="specular_roughness" type="float" value="0.3" />
    <input name="specular_ior" type="float" value="1.5" />
    <input name="coat_weight" type="float" value="0.5" />
    <input name="coat_roughness" type="float" value="0.1" />
    <input name="emission_color" type="color3" value="0.0, 0.0, 0.0" />
    <input name="emission_luminance" type="float" value="0.0" />
    <input name="geometry_opacity" type="float" value="1.0" />
  </open_pbr_surface>
</materialx>
)";

int main(int argc, char** argv) {
  std::string warn, err;

  std::cout << "=== LightUSD MaterialX Import Test ===\n\n";

  // Test 1: Parse MaterialX from string
  std::cout << "Test 1: Parsing MaterialX XML from string...\n";
  lightusd::legacy::MtlxModel mtlx;
  bool ret = lightusd::legacy::ReadMaterialXFromString(
      test_openpbr_mtlx, "test.mtlx", &mtlx, &warn, &err);

  if (!ret) {
    std::cerr << "ERROR: Failed to parse MaterialX\n";
    if (!err.empty()) {
      std::cerr << "Error: " << err << "\n";
    }
    return 1;
  }

  std::cout << "✓ Successfully parsed MaterialX\n";

  if (!warn.empty()) {
    std::cout << "Warnings: " << warn << "\n";
  }

  // Print parsed information
  std::cout << "\nParsed MaterialX information:\n";
  std::cout << "  Asset name: " << mtlx.asset_name << "\n";
  std::cout << "  Version: " << mtlx.version << "\n";
  std::cout << "  Shader name: " << mtlx.shader_name << "\n";
  std::cout << "  Surface materials: " << mtlx.surface_materials.size() << "\n";
  std::cout << "  Shaders: " << mtlx.shaders.size() << "\n\n";

  // Test 2: Convert to PrimSpec
  std::cout << "Test 2: Converting MaterialX to USD PrimSpec...\n";
  lightusd::legacy::PrimSpec ps;
  ret = lightusd::legacy::ToPrimSpec(mtlx, ps, &err);

  if (!ret) {
    std::cerr << "ERROR: Failed to convert MaterialX to PrimSpec\n";
    if (!err.empty()) {
      std::cerr << "Error: " << err << "\n";
    }
    return 1;
  }

  std::cout << "✓ Successfully converted to PrimSpec\n";
  std::cout << "  PrimSpec name: " << ps.name() << "\n";
  std::cout << "  PrimSpec type: " << ps.typeName() << "\n\n";

  // Test 3: Load from file (if provided)
  if (argc > 1) {
    std::cout << "Test 3: Loading MaterialX from file: " << argv[1] << "\n";

    lightusd::legacy::AssetResolutionResolver resolver;
    lightusd::legacy::MtlxModel mtlx_file;
    warn.clear();
    err.clear();

    ret = lightusd::legacy::ReadMaterialXFromFile(
        resolver, argv[1], &mtlx_file, &warn, &err);

    if (!ret) {
      std::cerr << "ERROR: Failed to load MaterialX from file\n";
      if (!err.empty()) {
        std::cerr << "Error: " << err << "\n";
      }
      return 1;
    }

    std::cout << "✓ Successfully loaded MaterialX from file\n";
    std::cout << "  Asset name: " << mtlx_file.asset_name << "\n";
    std::cout << "  Version: " << mtlx_file.version << "\n";
    std::cout << "  Shaders: " << mtlx_file.shaders.size() << "\n\n";
  }

  std::cout << "=== All tests passed! ===\n";
  return 0;
}
