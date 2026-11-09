// SPDX-License-Identifier: Apache-2.0
// Editable single-layer document operations implemented through lightusd_c.
#include "binding-next-layer.hh"
#include "binding-next-assets.hh"
#include "c-stage-bridge.hh"
#include "next/stage/stage.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace tn = lightusd::next;

namespace lightusd::web_next {
namespace {
constexpr uint32_t kMaxLayerDocumentBytes = uint32_t{1} << 29;
constexpr uint32_t kMaxRelationshipTargets = uint32_t{1} << 16;

bool CopyCString(const uint8_t* data, uint32_t size, std::string* out) {
  if (!out || (size && !data) ||
      (size && std::memchr(data, 0, size) != nullptr)) return false;
  out->assign(size ? reinterpret_cast<const char*>(data) : "", size);
  return true;
}

bool IsHalfType(lightusd_type type) {
  switch (type) {
    case LIGHTUSD_TYPE_HALF: case LIGHTUSD_TYPE_HALF2:
    case LIGHTUSD_TYPE_HALF3: case LIGHTUSD_TYPE_HALF4:
    case LIGHTUSD_TYPE_QUATH: case LIGHTUSD_TYPE_POINT3H:
    case LIGHTUSD_TYPE_VECTOR3H: case LIGHTUSD_TYPE_NORMAL3H:
    case LIGHTUSD_TYPE_COLOR3H: case LIGHTUSD_TYPE_COLOR4H:
    case LIGHTUSD_TYPE_TEXCOORD2H: case LIGHTUSD_TYPE_TEXCOORD3H:
      return true;
    default: return false;
  }
}
}  // namespace

NextLayerDocument::~NextLayerDocument() {
  end();
}

void NextLayerDocument::end() {
  if (stage_) lightusd_stage_destroy(stage_);
  stage_ = nullptr;
  clearExport_();
  error_.clear();
}

int32_t NextLayerDocument::fail_(const char* message) {
  error_ = message ? message : "Layer operation failed";
  return -1;
}

int32_t NextLayerDocument::failJSON_(const char* message) {
  if (stage_) lightusd_stage_destroy(stage_);
  stage_ = nullptr;
  clearExport_();
  error_ = message ? message : "Layer JSON import failed";
  return -1;
}

bool NextLayerDocument::ensureLoaded_() {
  if (stage_) return true;
  fail_("No layer is loaded");
  return false;
}

void NextLayerDocument::clearExport_() {
  if (export_text_) lightusd_string_destroy(export_text_);
  if (export_crate_) lightusd_string_destroy(export_crate_);
  export_text_ = nullptr;
  export_crate_ = nullptr;
  export_usdz_.clear();
  mh_profile_json_.clear();
  shading_graph_json_.clear();
  layer_json_.clear();
}


int32_t NextLayerDocument::load(
    const uint8_t* bytes, uint32_t size,
    const std::function<bool(const char*, size_t, size_t)>& progress) {
  if (!bytes || !size) return fail_("Layer input is empty");
  if (size > kMaxLayerDocumentBytes)
    return fail_("Layer input exceeds 512 MiB limit");

  lightusd_load_options options;
  lightusd_load_options_init(&options);
  options.composed = 0;
  options.max_input_bytes = kMaxLayerDocumentBytes;
  options.max_resident_bytes = kMaxLayerDocumentBytes;
  if (progress) {
    options.progress_callback = [](void* userdata, const char* phase,
                                   size_t current, size_t total) {
      const auto* callback = static_cast<const std::function<bool(
          const char*, size_t, size_t)>*>(userdata);
      return (*callback)(phase, current, total) ? 1 : 0;
    };
    options.progress_userdata = const_cast<void*>(
        static_cast<const void*>(&progress));
  }
  lightusd_stage* candidate = nullptr;
  const lightusd_status status = lightusd_stage_load_from_memory(
      bytes, size, &options, &candidate);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Layer load failed";
    return -1;
  }

  if (stage_) lightusd_stage_destroy(stage_);
  stage_ = candidate;
  clearExport_();
  error_.clear();
  return 0;
}


