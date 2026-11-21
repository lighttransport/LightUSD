// SPDX-License-Identifier: Apache-2.0
// Core scene queries, shader ports, hierarchy, bindings and attribute
// evaluation.
#include <iostream>
#include <string>

#include "load-usd.hh"
#include "schema/usd-shade.hh"
#include "tydra/scene-access.hh"
#include "writer/value-printer.hh"

int main(int argc, char** argv) {
  using namespace lightusd;
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: tydra_api input.usda/usdc/usdz\n";
    return 0;
  }
  if (argc != 2) {
    std::cerr << "Usage: tydra_api input.usda/usdc/usdz\n";
    return 1;
  }
  Stage stage;
  std::string warn, err;
  const bool loaded = LoadUSDComposed(argv[1], &stage, &warn, &err);
  if (!warn.empty()) std::cerr << warn << '\n';
  if (!loaded) {
    std::cerr << err << '\n';
    return 1;
  }

  const auto meshes = tydra::FindMeshes(stage);
  const auto xforms = tydra::FindXforms(stage);
  const auto materials = tydra::FindMaterials(stage);
  const auto shaders = tydra::FindPrimsByType(stage, "Shader");
  std::cout << "Meshes: " << meshes.size() << ", Xforms: " << xforms.size()
            << ", Materials: " << materials.size()
            << ", Shaders: " << shaders.size() << '\n';
  PrintOptions print;
  print.max_array_elements = 16;
  AttributeEval eval(&stage);
  eval.SetDefaultTime();
  for (const auto& prim : meshes) {
    std::cout << "Mesh: " << prim.GetPath().str() << '\n';
    std::cout << "  directlyBoundMaterial: " << GetBoundMaterialPath(prim)
              << '\n';
    std::cout << "  boundMaterial: "
              << GetInheritedBoundMaterialPath(stage, prim.GetPath().str())
              << '\n';
    const auto parent = tydra::GetParent(stage, prim);
    if (parent) std::cout << "  parent: " << parent.GetPath().str() << '\n';
  }
  for (const auto& prim : xforms) {
    const auto transform = eval.Eval(prim, "xformOp:transform");
    if (transform.success)
      std::cout << prim.GetPath().str()
                << " xformOp:transform = " << PrintValue(transform.value, print)
                << '\n';
  }
  for (const auto& prim : materials) {
    std::cout << "Material: " << prim.GetPath().str()
              << " surface: " << GetSurfaceShader(stage, prim) << '\n';
  }
  for (const auto& prim : shaders) {
    std::cout << "Shader: " << prim.GetPath().str()
              << " id: " << GetShaderId(prim) << '\n';
    for (const auto& input : GetShaderInputs(stage, prim)) {
      std::cout << "  " << input.name << " = "
                << PrintValue(input.value, print);
      if (input.is_connected) std::cout << " <- " << input.connection_path;
      std::cout << '\n';
    }
    // AttributeEval follows connections and returns default/sample provenance.
    const auto varname = eval.Eval(prim, "inputs:varname");
    if (varname.success)
      std::cout << "  evaluated inputs:varname = "
                << PrintValue(varname.value, print) << '\n';
  }
  return 0;
}
