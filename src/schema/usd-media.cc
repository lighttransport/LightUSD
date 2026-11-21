// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdMedia Schema Implementation

#include "usd-media.hh"

namespace lightusd {

bool IsMediaAudio(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "MediaAudio";
}

bool GetMediaAudioData(const Stage& stage, const UsdPrim& prim,
                        MediaAudioData* out) {
  if (!IsMediaAudio(prim) || !out) return false;

  (void)stage;

  // filePath
  {
    const Value* val = prim.GetPropertyValue("media:filePath");
    if (val) {
      auto s = val->as_string();
      if (s) out->filePath = *s;
    }
  }

  // gain
  {
    const Value* val = prim.GetPropertyValue("media:gain");
    if (val) {
      const float* f = val->as_float();
      if (f) out->gain = *f;
    }
  }

  // pitch
  {
    const Value* val = prim.GetPropertyValue("media:pitch");
    if (val) {
      const float* f = val->as_float();
      if (f) out->pitch = *f;
    }
  }

  // startTime
  {
    const Value* val = prim.GetPropertyValue("media:startTime");
    if (val) {
      const float* f = val->as_float();
      if (f) out->startTime = *f;
    }
  }

  // endTime
  {
    const Value* val = prim.GetPropertyValue("media:endTime");
    if (val) {
      const float* f = val->as_float();
      if (f) out->endTime = *f;
    }
  }

  // loop
  {
    const Value* val = prim.GetPropertyValue("media:loop");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->loop = *b;
    }
  }

  return true;
}

bool GetMediaDefaultThumbnail(const UsdPrim& prim, std::string* default_image) {
  if (!default_image || !prim.IsValid() || !prim.HasAPI("AssetPreviewsAPI")) return false;
  const auto& meta = prim.GetMeta();
  if (!meta.assetInfoAuthored()) return false;
  DictView dict = meta.assetInfo().as_dictionary();
  for (const char* key : {"previews", "thumbnails", "default"}) {
    if (!dict) return false;
    dict = dict.find(key).as_dictionary();
  }
  if (!dict) return false;
  const ValueView image = dict.find("defaultImage");
  if (!image || image.type_id() != TypeId::AssetPath) return false;
  const StringView path = image.as_asset_path();
  default_image->assign(path.data(), path.size());
  return true;
}

} // namespace lightusd
