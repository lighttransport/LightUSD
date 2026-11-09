// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-layer.hh"
#include "binding-next-layer-json-internal.hh"
#include "c-stage-bridge.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/layer/arc-reference.hh"
#include "next/types/type-info.hh"
#include <algorithm>
#include <limits>
#include <utility>
namespace tn = lightusd::next;
#include "next/writer/value-printer.hh"
#include "next/parser/lexer.hh"
#include "next/parser/value-parser.hh"
#include "next/composition/composition.hh"
#include "next/stage/stage.hh"

namespace lightusd {
namespace web_next {
using namespace layer_json_detail;
namespace {
size_t LazyDecodedBytes(tn::ValueView value) {
  size_t bytes = 0;
  std::vector<tn::ValueView> pending{value};
  while (!pending.empty()) {
    const tn::ValueView current = pending.back();
    pending.pop_back();
    if (current->is_lazy()) {
      size_t stride = tn::GetTypeSize(current->type_id());
      if (!stride) stride = sizeof(std::string);  // string-family arrays
      const size_t count = current->array_size();
      if (count > (kMaxLayerJSONBytes / 16 - bytes) / stride) return SIZE_MAX;
      bytes += count * stride;
    }
    if (auto dict = current->as_dictionary()) {
      for (const auto& entry : dict->entries()) pending.push_back(entry.second);
    }
  }
  return bytes;
}

bool ChargeLayerJSONValue(tn::ValueView value, size_t elements,
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

std::string LayerJSONValueText(tn::ValueView value) {
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
  const tn::PrimMetadata& prim_meta = prim->meta();
  const tn::ArcListOps* arc_edits = prim_meta.arc_edits();
  const auto encode_path_ops = [&](const char* field,
                                   const std::vector<std::string>& paths,
                                   const tn::ArcListOp* edit,
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
  const tn::StringListOps& variant_edits = prim_meta.variantSetNameEdits();
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
      for (const tn::PrimVariantSetData& set : prim_meta.variantSets())
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
                                      tn::ValueView value) {
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
    const tn::ArcListOp* edit = edit_it == prim->relationship_edits().end()
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

}  // namespace

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
  const auto add_dictionary_meta = [&](const char* key, tn::ValueView value,
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

}  // namespace web_next
}  // namespace lightusd
