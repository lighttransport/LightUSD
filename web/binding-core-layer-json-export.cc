// SPDX-License-Identifier: Apache-2.0
#include "binding-core-layer.hh"
#include "c-stage-bridge.hh"
#include "stage/stage.hh"
#include "writer/layer-json.hh"
#include <utility>
namespace lightusd { namespace web_core {
int32_t LayerDocument::exportJSONSize() {
  if (!ensureLoaded_()) return -1;
  clearExport_();
  const auto* stage = lightusd_internal::BorrowNativeStage(stage_);
  const auto* layer = stage ? stage->GetRootLayer() : nullptr;
  if (!layer) return fail_("No layer is loaded");
  auto result = WriteLayerToJSON(*layer);
  if (!result) return fail_(result.error().c_str());
  layer_json_ = std::move(result.value());
  error_.clear();
  return static_cast<int32_t>(layer_json_.size());
}
uintptr_t LayerDocument::exportJSONData() const {
  return reinterpret_cast<uintptr_t>(layer_json_.data());
}
} }
