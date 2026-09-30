// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Test for the native PCP-style lazy composition cache (next/pcp).
// Phase 1: internal references + lazy ComputePrimIndex + BuildStage.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include "next/eval/attribute-eval.hh"
#include "next/layer/asset-anchor.hh"
#include "next/layer/layer.hh"
#include "next/load-usd.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"

using namespace lightusd::next;

// Always-on check: this file's build type is Release for the ctest gate, where
// assert() is compiled out (-DNDEBUG). New assertions that must genuinely gate
// use PCP_CHECK, which records a failure and is honored by main()'s exit code.
static int g_pcp_check_failures = 0;
#define PCP_CHECK(cond, msg)                                              \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "PCP_CHECK FAILED: " << (msg) << " @ " << __FILE__     \
                << ":" << __LINE__ << std::endl;                          \
      ++g_pcp_check_failures;                                             \
    }                                                                     \
  } while (0)

static const std::string& PcpScratchDirectory() {
  static const std::string directory = [] {
    char pattern[] = "/tmp/lightusd-next-pcp-suite-XXXXXX";
    char* created = ::mkdtemp(pattern);
    return created ? std::string(created) : std::string("/tmp");
  }();
  return directory;
}

static std::string PcpScratchPath(const char* name) {
  return PcpScratchDirectory() + "/" + name;
}

