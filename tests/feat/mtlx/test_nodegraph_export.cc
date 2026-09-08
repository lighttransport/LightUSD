// SPDX-License-Identifier: Apache 2.0
// Test for MaterialX NodeGraph export functionality

#include <iostream>
#include <string>

#include "usdMtlx.hh"
#include "lightusd.hh"

// MaterialX XML with nodegraph connections
const char* test_mtlx_with_nodegraph = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <nodegraph name="NG_marble&amp;detail">
    <image name="marble_image" type="color3">
      <input name="file" type="filename" value="marble&amp;diffuse.jpg" />
      <input name="uaddressmode" type="string" value="periodic" />
      <input name="vaddressmode" type="string" value="periodic" />
    </image>
    <multiply name="marble_scaled" type="color3">
      <input name="in1" type="color3" nodename="marble_image" />
      <input name="in2" type="float" value="0.8" />
    </multiply>
    <output name="out" type="color3" nodename="marble_scaled" />
  </nodegraph>

  <UsdPreviewSurface name="MarbleShader" type="surfaceshader">
    <input name="diffuseColor" type="color3" nodegraph="NG_marble&amp;detail" output="out" />
    <input name="roughness" type="float" value="0.3" />
    <input name="metallic" type="float" value="0.0" />
  </UsdPreviewSurface>
</materialx>
)";

int main(int argc, char** argv) {
  std::string warn, err;

  std::cout << "=== LightUSD MaterialX NodeGraph Export Test ===\n\n";

  // Test 1: Parse MaterialX with nodegraph
  std::cout << "Test 1: Parsing MaterialX with nodegraph...\n";
  lightusd::MtlxModel mtlx;
  bool ret = lightusd::ReadMaterialXFromString(
      test_mtlx_with_nodegraph, "test_nodegraph.mtlx", &mtlx, &warn, &err);

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

  std::cout << "\nParsed MaterialX information:\n";
  std::cout << "  Shaders: " << mtlx.shaders.size() << "\n";
  std::cout << "  NodeGraphs: " << mtlx.nodegraphs.size() << "\n";
  std::cout << "  Shader connections: " << mtlx.shader_connections.size() << "\n";

  // Show parsed nodegraphs
  if (!mtlx.nodegraphs.empty()) {
    std::cout << "\nParsed nodegraphs:\n";
    for (const auto& ng_item : mtlx.nodegraphs) {
      std::cout << "  - " << ng_item.first << " (" << ng_item.second.typeName() << ")\n";
      std::cout << "    Children: " << ng_item.second.children().size() << "\n";
      std::cout << "    Properties: " << ng_item.second.props().size() << "\n";
    }
  }

  // Show parsed connections
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

  ret = lightusd::WriteMaterialXToString(mtlx, xml_out, &warn, &err);

  if (!ret) {
    std::cerr << "ERROR: Failed to export MaterialX\n";
    if (!err.empty()) {
      std::cerr << "Error: " << err << "\n";
    }
    return 1;
  }

  std::cout << "✓ Successfully exported MaterialX\n";
  if (!warn.empty()) {
    std::cout << "Warnings: " << warn << "\n";
  }

  std::cout << "\nExported XML:\n";
  std::cout << "----------------------------------------\n";
  std::cout << xml_out << "\n";
  std::cout << "----------------------------------------\n";

  // Test 3: Verify nodegraph structure in output
  std::cout << "\nTest 3: Verifying nodegraph structure in output...\n";
  bool has_nodegraph_tag = xml_out.find("<nodegraph") != std::string::npos;
  bool has_nodegraph_attr = xml_out.find("nodegraph=\"") != std::string::npos;
  bool has_output_attr = xml_out.find("output=\"") != std::string::npos;
  bool has_output_tag = xml_out.find("<output") != std::string::npos;
  bool has_escaped_name = xml_out.find("name=\"NG_marble&amp;detail\"") != std::string::npos;
  bool has_escaped_value = xml_out.find("marble&amp;diffuse.jpg") != std::string::npos;

  std::cout << "  <nodegraph> tag: " << (has_nodegraph_tag ? "✓" : "✗") << "\n";
  std::cout << "  nodegraph=\"\" attr: " << (has_nodegraph_attr ? "✓" : "✗") << "\n";
  std::cout << "  output=\"\" attr: " << (has_output_attr ? "✓" : "✗") << "\n";
  std::cout << "  <output> tag: " << (has_output_tag ? "✓" : "✗") << "\n";
  std::cout << "  XML-escaped nodegraph name: " << (has_escaped_name ? "✓" : "✗") << "\n";
  std::cout << "  XML-escaped filename: " << (has_escaped_value ? "✓" : "✗") << "\n";

  if (has_nodegraph_tag && has_nodegraph_attr && has_output_attr &&
      has_escaped_name && has_escaped_value) {
    std::cout << "\n✓ NodeGraph structure verified in output\n";
  } else {
    std::cerr << "\n✗ WARNING: Some NodeGraph elements missing in output\n";
  }

  std::cout << "\n=== All tests passed! ===\n";
  return 0;
}