int32_t NextLayerDocument::definePrim(const uint8_t* path,
                                      uint32_t path_size,
                                      const uint8_t* type,
                                      uint32_t type_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, type_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(type, type_size, &type_text))
    return fail_("Invalid prim path or type");
  const lightusd_status status = lightusd_stage_define_prim(
      stage_, path_text.c_str(), type_text.empty() ? nullptr : type_text.c_str(),
      0, nullptr);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::removePrim(const uint8_t* path,
                                      uint32_t path_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty())
    return fail_("Invalid prim path");
  const lightusd_status status = lightusd_stage_remove_prim(stage_, path_text.c_str());
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::removeAttribute(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty())
    return fail_("Invalid prim path or property name");
  const lightusd_status status = lightusd_attr_remove(
      stage_, path_text.c_str(), name_text.c_str());
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::setStringAttribute(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, const uint8_t* value, uint32_t value_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text, value_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty() ||
      !CopyCString(value, value_size, &value_text))
    return fail_("Invalid prim path, property name, or string value");
  const lightusd_status status = lightusd_attr_set(
      stage_, path_text.c_str(), name_text.c_str(), LIGHTUSD_TYPE_STRING, 0,
      value_text.c_str(), 1, LIGHTUSD_PROP_CUSTOM);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::setNumberAttribute(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, double value) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty())
    return fail_("Invalid prim path or property name");
  const lightusd_status status = lightusd_attr_set(
      stage_, path_text.c_str(), name_text.c_str(), LIGHTUSD_TYPE_DOUBLE, 0,
      &value, 1, LIGHTUSD_PROP_CUSTOM);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::setAttributeMetadata(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, const uint8_t* key, uint32_t key_size, uint8_t kind,
    const uint8_t* payload, uint32_t payload_size, double number,
    int32_t integer) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text, key_text, text_value;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty() ||
      !CopyCString(key, key_size, &key_text) || key_text.empty() ||
      (payload_size && !payload) || payload_size > kMaxLayerDocumentBytes)
    return fail_("Invalid attribute metadata path, name, key, or payload");
  lightusd_type type = LIGHTUSD_TYPE_INVALID;
  const void* data = nullptr;
  size_t count = 1;
  bool bool_value = false;
  switch (kind) {
    case 0:
      if (payload_size != 1 || payload[0] > 1)
        return fail_("Attribute metadata boolean must be 0 or 1");
      bool_value = payload[0] != 0;
      type = LIGHTUSD_TYPE_BOOL;
      data = &bool_value;
      break;
    case 1: case 2:
      if (!CopyCString(payload, payload_size, &text_value))
        return fail_("Attribute metadata text is invalid");
      type = kind == 1 ? LIGHTUSD_TYPE_TOKEN : LIGHTUSD_TYPE_STRING;
      data = text_value.c_str();
      break;
    case 3:
      if (payload_size != 0)
        return fail_("Attribute metadata integer must not have a byte payload");
      type = LIGHTUSD_TYPE_INT;
      data = &integer;
      break;
    case 4:
      if (payload_size != 0)
        return fail_("Attribute metadata double must not have a byte payload");
      type = LIGHTUSD_TYPE_DOUBLE;
      data = &number;
      break;
    default: return fail_("Unsupported attribute metadata value kind");
  }
  const lightusd_status status = lightusd_attr_set_metadata(stage_,
      path_text.c_str(), name_text.c_str(), key_text.c_str(), type, data, count);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::getAttributeMetadata(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, const uint8_t* key, uint32_t key_size,
    uint8_t* kind, uint8_t* out, uint32_t cap) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text, key_text;
  if (!kind || !CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty() ||
      !CopyCString(key, key_size, &key_text) || key_text.empty())
    return fail_("Invalid attribute metadata path, name, or key");
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage_, path_text.c_str());
  if (!lightusd_prim_is_valid(prim)) return fail_("Attribute metadata prim not found");
  lightusd_value* value = nullptr;
  const lightusd_status status =
      lightusd_attr_metadata(prim, name_text.c_str(), key_text.c_str(), &value);
  if (status != LIGHTUSD_OK || !value) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Attribute metadata query failed";
    if (value) lightusd_value_destroy(value);
    return -1;
  }
  lightusd_value_view view{};
  if (lightusd_value_get_view(value, &view) != LIGHTUSD_OK || view.is_array ||
      view.count != 1) {
    lightusd_value_destroy(value);
    return fail_("Attribute metadata value is not a supported scalar");
  }
  std::vector<uint8_t> bytes;
  uint8_t value_kind = 0;
  if (view.type == LIGHTUSD_TYPE_BOOL) {
    if (!view.data) {
      lightusd_value_destroy(value);
      return fail_("Attribute metadata boolean has no scalar data");
    }
    value_kind = 0;
    bytes.push_back(*static_cast<const bool*>(view.data) ? 1 : 0);
  } else if (view.type == LIGHTUSD_TYPE_TOKEN || view.type == LIGHTUSD_TYPE_STRING) {
    lightusd_sv text{};
    if (lightusd_value_get_string(value, &text) != LIGHTUSD_OK ||
        text.len > kMaxLayerDocumentBytes) {
      lightusd_value_destroy(value);
      return fail_("Attribute metadata string exceeds 512 MiB or is invalid");
    }
    value_kind = view.type == LIGHTUSD_TYPE_TOKEN ? 1 : 2;
    if (text.len) bytes.assign(text.data, text.data + text.len);
  } else if (view.type == LIGHTUSD_TYPE_INT) {
    if (!view.data) {
      lightusd_value_destroy(value);
      return fail_("Attribute metadata integer has no scalar data");
    }
    value_kind = 3;
    bytes.resize(sizeof(int32_t));
    std::memcpy(bytes.data(), view.data, bytes.size());
  } else if (view.type == LIGHTUSD_TYPE_DOUBLE) {
    if (!view.data) {
      lightusd_value_destroy(value);
      return fail_("Attribute metadata double has no scalar data");
    }
    value_kind = 4;
    bytes.resize(sizeof(double));
    std::memcpy(bytes.data(), view.data, bytes.size());
  } else {
    lightusd_value_destroy(value);
    return fail_("Attribute metadata value has an unsupported type");
  }
  lightusd_value_destroy(value);
  if (bytes.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return fail_("Attribute metadata result is too large");
  *kind = value_kind;
  if (!out || cap < bytes.size()) {
    error_.clear();
    return static_cast<int32_t>(bytes.size());
  }
  if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
  error_.clear();
  return static_cast<int32_t>(bytes.size());
}

