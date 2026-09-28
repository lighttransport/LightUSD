// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII parser stage metadata.

#include "ascii-parser-internal.hh"

#include <algorithm>
#include "value-parser.hh"

namespace lightusd {
namespace next {

bool AsciiParser::Impl::ParseStageMetadata() {
  // Check for #usda header
  const Token& first = lexer_->peek();
  if (first.type == TokenType::Identifier && first.value == "#usda") {
    // Skip #usda line
    lexer_->skip_line();
  }

  // Check for opening paren (metadata block)
  if (Check(TokenType::OpenParen)) {
    lexer_->next();

    // Parse metadata key-value pairs
    while (!Check(TokenType::CloseParen) && !AtEnd()) {
      // pxr accepts `;` as an optional statement separator.
      if (Match(TokenType::Semicolon)) continue;
      // A bare (often triple-quoted) string is the layer COMMENT — pxr's
      // only accepted spelling (26.x rejects `comment = "..."`; the bare
      // string maps to `comment`, not `doc`).
      if (Check(TokenType::String)) {
        std::string d;
        lexer_->expect(TokenType::String, d);
        layer_->meta().comment = d;
        layer_->meta().comment_set = true;
        continue;
      }
      std::string key;
      if (!lexer_->expect(TokenType::Identifier, key)) {
        AddError("Expected metadata key");
        return false;
      }

      if (!Match(TokenType::Equals)) {
        AddError("Expected '=' after metadata key");
        return false;
      }

      // Parse value based on known metadata keys
      if (key == "defaultPrim") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().defaultPrim = value;
          layer_->meta().defaultPrim_set = true;
        }
      } else if (key == "upAxis") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().upAxis = value;
          layer_->meta().upAxis_set = true;
        }
      } else if (key == "metersPerUnit") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().metersPerUnit = *result.value.as_double();
          layer_->meta().metersPerUnit_set = true;
        }
      } else if (key == "timeCodesPerSecond") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().timeCodesPerSecond = *result.value.as_double();
          layer_->meta().timeCodesPerSecond_set = true;
        }
      } else if (key == "startTimeCode") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().startTimeCode = *result.value.as_double();
          layer_->meta().startTimeCode_set = true;
        }
      } else if (key == "endTimeCode") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().endTimeCode = *result.value.as_double();
          layer_->meta().endTimeCode_set = true;
        }
      } else if (key == "framesPerSecond") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().framesPerSecond = *result.value.as_double();
          layer_->meta().framesPerSecond_set = true;
        }
      } else if (key == "kilogramsPerUnit") {
        ParseResult result = ParseValue(*lexer_, TypeId::Double);
        if (result.success && result.value.as_double()) {
          layer_->meta().kilogramsPerUnit = *result.value.as_double();
          layer_->meta().kilogramsPerUnit_set = true;
        }
      } else if (key == "relocates") {
        // Layer relocates: { </old/path>: </new/path>, ... }
        layer_->meta().relocates_set = true;
        if (Match(TokenType::OpenBrace)) {
          while (!Check(TokenType::CloseBrace) && !AtEnd()) {
            std::string src, dst;
            if (!lexer_->expect(TokenType::PathRef, src)) break;
            if (!Match(TokenType::Colon)) break;
            if (!lexer_->expect(TokenType::PathRef, dst)) break;
            layer_->meta().relocates.emplace_back(src, dst);
            Match(TokenType::Comma);
          }
          Match(TokenType::CloseBrace);
        }
      } else if (key == "colorConfiguration") {
        // asset (`@path@`, lexed as String) or quoted string
        std::string value;
        if (Check(TokenType::String)) {
          lexer_->expect(TokenType::String, value);
          layer_->meta().colorConfiguration = value;
          layer_->meta().colorConfiguration_set = true;
        }
      } else if (key == "colorManagementSystem") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().colorManagementSystem = value;
          layer_->meta().colorManagementSystem_set = true;
        }
      } else if (key == "renderSettingsPrimPath") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().renderSettingsPrimPath = value;
          layer_->meta().renderSettingsPrimPath_set = true;
        }
      } else if (key == "doc" || key == "documentation") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().doc = value;
          layer_->meta().doc_set = true;
        }
      } else if (key == "comment") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().comment = value;
          layer_->meta().comment_set = true;
        }
      } else if (key == "hasOwnedSubLayers") {
        ParseResult r = ParseValue(*lexer_, TypeId::Bool);
        if (r.success && r.value.as_bool()) {
          layer_->meta().hasOwnedSubLayers = *r.value.as_bool();
          layer_->meta().hasOwnedSubLayers_set = true;
        }
      } else if (key == "owner") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().owner = value;
          layer_->meta().owner_set = true;
        }
      } else if (key == "autoPlay") {
        ParseResult r = ParseValue(*lexer_, TypeId::Bool);
        if (r.success && r.value.as_bool()) {
          layer_->meta().autoPlay = *r.value.as_bool();
          layer_->meta().autoPlay_set = true;
        }
      } else if (key == "playbackMode") {
        std::string value;
        if (lexer_->expect(TokenType::String, value)) {
          layer_->meta().playbackMode = value == "loop" ? "loop" : "none";
          layer_->meta().playbackMode_set = true;
        }
      } else if (key == "customLayerData") {
        ParseResult r = ParseDict(*lexer_);
        if (r.success) {
          layer_->meta().customLayerData = std::move(r.value);
          layer_->meta().customLayerData_set = true;
        }
      } else if (key == "expressionVariables") {
        ParseResult r = ParseDict(*lexer_);
        if (r.success) {
          layer_->meta().expressionVariables = std::move(r.value);
          layer_->meta().expressionVariables_set = true;
        }
      } else if (key == "subLayers") {
        // Parse sublayer list. Each element is an asset ref (`@path@`, lexed as
        // a String) or a quoted string. Decide with a non-consuming Check first:
        // `expect()` always consumes, so `expect(A) || expect(B)` would eat two
        // tokens per element and corrupt the list.
        layer_->meta().subLayers_set = true;
        if (Match(TokenType::OpenBracket)) {
          while (!Check(TokenType::CloseBracket) && !AtEnd()) {
            std::string path;
            if (Check(TokenType::PathRef)) {
              lexer_->expect(TokenType::PathRef, path);
            } else if (Check(TokenType::String)) {
              lexer_->expect(TokenType::String, path);
            } else {
              break;  // unexpected token: stop rather than mis-consume
            }
            layer_->meta().subLayers.push_back(path);
            // Optional per-sublayer layer offset "(offset = ..; scale = ..)".
            double sl_offset = 0.0, sl_scale = 1.0;
            if (Check(TokenType::OpenParen)) {
              lexer_->next();
              while (!Check(TokenType::CloseParen) && !AtEnd()) {
                std::string okey;
                if (!lexer_->expect(TokenType::Identifier, okey)) break;
                if (!Match(TokenType::Equals)) break;
                ParseResult r = ParseValue(*lexer_, TypeId::Double);
                if (r.success && r.value.as_double()) {
                  if (okey == "offset") sl_offset = *r.value.as_double();
                  else if (okey == "scale") sl_scale = *r.value.as_double();
                }
                Match(TokenType::Semicolon);
              }
              Match(TokenType::CloseParen);
            }
            // `!(scale > 0)` also rejects NaN, which `scale <= 0` would let
            // through to poison every time-mapped sample of the sublayer.
            if (!(sl_scale > 0.0)) {
              if (options_.strict_aousd_conformance) {
                AddError("AOUSD layer-offset scale must be greater than zero");
                return false;
              }
              // pxr warns and substitutes NO offset (identity) — retaining a
              // negative/NaN scale would time-reverse or destroy the samples.
              AddWarning(
                  "Invalid sublayer offset (non-positive scale); using no "
                  "offset instead");
              sl_offset = 0.0;
              sl_scale = 1.0;
            }
            // Keep the offsets vector parallel to subLayers.
            auto& offs = layer_->meta().subLayerOffsets;
            offs.resize(layer_->meta().subLayers.size() - 1, {0.0, 1.0});
            offs.emplace_back(sl_offset, sl_scale);
            Match(TokenType::Comma);
          }
          Match(TokenType::CloseBracket);
        }
      } else {
        // AOUSD Core's layer grammar accepts any metadata key (the spec's own
        // file_formats baseline keeps `foo = bar` / `baz = None`, and elective
        // fields such as framePrecision). Strict mode therefore preserves
        // unknown stage metadata losslessly, like prim/property metadata.
        // Generic metadata may be a dictionary/list: consume it structurally
        // but PRESERVE the raw source text so the writer re-emits it verbatim
        // (uniform losslessness with prim/property metadata).
        lexer_->peek();  // ensure the value's first token is scanned
        const size_t vstart = lexer_->token_start();
        const bool skipped = SkipValueLike();
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
            layer_->meta().unknownMeta.emplace_back(
                key, std::string(base + vstart, vend - vstart));
          }
        }
        AddWarning("Unknown stage metadata (preserved): " + key);
      }
    }

    if (!Match(TokenType::CloseParen)) {
      AddError("Expected ')' to close metadata block");
      return false;
    }
  }

  return true;
}

}  // namespace next
}  // namespace lightusd
