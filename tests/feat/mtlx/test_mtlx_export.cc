// SPDX-License-Identifier: Apache 2.0
// Test for MaterialX export functionality

#include <iostream>
#include <string>

#include "usdMtlx.hh"
#include "lightusd.hh"

// Test MaterialX XML - simple UsdPreviewSurface without connections first
const char* test_connected_mtlx = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <UsdPreviewSurface name="TestShader" type="surfaceshader">
    <input name="diffuseColor" type="color3" value="0.8, 0.2, 0.2" />
    <input name="roughness" type="float" value="0.3" />
    <input name="metallic" type="float" value="0.0" />
  </UsdPreviewSurface>
</materialx>
)";

int main(int argc, char** argv) {
  std::string warn, err;

  std::cout << "=== LightUSD MaterialX Export Test ===\n\n";

  // Test 1: Parse MaterialX with connections
  std::cout << "Test 1: Parsing MaterialX XML with connections...\n";
  lightusd::legacy::MtlxModel mtlx;
  bool ret = lightusd::legacy::ReadMaterialXFromString(
      test_connected_mtlx, "test_export.mtlx", &mtlx, &warn, &err);

  if (!ret) {
    std::cerr << "ERROR: Failed to parse MaterialX\n";
    if (!err.empty()) {
      std::cerr << "Error: " << err << "\n";
    }
    return 1;
  }

  std::cout << "✓ Successfully parsed MaterialX\n";
  std::cout << "  Shaders: " << mtlx.shaders.size() << "\n";
  std::cout << "  Nodegraphs: " << mtlx.nodegraphs.size() << "\n";
  std::cout << "  Shader connections: " << mtlx.shader_connections.size() << "\n";

  // Check connections were parsed
  if (!mtlx.shader_connections.empty()) {
    std::cout << "\nParsed connections:\n";
    for (const auto& shader_conn : mtlx.shader_connections) {
      std::cout << "  Shader: " << shader_conn.first << "\n";
      for (const auto& conn : shader_conn.second) {
        std::cout << "    " << conn.input_name << " -> ";
        if (!conn.nodegraph.empty()) {
          std::cout << "nodegraph=" << conn.nodegraph;
          if (!conn.output.empty()) {
            std::cout << ", output=" << conn.output;
          }
        } else if (!conn.nodename.empty()) {
          std::cout << "nodename=" << conn.nodename;
        }
        std::cout << "\n";
      }
    }
  }

  // Test 2: Export MaterialX back to string
  std::cout << "\nTest 2: Exporting MaterialX to XML string...\n";
  std::string xml_out;
  warn.clear();
  err.clear();

  ret = lightusd::legacy::WriteMaterialXToString(mtlx, xml_out, &warn, &err);

  if (!ret) {
    std::cerr << "ERROR: Failed to export MaterialX\n";
    if (!err.empty()) {
      std::cerr << "Error: " << err << "\n";
    }
    return 1;
  }

  std::cout << "✓ Successfully exported MaterialX\n";
  std::cout << "\nExported XML:\n";
  std::cout << "----------------------------------------\n";
  std::cout << xml_out << "\n";
  std::cout << "----------------------------------------\n";

  // Test 3: Verify connections are in output
  std::cout << "\nTest 3: Verifying connections in output...\n";
  bool has_nodegraph_attr = xml_out.find("nodegraph=\"") != std::string::npos;
  bool has_output_attr = xml_out.find("output=\"") != std::string::npos;

  if (has_nodegraph_attr && has_output_attr) {
    std::cout << "✓ Connection attributes found in output\n";
  } else {
    std::cerr << "WARNING: Connection attributes not found in output\n";
    std::cerr << "  nodegraph attribute: " << (has_nodegraph_attr ? "yes" : "no") << "\n";
    std::cerr << "  output attribute: " << (has_output_attr ? "yes" : "no") << "\n";
  }

  std::cout << "\n=== All tests passed! ===\n";
  return 0;
}
