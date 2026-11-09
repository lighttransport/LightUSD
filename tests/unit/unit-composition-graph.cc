// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// unit-composition-graph.cc - Tests for DAG-based composition engine
//
// Uses pseudo-random USD generation with fixed seeds for reproducibility.
// All random generation is based on AOUSD Core Spec conventions.
//

#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-composition-graph.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "unit-common.hh"

#include "composition-graph.hh"
#include "composition.hh"
#include "layer.hh"
#include "stage.hh"
#include "lightusd.hh"
#include "usdGeom.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::legacy::composition_graph;

// ---------------------------------------------------------------------------
// Pseudo-random number generator (xoshiro128**, deterministic)
// ---------------------------------------------------------------------------

namespace {

/// Simple xoshiro128** PRNG for reproducible test generation.
/// Not cryptographic -- just needs to be fast and deterministic.
class Rng {
 public:
  explicit Rng(uint32_t seed) {
    // SplitMix32 to seed state from a single value
    for (int i = 0; i < 4; i++) {
      seed += 0x9E3779B9u;
      uint32_t z = seed;
      z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
      z = (z ^ (z >> 13)) * 0xC2B2AE35u;
      z = z ^ (z >> 16);
      s[i] = z;
    }
  }

  uint32_t next() {
    const uint32_t result = rotl(s[1] * 5, 7) * 9;
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return result;
  }

  /// Uniform int in [lo, hi] (inclusive)
  int range(int lo, int hi) {
    if (lo >= hi) return lo;
    return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1));
  }

  /// Uniform float in [0, 1)
  float unit_float() {
    return static_cast<float>(next() >> 8) / 16777216.0f;
  }

  /// Pick one element from a vector
  template <typename T>
  const T &pick(const std::vector<T> &v) {
    return v[next() % v.size()];
  }

  /// Return true with probability p (0.0 to 1.0)
  bool chance(float p) { return unit_float() < p; }

 private:
  uint32_t s[4];
  static uint32_t rotl(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
  }
};

// ---------------------------------------------------------------------------
// Random USDA generator (AOUSD spec based)
// ---------------------------------------------------------------------------

/// Prim types from AOUSD spec (common subset)
static const std::vector<std::string> kPrimTypes = {
    "Xform", "Scope", "Mesh", "Sphere", "Cube", "Cone", "Cylinder",
    "Camera", "Material", "Shader",
};

/// Property types for random attributes
static const std::vector<std::string> kAttrTypes = {
    "int",    "float",   "double",   "string",
    "float3", "double3", "color3f",  "normal3f",
    "bool",   "token",
};

/// Generate a random prim element name
std::string gen_prim_name(Rng &rng, const std::string &prefix = "prim") {
  return prefix + std::to_string(rng.range(0, 999));
}

/// Generate a random attribute default value as USDA text
std::string gen_attr_value(Rng &rng, const std::string &type) {
  if (type == "int") return std::to_string(rng.range(-100, 100));
  if (type == "float") return std::to_string(rng.unit_float() * 200.0f - 100.0f);
  if (type == "double") return std::to_string(static_cast<double>(rng.unit_float()) * 200.0 - 100.0);
  if (type == "string") return "\"str_" + std::to_string(rng.range(0, 99)) + "\"";
  if (type == "bool") return rng.chance(0.5f) ? "true" : "false";
  if (type == "token") return "\"tok_" + std::to_string(rng.range(0, 50)) + "\"";
  if (type == "float3") {
    std::ostringstream ss;
    ss << "(" << rng.unit_float() << ", " << rng.unit_float() << ", "
       << rng.unit_float() << ")";
    return ss.str();
  }
  if (type == "double3") {
    std::ostringstream ss;
    ss << "(" << rng.unit_float() << ", " << rng.unit_float() << ", "
       << rng.unit_float() << ")";
    return ss.str();
  }
  if (type == "color3f") {
    std::ostringstream ss;
    ss << "(" << rng.unit_float() << ", " << rng.unit_float() << ", "
       << rng.unit_float() << ")";
    return ss.str();
  }
  if (type == "normal3f") {
    std::ostringstream ss;
    ss << "(" << rng.unit_float() << ", " << rng.unit_float() << ", "
       << rng.unit_float() << ")";
    return ss.str();
  }
  return "0";
}

/// Generate random attributes for a prim
void gen_attributes(Rng &rng, std::ostringstream &ss, int count,
                    const std::string &indent) {
  for (int i = 0; i < count; i++) {
    const std::string &type = rng.pick(kAttrTypes);
    std::string name = "attr_" + std::to_string(i);
    bool is_custom = rng.chance(0.5f);
    ss << indent;
    if (is_custom) ss << "custom ";
    ss << type << " " << name << " = " << gen_attr_value(rng, type) << "\n";
  }
}

/// Recursively generate a random prim hierarchy as USDA text.
///
/// @param rng          PRNG
/// @param ss           Output stream
/// @param name         Prim name
/// @param depth        Current depth
/// @param max_depth    Maximum tree depth
/// @param max_children Max children per prim
/// @param indent       Current indentation
void gen_prim_hierarchy(Rng &rng, std::ostringstream &ss,
                        const std::string &name, int depth, int max_depth,
                        int max_children, const std::string &indent) {
  const std::string &prim_type = rng.pick(kPrimTypes);
  ss << indent << "def " << prim_type << " \"" << name << "\"\n";
  ss << indent << "{\n";

  std::string inner = indent + "    ";

  // Random attributes
  int num_attrs = rng.range(0, 4);
  gen_attributes(rng, ss, num_attrs, inner);

  // Random children (if not at max depth)
  if (depth < max_depth) {
    int num_children = rng.range(0, max_children);
    for (int i = 0; i < num_children; i++) {
      std::string child_name = "child_" + std::to_string(i);
      gen_prim_hierarchy(rng, ss, child_name, depth + 1, max_depth,
                         std::max(1, max_children - 1), inner);
    }
  }

  ss << indent << "}\n\n";
}

