// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file threejs_mtlx_export_example.cc
/// @brief Example demonstrating MaterialX XML export from OpenPBR materials
///

#include <iostream>
#include <fstream>
#include "tydra/render-data.hh"
#include "tydra/threejs-exporter.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

int main(int argc, char** argv) {
  std::cout << "=== Three.js MaterialX Export Example ===" << std::endl;

  // Create a RenderMaterial with OpenPBR shader
  RenderMaterial material;
  material.name = "example_openpbr_material";
  material.abs_path = "/materials/example_openpbr_material";
  material.handle = 1;

  // Create and configure OpenPBR shader
  OpenPBRSurfaceShader openpbr;
  openpbr.handle = 100;

  // Base layer - metallic gold-like material
  openpbr.base_weight.value = 1.0f;
  openpbr.base_color.value = {0.944f, 0.776f, 0.373f}; // Gold color
  openpbr.base_roughness.value = 0.2f;
  openpbr.base_metalness.value = 1.0f;

  // Specular layer
  openpbr.specular_weight.value = 1.0f;
  openpbr.specular_color.value = {1.0f, 1.0f, 1.0f};
  openpbr.specular_roughness.value = 0.2f;
  openpbr.specular_ior.value = 1.5f;
  openpbr.specular_ior_level.value = 0.5f;
  openpbr.specular_anisotropy.value = 0.0f;
  openpbr.specular_rotation.value = 0.0f;

  // Coat layer - add clear coat
  openpbr.coat_weight.value = 0.5f;
  openpbr.coat_color.value = {1.0f, 1.0f, 1.0f};
  openpbr.coat_roughness.value = 0.1f;
  openpbr.coat_ior.value = 1.5f;
  openpbr.coat_anisotropy.value = 0.0f;
  openpbr.coat_rotation.value = 0.0f;
  openpbr.coat_affect_color.value = {1.0f, 1.0f, 1.0f};
  openpbr.coat_affect_roughness.value = 0.0f;

  // Emission - no emission
  openpbr.emission_luminance.value = 0.0f;
  openpbr.emission_color.value = {0.0f, 0.0f, 0.0f};

  // Geometry
  openpbr.opacity.value = 1.0f;
  openpbr.normal.value = {0.0f, 0.0f, 1.0f};
  openpbr.tangent.value = {1.0f, 0.0f, 0.0f};

  // Transmission - no transmission
  openpbr.transmission_weight.value = 0.0f;
  openpbr.transmission_color.value = {1.0f, 1.0f, 1.0f};
  openpbr.transmission_depth.value = 0.0f;
  openpbr.transmission_scatter.value = {0.0f, 0.0f, 0.0f};
  openpbr.transmission_scatter_anisotropy.value = 0.0f;
  openpbr.transmission_dispersion.value = 0.0f;

  // Subsurface - no subsurface
  openpbr.subsurface_weight.value = 0.0f;
  openpbr.subsurface_color.value = {0.8f, 0.8f, 0.8f};
  openpbr.subsurface_radius.value = {1.0f, 1.0f, 1.0f};
  openpbr.subsurface_scale.value = 1.0f;
  openpbr.subsurface_anisotropy.value = 0.0f;

  // Sheen - no sheen
  openpbr.sheen_weight.value = 0.0f;
  openpbr.sheen_color.value = {1.0f, 1.0f, 1.0f};
  openpbr.sheen_roughness.value = 0.3f;

  // Assign OpenPBR shader to material
  material.openPBRShader = openpbr;

  // Create exporter
  ThreeJSMaterialExporter exporter;

  // Export to MaterialX format
  std::string mtlx_output;
  bool success = exporter.ExportMaterialX(material, mtlx_output);

  if (success) {
    std::cout << "\n=== MaterialX Export Successful ===" << std::endl;
    std::cout << mtlx_output << std::endl;

    // Save to file
    std::string filename = "example_openpbr_material.mtlx";
    std::ofstream ofs(filename);
    if (ofs.is_open()) {
      ofs << mtlx_output;
      ofs.close();
      std::cout << "\n=== Saved to: " << filename << " ===" << std::endl;
    } else {
      std::cerr << "Failed to write file: " << filename << std::endl;
      return 1;
    }
  } else {
    std::cerr << "MaterialX export failed!" << std::endl;
    std::cerr << "Error: " << exporter.GetError() << std::endl;
    return 1;
  }

  std::cout << "\n=== Example Complete ===" << std::endl;
  return 0;
}
