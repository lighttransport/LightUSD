// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII parser utility helpers.

#include "ascii-parser-internal.hh"
#include "value-parser.hh"
#include "../strfmt.hh"

namespace lightusd {
namespace next {

bool AsciiParser::Impl::ParseNamespacedName(std::string* out, const char* what) {
  if (!out) return false;

  const Token& first = lexer_->peek();
  if (!IsNameToken(first)) {
    AddError(std::string("Expected ") + what);
    return false;
  }

  *out = first.value;
  lexer_->next();

  while (Check(TokenType::Colon)) {
    lexer_->next();
    const Token& suffix = lexer_->peek();
    if (!IsNameToken(suffix)) {
      AddError(std::string("Expected namespaced suffix for ") + what);
      return false;
    }
    *out += ":";
    *out += suffix.value;
    lexer_->next();
  }

  return true;
}

bool AsciiParser::Impl::ParseOrderList(std::vector<std::string>* out) {
  if (!out || !Match(TokenType::Equals) || !Match(TokenType::OpenBracket)) {
    AddError("Expected '= [...]' after reorder field");
    return false;
  }
  std::vector<std::string> parsed;
  while (!Check(TokenType::CloseBracket) && !AtEnd()) {
    const Token& tok = lexer_->peek();
    if (tok.type != TokenType::String && !IsNameToken(tok)) {
      AddError("Expected a name in reorder list");
      return false;
    }
    parsed.push_back(tok.value);
    lexer_->next();
    if (!Check(TokenType::CloseBracket)) Match(TokenType::Comma);
  }
  if (!Match(TokenType::CloseBracket)) {
    AddError("Unterminated reorder list");
    return false;
  }
  *out = std::move(parsed);
  return true;
}

bool AsciiParser::Impl::SkipBalancedBlock(TokenType open, TokenType close,
                                         size_t depth_level) {
  if (!Check(open)) return false;

  // Same-type nesting is handled iteratively via `depth` below; only a change of
  // bracket type recurses. Cap that recursion to bound stack usage on hostile
  // input (e.g. an alternating "([{([{..." run). The limit is far deeper than any
  // legitimate metadata alternation (cf. the dict/variantSet depth caps of 64).
  constexpr size_t kMaxSkipDepth = 1024;
  if (depth_level > kMaxSkipDepth) {
    AddError("Skipped block nesting too deep");
    return false;
  }

  size_t depth = 0;
  while (!AtEnd()) {
    const Token& tok = lexer_->peek();
    if (tok.type == open) {
      ++depth;
      lexer_->next();
      continue;
    }
    if (tok.type == close) {
      lexer_->next();
      if (--depth == 0) {
        return true;
      }
      continue;
    }

    if (tok.type == TokenType::OpenParen) {
      if (!SkipBalancedBlock(TokenType::OpenParen, TokenType::CloseParen,
                             depth_level + 1)) return false;
      continue;
    }
    if (tok.type == TokenType::OpenBracket) {
      if (!SkipBalancedBlock(TokenType::OpenBracket, TokenType::CloseBracket,
                             depth_level + 1)) return false;
      continue;
    }
    if (tok.type == TokenType::OpenBrace) {
      if (!SkipBalancedBlock(TokenType::OpenBrace, TokenType::CloseBrace,
                             depth_level + 1)) return false;
      continue;
    }

    lexer_->next();
  }

  AddError("Unexpected end of input while skipping block");
  return false;
}

bool AsciiParser::Impl::SkipValueLike() {
  if (Check(TokenType::OpenParen)) {
    return SkipBalancedBlock(TokenType::OpenParen, TokenType::CloseParen);
  }
  if (Check(TokenType::OpenBracket)) {
    return SkipBalancedBlock(TokenType::OpenBracket, TokenType::CloseBracket);
  }
  if (Check(TokenType::OpenBrace)) {
    return SkipBalancedBlock(TokenType::OpenBrace, TokenType::CloseBrace);
  }

  if (AtEnd()) return false;
  lexer_->next();
  return true;
}

void AsciiParser::Impl::SkipPropertyMetadata() {
  while (Check(TokenType::OpenParen)) {
    if (!SkipBalancedBlock(TokenType::OpenParen, TokenType::CloseParen)) {
      return;
    }
  }
}

void AsciiParser::Impl::ParsePropertyMetadata(const std::string& prop_name) {
  if (!Check(TokenType::OpenParen)) return;
  lexer_->next();  // consume '('
  PrimSpec* prim = builder_->current();

  while (!Check(TokenType::CloseParen) && !AtEnd()) {
    // Bare string = property COMMENT (`float x = 1 ( "note" )`) — pxr's only
    // accepted spelling, same mapping as the prim/layer metadata blocks.
    if (Check(TokenType::String)) {
      std::string note;
      lexer_->expect(TokenType::String, note);
      if (prim) {
        PropMeta& dm = prim->ensure_property_meta(prop_name);
        dm.comment = std::move(note);
        dm.authored |= PropMeta::kComment;
      }
      Match(TokenType::Comma);
      continue;
    }
    std::string qualifier;
    if (Match(TokenType::Prepend)) qualifier = "prepend ";
    else if (Match(TokenType::Append)) qualifier = "append ";
    else if (Match(TokenType::Add)) qualifier = "add ";
    else if (Match(TokenType::Delete)) qualifier = "delete ";
    else if (Match(TokenType::Reorder)) qualifier = "reorder ";
    std::string key;
    if (!lexer_->expect(TokenType::Identifier, key)) {
      // expect() consumes the mismatched token; do NOT skip another one (a
      // second next() here used to eat the ')' and derail the prim body).
      continue;
    }
    if (!Match(TokenType::Equals)) break;
    if (key == "permission" && qualifier.empty()) {
      // Unquoted token (`permission = private`).
      std::string v;
      if (Check(TokenType::Identifier)) lexer_->expect(TokenType::Identifier, v);
      else if (Check(TokenType::String)) lexer_->expect(TokenType::String, v);
      if (prim && !v.empty()) {
        PropMeta& dm = prim->ensure_property_meta(prop_name);
        dm.permission = v;
        dm.authored |= PropMeta::kPermission;
      }
      continue;
    }
    if (!prim) {  // no current prim: consume the value and continue
      SkipValueLike();
      continue;
    }
    // ensure_property_meta is keyed by interned name, so it works even at the
    // pre-`=` site where the property slot does not exist yet.
    PropMeta& m = prim->ensure_property_meta(prop_name);

    auto read_str = [&](std::string& out, uint32_t bit) {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        out = v;
        m.authored |= bit;
      }
    };

    if (key == "interpolation") read_str(m.interpolation, PropMeta::kInterpolation);
    else if (key == "colorSpace") read_str(m.colorSpace, PropMeta::kColorSpace);
    else if (key == "renderType") read_str(m.renderType, PropMeta::kRenderType);
    else if (key == "connectability") read_str(m.connectability, PropMeta::kConnectability);
    else if (key == "outputName") read_str(m.outputName, PropMeta::kOutputName);
    else if (key == "bindMaterialAs") read_str(m.bindMaterialAs, PropMeta::kBindMaterialAs);
    else if (key == "kind") read_str(m.kind, PropMeta::kKind);
    else if (key == "displayName") read_str(m.displayName, PropMeta::kDisplayName);
    else if (key == "displayGroup") read_str(m.displayGroup, PropMeta::kDisplayGroup);
    else if (key == "doc" || key == "documentation") read_str(m.doc, PropMeta::kDoc);
    else if (key == "comment") read_str(m.comment, PropMeta::kComment);
    else if (key == "elementSize") {
      ParseResult r = ParseValue(*lexer_, TypeId::Int);
      if (r.success && r.value.as_int()) {
        m.elementSize = *r.value.as_int();
        m.authored |= PropMeta::kElementSize;
      }
    } else if (key == "unauthoredValuesIndex") {
      ParseResult r = ParseValue(*lexer_, TypeId::Int);
      if (r.success && r.value.as_int()) {
        m.unauthoredValuesIndex = *r.value.as_int();
        m.authored |= PropMeta::kUnauthoredIdx;
      }
    } else if (key == "weight") {
      ParseResult r = ParseValue(*lexer_, TypeId::Double);
      if (r.success && r.value.as_double()) {
        m.weight = *r.value.as_double();
        m.authored |= PropMeta::kWeight;
      }
    } else if (key == "hidden") {
      ParseResult r = ParseValue(*lexer_, TypeId::Bool);
      if (r.success && r.value.as_bool()) {
        m.hidden = *r.value.as_bool();
        m.authored |= PropMeta::kHidden;
      }
    } else if (key == "allowedTokens") {
      ParseResult r = ParseArrayValue(*lexer_, TypeId::Token);
      if (r.success && r.value.as_token_array()) {
        m.allowedTokens = *r.value.as_token_array();
        m.authored |= PropMeta::kAllowedTokens;
      }
    } else if (key == "customData") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) { m.customData = std::move(r.value); m.authored |= PropMeta::kCustomData; }
    } else if (key == "assetInfo") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) { m.assetInfo = std::move(r.value); m.authored |= PropMeta::kAssetInfo; }
    } else if (key == "sdrMetadata") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) { m.sdrMetadata = std::move(r.value); m.authored |= PropMeta::kSdrMetadata; }
    } else {
      // Unknown property metadata: consume the value structurally but
      // preserve its raw source text for verbatim re-emit (same treatment
      // as unknown PRIM metadata).
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
          m.unknownMeta.emplace_back(qualifier + key,
                                     std::string(base + vstart, vend - vstart));
          m.authored |= PropMeta::kUnknownMeta;
        }
      }
      AddWarning("Unknown property metadata (preserved): " + key);
    }
  }
  Match(TokenType::CloseParen);
}

void AsciiParser::Impl::AddError(const std::string& message) {
  ParseError err;
  err.line = lexer_ ? lexer_->line() : 0;
  err.column = lexer_ ? lexer_->column() : 0;
  err.message = message;
  errors_.push_back(err);
}

void AsciiParser::Impl::AddWarning(const std::string& message) {
  warnings_.push_back("Line " + UIntToStr(lexer_ ? lexer_->line() : 0) + ": " + message);
}

bool AsciiParser::Impl::Match(TokenType type) {
  if (Check(type)) {
    lexer_->consume();
    return true;
  }
  return false;
}

bool AsciiParser::Impl::Check(TokenType type) {
  return lexer_ && lexer_->peek().type == type;
}

bool AsciiParser::Impl::AtEnd() {
  return !lexer_ || lexer_->peek().type == TokenType::Eof;
}

}  // namespace next
}  // namespace lightusd