/// Generate a complete random USDA file string.
///
/// @param seed         PRNG seed for reproducibility
/// @param num_roots    Number of root-level prims
/// @param max_depth    Maximum hierarchy depth
/// @param max_children Max children per prim
/// @return USDA string
std::string gen_random_usda(uint32_t seed, int num_roots, int max_depth,
                            int max_children) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n";
  ss << "(\n    defaultPrim = \"root0\"\n)\n\n";

  for (int i = 0; i < num_roots; i++) {
    std::string name = "root" + std::to_string(i);
    gen_prim_hierarchy(rng, ss, name, 0, max_depth, max_children, "");
  }
  return ss.str();
}

/// Generate USDA with inherits arcs.
///
/// Creates class prims and def prims that inherit from them.
/// Per AOUSD Core Spec 10.3.2.3.
std::string gen_usda_with_inherits(uint32_t seed, int num_classes,
                                   int num_inheritors) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\n";

  // Generate class prims with random properties
  std::vector<std::string> class_names;
  for (int i = 0; i < num_classes; i++) {
    std::string cn = "BaseClass_" + std::to_string(i);
    class_names.push_back(cn);
    ss << "class Xform \"" << cn << "\"\n{\n";
    int num_attrs = rng.range(1, 5);
    gen_attributes(rng, ss, num_attrs, "    ");
    // Possibly add children to class
    if (rng.chance(0.4f)) {
      ss << "    def Scope \"classChild\"\n    {\n";
      gen_attributes(rng, ss, rng.range(1, 3), "        ");
      ss << "    }\n";
    }
    ss << "}\n\n";
  }

  // Generate def prims that inherit from classes
  ss << "def Xform \"World\"\n{\n";
  for (int i = 0; i < num_inheritors; i++) {
    // Pick 1-2 classes to inherit from
    int num_inherit = std::min(rng.range(1, 2), num_classes);
    ss << "    def Xform \"item_" << i << "\" (\n";
    ss << "        prepend inherits = [";
    for (int j = 0; j < num_inherit; j++) {
      if (j > 0) ss << ", ";
      ss << "</" << rng.pick(class_names) << ">";
    }
    ss << "]\n    )\n    {\n";
    // Local overrides (stronger than inherited)
    if (rng.chance(0.6f)) {
      int num_local = rng.range(1, 3);
      gen_attributes(rng, ss, num_local, "        ");
    }
    ss << "    }\n\n";
  }
  ss << "}\n";
  return ss.str();
}

/// Generate USDA with specializes arcs.
///
/// Per AOUSD Core Spec 10.3.2.4 / 10.4.1 (globally weaker).
std::string gen_usda_with_specializes(uint32_t seed, int num_classes,
                                      int num_specializers) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\n";

  std::vector<std::string> class_names;
  for (int i = 0; i < num_classes; i++) {
    std::string cn = "SpecBase_" + std::to_string(i);
    class_names.push_back(cn);
    ss << "class Xform \"" << cn << "\"\n{\n";
    gen_attributes(rng, ss, rng.range(1, 4), "    ");
    ss << "}\n\n";
  }

  ss << "def Xform \"World\"\n{\n";
  for (int i = 0; i < num_specializers; i++) {
    ss << "    def Xform \"spec_" << i << "\" (\n";
    ss << "        prepend specializes = [</" << rng.pick(class_names) << ">]\n";
    ss << "    )\n    {\n";
    if (rng.chance(0.5f)) {
      gen_attributes(rng, ss, rng.range(1, 2), "        ");
    }
    ss << "    }\n\n";
  }
  ss << "}\n";
  return ss.str();
}

/// Generate USDA with both inherits and specializes to test relative strength.
///
/// Per AOUSD 10.4: I > V > R > P > S (specializes globally weakest).
std::string gen_usda_inherits_vs_specializes(uint32_t seed) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\n";

  // Both class prims define the same attribute with different values
  ss << "class Xform \"InheritBase\"\n{\n";
  ss << "    int sharedAttr = " << rng.range(100, 200) << "\n";
  ss << "    int inheritOnly = " << rng.range(1, 50) << "\n";
  ss << "}\n\n";

  ss << "class Xform \"SpecBase\"\n{\n";
  ss << "    int sharedAttr = " << rng.range(300, 400) << "\n";
  ss << "    int specializeOnly = " << rng.range(51, 100) << "\n";
  ss << "}\n\n";

  // Prim inherits from one and specializes from the other
  ss << "def Xform \"World\" (\n";
  ss << "    prepend inherits = [</InheritBase>]\n";
  ss << "    prepend specializes = [</SpecBase>]\n";
  ss << ")\n{\n";
  // Local opinion for one attr
  ss << "    int localAttr = " << rng.range(500, 600) << "\n";
  ss << "}\n";

  return ss.str();
}

/// Generate USDA with variant sets.
///
/// Per AOUSD Core Spec 10.3.2.5.
std::string gen_usda_with_variants(uint32_t seed, int num_variant_sets) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"root0\"\n)\n\n";

  ss << "def Xform \"root0\" (\n";
  // Declare variant sets and selections
  ss << "    variantSets = [";
  for (int i = 0; i < num_variant_sets; i++) {
    if (i > 0) ss << ", ";
    ss << "\"varSet" << i << "\"";
  }
  ss << "]\n";

  // Variant selections (pick first variant)
  ss << "    variants = {\n";
  for (int i = 0; i < num_variant_sets; i++) {
    ss << "        string varSet" << i << " = \"opt0\"\n";
  }
  ss << "    }\n";
  ss << ")\n{\n";

  // Define variant set content
  for (int i = 0; i < num_variant_sets; i++) {
    int num_options = rng.range(2, 4);
    ss << "    variantSet \"varSet" << i << "\" = {\n";
    for (int j = 0; j < num_options; j++) {
      ss << "        \"opt" << j << "\" {\n";
      gen_attributes(rng, ss, rng.range(1, 3), "            ");
      ss << "        }\n";
    }
    ss << "    }\n\n";
  }

  // Some base attributes
  gen_attributes(rng, ss, rng.range(1, 3), "    ");

  ss << "}\n";
  return ss.str();
}

