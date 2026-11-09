// SPDX-License-Identifier: Apache-2.0
#include "render-converter-clip.hh"
#include "next/layer/layer.hh"
#include <utility>
#include <vector>

namespace lightusd { namespace tydra { namespace next {
namespace {
enum class ClipRead { Failed, InvalidArray, Ready };
ClipRead ReadOwner(const ::lightusd::next::UsdPrim& owner,
               const ::lightusd::next::UsdPrim& prim, const char* name,
               double time, const ::lightusd::next::ValueClipStageLoader& loader,
               ValueArrayRead<float>* out) {
  std::vector<::lightusd::next::ValueClipSet> sets;
  if (!::lightusd::next::ParseValueClipSets(owner, &sets, nullptr))
    return ClipRead::Failed;
  for (auto& set : sets) set.prim_path = prim.GetPath().str();
  ::lightusd::next::Value clipped;
  if (!::lightusd::next::ResolveValueClipFromSets(
          sets, prim, name, time, loader, &clipped)) return ClipRead::Failed;
  out->scratch.materialized = std::move(clipped);
  return ::lightusd::next::GetFloatArrayView(
      out->scratch.materialized, &out->scratch, &out->view)
      ? ClipRead::Ready : ClipRead::InvalidArray;
}
}  // namespace

bool ReadFloatArrayFromValueClips(
    const ::lightusd::next::UsdPrim& prim, const char* name, double time,
    const ::lightusd::next::ValueClipStageLoader& loader,
    ValueArrayRead<float>* out) {
  if (!out || !name || !loader) return false;
  for (auto owner = prim; owner.IsValid(); owner = owner.GetParent()) {
    const auto* spec = owner.GetPrimSpec();
    if (!spec || !spec->meta().clips().is_dictionary()) continue;
    if (ReadOwner(owner, prim, name, time, loader, out) == ClipRead::Ready)
      return true;
    break;
  }
  // Some compact composed variant paths omit parent links. Retain the
  // existing curve resolver's fallback through the layer's path index.
  const auto* layer = prim.GetLayer();
  if (!layer) return false;
  std::string path = prim.GetPath().str();
  for (;;) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return false;
    path.resize(slash);
    const uint32_t index = layer->index_at_path(path);
    if (index == UINT32_MAX) continue;
    const auto* spec = layer->prim_at_path(::lightusd::next::Path(path));
    if (!spec || !spec->meta().clips().is_dictionary()) continue;
    const ClipRead result = ReadOwner(
        ::lightusd::next::UsdPrim(spec, layer, index), prim, name, time,
        loader, out);
    if (result == ClipRead::Failed) return false;
    if (result == ClipRead::Ready) return true;
  }
}
}}}  // namespace lightusd::tydra::next
