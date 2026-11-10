// SPDX-License-Identifier: Apache-2.0
#include "diff/layer-diff.hh"
#include "reader/usda-reader.hh"
#include "reader/usdc-reader.hh"
#include "writer/usdc-writer.hh"
#include <iostream>
#include <vector>

int main() {
  using namespace lightusd;
  auto ascii = LoadUSDAFromString(R"USD(#usda 1.0
    def Scope "P" (
      prepend variantSets = "look"
      variants = { string look = "red" }
    ) {
      variantSet "look" = {
        "red" {
          float gain = 1
          float animated.timeSamples = { 0: 1, 1: 2 }
          def Scope "Child" { int x = 7; }
        }
        "blue" { float gain = 2 }
      }
    }
  )USD");
  if (!ascii.success) return 1;
  std::vector<uint8_t> bytes;
  if (!WriteUSDCToMemory(bytes, ascii.stage).success) return 1;
  auto crate = LoadUSDCFromMemory(bytes.data(), bytes.size());
  if (!crate.success) return 1;
  const Layer* layer = crate.stage.GetRootLayer();
  if (!layer || layer->prim_count() != 1 || layer->prim_at_path("/P/{look=red}")) {
    std::cerr << "Crate exposed bracketed variant implementation specs\n";
    return 1;
  }
  const PrimSpec* prim = layer->prim_at_path("/P");
  if (!prim || prim->meta().variantSets().size() != 1) return 1;
  const auto& set = prim->meta().variantSets()[0];
  if (set.selected != "red" || set.variants.size() != 2) return 1;
  const PrimVariantData* red = nullptr;
  for (const auto& option : set.variants) if (option.name == "red") red = &option;
  if (!red || red->properties.size() != 1 || !red->content) return 1;
  const Value& gain = red->properties[0].value;
  if (!gain.as_float() || *gain.as_float() != 1.0f) return 1;
  const PrimSpec* child = red->content->prim_at_path("/__self__/Child");
  const Value* x = child ? child->property_value("x") : nullptr;
  if (!x || !x->as_int() || *x->as_int() != 7) return 1;
  const PrimSpec* self = red->content->prim_at_path("/__self__");
  if (!self || !self->has_any_time_samples() ||
      !self->property_type_name("animated") ||
      *self->property_type_name("animated") != "float") return 1;
  if (DiffToText(*ascii.stage.GetRootLayer(), *layer) != "No differences found.\n") return 1;
  bytes.clear();
  if (!WriteUSDCToMemory(bytes, crate.stage).success) return 1;
  auto again = LoadUSDCFromMemory(bytes.data(), bytes.size());
  if (!again.success || DiffToText(*layer, *again.stage.GetRootLayer()) !=
      "No differences found.\n") return 1;
  std::cout << "PASS: Crate variants expose inline content and survive a second round trip\n";
  return 0;
}