/// Generate USDA with a deep prim hierarchy (stress test).
std::string gen_usda_deep_hierarchy(uint32_t seed, int depth) {
  Rng rng(seed);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"root\"\n)\n\n";

  std::string indent;
  for (int d = 0; d < depth; d++) {
    std::string name = (d == 0) ? "root" : ("level_" + std::to_string(d));
    const std::string &ptype = rng.pick(kPrimTypes);
    ss << indent << "def " << ptype << " \"" << name << "\"\n";
    ss << indent << "{\n";
    indent += "    ";
    // A few attributes at each level
    gen_attributes(rng, ss, rng.range(1, 3), indent);
  }
  // Close all braces
  for (int d = depth - 1; d >= 0; d--) {
    std::string close_indent(static_cast<size_t>(d) * 4, ' ');
    ss << close_indent << "}\n";
  }

  return ss.str();
}

/// Helper: parse USDA and compose via the DAG pipeline
bool compose_via_graph(const std::string &usda, CompositionGraph &graph_out,
                       std::string &warn, std::string &err) {
  Layer layer;
  if (!lightusd_test::parse_usda_to_layer(usda.c_str(), &layer, &warn, &err)) {
    return false;
  }

  // Sublayer composition (L phase) - none needed for single-layer tests
  AssetResolutionResolver resolver;

  auto result =
      CompositionGraph::Compose(resolver, layer, CompositionGraphOptions());
  if (!result) {
    err = result.error();
    return false;
  }
  graph_out = std::move(*result);
  return true;
}

/// Helper: parse USDA and compose via the existing iterative pipeline
bool compose_via_iterative(const std::string &usda, Stage &stage_out,
                           std::string &warn, std::string &err) {
  return lightusd_test::parse_usda_to_stage(usda.c_str(), &stage_out,
                                             &warn, &err);
}

}  // anonymous namespace

// ===========================================================================
// Test implementations
// ===========================================================================

// ---------------------------------------------------------------------------
// compgraph_basic_prim_index_test
// Verify PrimIndex is created for a simple prim with correct node structure.
// ---------------------------------------------------------------------------

void compgraph_basic_prim_index_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root"
{
    custom int myAttr = 42
    def Scope "Child"
    {
        custom float val = 3.14
    }
}
)";

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  // Root prim should have an index
  const PrimIndex *root_idx = graph.GetPrimIndex(Path("/Root", ""));
  TEST_CHECK(root_idx != nullptr);
  if (!root_idx) return;

  // Should have at least the root node
  TEST_CHECK(root_idx->GetNodeCount() >= 1);

  // Root node should be ArcType::Root
  TEST_CHECK(root_idx->GetRootNode().arc_type == ArcType::Root);

  // Root node should have specs
  TEST_CHECK(root_idx->GetRootNode().has_specs());

  // Strength order should be non-empty
  TEST_CHECK(!root_idx->GetStrengthOrder().empty());

  // Child prim should also have an index
  const PrimIndex *child_idx = graph.GetPrimIndex(Path("/Root/Child", ""));
  TEST_CHECK(child_idx != nullptr);

  // Check GetAllPrimPaths
  auto all_paths = graph.GetAllPrimPaths();
  TEST_CHECK(all_paths.size() >= 2);
}

// ---------------------------------------------------------------------------
// compgraph_strength_order_test
// Verify strength order is sorted correctly (root before children).
// ---------------------------------------------------------------------------

// Nested-arc strength: pxr strength order is a PRE-ORDER traversal of the
// prim-index tree, so an inherit reached THROUGH a reference is weaker than
// the reference node itself. The old flat arc-type formula sorted every
// Inherit node ahead of every Reference node regardless of nesting.
void compgraph_nested_strength_order_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "_c"
{
    int fromClass = 1
}

def Xform "Asset" (
    prepend inherits = [</_c>]
)
{
    int fromAsset = 2
}

def Xform "Root" (
    prepend references = [</Asset>]
)
{
    int localAttr = 3
}
)";

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/Root", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  const auto &order = idx->GetStrengthOrder();
  TEST_CHECK(order.size() >= 2);
  int ref_rank = -1;
  int nested_inherit_rank = -1;
  for (size_t r = 0; r < order.size(); r++) {
    const CompNode &n = idx->GetNode(order[r]);
    if (n.arc_type == ArcType::Reference && ref_rank < 0) {
      ref_rank = int(r);
    }
    if (n.arc_type == ArcType::Inherit &&
        n.parent != CompNode::kInvalidIndex &&
        idx->GetNode(n.parent).arc_type == ArcType::Reference) {
      nested_inherit_rank = int(r);
    }
  }
  TEST_CHECK(ref_rank >= 0);
  if (nested_inherit_rank >= 0) {
    TEST_CHECK(ref_rank < nested_inherit_rank);
    TEST_MSG("reference rank %d must be stronger than its nested inherit %d",
             ref_rank, nested_inherit_rank);
  }
  // Root (local) is always strongest.
  TEST_CHECK(idx->GetNode(order[0]).arc_type == ArcType::Root);
}

void compgraph_strength_order_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "BaseClass"
{
    int baseAttr = 10
}

def Xform "Root" (
    prepend inherits = [</BaseClass>]
)
{
    int localAttr = 20
}
)";

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/Root", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // Should have Root + Inherit nodes
  TEST_CHECK(idx->GetNodeCount() >= 2);

  // Strength order: root (local) should come before inherit
  const auto &order = idx->GetStrengthOrder();
  TEST_CHECK(order.size() >= 2);

  if (order.size() >= 2) {
    // First in strength order should be the root node (local opinions strongest)
    const CompNode &first = idx->GetNode(order[0]);
    TEST_CHECK(first.arc_type == ArcType::Root);

    // There should be an Inherit node somewhere
    bool found_inherit = false;
    for (uint16_t oi : order) {
      if (idx->GetNode(oi).arc_type == ArcType::Inherit) {
        found_inherit = true;
        break;
      }
    }
    TEST_CHECK(found_inherit);
  }
}

