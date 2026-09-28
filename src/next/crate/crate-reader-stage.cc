// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader stage reconstruction

#include "crate-reader-internal.hh"
#include "crate-timing.hh"
#include "../layer/array-edit.hh"
#include "../layer/layer.hh"
#include "../parser/lexer.hh"          // dict-as-USDA-text decode
#include "../parser/value-parser.hh"  // ParseDict
#include "../types/type-info.hh"
#include "../execution.hh"

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {

// Below these sizes the stage build stays serial: task setup and thread
// wake-up would cost more than the decode saves.
constexpr size_t kParallelBuildMinSpecs = 4096;
constexpr size_t kParallelBuildMinPrims = 512;

// Decode a dictionary that was written as USDA dict text in a String field
// (see crate-writer add_dict_field). Returns an empty Value on failure.
static Value ParseDictText(const std::string& text) {
  if (text.empty()) return Value();
  Lexer lexer(text.data(), text.size());
  ParseResult r = ParseDict(lexer);
  return r.success ? std::move(r.value) : Value();
}

static TypedExtensionField MakeTypedExtensionField(std::string name,
                                                   Value value) {
  TypedExtensionField extension;
  extension.name = std::move(name);
  extension.value = std::move(value);
  extension.unregistered = true;
  if (const std::string* source = extension.value.as_string()) {
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


// Decode one property-spec field into PropMeta, setting its authored bit.
// Returns false for field names that are not property metadata.
bool CrateReader::Impl::DecodePropMetaField(const std::string& name,
                                            ValueRep rep, PropMeta& pm) {
  auto tok_or_str = [&](std::string& dst, uint32_t bit) -> bool {
    Value v;
    if (!UnpackValue(rep, v)) return true;  // known field, bad value: eat it
    if (const std::string* s = v.as_token()) { dst = *s; pm.authored |= bit; }
    else if (const std::string* s = v.as_string()) { dst = *s; pm.authored |= bit; }
    return true;
  };
  auto dict_field = [&](Value& dst, uint32_t bit) -> bool {
    Value v;
    if (!UnpackValue(rep, v)) return true;
    if (v.is_dictionary()) { dst = std::move(v); pm.authored |= bit; }
    else if (const std::string* s = v.as_string()) {
      Value d = ParseDictText(*s);
      if (d.is_dictionary()) { dst = std::move(d); pm.authored |= bit; }
    } else if (const std::string* s = v.as_token()) {
      Value d = ParseDictText(*s);
      if (d.is_dictionary()) { dst = std::move(d); pm.authored |= bit; }
    }
    return true;
  };

  if (name == "interpolation") return tok_or_str(pm.interpolation, PropMeta::kInterpolation);
  if (name == "colorSpace") return tok_or_str(pm.colorSpace, PropMeta::kColorSpace);
  if (name == "renderType") return tok_or_str(pm.renderType, PropMeta::kRenderType);
  if (name == "connectability") return tok_or_str(pm.connectability, PropMeta::kConnectability);
  if (name == "outputName") return tok_or_str(pm.outputName, PropMeta::kOutputName);
  if (name == "bindMaterialAs") return tok_or_str(pm.bindMaterialAs, PropMeta::kBindMaterialAs);
  if (name == "kind") return tok_or_str(pm.kind, PropMeta::kKind);
  if (name == "permission") {
    return tok_or_str(pm.permission, PropMeta::kPermission);
  }
  if (name == "displayName") return tok_or_str(pm.displayName, PropMeta::kDisplayName);
  if (name == "displayGroup") return tok_or_str(pm.displayGroup, PropMeta::kDisplayGroup);
  if (name == "comment") return tok_or_str(pm.comment, PropMeta::kComment);
  if (name == "documentation" || name == "doc") return tok_or_str(pm.doc, PropMeta::kDoc);
  if (name == "elementSize") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const int32_t* i = v.as_int()) {
        pm.elementSize = *i;
        pm.authored |= PropMeta::kElementSize;
      }
    }
    return true;
  }
  if (name == "unauthoredValuesIndex") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const int32_t* i = v.as_int()) {
        pm.unauthoredValuesIndex = *i;
        pm.authored |= PropMeta::kUnauthoredIdx;
      }
    }
    return true;
  }
  if (name == "weight") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const double* d = v.as_double()) {
        pm.weight = *d;
        pm.authored |= PropMeta::kWeight;
      } else if (const float* f = v.as_float()) {
        pm.weight = *f;
        pm.authored |= PropMeta::kWeight;
      }
    }
    return true;
  }
  if (name == "hidden") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const bool* b = v.as_bool()) {
        pm.hidden = *b;
        pm.authored |= PropMeta::kHidden;
      }
    }
    return true;
  }
  if (name == "allowedTokens") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const std::vector<std::string>* toks = v.as_token_array()) {
        pm.allowedTokens = *toks;
        pm.authored |= PropMeta::kAllowedTokens;
      }
    }
    return true;
  }
  if (name == "customData") return dict_field(pm.customData, PropMeta::kCustomData);
  if (name == "assetInfo") return dict_field(pm.assetInfo, PropMeta::kAssetInfo);
  if (name == "sdrMetadata") return dict_field(pm.sdrMetadata, PropMeta::kSdrMetadata);
  return false;
}

