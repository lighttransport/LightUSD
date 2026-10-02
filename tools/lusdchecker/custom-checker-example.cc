// SPDX-License-Identifier: Apache-2.0
// Build-time extension example: no plugin loader or OpenUSD dependency.
#include "checker.hh"
int main(int argc, char** argv) {
  using namespace lightusd::next;
  ValidationRegistry registry;
  ValidatorDefinition rule;
  rule.id = "example.pipeline.tag";
  rule.description = "Require a pipeline tag on Sphere prims";
  rule.keywords = {"ExamplePipeline"};
  rule.schema_types = {"Sphere"};
  rule.scope = ValidationScope::Prim;
  rule.metadata.category = "recommendation";
  rule.callback = [](const ValidationContext&, const PrimSpec* prim)
      -> nonstd::expected<USDValidationResult, std::string> {
    USDValidationResult result;
    result.checked_groups.core = false;
    if (!prim->property("pipeline:tag"))
      result.issues.push_back({USDValidationSeverity::Error, "example.pipeline.tag",
                              prim->path().str(), "Sphere needs a pipeline:tag attribute"});
    return result;
  };
  if (!registry.RegisterValidator(std::move(rule))) return 2;
  return lusdchecker::RunChecker(argc, argv, registry);
}