// Build an in-memory root layer:
//   /World (Xform)
//     /A (Xform)  -> references </Lib/Model>
//   /Lib (Scope)
//     /Model (Mesh, size=float3) { /Inner (Sphere) }
static std::shared_ptr<Layer> BuildRootLayer() {
  Layer layer;
  LayerBuilder lb(layer);

  lb.begin_prim("World", "Xform");
  // Typeless so the referenced prim's type ("Mesh") composes through (a local
  // typeName would correctly win over the reference).
  lb.begin_prim("A", "");
  // Internal reference to /Lib/Model in the same layer.
  lb.current()->meta().references.push_back("</Lib/Model>");
  lb.end_prim();  // A
  // P carries a PAYLOAD (deferrable) to the same Model.
  lb.begin_prim("P", "");
  lb.current()->meta().payloads.push_back("</Lib/Model>");
  lb.end_prim();  // P
  // I inherits the class (provides type "Scope").
  lb.begin_prim("I", "");
  lb.current()->meta().inherits.push_back("</_class_Base>");
  lb.end_prim();  // I
  // IR inherits the class AND references RefModel (Mesh): inherit (Scope) is
  // STRONGER than the reference (Mesh), so composed type == "Scope".
  lb.begin_prim("IR", "");
  lb.current()->meta().inherits.push_back("</_class_Base>");
  lb.current()->meta().references.push_back("</Lib/RefModel>");
  lb.end_prim();  // IR
  // SP references RefModel (Mesh) AND specializes the class (Scope):
  // specialize is globally WEAKEST, so the reference (Mesh) wins.
  lb.begin_prim("SP", "");
  lb.current()->meta().references.push_back("</Lib/RefModel>");
  lb.current()->meta().specializes.push_back("</_class_Base>");
  lb.end_prim();  // SP
  // V has a variant set "vset" with selection "high".
  lb.begin_prim("V", "");
  {
    VariantSetData vss;
    vss.name = "vset";
    VariantData hi;
    hi.name = "high";
    hi.properties.push_back({"variantHigh", Value::MakeFloat3(9, 9, 9)});
    VariantData lo;
    lo.name = "low";
    lo.properties.push_back({"variantLow", Value::MakeFloat3(1, 1, 1)});
    vss.variants.push_back(std::move(hi));
    vss.variants.push_back(std::move(lo));
    lb.current()->meta().variantSets().push_back(std::move(vss));
    lb.current()->meta().variantSelection = "vset=high";
  }
  lb.end_prim();  // V
  // VC has a variant whose CONTENT subtree adds a host property + a child prim.
  {
    auto content = std::make_shared<Layer>();
    LayerBuilder cb(*content);
    cb.begin_prim("__self__", "");
    cb.add_property("hostProp", Value::MakeFloat3(7, 7, 7));
    cb.begin_prim("Geom", "Mesh");
    cb.end_prim();  // Geom
    cb.end_prim();  // __self__
    cb.finalize();

    lb.begin_prim("VC", "");
    VariantSetData vss;
    vss.name = "geo";
    VariantData full;
    full.name = "full";
    full.content = content;
    vss.variants.push_back(std::move(full));
    lb.current()->meta().variantSets().push_back(std::move(vss));
    lb.current()->meta().variantSelection = "geo=full";
    lb.end_prim();  // VC
  }
  // MV selects TWO variant sets at once via variantSelections.
  {
    lb.begin_prim("MV", "");
    VariantSetData s1;
    s1.name = "s1";
    VariantData s1on;
    s1on.name = "on";
    s1on.properties.push_back({"p1", Value::MakeFloat3(1, 1, 1)});
    s1.variants.push_back(std::move(s1on));
    VariantSetData s2;
    s2.name = "s2";
    VariantData s2on;
    s2on.name = "on";
    s2on.properties.push_back({"p2", Value::MakeFloat3(2, 2, 2)});
    s2.variants.push_back(std::move(s2on));
    lb.current()->meta().variantSets().push_back(std::move(s1));
    lb.current()->meta().variantSets().push_back(std::move(s2));
    lb.current()->meta().variantSelections().push_back({"s1", "on"});
    lb.current()->meta().variantSelections().push_back({"s2", "on"});
    lb.end_prim();  // MV
  }
  // Three instanceable prims: Inst1/Inst2 reference the same asset (one
  // prototype, shared); Inst3 references a different asset (separate prototype).
  lb.begin_prim("Inst1", "");
  lb.current()->meta().references.push_back("</Lib/Model>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.begin_prim("Inst2", "");
  lb.current()->meta().references.push_back("</Lib/Model>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.begin_prim("Inst3", "");
  lb.current()->meta().references.push_back("</Lib/RefModel>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  // Relocate an ARC-INTRODUCED prim (pxr only relocates prims delivered
  // across a composition arc on an ANCESTOR): RigHost references /Lib/Model,
  // whose child Inner (Sphere) is relocated to /World/New.
  lb.current()->meta().relocates().push_back(
      {"/World/RigHost/Inner", "/World/New"});
  lb.begin_prim("RigHost", "");
  lb.current()->meta().references.push_back("</Lib/Model>");
  lb.end_prim();  // RigHost
  lb.end_prim();  // World

  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Model", "Mesh");
  lb.add_property("size", Value::MakeFloat3(1.0f, 2.0f, 3.0f));
  lb.add_relationship("inside", Path("/Lib/Model/Inner"));
  lb.begin_prim("Inner", "Sphere");
  lb.end_prim();  // Inner
  lb.end_prim();  // Model
  lb.begin_prim("RefModel", "Mesh");
  lb.end_prim();  // RefModel
  lb.end_prim();  // Lib

  // Abstract class used by inherits/specializes (type "Scope").
  lb.begin_prim("_class_Base", "Scope", PrimSpecifier::Class);
  lb.end_prim();  // _class_Base

  lb.finalize();
  return std::make_shared<Layer>(std::move(layer));
}

static void test_compute_prim_index() {
  std::cout << "test_compute_prim_index..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();

  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened && "Cache::Open failed");
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  const pcp::PrimIndex *idx = cache.ComputePrimIndex(Path("/World/A"), &warn, &err);
  assert(idx != nullptr && "no PrimIndex for /World/A");
  // Root node + one reference node (to /Lib/Model).
  assert(idx->GetNodeCount() >= 2);
  assert(idx->HasAnySpecs());
  std::cout << idx->DumpToString();

  // Caching: second query returns the same pointer.
  const pcp::PrimIndex *idx2 = cache.ComputePrimIndex(Path("/World/A"), &warn, &err);
  assert(idx == idx2);
  assert(cache.HasComputedPrimIndex(Path("/World/A")));

  // A prim with no local spec returns nullptr (phase-1 behavior).
  assert(cache.ComputePrimIndex(Path("/Nope"), &warn, &err) == nullptr);

  std::cout << "  OK" << std::endl;
}

// Phase 10: Cache::ComposePrim lazily composes one prim (reusing the same
// source-merge as BuildStage) and must agree with the eagerly-built stage on
// type, properties, and child names -- without ever calling BuildStage.
static void test_compose_prim_lazy() {
  std::cout << "test_compose_prim_lazy..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  // Reference stage built eagerly.
  auto root_a = BuildRootLayer();
  auto oa = pcp::Cache::Open(resolver, root_a);
  assert(oa);
  pcp::Cache eager = std::move(*oa);
  Stage stage;
  assert(eager.BuildStage(&stage, &warn, &err));

  // Lazy cache: never BuildStage'd.
  auto root_b = BuildRootLayer();
  auto ob = pcp::Cache::Open(resolver, root_b);
  assert(ob);
  pcp::Cache lazy = std::move(*ob);

  // /World/A: type "Mesh" + "size" both come through the reference, lazily.
  const PrimSpec *a = lazy.ComposePrim(Path("/World/A"), &warn, &err);
  assert(a && "ComposePrim(/World/A) failed");
  UsdPrim sa = stage.GetPrimAtPath("/World/A");
  assert(a->type_name() == sa.GetTypeName() && a->type_name() == "Mesh");
  assert((a->property_value("size") != nullptr) ==
         (sa.GetPropertyValue("size") != nullptr));
  assert(a->property_value("size") != nullptr);

  // /World/A/Inner: a child reached through the reference composes lazily too.
  const PrimSpec *inner = lazy.ComposePrim(Path("/World/A/Inner"), &warn, &err);
  assert(inner && inner->type_name() == "Sphere");

  // Composed child names match the eager stage for this non-instance prim.
  std::vector<std::string> kids =
      lazy.ComposedChildNames(Path("/World/A"), &warn, &err);
  bool has_inner = false;
  for (const std::string &k : kids) if (k == "Inner") has_inner = true;
  assert(has_inner && "ComposedChildNames missed the referenced child");

  // Caching: a second compose returns the same borrowed pointer.
  assert(lazy.ComposePrim(Path("/World/A"), &warn, &err) == a);
  // A path with no opinions composes to nullptr.
  assert(lazy.ComposePrim(Path("/Nope"), &warn, &err) == nullptr);

  // Invalidate drops the lazily-composed spec; a recompose still yields a
  // valid, correctly-composed spec. (Pointer identity is not asserted here: the
  // freed spec's heap address may be reused by the recomposed one.)
  lazy.Invalidate(Path("/World/A"));
  const PrimSpec *a2 = lazy.ComposePrim(Path("/World/A"), &warn, &err);
  assert(a2 && a2->type_name() == "Mesh" && a2->property_value("size") &&
         "recompose after Invalidate must yield a valid spec");
  // And it is cached again (repeat returns the same borrowed pointer).
  assert(lazy.ComposePrim(Path("/World/A"), &warn, &err) == a2);
  std::cout << "  OK" << std::endl;
}

// pxr composes namespace child ORDER weak->strong (PcpComposeSiteChildNames):
// each arc/layer appends its unseen child names weakest-first, so a reference's
// children precede local-only children. Here /Inst references /Lib/Asset (kids
// [X, Materials]) and locally authors [X (over), LocalOnly]; the composed order
// must be [X, Materials, LocalOnly] -- NOT the strong-first [X, LocalOnly,
// Materials]. Verified against `usdcat --flatten`.
static void test_child_order_weak_to_strong() {
  std::cout << "test_child_order_weak_to_strong..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  Layer layer;
  LayerBuilder lb(layer);
  // Referenced asset with children [X, Materials].
  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Asset", "");
  lb.begin_prim("X", "Scope");
  lb.end_prim();  // X
  lb.begin_prim("Materials", "Scope");
  lb.end_prim();  // Materials
  lb.end_prim();  // Asset
  lb.end_prim();  // Lib
  // Inst references the asset; local children [X (over), LocalOnly].
  lb.begin_prim("Inst", "");
  lb.current()->meta().references.push_back("</Lib/Asset>");
  lb.begin_prim("X", "");  // local override of the referenced X
  lb.end_prim();
  lb.begin_prim("LocalOnly", "Scope");
  lb.end_prim();
  lb.end_prim();  // Inst
  lb.finalize();
  auto root = std::make_shared<Layer>(std::move(layer));

  auto o = pcp::Cache::Open(resolver, root);
  assert(o);
  pcp::Cache cache = std::move(*o);

  std::vector<std::string> kids =
      cache.ComposedChildNames(Path("/Inst"), &warn, &err);
  assert(kids.size() == 3 && "Inst should have 3 children");
  assert(kids[0] == "X" && "common child X first (reference position)");
  assert(kids[1] == "Materials" &&
         "reference-only Materials before local-only (weak->strong)");
  assert(kids[2] == "LocalOnly" && "local-only child last");
  std::cout << "  OK" << std::endl;
}

// Crate-authored variants: pxr crates store variant content as bracketed HOLDER
// prims ("/Asset/{LOD=high}/LOD0") with a VariantSetData carrying only
// {name, selected} (no inline content). The compositor must graft the selected
// holder's descendants onto the host. Combined with a reference + local overrides
// this reproduces a real UE-exported case: asset locally [Materials] +
// variant [LOD0]; host references it and authors over LOD0 + def LOD1 -> the
// composed order is [LOD0, Materials, LOD1] (variant child first, weak->strong),
// and the "{LOD=high}" marker must NOT appear as a child.
static void test_crate_style_variant_holder() {
  std::cout << "test_crate_style_variant_holder..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  Layer layer;
  LayerBuilder lb(layer);
  // Referenced asset with a crate-style selected variant set.
  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Asset", "");
  {
    VariantSetData vss;
    vss.name = "LOD";
    vss.selected = "high";  // crate form: selection only, no inline content
    lb.current()->meta().variantSets().push_back(std::move(vss));
  }
  lb.begin_prim("Materials", "Scope");  // asset-local child
  lb.end_prim();
  lb.begin_prim("{LOD=high}", "");  // bracketed variant holder
  lb.begin_prim("LOD0", "Scope");   // variant content child
  lb.end_prim();
  lb.end_prim();  // {LOD=high}
  lb.end_prim();  // Asset
  lb.end_prim();  // Lib
  // Host references the asset and locally authors over LOD0 + def LOD1.
  lb.begin_prim("Inst", "");
  lb.current()->meta().references.push_back("</Lib/Asset>");
  lb.begin_prim("LOD0", "");  // local override of the variant child
  lb.end_prim();
  lb.begin_prim("LOD1", "Scope");  // local-only child
  lb.end_prim();
  lb.end_prim();  // Inst
  lb.finalize();
  auto root = std::make_shared<Layer>(std::move(layer));

  auto o = pcp::Cache::Open(resolver, root);
  assert(o);
  pcp::Cache cache = std::move(*o);

  std::vector<std::string> kids =
      cache.ComposedChildNames(Path("/Inst"), &warn, &err);
  // No bracketed marker child; variant content grafted; weak->strong order.
  for (const std::string &k : kids)
    assert(k.front() != '{' && "variant holder marker leaked as a child");
  assert(kids.size() == 3 && "Inst should have LOD0, Materials, LOD1");
  assert(kids[0] == "LOD0" && "variant child LOD0 first");
  assert(kids[1] == "Materials" && "asset-local Materials second");
  assert(kids[2] == "LOD1" && "host-local LOD1 last");

  // Flatten must DROP variant-selection metadata: the selected variant's content
  // is grafted inline, so pxr's flattened output carries no `variants = {...}` /
  // variantSets (usdcat emits zero). Build + write and confirm none leak.
  Stage stage;
  assert(cache.BuildStage(&stage, &warn, &err));
  std::string usda = WriteUSDAToString(stage);
  assert(usda.find("variants = {") == std::string::npos &&
         "variant selection metadata must not survive flatten");
  assert(usda.find("variantSet") == std::string::npos &&
         "variantSet metadata/property must not survive flatten");
  // The selected variant's content (LOD0) is still present inline.
  assert(usda.find("\"LOD0\"") != std::string::npos &&
         "selected variant content missing from flattened output");
  std::cout << "  OK" << std::endl;
}

// USDA-authored `<name>.connect = </path>` must parse as a CONNECTION (stored in
// the connection map, re-emitted as `.connect`), not as a string value
// (`float x = "/path"`, invalid USDA). And `custom rel <name> = </path>` must
// parse as a qualified relationship, not choke ("Expected attribute type").
static void test_usda_connection_and_custom_rel() {
  std::cout << "test_usda_connection_and_custom_rel..." << std::endl;
  const std::string f = "/tmp/next_usda_conn.usda";
  {
    std::ofstream o(f);
    o << "#usda 1.0\n"
         "def Shader \"S\"\n{\n"
         "    float inputs:metallic.connect = </Other.inputs:metallic>\n"
         "    custom rel field:density = </Vol/density>\n"
         "}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  // custom rel must not fail the parse.
  assert(pcp::ComposeStageFromFile(f, resolver, &stage, {}, &warn, &err));
  std::string usda = WriteUSDAToString(stage);
  assert(usda.find("inputs:metallic.connect = </Other.inputs:metallic>") !=
             std::string::npos &&
         "connection must re-emit as `.connect`");
  assert(usda.find("inputs:metallic = \"") == std::string::npos &&
         "connection wrongly emitted as a string value");
  assert(usda.find("field:density") != std::string::npos &&
         "custom rel target dropped");
  std::remove(f.c_str());
  std::cout << "  OK" << std::endl;
}

// A composition arc (payload) authored on a selected variant OPTION's metadata
// block must be followed -- XGen-style assets author the geometry-defining
// `def Mesh` in a payload on the variant, not in the variant body. Before the
// fix the parser dropped variant-option arcs, so the prim stayed empty (the
// 13411-specifier Island residual). Relative `@./geo@` must anchor to the
// file that authored the variant.
static void test_variant_option_payload_arc() {
  std::cout << "test_variant_option_payload_arc..." << std::endl;
  const std::string geo = "/tmp/next_varopt_geo.usd";
  const std::string root = "/tmp/next_varopt_root.usda";
  {
    std::ofstream o(geo);
    o << "#usda 1.0\n"
         "def \"geo\"\n{\n"
         "    def Mesh \"shape\"\n    {\n"
         "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
         "    }\n}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def \"Asset\" (\n"
         "    variants = { string model = \"render\" }\n"
         "    prepend variantSets = \"model\"\n"
         ")\n{\n"
         "    variantSet \"model\" = {\n"
         "        \"render\" ( prepend payload = "
         "@./next_varopt_geo.usd@</geo> ) {\n        }\n"
         "        \"proxy\" {\n        }\n"
         "    }\n}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  std::string usda = WriteUSDAToString(stage);
  assert(usda.find("def Mesh \"shape\"") != std::string::npos &&
         "variant-option payload geometry was not composed");
  assert(usda.find("points = [(0, 0, 0)") != std::string::npos &&
         "variant-option payload mesh points missing");
  std::remove(geo.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// A payload authored by a prim inside variant content is anchored to the
// layer that authored the variant, not to the stronger layer that selected it.
// Keep this fixture generic: the real regression is the sublayer directory
// boundary, not an asset-specific prim or property name.
static void test_variant_content_payload_anchor() {
  std::cout << "test_variant_content_payload_anchor..." << std::endl;
  if (::system("mkdir -p /tmp/next_variant_anchor/sub") != 0) { /* ignore */ }
  const std::string root = "/tmp/next_variant_anchor/root.usda";
  const std::string variants = "/tmp/next_variant_anchor/sub/variants.usda";
  const std::string payload = "/tmp/next_variant_anchor/sub/payload.usda";
  {
    std::ofstream o(payload);
    o << "#usda 1.0\n"
         "def \"Payload\"\n"
         "{\n"
         "    def Mesh \"Shape\"\n"
         "    {\n"
         "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
         "    }\n"
         "}\n";
  }
  {
    std::ofstream o(variants);
    o << "#usda 1.0\n"
         "over \"Asset\" {\n"
         "    variantSet \"model\" = {\n"
         "        \"render\" {\n"
         "            def \"Payload\" ( prepend payload = "
         "@./payload.usda@</Payload> ) {}\n"
         "        }\n"
         "    }\n"
         "}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "( subLayers = [@./sub/variants.usda@] )\n"
         "over \"Asset\" ( variants = { string model = \"render\" } ) {}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp/next_variant_anchor");
  Stage stage;
  std::string warn, err;
  PCP_CHECK(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err),
            "variant-content relative payload composition failed: " + err);
  PCP_CHECK(stage.GetPrimAtPath("/Asset/Payload/Shape").IsValid(),
            "variant-content payload did not resolve from its authoring layer");
  std::remove(root.c_str());
  std::remove(variants.c_str());
  std::remove(payload.c_str());
  std::cout << "  OK" << std::endl;
}

// Parallel composition (CompositionOptions::num_threads > 1 -> the BuildStage
// sources pre-warm) must be BYTE-IDENTICAL to a serial build. Use several sibling
// prims referencing a shared asset so the warming's subtree frontier actually
// fans out and the parallel path (workers + merge) runs.
static void test_parallel_compose_byte_identical() {
  std::cout << "test_parallel_compose_byte_identical..." << std::endl;
  const std::string asset = "/tmp/next_pc_asset.usda";
  const std::string root = "/tmp/next_pc_root.usda";
  {
    std::ofstream o(asset);
    o << "#usda 1.0\n"
         "def \"A\"\n{\n"
         "    float inputs:roughness = 0.5\n"
         "    float inputs:metallic = 1\n"
         "    def Mesh \"m\"\n    {\n"
         "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
         "        int[] faceVertexIndices = [0, 1, 2]\n"
         "    }\n}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n";
    for (int i = 0; i < 16; ++i) {
      o << "def \"P" << i << "\" ( prepend references = @./next_pc_asset.usda@</A> )\n"
        << "{\n    float order = " << i << "\n    token tag = \"p" << i << "\"\n}\n";
    }
  }
  auto compose = [&](int nthreads) -> std::string {
    AssetResolver resolver;
    resolver.SetWorkingDirectory("/tmp");
    pcp::CompositionOptions opts;
    opts.num_threads = nthreads;
    Stage stage;
    std::string warn, err;
    assert(pcp::ComposeStageFromFile(root, resolver, &stage, opts, &warn, &err));
    return WriteUSDAToString(stage);
  };
  const std::string serial = compose(1);
  const std::string parallel = compose(4);
  assert(serial == parallel &&
         "parallel compose (--compose-threads) must be byte-identical to serial");
  std::remove(asset.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// An authored value block (`= None`) is a real opinion that blocks weaker values
// and must round-trip as `= None`, NOT collapse to a declared-only attribute
// (the 13.5k-line Island residual: indexed-primvar `:indices = None`).
static void test_value_block_roundtrip() {
  std::cout << "test_value_block_roundtrip..." << std::endl;
  const std::string base = "/tmp/next_block_base.usda";
  const std::string root = "/tmp/next_block_root.usda";
  // A weaker sublayer authors a real index array; the stronger root blocks it.
  { std::ofstream o(base);
    o << "#usda 1.0\ndef Mesh \"M\"\n{\n"
         "    int[] primvars:displayColor:indices = [0, 1, 2]\n}\n"; }
  { std::ofstream o(root);
    o << "#usda 1.0\n(\n    subLayers = [ @./next_block_base.usda@ ]\n)\n"
         "over Mesh \"M\"\n{\n"
         "    int[] primvars:displayColor:indices = None\n}\n"; }
  AssetResolver resolver; resolver.SetWorkingDirectory("/tmp");
  Stage stage; std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  std::string usda = WriteUSDAToString(stage);
  // The block must win over the weaker array and re-emit as `= None`.
  assert(usda.find("int[] primvars:displayColor:indices = None") !=
             std::string::npos &&
         "value block did not re-emit as `= None`");
  assert(usda.find("indices = [0, 1, 2]") == std::string::npos &&
         "value block failed to suppress the weaker array opinion");
  std::remove(base.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// InstanceFlattenMode::ExtractedPrototypes emits usdcat-style flatten: each
// prototype group's shared subtree moves to a root `over "/Flattened_Prototype_N"`
// and every member references it. Numbering is deterministic (sorted by prototype
// path) and reproducible run-to-run. Also covers nesting (a prototype that itself
// contains an instance references another /Flattened_Prototype_M).
static void test_extracted_prototypes() {
  std::cout << "test_extracted_prototypes..." << std::endl;
  const std::string f = PcpScratchPath("next_extract.usda");
  {
    std::ofstream o(f);
    o << "#usda 1.0\n"
         "def \"Lib\" ( active = false )\n{\n"
         "    def Xform \"Leaf\" { def Mesh \"M\" { int[] faceVertexCounts = [3] } }\n"
         "    def Xform \"Cluster\"\n    {\n"
         "        def Xform \"A\" ( instanceable = true "
         "references = </Lib/Leaf> ) {}\n"
         "        def Xform \"B\" ( instanceable = true "
         "references = </Lib/Leaf> ) {}\n    }\n}\n"
         "def Xform \"World\"\n{\n"
         "    def Xform \"L1\" ( instanceable = true references = </Lib/Leaf> ) {}\n"
         "    def Xform \"L2\" ( instanceable = true references = </Lib/Leaf> ) {}\n"
         "    def Xform \"C1\" ( instanceable = true "
         "references = </Lib/Cluster> ) {}\n"
         "    def Xform \"C2\" ( instanceable = true "
         "references = </Lib/Cluster> ) {}\n}\n";
  }
  auto flatten = [&](pcp::PrototypeNumbering num) {
    AssetResolver resolver; resolver.SetWorkingDirectory("/tmp");
    Stage stage; std::string warn, err;
    pcp::CompositionOptions opts;
    opts.instance_flatten_mode = pcp::InstanceFlattenMode::ExtractedPrototypes;
    opts.prototype_numbering = num;
    assert(pcp::ComposeStageFromFile(f, resolver, &stage, opts, &warn, &err));
    return WriteUSDAToString(stage);
  };
  const std::string usda = flatten(pcp::PrototypeNumbering::Deterministic);
  const auto npos = std::string::npos;
  // Two prototype groups extracted to root /Flattened_Prototype_N `over`s.
  assert(usda.find("over \"Flattened_Prototype_1\"") != npos &&
         "no extracted prototype root");
  assert(usda.find("over \"Flattened_Prototype_2\"") != npos &&
         "second prototype group not extracted");
  // The leaf geometry moved onto a prototype root (not inline under an instance).
  assert(usda.find("def Mesh \"M\"") != npos && "prototype geometry lost");
  // Instances reference a /Flattened_Prototype_N (not the in-place holder path).
  assert(usda.find("</Flattened_Prototype_1>") != npos &&
         "instances do not reference the extracted prototype");
  // Nested: the Cluster prototype's members reference the Leaf prototype root.
  assert(usda.find("</Flattened_Prototype_2>") != npos &&
         "nested prototype reference missing");
  // Determinism: a second compose is byte-identical.
  assert(flatten(pcp::PrototypeNumbering::Deterministic) == usda &&
         "deterministic numbering is not reproducible");
  // The usdcat-compatible scheme is also deterministic run-to-run.
  const std::string uc = flatten(pcp::PrototypeNumbering::UsdcatCompatible);
  assert(flatten(pcp::PrototypeNumbering::UsdcatCompatible) == uc &&
         "usdcat-compatible numbering is not reproducible");
  std::remove(f.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_extracted_prototypes_collision_and_remap() {
  std::cout << "test_extracted_prototypes_collision_and_remap..." << std::endl;
  const std::string f = PcpScratchPath("next_extract_collision.usda");
  {
    std::ofstream o(f);
    o << "#usda 1.0\n"
         "def Scope \"Flattened_Prototype_1\" { int user = 7 }\n"
         "def Scope \"Lib\" ( active = false )\n"
         "{\n"
         "    def Xform \"Asset\"\n"
         "    {\n"
         "        def Material \"Mat\" {}\n"
         "        def Mesh \"Mesh\"\n"
         "        {\n"
         "            rel material:binding = </Lib/Asset/Mat>\n"
         "        }\n"
         "    }\n"
         "}\n"
         "def Xform \"World\"\n"
         "{\n"
         "    def Xform \"A\" ( instanceable = true references = </Lib/Asset> ) {}\n"
         "    def Xform \"B\" ( instanceable = true references = </Lib/Asset> ) {}\n"
         "}\n";
  }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  pcp::CompositionOptions opts;
  opts.instance_flatten_mode = pcp::InstanceFlattenMode::ExtractedPrototypes;
  opts.prototype_numbering = pcp::PrototypeNumbering::Deterministic;
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(f, resolver, &stage, opts, &warn, &err));

  UsdPrim user = stage.GetPrimAtPath("/Flattened_Prototype_1");
  assert(user.IsValid() && user.GetPropertyValue("user") &&
         "user-authored /Flattened_Prototype_1 must survive name collision");
  UsdPrim proto = stage.GetPrimAtPath("/Flattened_Prototype_2");
  assert(proto.IsValid() && "extracted prototype did not skip occupied name");
  UsdPrim mesh = stage.GetPrimAtPath("/Flattened_Prototype_2/Mesh");
  assert(mesh.IsValid());
  const std::vector<Path>* binding = mesh.GetRelationship("material:binding");
  assert(binding && binding->size() == 1);
  assert((*binding)[0].str() == "/Flattened_Prototype_2/Mat" &&
         "internal relationship target was not remapped to extracted prototype");

  UsdPrim a = stage.GetPrimAtPath("/World/A");
  UsdPrim b = stage.GetPrimAtPath("/World/B");
  assert(a.IsValid() && b.IsValid());
  assert(a.GetChildCount() == 0 && b.GetChildCount() == 0 &&
         "original instance members must be orphaned after extraction");
  const bool a_refs = !a.GetMeta().references.empty() &&
                      a.GetMeta().references[0] == "</Flattened_Prototype_2>";
  const bool b_refs = !b.GetMeta().references.empty() &&
                      b.GetMeta().references[0] == "</Flattened_Prototype_2>";
  assert(a_refs && b_refs &&
         "instance members must reference the extracted prototype root");

  std::remove(f.c_str());
  std::cout << "  OK" << std::endl;
}

// A referenced asset's attribute connection targets must be remapped from the
// asset-local namespace into the composed (referencing) namespace -- including
// connections to a property on the referenced ROOT prim itself ("/Mat.attr",
// a '.' boundary, not just descendant "/Mat/Child.attr" '/' boundaries).
static void test_connection_namespace_remap() {
  std::cout << "test_connection_namespace_remap..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  Layer layer;
  LayerBuilder lb(layer);
  // Referenced asset: a Material with a value on its root + two child shaders
  // whose connection-only inputs target (a) the material root's own property
  // [. boundary] and (b) a sibling child's output [/ boundary].
  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Mat", "Material");
  lb.add_property("inputs:stPrimvarName", Value(std::string("st")));
  lb.begin_prim("Reader", "Shader");
  {
    PropNameId vid = GetPropNameTable().intern("inputs:varname");
    lb.current()->add_property_slot(vid, TypeId::String,
                                    PropSlot::kFlagConnection);
    // '.' boundary: connection to the referenced root prim's own property.
    lb.current()->add_connection("inputs:varname",
                                 Path("/Lib/Mat.inputs:stPrimvarName"));
  }
  lb.end_prim();  // Reader
  lb.begin_prim("Surface", "Shader");
  {
    PropNameId did = GetPropNameTable().intern("inputs:diffuse");
    lb.current()->add_property_slot(did, TypeId::Float3,
                                    PropSlot::kFlagConnection);
    // '/' boundary: connection to a descendant prim's property.
    lb.current()->add_connection("inputs:diffuse",
                                 Path("/Lib/Mat/Reader.outputs:result"));
  }
  lb.end_prim();  // Surface
  lb.end_prim();  // Mat
  lb.end_prim();  // Lib
  // Host references the material asset.
  lb.begin_prim("World", "Xform");
  lb.begin_prim("Bound", "");
  lb.current()->meta().references.push_back("</Lib/Mat>");
  lb.end_prim();  // Bound
  lb.end_prim();  // World
  lb.finalize();
  auto root = std::make_shared<Layer>(std::move(layer));

  auto o = pcp::Cache::Open(resolver, root);
  assert(o);
  pcp::Cache cache = std::move(*o);

  Stage stage;
  assert(cache.BuildStage(&stage, &warn, &err));
  std::string usda = WriteUSDAToString(stage);

  // Connections must resolve to the composed namespace, NOT the asset-local one.
  // (If remapping failed, the targets would read "/Lib/Mat..." and these exact
  // composed-namespace strings would be absent -- so the positive checks below
  // are sufficient. /Lib/Mat is also authored here as the internal-ref source,
  // with its own identity-mapped connections, so a global "/Lib/Mat" absence
  // check would not hold.)
  assert(usda.find("</World/Bound.inputs:stPrimvarName>") != std::string::npos &&
         "'.' boundary: root-prim-own-property connection not remapped");
  assert(usda.find("</World/Bound/Reader.outputs:result>") != std::string::npos &&
         "'/' boundary: descendant connection not remapped");
  std::cout << "  OK" << std::endl;
}

// pxr composes an attribute's default VALUE and its CONNECTIONS as independent
// fields: a stronger source's connection-only opinion must not erase a weaker
// source's authored default. Here a local `over` adds a connection-only metallic
// while the referenced material authors `metallic = 0`; the composed attribute
// must carry BOTH.
static void test_value_connection_field_compose() {
  std::cout << "test_value_connection_field_compose..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  Layer layer;
  LayerBuilder lb(layer);
  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Mat", "Material");
  // (A) Surface.inputs:metallic = 0 (value, no connection).
  lb.begin_prim("Surface", "Shader");
  lb.add_property("inputs:metallic", Value(0.0f));
  lb.end_prim();
  // (B) Surface2.inputs:roughness = 0 (value) AND a connection -- both authored
  // in the asset; the host will override with a value-ONLY opinion, and the
  // weaker connection must survive (mirror of case A).
  lb.begin_prim("Surface2", "Shader");
  lb.add_property("inputs:roughness", Value(0.0f));
  lb.current()->add_connection("inputs:roughness",
                               Path("/Lib/Mat/Tex2.outputs:r"));
  lb.end_prim();
  lb.end_prim();  // Mat
  lb.end_prim();  // Lib
  // Host references the material and locally overrides each Surface child.
  lb.begin_prim("World", "Xform");
  lb.begin_prim("Bound", "");
  lb.current()->meta().references.push_back("</Lib/Mat>");
  // (A) over: connection-only metallic (stronger, no default) over weak value.
  lb.begin_prim("Surface", "");
  {
    PropNameId mid = GetPropNameTable().intern("inputs:metallic");
    lb.current()->add_property_slot(mid, TypeId::Float,
                                    PropSlot::kFlagConnection);
    lb.current()->add_connection("inputs:metallic",
                                 Path("/World/Bound/Tex.outputs:r"));
  }
  lb.end_prim();
  // (B) over: value-only roughness (stronger) over weak value+connection.
  lb.begin_prim("Surface2", "");
  lb.add_property("inputs:roughness", Value(0.0f));
  lb.end_prim();
  lb.end_prim();  // Bound
  lb.end_prim();  // World
  lb.finalize();
  auto root = std::make_shared<Layer>(std::move(layer));

  auto o = pcp::Cache::Open(resolver, root);
  assert(o);
  pcp::Cache cache = std::move(*o);

  Stage stage;
  assert(cache.BuildStage(&stage, &warn, &err));
  std::string usda = WriteUSDAToString(stage);

  // (A) connection-only override must keep the weaker default value.
  assert(usda.find("inputs:metallic = 0") != std::string::npos &&
         "weak source's default value was dropped by the connection-only over");
  assert(usda.find("inputs:metallic.connect = </World/Bound/Tex.outputs:r>") !=
             std::string::npos &&
         "connection opinion missing");
  // (B) value-only override must keep the weaker connection (remapped), proving
  // value and connection compose as independent fields in BOTH directions.
  assert(usda.find("inputs:roughness = 0") != std::string::npos &&
         "roughness value missing");
  assert(usda.find("inputs:roughness.connect = </World/Bound/Tex2.outputs:r>") !=
             std::string::npos &&
         "weak source's connection was dropped by the value-only over");
  std::cout << "  OK" << std::endl;
}

static void test_compose_prim_dependency_invalidate() {
  std::cout << "test_compose_prim_dependency_invalidate..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  // Build the dependency map, then lazily compose a prim whose opinions come
  // from the class site. Invalidating the class must drop the dependent lazy
  // composed prim, not just its PrimIndex.
  assert(cache.ComputePrimIndex(Path("/World/I"), &warn, &err) != nullptr);
  const PrimSpec *before = cache.ComposePrim(Path("/World/I"), &warn, &err);
  assert(before && before->property_value("newClassOpinion") == nullptr);

  PrimSpec *cls = root->prim_at_path_mutable("/_class_Base");
  assert(cls);
  cls->add_property("newClassOpinion", Value(int32_t(42)));
  cache.Invalidate(Path("/_class_Base"));

  const PrimSpec *after = cache.ComposePrim(Path("/World/I"), &warn, &err);
  assert(after && after->property_value("newClassOpinion") &&
         "dependency invalidation must drop stale ComposePrim cache entries");
  std::cout << "  OK" << std::endl;
}

static void test_compose_prim_invalidate_layer_without_index() {
  std::cout << "test_compose_prim_invalidate_layer_without_index..." << std::endl;
  auto asset = std::make_shared<Layer>();
  {
    LayerBuilder ab(*asset);
    ab.begin_prim("A", "Mesh");
    ab.end_prim();
    ab.finalize();
  }

  auto root = std::make_shared<Layer>();
  {
    LayerBuilder rb(*root);
    rb.begin_prim("World", "Xform");
    rb.begin_prim("R", "");
    rb.current()->meta().references.push_back("@asset@</A>");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &p, const std::string &) { return p; });
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("asset", asset);

  std::string warn, err;
  const PrimSpec *before = cache.ComposePrim(Path("/World/R"), &warn, &err);
  assert(before && before->property_value("freshLayerOpinion") == nullptr);
  assert(!cache.HasComputedPrimIndex(Path("/World/R")) &&
         "test must exercise lazy composition without PrimIndex dependencies");

  PrimSpec *a = asset->prim_at_path_mutable("/A");
  assert(a);
  a->add_property("freshLayerOpinion", Value(int32_t(7)));
  cache.InvalidateLayer("asset");
  cache.PreloadLayer("asset", asset);

  const PrimSpec *after = cache.ComposePrim(Path("/World/R"), &warn, &err);
  assert(after && after->property_value("freshLayerOpinion") &&
         "InvalidateLayer must drop lazy ComposePrim caches even without a PrimIndex");
  std::cout << "  OK" << std::endl;
}

// Phase 10 (A2): lazy attribute evaluation through the cache -- resolve a
// prim's attribute value (default + time-sample interpolation) and follow a
// connection across lazily-composed prims, all WITHOUT BuildStage.
static void test_eval_attribute_lazy() {
  std::cout << "test_eval_attribute_lazy..." << std::endl;
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("Src", "Scope");
    lb.add_property("value", Value(0.5f));
    lb.add_time_sample("anim", 0.0, Value(1.0f));
    lb.add_time_sample("anim", 10.0, Value(3.0f));
    lb.end_prim();
    lb.begin_prim("Dst", "Scope");
    // Connection-only slot: /Dst.value -> /Src.value.
    PropNameId vid = GetPropNameTable().intern("value");
    lb.current()->add_property_slot(vid, TypeId::Float, PropSlot::kFlagConnection);
    lb.current()->add_connection("value", Path("/Src.value"));
    lb.end_prim();
    lb.finalize();
  }
  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  // Default value, composed lazily (no BuildStage).
  EvalResult r1 = EvalAttributeLazy(cache, Path("/Src"), "value");
  assert(r1.success && r1.from_default);
  const float *f1 = r1.value.as_float();
  assert(f1 && *f1 == 0.5f);

  // Time sample interpolated at t=5 -> 2.0.
  EvalOptions opts;
  opts.time = 5.0;
  EvalResult r2 = EvalAttributeLazy(cache, Path("/Src"), "anim", opts);
  assert(r2.success && r2.from_time_sample);
  const float *f2 = r2.value.as_float();
  assert(f2 && *f2 == 2.0f);

  // Connection following: /Dst.value resolves to /Src.value == 0.5.
  EvalResult r3 = EvalAttributeLazy(cache, Path("/Dst"), "value");
  assert(r3.success && r3.from_connection);
  const float *f3 = r3.value.as_float();
  assert(f3 && *f3 == 0.5f);

  // No opinion -> unsuccessful.
  EvalResult r4 = EvalAttributeLazy(cache, Path("/Nope"), "x");
  assert(!r4.success);

  assert(cache.HasComputedPrimIndex(Path("/Src")) == false &&
         "lazy eval must not have built a PrimIndex via ComputePrimIndex");
  std::cout << "  OK" << std::endl;
}

static void test_build_stage() {
  std::cout << "test_build_stage..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  bool ok = cache.BuildStage(&stage, &warn, &err);
  assert(ok && "BuildStage failed");

  // /World/A composed the reference: it should now be typed "Mesh", carry the
  // referenced "size" property, and have the referenced child "Inner".
  UsdPrim a = stage.GetPrimAtPath("/World/A");
  assert(a.IsValid() && "/World/A missing");
  assert(a.GetTypeName() == "Mesh" && "reference did not compose type");
  assert(a.GetPropertyValue("size") != nullptr && "referenced property missing");

  UsdPrim inner = stage.GetPrimAtPath("/World/A/Inner");
  assert(inner.IsValid() && "referenced child /World/A/Inner missing");
  assert(inner.GetTypeName() == "Sphere");

  // The source prim is still present too.
  UsdPrim model = stage.GetPrimAtPath("/Lib/Model");
  assert(model.IsValid() && "/Lib/Model missing");

  std::cout << "  OK" << std::endl;
}

static void test_invalidate() {
  std::cout << "test_invalidate..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  cache.ComputePrimIndex(Path("/World/A"), &warn, &err);
  assert(cache.HasComputedPrimIndex(Path("/World/A")));
  assert(cache.ComputedPrimIndexCount() == 1);

  cache.Invalidate(Path("/World/A"));
  assert(!cache.HasComputedPrimIndex(Path("/World/A")));
  assert(cache.ComputedPrimIndexCount() == 0);

  // Recompute works after invalidation.
  assert(cache.ComputePrimIndex(Path("/World/A"), &warn, &err) != nullptr);
  std::cout << "  OK" << std::endl;
}

// Phase 2: ancestral composition - a descendant of a referenced prim can be
// computed lazily even though it has no local spec.
static void test_ancestral_compute() {
  std::cout << "test_ancestral_compute..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  // /World/A/Inner exists only because /World/A references /Lib/Model whose
  // child is Inner. No local spec at /World/A/Inner.
  const pcp::PrimIndex *inner =
      cache.ComputePrimIndex(Path("/World/A/Inner"), &warn, &err);
  assert(inner != nullptr && "ancestral composition failed for /World/A/Inner");
  assert(inner->HasAnySpecs());
  std::cout << "  OK" << std::endl;
}

// Phase 2: deferred payloads.
static void test_deferred_payload() {
  std::cout << "test_deferred_payload..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();

  pcp::CompositionOptions opts;
  opts.payload_policy = [](const Path &, const std::string &) { return false; };
  auto opened = pcp::Cache::Open(resolver, root, "", opts);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  // Payload deferred: /World/P composed but WITHOUT the payload's content.
  assert(cache.HasDeferredPayload(Path("/World/P")) && "payload not deferred");
  UsdPrim p = stage.GetPrimAtPath("/World/P");
  assert(p.IsValid());
  assert(p.GetPropertyValue("size") == nullptr && "payload content leaked while deferred");
  assert(!stage.GetPrimAtPath("/World/P/Inner").IsValid());

  // Load the payload and recompose: content now present.
  assert(cache.LoadPayload(Path("/World/P"), &warn, &err));
  assert(!cache.HasDeferredPayload(Path("/World/P")));
  Stage stage2;
  assert(cache.BuildStage(&stage2, &warn, &err));
  assert(stage2.GetPrimAtPath("/World/P").IsLoaded());
  UsdPrim p2 = stage2.GetPrimAtPath("/World/P");
  assert(p2.GetPropertyValue("size") != nullptr && "payload did not load");
  assert(stage2.GetPrimAtPath("/World/P/Inner").IsValid());
  std::cout << "  OK" << std::endl;
}

// LoadRules: UnloadPayload must recompose so HasDeferredPayload is immediately
// accurate (the old set-based code left it stale until the next compose), and a
// Load/Unload round-trip must restore the deferred state. Base policy defaults
// to load-all; we Unload the payload prim explicitly.
static void test_load_rules_lifecycle() {
  std::cout << "test_load_rules_lifecycle..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);  // default: load all payloads
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  // Initially loaded (default policy), so not deferred.
  Stage s0;
  assert(cache.BuildStage(&s0, &warn, &err));
  assert(!cache.HasDeferredPayload(Path("/World/P")));
  assert(s0.GetPrimAtPath("/World/P/Inner").IsValid() && "payload not loaded by default");

  // Unload it: HasDeferredPayload must be TRUE right away (recompose happened),
  // not stale -- this is the S7 fix.
  assert(cache.UnloadPayload(Path("/World/P")));
  assert(cache.HasDeferredPayload(Path("/World/P")) &&
         "UnloadPayload left HasDeferredPayload stale");
  Stage s1;
  assert(cache.BuildStage(&s1, &warn, &err));
  assert(!s1.GetPrimAtPath("/World/P").IsLoaded() &&
         "populated stage lost deferred-payload state");
  assert(!s1.GetPrimAtPath("/World/P/Inner").IsValid() && "payload content leaked after unload");

  // Load it back with descendants.
  assert(cache.LoadPayload(Path("/World/P"), &warn, &err));
  assert(!cache.HasDeferredPayload(Path("/World/P")));
  Stage s2;
  assert(cache.BuildStage(&s2, &warn, &err));
  assert(s2.GetPrimAtPath("/World/P").IsLoaded());
  assert(s2.GetPrimAtPath("/World/P/Inner").IsValid() && "payload did not reload");
  std::cout << "  OK" << std::endl;
}

// Phase 7 E2: a reference with a layer offset (offset:scale) must remap the
// referenced prim's time-sample times into stage time (t -> offset + scale*t)
// when BuildStage bakes the composition.
static void test_layer_offset_timesamples() {
  std::cout << "test_layer_offset_timesamples..." << std::endl;

  // Asset: /A (Mesh) with "size" sampled at t=0 ->(0,0,0), t=10 ->(10,0,0).
  auto asset = std::make_shared<Layer>();
  {
    LayerBuilder ab(*asset);
    ab.begin_prim("A", "Mesh");
    ab.add_time_sample("size", 0.0, Value::MakeFloat3(0, 0, 0));
    ab.add_time_sample("size", 10.0, Value::MakeFloat3(10, 0, 0));
    ab.end_prim();
    ab.finalize();
  }
  // Root: /World/R references the asset with offset=100, scale=2.
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("World", "Xform");
    rb.begin_prim("R", "");
    rb.current()->meta().references.push_back("@asset@</A>?layerOffset=100:2");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("asset", asset);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim r = stage.GetPrimAtPath("/World/R");
  assert(r.IsValid());
  // Original sample times 0 and 10 map to 100 and 120 (= 100 + 2*10).
  const Value *at100 = r.GetValueAtTime("size", 100.0);
  const Value *at120 = r.GetValueAtTime("size", 120.0);
  assert(at100 && at100->as_float3() && at100->as_float3()[0] == 0.0f &&
         "sample at t=0 not remapped to t=100");
  assert(at120 && at120->as_float3() && at120->as_float3()[0] == 10.0f &&
         "sample at t=10 not remapped to t=120");
  std::cout << "  OK" << std::endl;
}

// SetLoadRules drives payload inclusion declaratively: LoadNone-style (Unload
// root) defers all payloads; a targeted Load re-includes one subtree.
static void test_set_load_rules() {
  std::cout << "test_set_load_rules..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  // Unload the whole stage: /World/P's payload is now deferred.
  pcp::LoadRules rules;
  rules.Unload("/");
  cache.SetLoadRules(rules);

  std::string warn, err;
  Stage s0;
  assert(cache.BuildStage(&s0, &warn, &err));
  assert(cache.HasDeferredPayload(Path("/World/P")));
  assert(!s0.GetPrimAtPath("/World/P/Inner").IsValid());

  // Re-include just /World/P.
  rules.LoadWithDescendants("/World/P");
  cache.SetLoadRules(rules);
  Stage s1;
  assert(cache.BuildStage(&s1, &warn, &err));
  assert(!cache.HasDeferredPayload(Path("/World/P")));
  assert(s1.GetPrimAtPath("/World/P/Inner").IsValid() && "targeted load did not include payload");
  std::cout << "  OK" << std::endl;
}

// Payload inside an instance: two instanceable prims with the same reference +
// a deferred payload share one prototype (the payload contributes no arc while
// deferred, so their keys match). Loading the payload on ONE changes its arc
// set -> its instance key diverges -> it splits into its own prototype group.
static void test_payload_in_instance() {
  std::cout << "test_payload_in_instance..." << std::endl;
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("World", "Xform");
    for (const char *nm : {"Pa", "Pb"}) {
      lb.begin_prim(nm, "");
      lb.current()->meta().references.push_back("</Lib/Model>");
      lb.current()->meta().payloads.push_back("</Lib/Extra>");
      lb.current()->meta().instanceable = true;
      lb.end_prim();
    }
    lb.end_prim();  // World
    lb.begin_prim("Lib", "Scope");
    lb.begin_prim("Model", "Mesh");
    lb.end_prim();
    lb.begin_prim("Extra", "Scope");
    lb.add_property("extraProp", Value::MakeFloat3(5, 5, 5));
    lb.end_prim();
    lb.end_prim();  // Lib
    lb.finalize();
  }

  AssetResolver resolver;
  pcp::CompositionOptions opts;
  opts.payload_policy = [](const Path &, const std::string &) { return false; };
  auto opened = pcp::Cache::Open(resolver, root, "", opts);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  Stage s0;
  assert(cache.BuildStage(&s0, &warn, &err));
  // Both deferred-payload instances share one prototype.
  assert(cache.GetPrototype(Path("/World/Pa")) ==
             cache.GetPrototype(Path("/World/Pb")) &&
         "deferred-payload instances did not share a prototype");
  const size_t protos_before = cache.PrototypeCount();

  // Load Pa's payload: its arc set now includes the payload content, so its key
  // diverges from Pb's -> Pa is no longer grouped with Pb.
  assert(cache.LoadPayload(Path("/World/Pa"), &warn, &err));
  Stage s1;
  assert(cache.BuildStage(&s1, &warn, &err));
  assert(cache.GetPrototype(Path("/World/Pa")) !=
             cache.GetPrototype(Path("/World/Pb")) &&
         "loading a payload did not split the instance from its group");
  assert(cache.PrototypeCount() > protos_before);
  std::cout << "  OK" << std::endl;
}

// Phase 3: inherits + specializes, with LIVRPS strength
// (Local > Inherit > Reference > Payload > Specialize).
// Phase 7 (E3): a transitive specialize chain P -> _B -> _C must compose every
// link's opinions, with the most-local (P) winning and the chain reaching the
// weakest (_C). Specialize is globally weakest, but still composes.
static void test_specialize_chain() {
  std::cout << "test_specialize_chain..." << std::endl;
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("_C", "Scope");          // weakest in the chain
    lb.add_property("cOnly", Value(30.0f));
    lb.add_property("val", Value(3.0f));
    lb.end_prim();
    lb.begin_prim("_B", "Scope");
    lb.current()->meta().specializes.push_back("</_C>");
    lb.add_property("bOnly", Value(20.0f));
    lb.add_property("val", Value(2.0f));
    lb.end_prim();
    lb.begin_prim("P", "Scope");
    lb.current()->meta().specializes.push_back("</_B>");
    lb.add_property("pOnly", Value(10.0f));
    lb.add_property("val", Value(1.0f));
    lb.end_prim();
    lb.finalize();
  }
  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim p = stage.GetPrimAtPath("/P");
  assert(p.IsValid());
  auto fval = [&](const char *n) -> float {
    const Value *v = p.GetPropertyValue(n);
    assert(v && "missing composed property");
    const float *f = v->as_float();
    assert(f);
    return *f;
  };
  assert(fval("val") == 1.0f && "local opinion must win over the chain");
  assert(fval("pOnly") == 10.0f);
  assert(fval("bOnly") == 20.0f && "direct specialize opinion must compose");
  assert(fval("cOnly") == 30.0f && "transitive specialize chain must compose");
  std::cout << "  OK" << std::endl;
}

// Phase 7 (S5): an arc authored identically in the root and a sublayer expands
// twice by default; with CompositionOptions::apply_list_ops it is de-duplicated
// and expands once (fewer PrimIndex nodes), without changing composed values.
static void test_cross_layer_arc_dedup() {
  std::cout << "test_cross_layer_arc_dedup..." << std::endl;
  const std::string sub = "/tmp/next_pcp_listop_sub.usda";
  {
    std::ofstream f(sub);
    f << "#usda 1.0\n"
         "def Xform \"World\"\n{\n"
         "    def \"Dup\" (\n        references = [</Lib/Model>]\n    )\n    {\n"
         "    }\n}\n";
  }
  auto root_layer = std::make_shared<Layer>();
  {
    root_layer->meta().subLayers.push_back("next_pcp_listop_sub.usda");
    LayerBuilder rb(*root_layer);
    rb.begin_prim("World", "Xform");
    rb.begin_prim("Dup", "");  // same site as the sublayer, same reference
    rb.current()->meta().references.push_back("</Lib/Model>");
    rb.end_prim();
    rb.end_prim();
    rb.begin_prim("Lib", "Scope");
    rb.begin_prim("Model", "Mesh");
    rb.add_property("size", Value::MakeFloat3(1, 1, 1));
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  std::string w, e;

  auto node_count = [&](bool flag) -> int {
    pcp::CompositionOptions opts;
    opts.apply_list_ops = flag;
    auto opened = pcp::Cache::Open(resolver, root_layer,
                                   "/tmp/next_pcp_listop_root.usda", opts);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    const pcp::PrimIndex *idx =
        cache.ComputePrimIndex(Path("/World/Dup"), &w, &e);
    assert(idx);
    // The reference resolves either way: the composed type is still "Mesh".
    Stage stage;
    assert(cache.BuildStage(&stage, &w, &e));
    assert(stage.GetPrimAtPath("/World/Dup").GetTypeName() == "Mesh");
    return idx->GetNodeCount();
  };

  int without = node_count(false);
  int with = node_count(true);
  assert(with < without && "apply_list_ops must drop the duplicate arc node");
  std::remove(sub.c_str());
  std::cout << "  OK (" << without << " -> " << with << " nodes)" << std::endl;
}

static void test_inherits_specializes() {
  std::cout << "test_inherits_specializes..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  // Inherit composes the class type.
  UsdPrim i = stage.GetPrimAtPath("/World/I");
  assert(i.IsValid());
  assert(i.GetTypeName() == "Scope" && "inherit did not compose class type");

  // Inherit (Scope) is STRONGER than the reference (Mesh).
  UsdPrim ir = stage.GetPrimAtPath("/World/IR");
  assert(ir.IsValid());
  assert(ir.GetTypeName() == "Scope" && "inherit should outrank reference");

  // Specialize (Scope) is WEAKER than the reference (Mesh).
  UsdPrim sp = stage.GetPrimAtPath("/World/SP");
  assert(sp.IsValid());
  assert(sp.GetTypeName() == "Mesh" && "reference should outrank specialize");

  std::cout << "  OK" << std::endl;
}

// Phase 4: variant selection grafts the chosen variant's opinions only.
// An attribute authored inside a USDA inline variant must keep its qualifier
// flags (custom / uniform) through parse -> compose -> write. (VariantData stored
// name->Value without flags, so the variant graft silently dropped them.)
static void test_variant_inline_property_flags() {
  std::cout << "test_variant_inline_property_flags..." << std::endl;
  const std::string root = "/tmp/next_pcp_varflags.usda";
  {
    std::ofstream f(root);
    f << "#usda 1.0\n"
         "def Xform \"P\" (\n"
         "    variants = { string v = \"a\" }\n"
         "    prepend variantSets = \"v\"\n"
         ")\n"
         "{\n"
         "    variantSet \"v\" = {\n"
         "        \"a\" {\n"
         "            custom bool isAsset = 1\n"
         "            uniform token axis = \"Z\"\n"
         "        }\n"
         "    }\n"
         "}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));

  // emit_custom on so the (preserved) custom flag is visible in the output.
  USDAWriteOptions opts;
  opts.emit_custom = true;
  std::string usda = WriteUSDAToString(stage, opts);
  assert(usda.find("custom bool isAsset") != std::string::npos &&
         "custom flag dropped from variant-grafted property");
  assert(usda.find("uniform token axis") != std::string::npos &&
         "uniform flag dropped from variant-grafted property");
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_variant_option_authored_state() {
  std::cout << "test_variant_option_authored_state..." << std::endl;
  const std::string root = "/tmp/next_pcp_var_authored_state.usda";
  {
    std::ofstream f(root);
    // Authored `hidden = 0` on the SELECTED option is a real opinion and must
    // land on the composed prim (pxr flatten emits `hidden = false`); the
    // unselected option's `hidden = 1` must not leak.
    f << "#usda 1.0\n"
         "def Xform \"P\" (\n"
         "    variants = { string v = \"a\" }\n"
         "    prepend variantSets = \"v\"\n"
         ")\n"
         "{\n"
         "    variantSet \"v\" = {\n"
         "        \"a\" ( hidden = 0 ) {\n"
         "            float inA = 1\n"
         "        }\n"
         "        \"b\" ( hidden = 1 ) {\n"
         "            float inB = 2\n"
         "        }\n"
         "    }\n"
         "}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  const std::string usda = WriteUSDAToString(stage);
  assert(usda.find("hidden = false") != std::string::npos &&
         "authored hidden=0 on the selected variant option must compose");
  assert(usda.find("hidden = true") == std::string::npos &&
         "unselected option's hidden=1 must not leak");
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_variants() {
  std::cout << "test_variants..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim v = stage.GetPrimAtPath("/World/V");
  assert(v.IsValid());
  assert(v.GetPropertyValue("variantHigh") != nullptr &&
         "selected variant 'high' not grafted");
  assert(v.GetPropertyValue("variantLow") == nullptr &&
         "non-selected variant 'low' leaked");
  std::cout << "  OK" << std::endl;
}

// Variant-content layer stacks are keyed by a STABLE identity (host stack +
// site + set + variant name), not the VariantData pointer. So the instance key
// of a variant-content prim is deterministic across independent compositions
// (the old uintptr_t key differed by heap address between runs, and aliased
// after a free+realloc). Two separate Cache::Open of the same scene must agree.
static void test_variant_content_key_stable() {
  std::cout << "test_variant_content_key_stable..." << std::endl;
  auto build = []() {
    auto content = std::make_shared<Layer>();
    LayerBuilder cb(*content);
    cb.begin_prim("__self__", "");
    cb.add_property("hostProp", Value::MakeFloat3(7, 7, 7));
    cb.begin_prim("Geom", "Mesh");
    cb.end_prim();
    cb.end_prim();
    cb.finalize();

    auto root = std::make_shared<Layer>();
    LayerBuilder lb(*root);
    lb.begin_prim("VC", "");
    VariantSetData vss;
    vss.name = "geo";
    VariantData full;
    full.name = "full";
    full.content = content;
    vss.variants.push_back(std::move(full));
    lb.current()->meta().variantSets().push_back(std::move(vss));
    lb.current()->meta().variantSelection = "geo=full";
    lb.end_prim();
    lb.finalize();
    return root;
  };

  std::string warn, err;
  AssetResolver r1;
  auto o1 = pcp::Cache::Open(r1, build());
  assert(o1);
  pcp::Cache c1 = std::move(*o1);
  std::string k1 = c1.ComputeInstanceKey(Path("/VC"), &warn, &err);

  AssetResolver r2;
  auto o2 = pcp::Cache::Open(r2, build());
  assert(o2);
  pcp::Cache c2 = std::move(*o2);
  std::string k2 = c2.ComputeInstanceKey(Path("/VC"), &warn, &err);

  assert(k1 == k2 && "variant-content instance key not stable across runs");
  assert(k1.find("0x") == std::string::npos &&
         "instance key still encodes a raw pointer");
  std::cout << "  OK" << std::endl;
}

// FU2: variant subtree (adds geometry) + multi-selection.
static void test_variants_v2() {
  std::cout << "test_variants_v2..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  // Content variant grafts a host property AND a child prim.
  UsdPrim vc = stage.GetPrimAtPath("/World/VC");
  assert(vc.IsValid());
  assert(vc.GetPropertyValue("hostProp") != nullptr && "variant host opinion missing");
  UsdPrim geom = stage.GetPrimAtPath("/World/VC/Geom");
  assert(geom.IsValid() && "variant child prim not grafted");
  assert(geom.GetTypeName() == "Mesh");

  // Multi-selection: both selected variant sets contribute.
  UsdPrim mv = stage.GetPrimAtPath("/World/MV");
  assert(mv.IsValid());
  assert(mv.GetPropertyValue("p1") != nullptr && "variant set s1 not applied");
  assert(mv.GetPropertyValue("p2") != nullptr && "variant set s2 not applied");
  std::cout << "  OK" << std::endl;
}

// Phase 5: instancing - structural prototype detection + grouping.
// CompositionOptions::flatten_instances turns native instancing into a
// self-contained layer: one member of each prototype group keeps the composed
// subtree (the holder, non-instanceable) and the others reference it.
static void test_flatten_instances() {
  std::cout << "test_flatten_instances..." << std::endl;
  const std::string asset = "/tmp/next_fi_asset.usda";
  const std::string root = "/tmp/next_fi_root.usda";
  { std::ofstream f(asset);
    f << "#usda 1.0\ndef Xform \"Model\" { def Mesh \"M\" { int n = 1 } }\n"; }
  { std::ofstream f(root);
    f << "#usda 1.0\n"
         "def Xform \"W\"\n{\n"
         "    def \"A\" (\n        instanceable = true\n"
         "        prepend references = @next_fi_asset.usda@</Model>\n    ) {}\n"
         "    def \"B\" (\n        instanceable = true\n"
         "        prepend references = @next_fi_asset.usda@</Model>\n    ) {}\n"
         "}\n"; }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  pcp::CompositionOptions opts;
  opts.flatten_instances = true;
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, opts, &warn, &err));
  std::string usda = WriteUSDAToString(stage);

  // One of {A,B} is the content holder; the other is an empty instanceable
  // prim referencing the holder by internal path </W/A> or </W/B>.
  const bool refs_a = usda.find("</W/A>") != std::string::npos;
  const bool refs_b = usda.find("</W/B>") != std::string::npos;
  assert((refs_a ^ refs_b) &&
         "exactly one of A/B must reference the other as the prototype holder");
  // The holder retains the composed subtree (Mesh M with n=1).
  assert(usda.find("def Mesh \"M\"") != std::string::npos &&
         usda.find("int n = 1") != std::string::npos &&
         "prototype holder lost its composed subtree");
  // Internal reference, NOT an external asset path (@...@).
  assert(usda.find("@</W/") == std::string::npos &&
         "internal reference was wrongly emitted as an asset path");
  std::remove(asset.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_instancing() {
  std::cout << "test_instancing..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  std::vector<Path> paths{Path("/World/Inst1"), Path("/World/Inst2"),
                          Path("/World/Inst3")};
  assert(cache.PrewarmPrimIndices(paths, &warn, &err));

  // Two distinct prototypes: {Inst1,Inst2} share, Inst3 is its own.
  assert(cache.PrototypeCount() == 2);

  std::string k1 = cache.ComputeInstanceKey(Path("/World/Inst1"), &warn, &err);
  std::string k2 = cache.ComputeInstanceKey(Path("/World/Inst2"), &warn, &err);
  std::string k3 = cache.ComputeInstanceKey(Path("/World/Inst3"), &warn, &err);
  assert(k1 == k2 && "same-asset instances must share a key");
  assert(k1 != k3 && "different-asset instances must differ");

  Path p1 = cache.GetPrototype(Path("/World/Inst1"));
  Path p2 = cache.GetPrototype(Path("/World/Inst2"));
  assert(p1 == p2 && "Inst1/Inst2 must share a prototype");
  assert(cache.GetInstancesForPrototype(p1).size() == 2);

  // Exactly one of the two is the prototype (IsInstance == false), the other an
  // instance (IsInstance == true).
  bool i1 = cache.IsInstance(Path("/World/Inst1"));
  bool i2 = cache.IsInstance(Path("/World/Inst2"));
  assert(i1 != i2);
  std::cout << "  OK" << std::endl;
}

// Nested-instancing fixture:
//   /World (Xform)
//     /CA (instanceable) -> </Lib/Cluster>
//     /CB (instanceable) -> </Lib/Cluster>
//   /Lib (Scope)
//     /Cluster (Xform) { /L1 (instanceable -> </Lib/Leaf>),
//                        /L2 (instanceable -> </Lib/Leaf>) }
//     /Leaf (Mesh) { /Tip (Sphere) }
static std::shared_ptr<Layer> BuildNestedInstanceLayer() {
  Layer layer;
  LayerBuilder lb(layer);
  lb.begin_prim("World", "Xform");
  lb.begin_prim("CA", "");
  lb.current()->meta().references.push_back("</Lib/Cluster>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.begin_prim("CB", "");
  lb.current()->meta().references.push_back("</Lib/Cluster>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.end_prim();  // World

  lb.begin_prim("Lib", "Scope");
  lb.begin_prim("Cluster", "Xform");
  lb.begin_prim("L1", "");
  lb.current()->meta().references.push_back("</Lib/Leaf>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.begin_prim("L2", "");
  lb.current()->meta().references.push_back("</Lib/Leaf>");
  lb.current()->meta().instanceable = true;
  lb.end_prim();
  lb.end_prim();  // Cluster
  lb.begin_prim("Leaf", "Mesh");
  lb.begin_prim("Tip", "Sphere");
  lb.end_prim();  // Tip
  lb.end_prim();  // Leaf
  lb.end_prim();  // Lib

  lb.finalize();
  return std::make_shared<Layer>(std::move(layer));
}

// I3: instance<->prototype path translation API.
static void test_path_translation() {
  std::cout << "test_path_translation..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::string warn, err;
  std::vector<Path> paths{Path("/World/Inst1"), Path("/World/Inst2"),
                          Path("/World/Inst3")};
  assert(cache.PrewarmPrimIndices(paths, &warn, &err));

  // Identify which of Inst1/Inst2 is the (non-prototype) instance and its shared
  // prototype root.
  const Path proto = cache.GetPrototype(Path("/World/Inst1"));
  PCP_CHECK(!proto.empty(), "Inst1 should have a prototype");
  const Path inst = cache.IsInstance(Path("/World/Inst1"))
                        ? Path("/World/Inst1")
                        : Path("/World/Inst2");
  PCP_CHECK(cache.IsInstance(inst), "picked path must be an instance");

  // Instance root -> prototype root.
  PCP_CHECK(cache.TranslatePathToPrototype(inst) == proto,
            "instance root should translate to its prototype root");
  // Descendant of the instance -> matching descendant of the prototype (the
  // referenced /Lib/Model has child Inner).
  const Path inst_inner = Path(inst.str() + "/Inner");
  const Path proto_inner = Path(proto.str() + "/Inner");
  PCP_CHECK(cache.TranslatePathToPrototype(inst_inner) == proto_inner,
            "instance descendant should translate into prototype space");
  // A path under no instance yields an empty translation.
  PCP_CHECK(cache.TranslatePathToPrototype(Path("/World")).empty(),
            "non-instance path should translate to empty");
  PCP_CHECK(cache.TranslatePathToPrototype(Path("/Lib/Model")).empty(),
            "prototype-space/library path should translate to empty");

  // Inverse translation + round-trip onto the specific instance.
  PCP_CHECK(cache.TranslatePathFromPrototype(proto, inst) == inst,
            "prototype root should map back to the instance root");
  PCP_CHECK(cache.TranslatePathFromPrototype(proto_inner, inst) == inst_inner,
            "prototype descendant should map back onto the instance");
  PCP_CHECK(cache.TranslatePathFromPrototype(
                cache.TranslatePathToPrototype(inst_inner), inst) == inst_inner,
            "to/from prototype should round-trip");
  // FromPrototype requires an instance root and an enclosing prototype.
  PCP_CHECK(
      cache.TranslatePathFromPrototype(proto_inner, Path("/World")).empty(),
      "FromPrototype on a non-instance root should be empty");
  PCP_CHECK(
      cache.TranslatePathFromPrototype(Path("/Lib/Other"), inst).empty(),
      "FromPrototype of a path outside the prototype should be empty");

  // --- Nested instancing: an instance inside a prototype. ---
  {
    auto nroot = BuildNestedInstanceLayer();
    auto nopened = pcp::Cache::Open(resolver, nroot);
    assert(nopened);
    pcp::Cache ncache = std::move(*nopened);
    Stage stage;
    // BuildStage discovers the full nested prototype grouping in one pass.
    assert(ncache.BuildStage(&stage, &warn, &err));

    // Two prototype groups: the Cluster group ({CA,CB}) and the nested Leaf
    // group ({L1,L2}) living inside the Cluster prototype holder.
    PCP_CHECK(ncache.PrototypeCount() == 2,
              "nested scene should have two prototype groups");
    const Path proto_cluster = ncache.GetPrototype(Path("/World/CB"));
    const Path proto_leaf = ncache.GetPrototype(Path("/World/CA/L2"));
    PCP_CHECK(!proto_cluster.empty() && !proto_leaf.empty(),
              "nested prototypes should resolve");

    // Single level: a Cluster instance -> the Cluster prototype root.
    PCP_CHECK(ncache.TranslatePathToPrototype(Path("/World/CB")) == proto_cluster,
              "Cluster instance should translate to the Cluster prototype");
    // Nested instance living inside the prototype holder -> the Leaf prototype.
    PCP_CHECK(ncache.TranslatePathToPrototype(Path("/World/CA/L2")) == proto_leaf,
              "nested instance inside a prototype should translate to Leaf proto");
    // Two-level: a leaf descendant reached through the outer instance AND the
    // nested instance rewrites through both prototype roots.
    const Path leaf_tip = Path(proto_leaf.str() + "/Tip");
    PCP_CHECK(ncache.TranslatePathToPrototype(Path("/World/CB/L2/Tip")) == leaf_tip,
              "two-level nested descendant should rewrite through both prototypes");
    PCP_CHECK(ncache.TranslatePathToPrototype(Path("/World/CB/L1")) ==
                  proto_leaf,
              "outer-instance nested child should translate to the Leaf proto");

    // Inverse round-trips at the outer level.
    PCP_CHECK(
        ncache.TranslatePathFromPrototype(proto_cluster, Path("/World/CB")) ==
            Path("/World/CB"),
        "Cluster prototype root should map back to the CB instance");

    // Identity consistency: a fresh recompose yields the same grouping.
    pcp::Cache ncache2 = std::move(*pcp::Cache::Open(resolver, nroot));
    Stage stage2;
    assert(ncache2.BuildStage(&stage2, &warn, &err));
    PCP_CHECK(ncache2.GetPrototype(Path("/World/CB")) == proto_cluster &&
                  ncache2.GetPrototype(Path("/World/CA/L2")) == proto_leaf,
              "prototype grouping must be identical across recompose");
  }
  std::cout << "  OK" << std::endl;
}

// FU1: relocates rename a prim in the composed namespace.
static void test_relocates() {
  std::cout << "test_relocates..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  assert(stage.GetPrimAtPath("/World/New").IsValid() && "relocate target missing");
  assert(!stage.GetPrimAtPath("/World/RigHost/Inner").IsValid() &&
         "relocate source leaked");
  // Content is the arc-delivered Sphere child of the referenced Model.
  assert(stage.GetPrimAtPath("/World/New").GetTypeName() == "Sphere");
  std::cout << "  OK" << std::endl;
}

// FU3: implied inherit propagation. A referenced asset inherits a class; a
// root-authored override on the same class path composes (implied into root).
static void test_implied_inherit() {
  std::cout << "test_implied_inherit..." << std::endl;

  // In-memory referenced asset: a class with libClassProp + an Asset prim that
  // inherits it.
  auto asset = std::make_shared<Layer>();
  {
    LayerBuilder ab(*asset);
    ab.begin_prim("_class_Over", "Scope", PrimSpecifier::Class);
    ab.add_property("libClassProp", Value::MakeFloat3(1, 1, 1));
    ab.end_prim();
    ab.begin_prim("Asset", "Mesh");
    ab.current()->meta().inherits.push_back("</_class_Over>");
    ab.end_prim();
    ab.finalize();
  }

  // Root layer: its OWN _class_Over (rootClassProp) + Q references the asset.
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("_class_Over", "Scope", PrimSpecifier::Class);
    rb.add_property("rootClassProp", Value::MakeFloat3(2, 2, 2));
    rb.end_prim();
    rb.begin_prim("World", "Xform");
    rb.begin_prim("Q", "");
    rb.current()->meta().references.push_back("@mem_asset@</Asset>");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("mem_asset", asset);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim q = stage.GetPrimAtPath("/World/Q");
  assert(q.IsValid());
  // Oracle-verified (pxr 26.05): the root-stack IMPLIED class is stronger
  // than the whole reference subtree, so its authored type (Scope) wins over
  // the referenced Mesh (usdcat --flatten composes `def Scope`).
  assert(q.GetTypeName() == "Scope" && "implied-class type must win");
  assert(q.GetPropertyValue("libClassProp") != nullptr &&
         "direct (referenced-stack) inherit missing");
  assert(q.GetPropertyValue("rootClassProp") != nullptr &&
         "IMPLIED root-stack inherit override missing");
  std::cout << "  OK" << std::endl;
}

// FU5: BuildStage materializes instances as proxies that share the prototype's
// subtree (no duplication), with transparent child access.
static void test_instance_proxy() {
  std::cout << "test_instance_proxy..." << std::endl;
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim i1 = stage.GetPrimAtPath("/World/Inst1");
  UsdPrim i2 = stage.GetPrimAtPath("/World/Inst2");
  assert(i1.IsValid() && i2.IsValid());

  // Inst1 is the prototype (subtree materialized); Inst2 links to it.
  assert(i1.GetMeta().instance_prototype().empty());
  assert(i2.GetMeta().instance_prototype() == "/World/Inst1");

  // The prototype owns the referenced child; the instance's subtree is NOT
  // duplicated in the layer.
  assert(stage.GetPrimAtPath("/World/Inst1/Inner").IsValid());
  assert(stage.GetRootLayer()->prim_at_path("/World/Inst2/Inner") == nullptr &&
         "instance subtree was duplicated");
  UsdPrim direct_proxy = stage.GetPrimAtPath("/World/Inst2/Inner");
  assert(direct_proxy.IsValid() &&
         direct_proxy.GetPath().str() == "/World/Inst2/Inner" &&
         "direct instance-proxy lookup failed");

  // ...but child access through the instance is transparent (proxy).
  assert(i2.GetChildCount() == i1.GetChildCount());
  assert(i2.GetPropertyValue("size") != nullptr &&
         "instance root must expose prototype properties");
  const std::vector<std::string> instance_properties = i2.GetPropertyNames();
  assert(std::find(instance_properties.begin(), instance_properties.end(),
                   "size") != instance_properties.end());
  const std::vector<std::string> instance_relationships =
      i2.GetRelationshipNames();
  assert(std::find(instance_relationships.begin(), instance_relationships.end(),
                   "inside") != instance_relationships.end());
  UsdPrim proxy_child = i2.GetChild("Inner");
  assert(proxy_child.IsValid() && "instance proxy child access failed");
  assert(proxy_child.GetPath().str() == "/World/Inst2/Inner" &&
         proxy_child.GetParent().GetPath().str() == "/World/Inst2");
  std::vector<Path> forwarded;
  assert(i2.GetForwardedRelationshipTargets("inside", &forwarded));
  assert(forwarded.size() == 1 &&
         forwarded[0].str() == "/World/Inst2/Inner" &&
         "prototype relationship target was not mapped into instance namespace");
  std::cout << "  OK" << std::endl;
}

// FU6: PrewarmPrimIndices with num_threads>1 parallelizes layer prefetch via the
// thread-safe registry. Uses preloaded in-memory assets so the parallel path is
// exercised safely; the composed result must match a sequential build.
static void test_parallel_prewarm() {
  std::cout << "test_parallel_prewarm..." << std::endl;
  const int N = 8;

  // N distinct in-memory assets, each a Mesh "A".
  std::vector<std::shared_ptr<Layer>> assets;
  for (int k = 0; k < N; ++k) {
    auto a = std::make_shared<Layer>();
    LayerBuilder ab(*a);
    ab.begin_prim("A", "Mesh");
    ab.add_property("tag", Value::MakeFloat3(float(k), 0, 0));
    ab.end_prim();
    ab.finalize();
    assets.push_back(a);
  }

  // Root: /World/R{k} references @asset_{k}@</A>.
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("World", "Xform");
    for (int k = 0; k < N; ++k) {
      rb.begin_prim("R" + std::to_string(k), "");
      rb.current()->meta().references.push_back("@asset_" + std::to_string(k) +
                                                "@</A>");
      rb.end_prim();
    }
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });

  pcp::CompositionOptions opts;
  opts.num_threads = 4;
  auto opened = pcp::Cache::Open(resolver, rootL, "", opts);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  for (int k = 0; k < N; ++k) {
    cache.PreloadLayer("asset_" + std::to_string(k), assets[k]);
  }

  std::vector<Path> paths;
  for (int k = 0; k < N; ++k) paths.push_back(Path("/World/R" + std::to_string(k)));

  std::string warn, err;
  assert(cache.PrewarmPrimIndices(paths, &warn, &err));

  // Every prim's index is built and composed the referenced Mesh.
  for (int k = 0; k < N; ++k) {
    Path p("/World/R" + std::to_string(k));
    assert(cache.HasComputedPrimIndex(p));
    const pcp::PrimIndex *idx = cache.ComputePrimIndex(p, &warn, &err);
    assert(idx && idx->GetNodeCount() >= 2);
  }
  std::cout << "  OK" << std::endl;
}

// FU7: cross-source variant selection - the variantSet is defined on a
// referenced asset, but the SELECTION is authored on the (stronger) local prim.
static void test_cross_source_variant() {
  std::cout << "test_cross_source_variant..." << std::endl;

  // Asset defines variantSet "vs" with variant "hi" (grafts xvar), no selection.
  auto asset = std::make_shared<Layer>();
  {
    LayerBuilder ab(*asset);
    ab.begin_prim("A", "");
    VariantSetData vss;
    vss.name = "vs";
    VariantData hi;
    hi.name = "hi";
    hi.properties.push_back({"xvar", Value::MakeFloat3(5, 5, 5)});
    vss.variants.push_back(std::move(hi));
    ab.current()->meta().variantSets().push_back(std::move(vss));
    ab.end_prim();
    ab.finalize();
  }

  // Root: CS references the asset AND authors the selection vs=hi.
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("World", "Xform");
    rb.begin_prim("CS", "");
    rb.current()->meta().references.push_back("@cs_asset@</A>");
    rb.current()->meta().variantSelection = "vs=hi";
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("cs_asset", asset);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim cs = stage.GetPrimAtPath("/World/CS");
  assert(cs.IsValid());
  assert(cs.GetPropertyValue("xvar") != nullptr &&
         "cross-source variant selection failed");
  std::cout << "  OK" << std::endl;
}

// FU8: implied class arcs propagate into INTERMEDIATE stacks of a multi-level
// reference chain (root -> A -> B), not just root. B inherits a class; each of
// B's, A's, and root's class opinions composes onto the final prim.
static void test_implied_intermediate() {
  std::cout << "test_implied_intermediate..." << std::endl;

  // ROOT-level untyped classes: pxr REJECTS a prim inheriting its own child
  // class through a reference chain ("Cycle detected ... CANNOT inherit
  // from"), so the classes live at each layer's root (oracle-verified shape:
  // T composes Mesh + fooB/fooA/fooRoot).
  auto B = std::make_shared<Layer>();
  {
    LayerBuilder bb(*B);
    bb.begin_prim("_class_Foo", "", PrimSpecifier::Class);
    bb.add_property("fooB", Value::MakeFloat3(1, 0, 0));
    bb.end_prim();
    bb.begin_prim("B", "Mesh");
    bb.current()->meta().inherits.push_back("</_class_Foo>");
    bb.end_prim();
    bb.finalize();
  }
  auto A = std::make_shared<Layer>();
  {
    LayerBuilder ab(*A);
    ab.begin_prim("_class_Foo", "", PrimSpecifier::Class);
    ab.add_property("fooA", Value::MakeFloat3(0, 1, 0));
    ab.end_prim();
    ab.begin_prim("A", "");
    ab.current()->meta().references.push_back("@assetB@</B>");
    ab.end_prim();
    ab.finalize();
  }
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("_class_Foo", "", PrimSpecifier::Class);
    rb.add_property("fooRoot", Value::MakeFloat3(0, 0, 1));
    rb.end_prim();
    rb.begin_prim("World", "Xform");
    rb.begin_prim("T", "");
    rb.current()->meta().references.push_back("@assetA@</A>");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("assetA", A);
  cache.PreloadLayer("assetB", B);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim t = stage.GetPrimAtPath("/World/T");
  assert(t.IsValid());
  assert(t.GetTypeName() == "Mesh");
  assert(t.GetPropertyValue("fooB") != nullptr && "direct (B stack) class missing");
  assert(t.GetPropertyValue("fooA") != nullptr &&
         "implied INTERMEDIATE (A stack) class missing");
  assert(t.GetPropertyValue("fooRoot") != nullptr &&
         "implied root class missing");
  std::cout << "  OK" << std::endl;
}

// FU10/FU12: end-to-end one-call composition from real USDA files, with a
// cross-file reference (exercises reader arc-metadata parsing + registry +
// namespace mapping).
// Phase 7 S5: cross-layer list-op merging (apply_list_ops). A `delete` in the
// stronger (root) layer removes an arc contributed by a weaker sublayer, and a
// bare/explicit list in the stronger layer replaces the weaker one. With the
// flag off, both arcs compose (legacy strong-first concatenation).
// Phase 7 S5: the USDA writer re-emits the authored list-op qualifier from the
// ArcEdit instead of always `prepend` -- a bare list as a bare list, and a
// prepend/delete edit with its qualifiers (the old writer could not emit
// `delete` at all).
static void test_writer_listop_fidelity() {
  std::cout << "test_writer_listop_fidelity..." << std::endl;
  Layer layer;
  LayerBuilder lb(layer);
  lb.begin_prim("Bare", "Scope");
  lb.current()->meta().references.push_back("ext.usda</X>");  // bare (no edit)
  lb.current()->meta().inherits.push_back("</_class_Base>");
  lb.current()->meta().specializes.push_back("</_class_Fallback>");
  lb.end_prim();
  lb.begin_prim("Edited", "Scope");
  lb.current()->meta().references.push_back("ext.usda</A>");  // within-spec list
  {
    ArcEdit& e = lb.current()->meta().ensure_arc_edits().references;
    e.authored = true;
    e.is_explicit = false;
    e.prepended.push_back("ext.usda</A>");
    e.deleted.push_back("ext.usda</Old>");
  }
  lb.end_prim();
  lb.finalize();

  std::string out = WriteLayerToString(layer, USDAWriteOptions());
  // The edit's qualifiers are preserved (old writer always emitted `prepend`
  // and never `delete`). Single arc targets print WITHOUT list brackets
  // (pxr spelling).
  assert(out.find("prepend references = ") != std::string::npos &&
         out.find("prepend references = [") == std::string::npos);
  assert(out.find("delete references = ") != std::string::npos &&
         out.find("delete references = [") == std::string::npos);
  // The bare prim emits an unqualified (explicit) list. It is the only prim
  // whose `references =` is not preceded by a qualifier word.
  assert(out.find("\n        references = ") != std::string::npos ||
         out.find("\n    references = ") != std::string::npos);
  assert(out.find("inherits = </_class_Base>") != std::string::npos &&
         "USDA writer must emit inherits arcs");
  assert(out.find("specializes = </_class_Fallback>") != std::string::npos &&
         "USDA writer must emit specializes arcs");
  std::cout << "  OK" << std::endl;
}

// Phase 7 S5: cross-layer list-op merging (apply_list_ops), exercised through
// the committed corpus fixtures in tests/usda/composition/. A `delete` and a
// bare (explicit) list in the stronger root layer drop a weaker sublayer's
// reference only with the flag on; with it off both arcs compose. These same
// fixtures give the apply_list_ops corpus-diff review positive coverage.
static void test_cross_layer_listops() {
  std::cout << "test_cross_layer_listops..." << std::endl;

  // Resolve the committed fixture dir relative to the ctest working directory
  // (build/next) or a repo-root run.
  const char *dirs[] = {"../../tests/usda/composition/",
                        "tests/usda/composition/",
                        "../../../tests/usda/composition/",
                        LIGHTUSD_TEST_REPO_ROOT "/tests/usda/composition/"};
  std::string base;
  for (const char *d : dirs) {
    std::ifstream probe(std::string(d) + "listop-xlayer-delete-000.usda");
    if (probe.good()) { base = d; break; }
  }
  if (base.empty()) {
    std::cout << "  (skipped: composition fixtures not found from cwd)\n";
    return;
  }

  auto compose = [&](const std::string &file, bool flag,
                     const char *prim) -> std::pair<bool, bool> {
    AssetResolver resolver;
    resolver.SetWorkingDirectory(base);
    pcp::CompositionOptions opts;
    opts.apply_list_ops = flag;
    Stage stage;
    std::string w, e;
    bool ok = pcp::ComposeStageFromFile(base + file, resolver, &stage, opts, &w, &e);
    assert(ok && "ComposeStageFromFile failed");
    UsdPrim p = stage.GetPrimAtPath(prim);
    assert(p.IsValid());
    return {p.GetPropertyValue("fromA") != nullptr,
            p.GetPropertyValue("fromB") != nullptr};
  };

  // DELETE fixture: /P. Off -> both arcs; on -> the stronger `delete` drops the
  // weak </Lib/A> so only fromB survives.
  const std::string del = "listop-xlayer-delete-000.usda";
  auto p_off = compose(del, false, "/P");
  assert(p_off.first && p_off.second && "default: weak+strong arcs both compose");
  auto p_on = compose(del, true, "/P");
  assert(!p_on.first && p_on.second && "delete must drop the weak arc cross-layer");

  // EXPLICIT fixture: /Q. Off -> both arcs; on -> the bare list replaces the
  // weak </Lib/A> so only fromB survives.
  const std::string exp = "listop-xlayer-explicit-000.usda";
  auto q_off = compose(exp, false, "/Q");
  assert(q_off.first && q_off.second);
  auto q_on = compose(exp, true, "/Q");
  assert(!q_on.first && q_on.second && "explicit list must replace the weak arc");

  // Empty explicit list: still authored, and must clear weaker references.
  const std::string empty_sub = "/tmp/next_pcp_empty_explicit_sub.usda";
  const std::string empty_root = "/tmp/next_pcp_empty_explicit_root.usda";
  {
    std::ofstream f(empty_sub);
    f << "#usda 1.0\n"
         "def \"E\" (\n"
         "    references = </Lib/A>\n"
         ")\n"
         "{\n"
         "}\n"
         "def Scope \"Lib\"\n"
         "{\n"
         "    def Mesh \"A\" { custom int fromA = 1 }\n"
         "}\n";
  }
  {
    std::ofstream f(empty_root);
    f << "#usda 1.0\n"
         "(\n"
         "    subLayers = [@next_pcp_empty_explicit_sub.usda@]\n"
         ")\n"
         "def \"E\" (\n"
         "    references = []\n"
         ")\n"
         "{\n"
         "}\n";
  }
  AssetResolver empty_resolver;
  empty_resolver.SetWorkingDirectory("/tmp");
  pcp::CompositionOptions empty_opts;
  empty_opts.apply_list_ops = true;
  Stage empty_stage;
  std::string empty_warn, empty_err;
  bool empty_ok = pcp::ComposeStageFromFile(
      empty_root, empty_resolver, &empty_stage, empty_opts, &empty_warn,
      &empty_err);
  if (!empty_ok) std::cout << "  [diag] err=" << empty_err << std::endl;
  assert(empty_ok);
  UsdPrim e = empty_stage.GetPrimAtPath("/E");
  assert(e.IsValid());
  assert(e.GetPropertyValue("fromA") == nullptr &&
         "empty explicit list must clear weaker references");
  std::remove(empty_sub.c_str());
  std::remove(empty_root.c_str());

  std::cout << "  OK" << std::endl;
}

static void test_cross_layer_arc_reorder_fixtures() {
  std::cout << "test_cross_layer_arc_reorder_fixtures..." << std::endl;
  const char *dirs[] = {"../../tests/usda/composition/",
                        "tests/usda/composition/",
                        "../../../tests/usda/composition/",
                        LIGHTUSD_TEST_REPO_ROOT "/tests/usda/composition/"};
  std::string base;
  for (const char *dir : dirs) {
    std::ifstream probe(std::string(dir) +
                        "listop-xlayer-reorder-000.usda");
    if (probe.good()) { base = dir; break; }
  }
  PCP_CHECK(!base.empty(), "reorder composition fixture must be discoverable");
  if (base.empty()) return;

  AssetResolver resolver;
  resolver.SetWorkingDirectory(base);
  pcp::CompositionOptions options;
  options.apply_list_ops = true;
  Stage stage;
  std::string warnings, errors;
  const bool ok = pcp::ComposeStageFromFile(
      base + "listop-xlayer-reorder-000.usda", resolver, &stage, options,
      &warnings, &errors);
  PCP_CHECK(ok, std::string("reorder fixture composition failed: ") + errors);
  if (!ok) return;

  auto check = [&](const char *path) {
    const UsdPrim prim = stage.GetPrimAtPath(path);
    PCP_CHECK(prim.IsValid(), std::string("missing composed prim ") + path);
    if (!prim.IsValid()) return;
    const Value *winner = prim.GetPropertyValue("winner");
    PCP_CHECK(winner && winner->as_string() &&
                  *winner->as_string() == "B",
              std::string(path) +
                  " reorder must make B the strongest arc opinion");
    PCP_CHECK(prim.GetPropertyValue("fromA") != nullptr &&
                  prim.GetPropertyValue("fromB") != nullptr &&
                  prim.GetPropertyValue("fromC") != nullptr,
              std::string(path) +
                  " reorder must retain unordered arcs and ignore absent keys");
  };
  check("/Ref");
  check("/Payload");
  std::cout << "  OK" << std::endl;
}

static void test_authored_arc_fidelity_fixture() {
  std::cout << "test_authored_arc_fidelity_fixture..." << std::endl;
  const char *paths[] = {
      "../../tests/usda/composition/authored-arc-fidelity.usda",
      "tests/usda/composition/authored-arc-fidelity.usda",
      "../../../tests/usda/composition/authored-arc-fidelity.usda",
      LIGHTUSD_TEST_REPO_ROOT
          "/tests/usda/composition/authored-arc-fidelity.usda"};
  std::string text;
  for (const char *path : paths) {
    std::ifstream stream(path);
    if (!stream.good()) continue;
    text.assign(std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
    break;
  }
  PCP_CHECK(!text.empty(), "authored arc fixture must be discoverable");
  if (text.empty()) return;

  std::string load_warnings, load_errors;
  auto loaded = pcp::LoadLayerFromMemory(
      "authored-arc-fidelity.usda",
      reinterpret_cast<const uint8_t *>(text.data()), text.size(),
      &load_warnings, &load_errors);
  PCP_CHECK(bool(loaded), "authored arc fixture must parse");
  if (!loaded) return;
  const PrimSpec *prim = loaded->prim_at_path("/ArcFidelity");
  PCP_CHECK(prim != nullptr, "authored arc fixture prim missing");
  if (!prim) return;
  const ArcListOpEdits *edits = prim->meta().arc_edits();
  PCP_CHECK(edits != nullptr, "authored arc edits missing");
  if (!edits) return;
  PCP_CHECK(edits->references.prepended.size() == 1 &&
                edits->references.appended.size() == 1 &&
                edits->references.deleted.size() == 1 &&
                edits->references.ordered.size() == 2,
            "all authored reference list-op sublists must survive parsing");
  PCP_CHECK(edits->payloads.prepended.size() == 1 &&
                edits->payloads.appended.size() == 1 &&
                edits->payloads.deleted.size() == 1 &&
                edits->payloads.ordered.size() == 2,
            "all authored payload list-op sublists must survive parsing");
  PCP_CHECK(prim->meta().references.size() == 2 &&
                prim->meta().references[0].find("</InternalA>") == 0 &&
                prim->meta().references[1].find("@asset-a.usda@") == 0,
            "reorder references must reorder existing inline arcs without adding duplicates");
  PCP_CHECK(prim->meta().payloads.size() == 2 &&
                prim->meta().payloads[0].find("</InternalPayload>") == 0 &&
                prim->meta().payloads[1].find("@payload-a.usda@") == 0,
            "reorder payloads must reorder existing inline arcs without adding duplicates");
  PCP_CHECK(edits->references.prepended[0].find("?layerOffset=2.000000:0.500000") !=
                std::string::npos,
            "reference layer offset must survive authored parsing");
  PCP_CHECK(edits->payloads.prepended[0].find("?layerOffset=-1.000000:2.000000") !=
                std::string::npos,
            "payload layer offset must survive authored parsing");

  const std::string rewritten =
      WriteLayerToString(*loaded, USDAWriteOptions());
  PCP_CHECK(rewritten.find("reorder references") != std::string::npos &&
                rewritten.find("reorder payload") != std::string::npos,
            "USDA writer must preserve authored reorder qualifiers");
  PCP_CHECK(rewritten.find("offset = 2") != std::string::npos &&
                rewritten.find("scale = 0.5") != std::string::npos &&
                rewritten.find("offset = -1") != std::string::npos &&
                rewritten.find("scale = 2") != std::string::npos,
            "USDA writer must decode canonical arc offsets");
  PCP_CHECK(rewritten.find("authored-arc-fidelity") != std::string::npos,
            "unrelated authored prim customData must not be lost");
  std::cout << "  OK" << std::endl;
}

static void test_listop_edit_does_not_clear_other_arc_fields() {
  std::cout << "test_listop_edit_does_not_clear_other_arc_fields..." << std::endl;
  const std::string weak = "/tmp/next_pcp_listop_sibling_weak.usda";
  const std::string root = "/tmp/next_pcp_listop_sibling_root.usda";
  {
    std::ofstream f(weak);
    f << "#usda 1.0\n"
         "def \"P\" (\n"
         "    payload = </Lib/Pay>\n"
         ")\n"
         "{\n"
         "}\n"
         "def Scope \"Lib\"\n"
         "{\n"
         "    def Sphere \"Pay\" { custom int payloadProp = 2 }\n"
         "}\n";
  }
  {
    std::ofstream f(root);
    f << "#usda 1.0\n"
         "(\n"
         "    subLayers = [@next_pcp_listop_sibling_weak.usda@]\n"
         ")\n"
         "def \"P\" (\n"
         "    prepend references = </Lib/Ref>\n"
         ")\n"
         "{\n"
         "}\n"
         "def Scope \"Lib\"\n"
         "{\n"
         "    def Mesh \"Ref\" { custom int refProp = 1 }\n"
         "}\n";
  }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  pcp::CompositionOptions opts;
  opts.apply_list_ops = true;
  Stage stage;
  std::string warn, err;
  bool ok = pcp::ComposeStageFromFile(root, resolver, &stage, opts, &warn, &err);
  if (!ok) std::cout << "  [diag] err=" << err << std::endl;
  assert(ok);
  UsdPrim p = stage.GetPrimAtPath("/P");
  assert(p.IsValid());
  assert(p.GetPropertyValue("refProp") &&
         "strong references list-op should compose");
  assert(p.GetPropertyValue("payloadProp") &&
         "references edit must not clear weak payload opinions");

  std::remove(weak.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_compose_from_file() {
  std::cout << "test_compose_from_file..." << std::endl;

  const std::string ref = "/tmp/next_pcp_ref.usda";
  const std::string root = "/tmp/next_pcp_root.usda";
  { std::ofstream f(ref); f << "#usda 1.0\ndef Mesh \"Geom\"\n{\n}\n"; }
  {
    std::ofstream f(root);
    f << "#usda 1.0\n"
         "def Xform \"World\"\n{\n"
         "    def \"Q\" (\n"
         "        prepend references = [@next_pcp_ref.usda@</Geom>]\n"
         "    )\n    {\n    }\n}\n";
  }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");

  Stage stage;
  std::string warn, err;
  bool ok = pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err);
  if (!ok) std::cout << "  [diag] err=" << err << std::endl;
  assert(ok && "ComposeStageFromFile failed");

  UsdPrim q = stage.GetPrimAtPath("/World/Q");
  assert(q.IsValid() && "/World/Q missing");
  assert(q.GetTypeName() == "Mesh" &&
         "cross-file reference did not compose through the loader");

  std::remove(ref.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// A NESTED reference whose inner asset path is relative to ITS OWN directory
// (e.g. a mesh under Meshes/Sub/ that references `../../Materials/M.usda`) must
// anchor against the mesh's directory, not the root layer's. Regression for the
// resolver double-strip bug: the anchor is the referencing FILE path, and
// AssetResolver derives the directory itself (passing a pre-stripped dir dropped
// one path level, so deep `../../` paths resolved too high and failed to load).
static void test_nested_relative_reference() {
  std::cout << "test_nested_relative_reference..." << std::endl;
  // Layout under /tmp:
  //   nrr_root.usda                 -> refs nrr/Meshes/Sub/mesh.usda
  //   nrr/Meshes/Sub/mesh.usda      -> refs ../../Materials/mat.usda
  //   nrr/Materials/mat.usda        (defines the leaf Material type)
  if (::system("mkdir -p /tmp/nrr/Meshes/Sub /tmp/nrr/Materials") != 0) { /* ignore */ }
  { std::ofstream f("/tmp/nrr/Materials/mat.usda");
    f << "#usda 1.0\ndef Material \"Mat\"\n{\n}\n"; }
  { std::ofstream f("/tmp/nrr/Meshes/Sub/mesh.usda");
    f << "#usda 1.0\n"
         "def Mesh \"Geom\" (\n"
         "    prepend references = [@../../Materials/mat.usda@</Mat>]\n"
         ")\n{\n}\n"; }
  { std::ofstream f("/tmp/nrr_root.usda");
    f << "#usda 1.0\n"
         "def Xform \"World\"\n{\n"
         "    def \"M\" (\n"
         "        prepend references = [@nrr/Meshes/Sub/mesh.usda@</Geom>]\n"
         "    )\n    {\n    }\n}\n"; }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  bool ok = pcp::ComposeStageFromFile("/tmp/nrr_root.usda", resolver, &stage, {},
                                      &warn, &err);
  assert(ok && "ComposeStageFromFile failed");

  // The mesh reference composes (type Mesh), and the nested material reference
  // (anchored to the mesh's dir) must ALSO compose -- proving `../../Materials`
  // resolved correctly.
  UsdPrim m = stage.GetPrimAtPath("/World/M");
  assert(m.IsValid() && m.GetTypeName() == "Mesh" && "mesh ref did not compose");
  // A failed nested layer load is non-fatal (accumulated in err/warn, not a
  // hard failure), so the load-failure string is the real regression signal.
  const std::string diag = warn + err;
  assert(diag.find("Failed to load") == std::string::npos &&
         diag.find("Failed to open") == std::string::npos &&
         "nested ../../ material layer failed to load (anchor double-strip)");

  std::remove("/tmp/nrr_root.usda");
  std::remove("/tmp/nrr/Meshes/Sub/mesh.usda");
  std::remove("/tmp/nrr/Materials/mat.usda");
  std::cout << "  OK" << std::endl;
}

// A reference/payload authored in a SUBLAYER (or inside a variant) must anchor
// its relative asset path to the LAYER that authored it, not the layer-stack
// root. Regression for the Scene A failure: entry.usda sublayers
// deep/dir/mid.usda, which references `../../target/asset.usda` relative to its
// OWN dir; anchoring to the root made `../../` resolve too high and fail.
static void test_sublayer_authored_reference_anchor() {
  std::cout << "test_sublayer_authored_reference_anchor..." << std::endl;
  // /tmp/sar/entry.usda            (root) -> sublayer a/b/mid.usda
  // /tmp/sar/a/b/mid.usda          -> /World/P references ../../target/asset.usda
  // /tmp/sar/target/asset.usda     def Mesh "Geom"
  if (::system("mkdir -p /tmp/sar/a/b /tmp/sar/target") != 0) { /* ignore */ }
  { std::ofstream f("/tmp/sar/target/asset.usda");
    f << "#usda 1.0\ndef Mesh \"Geom\" { custom int marker = 7 }\n"; }
  { std::ofstream f("/tmp/sar/a/b/mid.usda");
    f << "#usda 1.0\n"
         "def Xform \"World\"\n{\n"
         "    def \"P\" (\n"
         "        prepend references = @../../target/asset.usda@</Geom>\n"
         "    )\n    {\n    }\n}\n"; }
  { std::ofstream f("/tmp/sar/entry.usda");
    f << "#usda 1.0\n(\n    subLayers = [@a/b/mid.usda@]\n)\n"; }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp/sar");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile("/tmp/sar/entry.usda", resolver, &stage, {},
                                   &warn, &err));
  // The reference (authored in the sublayer, relative to the sublayer's dir)
  // must compose: /World/P picks up the asset's Mesh type + marker.
  UsdPrim p = stage.GetPrimAtPath("/World/P");
  assert(p.IsValid() && p.GetTypeName() == "Mesh" &&
         "sublayer-authored relative reference failed to anchor/compose");
  const std::string diag = warn + err;
  assert(diag.find("Failed to load") == std::string::npos &&
         diag.find("Failed to open") == std::string::npos &&
         "sublayer-authored reference anchored to the wrong (root) layer");

  std::remove("/tmp/sar/entry.usda");
  std::remove("/tmp/sar/a/b/mid.usda");
  std::remove("/tmp/sar/target/asset.usda");
  std::cout << "  OK" << std::endl;
}

// payload_policy_with_prim must receive the payload's AUTHORING PrimSpec after
// list-op merging across the layer stack, not the strongest spec. The root
// layer holds the strongest spec (an `over`); the payload arc lives on the
// weaker sublayer in a DIFFERENT directory, and only the authoring spec's
// asset anchor lets a policy stat the relative payload path correctly.
static void test_payload_policy_owner_anchor() {
  std::cout << "test_payload_policy_owner_anchor..." << std::endl;
  // /tmp/ppoa/root.usda      (strongest: over /World/P, no payload)
  // /tmp/ppoa/sub/weak.usda  (def /World/P, prepend payload = @./pay.usda@)
  // /tmp/ppoa/sub/pay.usda   (def Mesh "Pay" -- relative to sub/)
  if (::system("mkdir -p /tmp/ppoa/sub") != 0) { /* ignore */ }
  { std::ofstream f("/tmp/ppoa/sub/pay.usda");
    f << "#usda 1.0\ndef Mesh \"Pay\" { custom int marker = 42 }\n"; }
  { std::ofstream f("/tmp/ppoa/sub/weak.usda");
    f << "#usda 1.0\n"
         "def Xform \"World\"\n{\n"
         "    def \"P\" (\n"
         "        prepend payload = @./pay.usda@</Pay>\n"
         "    )\n    {\n    }\n}\n"; }
  { std::ofstream f("/tmp/ppoa/root.usda");
    f << "#usda 1.0\n(\n    subLayers = [@sub/weak.usda@]\n)\n"
         "over \"World\"\n{\n"
         "    over \"P\"\n    {\n"
         "        custom int strongOpinion = 1\n"
         "    }\n}\n"; }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp/ppoa");

  // Mimic LargeSceneLoader's budget policy: stat the relative asset against
  // the OWNER's anchor and admit only if the file exists there.
  std::vector<std::string> seen_anchors;
  pcp::CompositionOptions opts;
  opts.payload_policy_with_prim = [&seen_anchors](
      const Path &, const std::string &asset, const PrimSpec &owner) -> bool {
    const std::string &anchor = AssetAnchorPath(owner.asset_anchor_id());
    seen_anchors.push_back(anchor);
    if (anchor.empty()) return false;
    std::ifstream probe(anchor + "/" + asset);
    return probe.good();
  };

  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile("/tmp/ppoa/root.usda", resolver, &stage,
                                   opts, &warn, &err));

  // The policy saw the AUTHORING layer's anchor (sub/), not the root's.
  assert(!seen_anchors.empty() && "payload policy was never consulted");
  for (const std::string &anchor : seen_anchors) {
    assert(anchor.find("/ppoa/sub") != std::string::npos &&
           "policy owner is not the payload's authoring spec (wrong anchor)");
  }

  // And therefore the payload was admitted and composed.
  UsdPrim p = stage.GetPrimAtPath("/World/P");
  assert(p.IsValid() && p.GetTypeName() == "Mesh" &&
         "cross-layer payload was deferred (policy stat used wrong anchor)");
  assert(p.GetPropertyValue("marker") != nullptr &&
         "payload content missing after owner-anchored admission");
  assert(p.GetPropertyValue("strongOpinion") != nullptr &&
         "root layer's stronger over lost during list-op merge");

  std::remove("/tmp/ppoa/root.usda");
  std::remove("/tmp/ppoa/sub/weak.usda");
  std::remove("/tmp/ppoa/sub/pay.usda");
  std::cout << "  OK" << std::endl;
}

// Sublayers: weaker layer-stack opinions fill stronger root opinions; roots,
// child prims, and arcs authored only in a sublayer must still compose.
static void test_sublayer_stack_composition() {
  std::cout << "test_sublayer_stack_composition..." << std::endl;

  const std::string sub = "/tmp/next_pcp_sublayer_sub.usda";
  {
    std::ofstream f(sub);
    f << "#usda 1.0\n"
         "def Xform \"World\"\n"
         "{\n"
         "    custom int weakVal = 5\n"
         "    def Scope \"WeakChild\"\n"
         "    {\n"
         "    }\n"
         "    def \"SubRef\" (\n"
         "        prepend references = [</Lib/Model>]\n"
         "    )\n"
         "    {\n"
         "    }\n"
         "    def \"SubPayload\" (\n"
         "        payload = </Lib/Model>\n"
         "    )\n"
         "    {\n"
         "    }\n"
         "}\n"
         "def Scope \"OnlyInSub\"\n"
         "{\n"
         "    custom int subOnly = 9\n"
         "}\n"
         "def Scope \"Lib\"\n"
         "{\n"
         "    def Mesh \"Model\"\n"
         "    {\n"
         "        custom int modelVal = 7\n"
         "    }\n"
         "}\n";
  }

  auto root_layer = std::make_shared<Layer>();
  {
    root_layer->meta().subLayers.push_back("next_pcp_sublayer_sub.usda");
    LayerBuilder rb(*root_layer);
    rb.begin_prim("World", "Xform");
    rb.begin_prim("StrongChild", "Scope");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  std::string warn, err;
  auto opened = pcp::Cache::Open(resolver, root_layer,
                                 "/tmp/next_pcp_sublayer_root.usda");
  assert(opened && "sublayer Cache::Open failed");
  pcp::Cache cache = std::move(*opened);
  Stage stage;
  bool ok = cache.BuildStage(&stage, &warn, &err);
  if (!ok) std::cout << "  [diag] err=" << err << std::endl;
  assert(ok && "sublayer BuildStage failed");

  UsdPrim world = stage.GetPrimAtPath("/World");
  assert(world.IsValid());
  assert(world.GetPropertyValue("weakVal") != nullptr &&
         "weaker sublayer property did not fill");
  assert(stage.GetPrimAtPath("/World/StrongChild").IsValid());
  assert(stage.GetPrimAtPath("/World/WeakChild").IsValid() &&
         "weaker sublayer child missing");
  assert(stage.GetPrimAtPath("/OnlyInSub").IsValid() &&
         "sublayer-only root missing");

  UsdPrim sub_ref = stage.GetPrimAtPath("/World/SubRef");
  assert(sub_ref.IsValid());
  assert(sub_ref.GetTypeName() == "Mesh" &&
         "sublayer-authored reference did not compose");
  assert(sub_ref.GetPropertyValue("modelVal") != nullptr);

  UsdPrim sub_payload = stage.GetPrimAtPath("/World/SubPayload");
  assert(sub_payload.IsValid());
  assert(sub_payload.GetTypeName() == "Mesh" &&
         "sublayer-authored payload did not compose");
  assert(sub_payload.GetPropertyValue("modelVal") != nullptr);

  std::remove(sub.c_str());
  std::cout << "  OK" << std::endl;
}

static Stage BuildSinglePrimStage(const std::string& name,
                                  const std::string& type_name,
                                  const std::string& prop_name,
                                  int prop_value) {
  Layer layer;
  LayerBuilder lb(layer);
  lb.begin_prim(name, type_name);
  lb.add_property(prop_name, Value(static_cast<int32_t>(prop_value)));
  lb.end_prim();
  lb.finalize();

  Stage stage;
  stage.SetRootLayer(std::move(layer));
  return stage;
}

static void AppendU16LE(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
}

static void AppendU32LE(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xffu));
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xffu));
}

static void AppendStoredZipEntry(std::vector<uint8_t>* out,
                                 const std::string& name,
                                 const std::string& bytes) {
  AppendU32LE(out, 0x04034b50u);  // local file header
  AppendU16LE(out, 20);           // version needed
  AppendU16LE(out, 0);            // flags
  AppendU16LE(out, 0);            // stored
  AppendU16LE(out, 0);            // mod time
  AppendU16LE(out, 0);            // mod date
  AppendU32LE(out, 0);            // crc32 (ignored by USDZReader)
  AppendU32LE(out, static_cast<uint32_t>(bytes.size()));
  AppendU32LE(out, static_cast<uint32_t>(bytes.size()));
  AppendU16LE(out, static_cast<uint16_t>(name.size()));
  AppendU16LE(out, 0);            // extra field
  out->insert(out->end(), name.begin(), name.end());
  out->insert(out->end(), bytes.begin(), bytes.end());
}

static void WriteTwoEntryUSDZ(const std::string& filename,
                              const std::string& root_usda,
                              const std::string& sibling_usda) {
  std::vector<uint8_t> bytes;
  AppendStoredZipEntry(&bytes, "root.usda", root_usda);
  AppendStoredZipEntry(&bytes, "sibling.usda", sibling_usda);
  std::ofstream f(filename, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

// LoadUSDComposed must route direct .usdz layers and explicit package-path
// layers through PCP, preserving the normal archive and inner-parser caps.
static void test_usdz_package_layers() {
  std::cout << "test_usdz_package_layers..." << std::endl;

  {
    Stage sub_stage =
        BuildSinglePrimStage("World", "Xform", "weakVal", 42);
    USDZWriteResult wr =
        WriteUSDZToFile("/tmp/next_pcp_sublayer_pkg.usdz", sub_stage);
    assert(wr.success && "failed to write sublayer USDZ fixture");

    {
      std::ofstream f("/tmp/next_pcp_usdz_sublayer_root.usda");
      f << "#usda 1.0\n"
           "(\n"
           "    subLayers = [@next_pcp_sublayer_pkg.usdz@]\n"
           ")\n"
           "def Xform \"World\"\n"
           "{\n"
           "    custom int strongVal = 7\n"
           "}\n";
    }

    Stage stage;
    std::string warn, err;
    bool ok = LoadUSDComposed("/tmp/next_pcp_usdz_sublayer_root.usda",
                              &stage, &warn, &err);
    if (!ok) std::cout << "  [diag] err=" << err << std::endl;
    assert(ok && "LoadUSDComposed failed for direct USDZ sublayer");
    UsdPrim world = stage.GetPrimAtPath("/World");
    assert(world.IsValid());
    assert(world.GetPropertyValue("weakVal") != nullptr &&
           "direct USDZ sublayer weak opinion missing");
    assert(world.GetPropertyValue("strongVal") != nullptr &&
           "direct USDZ sublayer strong root opinion missing");
  }

  {
    Stage ref_stage =
        BuildSinglePrimStage("Asset", "Xform", "pkgVal", 99);
    USDZWriteResult wr =
        WriteUSDZToFile("/tmp/next_pcp_ref_pkg.usdz", ref_stage);
    assert(wr.success && "failed to write reference USDZ fixture");

    {
      std::ofstream f("/tmp/next_pcp_usdz_ref_root.usda");
      f << "#usda 1.0\n"
           "def Xform \"World\"\n"
           "{\n"
           "    def Xform \"P\" (\n"
           "        prepend references = "
           "[@next_pcp_ref_pkg.usdz[root.usdc]@</Asset>]\n"
           "    )\n"
           "    {\n"
           "    }\n"
           "}\n";
    }

    Stage stage;
    std::string warn, err;
    bool ok = LoadUSDComposed("/tmp/next_pcp_usdz_ref_root.usda",
                              &stage, &warn, &err);
    if (!ok) std::cout << "  [diag] err=" << err << std::endl;
    assert(ok && "LoadUSDComposed failed for USDZ package reference");
    UsdPrim p = stage.GetPrimAtPath("/World/P");
    assert(p.IsValid());
    assert(p.GetPropertyValue("pkgVal") != nullptr &&
           "USDZ package-path reference opinion missing");
  }

  {
    const std::string root_usda =
        "#usda 1.0\n"
        "def Xform \"World\"\n"
        "{\n"
        "    def Xform \"P\" (\n"
        "        prepend references = [@sibling.usda@</Asset>]\n"
        "    )\n"
        "    {\n"
        "    }\n"
        "}\n";
    const std::string sibling_usda =
        "#usda 1.0\n"
        "def Xform \"Asset\"\n"
        "{\n"
        "    custom int siblingVal = 123\n"
        "}\n";
    WriteTwoEntryUSDZ("/tmp/next_pcp_multi_pkg.usdz", root_usda,
                      sibling_usda);

    {
      AssetResolver package_resolver;
      const ResolvedAsset escaped = package_resolver.Resolve(
          "../../sibling.usda",
          "/tmp/next_pcp_multi_pkg.usdz[root.usda]",
          /*allow_suffix_fallback=*/false);
      assert(!escaped.exists &&
             "package-relative parent traversal escaped above archive root");
      const ResolvedAsset explicit_escaped = package_resolver.Resolve(
          "/tmp/next_pcp_multi_pkg.usdz[../sibling.usda]", "",
          /*allow_suffix_fallback=*/false);
      assert(!explicit_escaped.exists &&
             "explicit package path accepted parent traversal");
    }

    {
      Stage direct_stage;
      std::string direct_warn, direct_err;
      bool direct_ok = LoadUSDComposed("/tmp/next_pcp_multi_pkg.usdz",
                                       &direct_stage, &direct_warn, &direct_err);
      if (!direct_ok) std::cout << "  [diag] err=" << direct_err << std::endl;
      assert(direct_ok && "direct-root USDZ composition failed");
      UsdPrim direct_p = direct_stage.GetPrimAtPath("/World/P");
      assert(direct_p.IsValid());
      assert(direct_p.GetPropertyValue("siblingVal") != nullptr &&
             "direct-root USDZ did not anchor sibling reference in package");
    }

    {
      const std::string payload_root_usda =
          "#usda 1.0\n"
          "def Xform \"World\"\n"
          "{\n"
          "    def Xform \"P\" (\n"
          "        prepend payload = @sibling.usda@</Asset>\n"
          "    )\n"
          "    {\n"
          "    }\n"
          "}\n";
      WriteTwoEntryUSDZ("/tmp/next_pcp_payload_pkg.usdz",
                        payload_root_usda, sibling_usda);

      StageSessionOptions session_options;
      session_options.composition.load_payloads = false;
      StageSession session;
      assert(session.OpenFile("/tmp/next_pcp_payload_pkg.usdz",
                              session_options) &&
             "StageSession failed to open deferred package payload");
      assert(session.IsComposed());
      assert(session.GetDeferredPayloadPaths().size() == 1);
      assert(session.GetDeferredPayloadPaths()[0] == Path("/World/P"));
      StageSnapshot deferred_snapshot = session.GetSnapshot();
      UsdPrim deferred = deferred_snapshot->GetPrimAtPath("/World/P");
      assert(deferred.IsValid());
      assert(deferred.GetPropertyValue("siblingVal") == nullptr &&
             "deferred package payload was composed eagerly");

      assert(session.LoadPayload(Path("/World/P")) &&
             "package-internal deferred payload failed to load");
      StageSnapshot loaded_snapshot = session.GetSnapshot();
      UsdPrim loaded = loaded_snapshot->GetPrimAtPath("/World/P");
      assert(loaded.IsValid());
      assert(loaded.GetPropertyValue("siblingVal") != nullptr &&
             "loaded package payload opinion is missing");
      assert(session.GetDeferredPayloadPaths().empty());

      assert(session.UnloadPayload(Path("/World/P")) &&
             "package-internal payload failed to unload");
      assert(session.GetDeferredPayloadPaths().size() == 1);
      assert(session.LoadPayload(Path("/World/P")) &&
             "cached package payload failed to reload");
      assert(session.GetSnapshot()
                 ->GetPrimAtPath("/World/P")
                 .GetPropertyValue("siblingVal") != nullptr);
    }

    {
      std::ofstream f("/tmp/next_pcp_usdz_pkg_anchor_root.usda");
      f << "#usda 1.0\n"
           "(\n"
           "    subLayers = [@next_pcp_multi_pkg.usdz[root.usda]@]\n"
           ")\n";
    }

    Stage stage;
    std::string warn, err;
    bool ok = LoadUSDComposed("/tmp/next_pcp_usdz_pkg_anchor_root.usda",
                              &stage, &warn, &err);
    if (!ok) std::cout << "  [diag] err=" << err << std::endl;
    assert(ok && "LoadUSDComposed failed for package-internal reference");
    UsdPrim p = stage.GetPrimAtPath("/World/P");
    assert(p.IsValid());
    assert(p.GetPropertyValue("siblingVal") != nullptr &&
           "relative reference inside USDZ package did not resolve to sibling entry");
  }

  std::remove("/tmp/next_pcp_sublayer_pkg.usdz");
  std::remove("/tmp/next_pcp_usdz_sublayer_root.usda");
  std::remove("/tmp/next_pcp_ref_pkg.usdz");
  std::remove("/tmp/next_pcp_usdz_ref_root.usda");
  std::remove("/tmp/next_pcp_multi_pkg.usdz");
  std::remove("/tmp/next_pcp_payload_pkg.usdz");
  std::remove("/tmp/next_pcp_usdz_pkg_anchor_root.usda");
  std::cout << "  OK" << std::endl;
}

static void test_sublayer_cycle_and_depth() {
  std::cout << "test_sublayer_cycle_and_depth..." << std::endl;

  const std::string cycle_root = "/tmp/next_pcp_cycle_root.usda";
  {
    std::ofstream f(cycle_root);
    f << "#usda 1.0\n"
         "def Xform \"Root\"\n"
         "{\n"
         "}\n";
  }
  auto cycle_layer = std::make_shared<Layer>();
  {
    cycle_layer->meta().subLayers.push_back("next_pcp_cycle_root.usda");
    LayerBuilder lb(*cycle_layer);
    lb.begin_prim("Root", "Xform");
    lb.end_prim();
    lb.finalize();
  }

  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  std::string warn, err;
  auto cycle_opened = pcp::Cache::Open(resolver, cycle_layer, cycle_root);
  // pxr parity: a sublayer cycle is WARNED and the repeated layer skipped —
  // the rest of the scene still composes (previously the whole load failed).
  assert(cycle_opened && "sublayer cycle should warn + skip, not fail");
  {
    bool cycle_reported = false;
    for (const auto& is : cycle_opened->GetCompositionIssues()) {
      if (is.message.find("Sublayer cycle") != std::string::npos) {
        cycle_reported = true;
      }
    }
    assert(cycle_reported && "cycle must still be diagnosed");
  }

  const std::string depth_sub = "/tmp/next_pcp_depth_sub.usda";
  {
    std::ofstream f(depth_sub);
    f << "#usda 1.0\n"
         "def Scope \"Sub\"\n"
         "{\n"
         "}\n";
  }

  auto root_layer = std::make_shared<Layer>();
  {
    root_layer->meta().subLayers.push_back("next_pcp_depth_sub.usda");
    LayerBuilder lb(*root_layer);
    lb.begin_prim("Root", "Xform");
    lb.end_prim();
    lb.finalize();
  }
  pcp::CompositionOptions opts;
  opts.max_depth = 0;
  auto opened = pcp::Cache::Open(resolver, root_layer,
                                 "/tmp/next_pcp_depth_root.usda", opts);
  assert(!opened && "sublayer depth overflow should fail Cache::Open");

  std::remove(cycle_root.c_str());
  std::remove(depth_sub.c_str());
  std::cout << "  OK" << std::endl;
}

static void test_node_overflow_fails_cleanly() {
  std::cout << "test_node_overflow_fails_cleanly..." << std::endl;

  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("Boom", "");
    for (size_t i = 0; i < pcp::PrimIndex::kMaxNodeCount; ++i) {
      lb.current()->meta().references.push_back("</Lib/A>");
    }
    lb.end_prim();
    lb.begin_prim("Lib", "Scope");
    lb.begin_prim("A", "Mesh");
    lb.end_prim();
    lb.end_prim();
    lb.finalize();
  }

  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  std::string warn, err;
  const pcp::PrimIndex *idx =
      cache.ComputePrimIndex(Path("/Boom"), &warn, &err);
  assert(idx == nullptr && "overflowing PrimIndex should not be published");
  assert(err.find("uint16 capacity") != std::string::npos);
  assert(!cache.HasComputedPrimIndex(Path("/Boom")));
  std::cout << "  OK" << std::endl;
}

// FU9: the Cache is thread-safe (built with LIGHTUSD_ENABLE_THREAD) -- multiple
// threads may query/compose a shared cache concurrently.
static void test_concurrent_queries() {
  std::cout << "test_concurrent_queries..." << std::endl;
#if defined(LIGHTUSD_ENABLE_THREAD)
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  const std::vector<std::string> existing = {
      "/World", "/World/A", "/World/A/Inner", "/World/I",
      "/World/IR", "/World/SP", "/World/V", "/Lib/Model"};

  const int kThreads = 8;
  const int kIters = 1000;
  std::atomic<bool> ok{true};
  auto worker = [&]() {
    std::string w, e;
    for (int it = 0; it < kIters; ++it) {
      for (const std::string &p : existing) {
        if (cache.ComputePrimIndex(Path(p), &w, &e) == nullptr) ok.store(false);
      }
      (void)cache.ComputedPrimIndexCount();
      (void)cache.HasComputedPrimIndex(Path("/World/A"));
    }
  };
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) ts.emplace_back(worker);
  for (auto &t : ts) t.join();
  assert(ok.load());
#else
  std::cout << "  (skipped: build with -DLIGHTUSD_NEXT_ENABLE_THREAD=ON)\n";
#endif
  std::cout << "  OK" << std::endl;
}

// TSAN stress: race read-only queries against mutating payload edits
// (LoadPayload/UnloadPayload, Phase 6) and BuildStage on the SAME cache. The
// engine's recursive_mutex must serialize all of it -- this asserts no data
// race / no crash (run under build/next-tsan), not a specific composed result.
static void test_concurrent_payload_edits() {
  std::cout << "test_concurrent_payload_edits..." << std::endl;
#if defined(LIGHTUSD_ENABLE_THREAD)
  AssetResolver resolver;
  auto root = BuildRootLayer();
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  std::atomic<bool> stop{false};
  // Reader threads: hammer queries while writers mutate payload state.
  auto reader = [&]() {
    std::string w, e;
    while (!stop.load()) {
      cache.ComputePrimIndex(Path("/World/P"), &w, &e);
      (void)cache.HasDeferredPayload(Path("/World/P"));
      (void)cache.GetDeferredPayloadPaths();
      (void)cache.GetLoadRules().empty();
      (void)cache.ComputedPrimIndexCount();
    }
  };
  // Writer thread: toggle the payload load state repeatedly.
  auto writer = [&]() {
    std::string w, e;
    for (int i = 0; i < 400; ++i) {
      if (i & 1) cache.UnloadPayload(Path("/World/P"));
      else cache.LoadPayload(Path("/World/P"), &w, &e);
      Stage s;
      cache.BuildStage(&s, &w, &e);
    }
    stop.store(true);
  };

  std::vector<std::thread> ts;
  for (int i = 0; i < 6; ++i) ts.emplace_back(reader);
  ts.emplace_back(writer);
  for (auto &t : ts) t.join();
#else
  std::cout << "  (skipped: build with -DLIGHTUSD_NEXT_ENABLE_THREAD=ON)\n";
#endif
  std::cout << "  OK" << std::endl;
}

// FU9 (parallel build): a canonical, index-order-independent serialization of a
// PrimIndex -- strength-ordered (arc, site-string, has-specs) tuples + count.
// Captures everything observable about composition while ignoring the internal
// interning integers (which may differ between a serial and a parallel build).
static std::string CanonIndex(const pcp::PrimIndex *idx) {
  if (!idx) return "<null>";
  std::string s = "n=" + std::to_string(idx->GetNodeCount()) + ";";
  for (uint16_t ni : idx->GetStrengthOrder()) {
    const pcp::CompNode &node = idx->GetNode(ni);
    s += std::to_string(static_cast<int>(node.arc_type)) + ":" +
         idx->SitePath(node) + ":" + (node.has_specs() ? "1" : "0") + "|";
  }
  return s;
}

// Canonical (sorted) dump of the instance/prototype groupings of a cache.
static std::string CanonInstances(const pcp::Cache &cache) {
  std::vector<Path> protos = cache.GetPrototypePaths();
  std::vector<std::string> rows;
  for (const Path &p : protos) {
    std::vector<Path> inst = cache.GetInstancesForPrototype(p);
    std::vector<std::string> names;
    for (const Path &i : inst) names.push_back(i.str());
    std::sort(names.begin(), names.end());
    std::string row = p.str() + "=>";
    for (const std::string &n : names) row += n + ",";
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  std::string out;
  for (const std::string &r : rows) out += r + ";";
  return out;
}

// FU9 (parallel build): building a batch with num_threads>1 must produce indices
// structurally identical to a serial (num_threads=1) build, and the same
// instance/prototype groupings. Exercises the per-worker lock-free build + the
// deterministic input-order merge (incl. ordered prototype assignment).
static void test_parallel_build_matches_serial() {
  std::cout << "test_parallel_build_matches_serial..." << std::endl;
#if defined(LIGHTUSD_ENABLE_THREAD)
  const std::vector<std::string> path_strs = {
      "/World",          "/World/A",     "/World/A/Inner", "/World/I",
      "/World/IR",       "/World/SP",    "/World/V",       "/World/VC",
      "/World/VC/Geom",  "/World/MV",    "/World/Inst1",   "/World/Inst2",
      "/World/Inst3",    "/Lib/Model",   "/Lib/Model/Inner", "/Lib/RefModel"};
  std::vector<Path> paths;
  for (const std::string &p : path_strs) paths.push_back(Path(p));

  auto run = [&](int num_threads) -> std::pair<std::vector<std::string>, std::string> {
    AssetResolver resolver;
    auto root = BuildRootLayer();
    pcp::CompositionOptions opts;
    opts.num_threads = num_threads;
    auto opened = pcp::Cache::Open(resolver, root, "", opts);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    std::string warn, err;
    assert(cache.PrewarmPrimIndices(paths, &warn, &err));
    std::vector<std::string> dumps;
    for (const Path &p : paths) {
      dumps.push_back(CanonIndex(cache.ComputePrimIndex(p, &warn, &err)));
    }
    return {dumps, CanonInstances(cache)};
  };

  auto serial = run(1);
  auto parallel = run(4);

  for (size_t i = 0; i < paths.size(); ++i) {
    if (serial.first[i] != parallel.first[i]) {
      std::cout << "  MISMATCH at " << path_strs[i] << "\n   serial:   "
                << serial.first[i] << "\n   parallel: " << parallel.first[i]
                << std::endl;
    }
    assert(serial.first[i] == parallel.first[i] &&
           "parallel-built index differs from serial");
  }
  assert(serial.second == parallel.second &&
         "parallel instance groupings differ from serial");
  // Sanity: the instanceable prims actually grouped (Inst1+Inst2 share a
  // prototype; Inst3 is its own), so the ordered-merge path was exercised.
  assert(!parallel.second.empty() && "expected instance groupings");
#else
  std::cout << "  (skipped: build with -DLIGHTUSD_NEXT_ENABLE_THREAD=ON)\n";
#endif
  std::cout << "  OK" << std::endl;
}

// --- security: cycle / recursion hardening ----------------------------------
// Adversarial composition graphs must surface errors, never crash or exhaust
// the C++ stack (the module builds with -fno-exceptions).

// `def "A" { def "B" (references = </A>) {} }`: the referenced subtree contains
// the referencing prim, so the composed namespace would grow forever. Caught by
// the same-stack ancestor-arc check in ProcessArc.
static void test_ancestor_reference_cycle() {
  std::cout << "test_ancestor_reference_cycle..." << std::endl;
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("A", "Xform");
    lb.add_property("rootProp", Value::MakeFloat3(1, 1, 1));
    lb.begin_prim("B", "");
    lb.current()->meta().references.push_back("</A>");
    lb.end_prim();
    lb.end_prim();
    lb.finalize();
  }

  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));
  assert(!err.empty() && "ancestor-reference cycle must be reported");
  // The cycle arc is dropped: /A/B exists, but no unbounded /A/B/B/... chain.
  assert(stage.GetPrimAtPath("/A/B").IsValid());
  assert(!stage.GetPrimAtPath("/A/B/B/B/B/B/B/B/B").IsValid());
  std::cout << "  OK" << std::endl;
}

// Mutual reference cycle across two in-memory "files": A -> @b@</B> -> @a@</A>.
// Caught by the expansion frame chain (site revisited within one expansion).
static void test_mutual_reference_cycle() {
  std::cout << "test_mutual_reference_cycle..." << std::endl;
  auto a = std::make_shared<Layer>();
  {
    LayerBuilder ab(*a);
    ab.begin_prim("A", "Mesh");
    ab.add_property("fromA", Value::MakeFloat3(1, 0, 0));
    ab.current()->meta().references.push_back("@asset_b@</B>");
    ab.end_prim();
    ab.finalize();
  }
  auto b = std::make_shared<Layer>();
  {
    LayerBuilder bb(*b);
    bb.begin_prim("B", "");
    bb.add_property("fromB", Value::MakeFloat3(0, 1, 0));
    bb.current()->meta().references.push_back("@asset_a@</A>");
    bb.end_prim();
    bb.finalize();
  }
  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("X", "");
    rb.current()->meta().references.push_back("@asset_a@</A>");
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &p, const std::string &) { return p; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("asset_a", a);
  cache.PreloadLayer("asset_b", b);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));
  assert(!err.empty() && "mutual reference cycle must be reported");
  // Opinions up to the cycle point still compose.
  UsdPrim x = stage.GetPrimAtPath("/X");
  assert(x.IsValid());
  assert(x.GetPropertyValue("fromA") != nullptr);
  assert(x.GetPropertyValue("fromB") != nullptr);
  std::cout << "  OK" << std::endl;
}

// An (internal) payload arc targeting the prim's own site.
static void test_self_payload_cycle() {
  std::cout << "test_self_payload_cycle..." << std::endl;
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("P", "Xform");
    lb.current()->meta().payloads.push_back("</P>");
    lb.end_prim();
    lb.finalize();
  }
  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));
  assert(!err.empty() && "self-payload cycle must be reported");
  assert(stage.GetPrimAtPath("/P").IsValid());
  std::cout << "  OK" << std::endl;
}

// A selected variant whose content subtree references itself.
static void test_variant_content_cycle() {
  std::cout << "test_variant_content_cycle..." << std::endl;
  auto content = std::make_shared<Layer>();
  {
    LayerBuilder cb(*content);
    cb.begin_prim("__self__", "");
    cb.current()->meta().references.push_back("</__self__>");
    cb.add_property("vProp", Value::MakeFloat3(3, 3, 3));
    cb.end_prim();
    cb.finalize();
  }
  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("VC", "");
    VariantSetData vss;
    vss.name = "geo";
    VariantData full;
    full.name = "full";
    full.content = content;
    vss.variants.push_back(std::move(full));
    lb.current()->meta().variantSets().push_back(std::move(vss));
    lb.current()->meta().variantSelection = "geo=full";
    lb.end_prim();
    lb.finalize();
  }
  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));
  assert(!err.empty() && "variant-content self-reference must be reported");
  UsdPrim vc = stage.GetPrimAtPath("/VC");
  assert(vc.IsValid());
  assert(vc.GetPropertyValue("vProp") != nullptr);
  std::cout << "  OK" << std::endl;
}