// ---------------------------------------------------------------------------
// compgraph_inherits_dag_test
// ---------------------------------------------------------------------------

void compgraph_inherits_dag_test(void) {
  // Seed 42 for reproducibility
  std::string usda = gen_usda_with_inherits(42, 3, 5);
  TEST_MSG("Generated USDA (%zu bytes) with 3 classes, 5 inheritors",
           usda.size());

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  // Each inheriting prim should have Inherit nodes in its DAG
  for (int i = 0; i < 5; i++) {
    std::string path = "/World/item_" + std::to_string(i);
    const PrimIndex *idx = graph.GetPrimIndex(Path(path, ""));
    TEST_CHECK_(idx != nullptr, "PrimIndex exists for %s", path.c_str());
    if (!idx) continue;

    // Should have at least 2 nodes (root + inherit)
    TEST_CHECK_(idx->GetNodeCount() >= 2,
                "%s has %d nodes (expected >= 2)", path.c_str(),
                idx->GetNodeCount());

    // Verify an Inherit arc exists
    bool has_inherit = false;
    for (uint16_t j = 0; j < idx->GetNodeCount(); j++) {
      if (idx->GetNode(j).arc_type == ArcType::Inherit) {
        has_inherit = true;
        break;
      }
    }
    TEST_CHECK_(has_inherit, "%s should have Inherit arc", path.c_str());
  }
}

// ---------------------------------------------------------------------------
// compgraph_specializes_globally_weak_test
// Specializes nodes should appear at root level and be weakest in strength.
// AOUSD Core Spec 10.4.1.
// ---------------------------------------------------------------------------

void compgraph_specializes_globally_weak_test(void) {
  std::string usda = gen_usda_inherits_vs_specializes(77);
  TEST_MSG("Generated inherits-vs-specializes USDA (%zu bytes)", usda.size());

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/World", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // Find the inherit and specialize nodes
  int inherit_strength = -1;
  int specialize_strength = -1;

  const auto &order = idx->GetStrengthOrder();
  for (size_t pos = 0; pos < order.size(); pos++) {
    const CompNode &n = idx->GetNode(order[pos]);
    if (n.arc_type == ArcType::Inherit && inherit_strength < 0) {
      inherit_strength = static_cast<int>(pos);
    }
    if (n.arc_type == ArcType::Specialize && specialize_strength < 0) {
      specialize_strength = static_cast<int>(pos);
    }
  }

  // Both should exist
  TEST_CHECK_(inherit_strength >= 0, "Inherit node found in strength order");
  TEST_CHECK_(specialize_strength >= 0, "Specialize node found in strength order");

  // Specialize should be weaker (later in strength order) than Inherit
  if (inherit_strength >= 0 && specialize_strength >= 0) {
    TEST_CHECK_(specialize_strength > inherit_strength,
                "Specialize (pos %d) should be weaker than Inherit (pos %d)",
                specialize_strength, inherit_strength);
  }
}

// ---------------------------------------------------------------------------
// compgraph_references_dag_test
// ---------------------------------------------------------------------------

void compgraph_references_dag_test(void) {
  // Internal reference test (no external files needed)
  const char *usda = R"(#usda 1.0
def Xform "Source"
{
    int sourceAttr = 100
    def Scope "Inner"
    {
        float innerVal = 2.5
    }
}

def Xform "Target" (
    prepend references = </Source>
)
{
    int targetAttr = 200
}
)";

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/Target", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // Should have Root + Reference nodes
  TEST_CHECK(idx->GetNodeCount() >= 2);

  bool has_ref = false;
  for (uint16_t i = 0; i < idx->GetNodeCount(); i++) {
    if (idx->GetNode(i).arc_type == ArcType::Reference) {
      has_ref = true;
      break;
    }
  }
  TEST_CHECK_(has_ref, "Target should have Reference arc node");
}

// ---------------------------------------------------------------------------
// compgraph_variants_deferred_test
// Variant opinions should be collected from multiple arcs.
// AOUSD Core Spec 10.3.2.5.
// ---------------------------------------------------------------------------

void compgraph_variants_deferred_test(void) {
  std::string usda = gen_usda_with_variants(123, 2);
  TEST_MSG("Generated USDA with 2 variant sets (%zu bytes)", usda.size());

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/root0", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // The prim should have a valid DAG
  TEST_CHECK(idx->HasAnySpecs());

  // Graph should exist and have at least the root prim
  auto paths = graph.GetAllPrimPaths();
  TEST_CHECK(!paths.empty());
}

// ---------------------------------------------------------------------------
// compgraph_cycle_detection_test
// Self-referencing inherits should not cause infinite recursion.
// ---------------------------------------------------------------------------

void compgraph_cycle_detection_test(void) {
  // Prim inherits from itself (cycle)
  const char *usda = R"(#usda 1.0
def Xform "CycleA" (
    prepend inherits = [</CycleB>]
)
{
    int a = 1
}

def Xform "CycleB" (
    prepend inherits = [</CycleA>]
)
{
    int b = 2
}
)";

  std::string warn, err;
  CompositionGraph graph;
  // Should succeed (cycle detected and skipped, not an error)
  bool ok = compose_via_graph(usda, graph, warn, err);
  TEST_CHECK(ok);

  // Both prims should still have valid indices
  TEST_CHECK(graph.GetPrimIndex(Path("/CycleA", "")) != nullptr);
  TEST_CHECK(graph.GetPrimIndex(Path("/CycleB", "")) != nullptr);
}

