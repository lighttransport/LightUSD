// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include <iostream>

int main(int argc, char** argv) {
  namespace usd = lightusd::next;
  if (argc != 3) { std::cerr << "Usage: next_quickstart INPUT OUTPUT.usda|OUTPUT.usdc\n"; return 2; }
  usd::Stage stage;
  usd::LoadUSDOptions options;
  options.limits.max_resident_bytes = 256 * 1024 * 1024;
  std::string warning, error;
  if (!usd::LoadUSDComposed(argv[1], &stage, options, &warning, &error)) {
    std::cerr << error << '\n'; return 1;
  }
  if (!warning.empty()) std::cerr << warning << '\n';
  usd::AttributeEval eval(&stage);
  eval.SetDefaultTime();
  stage.Traverse([&](const usd::UsdPrim& prim) {
    std::cout << prim.GetPath().str() << " " << prim.GetTypeName();
    if (auto radius = eval.EvalDouble(prim, "radius")) std::cout << " radius=" << *radius;
    std::cout << '\n'; return true;
  });
  const std::string output(argv[2]);
  const bool ascii = output.size() >= 5 && output.substr(output.size() - 5) == ".usda";
  if (!(ascii ? usd::WriteUSDA(stage, output, &error) : usd::WriteUSDC(stage, output, &error))) { std::cerr << error << '\n'; return 1; }
  usd::Stage reopened;
  if (!usd::LoadUSD(argv[2], &reopened, options, &warning, &error)) {
    std::cerr << error << '\n'; return 1;
  }
  std::cout << "Reopened " << argv[2] << '\n';
  return 0;
}