// A deep (but legitimate) authored hierarchy composes; one beyond the
// namespace-depth backstop errors instead of exhausting the stack.
static void test_deep_hierarchy() {
  std::cout << "test_deep_hierarchy..." << std::endl;
  auto build_deep = [](size_t depth) {
    auto l = std::make_shared<Layer>();
    LayerBuilder lb(*l);
    for (size_t i = 0; i < depth; ++i) lb.begin_prim("P" + std::to_string(i), "Xform");
    for (size_t i = 0; i < depth; ++i) lb.end_prim();
    lb.finalize();
    return l;
  };

  {
    AssetResolver resolver;
    auto opened = pcp::Cache::Open(resolver, build_deep(1000));
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    assert(err.empty() && "1000-deep authored hierarchy must compose");
  }
  {
    AssetResolver resolver;
    pcp::CompositionOptions opts;
    opts.max_namespace_depth = 64;
    auto opened = pcp::Cache::Open(resolver, build_deep(100), "", opts);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    assert(!err.empty() && "beyond-backstop depth must be reported");
  }
  std::cout << "  OK" << std::endl;
}

// A reference chain longer than max_depth errors cleanly; a shorter one
// composes the deepest opinion through.
static void test_reference_chain_at_max_depth() {
  std::cout << "test_reference_chain_at_max_depth..." << std::endl;
  auto run_chain = [](size_t D, uint32_t max_depth, std::string *err_out) {
    std::vector<std::shared_ptr<Layer>> chain;
    for (size_t k = 0; k < D; ++k) {
      auto l = std::make_shared<Layer>();
      LayerBuilder lb(*l);
      lb.begin_prim("A", k + 1 == D ? "Mesh" : "");
      if (k + 1 < D) {
        lb.current()->meta().references.push_back("@chain_" +
                                                  std::to_string(k + 1) + "@</A>");
      } else {
        lb.add_property("deepest", Value::MakeFloat3(9, 9, 9));
      }
      lb.end_prim();
      lb.finalize();
      chain.push_back(l);
    }
    auto rootL = std::make_shared<Layer>();
    {
      LayerBuilder rb(*rootL);
      rb.begin_prim("Top", "");
      rb.current()->meta().references.push_back("@chain_0@</A>");
      rb.end_prim();
      rb.finalize();
    }
    AssetResolver resolver;
    resolver.SetCustomResolver(
        [](const std::string &p, const std::string &) { return p; });
    pcp::CompositionOptions opts;
    opts.max_depth = max_depth;
    auto opened = pcp::Cache::Open(resolver, rootL, "", opts);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    for (size_t k = 0; k < D; ++k)
      cache.PreloadLayer("chain_" + std::to_string(k), chain[k]);
    Stage stage;
    std::string warn;
    assert(cache.BuildStage(&stage, &warn, err_out));
    return stage.GetPrimAtPath("/Top").GetPropertyValue("deepest") != nullptr;
  };

  std::string err;
  assert(run_chain(100, 256, &err) && err.empty() &&
         "short chain must compose fully");
  err.clear();
  assert(!run_chain(300, 256, &err) && !err.empty() &&
         "over-max_depth chain must error (and not crash)");
  std::cout << "  OK" << std::endl;
}

