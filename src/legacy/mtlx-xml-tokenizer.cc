// SPDX-License-Identifier: Apache 2.0

#include "mtlx-xml-tokenizer.hh"
#include <algorithm>
#include <cctype>

namespace lightusd {
namespace mtlx {

bool XMLTokenizer::Initialize(const char* data, size_t size, size_t max_size) {
  if (!data) {
    error_ = "Input data is null";
    return false;
  }

  if (size > max_size) {
    error_ = "Input size exceeds maximum allowed size";
    return false;
  }

  data_ = data;
  size_ = size;
  position_ = 0;
  current_line_ = 1;
  current_column_ = 1;
  in_tag_ = false;
  error_.clear();

  return true;
}

char XMLTokenizer::PeekChar(size_t offset) const {
  size_t pos = position_ + offset;
  if (pos >= size_) {
    return '\0';
  }
  return data_[pos];
}

char XMLTokenizer::NextChar() {
  if (position_ >= size_) {
    return '\0';
  }
  char c = data_[position_++];
  UpdatePosition(c);
  return c;
}

void XMLTokenizer::UpdatePosition(char c) {
  if (c == '\n') {
    current_line_++;
    current_column_ = 1;
  } else if (c != '\r') {
    current_column_++;
  }
}

bool XMLTokenizer::Match(const char* str) {
  if (!str) return false;

  size_t len = std::strlen(str);
  if (position_ + len > size_) {
    return false;
  }

  return std::memcmp(data_ + position_, str, len) == 0;
}

bool XMLTokenizer::Consume(const char* str) {
  if (!Match(str)) return false;

  size_t len = std::strlen(str);
  for (size_t i = 0; i < len; ++i) {
    NextChar();
  }
  return true;
}

bool XMLTokenizer::SkipWhitespace() {
  bool skipped = false;
  while (position_ < size_) {
    char c = PeekChar();
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      NextChar();
      skipped = true;
    } else {
      break;
    }
  }
  return skipped;
}

bool XMLTokenizer::ParseName(std::string& name) {
  name.clear();

  char c = PeekChar();
  // XML name must start with letter or underscore
  if (!std::isalpha(c) && c != '_' && c != ':') {
    return false;
  }

  while (position_ < size_ && name.length() < MAX_NAME_LENGTH) {
    c = PeekChar();
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':') {
      name += NextChar();
    } else {
      break;
    }
  }

  if (name.length() >= MAX_NAME_LENGTH) {
    error_ = "Name exceeds maximum length";
    return false;
  }

  return !name.empty();
}

bool XMLTokenizer::ParseQuotedString(std::string& str, char quote) {
  str.clear();

  if (PeekChar() != quote) {
    return false;
  }
  NextChar(); // Consume opening quote

  while (position_ < size_ && str.length() < MAX_STRING_LENGTH) {
    char c = PeekChar();
    if (c == quote) {
      NextChar(); // Consume closing quote
      return true;
    } else if (c == '&') {
      // Handle XML entities
      if (Match("&lt;")) {
        Consume("&lt;");
        str += '<';
      } else if (Match("&gt;")) {
        Consume("&gt;");
        str += '>';
      } else if (Match("&amp;")) {
        Consume("&amp;");
        str += '&';
      } else if (Match("&quot;")) {
        Consume("&quot;");
        str += '"';
      } else if (Match("&apos;")) {
        Consume("&apos;");
        str += '\'';
      } else {
        // Unknown entity, treat as literal
        str += NextChar();
      }
    } else if (c == '\0') {
      error_ = "Unexpected end of input in quoted string";
      return false;
    } else {
      str += NextChar();
    }
  }

  if (str.length() >= MAX_STRING_LENGTH) {
    error_ = "String exceeds maximum length";
    return false;
  }

  error_ = "Unterminated quoted string";
  return false;
}

bool XMLTokenizer::ParseUntil(std::string& str, const char* delimiter) {
  str.clear();
  size_t delim_len = std::strlen(delimiter);
  (void)delim_len; // Currently unused, may be used for validation in the future

  while (position_ < size_ && str.length() < MAX_TEXT_LENGTH) {
    if (Match(delimiter)) {
      return true;
    }
    str += NextChar();
  }

  if (str.length() >= MAX_TEXT_LENGTH) {
    error_ = "Text exceeds maximum length";
    return false;
  }

  return false;
}

bool XMLTokenizer::ParseComment(Token& token) {
  if (!Consume("<!--")) {
    return false;
  }

  token.type = TokenType::Comment;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseUntil(token.value, "-->")) {
    error_ = "Unterminated comment";
    return false;
  }

  Consume("-->");
  return true;
}

bool XMLTokenizer::ParseCDATA(Token& token) {
  if (!Consume("<![CDATA[")) {
    return false;
  }

  token.type = TokenType::CDATA;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseUntil(token.value, "]]>")) {
    error_ = "Unterminated CDATA section";
    return false;
  }

  Consume("]]>");
  return true;
}

bool XMLTokenizer::ParseProcessingInstruction(Token& token) {
  if (!Consume("<?")) {
    return false;
  }

  token.type = TokenType::ProcessingInstruction;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseName(token.name)) {
    error_ = "Invalid processing instruction name";
    return false;
  }

  SkipWhitespace();

  if (!ParseUntil(token.value, "?>")) {
    error_ = "Unterminated processing instruction";
    return false;
  }

  // Trim trailing whitespace from value
  while (!token.value.empty() && std::isspace(token.value.back())) {
    token.value.pop_back();
  }

  Consume("?>");
  return true;
}

