// SPDX-License-Identifier: Apache-2.0
#include <iostream>

#include "layer/asset-anchor.hh"
#include "reader/usda-reader.hh"
#include "usdz/material-optimize.hh"
#define CHECK(expr)                                                \
  do {                                                             \
    if (!(expr)) {                                                 \
      std::cerr << "Failure line " << __LINE__ << ": " #expr "\n"; \
      return 1;                                                    \
    }                                                              \
  } while (false)
int main() {
  using namespace lightusd;
  auto source = LoadUSDAFromString(R"USD(#usda 1.0
  def Material "M" (prepend variantSets="look") {
    asset inputs:file=@tex.png@
    asset inputs:animated.timeSamples={0:@tex.png@,1:@tile.<UDIM>.png@}
    variantSet "look"={
      "a" {
        asset file=@variant.png@
        def Shader "Child" {asset inputs:file=@nested.png@}
      }
    }
  })USD");
  CHECK(source.success);
  Layer* layer = source.stage.GetRootLayer();
  StampLayerAssetAnchor(layer, "looks/source.usda");
  Layer clone = layer->Clone();
  CHECK(RebaseLayerAssetPaths(&clone, {}) == 5);
  const PrimSpec* rebased = clone.prim_at_path("/M");
  CHECK(rebased->property_value("inputs:file")->as_asset_path() ==
        "looks/tex.png");
  auto samples =
      rebased->time_samples(GetPropNameTable().find("inputs:animated"));
  CHECK(samples && samples->size() == 2);
  CHECK(rebased->time_sample_value((*samples)[1].second)->as_asset_path() ==
        "looks/tile.<UDIM>.png");
  const auto& variant = rebased->meta().variantSets()[0].variants[0];
  CHECK(variant.properties[0].value.as_asset_path() == "looks/variant.png");
  const PrimSpec* child = variant.content
                              ? variant.content->prim_at_path("/__self__/Child")
                              : nullptr;
  CHECK(child && child->property_value("inputs:file")->as_asset_path() ==
                     "looks/nested.png");
  // Layer::Clone shares variant content, so rebasing must detach that content.
  const PrimSpec* original = layer->prim_at_path("/M");
  CHECK(original->property_value("inputs:file")->as_asset_path() == "tex.png");
  CHECK(original->meta()
            .variantSets()[0]
            .variants[0]
            .content->prim_at_path("/__self__/Child")
            ->property_value("inputs:file")
            ->as_asset_path() == "nested.png");
  PrimSpec standalone = original->Clone();
  const std::map<std::string, std::string> remap = {
      {"tex.png", "root.png"}, {"looks/tex.png", "packaged.png"}};
  CHECK(standalone.remap_asset_paths(remap) > 0);
  CHECK(standalone.property_value("inputs:file")->as_asset_path() ==
        "root.png");
  standalone = original->Clone();
  CHECK(standalone.remap_asset_paths(remap, true) > 0);
  CHECK(standalone.property_value("inputs:file")->as_asset_path() ==
        "packaged.png");
  auto materials = LoadUSDAFromString(R"USD(#usda 1.0
    def Xform "Root" {
      def Material "A" {def Shader "Tex" {uniform token info:id="UsdUVTexture";asset inputs:file=@tex.png@}}
      def Material "B" {def Shader "Tex" {uniform token info:id="UsdUVTexture";asset inputs:file=@tex.png@}}
    })USD");
  CHECK(materials.success);
  Layer* material_layer = materials.stage.GetRootLayer();
  material_layer->prim_at_path_mutable("/Root/A/Tex")
      ->set_asset_anchor_id(InternAssetAnchor("red"));
  material_layer->prim_at_path_mutable("/Root/B/Tex")
      ->set_asset_anchor_id(InternAssetAnchor("blue"));
  CHECK(usdz::DedupeMaterials(material_layer) == 0);
  material_layer->prim_at_path_mutable("/Root/B/Tex")
      ->set_asset_anchor_id(InternAssetAnchor("red"));
  CHECK(usdz::DedupeMaterials(material_layer) == 1);
  // Comparison normalization must not change the retained material's asset.
  CHECK(material_layer->prim_at_path("/Root/A/Tex")
            ->property_value("inputs:file")
            ->as_asset_path() == "tex.png");
  std::cout << "PASS: source-layer rebasing, samples, variants, clone "
               "isolation and material identity\n";
}
