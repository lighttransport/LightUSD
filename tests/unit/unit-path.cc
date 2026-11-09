#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-path.h"
#include "../../src/legacy/core/path.hh"
#include "../../src/legacy/crate-format.hh"  // PathHasher / PathKeyEqual

#include <string>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Helper: a tstring_view slice equals an expected literal.
static bool sv_eq(const tstring_view &v, const char *s) { return v == s; }

// Test 1: part views match expectations for absolute / property / root paths,
// and full_path_name() / append_full_path_name() agree.
void path_parts_views_test(void) {
  {
    Path p("/A/B", "");
    TEST_CHECK(p.is_valid());
    TEST_CHECK(sv_eq(p.prim_part_view(), "/A/B"));
    TEST_CHECK(p.prop_part_view().empty());
    TEST_CHECK(sv_eq(p.element_name_view(), "B"));
    TEST_CHECK(sv_eq(p.full_path_view(), "/A/B"));
    TEST_CHECK(p.full_path_name() == "/A/B");
    // back-compat accessors are now views over the same bytes
    TEST_CHECK(p.prim_part() == "/A/B");
    TEST_CHECK(p.element_name() == "B");
  }
  {
    Path p("/A/B", "visibility");
    TEST_CHECK(sv_eq(p.prim_part_view(), "/A/B"));
    TEST_CHECK(sv_eq(p.prop_part_view(), "visibility"));
    TEST_CHECK(sv_eq(p.element_name_view(), "visibility"));
    TEST_CHECK(sv_eq(p.full_path_view(), "/A/B.visibility"));
    TEST_CHECK(p.full_path_name() == "/A/B.visibility");
    std::string buf = "PREFIX:";
    p.append_full_path_name(&buf);
    TEST_CHECK(buf == "PREFIX:/A/B.visibility");
    TEST_CHECK(p.is_prim_property_path());
    TEST_CHECK(p.is_property_path());
  }
  {
    // "/A.prop" form (prop encoded in the prim string).
    Path p("/A.prop", "");
    TEST_CHECK(sv_eq(p.prim_part_view(), "/A"));
    TEST_CHECK(sv_eq(p.prop_part_view(), "prop"));
    TEST_CHECK(p.full_path_name() == "/A.prop");
  }
  {
    Path root = Path::make_root_path();
    TEST_CHECK(root.is_valid());
    TEST_CHECK(root.is_root_path());
    TEST_CHECK(sv_eq(root.prim_part_view(), "/"));
    TEST_CHECK(root.prop_part_view().empty());
    TEST_CHECK(root.element_name_view().empty());
    TEST_CHECK(root.full_path_name() == "/");
  }
  {
    Path rp("/Root", "");
    TEST_CHECK(rp.is_root_prim());
    TEST_CHECK(rp.is_absolute_path());
    TEST_CHECK(sv_eq(rp.element_name_view(), "Root"));
  }
}

// Test 2: incremental construction via append_element / append_property
// produces the same canonical buffer + offsets.
void path_append_build_test(void) {
  Path p = Path::make_root_path();
  p.append_element("A");
  p.append_element("B");
  p.append_element("C");
  TEST_CHECK(sv_eq(p.prim_part_view(), "/A/B/C"));
  TEST_CHECK(sv_eq(p.element_name_view(), "C"));
  TEST_CHECK(p.full_path_name() == "/A/B/C");

  Path q("/Model/Mesh", "");
  q.append_property("points");
  TEST_CHECK(sv_eq(q.prim_part_view(), "/Model/Mesh"));
  TEST_CHECK(sv_eq(q.prop_part_view(), "points"));
  TEST_CHECK(sv_eq(q.element_name_view(), "points"));
  TEST_CHECK(q.full_path_name() == "/Model/Mesh.points");

  // Equivalence with single-shot construction.
  Path direct("/A/B/C", "");
  TEST_CHECK(p.prim_part_view() == direct.prim_part_view());
  TEST_CHECK(p == direct);
}

