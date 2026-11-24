// SPDX-License-Identifier: Apache 2.0
// MaterialX XML Tokenizer - Simple, secure, dependency-free XML tokenizer
// Designed specifically for MaterialX parsing with security in mind

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace lightusd {
namespace mtlx {

enum class TokenType {
  StartTag,        // <element
  EndTag,          // </element>
  SelfClosingTag,  // />
  Attribute,       // name="value"
  Text,            // Text content between tags
  Comment,         // <!-- comment -->
  ProcessingInstruction, // <?xml ... ?>
  CDATA,           // <![CDATA[ ... ]]>
  EndOfDocument,
  Error
};

struct Token {
  TokenType type;
  std::string name;   // Tag/attribute name
  std::string value;  // Attribute value or text content
  size_t line;
  size_t column;
};

class XMLTokenizer {
public:
  XMLTokenizer() = default;
  ~XMLTokenizer() = default;

  // Initialize tokenizer with input data
  // Returns false if data is nullptr or size exceeds max_size
  bool Initialize(const char* data, size_t size, size_t max_size = 1024 * 1024 * 100);

  // Get next token
  bool NextToken(Token& token);

  // Peek at next token without consuming it
  bool PeekToken(Token& token);

  // Get current position in document
  void GetPosition(size_t& line, size_t& column) const {
    line = current_line_;
    column = current_column_;
  }

  // Get error message if last operation failed
  const std::string& GetError() const { return error_; }

private:
  // Internal parsing methods
  bool SkipWhitespace();
  bool ParseStartTag(Token& token);
  bool ParseEndTag(Token& token);
  bool ParseAttribute(Token& token);
  bool ParseText(Token& token);
  bool ParseComment(Token& token);
  bool ParseCDATA(Token& token);
  bool ParseProcessingInstruction(Token& token);

  // Helper methods for safe string parsing
  bool ParseName(std::string& name);
  bool ParseQuotedString(std::string& str, char quote);
  bool ParseUntil(std::string& str, const char* delimiter);

  // Safe character access with bounds checking
  char PeekChar(size_t offset = 0) const;
  char NextChar();
  bool Match(const char* str);
  bool Consume(const char* str);

  // Update line/column position
  void UpdatePosition(char c);

  // Input data
  const char* data_ = nullptr;
  size_t size_ = 0;
  size_t position_ = 0;

  // Current position tracking
  size_t current_line_ = 1;
  size_t current_column_ = 1;

  // Error state
  std::string error_;

  // Parsing state
  bool in_tag_ = false;
  std::string current_tag_name_;

  // Security limits
  static constexpr size_t MAX_NAME_LENGTH = 256;
  static constexpr size_t MAX_STRING_LENGTH = 64 * 1024;
  static constexpr size_t MAX_TEXT_LENGTH = 1024 * 1024;
};

} // namespace mtlx
} // namespace lightusd
