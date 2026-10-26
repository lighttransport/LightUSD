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
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
namespace tn = lightusd::next;
#include "security-policy.hh"
#include "next/parser/lexer.hh"
#include "next/parser/value-parser.hh"
#include "next/parser/ascii-parser.hh"
#include "next/writer/value-printer.hh"
#include <string_view>

namespace lightusd {
namespace web_next {
using namespace layer_json_detail;
namespace {
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
                                  tn::ArcListOp* edit,
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
        *edit = tn::ArcListOp();
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
    tn::ArcListOps& edits = prim->meta().ensure_arc_edits();
    if (!parse_path_ops("inherits", &prim->meta().inherits.mut(), &edits.inherits) ||
        !parse_path_ops("specializes", &prim->meta().specializes.mut(),
                        &edits.specializes) ||
        !parse_path_ops("references", &prim->meta().references.mut(),
                        &edits.references, true) ||
        !parse_path_ops("payloads", &prim->meta().payloads.mut(),
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
    tn::StringListOps& edits = prim->meta().variantSetNameEdits();
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
        edits = tn::StringListOps();
        edits.authored = true;
        edits.is_explicit = true;
        edits.explicit_items = std::move(names);
      } else {
        if (edits.is_explicit) {
          edits = tn::StringListOps();
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
      tn::PrimVariantSetData set;
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
        for (tn::PrimVariantSetData& set : prim->meta().variantSets()) {
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
          tn::ArcListOp& edit = prim->ensure_relationship_edit(prop_name);
          edit.authored = true;
          edit.is_explicit = true;
          edit.is_value_block = blocked;
        } else if (list_qualifier != "resetToExplicit") {
          tn::ArcListOp& edit = prim->ensure_relationship_edit(prop_name);
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
            auto text = parsed.as_string();
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

}  // namespace

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

}  // namespace web_next
}  // namespace lightusd
