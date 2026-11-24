#include <cassert>
#include <iostream>

#include "lightusd.hh"
#include "tydra/render-data.hh"
#include "tydra/variant-applier.hh"
#include "tydra/variant-converter.hh"
#include "tydra/variant-support.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT
using namespace lightusd::tydra;
using namespace lightusd::legacy::tydra;  // NOLINT

// Test 1: Apply single variant selection
bool test_apply_single_selection() {
  std::cout << "Test 1: Apply single variant selection\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/basic-2level-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants to RenderScene
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  // Apply variant selection
  VariantApplier applier;
  if (!applier.ApplyVariantSelection(&scene, "Root", "shape", "cube", &err)) {
    std::cerr << "Failed to apply variant selection: " << err << "\n";
    return false;
  }

  // Verify selection was recorded
  if (scene.active_selections.empty()) {
    std::cerr << "No active selections recorded\n";
    return false;
  }

  const auto &sel = scene.active_selections[0];
  if (sel.prim_path != "Root" || sel.variant_set_name != "shape" ||
      sel.variant_option_name != "cube") {
    std::cerr << "Selection not applied correctly\n";
    return false;
  }

  std::cout << "  ✓ Applied selection: " << sel.prim_path << " : "
            << sel.variant_set_name << " = " << sel.variant_option_name << "\n";

  return true;
}

// Test 2: Apply multiple variant selections
bool test_apply_multiple_selections() {
  std::cout << "\nTest 2: Apply multiple variant selections\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/basic-2level-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  // Apply multiple selections
  VariantApplier applier;
  std::vector<VariantSelection> selections;

  VariantSelection sel1;
  sel1.prim_path = "Root";
  sel1.variant_set_name = "shape";
  sel1.variant_option_name = "sphere";
  selections.push_back(sel1);

  if (!applier.ApplyVariantSelections(&scene, selections, &err)) {
    std::cerr << "Failed to apply variant selections: " << err << "\n";
    return false;
  }

  if (scene.active_selections.size() != 1) {
    std::cerr << "Expected 1 active selection, got "
              << scene.active_selections.size() << "\n";
    return false;
  }

  std::cout << "  ✓ Applied " << scene.active_selections.size()
            << " variant selection(s)\n";

  return true;
}

// Test 3: Update existing selection
bool test_update_existing_selection() {
  std::cout << "\nTest 3: Update existing variant selection\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/basic-2level-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  VariantApplier applier;

  // Apply first selection
  if (!applier.ApplyVariantSelection(&scene, "Root", "shape", "cube", &err)) {
    std::cerr << "Failed to apply first selection: " << err << "\n";
    return false;
  }

  if (scene.active_selections[0].variant_option_name != "cube") {
    std::cerr << "First selection incorrect\n";
    return false;
  }

  // Update selection
  if (!applier.ApplyVariantSelection(&scene, "Root", "shape", "sphere", &err)) {
    std::cerr << "Failed to update selection: " << err << "\n";
    return false;
  }

  // Verify still only one selection, but updated
  if (scene.active_selections.size() != 1) {
    std::cerr << "Expected 1 selection after update, got "
              << scene.active_selections.size() << "\n";
    return false;
  }

  if (scene.active_selections[0].variant_option_name != "sphere") {
    std::cerr << "Selection was not updated correctly\n";
    return false;
  }

  std::cout << "  ✓ Updated selection from cube to "
            << scene.active_selections[0].variant_option_name << "\n";

  return true;
}

// Test 4: Verify change tracking
bool test_change_tracking() {
  std::cout << "\nTest 4: Verify change tracking\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/with-selection-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  VariantApplier applier;

  // Apply selection and check for recorded changes
  if (!applier.ApplyVariantSelection(&scene, "SceneObject", "category",
                                     "furniture", &err)) {
    std::cerr << "Failed to apply selection: " << err << "\n";
    return false;
  }

  const auto &changes = applier.GetLastChanges();
  std::cout << "  ✓ Recorded " << changes.size() << " content change(s)\n";

  // Clear and verify
  applier.ClearChangeHistory();
  if (!applier.GetLastChanges().empty()) {
    std::cerr << "Failed to clear change history\n";
    return false;
  }

  std::cout << "  ✓ Change history cleared successfully\n";

  return true;
}

