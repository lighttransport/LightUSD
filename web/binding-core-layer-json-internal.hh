// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Private layer JSON value conversions shared by import and export.
#pragma once
#include "core/minijson.hh"
#include <cstddef>
#include <string>
namespace lightusd {  class Value; class ValueView;  }

namespace lightusd {
namespace web_core {
namespace layer_json_detail {
constexpr size_t kMaxLayerJSONBytes = size_t{1} << 29;

bool ParseLayerJSONValue(const minijson::Value& encoded,
                         const std::string& type_name, ::lightusd::Value* out,
                         bool* is_array, std::string* error);
bool ParseLayerJSONMetadataValue(const minijson::Value& encoded,
                                ::lightusd::Value* out, size_t depth,
                                size_t* value_count);
minijson::Value LayerMetadataValueJSON(::lightusd::ValueView value);
size_t MetadataValueEntryCount(::lightusd::ValueView value);
}  // namespace layer_json_detail

}  // namespace web_core
}  // namespace lightusd
