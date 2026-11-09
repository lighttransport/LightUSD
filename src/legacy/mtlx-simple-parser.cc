// SPDX-License-Identifier: Apache 2.0

#include "mtlx-simple-parser.hh"
#include <cstring>

namespace lightusd {
namespace mtlx {

bool SimpleXMLParser::Parse(const std::string& xml) {
  XMLTokenizer tokenizer;

  if (!tokenizer.Initialize(xml.c_str(), xml.size())) {
    error_ = "Failed to initialize tokenizer: " + tokenizer.GetError();
    return false;
  }

  std::stack<SimpleXMLNodePtr> node_stack;
  SimpleXMLNodePtr current_node;
  Token token;
  bool have_pending_token = false;
  Token pending_token;

  while (have_pending_token || tokenizer.NextToken(token)) {
    // Use pending token if we have one
    if (have_pending_token) {
      token = pending_token;
      have_pending_token = false;
    }

    switch (token.type) {
      case TokenType::ProcessingInstruction:
        // Skip XML declaration
        continue;

      case TokenType::StartTag: {
        auto new_node = std::make_shared<SimpleXMLNode>(token.name);

        // Collect attributes
        Token attr_token;
        bool is_self_closing = false;

        while (tokenizer.NextToken(attr_token)) {
          if (attr_token.type == TokenType::Attribute) {
            new_node->attributes[attr_token.name] = attr_token.value;
          } else if (attr_token.type == TokenType::SelfClosingTag) {
            // Self-closing tag
            is_self_closing = true;
            break;
          } else {
            // We got a non-attribute token, save it for next iteration
            pending_token = attr_token;
            have_pending_token = true;
            break;
          }
        }

        // Add node to tree
        if (!node_stack.empty()) {
          node_stack.top()->children.push_back(new_node);
        } else if (!root_) {
          root_ = new_node;
        }

        // If not self-closing, push to stack for processing children
        // Cap nesting to prevent resource exhaustion on deeply nested input.
        if (!is_self_closing) {
          if (node_stack.size() > 100000) {
            error_ = "XML nesting depth exceeds maximum (100000)";
            return false;
          }
          node_stack.push(new_node);
        }
        break;
      }

      case TokenType::EndTag: {
        if (node_stack.empty()) {
          error_ = "Unexpected end tag: " + token.name;
          return false;
        }

        if (node_stack.top()->name != token.name) {
          error_ = "Mismatched end tag: expected </" + node_stack.top()->name +
                  "> but got </" + token.name + ">";
          return false;
        }

        node_stack.pop();
        break;
      }

      case TokenType::Text:
      case TokenType::CDATA: {
        if (!node_stack.empty()) {
          // Append text to current node
          node_stack.top()->text += token.value;
        }
        break;
      }

      case TokenType::Comment:
        // Ignore comments
        break;

      case TokenType::EndOfDocument:
        if (!node_stack.empty()) {
          error_ = "Unclosed tags at end of document";
          return false;
        }
        return true;

      case TokenType::Error:
        error_ = "Tokenizer error: " + tokenizer.GetError();
        return false;

      case TokenType::SelfClosingTag:
      case TokenType::Attribute:
        // These are handled within StartTag processing
        break;
    }
  }

  if (!node_stack.empty()) {
    error_ = "Unclosed tags at end of document";
    return false;
  }

  return root_ != nullptr;
}

} // namespace mtlx
} // namespace lightusd