// Phase 7 E4: typed composition diagnostics. Asserts that the same
// ancestor-reference cycle that fills `err` also surfaces a typed ArcCycle
// issue, that issues are recorded even with err==nullptr, and that
// ClearCompositionIssues drops them.
static void test_typed_composition_issues() {
  std::cout << "test_typed_composition_issues..." << std::endl;
  using EC = pcp::Cache::ErrorCode;

  auto root = std::make_shared<Layer>();
  {
    LayerBuilder lb(*root);
    lb.begin_prim("A", "Xform");
    lb.begin_prim("B", "");
    lb.current()->meta().references.push_back("</A>");  // ancestor cycle
    lb.end_prim();
    lb.end_prim();
    lb.begin_prim("Source", "Xform");
    lb.end_prim();
    lb.begin_prim("InvalidOffset", "");
    lb.current()->meta().references.push_back(
        "</Source>?layerOffset=0:0");
    lb.end_prim();
    lb.finalize();
  }

  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, root);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  // Build with err==nullptr: typed issues must still accumulate.
  Stage stage;
  assert(cache.BuildStage(&stage, nullptr, nullptr));
  const auto &issues = cache.GetCompositionIssues();
  bool saw_cycle = false;
  bool saw_invalid_offset = false;
  for (const auto &iss : issues) {
    if (iss.code == EC::ArcCycle) {
      saw_cycle = true;
      assert(!iss.message.empty() && !iss.site.empty());
    }
    if (iss.code == EC::InvalidReferenceOffset) {
      saw_invalid_offset = true;
      assert(iss.message.find("identity") != std::string::npos);
    }
  }
  assert(saw_cycle && "ArcCycle issue must be recorded even with err==nullptr");
  assert(saw_invalid_offset &&
         "invalid arc offsets must surface a typed diagnostic");

  // Invalidate begins a fresh diagnostics cycle (and bounds accumulation).
  cache.Invalidate(Path("/A"));
  assert(cache.GetCompositionIssues().empty() && "Invalidate must drop issues");

  // Recomposing re-accumulates, and an explicit Clear drops them again.
  Stage stage2;
  assert(cache.BuildStage(&stage2, nullptr, nullptr));
  assert(!cache.GetCompositionIssues().empty() && "recompose re-accumulates");
  cache.ClearCompositionIssues();
  assert(cache.GetCompositionIssues().empty() && "Clear must drop issues");
  std::cout << "  OK" << std::endl;
}

