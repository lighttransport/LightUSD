// Test the usdMtlx adapter

#include "../include/mtlx-usd-adapter.hh"
#include <iostream>
#include <cassert>

int main() {
  const char* xml = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <standard_surface name="SR_default" type="surfaceshader">
    <input name="base" type="float" value="1.0"/>
    <input name="base_color" type="color3" value="0.8, 0.8, 0.8"/>
  </standard_surface>

  <surfacematerial name="M_default" type="material">
    <shaderref name="surfaceshader" node="SR_default"/>
  </surfacematerial>
</materialx>)";

  // Test using pugixml-like API
  lightusd::legacy::mtlx::pugi::xml_document doc;
  lightusd::legacy::mtlx::pugi::xml_parse_result result = doc.load_string(xml);

  if (!result) {
    std::cerr << "Parse failed: " << result.description() << std::endl;
    return 1;
  }

  lightusd::legacy::mtlx::pugi::xml_node root = doc.child("materialx");
  if (!root) {
    std::cerr << "Root not found" << std::endl;
    return 1;
  }

  // Test attributes
  lightusd::legacy::mtlx::pugi::xml_attribute ver_attr = root.attribute("version");
  if (!ver_attr) {
    std::cerr << "Version attribute not found" << std::endl;
    return 1;
  }

  std::cout << "Version: " << ver_attr.as_string() << std::endl;
  assert(std::string(ver_attr.as_string()) == "1.38");

  // Test child access
  lightusd::legacy::mtlx::pugi::xml_node shader = root.child("standard_surface");
  if (!shader) {
    std::cerr << "Shader not found" << std::endl;
    return 1;
  }

  std::cout << "Shader name: " << shader.attribute("name").as_string() << std::endl;
  assert(std::string(shader.attribute("name").as_string()) == "SR_default");

  // Test iteration
  std::cout << "\nInputs:" << std::endl;
  for (lightusd::legacy::mtlx::pugi::xml_node input : shader) {
    if (std::string(input.name()) == "input") {
      std::cout << "  " << input.attribute("name").as_string()
                << " = " << input.attribute("value").as_string() << std::endl;
    }
  }

  // Test material
  lightusd::legacy::mtlx::pugi::xml_node material = root.child("surfacematerial");
  if (!material) {
    std::cerr << "Material not found" << std::endl;
    return 1;
  }

  lightusd::legacy::mtlx::pugi::xml_node shaderref = material.child("shaderref");
  if (!shaderref) {
    std::cerr << "Shaderref not found" << std::endl;
    return 1;
  }

  std::cout << "\nMaterial " << material.attribute("name").as_string()
            << " references shader: " << shaderref.attribute("node").as_string() << std::endl;

  std::cout << "\nAll tests passed!" << std::endl;

  return 0;
}