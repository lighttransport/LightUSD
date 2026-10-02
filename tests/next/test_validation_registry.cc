// SPDX-License-Identifier: Apache-2.0
#include "next/validation/validation-context.hh"
#include "next/pcp/layer-registry.hh"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace lightusd::next;
int main() {
  ValidationRegistry a, b;
  assert(!a.FindSchema("VendorSchema"));
  const char* schema = R"({"formatVersion":1,"schemas":[{"name":"VendorSchema","kind":"concrete","inherits":"Sphere","properties":[{"name":"weights","type":"float[]","kind":"attribute"}]}]})";
  auto loaded = a.LoadSchemaDefinitions(schema);
  if (!loaded) std::cerr << loaded.error() << '\n';
  assert(loaded);
  assert(a.InheritsFrom("VendorSchema", "Imageable"));
  assert(!b.FindSchema("VendorSchema"));
  assert(!GetBuiltinValidationRegistry().FindSchema("VendorSchema"));
  assert(a.LoadSchemaDefinitions(schema)); // identical imports are idempotent
  assert(!a.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[{"name":"VendorSchema","kind":"concrete","inherits":"Mesh","properties":[]}]})"));
  assert(a.InheritsFrom("VendorSchema", "Sphere"));
  assert(!a.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[{"name":"Good","kind":"concrete","properties":[]},{"name":"Cycle","kind":"concrete","inherits":"Cycle","properties":[]}]})"));
  assert(!a.FindSchema("Good")); // failed loads are transactional
  assert(a.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[{"name":"VendorAPI","kind":"multipleApply","propertyNamespacePrefix":"vendor","properties":[{"name":"weight","kind":"attribute","type":"double"}]}]})"));
  PrimSpec prim("P", "VendorSchema");
  prim.meta().apiSchemas().push_back("VendorAPI:left");
  assert(a.FindProperty(prim, "radius")->type == "double");
  assert(a.FindProperty(prim, "weights")->type == "float[]");
  assert(a.FindProperty(prim, "vendor:left:weight")->type == "double");
  assert(!a.FindProperty(prim, "vendor:right:weight"));
  assert(a.LoadShaderDefinitions(R"({"formatVersion":1,"shaders":[{"identifier":"VendorNode","sourceType":"test","inputs":{"value":"float"},"outputs":{"out":"float"}}]})"));
  assert(a.FindShader("VendorNode"));
  assert(!b.FindShader("VendorNode"));
  assert(!a.LoadShaderDefinitions(R"({"formatVersion":1,"shaders":[{"identifier":"VendorNode","sourceType":"test","inputs":{"value":"double"},"outputs":{"out":"float"}}]})"));
  assert(a.FindShader("VendorNode")->inputs.at("value") == "float");
  size_t calls = 0;
  ValidatorDefinition rule;
  rule.id = "test.callback"; rule.keywords = {"Test"}; rule.schema_types = {"Sphere"};
  rule.callback = [&](const ValidationContext&, const PrimSpec*) -> nonstd::expected<USDValidationResult, std::string> {
    ++calls;
    return nonstd::make_unexpected(std::string("required service unavailable"));
  };
  assert(a.RegisterValidator(rule));
  assert(!a.RegisterValidator(rule));
  ValidationContext context;
  context.options.validator_keywords = {"Other"};
  USDValidationResult result;
  a.RunCallbacks(ValidationScope::Prim, context, &prim, &result);
  assert(calls == 0 && result.complete);
  context.options.validator_keywords = {"Test"};
  a.RunCallbacks(ValidationScope::Prim, context, &prim, &result);
  assert(calls == 1 && !result.complete && !result.ok());
  USDValidationResult incomplete;
  incomplete.complete = false;
  assert(!incomplete.ok());
  assert(!a.LoadSchemaDefinitions(R"({"formatVersion":1.5,"schemas":[]})"));
  assert(!a.LoadShaderDefinitions(R"({"formatVersion":1,"shaders":[{"identifier":"Bad","sourceType":5,"inputs":{},"outputs":{}}]})"));
  USDValidationResult merged;
  MergeValidationResults(&merged, result);
  assert(!merged.complete);
  assert(GetValidationRuleMetadata("core.layer.defaultPrim.missing").category == "recommendation");
  assert(GetValidationRuleMetadata("core.schema.attributeType").category == "schema");
  assert(GetValidationRuleMetadata("core.prim.name").category == "normative");
  const char* animated = "#usda 1.0\ndef Sphere \"S\" {\n double radius.timeSamples = {1: 1, 2: 2}\n}\n";
  std::string warning, error;
  auto layer = pcp::LoadLayerFromMemory("sample.usda", reinterpret_cast<const uint8_t*>(animated),
                                      std::strlen(animated), &warning, &error);
  assert(layer);
  Stage stage;
  stage.SetRootLayer(layer->Clone());
  size_t samples = 0;
  ValidatorDefinition sampled;
  sampled.id = "test.sampled";
  sampled.schema_types = {"Sphere"};
  sampled.callback = [&](const ValidationContext& c, const PrimSpec* p) -> nonstd::expected<USDValidationResult, std::string> {
    assert(c.stage && c.layer && c.time.is_numeric());
    const auto* radius = p->property_value("radius");
    assert(radius && radius->as_double() && *radius->as_double() == c.time.numeric_time());
    ++samples;
    return USDValidationResult();
  };
  assert(b.RegisterValidator(sampled));
  ValidationContext sampled_context;
  sampled_context.layer = stage.GetRootLayer(); sampled_context.stage = &stage;
  sampled_context.options.registry = &b;
  const auto sample_result = ValidateStageSamples(stage, sampled_context);
  assert(sample_result.complete && samples == 2);
  std::cout << "validation registry contracts passed\n";
}