// Phase 9 F5: the per-prim ComputePrimIndex prototype grouping must be
// independent of the order in which prims are computed. /World/Inst1 and
// /World/Inst2 share an instance key; whichever is computed first, the
// deterministic prototype is the lexicographically-smallest path (Inst1).
static void test_prototype_order_independent() {
  std::cout << "test_prototype_order_independent..." << std::endl;
  AssetResolver resolver;
  std::string warn, err;

  auto compute_in_order = [&](const std::vector<std::string> &order) {
    auto root = BuildRootLayer();
    auto opened = pcp::Cache::Open(resolver, root);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    for (const std::string &p : order) {
      cache.ComputePrimIndex(Path(p), &warn, &err);
    }
    // Both instances must agree on the prototype, and it must be the min path.
    Path p1 = cache.GetPrototype(Path("/World/Inst1"));
    Path p2 = cache.GetPrototype(Path("/World/Inst2"));
    assert(p1 == p2 && "Inst1/Inst2 must share a prototype");
    assert(p1 == Path("/World/Inst1") &&
           "prototype must be the lexicographically-smallest member");
    // Exactly one is the prototype (Inst1), the other an instance (Inst2).
    assert(!cache.IsInstance(Path("/World/Inst1")));
    assert(cache.IsInstance(Path("/World/Inst2")));
    return p1.str();
  };

  // Compute Inst2 before Inst1, and vice versa: same result either way.
  std::string a = compute_in_order({"/World/Inst2", "/World/Inst1"});
  std::string b = compute_in_order({"/World/Inst1", "/World/Inst2"});
  assert(a == b && "prototype choice must not depend on computation order");
  std::cout << "  OK" << std::endl;
}