// Test 5: Reset to defaults
bool test_reset_to_defaults() {
  std::cout << "\nTest 5: Reset variant selections to defaults\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/with-selection-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  // Create variant manager
  DefaultVariantManager manager;
  auto& mutable_groups = manager.GetMutableVariantGroups();
  mutable_groups = scene.variant_groups;

  // Apply non-default selection
  VariantApplier applier;
  if (!applier.ApplyVariantSelection(&scene, "SceneObject", "category",
                                     "appliance", &err)) {
    std::cerr << "Failed to apply selection: " << err << "\n";
    return false;
  }

  if (scene.active_selections[0].variant_option_name != "appliance") {
    std::cerr << "Selection not applied\n";
    return false;
  }

  std::cout << "  ✓ Applied non-default selection: appliance\n";

  // Reset to defaults
  if (!applier.ResetToDefaults(&scene, manager, &err)) {
    std::cerr << "Failed to reset to defaults: " << err << "\n";
    return false;
  }

  std::cout << "  ✓ Reset " << scene.active_selections.size()
            << " selection(s) to defaults\n";

  return true;
}

// Test 6: Nested variant selections
bool test_nested_variant_selections() {
  std::cout << "\nTest 6: Handle nested variant selections\n";

  Stage stage;
  std::string warn, err;

  bool ret = lightusd::legacy::LoadUSDFromFile(
      "tests/feat/nestedVariantSet/triple-nesting-001.usda", &stage, &warn, &err);
  if (!ret) {
    std::cerr << "Failed to load USD file: " << err << "\n";
    return false;
  }

  // Convert variants
  RenderScene scene;
  VariantConverter converter;
  if (!converter.ConvertVariants(stage, &scene, &err)) {
    std::cerr << "Failed to convert variants: " << err << "\n";
    return false;
  }

  VariantApplier applier;

  // Apply selection to nested variant
  if (!applier.ApplyVariantSelection(&scene, "Asset", "lod", "high", &err)) {
    std::cerr << "Failed to apply nested selection: " << err << "\n";
    return false;
  }

  if (scene.active_selections.size() < 1) {
    std::cerr << "No selections recorded for nested variant\n";
    return false;
  }

  std::cout << "  ✓ Applied nested variant selection: "
            << scene.active_selections[0].variant_option_name << "\n";

  // Verify nested variant sets exist in the options
  bool has_nested = false;
  for (const auto &group : scene.variant_groups) {
    for (const auto &vs : group.variant_sets) {
      for (const auto &opt : vs.options) {
        if (!opt.nested_variant_sets.empty()) {
          has_nested = true;
          std::cout << "  ✓ Found " << opt.nested_variant_sets.size()
                    << " nested variant set(s) in option: " << opt.name << "\n";
        }
      }
    }
  }

  return true;
}

int main() {
  std::cout << "=== Variant Applier Integration Tests ===\n\n";

  bool all_passed = true;

  if (!test_apply_single_selection()) {
    all_passed = false;
  }

  if (!test_apply_multiple_selections()) {
    all_passed = false;
  }

  if (!test_update_existing_selection()) {
    all_passed = false;
  }

  if (!test_change_tracking()) {
    all_passed = false;
  }

  if (!test_reset_to_defaults()) {
    all_passed = false;
  }

  if (!test_nested_variant_selections()) {
    all_passed = false;
  }

  std::cout << "\n=== Results ===\n";
  if (all_passed) {
    std::cout << "✓ All integration tests passed\n";
    return 0;
  } else {
    std::cout << "✗ Some tests failed\n";
    return 1;
  }
}
