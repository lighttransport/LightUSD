// SPDX-License-Identifier: Apache-2.0
#include "extension-field.hh"
#include "lexer.hh"
#include "value-parser.hh"

namespace lightusd {
DecodedExtensionField ParseExtensionField(std::string name, std::string source) {
  DecodedExtensionField result;
  result.name = std::move(name);
  result.unregistered = true;
  result.unregistered_source = std::move(source);
  result.value = Value(result.unregistered_source);
  Lexer lexer(result.unregistered_source.data(), result.unregistered_source.size());
  TypeId inferred = TypeId::Invalid;
  ParseResult parsed = lexer.peek().type == TokenType::OpenBrace
      ? ParseDict(lexer) : ParseGenericValue(lexer, inferred);
  if (parsed.success && !lexer.has_error() && !lexer.has_fatal_error() &&
      lexer.peek().type == TokenType::Eof) result.value = std::move(parsed.value);
  return result;
}
}
