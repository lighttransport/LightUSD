// SPDX-License-Identifier: Apache-2.0
// Author a translated quad, indexed UVs and a custom attribute on a core layer.
#include "layer/layer.hh"
#include "writer/usda-writer.hh"
#include <iostream>

int main() {
  using namespace lightusd;
  Layer layer;
  LayerBuilder builder(layer);
  builder.define_prim("root", "Xform");
  builder.add_property("xformOp:translate", Value::MakeDouble3(1, 2, 3));
  builder.add_property("xformOpOrder", Value::MakeTokenArray({"xformOp:translate"}),
                       PropSlot::kFlagUniform);
  builder.define_prim("quad", "Mesh");
  Value points = Value::MakeFloat3Array({0,0,0, 1,0,0, 1,1,0, 0,1,0});
  points.retag_role(TypeId::Point3f);
  builder.add_property("points", std::move(points));
  builder.add_property("faceVertexCounts", Value::MakeIntArray({3, 3}));
  builder.add_property("faceVertexIndices", Value::MakeIntArray({0, 1, 2, 0, 2, 3}));
  Value uv = Value::MakeFloat2Array({0,0, 1,0, 1,1, 0,1});
  uv.retag_role(TypeId::Texcoord2f);
  builder.add_property("primvars:uv", std::move(uv));
  auto& interpolation = builder.current()->ensure_property_meta("primvars:uv");
  interpolation.interpolation = "vertex";
  interpolation.authored |= PropMeta::kInterpolation;
  builder.add_property("primvars:uv:indices", Value::MakeIntArray({0,1,2,3}));
  builder.add_property("myvalue", Value(3.0),
                       PropSlot::kFlagCustom | PropSlot::kFlagUniform);
  auto& custom = builder.current()->ensure_property_meta("myvalue");
  custom.hidden = false;
  custom.authored |= PropMeta::kHidden;
  builder.end_prim();
  builder.end_prim();
  const auto result = WriteLayerToFile("output.usda", layer);
  if (!result.success) std::cerr << result.error << '\n';
  return result.success ? 0 : 1;
}
