// SPDX-License-Identifier: Apache-2.0
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <cmath>
#include <vector>
#include "lightusd-cpp.hh"
#include "public_stage_queries.hh"

static lightusd::api::Stage Load(const char* source) {
  lightusd_stage* stage = nullptr;
  assert(lightusd_stage_load_from_memory(reinterpret_cast<const uint8_t*>(source),
      std::strlen(source), nullptr, &stage) == LIGHTUSD_OK);
  return lightusd::api::Stage(stage);
}
int main() {
  auto stage = Load(R"(#usda 1.0
(defaultPrim = "Root"
 upAxis = "Z"
 metersPerUnit = 2
 startTimeCode = -3
 endTimeCode = 72
 framesPerSecond = 30
 documentation = "test doc"
 comment = "test comment"
 timeCodesPerSecond = 48)
def Xform "Root" {
 def Scope "A" { def Scope "Nested" {} }
 def Scope "Disabled" (active = false) {}
}
def Scope "Last" {}
)");
  lusdview::PublicStageInfo info;
  assert(lusdview::ReadPublicStageInfo(stage.get(), &info));
  assert(info.defaultPrim == "Root" && info.upAxis == "Z");
  assert(info.metersPerUnit == 2 && info.startTimeCode == -3 && info.endTimeCode == 72);
  assert(info.timeCodesPerSecond == 48 && info.primCount == 5);
  assert(info.framesPerSecond == 30 && info.startTimeCodeAuthored && info.endTimeCodeAuthored);
  assert(info.comment == "test comment" && info.documentation == "test doc");
  assert(!lusdview::ReadPublicStageInfo(nullptr, &info));
  assert(info.defaultPrim == "Root" && info.primCount == 5);
  assert(!lusdview::ReadPublicStageInfo(stage.get(), nullptr));
  std::vector<std::string> paths;
  bool inactive = false;
  lusdview::VisitPublicPrims(stage.get(), [&](lightusd_prim prim) {
    paths.push_back(lusdview::PublicString(lightusd_prim_path(prim)));
    if (paths.back() == "/Root/Disabled") inactive = !lightusd_prim_is_active(prim);
    return true;
  });
  assert((paths == std::vector<std::string>{"/Root", "/Root/A", "/Root/A/Nested", "/Root/Disabled", "/Last"}));
  assert(inactive);
  paths.clear();
  lusdview::VisitPublicPrims(stage.get(), [&](lightusd_prim prim) {
    paths.push_back(lusdview::PublicString(lightusd_prim_path(prim)));
    return paths.size() < 2;
  });
  assert(paths.size() == 2 && paths.back() == "/Root/A");
  lusdview::VisitPublicPrims(nullptr, [](lightusd_prim) { assert(false); return false; });
  lusdview::VisitPublicPrims(stage.get(), {});
  auto empty = Load("#usda 1.0\n");
  lusdview::VisitPublicPrims(empty.get(), [](lightusd_prim) { assert(false); return false; });
  assert(lusdview::ReadPublicStageInfo(empty.get(), &info));
  assert(info.defaultPrim.empty() && info.primCount == 0);
  assert(!info.startTimeCodeAuthored && !info.endTimeCodeAuthored);
  auto zero = Load("#usda 1.0\n(startTimeCode = 0)\n");
  assert(lusdview::ReadPublicStageInfo(zero.get(), &info));
  assert(info.startTimeCode == 0 && info.startTimeCodeAuthored && !info.endTimeCodeAuthored);
  auto properties = Load(R"(#usda 1.0
def Cube "Cube" {
 custom bool enabled = true
 custom int count = 42
 custom float weight = 0.5
 custom string label = "sample"
 custom asset texture = @texture.png@
 custom float[] weights = [1, 2, 3]
 token visibility = None
}
)");
  const auto prim = lightusd_stage_prim_at_path(properties.get(), "/Cube");
  auto summary = [&](const char* name) {
    lightusd_value_view value{};
    lightusd_sv text{};
    assert(lightusd_attr_inspect_default(prim, name, &value, &text) == LIGHTUSD_OK);
    if (value.is_array) assert(!value.data);
    return lusdview::PublicDefaultSummary(value, text);
  };
  assert(summary("enabled") == "true");
  assert(summary("count") == "42");
  assert(summary("weight") == "0.500000");
  assert(summary("label") == "sample");
  assert(summary("texture") == "@texture.png@");
  assert(summary("size") == "2.000000"); // schema fallback
  lightusd_value_view array{};
  assert(lightusd_attr_inspect_default(prim, "weights", &array, nullptr) == LIGHTUSD_OK);
  assert(summary("weights") == std::string(lightusd_type_name(array.type)) + "[3]");
  assert(lightusd_attr_inspect_default(prim, "visibility", &array, nullptr) == LIGHTUSD_ERR_NOT_FOUND);

  auto extents = Load(R"(#usda 1.0
def Scope "Float" { float3[] extent = [(-1,-2,-3),(4,5,6)] }
def Scope "Double" { double3[] extentsHint = [(-2,-3,-4),(5,6,7),(8,9,10)] }
def Scope "Short" {
 float3[] extent = [(1,2,3)]
 float3[] extentsHint = [(0,0,0),(9,9,9)]
}
def Scope "Blocked" {
 float3[] extent = None
 float3[] extentsHint = [(-3,-4,-5),(6,7,8)]
}
def Scope "Wrong" { int3[] extent = [(1,2,3),(4,5,6)] }
def Scope "Sampled" { float3[] extent.timeSamples = {0: [(-1,-1,-1),(1,1,1)]} }
)");
  float min[3] = {}, max[3] = {};
  auto extent = [&](const char* path) {
    return lusdview::ReadPublicPreviewExtent(
        lightusd_stage_prim_at_path(extents.get(), path), min, max);
  };
  assert(extent("/Float") && min[0] == -1 && max[2] == 6);
  assert(extent("/Double") && min[0] == -2 && max[2] == 7);
  assert(extent("/Blocked") && min[0] == -3 && max[2] == 8);
  // A present but unusable extent must not fall through to extentsHint.
  assert(!extent("/Short") && min[0] == -3 && max[2] == 8);
  assert(!extent("/Wrong") && !extent("/Sampled") && !extent("/Missing"));
  assert(!lusdview::ReadPublicPreviewExtent({}, min, max));
  assert(!lusdview::ReadPublicPreviewExtent({}, nullptr, max));

  auto blends = Load(R"(#usda 1.0
def SkelAnimation "External" {
 token[] blendShapes = ["smile", "blink"]
 float[] blendShapeWeights = [0.1, 0.2]
 float[] blendShapeWeights.timeSamples = {0: [0, 0.2], 2: [1, 0.6]}
}
def SkelRoot "Rig" {
 def SkelAnimation "Fallback" {
  token[] blendShapes = ["fallback"]
  float[] blendShapeWeights = [0.75]
 }
 def Scope "Parent" (active = false) {
  rel skel:animationSource = </External>
  def Mesh "Explicit" {}
 }
 def Mesh "FallbackMesh" { rel skel:animationSource = </Missing> }
 def SkelRoot "Nested" { def Mesh "Mesh" {} }
}
def SkelRoot "Mismatch" {
 def SkelAnimation "Animation" {
  string[] blendShapes = ["only"]
  float[] blendShapeWeights.timeSamples = {0: [0.25], 2: [1, 2]}
 }
 def Mesh "Mesh" {}
}
def SkelRoot "EmptyNames" {
 def SkelAnimation "Animation" {
  token[] blendShapes = ["defaultName"]
  token[] blendShapes.timeSamples = {0: []}
  float[] blendShapeWeights = [0.5]
 }
 def Mesh "Mesh" {}
}
def Mesh "Unbound" {}
)");
  auto blend = [&](const char* path, double time) {
    return lusdview::ReadPublicBlendWeights(blends.get(),
        lightusd_stage_prim_at_path(blends.get(), path), time);
  };
  auto weights = blend("/Rig/Parent/Explicit", 1);
  assert(weights.size() == 2 && weights.at("smile") == 0.5f);
  assert(std::fabs(weights.at("blink") - 0.4f) < 1.e-6f);
  assert(blend("/Rig/Parent/Explicit", -1).at("smile") == 0);
  assert(blend("/Rig/Parent/Explicit", 3).at("smile") == 1);
  assert(blend("/Rig/FallbackMesh", 1).at("fallback") == 0.75f);
  // Retain the existing outermost-SkelRoot fallback and inactive traversal.
  assert(blend("/Rig/Nested/Mesh", 1).at("fallback") == 0.75f);
  assert(blend("/Mismatch/Mesh", 1).at("only") == 0.25f);
  assert(blend("/EmptyNames/Mesh", 1).at("defaultName") == 0.5f);
  assert(blend("/Unbound", 1).empty() && blend("/Missing", 1).empty());
  assert(lusdview::ReadPublicBlendWeights(nullptr, {}, 1).empty());
  assert(lusdview::ReadPublicBlendWeights(stage.get(),
      lightusd_stage_prim_at_path(blends.get(), "/Unbound"), 1).empty());

}