bool XMLTokenizer::ParseStartTag(Token& token) {
  if (PeekChar() != '<') {
    return false;
  }

  // Check for special cases
  if (Match("<!--")) {
    return ParseComment(token);
  }
  if (Match("<![CDATA[")) {
    return ParseCDATA(token);
  }
  if (Match("<?")) {
    return ParseProcessingInstruction(token);
  }
  if (Match("</")) {
    return ParseEndTag(token);
  }

  NextChar(); // Consume '<'

  token.type = TokenType::StartTag;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseName(token.name)) {
    error_ = "Invalid tag name";
    return false;
  }

  current_tag_name_ = token.name;
  in_tag_ = true;

  return true;
}

bool XMLTokenizer::ParseEndTag(Token& token) {
  if (!Consume("</")) {
    return false;
  }

  token.type = TokenType::EndTag;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseName(token.name)) {
    error_ = "Invalid end tag name";
    return false;
  }

  SkipWhitespace();

  if (PeekChar() != '>') {
    error_ = "Expected '>' after end tag name";
    return false;
  }
  NextChar();

  in_tag_ = false;
  return true;
}

bool XMLTokenizer::ParseAttribute(Token& token) {
  if (!in_tag_) {
    return false;
  }

  SkipWhitespace();

  // Check for tag closure
  if (Match("/>")) {
    Consume("/>");
    token.type = TokenType::SelfClosingTag;
    token.name = current_tag_name_;
    token.line = current_line_;
    token.column = current_column_;
    in_tag_ = false;
    return true;
  }

  if (PeekChar() == '>') {
    NextChar();
    in_tag_ = false;
    // Return to indicate end of attributes, caller should retry for content
    return false;
  }

  token.type = TokenType::Attribute;
  token.line = current_line_;
  token.column = current_column_;

  if (!ParseName(token.name)) {
    error_ = "Invalid attribute name";
    return false;
  }

  SkipWhitespace();

  if (PeekChar() != '=') {
    // Attribute without value (like HTML boolean attributes)
    token.value.clear();
    return true;
  }
  NextChar(); // Consume '='

  SkipWhitespace();

  char quote = PeekChar();
  if (quote != '"' && quote != '\'') {
    error_ = "Expected quoted attribute value";
    return false;
  }

  if (!ParseQuotedString(token.value, quote)) {
    error_ = "Invalid attribute value";
    return false;
  }

  return true;
}

bool XMLTokenizer::ParseText(Token& token) {
  token.type = TokenType::Text;
  token.line = current_line_;
  token.column = current_column_;
  token.value.clear();

  while (position_ < size_ && token.value.length() < MAX_TEXT_LENGTH) {
    char c = PeekChar();
    if (c == '<') {
      // End of text content
      break;
    } else if (c == '&') {
      // Handle XML entities
      if (Match("&lt;")) {
        Consume("&lt;");
        token.value += '<';
      } else if (Match("&gt;")) {
        Consume("&gt;");
        token.value += '>';
      } else if (Match("&amp;")) {
        Consume("&amp;");
        token.value += '&';
      } else if (Match("&quot;")) {
        Consume("&quot;");
        token.value += '"';
      } else if (Match("&apos;")) {
        Consume("&apos;");
        token.value += '\'';
      } else {
        // Unknown entity, treat as literal
        token.value += NextChar();
      }
    } else {
      token.value += NextChar();
    }
  }

  if (token.value.length() >= MAX_TEXT_LENGTH) {
    error_ = "Text content exceeds maximum length";
    return false;
  }

  // Trim whitespace-only text between tags
  bool all_whitespace = true;
  for (char c : token.value) {
    if (!std::isspace(c)) {
      all_whitespace = false;
      break;
    }
  }

  if (all_whitespace && !token.value.empty()) {
    // Skip whitespace-only text, try next token
    return NextToken(token);
  }

  return !token.value.empty();
}

bool XMLTokenizer::NextToken(Token& token) {
  token = Token();

  if (position_ >= size_) {
    token.type = TokenType::EndOfDocument;
    return true;
  }

  // If we're inside a tag, parse attributes
  if (in_tag_) {
    const size_t before = position_;
    if (ParseAttribute(token)) {
      return true;
    }
    // An attribute that could not be parsed without consuming input would be
    // retried forever (e.g. `<a x=1/>`): report a tokenizer error instead.
    if (in_tag_ && position_ == before) {
      in_tag_ = false;
      position_ = size_;
      token.type = TokenType::Error;
      token.value = "Malformed attribute in <" + current_tag_name_ + ">";
      return false;
    }
    // ParseAttribute returns false when a non-self-closing start tag consumes
    // its closing '>'. Return an empty text sentinel so callers that are
    // parsing attributes do not accidentally consume the first content token.
    token.type = TokenType::Text;
    token.value.clear();
    return true;
  }

  // Skip whitespace between tags
  SkipWhitespace();

  if (position_ >= size_) {
    token.type = TokenType::EndOfDocument;
    return true;
  }

  // Check what's next
  char c = PeekChar();

  if (c == '<') {
    return ParseStartTag(token);
  } else {
    return ParseText(token);
  }
}

bool XMLTokenizer::PeekToken(Token& token) {
  // Save current state
  size_t saved_pos = position_;
  size_t saved_line = current_line_;
  size_t saved_col = current_column_;
  bool saved_in_tag = in_tag_;
  std::string saved_tag_name = current_tag_name_;

  // Get next token
  bool result = NextToken(token);

  // Restore state
  position_ = saved_pos;
  current_line_ = saved_line;
  current_column_ = saved_col;
  in_tag_ = saved_in_tag;
  current_tag_name_ = saved_tag_name;

  return result;
}

} // namespace mtlx
} // namespace lightusd