int32_t NextLayerDocument::setStageMetadata(
    const uint8_t* key, uint32_t key_size, uint8_t kind,
    const uint8_t* text, uint32_t text_size, double number) {
  if (!ensureLoaded_()) return -1;
  std::string key_text, text_value;
  if (!CopyCString(key, key_size, &key_text) || key_text.empty() || kind > 3)
    return fail_("Invalid stage metadata key or kind");
  lightusd_type type = LIGHTUSD_TYPE_INVALID;
  const void* data = nullptr;
  size_t count = 1;
  switch (kind) {
    case 0: type = LIGHTUSD_TYPE_STRING; break;
    case 1: type = LIGHTUSD_TYPE_TOKEN; break;
    case 2: type = LIGHTUSD_TYPE_ASSET_PATH; break;
    case 3:
      type = LIGHTUSD_TYPE_DOUBLE;
      data = &number;
      break;
    default: return fail_("Invalid stage metadata kind");
  }
  if (kind < 3) {
    if (!CopyCString(text, text_size, &text_value))
      return fail_("Invalid stage metadata text");
    data = text_value.c_str();
  }
  const lightusd_status status = lightusd_stage_set_metadata(
      stage_, key_text.c_str(), type, data, count);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::setPrimMetadata(
    const uint8_t* path, uint32_t path_size, const uint8_t* key,
    uint32_t key_size, uint8_t kind, const uint8_t* payload,
    uint32_t payload_size, uint32_t count) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, key_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(key, key_size, &key_text) || key_text.empty() || kind > 3 ||
      (payload_size && !payload) || payload_size > kMaxLayerDocumentBytes)
    return fail_("Invalid prim metadata path, key, or payload");
  lightusd_status status = LIGHTUSD_ERR_INVALID_ARG;
  if (kind == 0) {
    if (payload_size != 1 || count != 1 || payload[0] > 1)
      return fail_("Prim metadata boolean payload must be 0 or 1");
    const bool value = payload[0] != 0;
    status = lightusd_prim_set_metadata(stage_, path_text.c_str(),
        key_text.c_str(), LIGHTUSD_TYPE_BOOL, &value, 1);
  } else if (kind == 1 || kind == 2) {
    std::string value;
    if (count != 1 || !CopyCString(payload, payload_size, &value))
      return fail_("Prim metadata string payload is invalid");
    status = lightusd_prim_set_metadata(stage_, path_text.c_str(),
        key_text.c_str(), kind == 1 ? LIGHTUSD_TYPE_TOKEN : LIGHTUSD_TYPE_STRING,
        value.c_str(), 1);
  } else {
    if (key_text != "apiSchemas" || count > kMaxRelationshipTargets ||
        count > payload_size / 4)
      return fail_("Invalid prim token-array metadata payload");
    std::vector<std::string> values;
    std::vector<const char*> items;
    values.reserve(count);
    items.reserve(count);
    size_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
      if (offset > payload_size || payload_size - offset < 4)
        return fail_("Truncated prim metadata token length");
      const uint32_t length = static_cast<uint32_t>(payload[offset]) |
          (static_cast<uint32_t>(payload[offset + 1]) << 8) |
          (static_cast<uint32_t>(payload[offset + 2]) << 16) |
          (static_cast<uint32_t>(payload[offset + 3]) << 24);
      offset += 4;
      if (length > payload_size - offset ||
          (length && std::memchr(payload + offset, 0, length) != nullptr))
        return fail_("Invalid prim metadata token");
      values.emplace_back(length
          ? reinterpret_cast<const char*>(payload + offset) : "", length);
      offset += length;
    }
    if (offset != payload_size)
      return fail_("Prim metadata token payload does not match count");
    for (const auto& value : values) items.push_back(value.c_str());
    status = lightusd_prim_set_metadata_token_array(stage_, path_text.c_str(),
        key_text.c_str(), items.data(), items.size());
  }
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::primMetadataIsAuthored(
    const uint8_t* path, uint32_t path_size, const uint8_t* key,
    uint32_t key_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, key_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(key, key_size, &key_text) || key_text.empty())
    return fail_("Invalid prim metadata path or key");
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage_, path_text.c_str());
  if (!lightusd_prim_is_valid(prim)) return fail_("Prim metadata path was not found");
  int authored = 0;
  const lightusd_status status =
      lightusd_prim_metadata_is_authored(prim, key_text.c_str(), &authored);
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Prim metadata authored-state query failed";
    return -1;
  }
  error_.clear();
  return authored ? 1 : 0;
}

