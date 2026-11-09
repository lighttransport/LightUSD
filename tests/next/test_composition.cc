// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Composition arc-expansion tests for src/next: references (internal +
// external, with subtree grafting), payloads, inherits, specializes, local
// override strength, arc clearing, and cycle safety.

#include "next/composition/composition.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include "next/crate/crate-writer.hh"
#include "next/crate/crate-reader.hh"
#include "next/reader/usda-reader.hh"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <cstring>

using namespace lightusd::next;

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
  do {                                                                      \
    if (!(cond)) { std::cerr << "  FAIL: " << msg << "\n"; ++g_fail; }      \
    else { std::cout << "  ok: " << msg << "\n"; }                          \
  } while (0)

static PrimSpec MakePrim(const std::string& path, const std::string& type) {
  size_t s = path.rfind('/');
  std::string name = (s == std::string::npos) ? path : path.substr(s + 1);
  PrimSpec p(name, type);
  p.set_path(Path(path));
  return p;
}

static const Value* PropOf(const Layer& l, const std::string& prim_path,
                           const std::string& prop) {
  const PrimSpec* p = l.prim_at_path(prim_path);
  if (!p) return nullptr;
  return p->property_value(prop);
}

// Inherits copy class opinions; local opinion wins; arc cleared after flatten.
static void test_inherits() {
  std::cout << "[inherits]\n";
  Layer layer;
  {
    PrimSpec cls = MakePrim("/_class_Sphere", "Sphere");
    cls.add_property("radius", Value(2.0));
    cls.add_property("displayColor", Value(std::string("red")));
    layer.add_prim(std::move(cls));
  }
  {
    PrimSpec ball = MakePrim("/Ball", "Sphere");
    ball.add_property("radius", Value(5.0));  // local override (strongest)
    ball.meta().inherits.push_back("/_class_Sphere");
    layer.add_prim(std::move(ball));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  CHECK(out != nullptr, "compose succeeds");
  const Value* r = PropOf(*out, "/Ball", "radius");
  CHECK(r && r->as_double() && *r->as_double() == 5.0,
        "local radius wins over inherited (5.0)");
  CHECK(PropOf(*out, "/Ball", "displayColor") != nullptr,
        "displayColor inherited from class");
  const PrimSpec* ball = out->prim_at_path("/Ball");
  CHECK(ball && ball->meta().inherits.empty(),
        "inherits arc cleared after flatten");
}

// Internal reference copies opinions AND grafts the referenced subtree.
static void test_internal_reference() {
  std::cout << "[internal reference]\n";
  Layer layer;
  {
    PrimSpec lib = MakePrim("/Lib", "Xform");
    lib.add_property("v", Value(int32_t(1)));
    layer.add_prim(std::move(lib));
  }
  {
    PrimSpec child = MakePrim("/Lib/Mesh", "Mesh");
    child.add_property("points", Value(int32_t(7)));
    layer.add_prim(std::move(child));
  }
  {
    PrimSpec inst = MakePrim("/Inst", "Xform");
    inst.meta().references.push_back("</Lib>");  // internal ref
    layer.add_prim(std::move(inst));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  CHECK(out != nullptr, "compose succeeds");
  CHECK(PropOf(*out, "/Inst", "v") != nullptr,
        "referenced opinion copied onto /Inst");
  CHECK(out->prim_at_path("/Inst/Mesh") != nullptr,
        "referenced subtree grafted under /Inst");
  CHECK(PropOf(*out, "/Inst/Mesh", "points") != nullptr,
        "grafted child carries its properties");
  const PrimSpec* inst = out->prim_at_path("/Inst");
  CHECK(inst && inst->meta().references.empty(),
        "reference arc cleared after flatten");
  // Original library prim still exists independently.
  CHECK(out->prim_at_path("/Lib") != nullptr, "/Lib still present");
}

// External reference resolved via a layer loader, with subtree graft.
static void test_external_reference() {
  std::cout << "[external reference]\n";
  Layer root;
  {
    PrimSpec r = MakePrim("/Root", "Xform");
    r.meta().references.push_back("@base.usd@</Base>");
    root.add_prim(std::move(r));
  }
  root.finalize();

  Compositor comp;
  comp.SetLayerLoader(
      [](const std::string& path, std::string* err) -> std::unique_ptr<Layer> {
        (void)path;
        (void)err;
        auto l = std::make_unique<Layer>();
        PrimSpec base = MakePrim("/Base", "Xform");
        base.add_property("imported", Value(int32_t(42)));
        l->add_prim(std::move(base));
        PrimSpec geo = MakePrim("/Base/Geo", "Mesh");
        geo.add_property("n", Value(int32_t(3)));
        l->add_prim(std::move(geo));
        l->finalize();  // external layers must be finalized for prim_at_path
        return l;
      });

  auto out = comp.Compose(root);
  CHECK(out != nullptr, "compose succeeds");
  CHECK(PropOf(*out, "/Root", "imported") != nullptr,
        "external opinion copied onto /Root");
  CHECK(out->prim_at_path("/Root/Geo") != nullptr,
        "external subtree grafted under /Root");
  CHECK(PropOf(*out, "/Root/Geo", "n") != nullptr,
        "grafted external child carries properties");
}

// Specializes is the weakest arc: only fills what is still missing.
static void test_specializes() {
  std::cout << "[specializes]\n";
  Layer layer;
  {
    PrimSpec base = MakePrim("/_base", "Material");
    base.add_property("roughness", Value(0.5));
    base.add_property("metallic", Value(0.0));
    layer.add_prim(std::move(base));
  }
  {
    PrimSpec mat = MakePrim("/Mat", "Material");
    mat.add_property("roughness", Value(0.9));  // local
    mat.meta().specializes.push_back("/_base");
    layer.add_prim(std::move(mat));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  const Value* rough = PropOf(*out, "/Mat", "roughness");
  CHECK(rough && rough->as_double() && *rough->as_double() == 0.9,
        "local roughness wins over specialized (0.9)");
  CHECK(PropOf(*out, "/Mat", "metallic") != nullptr,
        "metallic filled from specialize");
}

// Cyclic internal references must terminate.
static void test_cycle_safety() {
  std::cout << "[cycle safety]\n";
  Layer layer;
  {
    PrimSpec a = MakePrim("/A", "Xform");
    a.add_property("a", Value(int32_t(1)));
    a.meta().references.push_back("</B>");
    layer.add_prim(std::move(a));
  }
  {
    PrimSpec b = MakePrim("/B", "Xform");
    b.add_property("b", Value(int32_t(2)));
    b.meta().references.push_back("</A>");
    layer.add_prim(std::move(b));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);  // must not infinite-loop
  CHECK(out != nullptr, "compose terminates on cyclic references");
  const PrimSpec* a = out->prim_at_path("/A");
  CHECK(a && a->meta().references.empty(), "cycle: arcs cleared");
}

// Transitive internal reference: P -> Q -> R; P must end up with R's opinion.
static void test_transitive_reference() {
  std::cout << "[transitive reference]\n";
  Layer layer;
  {
    PrimSpec r = MakePrim("/R", "Xform");
    r.add_property("deep", Value(int32_t(99)));
    layer.add_prim(std::move(r));
  }
  {
    PrimSpec q = MakePrim("/Q", "Xform");
    q.meta().references.push_back("</R>");
    layer.add_prim(std::move(q));
  }
  {
    PrimSpec p = MakePrim("/P", "Xform");
    p.meta().references.push_back("</Q>");
    layer.add_prim(std::move(p));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  CHECK(PropOf(*out, "/P", "deep") != nullptr,
        "transitive opinion R->Q->P reached /P");
}

// Variant selection applies the chosen variant's opinions; a property authored
// locally (outside the variant) stays strongest; variant arcs are cleared.
static void test_variants() {
  std::cout << "[variants]\n";
  Layer layer;
  {
    PrimSpec shape = MakePrim("/Shape", "Sphere");
    shape.add_property("radius", Value(1.0));  // local opinion (strongest)

    PrimVariantSetData vsd;
    vsd.name = "shapeVariant";
    PrimVariantData big;
    big.name = "big";
    big.properties.emplace_back("radius", Value(10.0));            // weaker than local
    big.properties.emplace_back("color", Value(std::string("red")));  // variant-only
    vsd.variants.push_back(std::move(big));
    PrimVariantData small;
    small.name = "small";
    small.properties.emplace_back("radius", Value(0.5));
    vsd.variants.push_back(std::move(small));
    shape.meta().variantSets().push_back(std::move(vsd));
    shape.meta().variantSelection = "shapeVariant=big";
    layer.add_prim(std::move(shape));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  const Value* r = PropOf(*out, "/Shape", "radius");
  CHECK(r && r->as_double() && *r->as_double() == 1.0,
        "local radius (1.0) wins over variant radius (10.0)");
  CHECK(PropOf(*out, "/Shape", "color") != nullptr,
        "variant-only 'color' applied from selected variant 'big'");
  const PrimSpec* shape = out->prim_at_path("/Shape");
  CHECK(shape && shape->meta().variantSets().empty() &&
            shape->meta().variantSelection.empty(),
        "variant set/selection cleared after flatten");
}

// Multiple variant sets selected on one prim, each applied (via vs.selected).
static void test_variants_multiset() {
  std::cout << "[variants: multiple sets]\n";
  Layer layer;
  {
    PrimSpec p = MakePrim("/M", "Mesh");
    PrimVariantSetData lod;
    lod.name = "lod"; lod.selected = "high";
    { PrimVariantData v; v.name = "high"; v.properties.emplace_back("subdiv", Value(int32_t(2))); lod.variants.push_back(std::move(v)); }
    { PrimVariantData v; v.name = "low";  v.properties.emplace_back("subdiv", Value(int32_t(0))); lod.variants.push_back(std::move(v)); }
    p.meta().variantSets().push_back(std::move(lod));
    PrimVariantSetData shade;
    shade.name = "shading"; shade.selected = "red";
    { PrimVariantData v; v.name = "red"; v.properties.emplace_back("tint", Value(std::string("r"))); shade.variants.push_back(std::move(v)); }
    p.meta().variantSets().push_back(std::move(shade));
    layer.add_prim(std::move(p));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  const Value* sd = PropOf(*out, "/M", "subdiv");
  CHECK(sd && sd->as_int() && *sd->as_int() == 2,
        "set 'lod'=high applied (subdiv=2)");
  CHECK(PropOf(*out, "/M", "tint") != nullptr,
        "set 'shading'=red applied (tint) — both sets selected");
}

// A variant that adds CHILD PRIMS: the selected variant's sub-prims are grafted
// under the owning prim; the unselected variant's sub-prims are not.
static void test_variant_subprim() {
  std::cout << "[variants: sub-prim graft]\n";
  Layer layer;
  {
    PrimSpec root = MakePrim("/Root", "Xform");
    PrimVariantSetData vsd;
    vsd.name = "geo";
    vsd.selected = "hi";  // no property variants — only child prims
    root.meta().variantSets().push_back(std::move(vsd));
    layer.add_prim(std::move(root));
  }
  {  // selected variant's child (+ a grandchild) live under the holder path
    PrimSpec body = MakePrim("/Root/{geo=hi}/Body", "Scope");
    body.add_property("val", Value(int32_t(4)));
    layer.add_prim(std::move(body));
  }
  {
    PrimSpec sub = MakePrim("/Root/{geo=hi}/Body/Sub", "Scope");
    layer.add_prim(std::move(sub));
  }
  {  // unselected variant's child
    PrimSpec other = MakePrim("/Root/{geo=lo}/Other", "Scope");
    layer.add_prim(std::move(other));
  }
  layer.finalize();

  Compositor comp;
  auto out = comp.Compose(layer);
  CHECK(out->prim_at_path("/Root/Body") != nullptr,
        "selected variant's child prim grafted under owner");
  CHECK(PropOf(*out, "/Root/Body", "val") != nullptr,
        "grafted child carries its property");
  CHECK(out->prim_at_path("/Root/Body/Sub") != nullptr,
        "grafted child's grandchild grafted too (full subtree)");
  CHECK(out->prim_at_path("/Root/Other") == nullptr,
        "unselected variant's child prim NOT grafted");
}

// Unflattened variant round-trip: write a layer with variant holders/children
// WITHOUT composing, read it back, and confirm the variant content survives
// (and still composes correctly).
static void test_variant_roundtrip() {
  std::cout << "[variants: unflattened round-trip]\n";
  Layer layer;
  {
    PrimSpec root = MakePrim("/Root", "Xform");
    PrimVariantSetData vsd;
    vsd.name = "geo";
    vsd.selected = "hi";
    root.meta().variantSets().push_back(std::move(vsd));
    layer.add_prim(std::move(root));
  }
  {  // variant holder + child prim (a normal layer prim at the bracketed path)
    PrimSpec holder = MakePrim("/Root/{geo=hi}", "Xform");
    layer.add_prim(std::move(holder));
  }
  {
    PrimSpec body = MakePrim("/Root/{geo=hi}/Body", "Scope");
    body.add_property("val", Value(int32_t(4)));
    layer.add_prim(std::move(body));
  }
  layer.finalize();

  // Write WITHOUT composing, then read back.
  CrateWriter writer;
  std::vector<uint8_t> buf;
  CHECK(writer.WriteLayerToMemory(buf, layer).success,
        "unflattened write succeeds");
  CrateReadOptions ro;
  ro.lazy_arrays = true;
  CrateReader reader(ro);
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  CHECK(rr.success, "re-read succeeds");
  const Layer* l2 = rr.stage.GetRootLayer();
  const PrimSpec* root2 = l2 ? l2->prim_at_path("/Root") : nullptr;
  CHECK(root2 && !root2->meta().variantSets().empty(),
        "variant set + selection survived the unflattened write");
  CHECK(l2 && l2->prim_at_path("/Root/{geo=hi}/Body") != nullptr,
        "variant child prim survived in the layer");

  // Composing the re-read layer must still bake the selected variant.
  Compositor comp;
  auto composed = comp.Compose(*l2);
  CHECK(composed->prim_at_path("/Root/Body") != nullptr,
        "re-read variant composes: child grafted under owner");
  CHECK(PropOf(*composed, "/Root/Body", "val") != nullptr,
        "grafted child retains its property after round-trip");
}

// A modelingVariant set with two options ("ChairA"/"ChairB"); each option
// carries a distinguishing "which" property only when `populated`.
static PrimVariantSetData MakeModelingVariantSet(bool populated) {
  PrimVariantSetData vsd;
  vsd.name = "modelingVariant";
  PrimVariantData a; a.name = "ChairA";
  PrimVariantData b; b.name = "ChairB";
  if (populated) {
    a.properties.emplace_back("which", Value(std::string("I_am_ChairA")));
    b.properties.emplace_back("which", Value(std::string("I_am_ChairB")));
  }
  vsd.variants.push_back(std::move(a));
  vsd.variants.push_back(std::move(b));
  return vsd;
}

// Regression (Pixar Kitchen_set Chair.usd): a variant selection authored on the
// REFERENCING prim must win over the referenced asset's OWN default selection.
//   /C1 references @geom@</Chair>, selects non-default "ChairB"
//   geom/Chair: populated variantSet + its own (weaker) default "ChairA"
// The main backend (examples/lusdcat) regressed on the larger Kitchen_set chain;
// the next backend composes all arcs per-prim before ApplyVariants, so the local
// selection is still present when the variant is baked. Confirms that here.
static void test_variant_selection_over_reference() {
  std::cout << "[variants: local selection wins over referenced default]\n";
  Layer root;
  {
    PrimSpec c1 = MakePrim("/C1", "Xform");
    c1.meta().references.push_back("@geom@</Chair>");
    c1.meta().variantSelection = "modelingVariant=ChairB";  // strong, non-default
    root.add_prim(std::move(c1));
  }
  root.finalize();

  Compositor comp;
  comp.SetLayerLoader(
      [](const std::string&, std::string*) -> std::unique_ptr<Layer> {
        // Referenced asset: populated variantSet + its own default "ChairA".
        auto l = std::make_unique<Layer>();
        PrimSpec chair = MakePrim("/Chair", "Xform");
        chair.meta().variantSelection = "modelingVariant=ChairA";
        chair.meta().variantSets().push_back(
            MakeModelingVariantSet(/*populated=*/true));
        l->add_prim(std::move(chair));
        l->finalize();
        return l;
      });

  auto out = comp.Compose(root);
  CHECK(out != nullptr, "compose succeeds");
  const Value* which = out ? PropOf(*out, "/C1", "which") : nullptr;
  CHECK(which != nullptr,
        "/C1.which present (selected variant's opinion applied to host)");
  if (which) {
    auto s = which->as_string();
    CHECK(s && *s == "I_am_ChairB",
          std::string("local 'ChairB' wins over referenced default 'ChairA' "
                      "(got '") +
              (s ? std::string(*s) : std::string("<non-string>")) + "')");
  }
}

// Regression for variant-bearing instance libraries: the selected holder of
// each external reference lives in pending_graft_ until the arc-expansion pass
// finishes. ApplyVariants must inspect the grafts produced for that instance,
// without rescanning every earlier instance's grafts (quadratic on large UE
// scenes with thousands of LOD-bearing references).
static void test_many_variant_bearing_references() {
  std::cout << "[variants: many external references scale linearly]\n";
  constexpr size_t kInstances = 1024;
  constexpr size_t kParts = 8;

  Layer root;
  for (size_t i = 0; i < kInstances; ++i) {
    PrimSpec instance = MakePrim("/I" + std::to_string(i), "Xform");
    instance.meta().references.push_back("@model@</Model>");
    instance.meta().variantSelection = "lod=high";
    root.add_prim(std::move(instance));
  }
  root.finalize();

  auto loader = [](const std::string&,
                   std::string*) -> std::unique_ptr<Layer> {
    auto layer = std::make_unique<Layer>();
    PrimSpec model = MakePrim("/Model", "Xform");
    PrimVariantSetData lod;
    lod.name = "lod";
    lod.selected = "low";  // host's high selection must remain stronger.
    PrimVariantData low;
    low.name = "low";
    PrimVariantData high;
    high.name = "high";
    lod.variants.push_back(std::move(low));
    lod.variants.push_back(std::move(high));
    model.meta().variantSets().push_back(std::move(lod));
    layer->add_prim(std::move(model));

    PrimSpec holder = MakePrim("/Model/{lod=high}", "Xform");
    holder.add_property("quality", Value(int32_t(7)));
    layer->add_prim(std::move(holder));
    for (size_t i = 0; i < kParts; ++i) {
      PrimSpec part = MakePrim("/Model/{lod=high}/Part" + std::to_string(i),
                               "Mesh");
      part.add_property("part", Value(int32_t(i)));
      layer->add_prim(std::move(part));
    }
    PrimSpec low_holder = MakePrim("/Model/{lod=low}", "Xform");
    layer->add_prim(std::move(low_holder));
    layer->finalize();
    return layer;
  };

  Compositor comp;
  comp.SetLayerLoader(loader);
  const auto begin = std::chrono::steady_clock::now();
  auto out = comp.Compose(root);
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - begin).count();
  CHECK(out != nullptr, "many variant-bearing references compose");
  CHECK(seconds < 10.0,
        "many variant-bearing references avoid quadratic graft scans");
  if (!out) return;

  bool all_composed = true;
  for (size_t i = 0; i < kInstances; ++i) {
    const std::string path = "/I" + std::to_string(i);
    const Value* quality = PropOf(*out, path, "quality");
    if (!quality || !quality->as_int() || *quality->as_int() != 7 ||
        !out->prim_at_path(path + "/Part0") ||
        !out->prim_at_path(path + "/Part7") ||
        out->prim_at_path(path + "/{lod=low}")) {
      all_composed = false;
      break;
    }
  }
  CHECK(all_composed,
        "every reference bakes the selected holder and drops unselected LODs");
}

// Regression: the next backend must resolve the arcs authored on an
// EXTERNALLY-referenced target (its payload / nested references), not just the
// arcs on the prim itself. The exact Pixar Kitchen_set Chair.usd shape:
//   /C1 references @asset@</Chair>, selects non-default "ChairB"
//   asset/Chair: EMPTY variant blocks + payload -> @payload@ + own default "ChairA"
//   payload/Chair: references @geom@</Chair>
//   geom/Chair: POPULATED variantSet + own (weakest) default "ChairA"
// GetComposedExternalLayer() composes each referenced layer's own arcs (variants
// deferred) before its opinions are merged into the host, and CopyLocalOpinions
// merges variantSet CONTENT per-variant so the deep populated blocks fill the
// asset's empty ones. The host then bakes its own "ChairB" selection.
static void test_variant_ref_payload_chain() {
  std::cout << "[variants: selection across ref->payload->ref chain]\n";
  Layer root;
  {
    PrimSpec c1 = MakePrim("/C1", "Xform");
    c1.meta().references.push_back("@asset@</Chair>");
    c1.meta().variantSelection = "modelingVariant=ChairB";
    root.add_prim(std::move(c1));
  }
  root.finalize();

  Compositor comp;
  comp.SetLayerLoader(
      [](const std::string& path, std::string*) -> std::unique_ptr<Layer> {
        auto l = std::make_unique<Layer>();
        if (path.find("asset") != std::string::npos) {
          PrimSpec chair = MakePrim("/Chair", "Xform");
          chair.meta().payloads.push_back("@payload@</Chair>");
          chair.meta().variantSelection = "modelingVariant=ChairA";
          chair.meta().variantSets().push_back(
              MakeModelingVariantSet(/*populated=*/false));
          l->add_prim(std::move(chair));
        } else if (path.find("payload") != std::string::npos) {
          PrimSpec chair = MakePrim("/Chair", "Xform");
          chair.meta().references.push_back("@geom@</Chair>");
          l->add_prim(std::move(chair));
        } else {
          PrimSpec chair = MakePrim("/Chair", "Xform");
          chair.meta().variantSelection = "modelingVariant=ChairA";
          chair.meta().variantSets().push_back(
              MakeModelingVariantSet(/*populated=*/true));
          l->add_prim(std::move(chair));
        }
        l->finalize();
        return l;
      });

  auto out = comp.Compose(root);
  CHECK(out != nullptr, "compose succeeds");
  const Value* which = out ? PropOf(*out, "/C1", "which") : nullptr;
  CHECK(which != nullptr,
        "/C1.which present (deep populated variant content reached the host "
        "through the ref->payload->ref chain)");
  if (which) {
    auto s = which->as_string();
    CHECK(s && *s == "I_am_ChairB",
          std::string("local 'ChairB' wins across the full ref+payload chain "
                      "(got '") +
              (s ? std::string(*s) : std::string("<non-string>")) + "', want 'I_am_ChairB')");
  }
}

// Subtree-scoped external composition: a SELF-CONTAINED arc-bearing prim of a
// multi-prim library composes alone (only its subtree is materialized; the
// library's other prims are not needed) and yields the same result as composing
// it inside the whole layer. AssetA's only arc is an EXTERNAL reference, so it
// is self-contained; the library also holds an unrelated /Base and a
// NON-self-contained /AssetC to ensure they are neither needed nor leaked.
static void test_extref_self_contained_subtree() {
  std::cout << "[extref: self-contained subtree composed alone]\n";
  Layer root;
  {
    PrimSpec r = MakePrim("/R", "Xform");
    r.meta().references.push_back("@lib@</AssetA>");
    root.add_prim(std::move(r));
  }
  root.finalize();

  Compositor comp;
  comp.SetLayerLoader(
      [](const std::string& path, std::string*) -> std::unique_ptr<Layer> {
        auto l = std::make_unique<Layer>();
        if (path.find("shader") != std::string::npos) {
          PrimSpec s = MakePrim("/S", "Shader");
          s.add_property("mat", Value(std::string("red")));
          l->add_prim(std::move(s));
        } else {
          PrimSpec a = MakePrim("/AssetA", "Xform");
          a.meta().references.push_back("@shader@</S>");  // external → self-contained
          l->add_prim(std::move(a));
          PrimSpec ag = MakePrim("/AssetA/Geom", "Mesh");  // a descendant to graft
          ag.add_property("n", Value(int32_t(5)));
          l->add_prim(std::move(ag));
          PrimSpec base = MakePrim("/Base", "Xform");
          base.add_property("base", Value(int32_t(7)));
          l->add_prim(std::move(base));
          PrimSpec c = MakePrim("/AssetC", "Xform");
          c.meta().references.push_back("</Base>");  // internal → NOT self-contained
          l->add_prim(std::move(c));
        }
        l->finalize();
        return l;
      });

  auto out = comp.Compose(root);
  CHECK(out != nullptr, "compose succeeds");
  const Value* mat = out ? PropOf(*out, "/R", "mat") : nullptr;
  CHECK(mat && mat->as_string() && *mat->as_string() == "red",
        "self-contained AssetA's external ref resolved via subtree compose");
  CHECK(out && out->prim_at_path("/R/Geom") != nullptr,
        "AssetA's descendant grafted from the extracted subtree");
  CHECK(PropOf(*out, "/R/Geom", "n") != nullptr,
        "grafted descendant carries its property");
  CHECK(out && out->prim_at_path("/R/Base") == nullptr,
        "unrelated library sibling not leaked into the host");
}

// Fallback: a referenced prim whose INTERNAL reference escapes its subtree is
// NOT self-contained and must compose against the whole layer so the sibling
// target resolves.
static void test_extref_non_self_contained_fallback() {
  std::cout << "[extref: non-self-contained subtree falls back to whole layer]\n";
  Layer root;
  {
    PrimSpec r = MakePrim("/R", "Xform");
    r.meta().references.push_back("@lib@</AssetC>");
    root.add_prim(std::move(r));
  }
  root.finalize();

  Compositor comp;
  comp.SetLayerLoader(
      [](const std::string&, std::string*) -> std::unique_ptr<Layer> {
        auto l = std::make_unique<Layer>();
        PrimSpec base = MakePrim("/Base", "Xform");
        base.add_property("base", Value(int32_t(7)));
        l->add_prim(std::move(base));
        PrimSpec c = MakePrim("/AssetC", "Xform");
        c.meta().references.push_back("</Base>");  // internal ref to a sibling
        l->add_prim(std::move(c));
        l->finalize();
        return l;
      });

  auto out = comp.Compose(root);
  CHECK(out != nullptr, "compose succeeds");
  const Value* base = out ? PropOf(*out, "/R", "base") : nullptr;
  CHECK(base && base->as_int() && *base->as_int() == 7,
        "AssetC's internal ref to sibling /Base resolved via whole-layer compose");
}

// The crate (USDC) reader records a variant selection as BOTH a per-set
// PrimVariantSetData.selected AND the legacy variantSelection string. A host that
// overrides a referenced asset's variant must win regardless of which form it
// carries; the asset's (weaker) per-set `selected` must not ride along the
// CopyLocalOpinions variantSet merge and defeat the override in ApplyVariants
// (which prefers vs.selected over the legacy string).
static void test_variant_selected_field_over_reference() {
  std::cout << "[variants: host selection wins over referenced vs.selected]\n";

  // A modelingVariant set carrying its own per-set `selected` (reader form).
  auto makeSet = [](const std::string& sel) {
    PrimVariantSetData vsd;
    vsd.name = "modelingVariant";
    vsd.selected = sel;
    PrimVariantData a; a.name = "ChairA";
    a.properties.emplace_back("which", Value(std::string("I_am_ChairA")));
    PrimVariantData b; b.name = "ChairB";
    b.properties.emplace_back("which", Value(std::string("I_am_ChairB")));
    vsd.variants.push_back(std::move(a));
    vsd.variants.push_back(std::move(b));
    return vsd;
  };
  // Referenced asset always selects its OWN default "ChairA" via vs.selected.
  auto loader = [&](const std::string&, std::string*) -> std::unique_ptr<Layer> {
    auto l = std::make_unique<Layer>();
    PrimSpec ch = MakePrim("/Chair", "Xform");
    ch.meta().variantSets().push_back(makeSet("ChairA"));
    ch.meta().variantSelection = "modelingVariant=ChairA";
    l->add_prim(std::move(ch));
    l->finalize();
    return l;
  };
  auto whichOf = [](const Layer* out, const std::string& prim) {
    const Value* w = out ? PropOf(*out, prim, "which") : nullptr;
    return (w && w->as_string()) ? std::string(*w->as_string()) : std::string("<none>");
  };

  // (1) reader-consistent host: BOTH per-set selected AND the string = "ChairB".
  {
    Layer root;
    PrimSpec c = MakePrim("/H1", "Xform");
    c.meta().references.push_back("@asset@</Chair>");
    c.meta().variantSets().push_back(makeSet("ChairB"));
    c.meta().variantSelection = "modelingVariant=ChairB";
    root.add_prim(std::move(c));
    root.finalize();
    Compositor comp; comp.SetLayerLoader(loader);
    CHECK(whichOf(comp.Compose(root).get(), "/H1") == "I_am_ChairB",
          "host vs.selected 'ChairB' wins over referenced vs.selected 'ChairA'");
  }

  // (2) string-only host: the asset's per-set `selected` must NOT defeat the
  // host's legacy variantSelection (the hardened merge suppresses it).
  {
    Layer root;
    PrimSpec c = MakePrim("/H2", "Xform");
    c.meta().references.push_back("@asset@</Chair>");
    c.meta().variantSelection = "modelingVariant=ChairB";  // string only
    root.add_prim(std::move(c));
    root.finalize();
    Compositor comp; comp.SetLayerLoader(loader);
    CHECK(whichOf(comp.Compose(root).get(), "/H2") == "I_am_ChairB",
          "host variantSelection string 'ChairB' wins over referenced vs.selected");
  }

  // (3) control: host does NOT override → the asset's own default ChairA stands
  // (the hardening must not suppress a referenced selection the host didn't override).
  {
    Layer root;
    PrimSpec c = MakePrim("/H3", "Xform");
    c.meta().references.push_back("@asset@</Chair>");
    root.add_prim(std::move(c));
    root.finalize();
    Compositor comp; comp.SetLayerLoader(loader);
    CHECK(whichOf(comp.Compose(root).get(), "/H3") == "I_am_ChairA",
          "no host override → referenced default 'ChairA' applies");
  }
}


// ------------------------------------------------------------------
// Regression tests from the 2026-07 composition audit.
// ------------------------------------------------------------------

static std::unique_ptr<Layer> ParseLayer(const char* usda) {
  LoadResult r = LoadUSDAFromString(usda, std::strlen(usda));
  if (!r.success || !r.stage.GetRootLayer()) return nullptr;
  return r.stage.ReleaseRootLayer();
}

// Sublayer merge: one spec per path, root > s1 > s2 strength, children keep
// their hierarchy, subLayers cleared, defaultPrim filled from a sublayer.
static void test_sublayer_merge() {
  std::cout << "[sublayer merge]\n";
  auto loader = [&](const std::string& path,
                    std::string*) -> std::unique_ptr<Layer> {
    if (path.find("s1") != std::string::npos) {
      return ParseLayer(
          "#usda 1.0\n(\n defaultPrim = \"p\"\n)\n"
          "def Xform \"p\" { float a = 10\n float b = 10\n"
          "  def Mesh \"kid\" { float k = 1 } }\n");
    }
    return ParseLayer(
        "#usda 1.0\ndef Xform \"p\" { float a = 100\n float b = 100\n"
        " float c = 100 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n(\n subLayers = [ @./s1.usda@, @./s2.usda@ ]\n)\n"
      "def Xform \"p\" { float a = 1 }\n");
  CHECK(root != nullptr, "root parses");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");

  size_t p_specs = 0;
  for (const PrimSpec& pr : out->prims()) {
    if (pr.path().str() == "/p") ++p_specs;
  }
  CHECK(p_specs == 1, "single merged spec per path (no duplicates)");
  const Value* a = PropOf(*out, "/p", "a");
  CHECK(a && a->as_float() && *a->as_float() == 1.0f, "root wins (a=1)");
  const Value* b = PropOf(*out, "/p", "b");
  CHECK(b && b->as_float() && *b->as_float() == 10.0f,
        "earlier sublayer beats later (b=10)");
  const Value* c = PropOf(*out, "/p", "c");
  CHECK(c && c->as_float() && *c->as_float() == 100.0f,
        "weakest sublayer fills (c=100)");
  CHECK(PropOf(*out, "/p/kid", "k") != nullptr, "sublayer child composes");
  // Sublayer child must be a CHILD, not a stage root.
  bool kid_is_root = false;
  for (uint32_t ri : out->root_indices()) {
    const PrimSpec* rp = out->prim(ri);
    if (rp && rp->path().str() == "/p/kid") kid_is_root = true;
  }
  CHECK(!kid_is_root, "sublayer child not leaked to stage root");
  CHECK(out->meta().subLayers.empty(), "baked subLayers cleared from output");
  CHECK(out->meta().defaultPrim.empty(),
        "sublayer defaultPrim does NOT fill (root-layer-only, pxr parity)");
}

// LIVRPS strength: inherits > variants > references; specializes weakest.
// Also: parser-encoded "</class>" inherit/specialize arcs must resolve.
static void test_livrps_strength() {
  std::cout << "[LIVRPS strength]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\ndef Xform \"R\" { float x = 3\n float from_var = 30\n"
        " float shadowed = 3\n float weak = 2\n float from_ref = 3 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "class \"_c\" { float ib = 8\n float shadowed = 80 }\n"
      "class \"_s\" { float sv = 9\n float weak = 90 }\n"
      "def Xform \"p\" (\n"
      "    prepend inherits = </_c>\n"
      "    prepend specializes = </_s>\n"
      "    prepend references = @./r.usda@</R>\n"
      "    variants = { string v = \"on\" }\n"
      "    prepend variantSets = [\"v\"]\n"
      ") {\n"
      "    float x = 1\n"
      "    variantSet \"v\" = { \"on\" { float from_var = 2 } }\n"
      "}\n");
  CHECK(root != nullptr, "root parses");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  auto fval = [&](const char* n) -> float {
    const Value* v = PropOf(*out, "/p", n);
    return (v && v->as_float()) ? *v->as_float() : -999.0f;
  };
  CHECK(fval("x") == 1.0f, "local wins (x=1)");
  CHECK(fval("ib") == 8.0f, "parser-encoded </_c> inherit resolves (ib=8)");
  CHECK(fval("shadowed") == 80.0f, "inherits beat references (shadowed=80)");
  CHECK(fval("from_var") == 2.0f, "variants beat references (from_var=2)");
  CHECK(fval("from_ref") == 3.0f, "references compose (from_ref=3)");
  CHECK(fval("sv") == 9.0f, "parser-encoded </_s> specialize resolves (sv=9)");
  CHECK(fval("weak") == 2.0f, "references beat specializes (weak=2)");
}

// Relationship / connection targets must retarget into the host namespace on
// graft; targets OUTSIDE the referenced subtree are dropped (pxr behavior).
static void test_graft_retargeting() {
  std::cout << "[graft retargeting]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"B\" {\n"
        "    rel own = </B/geo>\n"
        "    rel escape = </Outside/thing>\n"
        "    def Mesh \"geo\" { rel mat = </B/looks/m>\n"
        "        token outputs:s.connect = </B/looks/m.outputs:x> }\n"
        "    def Scope \"looks\" { def Material \"m\" { token outputs:x } }\n"
        "}\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\ndef Xform \"A\" (prepend references = @./b.usda@</B>) { }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");

  const PrimSpec* a = out->prim_at_path("/A");
  const std::vector<Path>* own = a ? a->relationship("own") : nullptr;
  CHECK(own && own->size() == 1 && (*own)[0].str() == "/A/geo",
        "host rel retargeted (/B/geo -> /A/geo)");
  const std::vector<Path>* esc = a ? a->relationship("escape") : nullptr;
  CHECK(!esc || esc->empty(), "out-of-scope rel target dropped");
  const PrimSpec* geo = out->prim_at_path("/A/geo");
  const std::vector<Path>* mat = geo ? geo->relationship("mat") : nullptr;
  CHECK(mat && mat->size() == 1 && (*mat)[0].str() == "/A/looks/m",
        "grafted child rel retargeted (/B/... -> /A/...)");
  const std::vector<Path>* conn = geo ? geo->connection("outputs:s") : nullptr;
  CHECK(conn && conn->size() == 1 &&
            (*conn)[0].str() == "/A/looks/m.outputs:x",
        "grafted child connection retargeted");
}

// Layer offsets on references must BAKE into copied/grafted time samples,
// accumulating through chains (offset2 + scale2 * (offset1 + scale1 * t)).
static void test_layer_offset_baking() {
  std::cout << "[layer offsets]\n";
  auto loader = [&](const std::string& path,
                    std::string*) -> std::unique_ptr<Layer> {
    if (path.find("mid") != std::string::npos) {
      return ParseLayer(
          "#usda 1.0\ndef Xform \"M\" (prepend references = "
          "@./anim.usda@</A> (offset = 10)) { }\n");
    }
    return ParseLayer(
        "#usda 1.0\ndef Xform \"A\" {\n"
        "    double t.timeSamples = { 0: 100.0, 5: 200.0 }\n"
        "    def Mesh \"kid\" { double kt.timeSamples = { 1: 7.0 } }\n"
        "}\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"p\" (prepend references = @./anim.usda@</A> "
      "(offset = 10; scale = 2)) { }\n"
      "def Xform \"q\" (prepend references = @./mid.usda@</M> "
      "(offset = 100; scale = 2)) { }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");

  const PrimSpec* p = out->prim_at_path("/p");
  PropNameId tid = GetPropNameTable().find("t");
  const auto* ts = p ? p->time_samples(tid) : nullptr;
  CHECK(ts && ts->size() == 2 && (*ts)[0].first == 10.0 &&
            (*ts)[1].first == 20.0,
        "host samples remapped by (offset=10, scale=2)");
  const PrimSpec* kid = out->prim_at_path("/p/kid");
  PropNameId ktid = GetPropNameTable().find("kt");
  const auto* kts = kid ? kid->time_samples(ktid) : nullptr;
  CHECK(kts && kts->size() == 1 && (*kts)[0].first == 12.0,
        "grafted child samples remapped (1 -> 12)");
  const PrimSpec* q = out->prim_at_path("/q");
  const auto* qts = q ? q->time_samples(tid) : nullptr;
  CHECK(qts && qts->size() == 2 && (*qts)[0].first == 120.0 &&
            (*qts)[1].first == 130.0,
        "chained offsets accumulate (0 -> 120, 5 -> 130)");
}

// Variant option content (USDA representation): child prims graft, option
// metadata arcs resolve, nested variants apply, active=false deactivates.
static void test_variant_content_legacy() {
  std::cout << "[variant content/arcs/nested (legacy)]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer("#usda 1.0\ndef Xform \"Base\" { float z = 9 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"p\" (variants = { string s = \"on\" } "
      "prepend variantSets = [\"s\"]) {\n"
      "    variantSet \"s\" = {\n"
      "        \"on\" (prepend references = @./base.usda@</Base>\n"
      "                variants = { string inner = \"i2\" }) {\n"
      "            float a = 1\n"
      "            def Mesh \"Extra\" { float b = 2 }\n"
      "            variantSet \"inner\" = { \"i1\" { float n = 1 } "
      "\"i2\" { float n = 2 } }\n"
      "        }\n"
      "        \"off\" (active = false) { }\n"
      "    }\n"
      "}\n"
      "def Xform \"gone\" (variants = { string s = \"off\" } "
      "prepend variantSets = [\"s\"]) {\n"
      "    variantSet \"s\" = { \"on\" { } \"off\" (active = false) { } }\n"
      "}\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  const Value* a = PropOf(*out, "/p", "a");
  CHECK(a && a->as_float() && *a->as_float() == 1.0f, "inline prop (a=1)");
  CHECK(PropOf(*out, "/p/Extra", "b") != nullptr,
        "variant child prim grafted (usda content)");
  const Value* z = PropOf(*out, "/p", "z");
  CHECK(z && z->as_float() && *z->as_float() == 9.0f,
        "option-metadata reference arc composed (z=9)");
  const Value* n = PropOf(*out, "/p", "n");
  CHECK(n && n->as_float() && *n->as_float() == 2.0f,
        "nested variant applied (n=2)");

  CompositionOptions override_options;
  override_options.variant_overrides["inner"] = "i1";
  Compositor overridden;
  overridden.SetOptions(override_options);
  overridden.SetLayerLoader(loader);
  auto override_out = overridden.Compose(*root);
  const Value* overridden_n =
      override_out ? PropOf(*override_out, "/p", "n") : nullptr;
  CHECK(overridden_n && overridden_n->as_float() &&
            *overridden_n->as_float() == 1.0f,
        "caller override wins over nested authored selection");

  const PrimSpec* gone = out->prim_at_path("/gone");
  CHECK(gone && !gone->meta().active,
        "variant active=false deactivates host");
}

// Authored active=false must survive a weaker reference (fill-absent on
// AUTHORED opinions only; a weaker default used to flip it back).
static void test_active_authored() {
  std::cout << "[active authored]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer("#usda 1.0\ndef Xform \"R\" { float x = 1 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\ndef Xform \"A\" (active = false\n"
      " prepend references = @./r.usda@</R>) { }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  const PrimSpec* a = out ? out->prim_at_path("/A") : nullptr;
  CHECK(a && !a->meta().active,
        "authored active=false survives weaker reference");
  CHECK(a && PropOf(*out, "/A", "x") != nullptr, "reference still composes");
}

// A sublayer-defined prim's reference must survive a stronger layer's `over`
// for the same prim (arc merge across the layer stack).
static void test_cross_layer_arc_merge() {
  std::cout << "[cross-layer arc merge]\n";
  auto loader = [&](const std::string& path,
                    std::string*) -> std::unique_ptr<Layer> {
    if (path.find("weak") != std::string::npos) {
      return ParseLayer(
          "#usda 1.0\ndef Xform \"p\" "
          "(prepend references = @./r.usda@</R>) { }\n");
    }
    return ParseLayer("#usda 1.0\ndef Xform \"R\" { float x = 5 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n(\n subLayers = [ @./weak.usda@ ]\n)\n"
      "over \"p\" { float o = 1 }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  const Value* x = out ? PropOf(*out, "/p", "x") : nullptr;
  CHECK(x && x->as_float() && *x->as_float() == 5.0f,
        "sublayer reference survives stronger over (x=5)");
  const Value* o = out ? PropOf(*out, "/p", "o") : nullptr;
  CHECK(o && o->as_float() && *o->as_float() == 1.0f, "over opinion kept");
}


// 2026-07 composition/flatten audit regressions (legacy Compositor engine).
static void test_audit_2026_07() {
  std::cout << "[2026-07 audit regressions]\n";

  // --- Sublayer cycle: must not crash / recurse unbounded; the rest of the
  // scene still composes.
  {
    auto loader = [&](const std::string& path,
                      std::string*) -> std::unique_ptr<Layer> {
      if (path.find("cyc_b") != std::string::npos) {
        return ParseLayer(
            "#usda 1.0\n(\n subLayers = [ @./cyc_a.usda@ ]\n)\n"
            "def Xform \"B\" { int y = 2 }\n");
      }
      return ParseLayer(
          "#usda 1.0\n(\n subLayers = [ @./cyc_b.usda@ ]\n)\n"
          "def Xform \"A\" { int x = 1 }\n");
    };
    auto root = ParseLayer(
        "#usda 1.0\n(\n subLayers = [ @./cyc_b.usda@ ]\n)\n"
        "def Xform \"A\" { int x = 1 }\n");
    Compositor comp;
    comp.SetLayerLoader(loader);
    auto out = comp.Compose(*root);
    CHECK(out != nullptr, "cycle: compose survives");
    CHECK(out && PropOf(*out, "/B", "y") != nullptr, "cycle: other layer composed");
  }

  // --- Sublayer layer offsets applied to time samples (offset=10, scale=2).
  {
    auto loader = [&](const std::string&,
                      std::string*) -> std::unique_ptr<Layer> {
      return ParseLayer(
          "#usda 1.0\nover \"R\" { float v.timeSamples = { 0: 100, 5: 200 } }\n");
    };
    auto root = ParseLayer(
        "#usda 1.0\n(\n subLayers = [ @./anim.usda@ (offset = 10; scale = 2) ]\n)\n"
        "def Xform \"R\" { }\n");
    Compositor comp;
    comp.SetLayerLoader(loader);
    auto out = comp.Compose(*root);
    CHECK(out != nullptr, "sublayer offset: compose succeeds");
    const PrimSpec* r = out ? out->prim_at_path("/R") : nullptr;
    const auto* ts = r ? r->time_samples(GetPropNameTable().intern("v")) : nullptr;
    CHECK(ts && ts->size() == 2, "sublayer offset: samples present");
    CHECK(ts && (*ts)[0].first == 10.0 && (*ts)[1].first == 20.0,
          "sublayer offset: t -> t*scale + offset");
  }

  // --- A stronger authored default blocks a weaker layer's timeSamples, and
  // weaker property metadata (interpolation) fills absent fields.
  {
    auto loader = [&](const std::string&,
                      std::string*) -> std::unique_ptr<Layer> {
      return ParseLayer(
          "#usda 1.0\nover \"M\" {\n"
          "  double anim = 99\n"
          "  double anim.timeSamples = { 1: 1, 2: 2 }\n"
          "  float2[] primvars:st = [(0, 0)] ( interpolation = \"vertex\" )\n"
          "}\n");
    };
    auto root = ParseLayer(
        "#usda 1.0\n(\n subLayers = [ @./weak.usda@ ]\n)\n"
        "def Mesh \"M\" {\n"
        "  double anim = 42\n"
        "  float2[] primvars:st = [(1, 1)]\n"
        "}\n");
    Compositor comp;
    comp.SetLayerLoader(loader);
    auto out = comp.Compose(*root);
    const PrimSpec* m = out ? out->prim_at_path("/M") : nullptr;
    CHECK(m != nullptr, "default-vs-samples: prim composes");
    const Value* av = m ? m->property_value("anim") : nullptr;
    CHECK(av && av->as_double() && *av->as_double() == 42.0,
          "stronger default wins");
    CHECK(m && !m->has_time_samples(GetPropNameTable().intern("anim")),
          "weaker samples blocked by stronger default");
    const PropMeta* pm = m ? m->property_meta("primvars:st") : nullptr;
    CHECK(pm && (pm->authored & PropMeta::kInterpolation) &&
              pm->interpolation == "vertex",
          "weaker interpolation fills absent metadata");
  }

  // --- `delete references` in the root removes a weaker sublayer's reference.
  {
    auto loader = [&](const std::string& path,
                      std::string*) -> std::unique_ptr<Layer> {
      if (path.find("sub") != std::string::npos) {
        return ParseLayer(
            "#usda 1.0\ndef Xform \"M\" (\n"
            "  references = [ @./ra.usda@, @./rb.usda@ ]\n) { }\n");
      }
      if (path.find("ra") != std::string::npos) {
        return ParseLayer(
            "#usda 1.0\n(\n defaultPrim = \"P\"\n)\n"
            "def Xform \"P\" { int fromA = 1\n string src = \"a\" }\n");
      }
      return ParseLayer(
          "#usda 1.0\n(\n defaultPrim = \"P\"\n)\n"
          "def Xform \"P\" { int fromB = 1\n string src = \"b\" }\n");
    };
    auto root = ParseLayer(
        "#usda 1.0\n(\n subLayers = [ @./sub.usda@ ]\n)\n"
        "over \"M\" (\n  delete references = @./ra.usda@\n) { }\n");
    Compositor comp;
    comp.SetLayerLoader(loader);
    auto out = comp.Compose(*root);
    const PrimSpec* m = out ? out->prim_at_path("/M") : nullptr;
    CHECK(m != nullptr, "delete refs: prim composes");
    CHECK(m && m->property_value("fromB") != nullptr, "kept ref composes");
    CHECK(m && m->property_value("fromA") == nullptr,
          "deleted ref does not compose");
  }

  // --- Variant set and selection split across layers compose.
  {
    auto loader = [&](const std::string&,
                      std::string*) -> std::unique_ptr<Layer> {
      return ParseLayer(
          "#usda 1.0\nover \"M\" (\n"
          "  variants = { string look = \"red\" }\n) { }\n");
    };
    auto root = ParseLayer(
        "#usda 1.0\n(\n subLayers = [ @./sel.usda@ ]\n)\n"
        "def Xform \"M\" (\n  prepend variantSets = \"look\"\n) {\n"
        "  variantSet \"look\" = {\n"
        "    \"red\" { string color = \"red\" }\n"
        "    \"blue\" { string color = \"blue\" }\n"
        "  }\n}\n");
    Compositor comp;
    comp.SetLayerLoader(loader);
    auto out = comp.Compose(*root);
    const PrimSpec* m = out ? out->prim_at_path("/M") : nullptr;
    const Value* c = m ? m->property_value("color") : nullptr;
    CHECK(c && c->as_string() && *c->as_string() == "red",
          "cross-layer variant set/selection composes");
    CHECK(m && m->meta().variantSets().empty(),
          "baked variant metadata cleared");
  }
}


// 2026-07 composition audit: sublayer stage-metadata gap-fill (root wins,
// strongest sublayer fills unauthored fields) and prim-scoped variant
// overrides ("<primPath>{<set>}" beats the bare-set key).
static void test_audit_stage_meta_and_variant_overrides() {
  std::cout << "[stage-meta gap-fill + prim-scoped variant overrides]\n";
  auto loader = [&](const std::string& path,
                    std::string*) -> std::unique_ptr<Layer> {
    if (path.find("sub.usda") != std::string::npos) {
      return ParseLayer(
          "#usda 1.0\n(\n    upAxis = \"Z\"\n"
          "    metersPerUnit = 0.01\n    timeCodesPerSecond = 30\n)\n"
          "def Xform \"FromSub\" { }\n");
    }
    return nullptr;
  };
  auto root = ParseLayer(
      "#usda 1.0\n(\n    upAxis = \"Y\"\n"
      "    subLayers = [@./sub.usda@]\n)\n"
      "def Xform \"A\" (variants = { string shape = \"s1\" } "
      "prepend variantSets = [\"shape\"]) {\n"
      "    variantSet \"shape\" = { \"s1\" { int v = 1 } "
      "\"s2\" { int v = 2 } }\n"
      "}\n"
      "def Xform \"B\" (variants = { string shape = \"s1\" } "
      "prepend variantSets = [\"shape\"]) {\n"
      "    variantSet \"shape\" = { \"s1\" { int v = 1 } "
      "\"s2\" { int v = 2 } }\n"
      "}\n");

  // Root-authored upAxis wins; sublayer-authored stage metadata is IGNORED
  // (pxr parity: stage fields resolve from the root layer only). Prim-scoped
  // override flips only /B to s2.
  CompositionOptions opts;
  opts.variant_overrides["/B{shape}"] = "s2";
  Compositor comp;
  comp.SetOptions(opts);
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  CHECK(out->meta().upAxis == "Y" && out->meta().upAxis_set,
        "root upAxis wins over sublayer");
  CHECK(!out->meta().timeCodesPerSecond_set,
        "sublayer tCPS does NOT gap-fill (root-layer-only, pxr parity)");
  CHECK(!out->meta().metersPerUnit_set,
        "sublayer mPU does NOT gap-fill (root-layer-only, pxr parity)");
  const Value* va = PropOf(*out, "/A", "v");
  CHECK(va && va->as_int() && *va->as_int() == 1,
        "/A keeps authored selection (s1)");
  const Value* vb = PropOf(*out, "/B", "v");
  CHECK(vb && vb->as_int() && *vb->as_int() == 2,
        "/B prim-scoped override applies (s2)");

  // Bare-set key still applies stage-wide.
  CompositionOptions opts2;
  opts2.variant_overrides["shape"] = "s2";
  Compositor comp2;
  comp2.SetOptions(opts2);
  comp2.SetLayerLoader(loader);
  auto out2 = comp2.Compose(*root);
  CHECK(out2 != nullptr, "compose 2 succeeds");
  const Value* va2 = PropOf(*out2, "/A", "v");
  const Value* vb2 = PropOf(*out2, "/B", "v");
  CHECK(va2 && va2->as_int() && *va2->as_int() == 2 && vb2 &&
            vb2->as_int() && *vb2->as_int() == 2,
        "bare-set override applies to both prims");
}


// apiSchemas list-op merge across arcs: a prepend-qualified stronger list
// composes IN FRONT of the weaker layer's schemas instead of replacing them.
static void test_apischemas_cross_arc_merge() {
  std::cout << "[apiSchemas cross-arc merge]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"M\" (prepend apiSchemas = [\"PhysicsRigidBodyAPI\"]) "
        "{ }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"M\" (prepend references = @./base.usda@</M>\n"
      "                 prepend apiSchemas = [\"PhysicsMassAPI\"]) { }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  const PrimSpec* m = out->prim_at_path("/M");
  CHECK(m != nullptr, "prim exists");
  const auto& schemas = m->meta().apiSchemas();
  CHECK(schemas.size() == 2 && schemas[0] == "PhysicsMassAPI" &&
            schemas[1] == "PhysicsRigidBodyAPI",
        "stronger prepend merges in front of weaker schemas");
}

// P2 audit: a reference that names no prim path to a layer with no authored
// defaultPrim contributes NOTHING (pxr: "Unresolved reference prim path
// @...@<defaultPrim>" warning + empty prim) — never silently the first root.
static void test_audit_p2_ref_no_default_prim() {
  std::cout << "[P2: reference without prim path or defaultPrim]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"First\" { double size = 1 }\n"
        "def Xform \"Second\" { double size = 2 }\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"Hello\" (prepend references = @./lib.usda@) { }\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds (arc dropped, not fatal)");
  CHECK(out->prim_at_path("/Hello") != nullptr, "referencing prim kept");
  CHECK(PropOf(*out, "/Hello", "size") == nullptr,
        "no silent fallback to the first root prim");
  bool has_msg = false;
  for (const auto& e : comp.GetErrors()) {
    if (e.message.find("defaultPrim") != std::string::npos) has_msg = true;
  }
  CHECK(has_msg, "diagnostic mentions the missing defaultPrim");
}

// P2 audit: children of a deactivated prim are pruned from the final flatten
// (pxr composes no subtree under active=false; the prim itself is kept here
// with its authored active=false). A stronger layer re-activating the prim
// keeps the subtree.
static void test_audit_p2_inactive_subtree_pruned() {
  std::cout << "[P2: inactive prim subtree pruned]\n";
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"A\" (active = false) {\n"
      "    def Sphere \"B\" { double radius = 3 }\n"
      "}\n"
      "def Xform \"C\" { double v = 1 }\n");
  Compositor comp;
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  const PrimSpec* a = out->prim_at_path("/A");
  CHECK(a && a->meta().active_authored && !a->meta().active,
        "inactive prim kept with active=false");
  CHECK(out->prim_at_path("/A/B") == nullptr,
        "child of inactive prim pruned");
  CHECK(out->prim_at_path("/C") != nullptr, "sibling untouched");

  // Re-activation: the root's authored active=true is stronger than the
  // referenced layer's active=false — the subtree must survive.
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"R\" (active = false) {\n"
        "    def Sphere \"S\" { double radius = 1 }\n"
        "}\n");
  };
  auto root2 = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"P\" (active = true\n"
      "                 prepend references = @./lib.usda@</R>) { }\n");
  Compositor comp2;
  comp2.SetLayerLoader(loader);
  auto out2 = comp2.Compose(*root2);
  CHECK(out2 != nullptr, "compose 2 succeeds");
  const PrimSpec* p = out2->prim_at_path("/P");
  CHECK(p && p->meta().active, "stronger active=true wins");
  CHECK(out2->prim_at_path("/P/S") != nullptr,
        "re-activated prim keeps its referenced subtree");
}

// P2 audit: a stronger spec's authored DEFAULT blocks a weaker spec's
// timeSamples even when the two defaults happen to be VALUE-EQUAL (the old
// check compared values, so equal defaults let the weaker samples through).
static void test_audit_p2_default_blocks_equal_samples() {
  std::cout << "[P2: equal-value default still blocks weaker samples]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"P\" {\n"
        "    double x = 5\n"
        "    double x.timeSamples = { 1: 10, 2: 20 }\n"
        "    double y = 7\n"
        "    double y.timeSamples = { 1: 70 }\n"
        "}\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n"
      "def Xform \"P\" (prepend references = @./weak.usda@</P>) {\n"
      "    double x = 5\n"
      "}\n");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  const PrimSpec* p = out->prim_at_path("/P");
  CHECK(p != nullptr, "prim composed");
  const Value* x = PropOf(*out, "/P", "x");
  CHECK(x && x->as_double() && *x->as_double() == 5.0, "default kept (5)");
  PropNameId xid = GetPropNameTable().find("x");
  CHECK(p && !p->has_time_samples(xid),
        "stronger equal-value default blocks weaker samples");
  // Control: a property the stronger spec does NOT author rides through
  // whole (default + samples from the weaker spec).
  PropNameId yid = GetPropNameTable().find("y");
  const auto* yts = p ? p->time_samples(yid) : nullptr;
  CHECK(yts && yts->size() == 1,
        "unblocked property keeps its samples");
}

// A weaker layer's dictionary entry shadowed by a stronger scalar (or vice
// versa) is a TYPE CONFLICT: composition keeps the stronger opinion but must
// surface a diagnostic instead of silently dropping the weaker subtree.
static void test_dictionary_type_conflict_diagnostic() {
  std::cout << "[dictionary type-conflict diagnostic]\n";
  auto loader = [&](const std::string&,
                    std::string*) -> std::unique_ptr<Layer> {
    return ParseLayer(
        "#usda 1.0\n"
        "def Xform \"p\" (customData = { dictionary k = { int a = 1 } }) {}\n");
  };
  auto root = ParseLayer(
      "#usda 1.0\n(\n subLayers = [ @./weak.usda@ ]\n)\n"
      "def Xform \"p\" (customData = { int k = 3 }) {}\n");
  CHECK(root != nullptr, "root parses");
  Compositor comp;
  comp.SetLayerLoader(loader);
  auto out = comp.Compose(*root);
  CHECK(out != nullptr, "compose succeeds");
  const PrimSpec* p = out->prim_at_path("/p");
  CHECK(p != nullptr, "prim composed");
  auto cd = p ? p->meta().customData().as_dictionary() : nullptr;
  auto k = cd ? cd->find("k") : nullptr;
  CHECK(k && k->as_int() && *k->as_int() == 3,
        "stronger scalar opinion wins the conflicting key");
  bool saw = false;
  for (const CompositionError& e : comp.GetErrors()) {
    if (e.message.find("Dictionary type conflict") != std::string::npos &&
        e.message.find("customData.k") != std::string::npos) {
      saw = true;
    }
  }
  CHECK(saw, "type conflict surfaces a diagnostic naming the key");
}

int main() {
  test_inherits();
  test_internal_reference();
  test_external_reference();
  test_specializes();
  test_cycle_safety();
  test_transitive_reference();
  test_variants();
  test_variants_multiset();
  test_variant_subprim();
  test_variant_roundtrip();
  test_variant_selection_over_reference();
  test_many_variant_bearing_references();
  test_variant_ref_payload_chain();
  test_extref_self_contained_subtree();
  test_extref_non_self_contained_fallback();
  test_variant_selected_field_over_reference();
  test_sublayer_merge();
  test_dictionary_type_conflict_diagnostic();
  test_livrps_strength();
  test_graft_retargeting();
  test_layer_offset_baking();
  test_variant_content_legacy();
  test_active_authored();
  test_cross_layer_arc_merge();
  test_audit_2026_07();
  test_audit_stage_meta_and_variant_overrides();
  test_apischemas_cross_arc_merge();
  test_audit_p2_ref_no_default_prim();
  test_audit_p2_inactive_subtree_pruned();
  test_audit_p2_default_blocks_equal_samples();

  if (g_fail) {
    std::cerr << "\n" << g_fail << " composition check(s) FAILED\n";
    return 1;
  }
  std::cout << "\nAll composition checks passed.\n";
  return 0;
}
