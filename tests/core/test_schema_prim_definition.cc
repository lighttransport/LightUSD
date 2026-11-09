// SPDX-License-Identifier: Apache-2.0
// AOUSD Core 1.1 schema-definition rules (doc/validation/
// aousd11-schema-defs-uihints-2026-11-04.md): prim-definition fallback order,
// abstract / API typeName => typeless, and the `uiHints` UI-hint dictionary.
#include "validation/validation-context.hh"
#include "../../src/pcp/layer-registry.hh"
#include "schema/schema-registry.hh"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace lightusd;

#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " #c "\n";     \
      return 1;                                                     \
    }                                                               \
  } while (0)

static int TestFallbackOrder() {
  const SchemaRegistry& r = GetSchemaRegistry();
  // "Fallback Value Resolution": the typed schema is strongest, even when an
  // applied schema registered earlier defines the same property. OpenUSD
  // v26.11 agrees (PhysicsFixedJoint + PhysicsCollisionAPI -> false).
  PrimSpec joint("J", "PhysicsFixedJoint");
  joint.meta().apiSchemas().push_back("PhysicsCollisionAPI");
  const auto* d = r.FindProperty(joint, "physics:collisionEnabled");
  CHECK(d && d->schema_type == "PhysicsJoint");
  CHECK(d->fallback.as_bool() && *d->fallback.as_bool() == false);
  // Without a typed opinion the applied schema supplies it.
  PrimSpec xf("X", "Xform");
  xf.meta().apiSchemas().push_back("PhysicsCollisionAPI");
  d = r.FindProperty(xf, "physics:collisionEnabled");
  CHECK(d && d->schema_type == "PhysicsCollisionAPI");
  // Applied schemas in list-op resolved order, strongest first.
  PrimSpec a("A", "Mesh");
  a.meta().apiSchemas().push_back("PhysicsMaterialAPI");
  a.meta().apiSchemas().push_back("PhysicsMassAPI");
  d = r.FindProperty(a, "physics:density");
  CHECK(d && d->schema_type == "PhysicsMaterialAPI");
  PrimSpec b("B", "Mesh");
  b.meta().apiSchemas().push_back("PhysicsMassAPI");
  b.meta().apiSchemas().push_back("PhysicsMaterialAPI");
  d = r.FindProperty(b, "physics:density");
  CHECK(d && d->schema_type == "PhysicsMassAPI");
  return 0;
}

static int TestAbstractTypeIsTypeless() {
  const SchemaRegistry& r = GetSchemaRegistry();
  // "Typed Schemas": an abstract or non-typed schema name as typeName leaves
  // the prim typeless -- no typed-schema properties or fallbacks.
  for (const char* type : {"Imageable", "Gprim", "Xformable", "Boundable",
                           "PointBased", "CollectionAPI", "Typed"}) {
    PrimSpec p("P", type);
    CHECK(!r.IsConcreteTypedSchemaName(type));
    if (r.FindProperty(p, "visibility")) {
      std::cerr << "typeless " << type << " got visibility\n";
      return 1;
    }
    CHECK(r.PropertyNames(p).empty());
  }
  for (const char* type : {"Xform", "Mesh", "Sphere", "PhysicsFixedJoint"}) {
    PrimSpec p("P", type);
    CHECK(r.IsConcreteTypedSchemaName(type));
    CHECK(r.FindProperty(p, "visibility"));
  }
  // Applied schemas still contribute on a typeless prim.
  PrimSpec t("T", "Gprim");
  t.meta().apiSchemas().push_back("PhysicsCollisionAPI");
  const auto* d = r.FindProperty(t, "physics:collisionEnabled");
  CHECK(d && d->schema_type == "PhysicsCollisionAPI");

  // Validation registry property lookup follows the same rule.
  ValidationRegistry v;
  CHECK(v.LoadSchemaDefinitions(R"({"formatVersion":1,"schemas":[
      {"name":"AbsBase","kind":"abstract","properties":[
        {"name":"absprop","kind":"attribute","type":"int"}]},
      {"name":"Conc","kind":"concrete","inherits":"AbsBase","properties":[]}]})"));
  PrimSpec abs("P", "AbsBase");
  CHECK(!v.FindProperty(abs, "absprop"));
  PrimSpec conc("P", "Conc");
  CHECK(v.FindProperty(conc, "absprop"));
  return 0;
}

static const DecodedExtensionField* FindField(
    const std::vector<DecodedExtensionField>& fields, const char* name) {
  for (const auto& f : fields)
    if (f.name == name) return &f;
  return nullptr;
}

static int TestUIHints() {
  // "User Interface Fields": `uiHints` is a dictionary extension field on
  // prim and property specs. It must parse as a typed dictionary (not opaque
  // text) so it composes per key and writes as a USDC dictionary.
  const char* text =
      "#usda 1.0\n"
      "def Xform \"p\" (\n"
      "  displayName = \"Core\"\n"
      "  uiHints = {\n    string displayName = \"Hint\"\n    bool hidden = 1\n  }\n"
      ")\n{\n"
      "  float x = 1 (\n    displayGroup = \"G\"\n"
      "    uiHints = {\n      string displayGroup = \"H\"\n    }\n  )\n}\n";
  std::string warning, error;
  auto layer = pcp::LoadLayerFromMemory(
      "ui.usda", reinterpret_cast<const uint8_t*>(text), std::strlen(text),
      &warning, &error);
  CHECK(layer);
  CHECK(warning.find("uiHints") == std::string::npos);
  const PrimSpec* p = nullptr;
  for (const auto& prim : layer->prims())
    if (prim.path().str() == "/p") p = &prim;
  CHECK(p);
  CHECK(p->meta().unknownMeta().empty());
  const auto* f = FindField(p->meta().unknownFields(), "uiHints");
  CHECK(f && f->value.is_dictionary());
  const PropMeta* pm = p->property_meta("x");
  CHECK(pm);
  CHECK(pm->unknownMeta.empty());
  f = FindField(pm->unknownFields, "uiHints");
  CHECK(f && f->value.is_dictionary());
  return 0;
}

int main() {
  if (TestFallbackOrder()) return 1;
  if (TestAbstractTypeIsTypeless()) return 1;
  if (TestUIHints()) return 1;
  std::cout << "test_schema_prim_definition: OK\n";
  return 0;
}
