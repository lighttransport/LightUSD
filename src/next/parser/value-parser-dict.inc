// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - dictionary value parser.
// Included inside namespace lightusd::next.

// Recursive dictionary body parser. Grammar (USD):
//   { [<typeName>] <key> = <value>   ... }
// where <key> is an identifier or quoted string, <value> is a scalar/array
// literal or a nested `{ ... }`. The type name is optional (inferred if absent).
static ParseResult ParseDictDepth(Lexer& lexer, int depth) {
  if (depth > 64) return ParseResult::Error("dictionary nesting too deep");
  if (!lexer.expect(TokenType::OpenBrace)) {
    return ParseResult::Error("Expected '{' to start dictionary");
  }

  Value v = Value::MakeDictionary();
  auto d = v.as_dictionary();

  // Dictionary keys/type names may collide with lexer keywords ("add",
  // "custom", "delete", "true", ...): accept any keyword-ish token as a name
  // in key/type position, mirroring IsNameToken in the prim parser.
  auto is_dict_name = [](const Token& tk) {
    switch (tk.type) {
      case TokenType::Identifier:
      case TokenType::Def:
      case TokenType::Over:
      case TokenType::Class:
      case TokenType::True:
      case TokenType::False:
      case TokenType::None:
      case TokenType::TimeSamples:
      case TokenType::Custom:
      case TokenType::Uniform:
      case TokenType::Varying:
      case TokenType::Prepend:
      case TokenType::Append:
      case TokenType::Delete:
      case TokenType::Add:
      case TokenType::Reorder:
      case TokenType::Rel:
        return true;
      default:
        return false;
    }
  };

  while (lexer.peek_view().type != TokenType::CloseBrace &&
         lexer.peek_view().type != TokenType::Eof) {
    std::string type_name, key;
    bool have_type = false;

    const Token& t = lexer.peek();
    if (t.type == TokenType::String) {
      key = t.value;
      lexer.next();
    } else if (is_dict_name(t)) {
      std::string first = t.value;
      lexer.next();
      const Token& t2 = lexer.peek();
      if (is_dict_name(t2) || t2.type == TokenType::String ||
          t2.type == TokenType::OpenBracket) {
        // `first` was the type name; the key (after an optional []) follows.
        type_name = first;
        have_type = true;
        if (lexer.peek_view().type == TokenType::OpenBracket) {
          lexer.next();
          lexer.expect(TokenType::CloseBracket);
          type_name += "[]";
        }
        const Token& kt = lexer.peek();
        if (kt.type == TokenType::String || is_dict_name(kt)) {
          key = kt.value;
          lexer.next();
        } else {
          return ParseResult::Error("Expected dictionary key after type name");
        }
      } else {
        key = first;  // bare key, no type name
      }
    } else {
      lexer.next();  // unexpected token: skip to avoid an infinite loop
      continue;
    }

    if (!lexer.expect(TokenType::Equals)) {
      return ParseResult::Error("Expected '=' in dictionary entry");
    }

    ParseResult val;
    if (lexer.peek_view().type == TokenType::OpenBrace) {
      val = ParseDictDepth(lexer, depth + 1);
    } else if (have_type) {
      bool is_array = false;
      TypeId tid = ParseTypeName(type_name, is_array);
      if (tid == TypeId::Invalid) {
        TypeId inferred;
        val = ParseGenericValue(lexer, inferred);
      } else if (is_array) {
        val = ParseArrayValue(lexer, tid);
      } else {
        val = ParseValue(lexer, tid);
      }
    } else {
      TypeId inferred;
      val = ParseGenericValue(lexer, inferred);
    }
    if (!val.success) return val;
    // AOUSD 6.6.2 (L1224-1225): each key maps to one and only one value. The
    // dictionary keeps the last value (pxr parity), so the authored
    // duplicate survives only as a diagnostic (core.dict.duplicateKey).
    if (d.find(key)) {
      lexer.diagnostics.push_back(
          "Line " + std::to_string(lexer.line()) +
          ": [core.dict.duplicateKey] duplicate dictionary key `" + key +
          "`");
    }
    d->set(std::move(key), std::move(val.value));

    // Entries are separated by a newline or `;` (pxr dictionary grammar);
    // a `,` is tolerated for legacy LightUSD-written files. Two entries on
    // one line with no separator are rejected in strict mode, as OpenUSD does.
    const TokenType nt = lexer.peek_view().type;
    if (nt == TokenType::Comma || nt == TokenType::Semicolon) {
      lexer.next();
    } else if (lexer.strict_aousd_conformance &&
               nt != TokenType::CloseBrace && nt != TokenType::Eof &&
               !lexer.newline_before_current()) {
      // Strict (AOUSD) mode only: lenient mode keeps loading such files.
      return ParseResult::Error(
          "Expected newline or ';' between dictionary entries");
    }
  }

  if (!lexer.expect(TokenType::CloseBrace)) {
    return ParseResult::Error("Expected '}' to close dictionary");
  }
  return ParseResult::Ok(std::move(v));
}

ParseResult ParseDict(Lexer& lexer) { return ParseDictDepth(lexer, 0); }
