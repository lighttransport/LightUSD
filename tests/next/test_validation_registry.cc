// SPDX-License-Identifier: Apache-2.0
#include "validation/validation-context.hh"
#include "../../src/pcp/layer-registry.hh"
#include "schema/schema-registry.hh"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace lightusd::next;
// AOUSD Core 1.1 "Schema Families and Versioning" (table rows + edges).
static void TestSchemaFamilies() {
  struct Case { const char* name; const char* family; uint32_t version; bool versioned; };
  const Case cases[] = {
      {"Mesh", "Mesh", 0, false},
      {"Cylinder_1", "Cylinder", 1, true},
      {"GaussianSplatting_2", "GaussianSplatting", 2, true},
      {"Gaussian_Splatting", "Gaussian_Splatting", 0, false},
      {"Gaussian_Splatting_3", "Gaussian_Splatting", 3, true},
      {"MySchema_", "MySchema_", 0, false},
      // Edges the table does not list (OpenUSD agrees unless noted).
      {"Foo_01", "Foo", 1, true},
      {"Foo__1", "Foo_", 1, true},
      {"Foo_1_2", "Foo_1", 2, true},
      {"Foo_1a", "Foo_1a", 0, false},
      {"A_99999999999", "A", 4294967295u, true},
      {"_1", "_1", 0, false},  // grammar needs a family; pxr says ('', 1)
  };
  for (const Case& c : cases) {
    std::string family;
    uint32_t version = 7;
    bool versioned = true;
    assert(ParseSchemaFamilyAndVersion(c.name, &family, &version, &versioned));
    if (family != c.family || version != c.version || versioned != c.versioned) {
      std::cerr << "schema family mismatch for " << c.name << ": " << family
                << " " << version << " " << versioned << "\n";
      assert(false);
    }
  }
  std::string f;
  uint32_t v = 0;
  bool hv = false;
  assert(!ParseSchemaFamilyAndVersion("", &f, &v, &hv));

  // A versioned schema must not inherit from its own family, directly or
  // through an intermediate schema; another family is fine.
  ValidationRegistry r;
  assert(!r.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[
      {"name":"Cyl","kind":"concrete","inherits":"Gprim","properties":[]},
      {"name":"Cyl_2","kind":"concrete","inherits":"Cyl","properties":[]}]})"));
  assert(!r.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[
      {"name":"Cyl_1","kind":"concrete","inherits":"Gprim","properties":[]},
      {"name":"Mid","kind":"concrete","inherits":"Cyl_1","properties":[]},
      {"name":"Cyl_3","kind":"concrete","inherits":"Mid","properties":[]}]})"));
  assert(r.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[
      {"name":"Cyl_1","kind":"concrete","inherits":"Gprim","properties":[]},
      {"name":"Cyl_2","kind":"concrete","inherits":"Gprim","properties":[]},
      {"name":"Cyl","kind":"concrete","inherits":"Cyl_1","properties":[]}]})"));
}

int main() {
  TestSchemaFamilies();
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
  assert(a.FindShader("VendorNode")->input_type("value") == "float");
  size_t calls = 0;
  ValidatorDefinition rule;
  rule.id = "test.callback"; rule.keywords = {"Test"}; rule.schema_types = {"Sphere"};
  rule.callback = [&](const ValidationContext&, const PrimSpec*) -> lightusd::next::expected<USDValidationResult, std::string> {
    ++calls;
    return lightusd::next::make_unexpected(std::string("required service unavailable"));
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
  sampled.callback = [&](const ValidationContext& c, const PrimSpec* p) -> lightusd::next::expected<USDValidationResult, std::string> {
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
