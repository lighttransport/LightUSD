// Test for grouped/flattened parameter export functionality

#include <iostream>
#include <string>
#include "tydra/render-data.hh"
#include "tydra/threejs-exporter.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

int main() {
  // Create a simple OpenPBR material
  RenderMaterial material;
  material.name = "TestMaterial";
  material.handle = 12345;

  // Create OpenPBR shader
  OpenPBRSurfaceShader shader;
  shader.handle = 67890;

  // Set some basic parameters
  shader.base_weight.value = 1.0f;
  shader.base_color.value = {0.8f, 0.2f, 0.1f};
  shader.base_roughness.value = 0.5f;
  shader.base_metalness.value = 0.0f;

  shader.specular_weight.value = 1.0f;
  shader.specular_color.value = {1.0f, 1.0f, 1.0f};
  shader.specular_ior.value = 1.5f;

  shader.emission_luminance.value = 0.0f;
  shader.emission_color.value = {0.0f, 0.0f, 0.0f};

  shader.coat_weight.value = 0.5f;
  shader.coat_color.value = {1.0f, 1.0f, 1.0f};
  shader.coat_roughness.value = 0.1f;

  shader.opacity.value = 1.0f;
  shader.normal.value = {0.0f, 0.0f, 1.0f};
  shader.tangent.value = {1.0f, 0.0f, 0.0f};

  material.openPBRShader = shader;

  // Create exporter
  ThreeJSMaterialExporter exporter;

  // Test 1: Flattened parameters (default)
  std::cout << "=== Test 1: Flattened Parameters (default) ===" << std::endl;
  {
    ThreeJSMaterialExporter::ExportOptions options;
    options.use_webgpu = true;
    options.use_grouped_parameters = false;  // Flattened

    json output;
    if (exporter.ExportMaterial(material, options, output)) {
      std::cout << "Flattened output:" << std::endl;
      std::cout << output.dump(2) << std::endl;

      // Check if base_color exists in flattened format
      if (output["nodes"]["surface"]["inputs"].contains("base_color")) {
        std::cout << "✓ Found 'base_color' in flattened format" << std::endl;
      } else {
        std::cout << "✗ 'base_color' NOT found in flattened format" << std::endl;
      }
    } else {
      std::cout << "Error: " << exporter.GetError() << std::endl;
    }
  }

  std::cout << "\n=== Test 2: Grouped Parameters ===" << std::endl;
  {
    ThreeJSMaterialExporter::ExportOptions options;
    options.use_webgpu = true;
    options.use_grouped_parameters = true;  // Grouped

    json output;
    if (exporter.ExportMaterial(material, options, output)) {
      std::cout << "Grouped output:" << std::endl;
      std::cout << output.dump(2) << std::endl;

      // Check if base.color exists in grouped format
      if (output["nodes"]["surface"]["inputs"].contains("base") &&
          output["nodes"]["surface"]["inputs"]["base"].contains("color")) {
        std::cout << "✓ Found 'base.color' in grouped format" << std::endl;
      } else {
        std::cout << "✗ 'base.color' NOT found in grouped format" << std::endl;
      }

      // Check if specular.weight exists
      if (output["nodes"]["surface"]["inputs"].contains("specular") &&
          output["nodes"]["surface"]["inputs"]["specular"].contains("weight")) {
        std::cout << "✓ Found 'specular.weight' in grouped format" << std::endl;
      } else {
        std::cout << "✗ 'specular.weight' NOT found in grouped format" << std::endl;
      }
    } else {
      std::cout << "Error: " << exporter.GetError() << std::endl;
    }
  }

  return 0;
}