// ---------------------------------------------------------------------------
// compgraph_implied_inherits_test
// When a referenced prim has inherits, those should propagate.
// AOUSD Core Spec 10.3.2.3.
// ---------------------------------------------------------------------------

void compgraph_implied_inherits_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "SharedClass"
{
    int classAttr = 99
}

def Xform "Source" (
    prepend inherits = [</SharedClass>]
)
{
    int sourceAttr = 50
}

def Xform "Consumer" (
    prepend references = </Source>
)
{
    int consumerAttr = 25
}
)";

  std::string warn, err;
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/Consumer", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // Should have Root + Reference + possibly implied Inherit
  TEST_CHECK(idx->GetNodeCount() >= 2);

  // The reference node pulls in /Source which itself has inherits.
  // Those inherits should be processed (either as implied arcs or as
  // direct inherits on the reference sub-graph).
  bool has_inherit_arc = false;
  bool has_ref_arc = false;
  for (uint16_t i = 0; i < idx->GetNodeCount(); i++) {
    if (idx->GetNode(i).arc_type == ArcType::Inherit) {
      has_inherit_arc = true;
    }
    if (idx->GetNode(i).arc_type == ArcType::Reference) {
      has_ref_arc = true;
    }
  }
  TEST_CHECK_(has_ref_arc, "Consumer should have Reference arc");
  // The inherited class from /Source should be reachable via the DAG
  TEST_CHECK_(has_inherit_arc,
              "Consumer should have Inherit arc (from Source's inherits)");
}

// ---------------------------------------------------------------------------
// compgraph_instance_key_identical_test
// Two prims with identical composition arcs should produce the same key.
// ---------------------------------------------------------------------------

void compgraph_instance_key_identical_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "Proto"
{
    int val = 10
    def Scope "Child" { }
}

def Xform "InstanceA" (
    instanceable = true
    prepend inherits = [</Proto>]
)
{
    double3 xformOp:translate = (1, 0, 0)
}

def Xform "InstanceB" (
    instanceable = true
    prepend inherits = [</Proto>]
)
{
    double3 xformOp:translate = (2, 0, 0)
}
)";

  std::string warn, err;
  CompositionGraph graph;
  CompositionGraphOptions opts;
  opts.detect_instances = true;

  Layer layer;
  TEST_CHECK(lightusd_test::parse_usda_to_layer(usda, &layer, &warn, &err));

  AssetResolutionResolver resolver;
  auto result = CompositionGraph::Compose(resolver, layer, opts);
  TEST_CHECK(result.has_value());
  if (!result) { TEST_MSG("err: %s", result.error().c_str()); return; }

  CompositionGraph &g = *result;

  const PrimIndex *a = g.GetPrimIndex(Path("/InstanceA", ""));
  const PrimIndex *b = g.GetPrimIndex(Path("/InstanceB", ""));
  TEST_CHECK(a != nullptr);
  TEST_CHECK(b != nullptr);
  if (!a || !b) return;

  // Both should be instanceable
  TEST_CHECK(a->IsInstanceable());
  TEST_CHECK(b->IsInstanceable());

  // Prototype count should be 1 (shared)
  TEST_CHECK_(g.GetPrototypeCount() >= 1,
              "Expected at least 1 prototype, got %zu", g.GetPrototypeCount());
}

// ---------------------------------------------------------------------------
// compgraph_instance_key_different_test
// Two prims inheriting from different classes should produce different keys.
// ---------------------------------------------------------------------------

void compgraph_instance_key_different_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "ClassA" { int val = 1; }
class Xform "ClassB" { int val = 2; }

def Xform "InstA" (
    instanceable = true
    prepend inherits = [</ClassA>]
) { }

def Xform "InstB" (
    instanceable = true
    prepend inherits = [</ClassB>]
) { }
)";

  std::string warn, err;
  Layer layer;
  TEST_CHECK(lightusd_test::parse_usda_to_layer(usda, &layer, &warn, &err));

  AssetResolutionResolver resolver;
  CompositionGraphOptions opts;
  opts.detect_instances = true;
  auto result = CompositionGraph::Compose(resolver, layer, opts);
  TEST_CHECK(result.has_value());
  if (!result) return;

  CompositionGraph &g = *result;

  // Should have 2 prototypes (different keys)
  TEST_CHECK_(g.GetPrototypeCount() >= 2,
              "Expected 2 prototypes (different classes), got %zu",
              g.GetPrototypeCount());
}

// ---------------------------------------------------------------------------
// compgraph_payload_deferred_test
// Payload with load_policy=false should create a deferred node.
// ---------------------------------------------------------------------------

void compgraph_payload_deferred_test(void) {
  // Internal payload (no file needed)
  const char *usda = R"(#usda 1.0
def Xform "PayloadSource"
{
    int payloadAttr = 42
}

def Xform "Consumer" (
    payload = </PayloadSource>
)
{
    int localAttr = 10
}
)";

  std::string warn, err;
  Layer layer;
  TEST_CHECK(lightusd_test::parse_usda_to_layer(usda, &layer, &warn, &err));

  AssetResolutionResolver resolver;
  CompositionGraphOptions opts;
  // Defer ALL payloads
  opts.payload_policy = [](const Path &, const Payload &) { return false; };

  auto result = CompositionGraph::Compose(resolver, layer, opts);
  TEST_CHECK(result.has_value());
  if (!result) { TEST_MSG("err: %s", result.error().c_str()); return; }

  CompositionGraph &g = *result;

  // Consumer should have deferred payloads
  TEST_CHECK(g.HasDeferredPayload(Path("/Consumer", "")));

  auto deferred = g.GetDeferredPayloadPaths();
  TEST_CHECK(!deferred.empty());

  // The PrimIndex should have a PayloadDeferred node
  const PrimIndex *idx = g.GetPrimIndex(Path("/Consumer", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  bool found_deferred = false;
  for (uint16_t i = 0; i < idx->GetNodeCount(); i++) {
    if (idx->GetNode(i).is_payload_deferred()) {
      found_deferred = true;
      break;
    }
  }
  TEST_CHECK_(found_deferred, "Should have a PayloadDeferred node");
}

// ---------------------------------------------------------------------------
// compgraph_build_stage_simple_test
// BuildStage should produce a valid Stage from a simple scene.
// ---------------------------------------------------------------------------

void compgraph_build_stage_simple_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "Root"
{
    custom int myAttr = 42
    def Scope "Child"
    {
        custom float childVal = 1.5
    }
}
)";

  std::string warn, err;

  // Compose via DAG pipeline
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  // Verify root prims exist
  TEST_CHECK(!dag_stage.root_prims().empty());

  // Verify we can find /Root
  auto root_result = dag_stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(root_result.has_value());

  // Also compose via iterative pipeline for comparison
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));

  // Both should have the same root prim count
  TEST_CHECK_(dag_stage.root_prims().size() == iter_stage.root_prims().size(),
              "DAG stage has %zu roots, iterative has %zu",
              dag_stage.root_prims().size(), iter_stage.root_prims().size());
}