int32_t NextLayerDocument::getPrimMetadata(
    const uint8_t* path, uint32_t path_size, const uint8_t* key,
    uint32_t key_size, uint8_t* kind, uint32_t* count, uint8_t* out,
    uint32_t cap) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, key_text;
  if (!kind || !count || !CopyCString(path, path_size, &path_text) ||
      path_text.empty() || !CopyCString(key, key_size, &key_text) ||
      key_text.empty()) return fail_("Invalid prim metadata path or key");
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage_, path_text.c_str());
  if (!lightusd_prim_is_valid(prim)) return fail_("Prim metadata path was not found");
  lightusd_value* value = nullptr;
  if (lightusd_prim_get_metadata(prim, key_text.c_str(), &value) != LIGHTUSD_OK ||
      !value) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Prim metadata query failed";
    if (value) lightusd_value_destroy(value);
    return -1;
  }
  uint8_t value_kind = 0;
  uint32_t value_count = 1;
  std::vector<uint8_t> bytes;
  lightusd_value_view view{};
  const lightusd_status view_status = lightusd_value_get_view(value, &view);
  if (view_status != LIGHTUSD_OK) {
    lightusd_value_destroy(value);
    return fail_("Prim metadata value has no typed view");
  }
  if (view.type == LIGHTUSD_TYPE_BOOL && !view.is_array && view.count == 1 &&
      view.data) {
    value_kind = 0;
    bytes.push_back(*static_cast<const bool*>(view.data) ? 1 : 0);
  } else if ((view.type == LIGHTUSD_TYPE_TOKEN ||
              view.type == LIGHTUSD_TYPE_STRING) &&
             !view.is_array && view.count == 1) {
    lightusd_sv text{};
    if (lightusd_value_get_string(value, &text) != LIGHTUSD_OK ||
        text.len > kMaxLayerDocumentBytes) {
      lightusd_value_destroy(value);
      return fail_("Prim metadata string exceeds 512 MiB or is invalid");
    }
    value_kind = view.type == LIGHTUSD_TYPE_TOKEN ? 1 : 2;
    if (text.len) bytes.assign(text.data, text.data + text.len);
  } else if (view.type == LIGHTUSD_TYPE_TOKEN && view.is_array) {
    lightusd_strlist* items = nullptr;
    if (lightusd_value_get_token_array(value, &items) != LIGHTUSD_OK || !items) {
      lightusd_value_destroy(value);
      return fail_("Prim metadata token array is invalid");
    }
    const size_t item_count = lightusd_strlist_size(items);
    if (item_count > kMaxRelationshipTargets) {
      lightusd_strlist_destroy(items);
      lightusd_value_destroy(value);
      return fail_("Prim metadata token array exceeds 65536 entries");
    }
    size_t total = 0;
    for (size_t i = 0; i < item_count; ++i) {
      const lightusd_sv item = lightusd_strlist_get(items, i);
      if (item.len > kMaxLayerDocumentBytes - 4 ||
          total > kMaxLayerDocumentBytes - 4 - item.len) {
        lightusd_strlist_destroy(items);
        lightusd_value_destroy(value);
        return fail_("Prim metadata token array exceeds 512 MiB");
      }
      total += 4 + item.len;
    }
    bytes.resize(total);
    size_t offset = 0;
    for (size_t i = 0; i < item_count; ++i) {
      const lightusd_sv item = lightusd_strlist_get(items, i);
      const uint32_t length = static_cast<uint32_t>(item.len);
      bytes[offset] = static_cast<uint8_t>(length);
      bytes[offset + 1] = static_cast<uint8_t>(length >> 8);
      bytes[offset + 2] = static_cast<uint8_t>(length >> 16);
      bytes[offset + 3] = static_cast<uint8_t>(length >> 24);
      offset += 4;
      if (length) std::memcpy(bytes.data() + offset, item.data, length);
      offset += length;
    }
    value_kind = 3;
    value_count = static_cast<uint32_t>(item_count);
    lightusd_strlist_destroy(items);
  } else {
    lightusd_value_destroy(value);
    return fail_("Prim metadata value has an unsupported type");
  }
  lightusd_value_destroy(value);
  if (bytes.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return fail_("Prim metadata result is too large");
  const int32_t required = static_cast<int32_t>(bytes.size());
  *kind = value_kind;
  *count = value_count;
  if (out && cap < bytes.size()) return fail_("Prim metadata output buffer is too small");
  if (out && !bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
  error_.clear();
  return required;
}

int32_t NextLayerDocument::getStageMetadataNumber(
    const uint8_t* key, uint32_t key_size, double* out) {
  if (!ensureLoaded_()) return -1;
  std::string key_text;
  if (!out || !CopyCString(key, key_size, &key_text) || key_text.empty())
    return fail_("Invalid stage metadata key or output");
  lightusd_value* value = nullptr;
  const lightusd_status status = lightusd_stage_get_metadata(
      stage_, key_text.c_str(), &value);
  if (status != LIGHTUSD_OK || !value) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Stage metadata query failed";
    if (value) lightusd_value_destroy(value);
    return -1;
  }
  lightusd_value_view view{};
  const lightusd_status view_status = lightusd_value_get_view(value, &view);
  if (view_status != LIGHTUSD_OK || view.type != LIGHTUSD_TYPE_DOUBLE ||
      view.is_array || !view.data || view.count != 1) {
    lightusd_value_destroy(value);
    return fail_("Stage metadata is not a numeric scalar");
  }
  *out = *static_cast<const double*>(view.data);
  lightusd_value_destroy(value);
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::getStageMetadataString(
    const uint8_t* key, uint32_t key_size, uint8_t* out, uint32_t cap) {
  if (!ensureLoaded_()) return -1;
  std::string key_text;
  if (!CopyCString(key, key_size, &key_text) || key_text.empty())
    return fail_("Invalid stage metadata key");
  lightusd_value* value = nullptr;
  const lightusd_status status = lightusd_stage_get_metadata(
      stage_, key_text.c_str(), &value);
  if (status != LIGHTUSD_OK || !value) {
    error_ = lightusd_last_error();
    if (error_.empty()) error_ = "Stage metadata query failed";
    if (value) lightusd_value_destroy(value);
    return -1;
  }
  lightusd_sv text{};
  const lightusd_status string_status = lightusd_value_get_string(value, &text);
  if (string_status != LIGHTUSD_OK || text.len > kMaxLayerDocumentBytes ||
      text.len > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    lightusd_value_destroy(value);
    return fail_("Stage metadata is not a supported string scalar");
  }
  const int32_t required = static_cast<int32_t>(text.len);
  if (out && cap >= text.len && text.len) std::memcpy(out, text.data, text.len);
  else if (out && cap < text.len) {
    lightusd_value_destroy(value);
    return fail_("Stage metadata output buffer is too small");
  }
  lightusd_value_destroy(value);
  error_.clear();
  return required;
}

int32_t NextLayerDocument::stageMetadataIsAuthored(const uint8_t* key,
                                                  uint32_t key_size) {
  if (!ensureLoaded_()) return -1;
  std::string key_text;
  if (!CopyCString(key, key_size, &key_text) || key_text.empty())
    return fail_("Invalid stage metadata key");
  const int authored = lightusd_stage_metadata_is_authored(
      stage_, key_text.c_str());
  error_.clear();
  return authored ? 1 : 0;
}

int32_t NextLayerDocument::setRelationshipTargets(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, const uint8_t* packed_targets, uint32_t packed_size,
    uint32_t count) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty() ||
      (packed_size && !packed_targets) || packed_size > kMaxLayerDocumentBytes ||
      count > kMaxRelationshipTargets)
    return fail_("Invalid relationship path, name, or target payload");
  std::vector<std::string> targets;
  std::vector<const char*> target_ptrs;
  if (count > packed_size / 4) return fail_("Relationship target count exceeds payload");
  targets.reserve(count);
  target_ptrs.reserve(count);
  size_t offset = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (offset > packed_size || packed_size - offset < 4)
      return fail_("Truncated relationship target length");
    const uint32_t length = static_cast<uint32_t>(packed_targets[offset]) |
        (static_cast<uint32_t>(packed_targets[offset + 1]) << 8) |
        (static_cast<uint32_t>(packed_targets[offset + 2]) << 16) |
        (static_cast<uint32_t>(packed_targets[offset + 3]) << 24);
    offset += 4;
    if (length > packed_size - offset ||
        (length && std::memchr(packed_targets + offset, 0, length) != nullptr))
      return fail_("Invalid relationship target string");
    targets.emplace_back(length
        ? reinterpret_cast<const char*>(packed_targets + offset) : "", length);
    offset += length;
  }
  if (offset != packed_size)
    return fail_("Relationship target payload does not match count");
  for (const auto& target : targets) target_ptrs.push_back(target.c_str());
  const lightusd_status status = lightusd_rel_set_targets(
      stage_, path_text.c_str(), name_text.c_str(), target_ptrs.data(),
      target_ptrs.size());
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::relationshipTargetCount(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty())
    return fail_("Invalid relationship path or name");
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage_, path_text.c_str());
  if (!lightusd_prim_is_valid(prim)) return fail_("Relationship prim not found");
  if (!lightusd_prim_has_relationship(prim, name_text.c_str()))
    return fail_("Relationship not found");
  const size_t count = lightusd_rel_target_count(prim, name_text.c_str());
  if (count > kMaxRelationshipTargets ||
      count > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return fail_("Relationship target count exceeds limit");
  error_.clear();
  return static_cast<int32_t>(count);
}