bool CrateReader::Impl::BuildStage() {
  CratePhaseTimer timer(options_.enable_timing, "next_crate_stage");
  auto lap = [&](const char* phase) { timer.lap(phase); };
  // Create layer and builder
  Layer layer;
  LayerBuilder builder(layer);
  // Large production scenes can contain hundreds of thousands of specs. In
  // WASM every progress report crosses into JavaScript, where the worker also
  // checks its UI throttle. A coarser interval remains comfortably responsive
  // while avoiding thousands of otherwise-no-op JS/WASM transitions.
  constexpr size_t kReportInterval = 8192;
  auto report_stage = [&](const char* phase, size_t current,
                          size_t total) -> bool {
    if (current == 0 || current == total ||
        (current % kReportInterval) == 0) {
      return ReportProgress(phase, current, total);
    }
    return true;
  };

  // First, process the PseudoRoot spec to extract layer metadata
  // Authored child order per prim path (from primChildren fields; "/" = the
  // pseudo-root's list), applied after the sorted hierarchy build.
  std::unordered_map<std::string, std::vector<std::string>> prim_children_order;

  for (const auto& spec : specs_) {
    if (spec.spec_type != SpecType::PseudoRoot) continue;
    if (spec.path_index.value >= paths_.size()) continue;

    std::vector<std::pair<std::string, Value>> fields;
    if (!ResolveFieldset(spec.fieldset_index.value, fields)) {
      AddWarning("Failed to resolve pseudo-root fieldset");
    }

    for (auto& field : fields) {
      if (field.first == "primChildren") {
        if (const std::vector<std::string>* names =
                field.second.as_token_array()) {
          if (!names->empty()) prim_children_order["/"] = *names;
        }
      } else if (field.first == "primOrder") {
        layer.meta().rootPrimOrder_set = true;
        if (const std::vector<std::string>* names =
                field.second.as_token_array()) {
          layer.meta().rootPrimOrder = *names;
        }
      } else if (field.first == "defaultPrim") {
        layer.meta().defaultPrim_set = true;
        if (const std::string* s = field.second.as_token())
          layer.meta().defaultPrim = *s;
      } else if (field.first == "upAxis") {
        if (const std::string* s = field.second.as_token()) {
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
        } else if (const std::string* s = field.second.as_string()) {
          d = ParseDictText(*s);
        } else if (const std::string* s = field.second.as_token()) {
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
        if (const std::vector<std::string>* pairs =
                field.second.as_token_array()) {
          for (size_t i = 0; i + 1 < pairs->size(); i += 2) {
            layer.meta().relocates.emplace_back((*pairs)[i], (*pairs)[i + 1]);
          }
        }
      } else if (field.first == "colorConfiguration") {
        if (const std::string* s = field.second.as_asset_path()) {
          layer.meta().colorConfiguration = *s;
          layer.meta().colorConfiguration_set = true;
        } else if (const std::string* s = field.second.as_token()) {
          layer.meta().colorConfiguration = *s;
          layer.meta().colorConfiguration_set = true;
        }
      } else if (field.first == "colorManagementSystem") {
        if (const std::string* s = field.second.as_token()) {
          layer.meta().colorManagementSystem = *s;
          layer.meta().colorManagementSystem_set = true;
        } else if (const std::string* s = field.second.as_string()) {
          layer.meta().colorManagementSystem = *s;
          layer.meta().colorManagementSystem_set = true;
        }
      } else if (field.first == "renderSettingsPrimPath") {
        if (const std::string* s = field.second.as_string()) {
          layer.meta().renderSettingsPrimPath = *s;
          layer.meta().renderSettingsPrimPath_set = true;
        } else if (const std::string* s = field.second.as_token()) {
          layer.meta().renderSettingsPrimPath = *s;
          layer.meta().renderSettingsPrimPath_set = true;
        }
      } else if (field.first == "documentation" || field.first == "doc") {
        if (const std::string* s = field.second.as_string()) {
          layer.meta().doc = *s;
          layer.meta().doc_set = true;
        }
      } else if (field.first == "comment") {
        if (const std::string* s = field.second.as_string()) {
          layer.meta().comment = *s;
          layer.meta().comment_set = true;
        }
      } else if (field.first == "owner") {
        if (const std::string* s = field.second.as_string()) {
          layer.meta().owner = *s;
          layer.meta().owner_set = true;
        } else if (const std::string* s = field.second.as_token()) {
          layer.meta().owner = *s;
          layer.meta().owner_set = true;
        }
      } else if (field.first == "autoPlay") {
        if (const bool* value = field.second.as_bool()) {
          layer.meta().autoPlay = *value;
          layer.meta().autoPlay_set = true;
        }
      } else if (field.first == "playbackMode") {
        const std::string* value = field.second.as_token();
        if (!value) value = field.second.as_string();
        if (value) {
          layer.meta().playbackMode = *value == "loop" ? "loop" : "none";
          layer.meta().playbackMode_set = true;
        }
      } else if (field.first == "__lightusd_unknownMeta") {
        // lightusd-private: length-prefixed (key, raw-value) pairs of unmodeled
        // LAYER metadata the parser preserved (see the writer).
        const std::string* s = field.second.as_string();
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
        if (const std::vector<double>* arr = field.second.as_double_array()) {
          layer.meta().subLayerOffsets.clear();
          for (size_t i = 0; i + 1 < arr->size(); i += 2) {
            layer.meta().subLayerOffsets.emplace_back((*arr)[i], (*arr)[i + 1]);
          }
        }
      } else if (field.first == "subLayers") {
        layer.meta().subLayers_set = true;
        if (const std::vector<std::string>* arr = field.second.as_token_array()) {
          for (const auto& s : *arr) layer.meta().subLayers.push_back(s);
        } else if (const std::string* s = field.second.as_string()) {
          layer.meta().subLayers.push_back(*s);
        } else if (const std::string* s = field.second.as_token()) {
          layer.meta().subLayers.push_back(*s);
        }
      } else {
        // Preserve decodable extension metadata as a typed field. ResolveFields
        // has already unpacked this ValueRep, so retaining it is lossless for
        // every type the writer can encode.
        layer.meta().unknownFields.push_back(
            MakeTypedExtensionField(field.first, field.second));
      }
    }
    break; // Only process first PseudoRoot
  }

  // Process prim specs to build prims with hierarchy from paths
  struct PrimEntry {
    std::string full_path;
    std::string name;
    std::string type_name;
    PrimSpecifier specifier;
    uint32_t fieldset_index = 0;
    uint32_t path_index = 0;  // PATHS slot of full_path
  };

  // Variant content lives in the layer as bracketed holder/child prims
  // ("/Prim/{vset=sel}" + descendants); their attributes attach to those prims
  // via attr_map (so the layer fully represents the variants, enabling both the
  // compositor — which reads the selected holder — and unflattened round-trip).
  // We only need to know, per owning prim, which sets are selected.
  //   owning prim path -> { variantSet -> selected variant } (all selections).
  std::unordered_map<std::string, std::map<std::string, std::string>> variant_sel;
  // Variant OPTION names per owning prim/set, recovered from the holder specs
  // ("/Prim/{set=var}"): lets the model expose the full option list (not just
  // the selection) after a crate load.
  std::unordered_map<std::string, std::map<std::string, std::vector<std::string>>>
      variant_opts;

  lap("pseudoroot");
  // Prim / variant-holder specs of [spec_begin, spec_end), in spec order.
  // Variant option and selection records are applied to variant_opts /
  // variant_sel afterwards, in the same order, so the maps end up exactly as
  // an in-loop update would leave them. Shared by the serial and the parallel
  // (per spec-range task) collect; `report` only on the serial path.
  struct VariantOptRecord {
    std::string owner;
    std::string set;
    std::string option;
  };
  struct VariantSelRecord {
    std::string prim_path;
    std::vector<std::pair<std::string, std::string>> sels;
  };
  struct PrimRecords {
    std::vector<PrimEntry> entries;
    std::vector<VariantOptRecord> opts;
    std::vector<VariantSelRecord> sels;
  };
  auto collect_prims =
      [&](size_t spec_begin, size_t spec_end, PrimRecords& out,
          std::vector<std::pair<std::string, ValueRep>>& raw_field_scratch,
          std::vector<std::pair<std::string, Value>>& value_field_scratch,
          bool report) -> bool {
    for (size_t spec_index = spec_begin; spec_index < spec_end; ++spec_index) {
      if (report && spec_index > 0 &&
          !report_stage("stage.prims", spec_index, specs_.size())) {
        return false;
      }
      const auto& spec = specs_[spec_index];
      // Build Prim specs AND variant-namespace holders (Variant/VariantSet specs
      // at "/Prim/{vset=sel}" / "/Prim/{vset=}"). The holders are needed so that
      // variant CHILD prims ("/Prim/{vset=sel}/Geo") get a correct path from the
      // depth-stack builder (begin_prim recomputes path from its parent). Holders
      // and variant sub-prims are grafted onto the owning prim by the compositor
      // on selection and skipped by the writer (their paths contain '{').
      const bool is_prim = spec.spec_type == SpecType::Prim;
      const bool is_variant = spec.spec_type == SpecType::Variant ||
                              spec.spec_type == SpecType::VariantSet;
      if (!is_prim && !is_variant) continue;

      if (spec.path_index.value >= paths_.size()) continue;
      std::string full_path = paths_.str(spec.path_index.value);
      if (full_path.empty() || full_path == "/") continue;
      const bool bracketed = full_path.find('{') != std::string::npos;
      // Only real (non-bracketed) Prim specs carry a variantSelection.
      if (is_variant && !bracketed) continue;  // defensive

      // Record option names from holder specs "<owner>/{set=var}". The owner
      // is the path before the LAST bracketed component and may itself be a
      // holder ("/P/{a=x}/{b=y}" — a nested variant set on option {a=x}).
      if (bracketed && full_path.back() == '}') {
        size_t lb = full_path.rfind("/{");
        if (lb != std::string::npos) {
          const std::string nm = full_path.substr(lb + 2,
                                                  full_path.size() - lb - 3);
          size_t eq = nm.find('=');
          if (eq != std::string::npos && eq + 1 < nm.size() &&
              nm.find('{') == std::string::npos) {
            out.opts.push_back(VariantOptRecord{full_path.substr(0, lb),
                                                nm.substr(0, eq),
                                                nm.substr(eq + 1)});
          }
        }
      }

      // Decode the prim's variantSelection (a VariantSelectionMap that
      // UnpackValue/ResolveFieldset cannot represent). Captures every selection
      // so prims with multiple variant sets compose correctly. Holder specs
      // carry a variantSelection too when they own NESTED variant sets.
      if (ResolveFieldsetRaw(spec.fieldset_index.value, raw_field_scratch)) {
        for (auto& f : raw_field_scratch) {
          if (f.first == "variantSelection") {
            std::vector<std::pair<std::string, std::string>> sels;
            if (DecodeVariantSelectionMap(f.second, sels)) {
              // Create the entry even for an authored-EMPTY selection map so
              // the authored bit survives the round trip.
              out.sels.push_back(VariantSelRecord{full_path, std::move(sels)});
            }
            break;
          }
        }
      }

      size_t last_slash = full_path.rfind('/');
      std::string prim_name = (last_slash != std::string::npos && last_slash < full_path.size() - 1)
                              ? full_path.substr(last_slash + 1) : full_path;
      if (prim_name.empty()) continue;

      PrimEntry entry;
      entry.full_path = full_path;
      entry.name = prim_name;
      entry.fieldset_index = spec.fieldset_index.value;
      entry.path_index = spec.path_index.value;

      ResolveFieldset(spec.fieldset_index.value, value_field_scratch);

      // Untyped prims stay untyped: composition fills the type from the
      // referenced prim (a forced "Xform" default would mask e.g. a referenced
      // Mesh definition; the writer already skips empty type names).
      entry.type_name.clear();
      // A crate spec with NO `specifier` field composes as `over` in pxr —
      // for variant holder specs (which never carry one) AND for prim specs
      // (prim-with-no-specifier-001; pxr writes the field for every def, so
      // real crates are unaffected). A Def default here wrongly promoted
      // specifier-less specs to def during composition.
      entry.specifier = PrimSpecifier::Over;
      for (auto& f : value_field_scratch) {
        if (f.first == "typeName") {
          if (const std::string* s = f.second.as_token()) entry.type_name = *s;
          // Legacy "no prim type" spelling (pxr composes it as untyped).
          if (entry.type_name == "__AnyType__") entry.type_name.clear();
        } else if (f.first == "specifier") {
          if (const std::string* s = f.second.as_token()) {
            if (*s == "def") entry.specifier = PrimSpecifier::Def;
            else if (*s == "over") entry.specifier = PrimSpecifier::Over;
            else if (*s == "class") entry.specifier = PrimSpecifier::Class;
          }
        }
      }
      out.entries.push_back(std::move(entry));
    }
    return true;
  };

  std::vector<PrimEntry> prim_entries;
  const int build_threads = ResolveBuildThreads();
  if (!report_stage("stage.prims", 0, specs_.size())) return false;
  {
    std::vector<PrimRecords> prim_records;
    bool prims_collected = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && specs_.size() >= kParallelBuildMinSpecs) {
      const size_t task_size = std::max<size_t>(
          1024, specs_.size() / (static_cast<size_t>(build_threads) * 8));
      const size_t ntasks = (specs_.size() + task_size - 1) / task_size;
      prim_records.resize(ntasks);
      std::vector<std::unique_ptr<ThreadDecodeCtx>> ctxs(ntasks);
      TaskArena arena(static_cast<size_t>(build_threads));
      arena.Run(ntasks, [&](size_t t) {
        ctxs[t].reset(new ThreadDecodeCtx(this, *reader_));
        ScopedThreadDecodeCtx scoped(ctxs[t].get());
        std::vector<std::pair<std::string, ValueRep>> raw_scratch;
        std::vector<std::pair<std::string, Value>> value_scratch;
        collect_prims(t * task_size,
                      std::min(specs_.size(), (t + 1) * task_size),
                      prim_records[t], raw_scratch, value_scratch,
                      /*report=*/false);
      });
      for (auto& ctx : ctxs) MergeDecodeCtx(*ctx);
      prims_collected = true;
    }
#endif
    if (!prims_collected) {
      prim_records.resize(1);
      std::vector<std::pair<std::string, ValueRep>> raw_field_scratch;
      std::vector<std::pair<std::string, Value>> value_field_scratch;
      if (!collect_prims(0, specs_.size(), prim_records[0], raw_field_scratch,
                         value_field_scratch, /*report=*/true)) {
        return false;
      }
    }
    size_t total_entries = 0;
    for (const PrimRecords& r : prim_records) total_entries += r.entries.size();
    prim_entries.reserve(total_entries);
    for (PrimRecords& r : prim_records) {
      for (PrimEntry& e : r.entries) prim_entries.push_back(std::move(e));
      for (VariantOptRecord& o : r.opts) {
        variant_opts[o.owner][o.set].push_back(std::move(o.option));
      }
      for (VariantSelRecord& v : r.sels) {
        // Create the entry even for an authored-EMPTY selection map so the
        // authored bit survives the round trip.
        auto& per_prim = variant_sel[v.prim_path];
        for (auto& kv : v.sels) per_prim[kv.first] = kv.second;
      }
    }
  }
  if (!report_stage("stage.prims", specs_.size(), specs_.size())) {
    return false;
  }

  // Collect separate Attribute and Relationship specs. pxrUSD-authored crates
  // store each property as its own spec at "<primpath>/<name>" (rendered with a
  // leading '.' marker by ReadPaths), rather than inline in the prim's
  // fieldset. We capture, for each property: its declared typeName, its
  // `default` value (lazy if an array — no payload decode), any connection
  // targets, relationship targets, and uniform variability. These are attached
  // to the parent prim below so the writer can faithfully re-emit them.
  //
  // Rarely-authored parts live behind lazily allocated pointers: a property
  // record is otherwise ~1.4 KB (PropMeta alone is ~0.9 KB), and a large scene
  // holds millions of them between the collect and the body pass.
  struct AttrCold {
    ArrayEditData array_edit;  // VtArrayEdit default (crate 0.14)
    std::vector<std::string> connection_targets;
    ArcEdit connection_edit;
    std::string spline_text;  // Crate type-59 spline, decoded to USDA text
  };
  struct AttrInfo {
    std::string name;
    std::string type_name;
    bool has_default = false;
    bool has_array_edit = false;
    bool is_connection = false;  // implies `cold`
    bool uniform = false;
    bool custom = false;
    Value default_value;
    std::vector<std::pair<double, Value>> time_samples;
    std::unique_ptr<AttrCold> cold;
    // Per-property metadata (round-tripped via attribute spec fields);
    // `meta->authored` bits record which fields were present.
    std::unique_ptr<PropMeta> meta;
    AttrCold& ensure_cold() {
      if (!cold) cold.reset(new AttrCold());
      return *cold;
    }
    PropMeta& ensure_meta() {
      if (!meta) meta.reset(new PropMeta());
      return *meta;
    }
  };
  struct RelInfo {
    std::string name;
    std::vector<std::string> targets;
    ArcEdit edits;        // authored list-op sublists (marker-decoded)
    bool custom = false;
    bool uniform = false;
    bool variability_authored = false;
    bool varying = false;
    // Relationships carry PropMeta too (doc/hidden/...).
    std::unique_ptr<PropMeta> meta;
    PropMeta& ensure_meta() {
      if (!meta) meta.reset(new PropMeta());
      return *meta;
    }
  };
  lap("prims");
  // Sort by full path (produces correct depth-first order with parents before
  // children). Done before the property collect so property specs can be
  // resolved to their owning entry while they are decoded. The order is total
  // (duplicate paths keep their spec order), so the parallel sort -- sorted
  // chunks, then pairwise merge rounds -- yields exactly the serial order.
  {
    std::vector<uint32_t> perm(prim_entries.size());
    for (size_t i = 0; i < perm.size(); ++i) perm[i] = static_cast<uint32_t>(i);
    auto entry_less = [&prim_entries](uint32_t a, uint32_t b) {
      const int c = prim_entries[a].full_path.compare(prim_entries[b].full_path);
      return c != 0 ? c < 0 : a < b;
    };
    bool sorted = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && perm.size() >= 65536) {
      size_t runs = 1;
      while (runs * 2 <= static_cast<size_t>(build_threads)) runs *= 2;
      std::vector<size_t> bound(runs + 1);
      for (size_t r = 0; r <= runs; ++r) bound[r] = perm.size() * r / runs;
      TaskArena arena(static_cast<size_t>(build_threads));
      arena.Run(runs, [&](size_t r) {
        std::sort(perm.begin() + bound[r], perm.begin() + bound[r + 1],
                  entry_less);
      });
      for (size_t width = 1; width < runs; width *= 2) {
        const size_t merges = runs / (width * 2);
        arena.Run(merges, [&](size_t m) {
          const size_t lo = bound[m * width * 2];
          const size_t mid = bound[m * width * 2 + width];
          const size_t hi = bound[m * width * 2 + width * 2];
          std::inplace_merge(perm.begin() + lo, perm.begin() + mid,
                             perm.begin() + hi, entry_less);
        });
      }
      sorted = true;
    }
#endif
    if (!sorted) std::sort(perm.begin(), perm.end(), entry_less);
    std::vector<PrimEntry> ordered;
    ordered.reserve(prim_entries.size());
    for (uint32_t i : perm) ordered.push_back(std::move(prim_entries[i]));
    prim_entries.swap(ordered);
  }
  lap("sort");

  // Owning prim path -> entry index (the first entry on a duplicate path).
  // Property specs attach to the entry whose path equals their prim part;
  // specs of prims with no entry are dropped, as before. Keys view the
  // entries' path strings, which are not moved from here on.
  std::unordered_map<std::string_view, uint32_t> entry_by_path;
  entry_by_path.reserve(prim_entries.size());
  for (size_t i = 0; i < prim_entries.size(); ++i) {
    entry_by_path.emplace(prim_entries[i].full_path, static_cast<uint32_t>(i));
  }
  std::vector<std::pair<std::string, ValueRep>> property_raw_field_scratch;

  // Split a property spec path ".<primpath>/<name>" into (primpath, name).
  auto split_prop_path = [](const std::string_view raw, std::string& prim_path,
                            std::string& prop_name) -> bool {
    if (raw.empty()) return false;
    size_t begin = (raw[0] == '.') ? 1 : 0;
    size_t slash = raw.rfind('/');
    if (slash == std::string_view::npos || slash < begin) return false;
    prim_path.assign(raw.substr(begin, slash - begin));
    prop_name.assign(raw.substr(slash + 1));
    return !prim_path.empty() && !prop_name.empty();
  };

  // Collect the Attribute / Relationship specs of [spec_begin, spec_end) into
  // (prim path, info) records in spec order. Shared verbatim by the serial
  // and the parallel (per spec-range task) collect; `report` is set only on
  // the serial path (progress callbacks must stay on the calling thread).
  // Records are (owning entry index, info), in spec order.
  using AttrRecords = std::vector<std::pair<uint32_t, AttrInfo>>;
  using RelRecords = std::vector<std::pair<uint32_t, RelInfo>>;
  // Fast path: the property's parent slot in the PATHS tree names its prim
  // entry directly; it is taken only when that entry's path equals the split
  // prim path (so the answer is identical to the hash lookup it skips).
  std::vector<uint32_t> slot_to_entry(paths_.size(), UINT32_MAX);
  for (const PrimEntry& e : prim_entries) {
    if (e.path_index < slot_to_entry.size()) {
      slot_to_entry[e.path_index] = entry_by_path.find(e.full_path)->second;
    }
  }
  auto owning_entry = [&](const std::string& prim_path,
                          uint32_t prop_slot) -> uint32_t {
    if (prop_slot < path_parent_.size()) {
      const uint32_t parent_slot = path_parent_[prop_slot];
      if (parent_slot < slot_to_entry.size()) {
        const uint32_t e = slot_to_entry[parent_slot];
        if (e != UINT32_MAX && prim_entries[e].full_path == prim_path) return e;
      }
    }
    auto it = entry_by_path.find(std::string_view(prim_path));
    return it == entry_by_path.end() ? UINT32_MAX : it->second;
  };
  auto collect_properties =
      [&](size_t spec_begin, size_t spec_end, AttrRecords& attr_out,
          RelRecords& rel_out,
          std::vector<std::pair<std::string, ValueRep>>& property_raw_field_scratch,
          bool report) -> bool {
    {
      // Exact-reserve the record sinks (no growth moves).
      size_t n_attr = 0;
      size_t n_rel = 0;
      for (size_t i = spec_begin; i < spec_end; ++i) {
        n_attr += specs_[i].spec_type == SpecType::Attribute;
        n_rel += specs_[i].spec_type == SpecType::Relationship;
      }
      attr_out.reserve(attr_out.size() + n_attr);
      rel_out.reserve(rel_out.size() + n_rel);
    }
    for (size_t spec_index = spec_begin; spec_index < spec_end; ++spec_index) {
      if (report && spec_index > 0 &&
          !report_stage("stage.properties", spec_index, specs_.size())) {
        return false;
      }
      const auto& spec = specs_[spec_index];
      const bool is_attr = spec.spec_type == SpecType::Attribute;
      const bool is_rel = spec.spec_type == SpecType::Relationship;
      if (!is_attr && !is_rel) continue;
      if (spec.path_index.value >= paths_.size()) continue;
      const std::string_view raw_path = paths_.view(spec.path_index.value);

      // Variant attributes (bracketed paths) attach to their bracketed holder /
      // child prim like any other attribute (split_prop_path handles them): e.g.
      // ".Prim/{vset=sel}/prop" -> prim_path "/Prim/{vset=sel}" (the holder),
      // ".Prim/{vset=sel}/Geo/attr" -> "/Prim/{vset=sel}/Geo" (a variant child).
      // The compositor reads the selected holder; the writer re-emits it.
      std::string prim_path, prop_name;
      if (!split_prop_path(raw_path, prim_path, prop_name)) {
        continue;
      }

      if (!ResolveFieldsetRaw(spec.fieldset_index.value, property_raw_field_scratch)) continue;

      if (is_rel) {
        RelInfo ri;
        ri.name = std::move(prop_name);
        GetPropNameTable().intern(ri.name);  // see the attribute branch
        for (auto& f : property_raw_field_scratch) {
          if (f.first == "targetPaths") {
            std::vector<std::string> raw;
            if (!DecodePathTargets(f.second, raw, /*with_markers=*/true)) {
              AddWarning("Failed to decode relationship targets on " +
                         prim_path + "." + ri.name);
              continue;
            }
            if (raw.size() == 1 && (raw[0] == "\x01" "E" || raw[0] == "\x01E")) {
              // Authored explicit-clear (`rel r = None`): a declared,
              // target-less relationship with an authored (explicit) edit.
              ri.edits = ArcEdit();
              ri.edits.authored = true;
              ri.edits.is_value_block = true;
              raw.clear();
            } else if (!raw.empty() && !raw[0].empty() && raw[0][0] == '\x01') {
              // Non-explicit list op: sublists are marker-delimited.
              ri.edits.authored = true;
              ri.edits.is_explicit = false;
              std::vector<std::string>* cur = nullptr;
              for (std::string& t : raw) {
                if (t == "\x01" "P") cur = &ri.edits.prepended;
                else if (t == "\x01" "G") cur = &ri.edits.added;
                else if (t == "\x01" "A") cur = &ri.edits.appended;
                else if (t == "\x01" "D") cur = &ri.edits.deleted;
                else if (t == "\x01" "O") cur = &ri.edits.ordered;
                else if (cur) cur->push_back(std::move(t));
              }
              // Within-spec effective list: prepended then appended.
              ri.targets = ri.edits.prepended;
              for (const std::string& item : ri.edits.added) {
                if (std::find(ri.targets.begin(), ri.targets.end(), item) ==
                    ri.targets.end()) ri.targets.push_back(item);
              }
              ri.targets.insert(ri.targets.end(), ri.edits.appended.begin(),
                                ri.edits.appended.end());
            } else {
              ri.targets = std::move(raw);
            }
          } else if (f.first == "custom") {
            Value v;
            if (UnpackValue(f.second, v)) {
              if (const bool* b = v.as_bool()) ri.custom = *b;
            } else {
              AddWarning("Failed to decode relationship custom flag on " +
                         prim_path + "." + ri.name);
            }
          } else if (f.first == "variability") {
            Value v;
            if (UnpackValue(f.second, v)) {
              if (const std::string* s = v.as_token()) {
                ri.variability_authored = true;
                ri.uniform = (*s == "uniform");
                ri.varying = (*s == "varying");
              }
            } else {
              AddWarning("Failed to decode relationship variability on " +
                         prim_path + "." + ri.name);
            }
          } else if (!DecodePropMetaField(f.first, f.second,
                                          ri.ensure_meta())) {
            Value value;
            if (UnpackValue(f.second, value)) {
              ri.meta->unknownFields.push_back(
                  MakeTypedExtensionField(f.first, std::move(value)));
              ri.meta->authored |= PropMeta::kUnknownMeta;
            } else {
              AddWarning("Unknown relationship field '" + f.first + "' on " +
                         prim_path + "." + ri.name + " is not decodable");
            }
          }
        }
        const uint32_t owner = owning_entry(prim_path, spec.path_index.value);
      if (owner != UINT32_MAX) rel_out.emplace_back(owner, std::move(ri));
        continue;
      }

      // Attribute spec.
      AttrInfo ai;
      ai.name = std::move(prop_name);
      // Intern now (in the parallel collect) so the body pass only hits the
      // name table's lock-free snapshot.
      GetPropNameTable().intern(ai.name);
      for (auto& f : property_raw_field_scratch) {
        if (f.first == "typeName") {
          Value v;
          if (UnpackValue(f.second, v)) {
            if (const std::string* s = v.as_token()) ai.type_name = *s;
          } else {
            AddWarning("Failed to decode typeName on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "default") {
          if (f.second.is_array_edit()) {
            // Sparse VtArrayEdit default: structured edit, not a Value.
            if (UnpackArrayEditData(f.second, &ai.ensure_cold().array_edit)) {
              ai.has_array_edit = true;
            }
          } else {
            Value v;
            if (UnpackValue(f.second, v)) {
              ai.default_value = std::move(v);
              ai.has_default = true;
            } else {
              AddWarning("Failed to decode default value on " + prim_path +
                         "." + ai.name);
            }
          }
        } else if (f.first == "timeSamples") {
          if (!DecodeTimeSamples(f.second, &ai.time_samples)) {
            AddWarning("Failed to decode timeSamples on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "spline") {
          // Crate type-59 spline: decode to USDA text (PrimSpec's storage form).
          if (!DecodeSplineToText(f.second, &ai.ensure_cold().spline_text)) {
            AddWarning("Failed to decode spline on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "connectionPaths") {
          std::vector<std::string> raw;
          if (DecodePathTargets(f.second, raw, /*with_markers=*/true)) {
            // Present-but-empty = authored connection block (`.connect = None`).
            ai.is_connection = true;
            AttrCold& cold = ai.ensure_cold();
            cold.connection_edit.authored = true;
            if (raw.size() == 1 && raw[0] == "\x01" "E") {
              cold.connection_edit.is_explicit = true;
            } else if (!raw.empty() && !raw[0].empty() && raw[0][0] == '\x01') {
              cold.connection_edit.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (std::string& item : raw) {
                if (item == "\x01" "G") current = &cold.connection_edit.added;
                else if (item == "\x01" "P") current = &cold.connection_edit.prepended;
                else if (item == "\x01" "A") current = &cold.connection_edit.appended;
                else if (item == "\x01" "D") current = &cold.connection_edit.deleted;
                else if (item == "\x01" "O") current = &cold.connection_edit.ordered;
                else if (current) current->push_back(std::move(item));
              }
              cold.connection_targets = cold.connection_edit.prepended;
              for (const std::string& item : cold.connection_edit.added) {
                if (std::find(cold.connection_targets.begin(),
                              cold.connection_targets.end(), item) ==
                    cold.connection_targets.end()) {
                  cold.connection_targets.push_back(item);
                }
              }
              cold.connection_targets.insert(cold.connection_targets.end(),
                                           cold.connection_edit.appended.begin(),
                                           cold.connection_edit.appended.end());
            } else {
              cold.connection_edit.is_explicit = true;
              cold.connection_targets = std::move(raw);
            }
          } else {
            AddWarning("Failed to decode connection paths on " + prim_path +
                       "." + ai.name);
          }
        } else if (f.first == "variability") {
          Value v;
          if (UnpackValue(f.second, v)) {
            if (const std::string* s = v.as_token()) ai.uniform = (*s == "uniform");
          } else {
            AddWarning("Failed to decode variability on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "custom") {
          // The legacy `custom` qualifier (pxr stores a bool field). Preserved on
          // read; the USDA writer only re-emits it under emit_custom/--openusd-compat.
          Value v;
          if (UnpackValue(f.second, v)) {
            if (const bool* b = v.as_bool()) ai.custom = *b;
          } else {
            AddWarning("Failed to decode custom flag on " + prim_path + "." +
                       ai.name);
          }
        } else if (!DecodePropMetaField(f.first, f.second,
                                        ai.ensure_meta())) {
          Value value;
          if (UnpackValue(f.second, value)) {
            ai.meta->unknownFields.push_back(
                MakeTypedExtensionField(f.first, std::move(value)));
            ai.meta->authored |= PropMeta::kUnknownMeta;
          } else {
            AddWarning("Unknown property field '" + f.first + "' on " +
                       prim_path + "." + ai.name + " is not decodable");
          }
        }
      }
      // Role types (texCoord2f, point3f, color3h, ...) exist in the crate only
      // via the declared typeName — the value itself is stored as its base type
      // (Vec2f, Vec3f, ...). Re-tag so the in-memory Value matches what the
      // usda parser produces for the same authoring.
      if (ai.has_default && !ai.type_name.empty()) {
        std::string base = ai.type_name;
        if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
          base.resize(base.size() - 2);
        }
        TypeId declared = GetTypeIdFromName(base.c_str());
        if (declared != TypeId::Invalid) ai.default_value.retag_role(declared);
      }
      const uint32_t owner = owning_entry(prim_path, spec.path_index.value);
      if (owner != UINT32_MAX) attr_out.emplace_back(owner, std::move(ai));
    }
    return true;
  };

  if (!report_stage("stage.properties", 0, specs_.size())) return false;
  bool properties_collected = false;
  std::vector<AttrRecords*> attr_lists;
  std::vector<RelRecords*> rel_lists;
  std::unique_ptr<AttrRecords> serial_attrs;
  std::unique_ptr<RelRecords> serial_rels;
#if defined(LIGHTUSD_ENABLE_THREAD)
  struct CollectTask {
    size_t begin = 0;
    size_t end = 0;
    AttrRecords attrs;
    RelRecords rels;
    std::unique_ptr<ThreadDecodeCtx> ctx;
  };
  std::vector<CollectTask> tasks;
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (build_threads > 1 && specs_.size() >= kParallelBuildMinSpecs) {
    // Parallel collect: contiguous spec ranges decode into task-local record
    // vectors (each task with its own stream cursor + diagnostics), merged in
    // range order below -- exactly the serial append order.
    const size_t task_size = std::max<size_t>(
        1024, specs_.size() / (static_cast<size_t>(build_threads) * 8));
    tasks.resize((specs_.size() + task_size - 1) / task_size);
    for (size_t t = 0; t < tasks.size(); ++t) {
      tasks[t].begin = t * task_size;
      tasks[t].end = std::min(specs_.size(), tasks[t].begin + task_size);
    }
    TaskArena arena(static_cast<size_t>(build_threads));
    arena.Run(tasks.size(), [&](size_t t) {
      CollectTask& task = tasks[t];
      task.ctx.reset(new ThreadDecodeCtx(this, *reader_));
      ScopedThreadDecodeCtx scoped(task.ctx.get());
      std::vector<std::pair<std::string, ValueRep>> scratch;
      collect_properties(task.begin, task.end, task.attrs, task.rels, scratch,
                         /*report=*/false);
    });
    for (CollectTask& task : tasks) {
      MergeDecodeCtx(*task.ctx);
      attr_lists.push_back(&task.attrs);
      rel_lists.push_back(&task.rels);
    }
    properties_collected = true;
  }
#endif
  if (!properties_collected) {
    serial_attrs.reset(new AttrRecords());
    serial_rels.reset(new RelRecords());
    if (!collect_properties(0, specs_.size(), *serial_attrs, *serial_rels,
                            property_raw_field_scratch, /*report=*/true)) {
      return false;
    }
    attr_lists.push_back(serial_attrs.get());
    rel_lists.push_back(serial_rels.get());
  }

  lap("properties");
  // Per-entry record lists (CSR over the record vectors, which stay alive
  // until the bodies have moved from them): entry i owns
  // refs[off[i] .. off[i+1]), in spec order.
  auto build_entry_index = [&](auto& lists, std::vector<uint32_t>& off,
                               auto& refs) {
    off.assign(prim_entries.size() + 1, 0);
    size_t total = 0;
    for (auto* list : lists) {
      for (auto& rec : *list) {
        ++off[rec.first + 1];
        ++total;
      }
    }
    for (size_t i = 0; i < prim_entries.size(); ++i) off[i + 1] += off[i];
    refs.resize(total);
    std::vector<uint32_t> cursor(off.begin(), off.end() - 1);
    for (auto* list : lists) {
      for (auto& rec : *list) refs[cursor[rec.first]++] = &rec.second;
    }
  };
  std::vector<uint32_t> attr_off;
  std::vector<uint32_t> rel_off;
  std::vector<AttrInfo*> attr_refs;
  std::vector<RelInfo*> rel_refs;
  build_entry_index(attr_lists, attr_off, attr_refs);
  build_entry_index(rel_lists, rel_off, rel_refs);
  // Every property name was interned during the collect above; publish
  // the name table's lock-free snapshot so the body pass never takes its lock.
  GetPropNameTable().publish_snapshot();
  if (!report_stage("stage.properties", specs_.size(), specs_.size())) {
    return false;
  }

  lap("property_index");
  // Emit, in two passes over the path-sorted entries:
  //  1. structure (serial): the depth-stack begin_prim/end_prim walk creates
  //     every PrimSpec (name/type/specifier/path + hierarchy links) exactly as
  //     the interleaved build did, recording each entry's prim index;
  //  2. bodies: decode each entry's fields/metadata and attach its property
  //     specs into its own PrimSpec. Bodies only touch their own PrimSpec (and
  //     read-only shared tables), so when no entry re-opened an earlier prim
  //     (duplicate path) they run as parallel tasks over contiguous entry
  //     ranges; diagnostics merge back in entry order.
  constexpr uint32_t kSkipEntry = UINT32_MAX;
  std::vector<uint32_t> entry_prim(prim_entries.size(), kSkipEntry);
  bool irregular_entries = false;
  if (!report_stage("stage.hierarchy", 0, prim_entries.size())) return false;
  layer.reserve(prim_entries.size());
  {
    size_t open_depth = 0;
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      const auto& entry = prim_entries[entry_index];
      // Compute depth of this prim (number of '/' in path)
      const size_t depth =
          std::count(entry.full_path.begin(), entry.full_path.end(), '/');
      if (depth == 0) continue;  // warned about in the body pass (in order)

      // Pop stack until we're at the correct parent level
      while (open_depth > depth - 1) {
        builder.end_prim();
        --open_depth;
      }

      // Begin this prim
      const size_t prims_before = layer.prim_count();
      entry_prim[entry_index] =
          builder.begin_prim(entry.name, entry.type_name, entry.specifier);
      // An entry that re-opened an earlier prim (duplicate path), or whose
      // depth-stack path differs from its crate path (a sibling name sorting
      // between a prim and its children), makes the entry -> prim mapping
      // irregular: bodies and reorders then run serially, in entry order,
      // exactly as the interleaved build did.
      if (layer.prim_count() == prims_before ||
          layer.prim(entry_prim[entry_index])->path().str() !=
              entry.full_path) {
        irregular_entries = true;
      }
      ++open_depth;
    }
    // Close remaining prims
    while (open_depth > 0) {
      builder.end_prim();
      --open_depth;
    }
  }

  lap("structure");
  // Authored primChildren order per entry (merged into prim_children_order
  // after the body pass, in entry order).
  std::vector<std::vector<std::string>> children_order(prim_entries.size());

  auto emit_prim_body =
      [&](size_t entry_index,
          std::vector<std::pair<std::string, Value>>& prim_value_field_scratch) {
    auto& entry = prim_entries[entry_index];
    if (entry_prim[entry_index] == kSkipEntry) {
      AddWarning("Ignoring malformed non-absolute prim path: " +
                 entry.full_path);
      return;
    }
    PrimSpec* ps = layer.prim(entry_prim[entry_index]);

    // Extracts a token-list metadata field (written as a token array, with
    // single-token / string fallbacks for older encodings). Warns rather than
    // silently dropping a known arc field that fails to decode.
    auto append_token_list = [&](const Value& v, std::vector<std::string>& dst,
                                 const char* field_name) {
      if (const std::vector<std::string>* arr = v.as_token_array()) {
        // Marker-aware: keep prepend/append sublist items, skip
        // deleted/ordered sublists, strip the markers themselves.
        bool keep = true;
        for (const auto& s : *arr) {
          if (!s.empty() && s[0] == '\x01') {
            keep = !(s == "\x01" "D" || s == "\x01" "O");
            continue;
          }
          if (keep) dst.push_back(s);
        }
      } else if (const std::string* s = v.as_token()) {
        dst.push_back(*s);
      } else if (const std::string* s = v.as_string()) {
        dst.push_back(*s);
      } else {
        AddWarning(std::string("Composition arc field '") + field_name +
                   "' has unexpected encoding; dropped");
      }
    };

    // Phase 7 S5: decode a `<arc>_listOp` companion token[] (marker-delimited
    // prepend/append/delete/order sublists) into an ArcEdit.
    auto decode_arc_listop = [&](const Value& v, ArcEdit& e) {
      const std::vector<std::string>* arr = v.as_token_array();
      if (!arr) return;
      // "\x01E" alone = authored explicit-clear (`references = None` /
      // pxr's explicit-empty listop): authored with is_explicit kept true
      // and no items, which the USDA writer re-emits as `= None`.
      if (arr->size() == 1 && ((*arr)[0] == "\x01" "E" || (*arr)[0] == "\x01E")) {
        e = ArcEdit();
        e.authored = true;
        return;
      }
      e.authored = true;
      e.is_explicit = false;
      std::vector<std::string>* cur = nullptr;
      for (const std::string& s : *arr) {
        // Both marker spellings are accepted: the correct two-byte form
        // ("\x01" "A") and the legacy single-byte form (in C++, "\x01A"
        // parses as the one char 0x1A because 'A' is a hex digit) written by
        // older next crates.
        if (s == "\x01" "G") cur = &e.added;
        else if (s == "\x01" "P" || s == "\x01P") cur = &e.prepended;
        else if (s == "\x01" "A" || s == "\x1A") cur = &e.appended;
        else if (s == "\x01" "D" || s == "\x1D") cur = &e.deleted;
        else if (s == "\x01" "O" || s == "\x01O") cur = &e.ordered;
        else if (s == "\x01" "N" || s == "\x01N") cur = nullptr;
        else if (cur) cur->push_back(s);
      }
    };

    ResolveFieldset(entry.fieldset_index, prim_value_field_scratch);

    // Add properties and extract composition arcs / metadata into PrimSpecMeta
    for (auto& field : prim_value_field_scratch) {
      if (field.first == "typeName" || field.first == "specifier") continue;

      // Reserved prim metadata stored inline in the prim spec (pxrUSD keeps
      // these in the prim's fieldset, not as separate property specs). Route
      // them to PrimSpecMeta so they do not leak in as phantom properties.
      if (ps) {
        if (field.first == "active") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().active = *b;
            ps->meta().active_authored = true;
          }
          continue;
        }
        if (field.first == "hidden") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().hidden = *b;
            ps->meta().hidden_authored = true;
          }
          continue;
        }
        if (field.first == "documentation" || field.first == "doc") {
          if (const std::string* s = field.second.as_string()) {
            ps->meta().doc() = *s;
            ps->meta().set_doc_authored();
          } else if (const std::string* s = field.second.as_token()) {
            ps->meta().doc() = *s;
            ps->meta().set_doc_authored();
          }
          continue;
        }
      }

      // Composition arc + metadata fields: store in PrimSpecMeta, not as
      // regular properties. (Guarded by ps; if current() were null we simply
      // skip them rather than crash.)
      if (ps) {
        if (field.first == "primOrder") {
          ps->meta().setPrimOrderAuthored();
          if (const std::vector<std::string>* names =
                  field.second.as_token_array()) {
            ps->meta().editPrimOrder() = *names;
          }
          continue;
        }
        if (field.first == "propertyOrder") {
          ps->meta().setPropertyOrderAuthored();
          if (const std::vector<std::string>* names =
                  field.second.as_token_array()) {
            ps->meta().editPropertyOrder() = *names;
          }
          continue;
        }
        if (field.first == "displayGroupOrder") {
          if (const std::vector<std::string>* names =
                  field.second.as_token_array()) {
            ps->meta().editDisplayGroupOrder() = *names;
            ps->meta().setDisplayGroupOrderAuthored();
          }
          continue;
        }
        if (field.first == "apiSchemas") {
          ps->meta().setApiSchemasAuthored();
          StringListOpEdits& edits = ps->meta().apiSchemaEdits();
          edits = StringListOpEdits();
          edits.authored = true;
          if (const std::vector<std::string>* arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              edits.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items = *arr;
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items, "apiSchemas");
          }

          std::vector<std::string> applied = edits.is_explicit
              ? edits.explicit_items
              : edits.added;
          if (!edits.is_explicit) {
            applied.insert(applied.begin(), edits.prepended.begin(),
                           edits.prepended.end());
            applied.insert(applied.end(), edits.appended.begin(),
                           edits.appended.end());
            std::vector<std::string> ordered;
            for (const std::string& schema : edits.ordered) {
              auto it = std::find(applied.begin(), applied.end(), schema);
              if (it != applied.end()) {
                ordered.push_back(*it);
                applied.erase(it);
              }
            }
            ordered.insert(ordered.end(), applied.begin(), applied.end());
            applied = std::move(ordered);
          }
          ps->meta().apiSchemas() = std::move(applied);
          if (!edits.prepended.empty()) {
            ps->meta().apiSchemasQualifier() = "prepend";
          } else if (!edits.appended.empty() || !edits.added.empty()) {
            ps->meta().apiSchemasQualifier() = "append";
          } else if (!edits.deleted.empty()) {
            ps->meta().apiSchemasQualifier() = "delete";
          }
          continue;
        }
        // Composition arc lists. A marker-delimited token array (first entry
        // "\x01?") is a non-explicit list-op decoded from the native crate
        // encoding: reconstruct the authored edits and derive the effective
        // (within-spec) list; otherwise it is a plain explicit list.
        auto is_marker_list = [](const Value& v) {
          const std::vector<std::string>* arr = v.as_token_array();
          return arr && !arr->empty() && !(*arr)[0].empty() &&
                 (*arr)[0][0] == '\x01';
        };
        auto apply_arc_field = [&](const Value& v,
                                   std::vector<std::string>& target,
                                   ArcEdit& (*sel)(ArcListOpEdits&),
                                   const char* what) {
          if (is_marker_list(v)) {
            ArcEdit& e = sel(ps->meta().ensure_arc_edits());
            decode_arc_listop(v, e);
            // Effective list within a single spec: prepended then appended
            // (delete/reorder operate on weaker opinions, not this list).
            target = e.prepended;
            for (const std::string& item : e.added) {
              if (std::find(target.begin(), target.end(), item) == target.end())
                target.push_back(item);
            }
            target.insert(target.end(), e.appended.begin(), e.appended.end());
          } else {
            append_token_list(v, target, what);
          }
        };
        if (field.first == "references") {
          apply_arc_field(field.second, ps->meta().references,
                          [](ArcListOpEdits& a) -> ArcEdit& { return a.references; },
                          "references");
          continue;
        }
        if (field.first == "payload") {
          apply_arc_field(field.second, ps->meta().payloads,
                          [](ArcListOpEdits& a) -> ArcEdit& { return a.payloads; },
                          "payload");
          continue;
        }
        if (field.first == "inherits" || field.first == "inheritPaths") {
          // pxr's crate field name is "inheritPaths"; next's own writer used
          // "inherits". Accept both.
          apply_arc_field(field.second, ps->meta().inherits,
                          [](ArcListOpEdits& a) -> ArcEdit& { return a.inherits; },
                          "inherits");
          continue;
        }
        if (field.first == "specializes") {
          apply_arc_field(field.second, ps->meta().specializes,
                          [](ArcListOpEdits& a) -> ArcEdit& { return a.specializes; },
                          "specializes");
          continue;
        }
        if (field.first == "references_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().references);
          continue;
        }
        if (field.first == "payload_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().payloads);
          continue;
        }
        if (field.first == "inherits_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().inherits);
          continue;
        }
        if (field.first == "specializes_listOp") {
          decode_arc_listop(field.second,
                            ps->meta().ensure_arc_edits().specializes);
          continue;
        }
        if (field.first == "variantSelection") {
          if (const std::string* s = field.second.as_token())
            ps->meta().variantSelection = *s;
          else if (const std::string* s = field.second.as_string())
            ps->meta().variantSelection = *s;
          continue;
        }
        if (field.first == "comment") {
          if (const std::string* s = field.second.as_token())
            ps->meta().comment() = *s;
          else if (const std::string* s = field.second.as_string())
            ps->meta().comment() = *s;
          ps->meta().set_comment_authored();
          continue;
        }
        if (field.first == "kind") {
          if (const std::string* s = field.second.as_token())
            ps->meta().kind() = *s;
          else if (const std::string* s = field.second.as_string())
            ps->meta().kind() = *s;
          ps->meta().setKindAuthored();
          continue;
        }
        if (field.first == "permission") {
          if (const std::string* s = field.second.as_token())
            ps->meta().permission() = *s;
          else if (const std::string* s = field.second.as_string())
            ps->meta().permission() = *s;
          continue;
        }
        if (field.first == "displayName") {
          if (const std::string* s = field.second.as_string())
            ps->meta().displayName() = *s;
          else if (const std::string* s = field.second.as_token())
            ps->meta().displayName() = *s;
          ps->meta().setDisplayNameAuthored();
          continue;
        }
        if (field.first == "relocates") {
          ps->meta().setRelocatesAuthored();
          if (const std::vector<std::string>* pairs =
                  field.second.as_token_array()) {
            for (size_t i = 0; i + 1 < pairs->size(); i += 2) {
              ps->meta().relocates().emplace_back((*pairs)[i],
                                                  (*pairs)[i + 1]);
            }
          }
          continue;
        }
        if (field.first == "instanceable") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().instanceable = *b;
            ps->meta().instanceable_authored = true;
          }
          continue;
        }
        if (field.first == "clipSets") {
          StringListOpEdits& edits = ps->meta().clipSetEdits();
          edits = StringListOpEdits();
          edits.authored = true;
          if (const std::vector<std::string>* arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items = *arr;
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items, "clipSets");
          }
          continue;
        }
        if (field.first == "customData" || field.first == "assetInfo" ||
            field.first == "sdrMetadata" || field.first == "clips") {
          // pxr-authored: a binary VtDictionary (already decoded to a Dictionary
          // Value). next-authored: USDA dict text in a String field.
          Value d;
          if (field.second.is_dictionary()) {
            d = std::move(field.second);
          } else if (const std::string* s = field.second.as_string()) {
            d = ParseDictText(*s);
          } else if (const std::string* s = field.second.as_token()) {
            d = ParseDictText(*s);
          }
          if (d.is_dictionary()) {
            if (field.first == "customData") {
              ps->meta().customData() = std::move(d);
              ps->meta().setCustomDataAuthored();
            } else if (field.first == "assetInfo") {
              ps->meta().assetInfo() = std::move(d);
              ps->meta().setAssetInfoAuthored();
            } else if (field.first == "sdrMetadata") {
              ps->meta().sdrMetadata() = std::move(d);
              ps->meta().setSdrMetadataAuthored();
            } else {
              ps->meta().clips() = std::move(d);
              ps->meta().setClipsAuthored();
            }
          }
          continue;
        }
        if (field.first == "variantSets" ||
            field.first == "variantSetNames") {
          // The AUTHORED `variantSets` declaration (`variantSetNames` is pxr's
          // StringListOp form). Reconstruct name-only entries; the selection /
          // holder content is grafted on later. Strip any list-op markers a
          // StringListOp carries so a bare set name survives. Skip sets already
          // present (e.g. from holder specs) to avoid duplicates.
          StringListOpEdits& edits = ps->meta().variantSetNameEdits();
          edits = StringListOpEdits();
          edits.authored = true;
          if (const std::vector<std::string>* arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              edits.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items = *arr;
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items,
                              "variantSets");
          }

          std::vector<std::string> names = edits.is_explicit
              ? edits.explicit_items
              : edits.added;
          if (!edits.is_explicit) {
            names.insert(names.begin(), edits.prepended.begin(),
                         edits.prepended.end());
            names.insert(names.end(), edits.appended.begin(),
                         edits.appended.end());
            std::vector<std::string> ordered_names;
            for (const std::string& ordered : edits.ordered) {
              auto it = std::find(names.begin(), names.end(), ordered);
              if (it != names.end()) {
                ordered_names.push_back(*it);
                names.erase(it);
              }
            }
            ordered_names.insert(ordered_names.end(), names.begin(), names.end());
            names = std::move(ordered_names);
          }
          for (const std::string& n : names) {
            bool have = false;
            for (const VariantSetData& evs : ps->meta().variantSets()) {
              if (evs.name == n) { have = true; break; }
            }
            if (have) continue;
            VariantSetData vsd;
            vsd.name = n;
            ps->meta().variantSets().push_back(std::move(vsd));
          }
          continue;
        }
        if (field.first == "__lightusd_unknownMeta") {
          // lightusd-private field: length-prefixed (key, raw-value) pairs of
          // unmodeled prim metadata the parser preserved (see the writer).
          const std::string* blob = field.second.as_string();
          if (!blob) blob = field.second.as_token();
          if (blob) {
            size_t p = 0;
            auto read_chunk = [&](std::string& out) -> bool {
              size_t colon = blob->find(':', p);
              if (colon == std::string::npos) return false;
              size_t len = 0;
              for (size_t i = p; i < colon; ++i) {
                if ((*blob)[i] < '0' || (*blob)[i] > '9') return false;
                len = len * 10 + static_cast<size_t>((*blob)[i] - '0');
                // Cap: a chunk cannot exceed the blob. Prevents the multiply and
                // the bounds check from overflowing on a hostile length (which
                // would wrap past the check and re-parse forever -> DoS).
                if (len > blob->size()) return false;
              }
              if (len > blob->size() - (colon + 1)) return false;  // cannot wrap
              out = blob->substr(colon + 1, len);
              p = colon + 1 + len;  // colon >= p, so p strictly increases
              return true;
            };
            std::string key, val;
            while (p < blob->size() && read_chunk(key) && read_chunk(val)) {
              ps->meta().unknownMeta().emplace_back(std::move(key),
                                                    std::move(val));
            }
          }
          continue;
        }
      }
      // A loose sibling "variability" field cannot be re-associated with a
      // property here; consume it so it does not surface as a stray property.
      // Per-property variability is decoded above through Attribute/Relationship
      // specs and preserved as kFlagUniform.
      if (field.first == "variability") continue;

      // Reserved Sdf children-key ordering fields. pxrUSD stores prim/property
      // order in these (SdfChildrenKeys); USDA flatten output does NOT emit them
      // as body attributes -- order is implicit in the authored child/property
      // sequence. Composition derives child order from child_indices() directly
      // (see cache.cc ComposeInto), so consume these so they do not leak as
      // phantom `token[] primChildren`/`properties` attributes.
      if (field.first == "primChildren") {
        // Authored sibling order: the hierarchy below is built in path-sorted
        // order, so capture the order here and restore the child links after
        // finalize (see the prim_children_order pass). Do NOT also set
        // primOrder()/rootPrimOrder: pxr writes `primChildren` on every prim as
        // the natural order and preserves it purely by emission order, never
        // by re-emitting a `reorder nameChildren`/`reorder rootPrims`
        // statement. Restoring child_indices already reproduces that emission
        // order; setting primOrder() here made every USDC->USDA round trip gain
        // a spurious reorder statement pxr never emits.
        if (const std::vector<std::string>* names =
                field.second.as_token_array()) {
          if (!names->empty()) {
            children_order[entry_index] = *names;
          }
        }
        continue;
      }
      if (field.first == "properties" ||
          field.first == "propertyChildren") {
        // pxr writes the `properties` field (natural spec order) for EVERY
        // prim; it is not an authored `reorder properties`. Synthesizing a
        // propertyOrder from it made every USDC->USDA round trip gain a
        // spurious `reorder properties` statement that pxr's own round trip
        // never emits (pxr drops the order and sorts). So consume the field
        // without pinning an order. Authored USDA `reorder properties` still
        // round-trips through the USDA path (parser -> writer); like pxr, it
        // is not preserved through a crate round trip.
        continue;
      }
      if (field.first == "variantChildren" ||
          field.first == "variantSetChildren") {
        continue;
      }

      // `variantSetNames` (the declared list of variant-set names) likewise must
      // not leak as a phantom `string[] variantSetNames` body property. The
      // compositor drives variants from the bracketed-holder specs + the
      // variantSelection (not from this list), and flatten output drops variant
      // metadata entirely (see cache.cc ComposeInto), so consume it.
      if (field.first == "variantSetNames") {
        continue;
      }

      // Unknown Prim fields are extension metadata, not body properties.
      // Preserve the decoded typed value and re-emit it as the same field.
      if (ps) {
        ps->meta().unknownFields().push_back(
            MakeTypedExtensionField(field.first, std::move(field.second)));
      }
    }

    // Attach separate Attribute / Relationship specs that belong to this prim.
    // The dedup guard keeps any inline field authoritative.
    if (ps) {
      for (uint32_t k = attr_off[entry_index]; k < attr_off[entry_index + 1];
           ++k) {
        AttrInfo& ai = *attr_refs[k];
        if (ps->property(ai.name)) continue;  // inline opinion wins
        uint16_t flags = 0;
        if (ai.uniform) flags |= PropSlot::kFlagUniform;
        if (ai.custom) flags |= PropSlot::kFlagCustom;
        if (ai.is_connection) flags |= PropSlot::kFlagConnection;
        // The writer gates timeSamples emission on the slot flag, so mark a
        // time-sampled attribute (the value lives in add_time_sample below).
        if (!ai.time_samples.empty()) flags |= PropSlot::kFlagTimeSampled;
        const bool is_array =
            ai.type_name.size() >= 2 &&
            ai.type_name.compare(ai.type_name.size() - 2, 2, "[]") == 0;
        if (ai.has_default) {
          ps->add_property(ai.name, std::move(ai.default_value), flags);
        } else if (ai.has_array_edit) {
          // Sparse array-edit default: a typed slot with the structured edit
          // and its canonical `edit [...]` text (usda writer re-emits it;
          // composition stacks/resolves it).
          uint16_t edit_flags = flags;
          if (is_array) edit_flags |= PropSlot::kFlagArray;
          std::string base_tn = is_array
              ? ai.type_name.substr(0, ai.type_name.size() - 2)
              : ai.type_name;
          ps->add_property_slot(GetPropNameTable().intern(ai.name),
                                GetTypeIdFromName(base_tn.c_str()),
                                edit_flags);
          ps->set_raw_default_source(ai.name,
                                     BuildArrayEditText(ai.cold->array_edit));
          ps->set_array_edit(ai.name, std::move(ai.cold->array_edit));
        } else {
          // Connection-only / declared-only / timeSamples-only attribute:
          // register a typed slot with no authored default so it round-trips.
          if (is_array) flags |= PropSlot::kFlagArray;
          std::string base = is_array
              ? ai.type_name.substr(0, ai.type_name.size() - 2)
              : ai.type_name;
          TypeId tid = GetTypeIdFromName(base.c_str());
          ps->add_property_slot(GetPropNameTable().intern(ai.name), tid, flags);
        }
        // Time samples (an attribute may have timeSamples with or without a
        // default).
        if (!ai.time_samples.empty()) {
          PropNameId nid = GetPropNameTable().intern(ai.name);
          for (auto& ts : ai.time_samples) {
            ps->add_time_sample(nid, ts.first, std::move(ts.second));
          }
        }
        if (!ai.type_name.empty()) {
          ps->set_property_type_name(ai.name, ai.type_name);
        }
        if (ai.is_connection && ai.cold->connection_targets.empty()) {
          ps->set_connection_block(ai.name);  // authored `.connect = None`
        }
        if (ai.cold && !ai.cold->spline_text.empty()) {
          ps->set_spline_source(ai.name, std::move(ai.cold->spline_text));
        }
        if (ai.cold) {
          for (const auto& t : ai.cold->connection_targets) {
            ps->add_connection(ai.name, Path(t));
          }
        }
        if (ai.is_connection) {
          ps->ensure_connection_edit(ai.name) =
              std::move(ai.cold->connection_edit);
        }
        // Per-property metadata (full PropMeta round-trip).
        if (ai.meta && ai.meta->authored != 0) {
          ps->ensure_property_meta(ai.name) = std::move(*ai.meta);
        }
        ai = AttrInfo();  // release the record's leftovers on this task
      }
    }
    if (ps) {
      for (uint32_t k = rel_off[entry_index]; k < rel_off[entry_index + 1];
           ++k) {
        RelInfo& ri = *rel_refs[k];
        // A target-less relationship stays a declared (empty) relationship;
        // relationships with targets push one Path per target.
        if (ri.targets.empty()) {
          if (!ps->relationship(ri.name)) {
            ps->set_relationship_targets(ri.name, {});
          }
        } else {
          for (const auto& t : ri.targets) ps->add_relationship(ri.name, Path(t));
        }
        if (ri.edits.authored) {
          ps->ensure_relationship_edit(ri.name) = std::move(ri.edits);
        }
        if (ri.custom) {
          ps->set_relationship_flags(
              ri.name, static_cast<uint16_t>(ps->relationship_flags(ri.name) |
                                             PropSlot::kFlagCustom));
        }
        if (ri.variability_authored) {
          uint16_t flags = PropSlot::kFlagVariabilityAuthored;
          if (ri.uniform) flags |= PropSlot::kFlagUniform;
          if (ri.varying) flags |= PropSlot::kFlagVarying;
          ps->set_relationship_flags(
              ri.name, static_cast<uint16_t>(ps->relationship_flags(ri.name) |
                                             flags));
        }
        if (ri.meta && ri.meta->authored != 0) {
          ps->ensure_property_meta(ri.name) = std::move(*ri.meta);
        }
        ri = RelInfo();  // release the record's leftovers on this task
      }
    }

    // Variant sets: attach a VariantSetData per set, carrying the selection
    // and the option NAMES (as empty VariantData entries — the compositor
    // ignores option entries without content/properties and grafts the
    // bracketed holder prims instead, which hold the variant CONTENT).
    auto sel = variant_sel.find(entry.full_path);
    auto opt = variant_opts.find(entry.full_path);
    if (ps && (sel != variant_sel.end() || opt != variant_opts.end())) {
      // Union of set names from selections and holder specs.
      std::map<std::string, VariantSetData> sets;
      if (opt != variant_opts.end()) {
        for (const auto& kv : opt->second) {
          VariantSetData& vsd = sets[kv.first];
          vsd.name = kv.first;
          for (const std::string& var : kv.second) {
            VariantData vd;
            vd.name = var;
            vsd.variants.push_back(std::move(vd));
          }
        }
      }
      if (sel != variant_sel.end()) {
        for (const auto& kv : sel->second) {
          // Only stamp the selection onto a variant set that is actually
          // DEFINED here (present in variant_opts / an existing set). A
          // dangling selection — `variants = {set=sel}` with no local
          // variantSet definition (the variant set lives in a referenced
          // layer) — must NOT synthesize a variantSets declaration or an empty
          // `variantSet` block; it is recorded only in variantSelections()
          // below. pxr emits neither for such a prim.
          auto it = sets.find(kv.first);
          if (it != sets.end()) it->second.selected = kv.second;
        }
      }
      for (auto& kv : sets) {
        // Merge into a same-named entry when one already exists (older
        // next-authored crates carry a legacy `variantSets` token field that
        // pre-pushed name-only entries; pushing a second full entry would
        // duplicate the set — grafted twice and emitted twice as USDA).
        VariantSetData* existing = nullptr;
        for (VariantSetData& evs : ps->meta().variantSets()) {
          if (evs.name == kv.first) {
            existing = &evs;
            break;
          }
        }
        if (existing) {
          if (existing->selected.empty()) {
            existing->selected = kv.second.selected;
          }
          for (VariantData& vd : kv.second.variants) {
            bool have = false;
            for (const VariantData& evd : existing->variants) {
              if (evd.name == vd.name) {
                have = true;
                break;
              }
            }
            if (!have) existing->variants.push_back(std::move(vd));
          }
        } else {
          ps->meta().variantSets().push_back(std::move(kv.second));
        }
      }
      // Record ALL selections in the plural list (the single legacy string
      // can carry only one set; consumers that read it alone would lose
      // every selection after the first on a multi-set prim).
      if (sel != variant_sel.end()) {
        // Authored bit even for an explicit-empty selection dict.
        ps->meta().setVariantSelectionsAuthored();
        if (!sel->second.empty()) {
          for (const auto& kv : sel->second) {
            ps->meta().variantSelections().emplace_back(kv.first, kv.second);
          }
          const auto& first = *sel->second.begin();
          ps->meta().variantSelection = first.first + "=" + first.second;
        }
      }
    }
    // Pre-sort the property index here (per prim, on the body task) so the
    // serial Layer::finalize() sweep finds it already sorted. Idempotent: a
    // later body on the same prim (irregular entries) just re-sorts.
    ps->finalize_properties();
  };

  bool bodies_emitted = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (build_threads > 1 && !irregular_entries &&
      prim_entries.size() >= kParallelBuildMinPrims) {
    struct EmitTask {
      size_t begin = 0;
      size_t end = 0;
      std::unique_ptr<ThreadDecodeCtx> ctx;
    };
    const size_t task_size = std::max<size_t>(
        128, prim_entries.size() / (static_cast<size_t>(build_threads) * 16));
    std::vector<EmitTask> tasks((prim_entries.size() + task_size - 1) /
                                task_size);
    for (size_t t = 0; t < tasks.size(); ++t) {
      tasks[t].begin = t * task_size;
      tasks[t].end = std::min(prim_entries.size(), tasks[t].begin + task_size);
    }
    TaskArena arena(static_cast<size_t>(build_threads));
    arena.Run(tasks.size(), [&](size_t t) {
      EmitTask& task = tasks[t];
      task.ctx.reset(new ThreadDecodeCtx(this, *reader_));
      ScopedThreadDecodeCtx scoped(task.ctx.get());
      std::vector<std::pair<std::string, Value>> scratch;
      for (size_t i = task.begin; i < task.end; ++i) emit_prim_body(i, scratch);
    });
    for (EmitTask& task : tasks) MergeDecodeCtx(*task.ctx);
    bodies_emitted = true;
  }