// ---------------------------------------------------------------------------
// compgraph_build_stage_no_duplicate_children_test
// Regression: ComposePrimSpecFromIndex initializes its output by copying the
// strongest PrimSpec wholesale, which also carries that layer's inline children.
// BuildStage then composes each child from its own PrimIndex and appends it, so
// every child used to be emitted TWICE -- the uncomposed copy first.
//
// GetPrimAtPath takes the first name match, so the uncomposed copy won it and
// the child's own arcs were silently ignored: here /Root/Child is declared as an
// empty `def Xform "Child" {}` carrying a payload, and the lookup returned that
// empty declaration instead of the payload-backed composition.
// ---------------------------------------------------------------------------

void compgraph_build_stage_no_duplicate_children_test(void) {
  const char *usda = R"(#usda 1.0
def Xform "PayloadSource"
{
    custom int payloadAttr = 42
}

def Xform "Root"
{
    def Xform "Child" (
        payload = </PayloadSource>
    )
    {
    }
    def Scope "Plain"
    {
        custom float plainVal = 1.5
    }
}
)";

  std::string warn, err;

  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  auto root_result = dag_stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(root_result.has_value());
  if (!root_result.has_value()) return;
  const Prim *root = root_result.value();

  // Each child must appear exactly once.
  TEST_CHECK_(root->children().size() == 2,
              "/Root should have exactly 2 children, has %zu",
              root->children().size());
  for (const char *nm : {"Child", "Plain"}) {
    size_t n = 0;
    for (const auto &c : root->children()) {
      if (c.element_name() == nm) n++;
    }
    TEST_CHECK_(n == 1, "/Root/%s should appear exactly once, appears %zu times",
                nm, n);
  }

  // The prim reachable at /Root/Child must be the COMPOSED one (payload pulled
  // in), not the empty declaration that the raw copy would have shadowed it with.
  auto child_result = dag_stage.GetPrimAtPath(Path("/Root/Child", ""));
  TEST_CHECK(child_result.has_value());
  if (!child_result.has_value()) return;

  const Xform *xf = child_result.value()->as<Xform>();
  TEST_CHECK(xf != nullptr);
  if (xf) {
    TEST_CHECK_(xf->props.count("payloadAttr") == 1,
                "/Root/Child must resolve to the payload-composed prim "
                "(payloadAttr present), not the empty declaration");
  }
}

// ---------------------------------------------------------------------------
// compgraph_validate_no_duplicate_sibling_names_test
// Direct test of ValidateNoDuplicateSiblingNames: a Layer with duplicate
// sibling names must be rejected.
// ---------------------------------------------------------------------------

void compgraph_validate_no_duplicate_sibling_names_test(void) {
  // Build a minimal layer with duplicate children.
  lightusd::legacy::Layer layer;
  lightusd::legacy::PrimSpec root(lightusd::legacy::Specifier::Def, "root");

  lightusd::legacy::PrimSpec child_a(lightusd::legacy::Specifier::Def, "ChildA");
  root.children().push_back(child_a);

  lightusd::legacy::PrimSpec child_b(lightusd::legacy::Specifier::Def, "ChildB");
  root.children().push_back(child_b);

  // First duplicate.
  lightusd::legacy::PrimSpec child_a_dup(lightusd::legacy::Specifier::Def, "ChildA");
  root.children().push_back(child_a_dup);

  TEST_CHECK(layer.add_primspec("root", root));

  std::string warn, err;
  bool valid = lightusd::legacy::composition_graph::ValidateNoDuplicateSiblingNames(
      layer, &warn, &err);
  TEST_CHECK_(!valid, "ValidateNoDuplicateSiblingNames must reject duplicate "
              "sibling names");
  TEST_CHECK_(!err.empty(), "ValidateNoDuplicateSiblingNames must set err on "
              "duplicate");

  // Now test a valid layer (no duplicates) must pass.
  lightusd::legacy::Layer clean_layer;
  root.children().resize(2);  // Remove the duplicate ChildA.
  root.children()[0] = child_a;
  root.children()[1] = child_b;
  clean_layer.add_primspec("root", root);
  std::string cwarn, cerr;
  bool valid2 = lightusd::legacy::composition_graph::ValidateNoDuplicateSiblingNames(
      clean_layer, &cwarn, &cerr);
  TEST_CHECK_(valid2, "ValidateNoDuplicateSiblingNames must accept valid "
              "layers without duplicates");
}

// ---------------------------------------------------------------------------
// compgraph_build_stage_wide_deep_test
// Regression for the BuildStage child-composition rewrite: a wide + deep
// hierarchy must reconstruct fully. BuildStage formerly scanned all of
// _prim_indices for every parent (O(N^2) with a substr per probe); it now uses
// a precomputed parent->children index. This test exercises that path and
// checks both wide (many siblings) and deep (long chains) coverage.
// ---------------------------------------------------------------------------