int32_t NextLayerDocument::relationshipTargetCopy(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, uint32_t index, uint8_t* out, uint32_t cap) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty())
    return fail_("Invalid relationship path or name");
  const lightusd_prim prim = lightusd_stage_prim_at_path(stage_, path_text.c_str());
  if (!lightusd_prim_is_valid(prim) ||
      !lightusd_prim_has_relationship(prim, name_text.c_str()))
    return fail_("Relationship not found");
  const size_t count = lightusd_rel_target_count(prim, name_text.c_str());
  if (index >= count) return fail_("Relationship target index out of range");
  const lightusd_sv target = lightusd_rel_target(prim, name_text.c_str(), index);
  if (target.len > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return fail_("Relationship target string exceeds limit");
  const int32_t required = static_cast<int32_t>(target.len);
  if (!out || cap < target.len) {
    error_.clear();
    return required;
  }
  if (target.len) std::memcpy(out, target.data, target.len);
  error_.clear();
  return required;
}

int32_t NextLayerDocument::removeRelationship(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty())
    return fail_("Invalid relationship path or name");
  const lightusd_status status = lightusd_rel_remove(
      stage_, path_text.c_str(), name_text.c_str());
  if (status != LIGHTUSD_OK) {
    error_ = lightusd_last_error();
    return -1;
  }
  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::setAttribute(
    const uint8_t* path, uint32_t path_size, const uint8_t* name,
    uint32_t name_size, const uint8_t* type_name, uint32_t type_size,
    uint8_t is_array, const uint8_t* data, uint32_t data_size,
    uint32_t count) {
  if (!ensureLoaded_()) return -1;
  std::string path_text, name_text, type_text;
  if (!CopyCString(path, path_size, &path_text) || path_text.empty() ||
      !CopyCString(name, name_size, &name_text) || name_text.empty() ||
      !CopyCString(type_name, type_size, &type_text) || type_text.empty() ||
      ((is_array & 0x80u) ? (is_array & ~0x87u) != 0 : is_array > 1)) {
    return fail_("Invalid prim path, property name, type, or array flag");
  }

  // Preserve the existing 0/1 ABI. The high bit opts into explicit custom
  // and uniform flags without adding or reordering exported parameters.
  const uint16_t flags = (is_array & 0x80u)
      ? static_cast<uint16_t>(((is_array & 2u) ? LIGHTUSD_PROP_CUSTOM : 0) |
                              ((is_array & 4u) ? LIGHTUSD_PROP_UNIFORM : 0))
      : static_cast<uint16_t>(LIGHTUSD_PROP_CUSTOM);
  is_array &= 1u;

  const lightusd_type type = lightusd_type_from_name(type_text.c_str());
  if (type == LIGHTUSD_TYPE_INVALID)
    return fail_("Unknown attribute type");
  if (type == LIGHTUSD_TYPE_STRING || type == LIGHTUSD_TYPE_TOKEN ||
      type == LIGHTUSD_TYPE_ASSET_PATH) {
    if (is_array) {
      if (count && !data) return fail_("String-family array data is null");
      std::vector<std::string> values;
      std::vector<const char*> items;
      values.reserve(count);
      items.reserve(count);
      size_t offset = 0;
      for (uint32_t i = 0; i < count; ++i) {
        if (offset > data_size || data_size - offset < 4)
          return fail_("Truncated string-family array length");
        const uint32_t length = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
        offset += 4;
        if (length > data_size - offset ||
            (length && std::memchr(data + offset, 0, length) != nullptr))
          return fail_("Invalid string-family array item");
        values.emplace_back(length
            ? reinterpret_cast<const char*>(data + offset) : "", length);
        offset += length;
      }
      if (offset != data_size)
        return fail_("String-family array size does not match count");
      for (const auto& value : values) items.push_back(value.c_str());
      const lightusd_status status = lightusd_attr_set_token_array(
          stage_, path_text.c_str(), name_text.c_str(), type, items.data(),
          items.size(), flags);
      if (status != LIGHTUSD_OK) {
        error_ = lightusd_last_error();
        return -1;
      }
      clearExport_();
      error_.clear();
      return 0;
    }
    if (count != 1) return fail_("String-family scalars require one value");
    std::string value;
    if (!CopyCString(data, data_size, &value))
      return fail_("Invalid string attribute value");
    const lightusd_status status = lightusd_attr_set(
        stage_, path_text.c_str(), name_text.c_str(), type, 0,
        value.c_str(), 1, flags);
    if (status != LIGHTUSD_OK) {
      error_ = lightusd_last_error();
      return -1;
    }
  } else {
    const size_t element_size = lightusd_type_size(type);
    const size_t storage_size = is_array && IsHalfType(type)
        ? lightusd_type_component_count(type) * sizeof(float) : element_size;
    if (element_size == 0 || (!is_array && count == 0) ||
        static_cast<size_t>(count) >
            (static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) /
             storage_size) ||
        data_size != storage_size * static_cast<size_t>(count) || !data) {
      return fail_("Typed attribute data size does not match type and count");
    }
    if (!is_array && count != 1)
      return fail_("Scalar typed attributes require one value");
    const lightusd_status status = lightusd_attr_set(
        stage_, path_text.c_str(), name_text.c_str(), type, is_array,
        data, count, flags);
    if (status != LIGHTUSD_OK) {
      error_ = lightusd_last_error();
      return -1;
    }
  }

  clearExport_();
  error_.clear();
  return 0;
}

