// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "crate-reader-internal.hh"
#include "../parser/lexer.hh"
#include "../parser/value-parser.hh"
#include <utility>

namespace lightusd {
namespace next {
namespace crate_stage_detail {
// Decode dictionary fields preserved as USDA text by older crate writers.
// Return an empty Value on failure, matching ordinary field decoding.
Value ParseDictText(const std::string& text) {
  if (text.empty()) return Value();
  Lexer lexer(text.data(), text.size());
  ParseResult r = ParseDict(lexer);
  return r.success ? std::move(r.value) : Value();
}

DecodedExtensionField MakeTypedExtensionField(std::string name,
                                                   Value value) {
  DecodedExtensionField extension;
  extension.name = std::move(name);
  extension.value = std::move(value);
  extension.unregistered = true;
  if (auto source = extension.value.as_string()) {
    extension.unregistered_source = *source;
    Value dictionary = ParseDictText(*source);
    if (dictionary.is_dictionary()) {
      extension.value = std::move(dictionary);
    } else {
      // The raw text is the authored USDA spelling (quotes included for
      // strings). Reduce it to a typed value when it parses cleanly so the
      // USDA writer's PrintValue quotes once instead of re-quoting the raw
      // text; unparseable text (e.g. "$Side") keeps the string as-is.
      // unregistered_source stays verbatim — the crate rewrite contract.
      Lexer value_lexer(extension.unregistered_source.data(),
                        extension.unregistered_source.size());
      TypeId inferred = TypeId::Invalid;
      ParseResult parsed = ParseGenericValue(value_lexer, inferred);
      if (parsed.success && inferred != TypeId::Invalid &&
          !value_lexer.has_error() && !value_lexer.has_fatal_error() &&
          value_lexer.peek().type == TokenType::Eof) {
        extension.value = std::move(parsed.value);
      }
    }
  }
  return extension;
}

}  // namespace crate_stage_detail

using namespace crate_stage_detail;

void CrateReader::Impl::DecodeStageMetadata(
    Layer& layer,
    std::unordered_map<std::string, std::vector<std::string>>& prim_children_order) {
  for (const auto& spec : specs_) {
    if (spec.spec_type != SpecType::PseudoRoot) continue;
    if (spec.path_index.value >= paths_.size()) continue;

    std::vector<std::pair<std::string_view, Value>> fields;
    if (!ResolveFieldset(spec.fieldset_index.value, fields)) {
      AddWarning("Failed to resolve pseudo-root fieldset");
    }

    for (auto& field : fields) {
      if (field.first == "primChildren") {
        if (auto names =
                field.second.as_token_array()) {
          if (!names->empty()) prim_children_order["/"].assign(names->begin(), names->end());
        }
      } else if (field.first == "primOrder") {
        layer.meta().rootPrimOrder_set = true;
        if (auto names =
                field.second.as_token_array()) {
          layer.meta().rootPrimOrder.assign(names->begin(), names->end());
        }
      } else if (field.first == "defaultPrim") {
        layer.meta().defaultPrim_set = true;
        if (auto s = field.second.as_token())
          layer.meta().defaultPrim = *s;
      } else if (field.first == "upAxis") {
        if (auto s = field.second.as_token()) {
          layer.meta().upAxis = *s;
          layer.meta().upAxis_set = true;
        }
      } else if (field.first == "metersPerUnit") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().metersPerUnit = *d;
          layer.meta().metersPerUnit_set = true;
        }
      } else if (field.first == "hasOwnedSubLayers") {
        if (const bool* b = field.second.as_bool()) {
          layer.meta().hasOwnedSubLayers = *b;
          layer.meta().hasOwnedSubLayers_set = true;
        }
      } else if (field.first == "timeCodesPerSecond") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().timeCodesPerSecond = *d;
          layer.meta().timeCodesPerSecond_set = true;
        }
      } else if (field.first == "startTimeCode") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().startTimeCode = *d;
          layer.meta().startTimeCode_set = true;
        }
      } else if (field.first == "endTimeCode") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().endTimeCode = *d;
          layer.meta().endTimeCode_set = true;
        }
      } else if (field.first == "framesPerSecond") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().framesPerSecond = *d;
          layer.meta().framesPerSecond_set = true;
        }
      } else if (field.first == "kilogramsPerUnit") {
        const double* d = field.second.as_double();
        if (d) {
          layer.meta().kilogramsPerUnit = *d;
          layer.meta().kilogramsPerUnit_set = true;
        }
      } else if (field.first == "customLayerData" ||
                 field.first == "expressionVariables") {
        // pxr-authored: binary VtDictionary; next-authored: USDA dict text.
        Value d;
        if (field.second.is_dictionary()) {
          d = std::move(field.second);
        } else if (auto s = field.second.as_string()) {
          d = ParseDictText(*s);
        } else if (auto s = field.second.as_token()) {
          d = ParseDictText(*s);
        }
        if (d.is_dictionary()) {
          if (field.first == "customLayerData") {
            layer.meta().customLayerData = std::move(d);
            layer.meta().customLayerData_set = true;
          } else {
            layer.meta().expressionVariables = std::move(d);
            layer.meta().expressionVariables_set = true;
          }
        }
      } else if (field.first == "layerRelocates" ||
                 field.first == "relocates") {
        layer.meta().relocates_set = true;
        if (auto pairs =
                field.second.as_token_array()) {
          for (size_t i = 0; i + 1 < pairs->size(); i += 2) {
            layer.meta().relocates.emplace_back((*pairs)[i], (*pairs)[i + 1]);
          }
        }
      } else if (field.first == "colorConfiguration") {
        if (auto s = field.second.as_asset_path()) {
          layer.meta().colorConfiguration = *s;
          layer.meta().colorConfiguration_set = true;
        } else if (auto s = field.second.as_token()) {
          layer.meta().colorConfiguration = *s;
          layer.meta().colorConfiguration_set = true;
        }
      } else if (field.first == "colorManagementSystem") {
        if (auto s = field.second.as_token()) {
          layer.meta().colorManagementSystem = *s;
          layer.meta().colorManagementSystem_set = true;
        } else if (auto s = field.second.as_string()) {
          layer.meta().colorManagementSystem = *s;
          layer.meta().colorManagementSystem_set = true;
        }
      } else if (field.first == "renderSettingsPrimPath") {
        if (auto s = field.second.as_string()) {
          layer.meta().renderSettingsPrimPath = *s;
          layer.meta().renderSettingsPrimPath_set = true;
        } else if (auto s = field.second.as_token()) {
          layer.meta().renderSettingsPrimPath = *s;
          layer.meta().renderSettingsPrimPath_set = true;
        }
      } else if (field.first == "documentation" || field.first == "doc") {
        if (auto s = field.second.as_string()) {
          layer.meta().doc = *s;
          layer.meta().doc_set = true;
        }
      } else if (field.first == "comment") {
        if (auto s = field.second.as_string()) {
          layer.meta().comment = *s;
          layer.meta().comment_set = true;
        }
      } else if (field.first == "owner") {
        if (auto s = field.second.as_string()) {
          layer.meta().owner = *s;
          layer.meta().owner_set = true;
        } else if (auto s = field.second.as_token()) {
          layer.meta().owner = *s;
          layer.meta().owner_set = true;
        }
      } else if (field.first == "autoPlay") {
        if (const bool* value = field.second.as_bool()) {
          layer.meta().autoPlay = *value;
          layer.meta().autoPlay_set = true;
        }
      } else if (field.first == "playbackMode") {
        auto value = field.second.as_token();
        if (!value) value = field.second.as_string();
        if (value) {
          layer.meta().playbackMode = *value == "loop" ? "loop" : "none";
          layer.meta().playbackMode_set = true;
        }
      } else if (field.first == "__lightusd_unknownMeta") {
        // lightusd-private: length-prefixed (key, raw-value) pairs of unmodeled
        // LAYER metadata the parser preserved (see the writer).
        auto s = field.second.as_string();
        if (!s) s = field.second.as_token();
        if (s) {
          size_t p = 0;
          auto read_chunk = [&](std::string& out) -> bool {
            size_t colon = s->find(':', p);
            if (colon == std::string::npos) return false;
            size_t len = 0;
            for (size_t i = p; i < colon; ++i) {
              if ((*s)[i] < '0' || (*s)[i] > '9') return false;
              len = len * 10 + static_cast<size_t>((*s)[i] - '0');
              // A chunk cannot exceed the blob; capping here also prevents the
              // accumulation and the bounds check below from overflowing on a
              // hostile length (which would wrap past the check and re-parse the
              // same colon forever -> DoS on a crafted crate).
              if (len > s->size()) return false;
            }
            if (len > s->size() - (colon + 1)) return false;  // cannot wrap
            out = s->substr(colon + 1, len);
            p = colon + 1 + len;  // colon >= p, so p strictly increases
            return true;
          };
          std::string key, val;
          while (p < s->size() && read_chunk(key) && read_chunk(val)) {
            layer.meta().unknownMeta.emplace_back(std::move(key),
                                                  std::move(val));
          }
        }
      } else if (field.first == "subLayerOffsets") {
        if (auto arr = field.second.as_double_array()) {
          layer.meta().subLayerOffsets.clear();
          for (size_t i = 0; i + 1 < arr->size(); i += 2) {
            layer.meta().subLayerOffsets.emplace_back((*arr)[i], (*arr)[i + 1]);
          }
        }
      } else if (field.first == "subLayers") {
        layer.meta().subLayers_set = true;
        if (auto arr = field.second.as_token_array()) {
          for (const auto& s : *arr) layer.meta().subLayers.push_back(s);
        } else if (auto s = field.second.as_string()) {
          layer.meta().subLayers.push_back(*s);
        } else if (auto s = field.second.as_token()) {
          layer.meta().subLayers.push_back(*s);
        }
      } else {
        // Preserve decodable extension metadata as a typed field. ResolveFields
        // has already unpacked this ValueRep, so retaining it is lossless for
        // every type the writer can encode.
        layer.meta().unknownFields.push_back(
            MakeTypedExtensionField(std::string(field.first), field.second));
      }
    }
    break; // Only process first PseudoRoot
  }
}

}  // namespace next
}  // namespace lightusd