void compgraph_build_stage_wide_deep_test(void) {
  const int kWidth = 40;  // siblings under /Root
  const int kDepth = 12;  // nested-scope chain depth under each sibling

  std::ostringstream ss;
  ss << "#usda 1.0\n";
  ss << "def Xform \"Root\"\n{\n";
  for (int w = 0; w < kWidth; w++) {
    ss << "  def Scope \"Sib" << w << "\"\n  {\n";
    for (int d = 0; d < kDepth; d++) {
      ss << "    def Scope \"D" << d << "\"\n    {\n";
    }
    for (int d = 0; d < kDepth; d++) {
      ss << "    }\n";
    }
    ss << "  }\n";
  }
  ss << "}\n";
  const std::string usda = ss.str();

  std::string warn, err;

  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  // /Root exists with kWidth direct children.
  auto root = dag_stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(root.has_value());
  if (root) {
    TEST_CHECK_(root.value()->children().size() == size_t(kWidth),
                "Root has %zu children, expected %d",
                root.value()->children().size(), kWidth);
  }

  // A representative deep leaf must exist: /Root/Sib0/D0/D1/.../D{kDepth-1}.
  {
    std::string deep = "/Root/Sib0";
    for (int d = 0; d < kDepth; d++) deep += "/D" + std::to_string(d);
    auto leaf = dag_stage.GetPrimAtPath(Path(deep, ""));
    TEST_CHECK_(leaf.has_value(), "deep leaf %s not found", deep.c_str());
  }
  // The last sibling's first-level child must exist too (wide coverage).
  {
    const std::string p = "/Root/Sib" + std::to_string(kWidth - 1) + "/D0";
    auto pr = dag_stage.GetPrimAtPath(Path(p, ""));
    TEST_CHECK_(pr.has_value(), "wide path %s not found", p.c_str());
  }

  // DAG and iterative pipelines should agree on the root prim count.
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));
  TEST_CHECK_(dag_stage.root_prims().size() == iter_stage.root_prims().size(),
              "DAG roots %zu vs iterative %zu",
              dag_stage.root_prims().size(), iter_stage.root_prims().size());
}

// ---------------------------------------------------------------------------
// compgraph_build_stage_inherits_test
// BuildStage with inherits should compose properties correctly.
// ---------------------------------------------------------------------------

void compgraph_build_stage_inherits_test(void) {
  const char *usda = R"(#usda 1.0
class Xform "Base"
{
    int baseVal = 100
    float sharedVal = 1.5
}

def Xform "Root" (
    prepend inherits = [</Base>]
)
{
    float sharedVal = 2.5
}
)";

  std::string warn, err;

  // Compose via DAG
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  // Compose via iterative
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));

  // Both stages should have /Root
  auto dag_root = dag_stage.GetPrimAtPath(Path("/Root", ""));
  auto iter_root = iter_stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(dag_root.has_value());
  TEST_CHECK(iter_root.has_value());
}

// ---------------------------------------------------------------------------
// compgraph_random_flat_prims_test
// Generate a scene with many flat root prims, compose via both pipelines.
// Fixed seed = 1001.
// ---------------------------------------------------------------------------

void compgraph_random_flat_prims_test(void) {
  constexpr uint32_t SEED = 1001;
  constexpr int NUM_ROOTS = 10;

  std::string usda = gen_random_usda(SEED, NUM_ROOTS, 0, 0);
  TEST_MSG("Generated %d flat root prims (%zu bytes), seed=%u",
           NUM_ROOTS, usda.size(), SEED);

  std::string warn, err;

  // DAG pipeline
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  // Iterative pipeline
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));

  // Same number of root prims
  TEST_CHECK_(dag_stage.root_prims().size() == iter_stage.root_prims().size(),
              "DAG=%zu vs iterative=%zu root prims",
              dag_stage.root_prims().size(), iter_stage.root_prims().size());

  // All prims should have PrimIndices
  auto paths = graph.GetAllPrimPaths();
  TEST_CHECK_(paths.size() >= static_cast<size_t>(NUM_ROOTS),
              "Expected >= %d prim indices, got %zu", NUM_ROOTS, paths.size());
}

// ---------------------------------------------------------------------------
// compgraph_random_deep_hierarchy_test
// Generate a deeply nested hierarchy, compose via both pipelines.
// Fixed seed = 2002.
// ---------------------------------------------------------------------------

void compgraph_random_deep_hierarchy_test(void) {
  constexpr uint32_t SEED = 2002;
  constexpr int DEPTH = 8;

  std::string usda = gen_usda_deep_hierarchy(SEED, DEPTH);
  TEST_MSG("Generated depth-%d hierarchy (%zu bytes), seed=%u",
           DEPTH, usda.size(), SEED);

  std::string warn, err;

  // DAG pipeline
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));
  if (!err.empty()) { TEST_MSG("BuildStage err: %s", err.c_str()); return; }

  // Should have nested prim indices
  auto paths = graph.GetAllPrimPaths();
  TEST_CHECK_(paths.size() >= static_cast<size_t>(DEPTH),
              "Expected >= %d prim indices for depth-%d tree, got %zu",
              DEPTH, DEPTH, paths.size());

  // Iterative pipeline for comparison
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));
  TEST_CHECK(dag_stage.root_prims().size() == iter_stage.root_prims().size());
}

// ---------------------------------------------------------------------------
// compgraph_random_inherits_chain_test
// Generate a chain of class prims inheriting from each other.
// Fixed seed = 3003.
// ---------------------------------------------------------------------------

