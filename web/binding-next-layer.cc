// SPDX-License-Identifier: Apache-2.0
// Editable single-layer document operations implemented through lightusd_c.
#include "binding-next-layer.hh"
#include "binding-next-scene.hh"
#include "binding-next-assets.hh"
#include "c-stage-bridge.hh"
#include "next/composition/composition.hh"
#include "next/layer/prim-spec.hh"
#include "next/layer/arc-reference.hh"
#include "next/stage/stage.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "next/writer/value-printer.hh"
#include "next/layer/property-index.hh"
#include "next/parser/ascii-parser.hh"
#include "next/parser/lexer.hh"
#include "next/parser/value-parser.hh"
#include "security-policy.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace tn = lightusd::next;

namespace lightusd::web_next {
namespace {
constexpr uint32_t kMaxLayerDocumentBytes = uint32_t{1} << 29;
constexpr uint32_t kMaxRelationshipTargets = uint32_t{1} << 16;
constexpr size_t kMaxLayerJSONBytes = size_t{1} << 29;

// Legacy JSONToLayer parses these optional transport tables even though its
// PrimSpec importer consumes canonical USD value text rather than accessors.
// Validate them before accepting the same document, without allocating a
// second decoded copy of table bytes that no Layer property will read.
bool ValidateLayerJSONTables(const minijson::Value& root, std::string* error) {
  const auto fail = [error](const char* message) {
    if (error) *error = message;
    return false;
  };
  const auto size_field = [](const minijson::Value& value) {
    size_t unused = 0;
    return value.as_size_t(&unused);
  };
  const minijson::Value& buffers = root["buffers"];
  if (root.contains("buffers")) {
    if (!buffers.is_array()) return fail("Buffers must be an array");
    constexpr std::string_view kPrefix =
        "data:application/octet-stream;base64,";
    for (const minijson::Value& buffer : buffers) {
      if (!buffer.is_object() || !buffer.contains("byteLength") ||
          !buffer.contains("uri")) return fail("Invalid buffer object");
      if (!size_field(buffer["byteLength"]))
        return fail("buffer.byteLength must be a non-negative integer");
      if (!buffer["uri"].is_string())
        return fail("buffer.uri must be a string");
      const std::string& uri = buffer["uri"].get_string();
      if (uri.compare(0, kPrefix.size(), kPrefix) != 0)
        return fail("External buffer URI requires a JSON asset resolver");
      const size_t encoded_size = uri.size() - kPrefix.size();
      size_t byte_length = 0;
      buffer["byteLength"].as_size_t(&byte_length);
      if (encoded_size > security_policy::kJSONMaxBase64InputChars)
        return fail("Embedded base64 buffer is too large");
      if (byte_length > security_policy::kJSONMaxDecodedBytes)
        return fail("Embedded buffer byteLength exceeds limit");
      if (encoded_size % 4 != 0)
        return fail("Invalid base64 buffer encoding");
      const std::string_view encoded(uri.data() + kPrefix.size(), encoded_size);
      const size_t padding = encoded_size && encoded.back() == '='
          ? (encoded_size > 1 && encoded[encoded_size - 2] == '=' ? 2 : 1) : 0;
      const size_t decoded_size = (encoded_size / 4) * 3 - padding;
      if (decoded_size != byte_length) return fail("Buffer size mismatch");
      for (size_t i = 0; i < encoded_size; ++i) {
        const char c = encoded[i];
        const bool alphabet = (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '/';
        if (!alphabet && !(c == '=' && i >= encoded_size - padding))
          return fail("Invalid base64 buffer encoding");
      }
    }
  }
  const minijson::Value& views = root["bufferViews"];
  if (root.contains("bufferViews")) {
    if (!views.is_array()) return fail("BufferViews must be an array");
    for (const minijson::Value& view : views) {
      if (!view.is_object() || !view.contains("buffer") ||
          !view.contains("byteOffset") || !view.contains("byteLength"))
        return fail("Invalid bufferView object");
      for (const char* key : {"buffer", "byteOffset", "byteLength"}) {
        if (!size_field(view[key]))
          return fail("Invalid bufferView non-negative integer field");
      }
      if (view.contains("byteStride") && !size_field(view["byteStride"]))
        return fail("Invalid bufferView byteStride");
    }
  }
  const minijson::Value& accessors = root["accessors"];
  if (root.contains("accessors")) {
    if (!accessors.is_array()) return fail("Accessors must be an array");
    for (const minijson::Value& accessor : accessors) {
      if (!accessor.is_object() || !accessor.contains("bufferView") ||
          !accessor.contains("componentType") || !accessor.contains("count") ||
          !accessor.contains("type")) return fail("Invalid accessor object");
      if (!size_field(accessor["bufferView"]) ||
          !size_field(accessor["count"]) ||
          (accessor.contains("byteOffset") &&
           !size_field(accessor["byteOffset"])))
        return fail("Invalid accessor non-negative integer field");
      if (!accessor["componentType"].is_string() ||
          !accessor["type"].is_string())
        return fail("accessor.componentType and accessor.type must be strings");
    }
  }
  return true;
}

minijson::Value LayerMetadataValueJSON(const tn::Value& value);
size_t MetadataValueEntryCount(const tn::Value& value);

// A crate-backed array has not paid for its decoded buffer in the retained
// Layer estimate. Charge its minimum materialized storage before any accessor
// or value printer can trigger decoding. The 16x weight matches the exporter's
// 32 MiB retained-input gate against its 512 MiB output/working-set ceiling.
size_t LazyDecodedBytes(const tn::Value& value) {
  size_t bytes = 0;
  std::vector<const tn::Value*> pending{&value};
  while (!pending.empty()) {
    const tn::Value* current = pending.back();
    pending.pop_back();
    if (current->is_lazy()) {
      size_t stride = tn::GetTypeSize(current->type_id());
      if (!stride) stride = sizeof(std::string);  // string-family arrays
      const size_t count = current->array_size();
      if (count > (kMaxLayerJSONBytes / 16 - bytes) / stride) return SIZE_MAX;
      bytes += count * stride;
    }
    if (const tn::Dict* dict = current->as_dictionary()) {
      for (const auto& entry : dict->entries()) pending.push_back(&entry.second);
    }
  }
  return bytes;
}

bool ChargeLayerJSONValue(const tn::Value& value, size_t elements,
                          size_t* estimate) {
  if (!estimate || *estimate > kMaxLayerJSONBytes || elements == SIZE_MAX)
    return false;
  const size_t strings = value.dynamic_string_memory_usage();
  const size_t lazy_bytes = LazyDecodedBytes(value);
  if (lazy_bytes == SIZE_MAX) return false;
  const size_t remaining = kMaxLayerJSONBytes - *estimate;
  if (strings > remaining || elements > (remaining - strings) / 128 ||
      lazy_bytes > (remaining - strings) / 16) return false;
  *estimate += strings + std::max(elements * 128, lazy_bytes * 16);
  return true;
}

// Legacy Layer JSON spells matrix values like pxr's GfMatrix stream output,
// `( (1, 0), (0, 1) )`; the next value printer emits the compact
// `((1, 0), (0, 1))`. Matrix text holds only numbers, so the rewrite is exact.
std::string LayerJSONValueText(const tn::Value& value) {
  std::string text = tn::PrintValue(value);
  switch (value.type_id()) {
    case tn::TypeId::Matrix2f: case tn::TypeId::Matrix2d:
    case tn::TypeId::Matrix3f: case tn::TypeId::Matrix3d:
    case tn::TypeId::Matrix4f: case tn::TypeId::Matrix4d: break;
    default: return text;
  }
  std::string spaced;
  spaced.reserve(text.size() + text.size() / 8);
  for (size_t i = 0; i < text.size(); ++i) {
    spaced.push_back(text[i]);
    if (i + 1 < text.size() && ((text[i] == '(' && text[i + 1] == '(') ||
                                (text[i] == ')' && text[i + 1] == ')')))
      spaced.push_back(' ');
  }
  return spaced;
}

minijson::Value LayerPrimJSON(const tn::Layer& layer, uint32_t index,
                              size_t* estimate, size_t prim_depth) {
  const tn::PrimSpec* prim = layer.prim(index);
  if (!prim || prim_depth > 1024) return minijson::Value();
  const auto charge = [estimate](size_t amount) {
    if (amount > kMaxLayerJSONBytes - *estimate) return false;
    *estimate += amount;
    return true;
  };
  const auto charge_text = [&charge](size_t bytes, size_t overhead = 0) {
    if (overhead > kMaxLayerJSONBytes ||
        bytes > (kMaxLayerJSONBytes - overhead) / 8) return false;
    return charge(bytes * 8 + overhead);
  };
  minijson::Value result = minijson::Value::object();
  if (!charge_text(prim->name().size()) ||
      !charge_text(prim->type_name().size(), 256))
    return minijson::Value();
  result["name"] = prim->name();
  result["typeName"] = prim->type_name();
  switch (prim->specifier()) {
    case tn::PrimSpecifier::Def: result["specifier"] = "def"; break;
    case tn::PrimSpecifier::Over: result["specifier"] = "over"; break;
    case tn::PrimSpecifier::Class: result["specifier"] = "class"; break;
  }
  const tn::PrimSpecMeta& prim_meta = prim->meta();
  const tn::ArcListOpEdits* arc_edits = prim_meta.arc_edits();
  const auto encode_path_ops = [&](const char* field,
                                   const std::vector<std::string>& paths,
                                   const tn::ArcEdit* edit,
                                   bool structured = false) {
    if ((!edit || !edit->authored) && paths.empty()) return true;
    minijson::Value operations = minijson::Value::array();
    const auto append = [&](const char* op,
                            const std::vector<std::string>& items) {
      minijson::Value record = minijson::Value::object();
      minijson::Value encoded_items = minijson::Value::array();
      for (const std::string& item : items) {
        if (structured) {
          const tn::CompositionArc arc = tn::Compositor::ParseReference(item);
          if (!charge_text(arc.asset_path.size() + arc.prim_path.size(), 96))
            return false;
          minijson::Value value = minijson::Value::object();
          value["assetPath"] = arc.asset_path;
          value["primPath"] = arc.prim_path.empty() ? "#INVALID#" : arc.prim_path;
          if (!arc.layer_offset.empty()) {
            double offset = 0.0, scale = 1.0;
            tn::Compositor::ParseLayerOffset(arc.layer_offset, offset, scale);
            if (offset != 0.0 || scale != 1.0) {
              value["offset"] = offset;
              value["scale"] = scale;
            }
          }
          const std::string custom_data = tn::ArcReferenceCustomData(item);
          if (!custom_data.empty()) {
            if (!charge_text(custom_data.size(), 128)) return false;
            tn::Lexer lexer(custom_data.data(), custom_data.size());
            tn::ParseResult parsed = tn::ParseValue(lexer, tn::TypeId::Dictionary);
            if (!parsed.success || lexer.peek().type != tn::TokenType::Eof ||
                !parsed.value.as_dictionary() ||
                !ChargeLayerJSONValue(parsed.value,
                    MetadataValueEntryCount(parsed.value), estimate)) return false;
            value["customData"] = LayerMetadataValueJSON(parsed.value);
          }
          encoded_items.push_back(std::move(value));
          continue;
        }
        const std::string path = item.size() >= 2 && item.front() == '<' &&
            item.back() == '>' ? item.substr(1, item.size() - 2) : item;
        if (!charge_text(path.size(), 32)) return false;
        encoded_items.push_back(path);
      }
      record["op"] = op;
      record["items"] = std::move(encoded_items);
      operations.push_back(std::move(record));
      return true;
    };
    if (!edit || !edit->authored || edit->is_explicit) {
      if (!append("", paths)) return false;
    } else {
      if ((!edit->added.empty() && !append("add", edit->added)) ||
          (!edit->prepended.empty() && !append("prepend", edit->prepended)) ||
          (!edit->appended.empty() && !append("append", edit->appended)) ||
          (!edit->deleted.empty() && !append("delete", edit->deleted)) ||
          (!edit->ordered.empty() && !append("order", edit->ordered))) return false;
    }
    result[field] = std::move(operations);
    return true;
  };
  if (!encode_path_ops("inherits", prim_meta.inherits,
                       arc_edits ? &arc_edits->inherits : nullptr) ||
      !encode_path_ops("specializes", prim_meta.specializes,
                       arc_edits ? &arc_edits->specializes : nullptr) ||
      !encode_path_ops("references", prim_meta.references,
                       arc_edits ? &arc_edits->references : nullptr, true) ||
      !encode_path_ops("payloads", prim_meta.payloads,
                       arc_edits ? &arc_edits->payloads : nullptr, true))
    return minijson::Value();
  const tn::StringListOpEdits& variant_edits = prim_meta.variantSetNameEdits();
  if (variant_edits.authored || !prim_meta.variantSets().empty()) {
    minijson::Value operations = minijson::Value::array();
    const auto append = [&](const char* op, const std::vector<std::string>& items) {
      minijson::Value record = minijson::Value::object();
      minijson::Value names = minijson::Value::array();
      for (const std::string& item : items) {
        if (!charge_text(item.size(), 32)) return false;
        names.push_back(item);
      }
      record["op"] = op;
      record["items"] = std::move(names);
      operations.push_back(std::move(record));
      return true;
    };
    if (variant_edits.authored && variant_edits.is_explicit) {
      if (!append("", variant_edits.explicit_items)) return minijson::Value();
    } else if (variant_edits.authored) {
      if ((!variant_edits.added.empty() && !append("add", variant_edits.added)) ||
          (!variant_edits.prepended.empty() && !append("prepend", variant_edits.prepended)) ||
          (!variant_edits.appended.empty() && !append("append", variant_edits.appended)) ||
          (!variant_edits.deleted.empty() && !append("delete", variant_edits.deleted)) ||
          (!variant_edits.ordered.empty() && !append("order", variant_edits.ordered)))
        return minijson::Value();
    } else {
      std::vector<std::string> names;
      for (const tn::VariantSetData& set : prim_meta.variantSets())
        names.push_back(set.name);
      if (!append("prepend", names)) return minijson::Value();
    }
    result["variantSets"] = std::move(operations);
  }
  if (prim_meta.variantSelectionsAuthored() ||
      !prim_meta.variantSelections().empty() ||
      !prim_meta.variantSelection.empty()) {
    minijson::Value selections = minijson::Value::object();
    for (const auto& selection : prim_meta.variantSelections()) {
      if (!charge_text(selection.first.size() + selection.second.size(), 64))
        return minijson::Value();
      selections[selection.first] = selection.second;
    }
    if (prim_meta.variantSelections().empty() &&
        !prim_meta.variantSelection.empty()) {
      const size_t equal = prim_meta.variantSelection.find('=');
      if (equal != std::string::npos) {
        const std::string name = prim_meta.variantSelection.substr(0, equal);
        const std::string selected = prim_meta.variantSelection.substr(equal + 1);
        if (!charge_text(name.size() + selected.size(), 64))
          return minijson::Value();
        selections[name] = selected;
      }
    }
    result["variants"] = std::move(selections);
  }
  minijson::Value properties = minijson::Value::object();
  const tn::PropNameTable& names = tn::GetPropNameTable();
  for (const tn::PropSlot& slot : prim->properties().slots()) {
    const std::string name(names.get(slot.name_id));
    const std::string* declared = prim->property_type_name(slot.name_id);
    const std::string type_name = declared ? *declared
        : tn::PrintTypeName(static_cast<tn::TypeId>(slot.value_type), slot.is_array());
    if (!charge_text(name.size()) || !charge_text(type_name.size(), 256))
      return minijson::Value();
    minijson::Value property = minijson::Value::object();
    minijson::Value attribute = minijson::Value::object();
    const tn::Value* property_value = prim->property_value(slot.name_id);
    // Array edits and undecodable extension defaults keep their canonical
    // USDA text instead of a Value; legacy exports that text as the value.
    const std::string* raw_default = property_value ? nullptr
        : prim->raw_default_source(slot.name_id);
    if (!slot.is_connection() && !property_value && !raw_default &&
        !prim->has_time_samples(slot.name_id)) {
      property["propertyType"] = "emptyAttribute";
      property["typeName"] = type_name;
      property["isCustom"] = slot.is_custom();
      property["listEditQual"] = "resetToExplicit";
      property["isAttribute"] = true;
      property["isRelationship"] = false;
      property["isEmpty"] = true;
      property["isAttributeConnection"] = false;
      properties[name] = std::move(property);
      continue;
    }
    attribute["name"] = (prim->has_time_samples(slot.name_id) ||
        (property_value && property_value->is_block())) ? name : "";
    attribute["typeName"] = type_name;
    attribute["variability"] = slot.is_uniform() ? "uniform" : "varying";
    attribute["isConnection"] = slot.is_connection();
    if (const tn::PropMeta* meta = prim->property_meta(slot.name_id)) {
      minijson::Value metadata = minijson::Value::object();
      if (meta->authored & tn::PropMeta::kInterpolation) {
        metadata["interpolation"] = tn::PrintValue(tn::Value(meta->interpolation));
        attribute["interpolation"] = meta->interpolation;
      }
      if (meta->authored & tn::PropMeta::kElementSize)
        metadata["elementSize"] = std::to_string(meta->elementSize);
      if (meta->authored & tn::PropMeta::kHidden)
        metadata["hidden"] = meta->hidden ? "1" : "0";
      if (meta->authored & tn::PropMeta::kComment) {
        if (!charge_text(meta->comment.size())) return minijson::Value();
        metadata["comment"] = tn::PrintValue(tn::Value(meta->comment));
      }
      if (meta->authored & tn::PropMeta::kWeight)
        metadata["weight"] = tn::PrintValue(tn::Value(meta->weight));
      const auto add_string_meta = [&](uint32_t flag, const char* key,
                                       const std::string& text) {
        if (!(meta->authored & flag)) return true;
        if (!charge_text(text.size())) return false;
        metadata[key] = tn::PrintValue(tn::Value(text));
        return true;
      };
      if (!add_string_meta(tn::PropMeta::kColorSpace, "colorSpace", meta->colorSpace))
        return minijson::Value();
      if (meta->authored & tn::PropMeta::kDisplayName) {
        if (!charge_text(meta->displayName.size())) return minijson::Value();
        metadata["displayName"] = tn::PrintValue(tn::Value(meta->displayName));
      }
      if (meta->authored & tn::PropMeta::kDisplayGroup) {
        if (!charge_text(meta->displayGroup.size())) return minijson::Value();
        metadata["displayGroup"] = tn::PrintValue(tn::Value(meta->displayGroup));
      }
      if (meta->authored & tn::PropMeta::kDoc) {
        if (!charge_text(meta->doc.size())) return minijson::Value();
        // Legacy's canonical metadata printer uses USDA triple quotes for
        // multiline documentation instead of escaping each newline.
        if (meta->doc.find('\n') != std::string::npos &&
            meta->doc.find("\"\"\"") == std::string::npos)
          metadata["documentation"] = "\"\"\"" + meta->doc + "\"\"\"";
        else
          metadata["documentation"] = tn::PrintValue(tn::Value(meta->doc));
      }
      if (!add_string_meta(tn::PropMeta::kRenderType, "renderType", meta->renderType) ||
          !add_string_meta(tn::PropMeta::kConnectability, "connectability", meta->connectability) ||
          !add_string_meta(tn::PropMeta::kOutputName, "outputName", meta->outputName) ||
          !add_string_meta(tn::PropMeta::kBindMaterialAs, "bindMaterialAs", meta->bindMaterialAs) ||
          !add_string_meta(tn::PropMeta::kKind, "kind", meta->kind) ||
          !add_string_meta(tn::PropMeta::kPermission, "permission", meta->permission))
        return minijson::Value();
      if (meta->authored & tn::PropMeta::kUnauthoredIdx)
        metadata["unauthoredValuesIndex"] =
            std::to_string(meta->unauthoredValuesIndex);
      if (meta->authored & tn::PropMeta::kAllowedTokens) {
        minijson::Value tokens = minijson::Value::array();
        for (const std::string& token : meta->allowedTokens) {
          if (!charge_text(token.size())) return minijson::Value();
          tokens.push_back(token);
        }
        metadata["allowedTokens"] = std::move(tokens);
      }
      const auto add_value_meta = [&](uint32_t flag, const char* key,
                                      const tn::Value& value) {
        if (!(meta->authored & flag)) return true;
        const size_t elements = MetadataValueEntryCount(value);
        if (!ChargeLayerJSONValue(value, elements, estimate)) return false;
        metadata[key] = LayerMetadataValueJSON(value);
        return true;
      };
      if (!add_value_meta(tn::PropMeta::kCustomData, "customData", meta->customData) ||
          !add_value_meta(tn::PropMeta::kAssetInfo, "assetInfo", meta->assetInfo) ||
          !add_value_meta(tn::PropMeta::kSdrMetadata, "sdrMetadata", meta->sdrMetadata))
        return minijson::Value();
      if (!metadata.empty()) attribute["metadata"] = std::move(metadata);
    }
    property["propertyType"] = slot.is_connection() ? "connection" : "attribute";
    property["isCustom"] = slot.is_custom();
    property["listEditQual"] = "resetToExplicit";
    property["isAttribute"] = true;
    property["isRelationship"] = false;
    property["isAttributeConnection"] = slot.is_connection();
    if (slot.is_connection()) property["valueTypeName"] = attribute["typeName"];
    const std::vector<tn::Path>* connections = prim->connection(name);
    property["isEmpty"] = !property_value && !raw_default &&
        !prim->has_time_samples(slot.name_id) &&
        (!connections || connections->empty());
    if (connections && !connections->empty()) {
      if (connections->size() == 1) {
        if (!charge_text(connections->front().str().size(), 8))
          return minijson::Value();
        attribute["connection"] = connections->front().str();
      }
      else {
        minijson::Value targets = minijson::Value::array();
        for (const tn::Path& target : *connections) {
          if (!charge_text(target.str().size(), 8)) return minijson::Value();
          targets.push_back(target.str());
        }
        attribute["connections"] = std::move(targets);
      }
    }
    const tn::Value* value = property_value;
    const auto* samples = prim->time_samples(slot.name_id);
    const bool has_samples = samples && !samples->empty();
    const bool blocked_value = value && value->is_block();
    const bool has_value = (value != nullptr && !blocked_value) ||
        raw_default || has_samples;
    attribute["hasValue"] = has_value;
    attribute["valueType"] = blocked_value ? "blocked"
        : (has_value ? "data" : "empty");
    if (value && !blocked_value) {
      const size_t elements = value->array_size();
      if (!ChargeLayerJSONValue(*value, elements, estimate))
        return minijson::Value();
      const std::string printed = LayerJSONValueText(*value);
      if (!charge_text(printed.size(), 8)) return minijson::Value();
      attribute["value"] = printed;
      attribute["valueTypeName"] = attribute["typeName"];
    } else if (raw_default) {
      if (!charge_text(raw_default->size(), 8)) return minijson::Value();
      attribute["value"] = *raw_default;
      attribute["valueTypeName"] = attribute["typeName"];
    } else {
      attribute["value"] = minijson::Value();
      if (has_samples) attribute["valueTypeName"] = attribute["typeName"];
    }
    attribute["hasTimeSamples"] = has_samples;
    if (has_samples) {
      minijson::Value encoded = minijson::Value::array();
      for (const auto& sample : *samples) {
        const tn::Value* sample_value = prim->time_sample_value(sample.second);
        if (!sample_value) return minijson::Value();
        const size_t elements = sample_value->array_size();
        if (!ChargeLayerJSONValue(*sample_value, elements, estimate))
          return minijson::Value();
        const std::string printed = LayerJSONValueText(*sample_value);
        if (!charge_text(printed.size(), 64)) return minijson::Value();
        minijson::Value item = minijson::Value::object();
        item["time"] = sample.first;
        const bool sample_blocked = sample_value->is_block();
        item["blocked"] = sample_blocked;
        item["value"] = sample_blocked ? minijson::Value() : minijson::Value(printed);
        encoded.push_back(std::move(item));
      }
      attribute["timeSamples"] = std::move(encoded);
    }
    property["attribute"] = std::move(attribute);
    properties[name] = std::move(property);
  }
  for (const std::string& name : prim->relationship_names()) {
    if (!charge_text(name.size(), 96)) return minijson::Value();
    minijson::Value property = minijson::Value::object();
    minijson::Value rel = minijson::Value::object();
    rel["type"] = "relationship";
    const auto* targets = prim->relationship(name);
    const auto edit_it = prim->relationship_edits().find(name);
    const tn::ArcEdit* edit = edit_it == prim->relationship_edits().end()
        ? nullptr : &edit_it->second;
    const std::vector<std::string>* edit_targets = nullptr;
    const char* list_qualifier = "resetToExplicit";
    if (edit && edit->authored && !edit->is_explicit) {
      const std::pair<const char*, const std::vector<std::string>*> candidates[] = {
          {"add", &edit->added}, {"prepend", &edit->prepended},
          {"append", &edit->appended}, {"delete", &edit->deleted},
          {"order", &edit->ordered}};
      for (const auto& candidate : candidates) {
        if (!candidate.second->empty()) {
          edit_targets = candidate.second;
          list_qualifier = candidate.first;
          break;
        }
      }
    }
    rel["listEditQual"] = list_qualifier;
    const bool explicit_empty = (!targets || targets->empty()) &&
        edit && edit->authored && edit->is_explicit;
    const bool blocked = explicit_empty && edit->is_value_block;
    const size_t target_count = edit_targets ? edit_targets->size()
        : (targets ? targets->size() : 0);
    const auto target_at = [&](size_t i) -> std::string {
      return edit_targets ? (*edit_targets)[i] : (*targets)[i].str();
    };
    if (blocked) {
      rel["valueType"] = "valueBlock";
      rel["hasTargets"] = false;
      rel["blocked"] = true;
    } else if (target_count == 0 && !explicit_empty) {
      rel["valueType"] = "defineOnly";
      rel["hasTargets"] = false;
    } else if (target_count == 1 && !edit_targets) {
      rel["valueType"] = "path";
      rel["hasTargets"] = true;
      rel["target"] = target_at(0);
      property["relationTarget"] = target_at(0);
      minijson::Value relation_targets = minijson::Value::array();
      relation_targets.push_back(target_at(0));
      property["relationTargets"] = std::move(relation_targets);
      if (!charge_text(target_at(0).size(), 8)) return minijson::Value();
    } else {
      rel["valueType"] = "pathVector";
      rel["hasTargets"] = true;
      rel["targets"] = minijson::Value::array();
      minijson::Value relation_targets = minijson::Value::array();
      for (size_t i = 0; i < target_count; ++i) {
        const std::string target = target_at(i);
        if (!charge_text(target.size(), 8)) return minijson::Value();
        rel["targets"].push_back(target);
        relation_targets.push_back(target);
      }
      rel["targetCount"] = static_cast<uint32_t>(target_count);
      if (target_count > 0) property["relationTarget"] = target_at(0);
      if (target_count > 0) property["relationTargets"] = std::move(relation_targets);
    }
    property["propertyType"] = target_count == 0 && !explicit_empty
        ? "noTargetsRelationship" : "relationship";
    property["relationship"] = std::move(rel);
    property["isCustom"] = false;
    property["listEditQual"] = list_qualifier;
    property["isAttribute"] = false;
    property["isRelationship"] = true;
    property["isEmpty"] = !explicit_empty && target_count == 0;
    property["isAttributeConnection"] = false;
    properties[name] = std::move(property);
  }
  if (!properties.empty()) result["properties"] = std::move(properties);
  const std::vector<const tn::PrimSpec*> children = layer.children(index);
  if (!children.empty()) {
    minijson::Value child_json = minijson::Value::object();
    for (const tn::PrimSpec* child : children) {
      if (!child) return minijson::Value();
      const uint32_t child_index = layer.index_at_path(child->path().str());
      if (child_index == UINT32_MAX) return minijson::Value();
      minijson::Value value = LayerPrimJSON(layer, child_index, estimate,
                                            prim_depth + 1);
      if (value.is_null()) return minijson::Value();
      child_json[child->name()] = std::move(value);
    }
    result["children"] = std::move(child_json);
  }
  return result;
}

bool ParseLayerJSONValue(const minijson::Value& encoded,
                         const std::string& type_name, tn::Value* out,
                         bool* is_array, std::string* error) {
  if (!encoded.is_string() || !out || !is_array) {
    if (error) *error = "Layer JSON attribute value must be canonical USD text";
    return false;
  }
  bool array = false;
  const tn::TypeId type = tn::ParseTypeName(type_name, array);
  if (type == tn::TypeId::Invalid) {
    if (error) *error = "Unsupported Layer JSON attribute type: " + type_name;
    return false;
  }
  const std::string text = encoded.get_string();
  tn::Lexer lexer(text.data(), text.size());
  tn::ParseResult parsed = array ? tn::ParseArrayValue(lexer, type)
                                 : tn::ParseValue(lexer, type);
  if (!parsed.success || lexer.peek().type != tn::TokenType::Eof) {
    if (error) *error = parsed.error.empty()
        ? "Trailing tokens in Layer JSON attribute value" : parsed.error;
    return false;
  }
  *out = std::move(parsed.value);
  *is_array = array;
  return true;
}

bool FormatJSONMetadataUSDValue(const minijson::Value& encoded, bool tuple,
                                std::string* out) {
  if (!out) return false;
  if (encoded.is_array()) {
    out->push_back(tuple ? '(' : '[');
    bool first = true;
    for (const minijson::Value& item : encoded) {
      if (!first) out->append(", ");
      first = false;
      if (!FormatJSONMetadataUSDValue(item, tuple, out)) return false;
    }
    out->push_back(tuple ? ')' : ']');
    return true;
  }
  if (encoded.is_object()) return false;
  out->append(encoded.dump());
  return true;
}

bool ParseLayerJSONMetadataValue(const minijson::Value& encoded,
                                tn::Value* out, size_t depth,
                                size_t* value_count) {
  if (!out || !value_count || depth > 64 || ++*value_count > 1000000)
    return false;
  if (encoded.is_boolean()) {
    *out = tn::Value(encoded.get_bool());
    return true;
  }
  if (encoded.is_number_integer()) {
    if (encoded.is_number_unsigned())
      *out = tn::Value(encoded.get_uint64());
    else
      *out = tn::Value(encoded.get_int64());
    return true;
  }
  if (encoded.is_number()) {
    *out = tn::Value(encoded.get_double());
    return true;
  }
  if (encoded.is_string()) {
    *out = tn::Value(encoded.get_string());
    return true;
  }
  if (encoded.is_object() && encoded["type"].is_string() &&
      encoded.contains("value")) {
    const std::string type_name = encoded["type"].get_string();
    bool is_array = false;
    const tn::TypeId type = tn::ParseTypeName(type_name, is_array);
    if (type == tn::TypeId::Invalid) return false;
    // Legacy typed asset metadata uses an object payload. The next Value
    // stores the authored path; resolvedPath is a transient resolver hint.
    if (type == tn::TypeId::AssetPath && !is_array) {
      const minijson::Value& asset = encoded["value"];
      if (!asset.is_object() ||
          (asset.contains("assetPath") && !asset["assetPath"].is_string()) ||
          (asset.contains("resolvedPath") && !asset["resolvedPath"].is_string()))
        return false;
      *out = tn::Value::MakeAssetPath(asset["assetPath"].is_string()
          ? asset["assetPath"].get_string() : std::string());
      return true;
    }
    if (type == tn::TypeId::Dictionary && !is_array) {
      const minijson::Value& members = encoded["value"];
      if (!members.is_object()) return false;
      tn::Dict dict;
      const auto* items = members.object_items();
      if (!items) return false;
      for (const auto& member : *items) {
        if (!member.value().is_object() ||
            !member.value()["type"].is_string() ||
            !member.value().contains("value")) return false;
        tn::Value child;
        if (!ParseLayerJSONMetadataValue(member.value(), &child, depth + 1,
                                         value_count)) return false;
        dict.set(member.key, std::move(child));
      }
      dict.set_typed_json_wrapper(true);
      *out = tn::Value::MakeDictionary(std::move(dict));
      return true;
    }
    std::string text;
    const bool compound = tn::GetComponentCount(type) > 1;
    if (is_array) {
      const minijson::Value& items = encoded["value"];
      if (!items.is_array()) return false;
      text.push_back('[');
      bool first = true;
      for (const minijson::Value& item : items) {
        if (!first) text.append(", ");
        first = false;
        if (!FormatJSONMetadataUSDValue(item, compound, &text)) return false;
      }
      text.push_back(']');
    } else if (!FormatJSONMetadataUSDValue(encoded["value"], compound, &text)) {
      return false;
    }
    tn::Lexer lexer(text.data(), text.size());
    tn::ParseResult parsed = is_array ? tn::ParseArrayValue(lexer, type)
                                      : tn::ParseValue(lexer, type);
    if (!parsed.success || lexer.peek().type != tn::TokenType::Eof) return false;
    *out = std::move(parsed.value);
    return true;
  }
  if (!encoded.is_object()) return false;
  tn::Dict dict;
  const auto* items = encoded.object_items();
  if (!items) return false;
  for (const auto& item : *items) {
    tn::Value value;
    if (!ParseLayerJSONMetadataValue(item.value(), &value, depth + 1,
                                     value_count)) return false;
    dict.set(item.key, std::move(value));
  }
  *out = tn::Value::MakeDictionary(std::move(dict));
  return true;
}

minijson::Value LayerMetadataPayloadJSON(const tn::Value& value) {
  minijson::Value flat = NextValueJSON(value);
  if (flat.is_null() && !value.is_array()) {
    if (value.type_id() == tn::TypeId::Half) {
      float number = 0.0f;
      if (value.to_float(&number))
        return std::isfinite(number) ? minijson::Value(number)
                                     : minijson::Value();
    }
    const float* floats = nullptr;
    const double* doubles = nullptr;
    const int32_t* ints = nullptr;
    float half_lanes[4] = {};
    switch (value.type_id()) {
      case tn::TypeId::Int2: ints = value.as_int2(); break;
      case tn::TypeId::Int3: ints = value.as_int3(); break;
      case tn::TypeId::Int4: ints = value.as_int4(); break;
      case tn::TypeId::Half2:
      case tn::TypeId::Texcoord2h:
        if (value.to_float2(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Half3:
      case tn::TypeId::Point3h:
      case tn::TypeId::Vector3h:
      case tn::TypeId::Normal3h:
      case tn::TypeId::Color3h:
      case tn::TypeId::Texcoord3h:
        if (value.to_float3(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Half4:
      case tn::TypeId::Color4h:
      case tn::TypeId::Quath:
        if (value.to_float4(half_lanes)) floats = half_lanes;
        break;
      case tn::TypeId::Matrix2f: floats = value.as_matrix2f(); break;
      case tn::TypeId::Matrix3f: floats = value.as_matrix3f(); break;
      case tn::TypeId::Matrix2d: doubles = value.as_matrix2d(); break;
      case tn::TypeId::Matrix3d: doubles = value.as_matrix3d(); break;
      default: break;
    }
    if (floats || doubles || ints) {
      flat = minijson::Value::array();
      for (size_t i = 0; i < tn::GetComponentCount(value.type_id()); ++i) {
        if (ints) {
          flat.push_back(ints[i]);
        } else {
          const double number = floats ? floats[i] : doubles[i];
          flat.push_back(std::isfinite(number)
              ? minijson::Value(number) : minijson::Value());
        }
      }
    }
  }
  if (!flat.is_array()) return flat;
  const size_t components = tn::GetComponentCount(value.type_id());
  if (components <= 1) return flat;
  const size_t count = value.is_array() ? value.array_size() : 1;
  if (count > flat.size() / components ||
      count * components != flat.size()) return flat;
  size_t matrix_width = 0;
  switch (value.type_id()) {
    case tn::TypeId::Matrix2f: case tn::TypeId::Matrix2d:
      matrix_width = 2; break;
    case tn::TypeId::Matrix3f: case tn::TypeId::Matrix3d:
      matrix_width = 3; break;
    case tn::TypeId::Matrix4f: case tn::TypeId::Matrix4d:
    case tn::TypeId::Frame4d:
      matrix_width = 4; break;
    default: break;
  }
  if (!value.is_array() && !matrix_width) return flat;
  minijson::Value grouped = minijson::Value::array();
  for (size_t item = 0; item < count; ++item) {
    minijson::Value element = minijson::Value::array();
    if (matrix_width) {
      for (size_t row = 0; row < matrix_width; ++row) {
        minijson::Value cells = minijson::Value::array();
        for (size_t column = 0; column < matrix_width; ++column)
          cells.push_back(flat[item * components + row * matrix_width + column]);
        element.push_back(std::move(cells));
      }
    } else {
      for (size_t component = 0; component < components; ++component)
        element.push_back(flat[item * components + component]);
    }
    grouped.push_back(std::move(element));
  }
  return value.is_array() ? grouped : grouped[0];
}

minijson::Value LayerTypedMetadataValueJSON(const tn::Value& value) {
  minijson::Value out = minijson::Value::object();
  out["type"] = tn::PrintTypeName(value.type_id(), value.is_array());
  if (const tn::Dict* dict = value.as_dictionary()) {
    minijson::Value members = minijson::Value::object();
    for (const auto& entry : dict->entries())
      members[entry.first] = LayerTypedMetadataValueJSON(entry.second);
    out["value"] = std::move(members);
  } else if (const std::string* asset = value.as_asset_path()) {
    minijson::Value path = minijson::Value::object();
    path["assetPath"] = *asset;
    path["resolvedPath"] = "";
    out["value"] = std::move(path);
  } else {
    out["value"] = LayerMetadataPayloadJSON(value);
  }
  return out;
}

minijson::Value LayerMetadataValueJSON(const tn::Value& value) {
  if (const tn::Dict* dictionary = value.as_dictionary()) {
    if (dictionary->typed_json_wrapper())
      return LayerTypedMetadataValueJSON(value);
    minijson::Value out = minijson::Value::object();
    for (const auto& entry : dictionary->entries())
      out[entry.first] = LayerMetadataValueJSON(entry.second);
    return out;
  }
  if (value.is_array() || tn::GetComponentCount(value.type_id()) > 1 ||
      value.type_id() == tn::TypeId::TimeCode ||
      value.type_id() == tn::TypeId::Half) {
    minijson::Value out = minijson::Value::object();
    out["type"] = tn::PrintTypeName(value.type_id(), value.is_array());
    out["value"] = LayerMetadataPayloadJSON(value);
    return out;
  }
  return NextValueJSON(value);
}

size_t MetadataValueEntryCount(const tn::Value& value) {
  size_t count = 1;
  std::vector<const tn::Value*> pending{&value};
  while (!pending.empty()) {
    const tn::Value* current = pending.back();
    pending.pop_back();
    if (const tn::Dict* dict = current->as_dictionary()) {
      if (dict->size() > 1000000 - count) return SIZE_MAX;
      count += dict->size();
      for (const auto& item : dict->entries()) pending.push_back(&item.second);
    } else if (current->is_array()) {
      if (current->array_size() > 1000000 - count) return SIZE_MAX;
      count += current->array_size();
    }
  }
  return count;
}

bool ParseLayerJSONPrim(const minijson::Value& encoded, tn::LayerBuilder* builder,
                        size_t depth, size_t* prim_count, size_t prim_limit,
                        std::string* error) {
  if (!encoded.is_object() || !builder || depth > 1024 || !prim_count ||
      ++*prim_count > prim_limit) {
    if (error) *error = "Invalid Layer JSON prim or nesting/count limit exceeded";
    return false;
  }
  if (!encoded["name"].is_string() || !encoded["typeName"].is_string() ||
      !encoded["specifier"].is_string()) {
    if (error) *error = "Layer JSON prim requires name, typeName and specifier";
    return false;
  }
  const std::string name = encoded["name"].get_string();
  const std::string type_name = encoded["typeName"].get_string();
  const std::string specifier_text = encoded["specifier"].get_string();
  tn::PrimSpecifier specifier;
  if (specifier_text == "def") specifier = tn::PrimSpecifier::Def;
  else if (specifier_text == "over") specifier = tn::PrimSpecifier::Over;
  else if (specifier_text == "class") specifier = tn::PrimSpecifier::Class;
  else {
    if (error) *error = "Invalid Layer JSON prim specifier";
    return false;
  }
  if (name.empty() || builder->begin_prim(name, type_name, specifier) == UINT32_MAX) {
    if (error) *error = "Invalid Layer JSON prim name or hierarchy";
    return false;
  }
  tn::PrimSpec* prim = builder->current();
  const auto parse_path_ops = [&](const char* field,
                                  std::vector<std::string>* effective,
                                  tn::ArcEdit* edit,
                                  bool structured = false) {
    const minijson::Value& operations = encoded[field];
    if (operations.is_null()) return true;
    if (!effective || !edit || !operations.is_array() || operations.size() == 0) {
      if (error) *error = std::string("Invalid Layer JSON ") + field + " list edits";
      return false;
    }
    for (const minijson::Value& operation : operations) {
      if (!operation.is_object() || !operation["op"].is_string() ||
          !operation["items"].is_array()) {
        if (error) *error = std::string("Invalid Layer JSON ") + field + " operation";
        return false;
      }
      const std::string qualifier = operation["op"].get_string();
      std::vector<std::string> items;
      for (const minijson::Value& encoded_path : operation["items"]) {
        if (structured) {
          if (!encoded_path.is_object() ||
              !encoded_path["assetPath"].is_string() ||
              !encoded_path["primPath"].is_string()) {
            if (error) *error = std::string("Unsupported Layer JSON ") + field + " target";
            return false;
          }
          const std::string asset = encoded_path["assetPath"].get_string();
          const std::string encoded_prim = encoded_path["primPath"].get_string();
          const std::string path = encoded_prim == "#INVALID#" ? "" : encoded_prim;
          if ((asset.empty() && path.empty()) || asset.find('@') != std::string::npos ||
              (!path.empty() && !tn::Path(path).is_valid())) {
            if (error) *error = std::string("Invalid Layer JSON ") + field + " target";
            return false;
          }
          std::string arc = asset.empty() ? "" : "@" + asset + "@";
          if (!path.empty()) arc += "<" + path + ">";
          if (!encoded_path["offset"].is_null() || !encoded_path["scale"].is_null()) {
            const minijson::Value& encoded_offset = encoded_path["offset"];
            const minijson::Value& encoded_scale = encoded_path["scale"];
            if (!encoded_offset.is_number() || !encoded_scale.is_number() ||
                !std::isfinite(encoded_offset.get_double()) ||
                !std::isfinite(encoded_scale.get_double())) {
              if (error) *error = std::string("Invalid Layer JSON ") + field + " offset";
              return false;
            }
            arc += "?layerOffset=" + std::to_string(encoded_offset.get_double()) +
                   ":" + std::to_string(encoded_scale.get_double());
          }
          if (encoded_path.contains("customData")) {
            tn::Value custom_data;
            size_t value_count = 0;
            if (std::strcmp(field, "references") != 0 ||
                !encoded_path["customData"].is_object() ||
                !ParseLayerJSONMetadataValue(encoded_path["customData"],
                    &custom_data, 0, &value_count) || !custom_data.as_dictionary()) {
              if (error) *error = "Invalid Layer JSON reference customData";
              return false;
            }
            if (!custom_data.as_dictionary()->empty()) {
              tn::PrintOptions options;
              options.float_precision = 9;
              options.double_precision = 17;
              options.sort_dictionary_keys = true;
              arc.push_back('\x1f');
              arc += tn::PrintValue(custom_data, options);
            }
          }
          items.push_back(std::move(arc));
          continue;
        }
        if (!encoded_path.is_string() ||
            !tn::Path(encoded_path.get_string()).is_valid()) {
          if (error) *error = std::string("Invalid Layer JSON ") + field + " path";
          return false;
        }
        items.push_back("<" + encoded_path.get_string() + ">");
      }
      if (qualifier.empty() || qualifier == "resetToExplicit") {
        *edit = tn::ArcEdit();
        edit->authored = true;
        *effective = std::move(items);
      } else {
        edit->authored = true;
        edit->is_explicit = false;
        std::vector<std::string>* sublist = qualifier == "add" ? &edit->added
            : qualifier == "prepend" ? &edit->prepended
            : qualifier == "append" ? &edit->appended
            : qualifier == "delete" ? &edit->deleted
            : qualifier == "order" ? &edit->ordered : nullptr;
        if (!sublist) {
          if (error) *error = std::string("Unsupported Layer JSON ") + field + " operation";
          return false;
        }
        sublist->insert(sublist->end(), items.begin(), items.end());
        if (qualifier == "prepend") {
          effective->insert(effective->begin(), items.begin(), items.end());
        } else if (qualifier == "add" || qualifier == "append") {
          effective->insert(effective->end(), items.begin(), items.end());
        } else if (qualifier == "delete") {
          effective->erase(std::remove_if(effective->begin(), effective->end(),
              [&](const std::string& path) {
                return std::find(items.begin(), items.end(), path) != items.end();
              }), effective->end());
        } else {
          std::vector<std::string> reordered;
          for (const std::string& path : items) {
            if (std::find(effective->begin(), effective->end(), path) != effective->end() &&
                std::find(reordered.begin(), reordered.end(), path) == reordered.end())
              reordered.push_back(path);
          }
          for (const std::string& path : *effective) {
            if (std::find(reordered.begin(), reordered.end(), path) == reordered.end())
              reordered.push_back(path);
          }
          *effective = std::move(reordered);
        }
      }
    }
    return true;
  };
  if (!encoded["inherits"].is_null() || !encoded["specializes"].is_null() ||
      !encoded["references"].is_null() || !encoded["payloads"].is_null()) {
    tn::ArcListOpEdits& edits = prim->meta().ensure_arc_edits();
    if (!parse_path_ops("inherits", &prim->meta().inherits, &edits.inherits) ||
        !parse_path_ops("specializes", &prim->meta().specializes,
                        &edits.specializes) ||
        !parse_path_ops("references", &prim->meta().references,
                        &edits.references, true) ||
        !parse_path_ops("payloads", &prim->meta().payloads,
                        &edits.payloads, true)) {
      builder->end_prim();
      return false;
    }
  }
  const minijson::Value& encoded_sets = encoded["variantSets"];
  if (!encoded_sets.is_null()) {
    if (!encoded_sets.is_array() || encoded_sets.size() == 0) {
      if (error) *error = "Invalid Layer JSON variantSets list edits";
      builder->end_prim();
      return false;
    }
    tn::StringListOpEdits& edits = prim->meta().variantSetNameEdits();
    for (const minijson::Value& operation : encoded_sets) {
      if (!operation.is_object() || !operation["op"].is_string() ||
          !operation["items"].is_array()) {
        if (error) *error = "Invalid Layer JSON variantSets operation";
        builder->end_prim();
        return false;
      }
      std::vector<std::string> names;
      for (const minijson::Value& item : operation["items"]) {
        if (!item.is_string() || item.get_string().empty()) {
          if (error) *error = "Invalid Layer JSON variant set name";
          builder->end_prim();
          return false;
        }
        names.push_back(item.get_string());
      }
      const std::string op = operation["op"].get_string();
      if (op.empty() || op == "resetToExplicit") {
        edits = tn::StringListOpEdits();
        edits.authored = true;
        edits.is_explicit = true;
        edits.explicit_items = std::move(names);
      } else {
        if (edits.is_explicit) {
          edits = tn::StringListOpEdits();
        }
        edits.authored = true;
        edits.is_explicit = false;
        std::vector<std::string>* target = op == "add" ? &edits.added
            : op == "prepend" ? &edits.prepended
            : op == "append" ? &edits.appended
            : op == "delete" ? &edits.deleted
            : op == "order" ? &edits.ordered : nullptr;
        if (!target) {
          if (error) *error = "Unsupported Layer JSON variantSets operation";
          builder->end_prim();
          return false;
        }
        target->insert(target->end(), names.begin(), names.end());
      }
    }
    std::vector<std::string> names = edits.is_explicit
        ? edits.explicit_items : edits.added;
    if (!edits.is_explicit) {
      names.insert(names.begin(), edits.prepended.begin(), edits.prepended.end());
      names.insert(names.end(), edits.appended.begin(), edits.appended.end());
      std::vector<std::string> ordered;
      for (const std::string& name : edits.ordered) {
        auto it = std::find(names.begin(), names.end(), name);
        if (it != names.end()) {
          ordered.push_back(*it);
          names.erase(it);
        }
      }
      ordered.insert(ordered.end(), names.begin(), names.end());
      names = std::move(ordered);
    }
    for (const std::string& name : names) {
      tn::VariantSetData set;
      set.name = name;
      prim->meta().variantSets().push_back(std::move(set));
    }
  }
  const minijson::Value& encoded_variants = encoded["variants"];
  if (!encoded_variants.is_null()) {
    if (!encoded_variants.is_object()) {
      if (error) *error = "Invalid Layer JSON variants selection map";
      builder->end_prim();
      return false;
    }
    prim->meta().setVariantSelectionsAuthored();
    if (const auto* items = encoded_variants.object_items()) {
      for (const auto& item : *items) {
        if (item.key.empty() || !item.value().is_string()) {
          if (error) *error = "Invalid Layer JSON variant selection";
          builder->end_prim();
          return false;
        }
        const std::string selected = item.value().get_string();
        prim->meta().variantSelections().emplace_back(item.key, selected);
        if (prim->meta().variantSelection.empty())
          prim->meta().variantSelection = item.key + "=" + selected;
        for (tn::VariantSetData& set : prim->meta().variantSets()) {
          if (set.name == item.key) set.selected = selected;
        }
      }
    }
  }
  const minijson::Value& properties = encoded["properties"];
  if (!properties.is_null() && !properties.is_object()) {
    if (error) *error = "Layer JSON properties must be an object";
    builder->end_prim();
    return false;
  }
  if (const auto* items = properties.object_items()) {
    for (const auto& item : *items) {
      const std::string& prop_name = item.key;
      const minijson::Value& prop = item.value();
      if (!prop.is_object() || !prop["propertyType"].is_string()) {
        if (error) *error = "Invalid Layer JSON property record";
        builder->end_prim();
        return false;
      }
      const std::string property_type = prop["propertyType"].get_string();
      const bool custom = prop["isCustom"].is_boolean() && prop["isCustom"].get_bool();
      const minijson::Value& encoded_qualifier = prop["listEditQual"];
      const std::string list_qualifier = encoded_qualifier.is_string()
          ? encoded_qualifier.get_string() : "resetToExplicit";
      if (list_qualifier != "resetToExplicit" && list_qualifier != "add" &&
          list_qualifier != "append" && list_qualifier != "prepend" &&
          list_qualifier != "delete" && list_qualifier != "order") {
        if (error) *error = "Unsupported Layer JSON property list edit";
        builder->end_prim();
        return false;
      }
      if (property_type == "relationship" ||
          property_type == "noTargetsRelationship") {
        const minijson::Value& relation = prop["relationship"];
        if (!relation.is_object() || !relation["valueType"].is_string()) {
          if (error) *error = "Unsupported Layer JSON relationship record";
          builder->end_prim();
          return false;
        }
        std::vector<tn::Path> targets;
        const std::string value_type = relation["valueType"].get_string();
        const bool blocked = value_type == "valueBlock";
        if (property_type == "noTargetsRelationship" &&
            value_type != "defineOnly") {
          if (error) *error = "Invalid Layer JSON declared relationship";
          builder->end_prim();
          return false;
        }
        if (relation["listEditQual"].is_string() &&
            relation["listEditQual"].get_string() != list_qualifier) {
          if (error) *error = "Conflicting Layer JSON relationship list qualifiers";
          builder->end_prim();
          return false;
        }
        const auto add_target = [&](const minijson::Value& path_value) {
          if (!path_value.is_string()) return false;
          tn::Path path(path_value.get_string());
          if (!path.is_valid()) return false;
          targets.push_back(std::move(path));
          return true;
        };
        if (value_type == "path") {
          if (!add_target(relation["target"])) {
            if (error) *error = "Invalid Layer JSON relationship target";
            builder->end_prim();
            return false;
          }
        } else if (value_type == "pathVector") {
          if (!relation["targets"].is_array()) {
            if (error) *error = "Invalid Layer JSON relationship target list";
            builder->end_prim();
            return false;
          }
          for (const minijson::Value& target : relation["targets"]) {
            if (!add_target(target)) {
              if (error) *error = "Invalid Layer JSON relationship target";
              builder->end_prim();
              return false;
            }
          }
        } else if (value_type != "defineOnly" && !blocked) {
          if (error) *error = "Unsupported Layer JSON relationship value type";
          builder->end_prim();
          return false;
        }
        const bool explicit_empty = targets.empty() &&
            (blocked || value_type == "pathVector");
        prim->set_relationship_targets(prop_name, std::move(targets));
        if (explicit_empty) {
          tn::ArcEdit& edit = prim->ensure_relationship_edit(prop_name);
          edit.authored = true;
          edit.is_explicit = true;
          edit.is_value_block = blocked;
        } else if (list_qualifier != "resetToExplicit") {
          tn::ArcEdit& edit = prim->ensure_relationship_edit(prop_name);
          edit.authored = true;
          edit.is_explicit = false;
          const std::vector<tn::Path>* authored_targets = prim->relationship(prop_name);
          std::vector<std::string>* operation = list_qualifier == "add" ? &edit.added
              : list_qualifier == "append" ? &edit.appended
              : list_qualifier == "prepend" ? &edit.prepended
              : list_qualifier == "delete" ? &edit.deleted : &edit.ordered;
          if (authored_targets) {
            for (const tn::Path& target : *authored_targets)
              operation->push_back(target.str());
          }
        }
        prim->set_relationship_flags(prop_name, custom ? tn::PropSlot::kFlagCustom : 0);
        continue;
      }
      if (property_type == "emptyAttribute") {
        if (!prop["typeName"].is_string()) {
          if (error) *error = "Invalid Layer JSON empty attribute type";
          builder->end_prim();
          return false;
        }
        const std::string attr_type = prop["typeName"].get_string();
        bool is_array = false;
        const tn::TypeId type = tn::ParseTypeName(attr_type, is_array);
        if (type == tn::TypeId::Invalid) {
          if (error) *error = "Unsupported Layer JSON empty attribute type";
          builder->end_prim();
          return false;
        }
        const tn::PropNameId name_id = tn::GetPropNameTable().intern(prop_name);
        prim->add_property_slot(name_id, type,
            static_cast<uint16_t>((custom ? tn::PropSlot::kFlagCustom : 0) |
                (is_array ? tn::PropSlot::kFlagArray : 0)));
        prim->set_property_type_name(prop_name, attr_type);
        continue;
      }
      if (property_type != "attribute" && property_type != "connection") {
        if (error) *error = "Unsupported Layer JSON property type: " + property_type;
        builder->end_prim();
        return false;
      }
      const minijson::Value& attribute = prop["attribute"];
      if (!attribute.is_object() || !attribute["typeName"].is_string()) {
        if (error) *error = "Invalid Layer JSON attribute record";
        builder->end_prim();
        return false;
      }
      const std::string attr_type = attribute["typeName"].get_string();
      bool is_array = false;
      const tn::TypeId type = tn::ParseTypeName(attr_type, is_array);
      if (type == tn::TypeId::Invalid) {
        if (error) *error = "Unsupported Layer JSON attribute type: " + attr_type;
        builder->end_prim();
        return false;
      }
      uint16_t flags = custom ? tn::PropSlot::kFlagCustom : 0;
      if (is_array) flags |= tn::PropSlot::kFlagArray;
      if (attribute["variability"].is_string() &&
          attribute["variability"].get_string() == "uniform")
        flags |= tn::PropSlot::kFlagUniform;
      const bool has_value = attribute["hasValue"].is_boolean() &&
                             attribute["hasValue"].get_bool() &&
                             attribute["value"].is_string();
      const bool blocked_value = attribute["valueType"].is_string() &&
                                 attribute["valueType"].get_string() == "blocked";
      const bool has_samples = attribute["hasTimeSamples"].is_boolean() &&
                               attribute["hasTimeSamples"].get_bool();
      std::string edit_text;
      tn::ArrayEditData edit_data;
      bool array_edit = false;
      if (has_value && is_array && !blocked_value) {
        const std::string text = attribute["value"].get_string();
        tn::Lexer lexer(text.data(), text.size());
        const tn::Token& head = lexer.peek();
        if (head.type == tn::TokenType::Identifier && head.value == "edit") {
          std::string edit_error;
          if (!tn::ParseArrayEditText(lexer, type, &edit_text, &edit_data,
                                      &edit_error) ||
              lexer.peek().type != tn::TokenType::Eof) {
            if (error) *error = edit_error.empty()
                ? "Trailing tokens in Layer JSON array edit" : edit_error;
            builder->end_prim();
            return false;
          }
          array_edit = true;
        }
      }
      tn::Value value;
      bool parsed_array = false;
      if (has_value && !array_edit &&
          !ParseLayerJSONValue(attribute["value"], attr_type,
                                             &value, &parsed_array, error)) {
        builder->end_prim();
        return false;
      }
      if (blocked_value) {
        builder->add_property(prop_name, tn::Value::MakeBlock(), flags);
      } else if (array_edit) {
        const tn::PropNameId name_id = tn::GetPropNameTable().intern(prop_name);
        prim->add_property_slot(name_id, type, flags);
        prim->set_raw_default_source(prop_name, std::move(edit_text));
        prim->set_array_edit(prop_name, std::move(edit_data));
        if (has_samples) prim->mark_property_time_sampled(name_id);
      } else if (has_value) {
        builder->add_property(prop_name, std::move(value), flags);
      } else {
        const tn::PropNameId name_id = tn::GetPropNameTable().intern(prop_name);
        prim->add_property_slot(name_id, type, flags);
        if (has_samples) prim->mark_property_time_sampled(name_id);
      }
      prim->set_property_type_name(prop_name, attr_type);
      if (property_type == "connection") {
        std::vector<tn::Path> targets;
        if (attribute["connection"].is_string()) {
          tn::Path target(attribute["connection"].get_string());
          if (!target.is_valid()) {
            if (error) *error = "Invalid Layer JSON connection target";
            builder->end_prim();
            return false;
          }
          targets.push_back(std::move(target));
        } else if (attribute["connections"].is_array()) {
          for (const minijson::Value& encoded_target : attribute["connections"]) {
            if (!encoded_target.is_string()) {
              if (error) *error = "Invalid Layer JSON connection target list";
              builder->end_prim();
              return false;
            }
            tn::Path target(encoded_target.get_string());
            if (!target.is_valid()) {
              if (error) *error = "Invalid Layer JSON connection target";
              builder->end_prim();
              return false;
            }
            targets.push_back(std::move(target));
          }
        }
        // Legacy exports an attribute authoring both a value and `.connect`
        // as a targetless "connection" record carrying only the value.
        if (targets.empty() && !has_value && !blocked_value && !has_samples) {
          if (error) *error = "Layer JSON connection has no targets";
          builder->end_prim();
          return false;
        }
        if (!targets.empty())
          prim->set_connection_targets(prop_name, std::move(targets));
      }
      const minijson::Value& metadata = attribute["metadata"];
      if (!metadata.is_null()) {
        if (!metadata.is_object()) {
          if (error) *error = "Layer JSON attribute metadata must be an object";
          builder->end_prim();
          return false;
        }
        tn::PropMeta& meta = prim->ensure_property_meta(prop_name);
        const auto copy_string = [&](const char* key, uint32_t flag,
                                     std::string* out, bool canonical = false) {
          const minijson::Value& field = metadata[key];
          if (field.is_null()) return true;
          if (!field.is_string()) return false;
          if (canonical && !field.get_string().empty() &&
              field.get_string().front() == '"') {
            tn::Value parsed;
            bool parsed_array = false;
            if (!ParseLayerJSONValue(field, "string", &parsed,
                                     &parsed_array, nullptr)) return false;
            const std::string* text = parsed.as_string();
            if (!text) return false;
            *out = *text;
          } else {
            *out = field.get_string();
          }
          meta.authored |= flag;
          return true;
        };
        if (!copy_string("interpolation", tn::PropMeta::kInterpolation, &meta.interpolation, true) ||
            !copy_string("colorSpace", tn::PropMeta::kColorSpace, &meta.colorSpace, true) ||
            !copy_string("displayName", tn::PropMeta::kDisplayName, &meta.displayName, true) ||
            !copy_string("displayGroup", tn::PropMeta::kDisplayGroup, &meta.displayGroup, true) ||
            !copy_string("documentation", tn::PropMeta::kDoc, &meta.doc, true) ||
            !copy_string("doc", tn::PropMeta::kDoc, &meta.doc, true) ||
            !copy_string("comment", tn::PropMeta::kComment, &meta.comment, true) ||
            !copy_string("renderType", tn::PropMeta::kRenderType, &meta.renderType, true) ||
            !copy_string("connectability", tn::PropMeta::kConnectability, &meta.connectability, true) ||
            !copy_string("outputName", tn::PropMeta::kOutputName, &meta.outputName, true) ||
            !copy_string("bindMaterialAs", tn::PropMeta::kBindMaterialAs, &meta.bindMaterialAs, true) ||
            !copy_string("kind", tn::PropMeta::kKind, &meta.kind, true) ||
            !copy_string("permission", tn::PropMeta::kPermission, &meta.permission, true)) {
          if (error) *error = "Invalid Layer JSON string metadata";
          builder->end_prim();
          return false;
        }
        const auto copy_bool = [&](const char* key, uint32_t flag, bool* out) {
          const minijson::Value& field = metadata[key];
          if (field.is_null()) return true;
          if (field.is_boolean()) *out = field.get_bool();
          else if (field.is_string() &&
                   (field.get_string() == "0" || field.get_string() == "1"))
            *out = field.get_string() == "1";
          else return false;
          meta.authored |= flag;
          return true;
        };
        if (!copy_bool("hidden", tn::PropMeta::kHidden, &meta.hidden)) {
          if (error) *error = "Invalid Layer JSON boolean metadata";
          builder->end_prim();
          return false;
        }
        const auto read_int = [&](const char* key, int32_t* out) {
          const minijson::Value& field = metadata[key];
          if (field.is_number_integer()) {
            *out = field.get_int();
            return true;
          }
          if (!field.is_string()) return false;
          tn::Value parsed;
          bool parsed_array = false;
          if (!ParseLayerJSONValue(field, "int", &parsed, &parsed_array, nullptr) ||
              !parsed.as_int()) return false;
          *out = *parsed.as_int();
          return true;
        };
        if (!metadata["elementSize"].is_null()) {
          if (!read_int("elementSize", &meta.elementSize)) {
            if (error) *error = "Invalid Layer JSON elementSize";
            builder->end_prim();
            return false;
          }
          meta.authored |= tn::PropMeta::kElementSize;
        }
        if (!metadata["weight"].is_null()) {
          const minijson::Value& field = metadata["weight"];
          if (field.is_number()) meta.weight = field.get_double();
          else if (field.is_string()) {
            tn::Value parsed;
            bool parsed_array = false;
            if (!ParseLayerJSONValue(field, "double", &parsed,
                                     &parsed_array, nullptr) || !parsed.as_double()) {
              if (error) *error = "Invalid Layer JSON weight";
              builder->end_prim();
              return false;
            }
            meta.weight = *parsed.as_double();
          } else {
            if (error) *error = "Invalid Layer JSON weight";
            builder->end_prim();
            return false;
          }
          meta.authored |= tn::PropMeta::kWeight;
        }
        if (!metadata["unauthoredValuesIndex"].is_null()) {
          if (!read_int("unauthoredValuesIndex", &meta.unauthoredValuesIndex)) {
            if (error) *error = "Invalid Layer JSON unauthoredValuesIndex";
            builder->end_prim();
            return false;
          }
          meta.authored |= tn::PropMeta::kUnauthoredIdx;
        }
        const minijson::Value& allowed_tokens = metadata["allowedTokens"];
        if (!allowed_tokens.is_null()) {
          if (!allowed_tokens.is_array()) {
            if (error) *error = "Layer JSON allowedTokens must be an array";
            builder->end_prim();
            return false;
          }
          for (const minijson::Value& token : allowed_tokens) {
            if (!token.is_string()) {
              if (error) *error = "Layer JSON allowedTokens must contain strings";
              builder->end_prim();
              return false;
            }
            meta.allowedTokens.push_back(token.get_string());
          }
          meta.authored |= tn::PropMeta::kAllowedTokens;
        }
        const auto copy_dictionary = [&](const char* key, uint32_t flag,
                                         tn::Value* out) {
          const minijson::Value& field = metadata[key];
          if (field.is_null()) return true;
          size_t value_count = 0;
          tn::Value parsed;
          if (!field.is_object() ||
              !ParseLayerJSONMetadataValue(field, &parsed, 0, &value_count) ||
              !parsed.as_dictionary()) return false;
          *out = std::move(parsed);
          meta.authored |= flag;
          return true;
        };
        if (!copy_dictionary("customData", tn::PropMeta::kCustomData,
                             &meta.customData) ||
            !copy_dictionary("assetInfo", tn::PropMeta::kAssetInfo,
                             &meta.assetInfo) ||
            !copy_dictionary("sdrMetadata", tn::PropMeta::kSdrMetadata,
                             &meta.sdrMetadata)) {
          if (error) *error = "Invalid Layer JSON property dictionary metadata";
          builder->end_prim();
          return false;
        }
      }
      if (has_samples) {
        const minijson::Value& samples = attribute["timeSamples"];
        if (!samples.is_array()) {
          if (error) *error = "Layer JSON timeSamples must be an array";
          builder->end_prim();
          return false;
        }
        for (const minijson::Value& sample : samples) {
          const bool sample_blocked = sample.is_object() &&
              sample["blocked"].is_boolean() && sample["blocked"].get_bool();
          if (!sample.is_object() || !sample["time"].is_number() ||
              (!sample_blocked && !sample["value"].is_string())) {
            if (error) *error = "Unsupported Layer JSON time sample";
            builder->end_prim();
            return false;
          }
          tn::Value sample_value;
          bool sample_array = false;
          if (sample_blocked) sample_value = tn::Value::MakeBlock();
          else if (!ParseLayerJSONValue(sample["value"], attr_type,
                                        &sample_value, &sample_array, error)) {
            builder->end_prim();
            return false;
          }
          builder->add_time_sample(prop_name, sample["time"].get_double(),
                                   std::move(sample_value));
        }
      }
    }
  }
  const minijson::Value& children = encoded["children"];
  if (!children.is_null()) {
    if (!children.is_object()) {
      if (error) *error = "Layer JSON children must be an object";
      builder->end_prim();
      return false;
    }
    if (const auto* items = children.object_items()) {
      for (const auto& child : *items) {
        if (child.key != child.value()["name"].get_string() ||
            !ParseLayerJSONPrim(child.value(), builder, depth + 1, prim_count,
                                prim_limit, error)) {
          builder->end_prim();
          return false;
        }
      }
    }
  }
  builder->end_prim();
  return true;
}

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

int32_t NextLayerDocument::exportJSONSize() {
  if (!ensureLoaded_()) return -1;
  clearExport_();
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  const tn::Layer* layer = stage ? stage->GetRootLayer() : nullptr;
  if (!layer) return fail_("No layer is loaded");
  const size_t retained = layer->memory_usage();
  if (retained > kMaxLayerJSONBytes / 16)
    return fail_("Layer JSON output exceeds 512 MiB limit");
  const tn::LayerMeta& meta = layer->meta();
  size_t estimate = 256;
  for (const std::string* text : {&meta.doc, &meta.comment, &meta.owner,
       &meta.playbackMode,
       &meta.defaultPrim, &meta.upAxis, &meta.colorConfiguration,
       &meta.colorManagementSystem, &meta.renderSettingsPrimPath}) {
    if (text->size() > (kMaxLayerJSONBytes - estimate) / 8)
      return fail_("Layer JSON output exceeds 512 MiB limit");
    estimate += text->size() * 8;
  }
  minijson::Value root = minijson::Value::object();
  root["name"] = "";
  root["typeName"] = "Layer";
  minijson::Value encoded_meta = minijson::Value::object();
  if (meta.upAxis_set) encoded_meta["upAxis"] = meta.upAxis;
  if (meta.defaultPrim_set) encoded_meta["defaultPrim"] = meta.defaultPrim;
  if (meta.metersPerUnit_set) encoded_meta["metersPerUnit"] = meta.metersPerUnit;
  if (meta.timeCodesPerSecond_set)
    encoded_meta["timeCodesPerSecond"] = meta.timeCodesPerSecond;
  if (meta.framesPerSecond_set)
    encoded_meta["framesPerSecond"] = meta.framesPerSecond;
  if (meta.startTimeCode_set) encoded_meta["startTimeCode"] = meta.startTimeCode;
  if (meta.endTimeCode_set) encoded_meta["endTimeCode"] = meta.endTimeCode;
  if (meta.kilogramsPerUnit_set)
    encoded_meta["kilogramsPerUnit"] = meta.kilogramsPerUnit;
  if (meta.doc_set) encoded_meta["doc"] = meta.doc;
  if (meta.comment_set) encoded_meta["comment"] = meta.comment;
  if (meta.owner_set) encoded_meta["owner"] = meta.owner;
  if (meta.colorConfiguration_set)
    encoded_meta["colorConfiguration"] = meta.colorConfiguration;
  if (meta.colorManagementSystem_set)
    encoded_meta["colorManagementSystem"] = meta.colorManagementSystem;
  if (meta.renderSettingsPrimPath_set)
    encoded_meta["renderSettingsPrimPath"] = meta.renderSettingsPrimPath;
  if (meta.autoPlay_set) encoded_meta["autoPlay"] = meta.autoPlay;
  if (meta.playbackMode_set)
    encoded_meta["playbackMode"] = meta.playbackMode;
  if (meta.hasOwnedSubLayers_set)
    encoded_meta["hasOwnedSubLayers"] = meta.hasOwnedSubLayers;
  if (meta.rootPrimOrder_set) {
    minijson::Value children = minijson::Value::array();
    for (const std::string& child : meta.rootPrimOrder) {
      if (child.size() > (kMaxLayerJSONBytes - estimate) / 8)
        return fail_("Layer JSON output exceeds 512 MiB limit");
      estimate += child.size() * 8;
      children.push_back(child);
    }
    encoded_meta["primChildren"] = std::move(children);
  }
  if (meta.subLayers_set) {
    minijson::Value sublayers = minijson::Value::array();
    for (size_t i = 0; i < meta.subLayers.size(); ++i) {
      const std::string& asset = meta.subLayers[i];
      if (asset.size() > (kMaxLayerJSONBytes - estimate) / 8)
        return fail_("Layer JSON output exceeds 512 MiB limit");
      estimate += asset.size() * 8;
      minijson::Value sublayer = minijson::Value::object();
      sublayer["assetPath"] = asset;
      if (i < meta.subLayerOffsets.size() &&
          (meta.subLayerOffsets[i].first != 0.0 ||
           meta.subLayerOffsets[i].second != 1.0)) {
        minijson::Value offset = minijson::Value::object();
        offset["offset"] = meta.subLayerOffsets[i].first;
        offset["scale"] = meta.subLayerOffsets[i].second;
        sublayer["layerOffset"] = std::move(offset);
      }
      sublayers.push_back(std::move(sublayer));
    }
    encoded_meta["subLayers"] = std::move(sublayers);
  }
  if (meta.relocates_set || !meta.relocates.empty()) {
    minijson::Value relocates = minijson::Value::array();
    for (const auto& entry : meta.relocates) {
      if (entry.first.size() > (kMaxLayerJSONBytes - estimate) / 8 ||
          entry.second.size() > (kMaxLayerJSONBytes - estimate - entry.first.size() * 8) / 8)
        return fail_("Layer JSON output exceeds 512 MiB limit");
      estimate += (entry.first.size() + entry.second.size()) * 8;
      minijson::Value item = minijson::Value::object();
      item["source"] = entry.first;
      item["target"] = entry.second;
      relocates.push_back(std::move(item));
    }
    encoded_meta["layerRelocates"] = std::move(relocates);
  }
  if (!meta.unknownMeta.empty()) {
    minijson::Value unknown = minijson::Value::object();
    for (const auto& entry : meta.unknownMeta) {
      if (entry.first.size() > (kMaxLayerJSONBytes - estimate) / 8 ||
          entry.second.size() > (kMaxLayerJSONBytes - estimate - entry.first.size() * 8) / 8)
        return fail_("Layer JSON output exceeds 512 MiB limit");
      estimate += (entry.first.size() + entry.second.size()) * 8;
      unknown[entry.first] = entry.second;
    }
    encoded_meta["unregisteredMetas"] = std::move(unknown);
  }
  const auto add_dictionary_meta = [&](const char* key, const tn::Value& value,
                                      bool authored) {
    if (!authored) return true;
    const size_t entries = MetadataValueEntryCount(value);
    if (!ChargeLayerJSONValue(value, entries, &estimate)) return false;
    encoded_meta[key] = LayerMetadataValueJSON(value);
    return true;
  };
  if (!add_dictionary_meta("customLayerData", meta.customLayerData,
                           meta.customLayerData_set) ||
      !add_dictionary_meta("expressionVariables", meta.expressionVariables,
                           meta.expressionVariables_set))
    return fail_("Layer JSON output exceeds 512 MiB limit");
  if (!encoded_meta.empty()) root["metas"] = std::move(encoded_meta);
  minijson::Value prim_specs = minijson::Value::object();
  for (uint32_t index : layer->root_indices()) {
    const tn::PrimSpec* prim = layer->prim(index);
    if (!prim) return fail_("Invalid root prim in Layer JSON export");
    minijson::Value encoded = LayerPrimJSON(*layer, index, &estimate, 0);
    if (encoded.is_null())
      return fail_("Layer JSON output exceeds 512 MiB limit or nesting limit");
    prim_specs[prim->name()] = std::move(encoded);
  }
  if (!prim_specs.empty()) root["primSpecs"] = std::move(prim_specs);
  layer_json_ = root.dump(2);
  if (layer_json_.size() > kMaxLayerJSONBytes ||
      layer_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    layer_json_.clear();
    return fail_("Layer JSON output exceeds 512 MiB limit");
  }
  error_.clear();
  return static_cast<int32_t>(layer_json_.size());
}

uintptr_t NextLayerDocument::exportJSONData() const {
  return reinterpret_cast<uintptr_t>(layer_json_.data());
}

int32_t NextLayerDocument::mhProfileJSONSize() {
  if (!stage_) {
    mh_profile_json_ = "[]";
    return 2;
  }
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  if (!stage) return fail_("No layer is loaded");
  constexpr size_t kMaxJSONBytes = size_t{1} << 29;
  size_t estimated_bytes = 2;
  size_t prim_count = 0;
  minijson::Value result = minijson::Value::array();
  bool exceeded = false;
  stage->Traverse([&](const tn::UsdPrim& prim) {
    if (++prim_count > 1000000) { exceeded = true; return false; }
    const std::string& type = prim.GetTypeName();
    if (type != "Skeleton" && type != "SkelAnimation" &&
        type != "Material" && type != "SkelRoot") return true;
    minijson::Value attrs = minijson::Value::object();
    minijson::Value rels = minijson::Value::object();
    std::vector<std::string> names = prim.GetPropertyNames();
    const std::vector<std::string> relationship_names = prim.GetRelationshipNames();
    names.insert(names.end(), relationship_names.begin(), relationship_names.end());
    for (const std::string& name : names) {
      if (name.rfind("mh:", 0) != 0) continue;
      if (!prim.HasAuthoredProperty(name) && !prim.GetRelationship(name)) continue;
      if (estimated_bytes > kMaxJSONBytes - std::min(name.size() * size_t{4} + 512,
                                                      kMaxJSONBytes)) {
        exceeded = true; return false;
      }
      estimated_bytes += name.size() * size_t{4} + 512;
      const auto* targets = prim.GetRelationship(name);
      if (targets) {
        minijson::Value encoded = minijson::Value::array();
        for (const tn::Path& path : *targets) {
          if (path.str().size() > (kMaxJSONBytes - estimated_bytes) / 4) {
            exceeded = true; return false;
          }
          estimated_bytes += path.str().size() * 4;
          encoded.push_back(path.str());
        }
        rels[name] = std::move(encoded);
        continue;
      }
      const tn::Value* value = prim.GetPropertyValue(name);
      const size_t elements = value ? value->array_size() : 0;
      if (value) {
        const size_t remaining = kMaxJSONBytes - estimated_bytes;
        const size_t strings = value->dynamic_string_memory_usage();
        if (strings > remaining || elements > (remaining - strings) / 256) {
          exceeded = true; return false;
        }
        estimated_bytes += strings + elements * 256;
      }
      if (prim.HasTimeSamples(name)) {
        minijson::Value samples = minijson::Value::array();
        const std::vector<double> times = prim.GetTimeSampleTimes(name);
        if (times.size() > (kMaxJSONBytes - estimated_bytes) / 512) {
          exceeded = true; return false;
        }
        estimated_bytes += times.size() * 512;
        for (double time : times) {
          const tn::Value* sample = prim.GetValueAtTime(name, time);
          if (!sample) continue;
          const auto* floats = sample->as_float_array();
          if (!floats) continue;
          if (floats->size() > (kMaxJSONBytes - estimated_bytes) / 32) {
            exceeded = true; return false;
          }
          estimated_bytes += floats->size() * 32;
          minijson::Value encoded = minijson::Value::object();
          encoded["t"] = time;
          encoded["v"] = NextValueJSON(*sample);
          samples.push_back(std::move(encoded));
        }
        minijson::Value sampled = minijson::Value::object();
        sampled["timeSamples"] = std::move(samples);
        attrs[name] = std::move(sampled);
      } else if (value) {
        attrs[name] = NextValueJSON(*value);
      } else {
        attrs[name] = minijson::Value();
      }
    }
    if (!attrs.empty() || !rels.empty()) {
      minijson::Value item = minijson::Value::object();
      item["path"] = prim.GetPath().str();
      item["type"] = type;
      item["attrs"] = std::move(attrs);
      if (!rels.empty()) item["rels"] = std::move(rels);
      result.push_back(std::move(item));
    }
    return true;
  });
  if (exceeded) return fail_("Metahuman profile exceeds 512 MiB output limit");
  mh_profile_json_ = result.dump();
  if (mh_profile_json_.size() > kMaxJSONBytes ||
      mh_profile_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    mh_profile_json_.clear();
    return fail_("Metahuman profile exceeds 512 MiB output limit");
  }
  error_.clear();
  return static_cast<int32_t>(mh_profile_json_.size());
}

int32_t NextLayerDocument::mhProfileJSONCopy(uint8_t* out,
                                             uint32_t cap) const {
  if (mh_profile_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) return -1;
  const int32_t required = static_cast<int32_t>(mh_profile_json_.size());
  if (!out || cap < mh_profile_json_.size()) return required;
  std::memcpy(out, mh_profile_json_.data(), mh_profile_json_.size());
  return required;
}

int32_t NextLayerDocument::shadingGraphJSONSize() {
  if (!stage_) {
    error_ = "Shading graph inspection requires a loaded Layer";
    shading_graph_json_.clear();
    return 0;
  }
  const tn::Stage* stage = lightusd_internal::BorrowNativeStage(stage_);
  if (!stage) return fail_("No layer is loaded");
  constexpr size_t kMaxJSONBytes = size_t{1} << 29;
  size_t estimated_bytes = 256;
  size_t prim_count = 0;
  bool exceeded = false;
  const auto charge = [&](size_t bytes) {
    if (bytes > kMaxJSONBytes - estimated_bytes) {
      exceeded = true;
      return false;
    }
    estimated_bytes += bytes;
    return true;
  };
  const auto charge_string = [&](size_t size, size_t overhead = 0) {
    if (size > (kMaxJSONBytes - estimated_bytes -
                std::min(overhead, kMaxJSONBytes - estimated_bytes)) / 4) {
      exceeded = true;
      return false;
    }
    return charge(size * 4 + overhead);
  };
  minijson::Value root = minijson::Value::object();
  root["version"] = 1;
  root["colorMetadataVersion"] = 1;
  root["colorSpaces"] = minijson::Value::object();
  root["assetPaths"] = minijson::Value::array();
  root["prims"] = minijson::Value::array();
  stage->Traverse([&](const tn::UsdPrim& prim) {
    if (++prim_count > 1000000 || !charge_string(prim.GetPath().str().size(), 128)) {
      exceeded = true;
      return false;
    }
    const std::string& type = prim.GetTypeName();
    const bool shading_prim = type == "Shader" || type == "Material" ||
                              type == "NodeGraph";
    minijson::Value properties = minijson::Value::object();
    const std::vector<std::string> property_names = prim.GetPropertyNames();
    for (const std::string& name : property_names) {
      if (!prim.HasAuthoredProperty(name)) continue;
      if (!charge_string(name.size(), 192)) return false;
      const tn::Value* value = prim.GetPropertyValue(name);
      const bool authored = true;
      if (value && authored) {
        const size_t remaining = kMaxJSONBytes - estimated_bytes;
        const size_t strings = value->dynamic_string_memory_usage();
        if (strings > remaining || value->array_size() > (remaining - strings) / 512) {
          exceeded = true;
          return false;
        }
        if (!charge(strings + value->array_size() * 512)) return false;
        if (const std::string* asset = value->as_asset_path()) {
          minijson::Value record = minijson::Value::object();
          record["primPath"] = prim.GetPath().str();
          record["propertyPath"] = prim.GetPath().str() + "." + name;
          record["authored"] = *asset;
          root["assetPaths"].push_back(std::move(record));
        }
      }
      const std::vector<tn::Path>* connections =
          NextPropertyConnections(prim, name);
      if (shading_prim) {
        const tn::PrimSpec* spec = prim.GetPrimSpec();
        const std::string* type_name = spec ? spec->property_type_name(name) : nullptr;
        std::string property_type = type_name ? *type_name
            : value ? tn::GetTypeName(value->type_id()) : std::string();
        if (value && value->is_array() && property_type.find("[]") == std::string::npos)
          property_type += "[]";
        minijson::Value item = minijson::Value::object();
        item["type"] = property_type;
        item["connections"] = minijson::Value::array();
        item["timeSampled"] = prim.HasTimeSamples(name);
        if (const tn::PropMeta* meta = prim.GetPropertyMeta(name)) {
          if ((meta->authored & tn::PropMeta::kColorSpace) && !meta->colorSpace.empty())
            item["colorSpace"] = meta->colorSpace;
        }
        if (connections) {
          for (const tn::Path& connection : *connections) {
            if (!charge_string(connection.str().size(), 32)) return false;
            item["connections"].push_back(connection.str());
          }
        }
        if (value && authored) item["value"] = NextValueJSON(*value);
        properties[name] = std::move(item);
      }
    }
    if (shading_prim) {
      for (const std::string& name : prim.GetRelationshipNames()) {
        if (!charge_string(name.size(), 96)) return false;
        minijson::Value targets = minijson::Value::array();
        const std::vector<tn::Path>* values = prim.GetRelationship(name);
        if (values) {
          for (const tn::Path& target : *values) {
            if (!charge_string(target.str().size(), 16)) return false;
            targets.push_back(target.str());
          }
        }
        minijson::Value relationship = minijson::Value::object();
        relationship["targets"] = std::move(targets);
        properties[name] = std::move(relationship);
      }
      minijson::Value record = minijson::Value::object();
      record["path"] = prim.GetPath().str();
      record["type"] = type;
      record["properties"] = std::move(properties);
      root["prims"].push_back(std::move(record));
    }
    for (const std::string& schema : prim.GetMeta().apiSchemas()) {
      if (schema != "ColorSpaceAPI") continue;
      const tn::Value* value = prim.GetPropertyValue("colorSpace:name");
      if (!value || !prim.HasAuthoredProperty("colorSpace:name")) continue;
      minijson::Value color = minijson::Value::object();
      color["value"] = NextValueJSON(*value);
      color["timeSampled"] = prim.HasTimeSamples("colorSpace:name");
      root["colorSpaces"][prim.GetPath().str()] = std::move(color);
    }
    return true;
  });
  if (exceeded) return fail_("Shading graph traversal budget exceeded");
  shading_graph_json_ = root.dump();
  if (shading_graph_json_.size() > kMaxJSONBytes ||
      shading_graph_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) {
    shading_graph_json_.clear();
    return fail_("Shading graph output exceeds 512 MiB limit");
  }
  error_.clear();
  return static_cast<int32_t>(shading_graph_json_.size());
}

int32_t NextLayerDocument::shadingGraphJSONCopy(uint8_t* out,
                                                uint32_t cap) const {
  if (shading_graph_json_.size() > static_cast<size_t>((std::numeric_limits<int32_t>::max)())) return -1;
  const int32_t required = static_cast<int32_t>(shading_graph_json_.size());
  if (!out || cap < shading_graph_json_.size()) return required;
  std::memcpy(out, shading_graph_json_.data(), shading_graph_json_.size());
  return required;
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

int32_t NextLayerDocument::loadJSON(const uint8_t* bytes, uint32_t size) {
  constexpr size_t kJSONWorkingLimit = size_t{1} << 30;
  if (!bytes || !size) return failJSON_("Layer JSON input is empty");
  if (size > kMaxLayerJSONBytes || size > kJSONWorkingLimit / 8)
    return failJSON_("Layer JSON input exceeds its 1 GiB working budget");
  minijson::Value root;
  minijson::Error parse_error;
  minijson::ParseOptions parse_options;
  parse_options.max_depth = 1024;
  if (!minijson::Parse(reinterpret_cast<const char*>(bytes), size,
                       &root, &parse_error, parse_options)) {
    const std::string message = "Failed to parse Layer JSON: " + parse_error.message;
    return failJSON_(message.c_str());
  }
  if (!root.is_object() ||
      (root.contains("typeName") &&
       (!root["typeName"].is_string() ||
        root["typeName"].get_string() != "Layer")))
    return failJSON_("Layer JSON root must be a Layer object");
  std::string table_error;
  if (!ValidateLayerJSONTables(root, &table_error))
    return failJSON_(table_error.c_str());

  tn::Layer layer;
  const minijson::Value& metadata = root["metas"];
  if (!metadata.is_null() && !metadata.is_object())
    return failJSON_("Layer JSON metas must be an object");
  if (metadata.is_object()) {
    tn::LayerMeta& meta = layer.meta();
    const auto read_string = [&](const char* key, std::string* out,
                                 bool* authored) {
      const minijson::Value& field = metadata[key];
      if (field.is_null()) return true;
      if (!field.is_string()) return false;
      *out = field.get_string();
      *authored = true;
      return true;
    };
    if (!read_string("upAxis", &meta.upAxis, &meta.upAxis_set) ||
        !read_string("defaultPrim", &meta.defaultPrim, &meta.defaultPrim_set) ||
        !read_string("doc", &meta.doc, &meta.doc_set) ||
        !read_string("comment", &meta.comment, &meta.comment_set) ||
        !read_string("owner", &meta.owner, &meta.owner_set) ||
        !read_string("colorConfiguration", &meta.colorConfiguration,
                      &meta.colorConfiguration_set) ||
        !read_string("colorManagementSystem", &meta.colorManagementSystem,
                      &meta.colorManagementSystem_set) ||
        !read_string("renderSettingsPrimPath", &meta.renderSettingsPrimPath,
                      &meta.renderSettingsPrimPath_set))
      return failJSON_("Invalid Layer JSON string metadata");
    const auto read_number = [&](const char* key, double* out, bool* authored) {
      const minijson::Value& field = metadata[key];
      if (field.is_null()) return true;
      if (!field.is_number()) return false;
      *out = field.get_double();
      *authored = true;
      return true;
    };
    if (!read_number("metersPerUnit", &meta.metersPerUnit, &meta.metersPerUnit_set) ||
        !read_number("timeCodesPerSecond", &meta.timeCodesPerSecond,
                     &meta.timeCodesPerSecond_set) ||
        !read_number("framesPerSecond", &meta.framesPerSecond,
                     &meta.framesPerSecond_set) ||
        !read_number("startTimeCode", &meta.startTimeCode, &meta.startTimeCode_set) ||
        !read_number("endTimeCode", &meta.endTimeCode, &meta.endTimeCode_set) ||
        !read_number("kilogramsPerUnit", &meta.kilogramsPerUnit,
                     &meta.kilogramsPerUnit_set))
      return failJSON_("Invalid Layer JSON numeric metadata");
    if (!metadata["autoPlay"].is_null()) {
      if (!metadata["autoPlay"].is_boolean())
        return failJSON_("Invalid Layer JSON autoPlay metadata");
      meta.autoPlay = metadata["autoPlay"].get_bool();
      meta.autoPlay_set = true;
    }
    if (!metadata["playbackMode"].is_null()) {
      if (!metadata["playbackMode"].is_string())
        return failJSON_("Invalid Layer JSON playbackMode metadata");
      meta.playbackMode = metadata["playbackMode"].get_string() == "loop"
          ? "loop" : "none";
      meta.playbackMode_set = true;
    }
    if (!metadata["hasOwnedSubLayers"].is_null()) {
      if (!metadata["hasOwnedSubLayers"].is_boolean())
        return failJSON_("Invalid Layer JSON hasOwnedSubLayers metadata");
      meta.hasOwnedSubLayers = metadata["hasOwnedSubLayers"].get_bool();
      meta.hasOwnedSubLayers_set = true;
    }
    const minijson::Value& prim_children = metadata["primChildren"];
    if (!prim_children.is_null()) {
      if (!prim_children.is_array())
        return failJSON_("Layer JSON primChildren must be an array");
      for (const minijson::Value& child : prim_children) {
        if (!child.is_string())
          return failJSON_("Layer JSON primChildren must contain strings");
        meta.rootPrimOrder.push_back(child.get_string());
      }
      meta.rootPrimOrder_set = true;
    }
    const minijson::Value& sublayers = metadata["subLayers"];
    if (!sublayers.is_null()) {
      if (!sublayers.is_array())
        return failJSON_("Layer JSON subLayers must be an array");
      for (const minijson::Value& sublayer : sublayers) {
        if (!sublayer.is_object() || !sublayer["assetPath"].is_string())
          return failJSON_("Invalid Layer JSON subLayer record");
        meta.subLayers.push_back(sublayer["assetPath"].get_string());
        double offset = 0.0, scale = 1.0;
        const minijson::Value& layer_offset = sublayer["layerOffset"];
        if (!layer_offset.is_null()) {
          if (!layer_offset.is_object() ||
              (!layer_offset["offset"].is_null() &&
               !layer_offset["offset"].is_number()) ||
              (!layer_offset["scale"].is_null() &&
               !layer_offset["scale"].is_number()))
            return failJSON_("Invalid Layer JSON subLayer offset");
          if (layer_offset["offset"].is_number())
            offset = layer_offset["offset"].get_double();
          if (layer_offset["scale"].is_number())
            scale = layer_offset["scale"].get_double();
        }
        meta.subLayerOffsets.emplace_back(offset, scale);
      }
      meta.subLayers_set = true;
    }
    const minijson::Value& relocates = metadata["layerRelocates"];
    if (!relocates.is_null()) {
      if (!relocates.is_array())
        return failJSON_("Layer JSON layerRelocates must be an array");
      for (const minijson::Value& relocate : relocates) {
        if (!relocate.is_object() || !relocate["source"].is_string() ||
            !relocate["target"].is_string())
          return failJSON_("Invalid Layer JSON relocation record");
        tn::Path source = tn::Path::Parse(relocate["source"].get_string());
        tn::Path target = tn::Path::Parse(relocate["target"].get_string());
        if (!source.is_valid() || !target.is_valid())
          return failJSON_("Invalid Layer JSON relocation path");
        meta.relocates.emplace_back(source.str(), target.str());
      }
      meta.relocates_set = true;
    }
    const minijson::Value& unregistered = metadata["unregisteredMetas"];
    if (!unregistered.is_null()) {
      if (!unregistered.is_object())
        return failJSON_("Layer JSON unregisteredMetas must be an object");
      for (const auto& entry : *unregistered.object_items()) {
        if (!entry.value().is_string())
          return failJSON_("Unregistered Layer JSON metadata must be strings");
        meta.unknownMeta.emplace_back(entry.key, entry.value().get_string());
      }
    }
    const auto read_dictionary = [&](const char* key, tn::Value* value,
                                     bool* authored) {
      const minijson::Value& field = metadata[key];
      if (field.is_null()) return true;
      if (!field.is_object()) return false;
      size_t value_count = 0;
      if (!ParseLayerJSONMetadataValue(field, value, 0, &value_count))
        return false;
      *authored = true;
      return true;
    };
    if (!read_dictionary("customLayerData", &meta.customLayerData,
                         &meta.customLayerData_set) ||
        !read_dictionary("expressionVariables", &meta.expressionVariables,
                         &meta.expressionVariables_set))
      return failJSON_("Invalid Layer JSON dictionary metadata");
  }

  const minijson::Value& specs = root["primSpecs"];
  if (!specs.is_null() && !specs.is_object())
    return failJSON_("Layer JSON primSpecs must be an object");
  tn::LayerBuilder builder(layer);
  size_t prim_count = 0;
  if (const auto* items = specs.object_items()) {
    for (const auto& item : *items) {
      if (!item.value().is_object() || !item.value()["name"].is_string() ||
          item.key != item.value()["name"].get_string())
        return failJSON_("Layer JSON root prim key must match its name");
      std::string error;
      if (!ParseLayerJSONPrim(item.value(), &builder, 0, &prim_count,
                              1000000, &error))
        return failJSON_(error.c_str());
    }
  }
  builder.finalize();
  const size_t layer_bytes = layer.memory_usage();
  const size_t input_bytes = size;
  if (input_bytes > kJSONWorkingLimit / 4 ||
      layer_bytes > kJSONWorkingLimit - input_bytes * 4)
    return failJSON_("Layer JSON import working set exceeds 1 GiB");
  lightusd_stage* candidate = nullptr;
  const lightusd_status create_status = lightusd_stage_create(&candidate);
  if (create_status != LIGHTUSD_OK || !candidate)
    return failJSON_("Could not allocate a Layer JSON document");
  if (!lightusd_internal::SetNativeRootLayer(candidate, std::move(layer))) {
    lightusd_stage_destroy(candidate);
    return failJSON_("Could not install the imported Layer JSON");
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
      is_array > 1) {
    return fail_("Invalid prim path, property name, type, or array flag");
  }

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
          items.size(), LIGHTUSD_PROP_CUSTOM);
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
        value.c_str(), 1, LIGHTUSD_PROP_CUSTOM);
    if (status != LIGHTUSD_OK) {
      error_ = lightusd_last_error();
      return -1;
    }
  } else {
    const size_t element_size = lightusd_type_size(type);
    const size_t storage_size = is_array && IsHalfType(type)
        ? lightusd_type_component_count(type) * sizeof(float) : element_size;
    if (element_size == 0 || count == 0 ||
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
        data, count, LIGHTUSD_PROP_CUSTOM);
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
    const std::string root = tn::WriteUSDAToString(*stage);
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
