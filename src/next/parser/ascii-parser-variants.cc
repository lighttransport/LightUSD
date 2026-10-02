// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII parser variant-set support.

#include "ascii-parser-internal.hh"
#include <algorithm>
#include <unordered_set>
#include "value-parser.hh"

namespace lightusd {
namespace next {

bool AsciiParser::Impl::ParseVariantSetBody(const std::string& variant_set_name) {
  PrimSpec* prim = builder_->current();
  if (!prim) return false;
  return ParseVariantSetBodyInto(variant_set_name, prim->meta().variantSets(), 0);
}

bool AsciiParser::Impl::ParseVariantSetBodyInto(
    const std::string& variant_set_name, std::vector<VariantSetData>& target,
    int depth) {
  if (depth > 64) {
    AddError("variantSet nesting too deep");
    return false;
  }

  if (!Match(TokenType::OpenBrace)) {
    AddError("Expected '{' for variantSet body");
    return false;
  }

  VariantSetData* vs_data = nullptr;
  for (auto& vs : target) {
    if (vs.name == variant_set_name) {
      vs_data = &vs;
      break;
    }
  }
  if (!vs_data) {
    target.emplace_back();
    vs_data = &target.back();
    vs_data->name = variant_set_name;
  }

  while (!Check(TokenType::CloseBrace) && !AtEnd()) {
    VariantData variant;
    if (!ParseVariantOption(&variant, depth)) {
      return false;
    }
    vs_data->variants.push_back(std::move(variant));
  }

  return Match(TokenType::CloseBrace);
}

bool AsciiParser::Impl::ParseVariantOption(VariantData* out, int depth) {
  if (!out) return false;

  if (!lexer_->expect(TokenType::String, out->name)) {
    AddError("Expected variant option name");
    return false;
  }

  // Nested variant selections authored in the option's metadata
  // (`"o1" ( variants = { string inner = "i1" } ) {...}`). The nested sets
  // themselves are parsed from the BODY below, so stash selections and apply
  // them to out->variantSets afterwards.
  std::vector<std::pair<std::string, std::string>> pending_selections;

  if (Check(TokenType::OpenParen)) {
    lexer_->next();
    while (!Check(TokenType::CloseParen) && !AtEnd()) {
      // Arc list-op qualifier. It changes how the parsed items are applied
      // below: `delete` must REMOVE matching arcs (treating it as an append
      // composed the deleted asset), `prepend` inserts at the front,
      // `reorder` is a no-op on the flat list.
      enum class ArcOp { Bare, Prepend, Append, Delete, Reorder } arc_op =
          ArcOp::Bare;
      bool add_spelling = false;
      if (Match(TokenType::Prepend)) {
        arc_op = ArcOp::Prepend;
      } else if (Match(TokenType::Append)) {
        arc_op = ArcOp::Append;
      } else if (Match(TokenType::Add)) {
        arc_op = ArcOp::Append;
        add_spelling = true;
      } else if (Match(TokenType::Delete)) {
        arc_op = ArcOp::Delete;
      } else if (Match(TokenType::Reorder)) {
        arc_op = ArcOp::Reorder;
      }

      std::string key;
      if (!lexer_->expect(TokenType::Identifier, key)) {
        if (!AtEnd()) lexer_->next();
        continue;
      }
      if (!Match(TokenType::Equals)) break;

      if (key == "active") {
        ParseResult result = ParseValue(*lexer_, TypeId::Bool);
        if (result.success && result.value.as_bool()) {
          out->active = *result.value.as_bool();
          out->active_authored = true;
        }
      } else if (key == "hidden") {
        ParseResult result = ParseValue(*lexer_, TypeId::Bool);
        if (result.success && result.value.as_bool()) {
          out->hidden = *result.value.as_bool();
          out->hidden_authored = true;
        }
      } else if (key == "doc" || key == "documentation") {
        std::string v;
        if (lexer_->expect(TokenType::String, v)) out->doc = v;
      } else if (key == "kind") {
        std::string v;
        if (lexer_->expect(TokenType::String, v)) out->kind = v;
      } else if (key == "variants" || key == "variantSelection") {
        // Same grammar as prim-level `variants = { string set = "sel" }`.
        if (Match(TokenType::OpenBrace)) {
          while (!Check(TokenType::CloseBrace) && !AtEnd()) {
            std::string set_name;
            if (Check(TokenType::String)) {
              lexer_->expect(TokenType::String, set_name);
            } else if (Check(TokenType::Identifier)) {
              std::string first;
              lexer_->expect(TokenType::Identifier, first);
              if (!Check(TokenType::Equals) &&
                  (Check(TokenType::Identifier) || Check(TokenType::String))) {
                if (Check(TokenType::String)) {
                  lexer_->expect(TokenType::String, set_name);
                } else {
                  lexer_->expect(TokenType::Identifier, set_name);
                }
              } else {
                set_name = first;
              }
            } else {
              break;
            }
            if (!Match(TokenType::Equals)) break;
            std::string sel_name;
            if (!lexer_->expect(TokenType::String, sel_name)) break;
            pending_selections.emplace_back(set_name, sel_name);
            Match(TokenType::Comma);
          }
          Match(TokenType::CloseBrace);
        }
      } else if (key == "references" || key == "payload" ||
                 key == "inherits" || key == "specializes") {
        std::vector<std::string>* target =
            key == "references"   ? &out->references
            : key == "payload"    ? &out->payloads
            : key == "inherits"   ? &out->inherits
                                  : &out->specializes;
        auto apply_arc = [&](std::string ref) {
          switch (arc_op) {
            case ArcOp::Delete:
              target->erase(
                  std::remove(target->begin(), target->end(), ref),
                  target->end());
              break;
            case ArcOp::Reorder:
              break;  // ordering-only edit; flat list keeps parse order
            case ArcOp::Prepend:
              target->insert(target->begin(), std::move(ref));
              break;
            default:
              target->push_back(std::move(ref));
              break;
          }
        };
        if (Check(TokenType::None)) {
          lexer_->next();
          if (arc_op == ArcOp::Bare || arc_op == ArcOp::Delete) {
            target->clear();  // explicit `= None` clears the list
          }
        } else if (Match(TokenType::OpenBracket)) {
          // For prepend, preserve authored order: collect then insert.
          std::vector<std::string> items;
          while (!Check(TokenType::CloseBracket) && !AtEnd()) {
            std::string ref;
            if (!ReadArcRef(&ref)) break;
            items.push_back(std::move(ref));
            Match(TokenType::Comma);
          }
          Match(TokenType::CloseBracket);
          if (arc_op == ArcOp::Prepend) {
            target->insert(target->begin(),
                           std::make_move_iterator(items.begin()),
                           std::make_move_iterator(items.end()));
          } else if (arc_op == ArcOp::Delete) {
            // Bulk delete via a hash set (per-item apply_arc would be
            // O(items * target) = O(N^2) for a `delete <arc> = [...]` list).
            const std::unordered_set<std::string> del(items.begin(),
                                                     items.end());
            target->erase(std::remove_if(target->begin(), target->end(),
                                         [&](const std::string& x) {
                                           return del.count(x) != 0;
                                         }),
                          target->end());
          } else {
            for (auto& it : items) apply_arc(std::move(it));
          }
        } else {
          std::string ref;
          if (ReadArcRef(&ref)) apply_arc(std::move(ref));
        }
      } else {
        // Unknown (unmodeled) variant-option metadata: consume the value
        // structurally but PRESERVE its raw source text so the writer can
        // re-emit it verbatim (same policy as prim-level unknownMeta).
        lexer_->peek();  // ensure the value's first token is scanned
        const size_t vstart = lexer_->token_start();
        const bool skipped = SkipValueLike();
        while (Check(TokenType::PathRef)) lexer_->next();
        if (Check(TokenType::OpenParen)) SkipValueLike();
        if (skipped) {
          lexer_->peek();  // the following token's start bounds the value
          size_t vend = lexer_->token_start();
          const char* base = lexer_->input_data();
          while (vend > vstart &&
                 (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
                  base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
            vend--;
          }
          if (vend > vstart) {
            std::string qual_prefix;
            switch (arc_op) {
              case ArcOp::Prepend: qual_prefix = "prepend "; break;
              case ArcOp::Append: qual_prefix = add_spelling ? "add " : "append "; break;
              case ArcOp::Delete: qual_prefix = "delete "; break;
              case ArcOp::Reorder: qual_prefix = "reorder "; break;
              default: break;
            }
            out->unknownMeta.emplace_back(
                qual_prefix + key, std::string(base + vstart, vend - vstart));
          }
        }
      }
    }
    Match(TokenType::CloseParen);
  }

  if (!Match(TokenType::OpenBrace)) {
    AddError("Expected '{' for variant option body");
    return false;
  }

  std::unique_ptr<Layer> content_layer;
  std::unique_ptr<LayerBuilder> content_builder;

  while (!Check(TokenType::CloseBrace) && !AtEnd()) {
    const Token& tok = lexer_->peek();

    if (tok.type == TokenType::Def || tok.type == TokenType::Over ||
        tok.type == TokenType::Class) {
      if (!content_layer) {
        content_layer.reset(new Layer());
        content_builder.reset(new LayerBuilder(*content_layer));
        content_builder->begin_prim("__self__", "", PrimSpecifier::Over);
      }
      std::unique_ptr<Layer> host_layer = std::move(layer_);
      std::unique_ptr<LayerBuilder> host_builder = std::move(builder_);
      layer_ = std::move(content_layer);
      builder_ = std::move(content_builder);
      bool ok = ParsePrim();
      content_layer = std::move(layer_);
      content_builder = std::move(builder_);
      layer_ = std::move(host_layer);
      builder_ = std::move(host_builder);
      if (!ok) return false;
    } else if (tok.type == TokenType::Rel) {
      lexer_->next();
      std::string rel_name;
      if (!ParseNamespacedName(&rel_name, "relationship name")) break;
      SkipPropertyMetadata();
      if (Match(TokenType::Equals)) {
        if (Check(TokenType::None)) {
          lexer_->next();
          out->relationships[rel_name];  // explicit target-less block
        } else if (Check(TokenType::OpenBracket)) {
          lexer_->next();
          while (!Check(TokenType::CloseBracket) && !AtEnd()) {
            std::string target;
            if (lexer_->expect(TokenType::PathRef, target)) {
              out->relationships[rel_name].push_back(Path(target));
            }
            Match(TokenType::Comma);
          }
          Match(TokenType::CloseBracket);
        } else if (Check(TokenType::PathRef)) {
          std::string target;
          lexer_->expect(TokenType::PathRef, target);
          out->relationships[rel_name].push_back(Path(target));
        }
      } else {
        out->relationships[rel_name];  // declaration without targets
      }
      SkipPropertyMetadata();
    } else if (tok.type == TokenType::Identifier && tok.value == "variantSet") {
      lexer_->next();
      std::string nested_vs_name;
      if (!lexer_->expect(TokenType::String, nested_vs_name)) break;
      if (!Match(TokenType::Equals)) break;
      if (!ParseVariantSetBodyInto(nested_vs_name, out->variantSets, depth + 1)) {
        return false;
      }
    } else if (tok.type == TokenType::Custom || tok.type == TokenType::Uniform ||
               tok.type == TokenType::Varying ||
               tok.type == TokenType::Identifier) {
      uint16_t vflags = 0;
      while (true) {
        TokenType tt = lexer_->peek().type;
        if (tt == TokenType::Custom) {
          vflags |= PropSlot::kFlagCustom;
          lexer_->next();
        } else if (tt == TokenType::Uniform) {
          vflags |= PropSlot::kFlagUniform |
                    PropSlot::kFlagVariabilityAuthored;
          lexer_->next();
        } else if (tt == TokenType::Varying) {
          vflags |= PropSlot::kFlagVarying |
                    PropSlot::kFlagVariabilityAuthored;
          lexer_->next();
        } else {
          break;
        }
      }
      // Qualified relationship: `custom rel x = </t>` / `uniform rel ...`.
      if (Check(TokenType::Rel)) {
        lexer_->next();
        std::string rel_name;
        if (!ParseNamespacedName(&rel_name, "relationship name")) {
          AddError("Failed to parse relationship name in variant option");
          return false;
        }
        SkipPropertyMetadata();
        if (Match(TokenType::Equals)) {
          if (Check(TokenType::None)) {
            lexer_->next();
            out->relationships[rel_name];  // explicit target-less block
          } else if (Check(TokenType::OpenBracket)) {
            lexer_->next();
            while (!Check(TokenType::CloseBracket) && !AtEnd()) {
              std::string target;
              if (lexer_->expect(TokenType::PathRef, target)) {
                out->relationships[rel_name].push_back(Path(target));
              }
              Match(TokenType::Comma);
            }
            Match(TokenType::CloseBracket);
          } else if (Check(TokenType::PathRef)) {
            std::string target;
            lexer_->expect(TokenType::PathRef, target);
            out->relationships[rel_name].push_back(Path(target));
          }
        } else {
          out->relationships[rel_name];  // declaration without targets
        }
        SkipPropertyMetadata();
        out->relationshipFlags[rel_name] = vflags;
        continue;
      }
      std::string type_name;
      if (!lexer_->expect(TokenType::Identifier, type_name)) {
        AddError("Expected attribute type name in variant option body");
        return false;
      }
      bool is_array = false;
      if (Check(TokenType::OpenBracket)) {
        lexer_->next();
        Match(TokenType::CloseBracket);
        is_array = true;
      }
      if (is_array) vflags |= PropSlot::kFlagArray;
      std::string prop_name;
      if (!ParseNamespacedName(&prop_name, "attribute name")) break;
      SkipPropertyMetadata();
      if (Check(TokenType::Dot)) {
        lexer_->next();
        const Token& suffix_tok = lexer_->peek();
        if (suffix_tok.type == TokenType::TimeSamples) {
          // `<type> <name>.timeSamples = {...}` inside a variant option.
          // VariantProperty cannot carry samples; route them into the
          // content sub-layer's "__self__" prim (the compositor grafts its
          // opinions onto the host prim on selection).
          lexer_->next();
          if (Match(TokenType::Equals)) {
            bool suffix_array = false;
            TypeId tid = ParseTypeName(type_name, suffix_array);
            const bool arr = is_array || suffix_array;
            if (!content_layer) {
              content_layer.reset(new Layer());
              content_builder.reset(new LayerBuilder(*content_layer));
              content_builder->begin_prim("__self__", "", PrimSpecifier::Over);
            }
            std::unique_ptr<Layer> host_layer = std::move(layer_);
            std::unique_ptr<LayerBuilder> host_builder = std::move(builder_);
            layer_ = std::move(content_layer);
            builder_ = std::move(content_builder);
            bool ok = ParseTimeSamples(prop_name, tid, arr);
            content_layer = std::move(layer_);
            content_builder = std::move(builder_);
            layer_ = std::move(host_layer);
            builder_ = std::move(host_builder);
            if (!ok) return false;
          }
          SkipPropertyMetadata();
        } else if (suffix_tok.type == TokenType::Identifier &&
                   suffix_tok.value == "connect") {
          // `<type> <name>.connect = </target>` inside a variant option:
          // record the connection on the content "__self__" prim (grafted
          // onto the host on selection).
          lexer_->next();
          if (Match(TokenType::Equals)) {
            if (!content_layer) {
              content_layer.reset(new Layer());
              content_builder.reset(new LayerBuilder(*content_layer));
              content_builder->begin_prim("__self__", "", PrimSpecifier::Over);
            }
            PrimSpec* self = content_builder->current();
            auto add_conn = [&](const std::string& t) {
              if (self) {
                self->add_connection(prop_name, Path(t));
                PropNameId pid = GetPropNameTable().intern(prop_name);
                if (!self->property(pid)) {
                  bool sa = false;
                  TypeId tid = ParseTypeName(type_name, sa);
                  uint16_t cflags = PropSlot::kFlagConnection | vflags;
                  if (is_array || sa) cflags |= PropSlot::kFlagArray;
                  self->add_property_slot(pid, tid, cflags);
                  self->set_property_type_name(
                      prop_name, (is_array || sa) ? type_name + "[]"
                                                  : type_name);
                }
              }
            };
            if (Check(TokenType::None)) {
              lexer_->next();
            } else if (Match(TokenType::OpenBracket)) {
              while (!Check(TokenType::CloseBracket) && !AtEnd()) {
                std::string t;
                if (lexer_->expect(TokenType::PathRef, t)) add_conn(t);
                Match(TokenType::Comma);
              }
              Match(TokenType::CloseBracket);
            } else {
              std::string t;
              if (lexer_->expect(TokenType::PathRef, t)) add_conn(t);
            }
          }
          SkipPropertyMetadata();
        } else {
          // Other suffixed statements: skip.
          if (!AtEnd()) lexer_->next();
          if (Match(TokenType::Equals)) {
            SkipValueLike();
            while (Check(TokenType::PathRef)) lexer_->next();
          }
          SkipPropertyMetadata();
        }
      } else if (Match(TokenType::Equals)) {
        // ParseTypeName's second parameter is an OUT param (detects a "[]"
        // suffix embedded in the type NAME) — do not pass `is_array` itself
        // or the `[]` tokens scanned above get clobbered to false and every
        // array-valued variant property is silently dropped.
        bool suffix_array = false;
        TypeId tid = ParseTypeName(type_name, suffix_array);
        if (suffix_array && !is_array) {
          is_array = true;
          vflags |= PropSlot::kFlagArray;
        }
        if (tid == TypeId::Invalid) {
          if (!SkipValueLike()) break;
          out->properties.push_back({prop_name, Value(), vflags});
        } else {
          ParseResult result;
          ParseArrayContext array_ctx;
          array_ctx.source = source_;
          array_ctx.enable_usda_lazy_arrays = options_.enable_usda_lazy_arrays;
          array_ctx.max_usda_lazy_array_elements =
              options_.max_usda_lazy_array_elements;
          array_ctx.num_threads = options_.num_threads;
          if (is_array) {
            result = ParseArrayValue(*lexer_, tid, array_ctx);
          } else {
            result = ParseValue(*lexer_, tid);
          }
          if (result.success) {
            if (Check(TokenType::OpenParen)) {
              // Trailing property metadata (`( interpolation = "constant" )`):
              // VariantProperty cannot carry PropMeta, so author value + meta
              // on the content "__self__" prim instead — both graft onto the
              // host prim on selection (CopyLocalOpinions merges PropMeta),
              // and the writer emits __self__ properties back into the body.
              if (!content_layer) {
                content_layer.reset(new Layer());
                content_builder.reset(new LayerBuilder(*content_layer));
                content_builder->begin_prim("__self__", "",
                                            PrimSpecifier::Over);
              }
              PrimSpec* self = content_builder->current();
              if (self) {
                self->add_property(prop_name, std::move(result.value), vflags);
                const bool has_sfx =
                    type_name.size() >= 2 &&
                    type_name.compare(type_name.size() - 2, 2, "[]") == 0;
                self->set_property_type_name(
                    prop_name,
                    (is_array && !has_sfx) ? type_name + "[]" : type_name);
                std::unique_ptr<Layer> host_layer = std::move(layer_);
                std::unique_ptr<LayerBuilder> host_builder = std::move(builder_);
                layer_ = std::move(content_layer);
                builder_ = std::move(content_builder);
                ParsePropertyMetadata(prop_name);
                content_layer = std::move(layer_);
                content_builder = std::move(builder_);
                layer_ = std::move(host_layer);
                builder_ = std::move(host_builder);
              } else {
                out->properties.push_back(
                    {prop_name, std::move(result.value), vflags});
              }
            } else {
              out->properties.push_back(
                  {prop_name, std::move(result.value), vflags});
            }
          }
        }
        SkipPropertyMetadata();
      }
    } else {
      lexer_->next();
    }
  }

  // Apply nested selections stashed from the option metadata to the nested
  // sets parsed from the body.
  out->variantSelections = pending_selections;
  for (const auto& sel : pending_selections) {
    for (VariantSetData& nvs : out->variantSets) {
      if (nvs.name == sel.first) {
        nvs.selected = sel.second;
        break;
      }
    }
  }

  if (content_layer) {
    content_builder->end_prim();
    content_builder->finalize();
    out->content = std::shared_ptr<Layer>(content_layer.release());
  }

  return Match(TokenType::CloseBrace);
}

}  // namespace next
}  // namespace lightusd