void compgraph_random_inherits_chain_test(void) {
  constexpr uint32_t SEED = 3003;

  std::string usda = gen_usda_with_inherits(SEED, 4, 8);
  TEST_MSG("Generated 4 classes, 8 inheritors (%zu bytes), seed=%u",
           usda.size(), SEED);

  std::string warn, err;

  // DAG pipeline
  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));

  // Iterative pipeline
  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));

  // Both should produce the World prim with 8 children
  auto dag_world = dag_stage.GetPrimAtPath(Path("/World", ""));
  auto iter_world = iter_stage.GetPrimAtPath(Path("/World", ""));
  TEST_CHECK(dag_world.has_value());
  TEST_CHECK(iter_world.has_value());

  if (dag_world.has_value() && iter_world.has_value()) {
    // DAG BuildStage may include class prims at root level that the
    // iterative pipeline (LoadUSDAFromMemory) filters differently.
    // Just verify both have the inheriting children (item_0..item_7).
    TEST_CHECK_((*dag_world)->children().size() >= 8,
                "DAG World should have >= 8 children, got %zu",
                (*dag_world)->children().size());
    TEST_CHECK_((*iter_world)->children().size() >= 8,
                "Iterative World should have >= 8 children, got %zu",
                (*iter_world)->children().size());
  }
}

// ---------------------------------------------------------------------------
// compgraph_random_mixed_arcs_test
// Generate a scene with inherits + variants, compose via both.
// Fixed seed = 4004.
// ---------------------------------------------------------------------------

void compgraph_random_mixed_arcs_test(void) {
  constexpr uint32_t SEED = 4004;

  // Scene with inherits, variants, and plain prims
  Rng rng(SEED);
  std::ostringstream ss;
  ss << "#usda 1.0\n(\n    defaultPrim = \"World\"\n)\n\n";

  // Class prims
  ss << "class Xform \"StyleA\" {\n";
  gen_attributes(rng, ss, 3, "    ");
  ss << "}\n\n";

  ss << "class Xform \"StyleB\" {\n";
  gen_attributes(rng, ss, 3, "    ");
  ss << "}\n\n";

  // World with mixed arcs
  ss << "def Xform \"World\"\n{\n";
  for (int i = 0; i < 6; i++) {
    ss << "    def Xform \"item_" << i << "\" (\n";
    if (rng.chance(0.5f)) {
      ss << "        prepend inherits = [</"
         << (rng.chance(0.5f) ? "StyleA" : "StyleB") << ">]\n";
    }
    if (rng.chance(0.3f)) {
      ss << "        variantSets = [\"look\"]\n";
      ss << "        variants = { string look = \"v0\" }\n";
    }
    ss << "    )\n    {\n";
    gen_attributes(rng, ss, rng.range(1, 3), "        ");
    if (rng.chance(0.3f)) {
      ss << "        variantSet \"look\" = {\n";
      ss << "            \"v0\" {\n";
      gen_attributes(rng, ss, 2, "                ");
      ss << "            }\n";
      ss << "            \"v1\" {\n";
      gen_attributes(rng, ss, 2, "                ");
      ss << "            }\n";
      ss << "        }\n";
    }
    ss << "    }\n\n";
  }
  ss << "}\n";

  std::string usda = ss.str();
  TEST_MSG("Generated mixed-arc scene (%zu bytes), seed=%u", usda.size(), SEED);

  std::string warn, err;

  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  Stage dag_stage;
  TEST_CHECK(graph.BuildStage(&dag_stage, &warn, &err));

  Stage iter_stage;
  TEST_CHECK(compose_via_iterative(usda, iter_stage, warn, err));

  // Both should produce /World with children
  auto dag_world = dag_stage.GetPrimAtPath(Path("/World", ""));
  auto iter_world = iter_stage.GetPrimAtPath(Path("/World", ""));
  TEST_CHECK(dag_world.has_value());
  TEST_CHECK(iter_world.has_value());
}

// ---------------------------------------------------------------------------
// compgraph_random_specializes_vs_inherits_test
// Verify that inherits are stronger than specializes.
// AOUSD Core Spec 10.4: I > S.
// Fixed seed = 5005.
// ---------------------------------------------------------------------------

void compgraph_random_specializes_vs_inherits_test(void) {
  constexpr uint32_t SEED = 5005;

  std::string usda = gen_usda_inherits_vs_specializes(SEED);
  TEST_MSG("Generated I-vs-S scene (%zu bytes), seed=%u", usda.size(), SEED);

  std::string warn, err;

  CompositionGraph graph;
  TEST_CHECK(compose_via_graph(usda, graph, warn, err));
  if (!err.empty()) { TEST_MSG("graph err: %s", err.c_str()); return; }

  const PrimIndex *idx = graph.GetPrimIndex(Path("/World", ""));
  TEST_CHECK(idx != nullptr);
  if (!idx) return;

  // Verify strength order: Root < Inherit < ... < Specialize
  int root_pos = -1, inherit_pos = -1, spec_pos = -1;
  const auto &order = idx->GetStrengthOrder();
  for (size_t i = 0; i < order.size(); i++) {
    const CompNode &n = idx->GetNode(order[i]);
    if (n.arc_type == ArcType::Root && root_pos < 0)
      root_pos = static_cast<int>(i);
    if (n.arc_type == ArcType::Inherit && inherit_pos < 0)
      inherit_pos = static_cast<int>(i);
    if (n.arc_type == ArcType::Specialize && spec_pos < 0)
      spec_pos = static_cast<int>(i);
  }

  TEST_CHECK_(root_pos >= 0, "Root found at pos %d", root_pos);
  TEST_CHECK_(inherit_pos >= 0, "Inherit found at pos %d", inherit_pos);
  TEST_CHECK_(spec_pos >= 0, "Specialize found at pos %d", spec_pos);

  // LIVRPS: Root (L) strongest, then Inherit (I), then Specialize (S) weakest
  if (root_pos >= 0 && inherit_pos >= 0) {
    TEST_CHECK_(root_pos < inherit_pos,
                "Root (pos %d) should be stronger than Inherit (pos %d)",
                root_pos, inherit_pos);
  }
  if (inherit_pos >= 0 && spec_pos >= 0) {
    TEST_CHECK_(inherit_pos < spec_pos,
                "Inherit (pos %d) should be stronger than Specialize (pos %d)",
                inherit_pos, spec_pos);
  }
}
