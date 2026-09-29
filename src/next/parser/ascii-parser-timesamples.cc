// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII parser timeSamples support.

#include "ascii-parser-internal.hh"
#include "value-parser.hh"
#include "value-parser-numeric.hh"

namespace lightusd {
namespace next {

bool AsciiParser::Impl::ParseTimeSamples(const std::string& prop_name,
                                         TypeId type_id, bool is_array) {
  if (!Match(TokenType::OpenBrace)) {
    AddError("Expected '{' for timeSamples");
    return false;
  }

  while (!Check(TokenType::CloseBrace) && !AtEnd()) {
    // Numeric keys use the same conversion as ParseDouble, without building
    // a temporary Value for every sample. Special spellings take that path.
    double time = 0.0;
    const Token& time_tok = lexer_->peek();
    if (time_tok.type == TokenType::Number) {
      time = value_parser_detail::FastFloatParseToken<double>(time_tok.value);
      lexer_->consume();
    } else {
      ParseResult time_result = ParseValue(*lexer_, TypeId::Double);
      if (!time_result.success || !time_result.value.as_double()) {
        AddError("Expected time value in timeSamples");
        return false;
      }
      time = *time_result.value.as_double();
    }

    if (!Match(TokenType::Colon)) {
      AddError("Expected ':' after time in timeSamples");
      return false;
    }

    if (type_id == TypeId::Invalid) {
      if (!SkipValueLike()) {
        AddError("Failed to skip timeSample value for unknown attribute type");
        return false;
      }
      Match(TokenType::Comma);
      continue;
    }

    ParseResult value_result;
    bool deferred = false;
    if (is_array) {
      value_result = ParseArrayAttributeValue(type_id, &deferred);
    } else if (Check(TokenType::Number) && !IsScalarType(type_id)) {
      // AOUSD permits format implementations to retain a default/time sample
      // whose stored value disagrees with the declared type. Parse the scalar
      // as its own VariantValue instead of rejecting the whole layer (the
      // supplemental attributes fixture deliberately exercises this).
      value_result = ParseValue(*lexer_, TypeId::Double);
    } else {
      value_result = ParseValue(*lexer_, type_id);
    }
    if (!value_result.success) {
      AddError("Failed to parse timeSample value: " + value_result.error);
      return false;
    }

    // Deferred-fill values skip content-hash dedup: their payload is not
    // parsed yet (see PrimSpec::add_time_sample).
    builder_->add_time_sample(prop_name, time, std::move(value_result.value),
                              /*dedup=*/!deferred);

    Match(TokenType::Comma);
  }

  return Match(TokenType::CloseBrace);
}

}  // namespace next
}  // namespace lightusd