// Test 3: variant selection "{v=sel}" stays embedded in the prim region and
// parent navigation strips it.
void path_variant_test(void) {
  Path p("/A", "");
  p.append_element("{v=sel}");
  TEST_CHECK(p.is_valid());
  // The variant group is part of the prim buffer.
  TEST_CHECK(sv_eq(p.prim_part_view(), "/A{v=sel}"));
  TEST_CHECK(sv_eq(p.full_path_view(), "/A{v=sel}"));
  TEST_CHECK(p.full_path_name() == "/A{v=sel}");

  // Parent of a variant element is the owning prim.
  Path parent = p.get_parent_prim_path();
  TEST_CHECK(sv_eq(parent.prim_part_view(), "/A"));
}

// Test 4: equality / ordering / PathHasher are consistent and parts-based.
void path_compare_hash_test(void) {
  Path a("/A/B", "");
  Path a2("/A/B", "");
  Path b("/A/C", "");
  Path ap("/A/B", "prop");

  TEST_CHECK(a == a2);
  TEST_CHECK(!(a == b));
  TEST_CHECK(!(a == ap));  // differ in prop part

  // operator< total order; equal paths are not < each other.
  TEST_CHECK(!(a < a2) && !(a2 < a));
  TEST_CHECK((a < b) != (b < a));  // strict order one way

  // PathHasher: equal paths hash equally; PathKeyEqual agrees with ==.
  crate::PathHasher hasher;
  crate::PathKeyEqual eq;
  TEST_CHECK(hasher(a) == hasher(a2));
  TEST_CHECK(eq(a, a2));
  TEST_CHECK(!eq(a, b));
  TEST_CHECK(!eq(a, ap));
}

// Test 4b: PathHasher and PathKeyEqual must distinguish paths that differ in
// variant selection. NOTE: append_element() embeds the variant group into the
// prim text, so prim_part() already differs ("/A{color=red}" vs
// "/A{color=blue}") — that prim-part difference alone is what drives the
// distinct hashes here. The variant-member hashing added for concern 5 is
// belt-and-suspenders for a hypothetical variant-only-in-members path, which
// the public API can't currently construct. This test pins the contract that
// variant-bearing paths hash distinctly and that the hasher agrees with
// operator==. See concern 5 in review.md.
void path_variant_hash_test(void) {
  Path base_red("/A", "");
  base_red.append_element("{color=red}");
  Path base_blue("/A", "");
  base_blue.append_element("{color=blue}");

  // operator== agrees the paths differ.
  TEST_CHECK(!(base_red == base_blue));

  crate::PathHasher hasher;
  crate::PathKeyEqual eq;

  // PathHasher and PathKeyEqual must agree with operator!=.
  TEST_CHECK(hasher(base_red) != hasher(base_blue));
  TEST_CHECK(!eq(base_red, base_blue));

  // And the same path inserted twice collapses as expected.
  Path base_red_dup("/A", "");
  base_red_dup.append_element("{color=red}");
  TEST_CHECK(hasher(base_red) == hasher(base_red_dup));
  TEST_CHECK(eq(base_red, base_red_dup));
}

// Test 5: get_parent_path and make_relative.
void path_parent_relative_test(void) {
  Path p("/A/B/C", "");
  Path parent = p.get_parent_path();
  TEST_CHECK(sv_eq(parent.prim_part_view(), "/A/B"));
  TEST_CHECK(sv_eq(parent.element_name_view(), "B"));

  Path prop("/A/B", "x");
  Path pprop_parent = prop.get_parent_path();  // /A/B.x -> /A/B
  TEST_CHECK(sv_eq(pprop_parent.prim_part_view(), "/A/B"));
  TEST_CHECK(pprop_parent.prop_part_view().empty());

  Path abs("/A/B/C", "");
  abs.make_relative();
  TEST_CHECK(sv_eq(abs.prim_part_view(), "A/B/C"));
  TEST_CHECK(sv_eq(abs.element_name_view(), "C"));
  TEST_CHECK(abs.is_relative_path());
}