int32_t NextLayerDocument::primCount() const {
  if (!stage_) return 0;
  const size_t count = lightusd_stage_prim_count(stage_);
  return count > static_cast<size_t>((std::numeric_limits<int32_t>::max)())
      ? -1 : static_cast<int32_t>(count);
}

int32_t NextLayerDocument::errorSize() const {
  if (error_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)()))
    return -1;
  return static_cast<int32_t>(error_.size());
}

int32_t NextLayerDocument::errorCopy(uint8_t* out, uint32_t cap) const {
  if (error_.size() > cap || (error_.size() && !out)) return -1;
  if (!error_.empty()) std::memcpy(out, error_.data(), error_.size());
  return static_cast<int32_t>(error_.size());
}

int32_t NextLayerDocument::exportUsdaSize() {
  if (!ensureLoaded_()) return -1;
  if (!export_text_) {
    const lightusd_status status = lightusd_stage_export_usda(stage_, &export_text_);
    if (status != LIGHTUSD_OK) {
      error_ = lightusd_last_error();
      return -1;
    }
  }
  const lightusd_sv view = lightusd_string_view(export_text_);
  if (view.len > kMaxLayerDocumentBytes) {
    clearExport_();
    fail_("USDA output exceeds 512 MiB limit");
    return -1;
  }
  error_.clear();
  return static_cast<int32_t>(view.len);
}