#endif
  if (!bodies_emitted) {
    std::vector<std::pair<std::string, Value>> prim_value_field_scratch;
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      if (entry_index > 0 &&
          !report_stage("stage.hierarchy", entry_index, prim_entries.size())) {
        return false;
      }
      emit_prim_body(entry_index, prim_value_field_scratch);
    }
  }
  if (irregular_entries) {
    // Several entries share a prim: keep the last authored order per path.
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      if (!children_order[entry_index].empty()) {
        prim_children_order[prim_entries[entry_index].full_path] =
            std::move(children_order[entry_index]);
      }
    }
  }
  if (!report_stage("stage.hierarchy", prim_entries.size(),
                    prim_entries.size())) {
    return false;
  }

  lap("bodies");
  // Finalize
  builder.finalize();
  lap("finalize");

  // Restore authored namespace order from primChildren: reorder each prim's
  // child links (and the root list) to the recorded name order; unlisted
  // children keep their relative (sorted) order at the end.
  // Restore authored namespace order from primChildren: reorder each prim's
  // child links (and the root list) to the recorded name order; unlisted
  // children keep their relative (sorted) order at the end. (The path index
  // used below was built by finalize().)
  auto reorder = [&](const std::vector<uint32_t>& current,
                     const std::vector<std::string>& names) {
    std::vector<uint32_t> out;
    out.reserve(current.size());
    std::vector<bool> used(current.size(), false);
    // Map each child name to a FIFO of its positions in `current` (preserving
    // original order for duplicate names). Reordering is then O(N+M) instead
    // of O(N*M): a malformed file with N root prims and an N-name primChildren
    // list previously cost O(N^2) string compares (a ~O(N) file could hang).
    std::unordered_map<std::string, std::deque<size_t>> by_name;
    for (size_t i = 0; i < current.size(); ++i) {
      const PrimSpec* c = layer.prim(current[i]);
      if (c) by_name[c->name()].push_back(i);
    }
    for (const std::string& nm : names) {
      auto it = by_name.find(nm);
      if (it == by_name.end() || it->second.empty()) continue;
      size_t idx = it->second.front();
      it->second.pop_front();
      used[idx] = true;
      out.push_back(current[idx]);
    }
    for (size_t i = 0; i < current.size(); ++i) {
      if (!used[i]) out.push_back(current[i]);
    }
    return out;
  };
  // The common case -- children already in the authored order (the sorted
  // hierarchy build matches it) -- needs no reorder at all.
  auto already_ordered = [&](const std::vector<uint32_t>& current,
                             const std::vector<std::string>& names) {
    if (current.size() != names.size()) return false;
    for (size_t i = 0; i < current.size(); ++i) {
      const PrimSpec* c = layer.prim(current[i]);
      if (!c || c->name() != names[i]) return false;
    }
    return true;
  };
  auto apply_children_order = [&](PrimSpec* p,
                                  const std::vector<std::string>& names) {
    if (!p || already_ordered(p->child_indices(), names)) return;
    std::vector<uint32_t> co = reorder(p->child_indices(), names);
    p->set_child_indices(std::move(co));
  };
  for (auto& kv : prim_children_order) {
    if (kv.first == "/") {
      if (!already_ordered(layer.root_indices(), kv.second)) {
        std::vector<uint32_t> ro = reorder(layer.root_indices(), kv.second);
        layer.set_root_indices(std::move(ro));
      }
      continue;
    }
    apply_children_order(layer.prim_at_path_mutable(kv.first), kv.second);
  }
  if (!irregular_entries) {
    // Every entry owns a distinct prim, so the per-prim reorders are
    // independent (each writes only its own child list and reads immutable
    // names): run them as tasks when there are many.
    auto reorder_range = [&](size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        if (children_order[i].empty() || entry_prim[i] == kSkipEntry) continue;
        apply_children_order(layer.prim(entry_prim[i]), children_order[i]);
      }
    };
    bool reordered = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && prim_entries.size() >= kParallelBuildMinPrims) {
      const size_t task_size = std::max<size_t>(
          1024, prim_entries.size() / (static_cast<size_t>(build_threads) * 4));
      const size_t ntasks = (prim_entries.size() + task_size - 1) / task_size;
      TaskArena arena(static_cast<size_t>(build_threads));
      arena.Run(ntasks, [&](size_t t) {
        reorder_range(t * task_size,
                      std::min(prim_entries.size(), (t + 1) * task_size));
      });
      reordered = true;
    }
#endif
    if (!reordered) reorder_range(0, prim_entries.size());
  }

  lap("reorder");
  // Create stage from layer
  result_.stage.SetRootLayer(std::move(layer));
  lap("set_root");
  return true;
}


}  // namespace next
}  // namespace lightusd