// Regression: pcp::Cache must honor the per-set VariantSetData.selected field as
// a selection opinion. The crate (USDC) reader sets `selected` for EVERY chosen
// variant set but the legacy variantSelection STRING for only the FIRST set (and
// no variantSelections map). Reading only the string dropped every set after the
// first on a multi-set USDC selection; a vs.selected-only selection was ignored
// entirely.
static void test_variant_selected_field() {
  std::cout << "test_variant_selected_field..." << std::endl;

  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("World", "Xform");

    // P selects TWO sets in the crate-reader form: `selected` on both sets, the
    // legacy string on only the FIRST, no variantSelections map.
    rb.begin_prim("P", "");
    {
      VariantSetData lod;
      lod.name = "lod";
      lod.selected = "high";
      {
        VariantData v; v.name = "high";
        v.properties.push_back({"L", Value::MakeFloat3(9, 9, 9)});
        lod.variants.push_back(std::move(v));
      }
      {
        VariantData v; v.name = "low";
        v.properties.push_back({"L", Value::MakeFloat3(1, 1, 1)});
        lod.variants.push_back(std::move(v));
      }
      VariantSetData shade;
      shade.name = "shade";
      shade.selected = "red";
      {
        VariantData v; v.name = "red";
        v.properties.push_back({"S", Value::MakeFloat3(1, 0, 0)});
        shade.variants.push_back(std::move(v));
      }
      rb.current()->meta().variantSets().push_back(std::move(lod));
      rb.current()->meta().variantSets().push_back(std::move(shade));
      rb.current()->meta().variantSelection = "lod=high";  // FIRST set only
    }
    rb.end_prim();

    // Q selects a single set via `selected` ONLY (no string, no map).
    rb.begin_prim("Q", "");
    {
      VariantSetData s;
      s.name = "geo";
      s.selected = "hi";
      VariantData v; v.name = "hi";
      v.properties.push_back({"G", Value::MakeFloat3(2, 2, 2)});
      s.variants.push_back(std::move(v));
      rb.current()->meta().variantSets().push_back(std::move(s));
    }
    rb.end_prim();

    rb.end_prim();  // World
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim p = stage.GetPrimAtPath("/World/P");
  assert(p.IsValid());
  assert(p.GetPropertyValue("L") != nullptr &&
         "multi-set: first set 'lod' applied");
  assert(p.GetPropertyValue("S") != nullptr &&
         "multi-set: SECOND set 'shade' applied (dropped if vs.selected ignored)");

  UsdPrim q = stage.GetPrimAtPath("/World/Q");
  assert(q.IsValid());
  assert(q.GetPropertyValue("G") != nullptr &&
         "vs.selected-only selection applied");

  std::cout << "  OK" << std::endl;
}


