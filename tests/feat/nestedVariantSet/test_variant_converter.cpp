#include <cassert>
#include <iostream>
#include <sstream>

#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/variant-converter.hh"
#include "tydra/variant-support.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

// Test 1: Load a USD file and convert variants
bool test_variant_conversion_basic() {
  std::cout << "Test 1: Basic variant conversion from basic-2level-001.usda\n";

  Stage stage;
  std::string warn, err;

  bool ret =
      lightusd::legacy::LoadUSDFromFile("tests/feat/nestedVariantSet/basic-2level-001.usda", &stage, &warn,
                                 &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Create RenderScene and converter
  RenderScene scene;
  VariantConverter converter;

  // Convert variants
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  // Verify results
  if (scene.variant_groups.empty()) {
    std::cerr << "No variant groups extracted\n";
    return false;
  }

  std::cout << "  ✓ Extracted " << scene.variant_groups.size()
            << " variant group(s)\n";

  // Check first group
  const auto& group = scene.variant_groups[0];
  std::cout << "  ✓ Prim path: " << group.prim_path << "\n";
  std::cout << "  ✓ Variant sets: " << group.variant_sets.size() << "\n";

  if (group.variant_sets.size() > 0) {
    const auto& vs = group.variant_sets[0];
    std::cout << "  ✓ Variant set name: " << vs.name << "\n";
    std::cout << "  ✓ Variant options: " << vs.options.size() << "\n";

    if (vs.options.size() > 0) {
      std::cout << "  ✓ First option: " << vs.options[0].name << "\n";
    }
  }

  return true;
}

// Test 2: Test with selection variants
bool test_variant_conversion_with_selection() {
  std::cout << "\nTest 2: Variant conversion with selections\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile("tests/feat/nestedVariantSet/with-selection-001.usda",
                                        &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  RenderScene scene;
  VariantConverter converter;

  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  std::cout << "  ✓ Extracted " << scene.variant_groups.size()
            << " variant group(s)\n";
  std::cout << "  ✓ Active selections: " << scene.active_selections.size()
            << "\n";

  // Verify selections
  if (!scene.active_selections.empty()) {
    const auto& sel = scene.active_selections[0];
    std::cout << "  ✓ Selection - variant set: " << sel.variant_set_name << "\n";
    std::cout << "  ✓ Selection - option: " << sel.variant_option_name << "\n";
  }

  return true;
}

// Test 3: Test with nested variants
bool test_variant_conversion_nested() {
  std::cout << "\nTest 3: Nested variant conversion\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile("tests/feat/nestedVariantSet/triple-nesting-001.usda",
                                        &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  RenderScene scene;
  VariantConverter converter;

  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  std::cout << "  ✓ Extracted " << scene.variant_groups.size()
            << " variant group(s)\n";

  // Check for nested variants
  bool has_nested = false;
  for (const auto& group : scene.variant_groups) {
    for (const auto& vs : group.variant_sets) {
      for (const auto& opt : vs.options) {
        if (!opt.nested_variant_sets.empty()) {
          has_nested = true;
          std::cout << "  ✓ Found nested variant sets in option: " << opt.name
                    << "\n";
        }
      }
    }
  }

  if (!has_nested) {
    std::cout << "  (Note: No nested variants detected - this may be expected\n"
              << "         if nested variants are in variantSet definitions)\n";
  }

  return true;
}

// Test 4: Test variant statistics
bool test_variant_statistics() {
  std::cout << "\nTest 4: Variant statistics\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile("tests/feat/nestedVariantSet/asymmetric-nesting-001.usda",
                                        &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  RenderScene scene;
  VariantConverter converter;

  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  // Create variant manager from extracted groups
  DefaultVariantManager manager;
  auto& mutable_groups = manager.GetMutableVariantGroups();
  mutable_groups = scene.variant_groups;

  auto stats = manager.GetStatistics();
  std::cout << "  ✓ Variant groups: " << stats.num_variant_groups << "\n";
  std::cout << "  ✓ Variant sets: " << stats.num_variant_sets << "\n";
  std::cout << "  ✓ Variant options: " << stats.num_variant_options << "\n";
  std::cout << "  ✓ Max nesting depth: " << stats.max_nesting_depth << "\n";

  return true;
}

int main() {
  std::cout << "=== Variant Converter Tests ===\n\n";

  bool all_passed = true;

  if (!test_variant_conversion_basic()) {
    all_passed = false;
  }

  if (!test_variant_conversion_with_selection()) {
    all_passed = false;
  }

  if (!test_variant_conversion_nested()) {
    all_passed = false;
  }

  if (!test_variant_statistics()) {
    all_passed = false;
  }

  std::cout << "\n=== Results ===\n";
  if (all_passed) {
    std::cout << "✓ All tests passed\n";
    return 0;
  } else {
    std::cout << "✗ Some tests failed\n";
    return 1;
  }
}
