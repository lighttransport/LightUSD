// Debug XML parser

#include "../include/mtlx-xml-parser.hh"
#include <iostream>

using namespace lightusd::mtlx;

using namespace lightusd::legacy::mtlx;  // NOLINT

int main() {
  const char* xml = R"(<?xml version="1.0"?>
    <materialx version="1.38">
      <nodegraph name="test"/>
    </materialx>
  )";

  std::cout << "Parsing simple XML..." << std::endl;

  XMLDocument doc;
  if (!doc.ParseString(xml)) {
    std::cerr << "Parse failed: " << doc.GetError() << std::endl;

    // Try with tokenizer directly
    XMLTokenizer tokenizer;
    if (!tokenizer.Initialize(xml, std::strlen(xml))) {
      std::cerr << "Tokenizer init failed: " << tokenizer.GetError() << std::endl;
      return 1;
    }

    Token token;
    while (tokenizer.NextToken(token)) {
      if (token.type == TokenType::Error) {
        std::cerr << "Token error at " << token.line << ":" << token.column << std::endl;
        break;
      }
      if (token.type == TokenType::EndOfDocument) break;

      switch (token.type) {
        case TokenType::StartTag:
          std::cout << "Start: " << token.name << std::endl;
          break;
        case TokenType::EndTag:
          std::cout << "End: " << token.name << std::endl;
          break;
        case TokenType::Attribute:
          std::cout << "  Attr: " << token.name << "=" << token.value << std::endl;
          break;
        case TokenType::SelfClosingTag:
          std::cout << "SelfClose: " << token.name << std::endl;
          break;
        default:
          break;
      }
    }

    return 1;
  }

  std::cout << "Parse succeeded!" << std::endl;

  auto root = doc.GetRoot();
  if (root) {
    std::cout << "Root: " << root->GetName() << std::endl;
    std::cout << "Version: " << root->GetAttribute("version") << std::endl;
  }

  return 0;
}