uintptr_t NextLayerDocument::exportUsdaData() const {
  if (!export_text_) return 0;
  const lightusd_sv view = lightusd_string_view(export_text_);
  return reinterpret_cast<uintptr_t>(view.data);
}

int32_t NextLayerDocument::exportUsdcSize() {
  if (!ensureLoaded_()) return -1;
  if (!export_crate_) {
    const lightusd_status status = lightusd_stage_export_usdc(stage_, &export_crate_);
    if (status != LIGHTUSD_OK) {
      error_ = lightusd_last_error();
      return -1;
    }
  }
  const lightusd_sv view = lightusd_string_view(export_crate_);
  if (view.len > kMaxLayerDocumentBytes) {
    clearExport_();
    fail_("USDC output exceeds 512 MiB limit");
    return -1;
  }
  error_.clear();
  return static_cast<int32_t>(view.len);
}

uintptr_t NextLayerDocument::exportUsdcData() const {
  if (!export_crate_) return 0;
  const lightusd_sv view = lightusd_string_view(export_crate_);
  return reinterpret_cast<uintptr_t>(view.data);
}

int32_t NextLayerDocument::exportUsdz(NextAssetStore* asset_store,
                                      uint8_t root_format) {
  if (!ensureLoaded_()) return -1;
  if (root_format > 1) return fail_("Invalid USDZ root layer format");
  clearExport_();
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  if (!stage) return fail_("No layer is loaded");
  constexpr size_t kPayloadLimit = size_t{1} << 29;
  constexpr size_t kWorkingLimit = size_t{1} << 30;
  size_t baseline_bytes = stage->GetMemoryUsage();
  if (baseline_bytes >= kWorkingLimit)
    return fail_("USDZ export working set exceeds 1 GiB limit");
  if (asset_store) {
    const uint64_t store_payload = asset_store->memoryBytes();
    const uint64_t store_metadata = asset_store->metadataBytes();
    if (store_payload > kWorkingLimit || store_metadata > kWorkingLimit ||
        static_cast<size_t>(store_payload) > kWorkingLimit - baseline_bytes ||
        static_cast<size_t>(store_metadata) >
            kWorkingLimit - baseline_bytes - static_cast<size_t>(store_payload))
      return fail_("USDZ export working set exceeds 1 GiB limit");
    baseline_bytes += static_cast<size_t>(store_payload) +
                      static_cast<size_t>(store_metadata);
  }
  if (baseline_bytes >= kWorkingLimit)
    return fail_("USDZ export working set exceeds 1 GiB limit");
  const size_t remaining_working_bytes = kWorkingLimit - baseline_bytes;
  std::map<std::string, std::vector<uint8_t>> assets;
  size_t asset_bytes = 0;
  if (asset_store) {
    const int count = asset_store->identifierCount();
    if (count < 0 || count > 65536) return fail_("USDZ asset count exceeds limit");
    for (int i = 0; i < count; ++i) {
      const int name_size = asset_store->identifierCopy(i, nullptr, 0);
      if (name_size <= 0 || static_cast<size_t>(name_size) > kPayloadLimit - asset_bytes)
        return fail_("USDZ asset identifiers exceed 512 MiB limit");
      if (static_cast<size_t>(name_size) >
          remaining_working_bytes - std::min(asset_bytes, remaining_working_bytes))
        return fail_("USDZ package asset copies exceed remaining memory budget");
      std::string name(static_cast<size_t>(name_size), '\0');
      if (asset_store->identifierCopy(i, reinterpret_cast<uint8_t*>(name.data()),
                                      static_cast<uint32_t>(name.size())) != name_size)
        return fail_("USDZ asset identifier changed during export");
      asset_bytes += name.size();
      const uint8_t* data = nullptr;
      uint32_t size = 0;
      if (asset_store->borrowedAssetView(
              reinterpret_cast<const uint8_t*>(name.data()),
              static_cast<uint32_t>(name.size()), &data, &size) != 1 ||
          (size && !data) || static_cast<size_t>(size) > kPayloadLimit - asset_bytes)
          return fail_("USDZ assets exceed 512 MiB limit or are unavailable");
      if (static_cast<size_t>(size) >
          remaining_working_bytes - std::min(asset_bytes, remaining_working_bytes))
        return fail_("USDZ package asset copies exceed remaining memory budget");
      asset_bytes += size;
      std::vector<uint8_t> bytes;
      if (size) bytes.assign(data, data + size);
      assets.emplace(std::move(name), std::move(bytes));
    }
  }
  tn::USDZWriteOptions options;
  options.max_file_size_bytes = kPayloadLimit;
  options.max_memory_bytes = remaining_working_bytes;
  tn::USDZWriteResult result;
  if (root_format == 0) {
    tn::USDAWriteOptions usda_options;
    usda_options.emit_custom = true;
    const std::string root = tn::WriteUSDAToString(*stage, usda_options);
    if (root.empty()) return fail_("USDA root serialization failed");
    result = tn::WriteUSDZFromUSDAAndAssetsToMemory(
        export_usdz_, reinterpret_cast<const uint8_t*>(root.data()), root.size(),
        assets, options);
  } else {
    std::vector<uint8_t> root;
    tn::USDCWriteOptions write_options;
    const size_t package_asset_bytes = asset_bytes;
    write_options.crate_options.max_memory_bytes =
        static_cast<uint64_t>(package_asset_bytes) > options.max_memory_bytes
            ? 0
            : options.max_memory_bytes -
                  static_cast<uint64_t>(package_asset_bytes);
    const tn::USDCWriteResult crate =
        tn::WriteUSDCToMemory(root, *stage, write_options);
    if (!crate.success) {
      error_ = crate.error.empty() ? "USDC root serialization failed" : crate.error;
      return -1;
    }
    result = tn::WriteUSDZFromUSDCAndAssetsToMemory(
        export_usdz_, root.data(), root.size(), assets, options);
  }
  if (!result.success) {
    export_usdz_.clear();
    error_ = result.error.empty() ? "USDZ export failed" : result.error;
    return -1;
  }
  if (export_usdz_.size() > kPayloadLimit ||
      export_usdz_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    export_usdz_.clear();
    return fail_("USDZ output exceeds 512 MiB limit");
  }
  error_.clear();
  return static_cast<int32_t>(export_usdz_.size());
}

int32_t NextLayerDocument::exportUsdzSize() const {
  return export_usdz_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())
      ? -1 : static_cast<int32_t>(export_usdz_.size());
}

uintptr_t NextLayerDocument::exportUsdzData() const {
  return export_usdz_.empty() ? 0 : reinterpret_cast<uintptr_t>(export_usdz_.data());
}

}  // namespace lightusd::web_next