// ------------------------------------------------------------------
// Regression tests from the 2026-07 composition/pcp audit.
// ------------------------------------------------------------------

// Authored `active = false` must survive weaker sources (a referenced prim's
// DEFAULT active=true used to flip it back — the inverse of LIVRPS).
static void test_active_survives_weaker_arc() {
  std::cout << "test_active_survives_weaker_arc..." << std::endl;
  const std::string ref = "/tmp/next_act_ref.usda";
  const std::string root = "/tmp/next_act_root.usda";
  {
    std::ofstream o(ref);
    o << "#usda 1.0\ndef Xform \"R\"\n{\n    float x = 1\n}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def Xform \"A\" (\n"
         "    active = false\n"
         "    prepend references = @./next_act_ref.usda@</R>\n"
         ")\n{\n}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  UsdPrim a = stage.GetPrimAtPath("/A");
  assert(a.IsValid());
  assert(!a.IsActive() && "authored active=false clobbered by weaker source");
  assert(a.GetPropertyValue("x") != nullptr && "reference still composes");
  std::remove(ref.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// Relationship / connection targets pointing OUTSIDE a referenced subtree are
// not expressible in the composed namespace and must be dropped (pxr warns
// and drops); leaking them verbatim silently aliased unrelated stage prims.
// In-scope targets keep retargeting.
static void test_out_of_scope_targets_dropped() {
  std::cout << "test_out_of_scope_targets_dropped..." << std::endl;
  const std::string asset = "/tmp/next_scope_asset.usda";
  const std::string root = "/tmp/next_scope_root.usda";
  {
    std::ofstream o(asset);
    o << "#usda 1.0\n"
         "def Xform \"Model\"\n{\n"
         "    def Mesh \"Geo\"\n    {\n"
         "        rel material:binding = </Materials/Red>\n"
         "        rel ok = </Model/Geo>\n"
         "    }\n"
         "}\n"
         "def Scope \"Materials\" { def Material \"Red\" { } }\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def Xform \"W\" (prepend references = "
         "@./next_scope_asset.usda@</Model>)\n{\n}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  UsdPrim geo = stage.GetPrimAtPath("/W/Geo");
  assert(geo.IsValid());
  const std::vector<Path>* binding = geo.GetRelationship("material:binding");
  assert((!binding || binding->empty()) &&
         "out-of-scope rel target must be dropped, not leaked verbatim");
  const std::vector<Path>* ok = geo.GetRelationship("ok");
  assert(ok && ok->size() == 1 && (*ok)[0].str() == "/W/Geo" &&
         "in-scope rel target retargeted");
  std::remove(asset.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// HasDeferredPayload must not depend on authored payload ORDER: with two
// payloads where only one loads, a later loaded arc used to erase the
// deferral marker set by an earlier deferred one.
static void test_deferred_payload_marker_order() {
  std::cout << "test_deferred_payload_marker_order..." << std::endl;
  char directory_template[] = "/tmp/lightusd-next-pcp-XXXXXX";
  char* directory = ::mkdtemp(directory_template);
  PCP_CHECK(directory != nullptr, "create isolated PCP temporary directory");
  if (!directory) return;
  const std::string temp_dir(directory);
  const std::string pay1 = temp_dir + "/next_def_p1.usda";
  const std::string pay2 = temp_dir + "/next_def_p2.usda";
  {
    std::ofstream o(pay1);
    o << "#usda 1.0\ndef Xform \"P1\" { float a = 1 }\n";
  }
  {
    std::ofstream o(pay2);
    o << "#usda 1.0\ndef Xform \"P2\" { float b = 2 }\n";
  }
  for (int order = 0; order < 2; ++order) {
    const std::string root = temp_dir + "/next_def_root.usda";
    {
      std::ofstream o(root);
      o << "#usda 1.0\n"
           "def Xform \"A\" (prepend payload = [";
      if (order == 0) {
      o << "@./next_def_p2.usda@</P2>, @./next_def_p1.usda@</P1>";
      } else {
        o << "@./next_def_p1.usda@</P1>, @./next_def_p2.usda@</P2>";
      }
      o << "])\n{\n}\n";
    }
    AssetResolver resolver;
    resolver.SetWorkingDirectory(temp_dir);
    pcp::CompositionOptions opts;
    // Load only p1; p2 stays deferred — regardless of arc order.
    opts.payload_policy = [](const Path&, const std::string& asset) {
      return asset.find("p1") != std::string::npos;
    };
    auto layer = std::make_shared<Layer>();
    {
      Stage tmp;
      std::string w, e;
      assert(LoadUSDA(root, &tmp, &w, &e));
      layer = std::shared_ptr<Layer>(tmp.ReleaseRootLayer());
    }
    auto opened = pcp::Cache::Open(resolver, layer, root, opts);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    UsdPrim a = stage.GetPrimAtPath("/A");
    assert(a.GetPropertyValue("a") != nullptr && "loaded payload composes");
    assert(a.GetPropertyValue("b") == nullptr && "deferred payload leaked");
    assert(cache.HasDeferredPayload(Path("/A")) &&
           "deferral marker lost (payload order dependence)");
  std::remove(root.c_str());
  }
  std::remove(pay1.c_str());
  std::remove(pay2.c_str());
  ::rmdir(temp_dir.c_str());
  std::cout << "  OK" << std::endl;
}

// A per-sublayer layer offset must not fail the whole load (offsets are
// parsed and skipped with a warning until composition plumbs them through).
static void test_sublayer_offset_parses() {
  std::cout << "test_sublayer_offset_parses..." << std::endl;
  const std::string sub = "/tmp/next_slo_sub.usda";
  const std::string root = "/tmp/next_slo_root.usda";
  {
    std::ofstream o(sub);
    o << "#usda 1.0\ndef Xform \"p\" { float a = 1 }\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n(\n"
         "    subLayers = [ @./next_slo_sub.usda@ (offset = 10; scale = 2) ]\n"
         ")\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err) &&
         "sublayer layer-offset syntax must not fail the load");
  assert(stage.GetPrimAtPath("/p").IsValid());
  std::remove(sub.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}


// 2026-07 deferred audit items: layer-level relocates apply during
// BuildStage (cross-parent move, matching pxr), and the applied list is
// dropped from the flattened output metadata.
static void test_layer_relocates() {
  std::cout << "test_layer_relocates..." << std::endl;

  auto asset = std::make_shared<Layer>();
  {
    LayerBuilder ab(*asset);
    ab.begin_prim("CharRig", "Xform");
    ab.begin_prim("Rig", "Xform");
    ab.begin_prim("Anim", "Xform");
    ab.add_property("v", Value(int32_t(7)));
    ab.end_prim();
    ab.end_prim();
    ab.end_prim();
    ab.finalize();
  }
  auto rootL = std::make_shared<Layer>();
  {
    rootL->meta().relocates.emplace_back("/Char/Rig/Anim", "/Char/Anim");
    LayerBuilder rb(*rootL);
    rb.begin_prim("Char", "Xform");
    rb.current()->meta().references.push_back("@mem_rig@</CharRig>");
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("mem_rig", asset);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim moved = stage.GetPrimAtPath("/Char/Anim");
  assert(moved.IsValid() && "relocated prim must exist at the new path");
  const Value* v = moved.GetPropertyValue("v");
  assert(v && v->as_int() && *v->as_int() == 7);
  UsdPrim old_loc = stage.GetPrimAtPath("/Char/Rig/Anim");
  assert(!old_loc.IsValid() && "source location must be vacated");
  UsdPrim rig = stage.GetPrimAtPath("/Char/Rig");
  assert(rig.IsValid() && "unrelocated sibling structure intact");
  // Applied relocates must not survive into the flattened metadata.
  assert(stage.GetRootLayer()->meta().relocates.empty());
  std::cout << "  OK" << std::endl;
}

// Relocates are a per-LAYER-STACK namespace edit, not a root-layer-only one: a
// relocate authored in a REFERENCED layer renames prims inside the referenced
// namespace, and the rename maps through the arc into root space. Mirrors the
// AOUSD supplemental TrickyMultipleRelocations case:
//   subrig.usd  /SubRig/Anim/AnimScope        (content, delivered by a reference)
//               /SubRig/Rig2                  (shadow spec at a relocated path)
//   rig.usd     /CharRig{Anim, Rig{SubRig -> @subrig@</SubRig>}}
//               relocates { /CharRig/Rig/SubRig/Anim/AnimScope -> /CharRig/Anim/AnimScope }
//   root.usd    /Char -> @rig@</CharRig>
//               relocates { /Char/Anim -> /Char/Anim2 }   (renames on top of the above)
static void test_relocates_in_referenced_layer_stack() {
  std::cout << "test_relocates_in_referenced_layer_stack..." << std::endl;

  auto subrig = std::make_shared<Layer>();
  {
    LayerBuilder sb(*subrig);
    sb.begin_prim("SubRig", "Xform");
    sb.begin_prim("Anim", "Xform");
    sb.begin_prim("AnimScope", "Xform");
    sb.add_property("x", Value(int32_t(42)));
    sb.end_prim();
    sb.end_prim();
    sb.end_prim();
    sb.finalize();
  }
  auto rig = std::make_shared<Layer>();
  {
    rig->meta().relocates.emplace_back("/CharRig/Rig/SubRig/Anim/AnimScope",
                                       "/CharRig/Anim/AnimScope");
    LayerBuilder rb(*rig);
    rb.begin_prim("CharRig", "Xform");
    rb.begin_prim("Anim", "Xform");
    rb.end_prim();
    rb.begin_prim("Rig", "Xform");
    rb.begin_prim("SubRig", "Xform");
    rb.current()->meta().references.push_back("@mem_subrig@</SubRig>");
    rb.end_prim();
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }
  auto rootL = std::make_shared<Layer>();
  {
    rootL->meta().relocates.emplace_back("/Char/Anim", "/Char/Anim2");
    LayerBuilder lb(*rootL);
    lb.begin_prim("Char", "Xform");
    lb.current()->meta().references.push_back("@mem_rig@</CharRig>");
    lb.end_prim();
    // An opinion authored at the relocate TARGET path composes on top of the
    // relocated content (the target address is a real site in this layer stack).
    lb.begin_prim("Char2Anim2Holder", "Xform");
    lb.end_prim();
    lb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("mem_rig", rig);
  cache.PreloadLayer("mem_subrig", subrig);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  // The referenced stack's relocate moved AnimScope up to /CharRig/Anim, which
  // the root stack's relocate then renamed to Anim2 -- and the content still
  // resolves through the SubRig reference into subrig.usd.
  UsdPrim moved = stage.GetPrimAtPath("/Char/Anim2/AnimScope");
  assert(moved.IsValid() && "relocate authored in a referenced layer stack");
  const Value *x = moved.GetPropertyValue("x");
  assert(x && x->as_int() && *x->as_int() == 42);
  // Both source locations are vacated, in both namespaces.
  assert(!stage.GetPrimAtPath("/Char/Anim").IsValid());
  assert(!stage.GetPrimAtPath("/Char/Rig/SubRig/Anim/AnimScope").IsValid());
  // The unrelocated structure around them is intact.
  assert(stage.GetPrimAtPath("/Char/Rig/SubRig/Anim").IsValid());
  assert(stage.GetRootLayer()->meta().relocates.empty());
  std::cout << "  OK" << std::endl;
}

// A relocate may move a prim to a NEW ROOT PRIM; opinions authored at the new
// root name compose on top of the relocated content (supplemental
// TrickyInheritsAndRelocatesToNewRootPrim).
static void test_relocate_to_new_root_prim() {
  std::cout << "test_relocate_to_new_root_prim..." << std::endl;

  // The relocate source must be ARC-INTRODUCED (pxr): the reference sits on
  // the ANCESTOR /Group, delivering the Model child (museum
  // TrickyInheritsAndRelocatesToNewRootPrim shape).
  auto model = std::make_shared<Layer>();
  {
    LayerBuilder mb(*model);
    mb.begin_prim("GroupSrc", "Xform");
    mb.begin_prim("Model", "Xform");
    mb.begin_prim("Scope", "Xform");
    mb.add_property("v", Value(int32_t(3)));
    mb.end_prim();
    mb.end_prim();
    mb.end_prim();
    mb.finalize();
  }
  auto rootL = std::make_shared<Layer>();
  {
    rootL->meta().relocates.emplace_back("/Group/Model", "/Model_Renamed");
    LayerBuilder rb(*rootL);
    rb.begin_prim("Group", "Xform");
    rb.current()->meta().references.push_back("@mem_model@</GroupSrc>");
    rb.end_prim();
    // An `over` at the new root name: authored at the relocate TARGET.
    rb.begin_prim("Model_Renamed", "");
    rb.current()->set_specifier(PrimSpecifier::Over);
    rb.add_property("tag", Value(std::string("target")));
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  resolver.SetCustomResolver(
      [](const std::string &a, const std::string &) { return a; });
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  cache.PreloadLayer("mem_model", model);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim moved = stage.GetPrimAtPath("/Model_Renamed");
  assert(moved.IsValid() && "relocate to a new root prim");
  const Value *tag = moved.GetPropertyValue("tag");
  assert(tag && tag->as_string() && *tag->as_string() == "target");
  UsdPrim scope = stage.GetPrimAtPath("/Model_Renamed/Scope");
  assert(scope.IsValid() && "relocated subtree follows the prim");
  const Value *v = scope.GetPropertyValue("v");
  assert(v && v->as_int() && *v->as_int() == 3);
  assert(!stage.GetPrimAtPath("/Group/Model").IsValid());
  assert(stage.GetPrimAtPath("/Group").IsValid());
  std::cout << "  OK" << std::endl;
}

// An inherit/specialize target may be authored RELATIVE to the prim that
// authors it (`<../_Y>`), which resolves against the authoring site.
static void test_relative_class_arc_target() {
  std::cout << "test_relative_class_arc_target..." << std::endl;

  auto rootL = std::make_shared<Layer>();
  {
    LayerBuilder rb(*rootL);
    rb.begin_prim("X", "");
    rb.current()->set_specifier(PrimSpecifier::Class);
    rb.begin_prim("_Y", "");
    rb.current()->set_specifier(PrimSpecifier::Class);
    rb.add_property("v", Value(int32_t(9)));
    rb.end_prim();
    rb.begin_prim("B", "Xform");
    rb.current()->meta().inherits.push_back("<../_Y>");
    rb.end_prim();
    rb.end_prim();
    rb.finalize();
  }

  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);

  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  UsdPrim b = stage.GetPrimAtPath("/X/B");
  assert(b.IsValid());
  const Value *v = b.GetPropertyValue("v");
  assert(v && v->as_int() && *v->as_int() == 9 &&
         "relative inherit target must resolve against the authoring prim");
  std::cout << "  OK" << std::endl;
}

// Composed specifier resolution (pxr _GetPrimSpecifierImpl): a defining
// specifier always beats `over`, and a `class` due to a DIRECT inherit is
// weaker than any other defining specifier (classness is not inherited) —
// but a class reached through an ANCESTRAL inherit resolves in plain
// strength order and must NOT be downgraded by a weaker `def`.
static void test_specifier_resolution() {
  std::cout << "test_specifier_resolution..." << std::endl;

  // Case 1 (regression): /World/Foo inherits /_C; `class _C { class Child }`;
  // _C inherits /_D; `class _D { def Child }`. The inherit arcs are introduced
  // at Foo, so for the CHILD they are ancestral: /World/Foo/Child stays class.
  {
    auto rootL = std::make_shared<Layer>();
    {
      LayerBuilder rb(*rootL);
      rb.begin_prim("_D", "");
      rb.current()->set_specifier(PrimSpecifier::Class);
      rb.begin_prim("Child", "Scope");  // def Child
      rb.end_prim();
      rb.end_prim();
      rb.begin_prim("_C", "");
      rb.current()->set_specifier(PrimSpecifier::Class);
      rb.current()->meta().inherits.push_back("</_D>");
      rb.begin_prim("Child", "Scope");
      rb.current()->set_specifier(PrimSpecifier::Class);
      rb.end_prim();
      rb.end_prim();
      rb.begin_prim("World", "Xform");
      rb.begin_prim("Foo", "Xform");
      rb.current()->meta().inherits.push_back("</_C>");
      rb.end_prim();
      rb.end_prim();
      rb.finalize();
    }
    AssetResolver resolver;
    auto opened = pcp::Cache::Open(resolver, rootL);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    UsdPrim child = stage.GetPrimAtPath("/World/Foo/Child");
    assert(child.IsValid());
    assert(child.GetSpecifier() == PrimSpecifier::Class &&
           "ancestral-inherit class must not be downgraded by a weaker def");
    UsdPrim foo = stage.GetPrimAtPath("/World/Foo");
    assert(foo.IsValid() && foo.GetSpecifier() == PrimSpecifier::Def);
  }

  // Case 2 (pxr stage.cpp doc case): `over A (references @other@</B>)` where
  // `def B (inherits </C>)` and `class C {}`. The class comes from a DIRECT
  // inherit, so the weaker def from /B wins: /A composes as `def`.
  {
    auto other = std::make_shared<Layer>();
    {
      LayerBuilder ob(*other);
      ob.begin_prim("C", "");
      ob.current()->set_specifier(PrimSpecifier::Class);
      ob.end_prim();
      ob.begin_prim("B", "Xform");
      ob.current()->meta().inherits.push_back("</C>");
      ob.end_prim();
      ob.finalize();
    }
    auto rootL = std::make_shared<Layer>();
    {
      LayerBuilder rb(*rootL);
      rb.begin_prim("C", "");
      rb.current()->set_specifier(PrimSpecifier::Class);
      rb.end_prim();
      rb.begin_prim("A", "");
      rb.current()->set_specifier(PrimSpecifier::Over);
      rb.current()->meta().references.push_back("@mem_other@</B>");
      rb.end_prim();
      rb.finalize();
    }
    AssetResolver resolver;
    resolver.SetCustomResolver(
        [](const std::string &a, const std::string &) { return a; });
    auto opened = pcp::Cache::Open(resolver, rootL);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    cache.PreloadLayer("mem_other", other);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    UsdPrim a = stage.GetPrimAtPath("/A");
    assert(a.IsValid());
    assert(a.GetSpecifier() == PrimSpecifier::Def &&
           "class due to a direct inherit must lose to a weaker def");
  }

  // Case 3: an `over` prim with direct inherits [class, def] composes as
  // `def` regardless of the inherit list order (a direct-inherit class is
  // weaker than any other defining specifier).
  {
    auto rootL = std::make_shared<Layer>();
    {
      LayerBuilder rb(*rootL);
      rb.begin_prim("_CLS", "");
      rb.current()->set_specifier(PrimSpecifier::Class);
      rb.end_prim();
      rb.begin_prim("_DEF", "Scope");  // def
      rb.end_prim();
      rb.begin_prim("O", "");
      rb.current()->set_specifier(PrimSpecifier::Over);
      rb.current()->meta().inherits.push_back("</_CLS>");
      rb.current()->meta().inherits.push_back("</_DEF>");
      rb.end_prim();
      rb.finalize();
    }
    AssetResolver resolver;
    auto opened = pcp::Cache::Open(resolver, rootL);
    assert(opened);
    pcp::Cache cache = std::move(*opened);
    Stage stage;
    std::string warn, err;
    assert(cache.BuildStage(&stage, &warn, &err));
    UsdPrim o = stage.GetPrimAtPath("/O");
    assert(o.IsValid());
    assert(o.GetSpecifier() == PrimSpecifier::Def &&
           "def among direct inherits wins over class regardless of order");
  }

  std::cout << "  OK" << std::endl;
}

// Variable expressions in SUBLAYER asset paths (evaluated against the stack
// root layer's expressionVariables) and in VARIANT SELECTIONS (evaluated
// against the source's composed expression variables).
static void test_expression_sublayer_and_variant_selection() {
  std::cout << "test_expression_sublayer_and_variant_selection..." << std::endl;
  const std::string sub = "/tmp/next_exprvar_sub.usda";
  const std::string root = "/tmp/next_exprvar_root.usda";
  {
    std::ofstream o(sub);
    o << "#usda 1.0\n"
         "def Xform \"FromSub\" { int tag = 42 }\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "(\n"
         "    expressionVariables = {\n"
         "        string SUB = \"next_exprvar_sub.usda\"\n"
         "        string COLOR = \"green\"\n"
         "    }\n"
         "    subLayers = [@`${SUB}`@]\n"
         ")\n"
         "def Xform \"Root\" (\n"
         "    variants = { string shadingVariant = \"`if(eq(${COLOR}, "
         "\\\"green\\\"), \\\"green\\\", \\\"red\\\")`\" }\n"
         "    prepend variantSets = \"shadingVariant\"\n"
         ") {\n"
         "    variantSet \"shadingVariant\" = {\n"
         "        \"red\" { int c = 1 }\n"
         "        \"green\" { int c = 2 }\n"
         "    }\n"
         "}\n";
  }

  AssetResolver resolver;
  Stage stage;
  pcp::CompositionOptions opts;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, opts, &warn, &err));
  UsdPrim from_sub = stage.GetPrimAtPath("/FromSub");
  assert(from_sub.IsValid() &&
         "sublayer expression must resolve against the root layer's "
         "expressionVariables");
  const Value* tag = from_sub.GetPropertyValue("tag");
  assert(tag && tag->as_int() && *tag->as_int() == 42);
  UsdPrim root_prim = stage.GetPrimAtPath("/Root");
  assert(root_prim.IsValid());
  const Value* c = root_prim.GetPropertyValue("c");
  assert(c && c->as_int() && *c->as_int() == 2 &&
         "variant selection expression must evaluate before selection");
  std::cout << "  OK" << std::endl;
}

// Layer-stack identity includes expression variables (pxr keys stacks by
// (identifier, expression variables)): the SAME asset referenced from two
// sites whose inherited variables differ must produce two DISTINCT layer
// stacks, and each stack's sublayer expressions must evaluate against the
// referencing site's variables (root-most opinion wins).
static void test_layer_stack_identity_expression_vars() {
  std::cout << "test_layer_stack_identity_expression_vars..." << std::endl;
  {
    std::ofstream o("/tmp/next_stackid_sub_a.usda");
    o << "#usda 1.0\ndef Xform \"X\" { int tag = 1 }\n";
  }
  {
    std::ofstream o("/tmp/next_stackid_sub_b.usda");
    o << "#usda 1.0\ndef Xform \"X\" { int tag = 2 }\n";
  }
  {
    // The shared lib: its only content arrives through an EXPRESSION-valued
    // sublayer path; the variable is authored by the REFERENCING stacks.
    std::ofstream o("/tmp/next_stackid_lib.usda");
    o << "#usda 1.0\n( subLayers = [@`${SUB}`@] )\n";
  }
  {
    std::ofstream o("/tmp/next_stackid_mid_a.usda");
    o << "#usda 1.0\n"
         "( expressionVariables = { string SUB = \"next_stackid_sub_a.usda\" } )\n"
         "def Xform \"P\" (references = @next_stackid_lib.usda@</X>) {}\n";
  }
  {
    std::ofstream o("/tmp/next_stackid_mid_b.usda");
    o << "#usda 1.0\n"
         "( expressionVariables = { string SUB = \"next_stackid_sub_b.usda\" } )\n"
         "def Xform \"Q\" (references = @next_stackid_lib.usda@</X>) {}\n";
  }
  {
    std::ofstream o("/tmp/next_stackid_root.usda");
    o << "#usda 1.0\n"
         "def Xform \"A\" (references = @next_stackid_mid_a.usda@</P>) {}\n"
         "def Xform \"B\" (references = @next_stackid_mid_b.usda@</Q>) {}\n";
  }

  AssetResolver resolver;
  Stage stage;
  pcp::CompositionOptions opts;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile("/tmp/next_stackid_root.usda", resolver,
                                   &stage, opts, &warn, &err));
  const Value* a = stage.GetPrimAtPath("/A").GetPropertyValue("tag");
  const Value* b = stage.GetPrimAtPath("/B").GetPropertyValue("tag");
  assert(a && a->as_int() && *a->as_int() == 1 &&
         "lib referenced under SUB=sub_a must compose sub_a's content");
  assert(b && b->as_int() && *b->as_int() == 2 &&
         "the same lib under SUB=sub_b must be a DISTINCT stack with "
         "sub_b's content");
  std::cout << "  OK" << std::endl;
}

// Generated multi-layer string list-op combinations (item 4b breadth):
// every qualifier of a STRONGER layer's apiSchemas edit applied over a
// weaker layer's explicit list, plus a clipSets ordering edit. Locks in the
// weakest->strongest AOUSD list-op merge across a sublayer stack.
static void test_cross_layer_string_listop_matrix() {
  std::cout << "test_cross_layer_string_listop_matrix..." << std::endl;
  const std::string weak_path = "/tmp/next_listop_weak.usda";
  {
    std::ofstream o(weak_path);
    o << "#usda 1.0\n"
         "def Xform \"P\" (\n"
         "    apiSchemas = [\"CollectionAPI:a\", \"CollectionAPI:b\"]\n"
         ") {}\n";
  }
  struct Case {
    const char* edit;                       // authored in the STRONG layer
    std::vector<std::string> expected;      // composed apiSchemas
  };
  const Case cases[] = {
      {"apiSchemas = [\"NewAPI\"]", {"NewAPI"}},  // explicit replaces
      {"prepend apiSchemas = [\"NewAPI\"]",
       {"NewAPI", "CollectionAPI:a", "CollectionAPI:b"}},
      {"append apiSchemas = [\"NewAPI\"]",
       {"CollectionAPI:a", "CollectionAPI:b", "NewAPI"}},
      {"add apiSchemas = [\"NewAPI\"]",
       {"CollectionAPI:a", "CollectionAPI:b", "NewAPI"}},
      {"delete apiSchemas = [\"CollectionAPI:b\"]", {"CollectionAPI:a"}},
      {"reorder apiSchemas = [\"CollectionAPI:b\", \"CollectionAPI:a\"]",
       {"CollectionAPI:b", "CollectionAPI:a"}},
      // Multi-op in one stronger layer: delete + prepend compose.
      {"prepend apiSchemas = [\"NewAPI\"]\n"
       "    delete apiSchemas = [\"CollectionAPI:a\"]",
       {"NewAPI", "CollectionAPI:b"}},
  };
  for (const Case& c : cases) {
    const std::string root_path = "/tmp/next_listop_root.usda";
    {
      std::ofstream o(root_path);
      o << "#usda 1.0\n"
           "( subLayers = [@next_listop_weak.usda@] )\n"
           "over \"P\" (\n    "
        << c.edit << "\n) {}\n";
    }
    AssetResolver resolver;
    Stage stage;
    pcp::CompositionOptions opts;
    std::string warn, err;
    assert(pcp::ComposeStageFromFile(root_path, resolver, &stage, opts, &warn,
                                     &err)
               ? true
               : (std::cerr << err << std::endl, false));
    UsdPrim p = stage.GetPrimAtPath("/P");
    assert(p.IsValid());
    const std::vector<std::string>& got = p.GetMeta().apiSchemas();
    if (got != c.expected) {
      std::cerr << "  edit: " << c.edit << "\n  got: [";
      for (const auto& s : got) std::cerr << s << ", ";
      std::cerr << "]" << std::endl;
    }
    assert(got == c.expected && "cross-layer apiSchemas list-op combination");
  }
  std::cout << "  OK" << std::endl;
}

// P2 audit: a reference that names no prim path to a layer without an
// authored defaultPrim contributes NOTHING and warns (pxr: "Unresolved
// reference prim path @...@<defaultPrim>") — never silently the first root
// prim.
static void test_p2_ref_no_default_prim() {
  std::cout << "test_p2_ref_no_default_prim..." << std::endl;
  const std::string lib = "/tmp/next_p2_nodef_lib.usda";
  const std::string root = "/tmp/next_p2_nodef_root.usda";
  {
    std::ofstream o(lib);
    o << "#usda 1.0\n"
         "def Xform \"First\" { double size = 1 }\n"
         "def Xform \"Second\" { double size = 2 }\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def Xform \"Hello\" (prepend references = "
         "@./next_p2_nodef_lib.usda@) { }\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err) &&
         "unresolved default-prim reference must not fail the compose");
  UsdPrim hello = stage.GetPrimAtPath("/Hello");
  assert(hello.IsValid() && "referencing prim kept (empty)");
  assert(hello.GetPropertyValue("size") == nullptr &&
         "no silent fallback to the first root prim");
  assert(warn.find("defaultPrim") != std::string::npos &&
         "warning names the missing defaultPrim");
  std::remove(lib.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// P2 audit: BuildStage prunes the SUBTREE of a deactivated prim (pxr composes
// no children under active=false; the prim itself is kept, with its authored
// active=false, so consumers can query IsActive). A stronger layer's
// active=true re-activation keeps the subtree.
static void test_p2_inactive_subtree_pruned() {
  std::cout << "test_p2_inactive_subtree_pruned..." << std::endl;
  const std::string f = "/tmp/next_p2_inactive.usda";
  {
    std::ofstream o(f);
    o << "#usda 1.0\n"
         "def Xform \"A\" (active = false)\n{\n"
         "    def Sphere \"B\" { double radius = 3 }\n"
         "}\n"
         "def Xform \"C\" { double v = 1 }\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(f, resolver, &stage, {}, &warn, &err));
  UsdPrim a = stage.GetPrimAtPath("/A");
  assert(a.IsValid() && !a.IsActive() &&
         "inactive prim kept with active=false");
  assert(!stage.GetPrimAtPath("/A/B").IsValid() &&
         "child of inactive prim pruned from the stage");
  assert(stage.GetPrimAtPath("/C").IsValid() && "sibling untouched");
  std::remove(f.c_str());

  // Re-activation: root-authored active=true is stronger than the referenced
  // prim's active=false — the referenced subtree must survive.
  const std::string ref = "/tmp/next_p2_react_ref.usda";
  const std::string root = "/tmp/next_p2_react_root.usda";
  {
    std::ofstream o(ref);
    o << "#usda 1.0\n"
         "def Xform \"R\" (active = false)\n{\n"
         "    def Sphere \"S\" { double radius = 1 }\n"
         "}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def Xform \"P\" (active = true\n"
         "                 prepend references = @./next_p2_react_ref.usda@</R>"
         ")\n{\n}\n";
  }
  Stage stage2;
  warn.clear(); err.clear();
  assert(pcp::ComposeStageFromFile(root, resolver, &stage2, {}, &warn, &err));
  UsdPrim p = stage2.GetPrimAtPath("/P");
  assert(p.IsValid() && p.IsActive() && "stronger active=true wins");
  assert(stage2.GetPrimAtPath("/P/S").IsValid() &&
         "re-activated prim keeps its referenced subtree");
  std::remove(ref.c_str());
  std::remove(root.c_str());
  std::cout << "  OK" << std::endl;
}

// P2 audit: an inherit to a SUB-ROOT class reached through a reference is
// implied at the MAPPED class path in the referencing stack, so a root-layer
// override at that path composes onto the inheriting prim (pxr maps implied
// classes through the arc's map function). Also covers the root-level class
// case for a CHILD prim of the referenced root (the re-rooted source's
// ancestor chain must include the root stack).
static void test_p2_implied_subroot_class() {
  std::cout << "test_p2_implied_subroot_class..." << std::endl;
  const std::string models = "/tmp/next_p2_models.usda";
  const std::string root = "/tmp/next_p2_models_root.usda";
  {
    std::ofstream o(models);
    o << "#usda 1.0\n(\n    defaultPrim = \"Models\"\n)\n"
         "def Scope \"Models\"\n{\n"
         "    class \"_class_X\" { double a = 1 }\n"
         "    def Xform \"M\" (inherits = </Models/_class_X>) { }\n"
         "}\n";
  }
  {
    std::ofstream o(root);
    o << "#usda 1.0\n"
         "def Xform \"World\" (references = @./next_p2_models.usda@)\n{\n"
         "    over \"_class_X\" { double a = 2 }\n"
         "}\n";
  }
  AssetResolver resolver;
  resolver.SetWorkingDirectory("/tmp");
  Stage stage;
  std::string warn, err;
  assert(pcp::ComposeStageFromFile(root, resolver, &stage, {}, &warn, &err));
  UsdPrim m = stage.GetPrimAtPath("/World/M");
  assert(m.IsValid());
  const Value* a = m.GetPropertyValue("a");
  assert(a && a->as_double() && *a->as_double() == 2.0 &&
         "root override on the implied sub-root class path must win");
  std::remove(models.c_str());
  std::remove(root.c_str());

  // Root-level class inherited by a CHILD of the referenced root prim.
  const std::string lib2 = "/tmp/next_p2_models2.usda";
  const std::string root2 = "/tmp/next_p2_models2_root.usda";
  {
    std::ofstream o(lib2);
    o << "#usda 1.0\n(\n    defaultPrim = \"Models\"\n)\n"
         "class \"_class_X\" { double a = 1 }\n"
         "def Scope \"Models\"\n{\n"
         "    def Xform \"M\" (inherits = </_class_X>) { }\n"
         "}\n";
  }
  {
    std::ofstream o(root2);
    o << "#usda 1.0\n"
         "over \"_class_X\" { double a = 2 }\n"
         "def Xform \"World\" (references = @./next_p2_models2.usda@) { }\n";
  }
  Stage stage2;
  warn.clear(); err.clear();
  assert(pcp::ComposeStageFromFile(root2, resolver, &stage2, {}, &warn, &err));
  UsdPrim m2 = stage2.GetPrimAtPath("/World/M");
  assert(m2.IsValid());
  const Value* a2 = m2.GetPropertyValue("a");
  assert(a2 && a2->as_double() && *a2->as_double() == 2.0 &&
         "root override on a root-level implied class must reach a "
         "referenced CHILD prim");
  std::remove(lib2.c_str());
  std::remove(root2.c_str());
  std::cout << "  OK" << std::endl;
}


// `reorder nameChildren` / `reorder rootPrims` apply to the flattened child
// order (pxr semantics: listed names move to the front in list order, the
// rest keep authored order). Locked here because the audit once flagged
// reorder as unimplemented.
static void test_reorder_children() {
  std::cout << "test_reorder_children..." << std::endl;
  auto parse_to_layer = [](const char* usda) -> std::shared_ptr<Layer> {
    std::string w, e;
    auto lp = pcp::LoadLayerFromMemory(
        "mem.usda", reinterpret_cast<const uint8_t*>(usda),
        std::strlen(usda), &w, &e);
    return lp;
  };
  auto rootL = parse_to_layer(
      "#usda 1.0\n"
      "reorder rootPrims = [\"Z\", \"M\"]\n"
      "def Xform \"M\" { }\n"
      "def Xform \"Q\" { }\n"
      "def Xform \"Z\"\n"
      "{\n"
      "    reorder nameChildren = [\"C\", \"A\"]\n"
      "    def Sphere \"A\" { }\n"
      "    def Cube \"B\" { }\n"
      "    def Cone \"C\" { }\n"
      "}\n");
  AssetResolver resolver;
  auto opened = pcp::Cache::Open(resolver, rootL);
  assert(opened);
  pcp::Cache cache = std::move(*opened);
  Stage stage;
  std::string warn, err;
  assert(cache.BuildStage(&stage, &warn, &err));

  std::vector<UsdPrim> roots = stage.GetRootPrims();
  assert(roots.size() == 3);
  assert(roots[0].GetName() == "Z");
  assert(roots[1].GetName() == "M");
  assert(roots[2].GetName() == "Q");
  std::vector<UsdPrim> kids = roots[0].GetChildren();
  assert(kids.size() == 3);
  assert(kids[0].GetName() == "C");
  assert(kids[1].GetName() == "A");
  assert(kids[2].GetName() == "B");
  std::cout << "  OK" << std::endl;
}

int main() {
  test_compute_prim_index();
  test_typed_composition_issues();
  test_prototype_order_independent();
  test_compose_prim_lazy();
  test_child_order_weak_to_strong();
  test_crate_style_variant_holder();
  test_usda_connection_and_custom_rel();
  test_variant_option_payload_arc();
  test_variant_content_payload_anchor();
  test_active_survives_weaker_arc();
  test_out_of_scope_targets_dropped();
  test_deferred_payload_marker_order();
  test_sublayer_offset_parses();
  test_parallel_compose_byte_identical();
  test_value_block_roundtrip();
  test_extracted_prototypes();
  test_extracted_prototypes_collision_and_remap();
  test_connection_namespace_remap();
  test_value_connection_field_compose();
  test_compose_prim_dependency_invalidate();
  test_compose_prim_invalidate_layer_without_index();
  test_eval_attribute_lazy();
  test_specialize_chain();
  test_cross_layer_arc_dedup();
  test_build_stage();
  test_invalidate();
  test_ancestral_compute();
  test_deferred_payload();
  test_load_rules_lifecycle();
  test_set_load_rules();
  test_layer_offset_timesamples();
  test_payload_in_instance();
  test_inherits_specializes();
  test_variants();
  test_variant_inline_property_flags();
  test_variant_option_authored_state();
  test_variant_content_key_stable();
  test_variants_v2();
  test_instancing();
  test_path_translation();
  test_flatten_instances();
  test_relocates();
  test_implied_inherit();
  test_layer_relocates();
  test_relocates_in_referenced_layer_stack();
  test_relocate_to_new_root_prim();
  test_relative_class_arc_target();
  test_specifier_resolution();
  test_expression_sublayer_and_variant_selection();
  test_cross_layer_string_listop_matrix();
  test_layer_stack_identity_expression_vars();
  test_reorder_children();
  test_p2_ref_no_default_prim();
  test_p2_inactive_subtree_pruned();
  test_p2_implied_subroot_class();
  test_instance_proxy();
  test_parallel_prewarm();
  test_cross_source_variant();
  test_variant_selected_field();
  test_implied_intermediate();
  test_cross_layer_listops();
  test_cross_layer_arc_reorder_fixtures();
  test_authored_arc_fidelity_fixture();
  test_listop_edit_does_not_clear_other_arc_fields();
  test_writer_listop_fidelity();
  test_compose_from_file();
  test_nested_relative_reference();
  test_sublayer_authored_reference_anchor();
  test_payload_policy_owner_anchor();
  test_sublayer_stack_composition();
  test_usdz_package_layers();
  test_sublayer_cycle_and_depth();
  test_node_overflow_fails_cleanly();
  test_concurrent_queries();
  test_concurrent_payload_edits();
  test_parallel_build_matches_serial();
  test_ancestor_reference_cycle();
  test_mutual_reference_cycle();
  test_self_payload_cycle();
  test_variant_content_cycle();
  test_deep_hierarchy();
  test_reference_chain_at_max_depth();
  if (g_pcp_check_failures != 0) {
    std::cerr << g_pcp_check_failures << " PCP_CHECK failure(s)." << std::endl;
    return 1;
  }
  std::cout << "All next/pcp tests passed." << std::endl;
  return 0;
}
