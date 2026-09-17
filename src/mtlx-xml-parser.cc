// SPDX-License-Identifier: Apache 2.0

#include "mtlx-xml-parser.hh"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace lightusd {
namespace mtlx {

// XMLNode implementation

bool XMLNode::HasAttribute(const std::string& name) const {
  return attributes_.find(name) != attributes_.end();
}

std::string XMLNode::GetAttribute(const std::string& name, const std::string& default_value) const {
  auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    return it->second;
  }
  return default_value;
}

bool XMLNode::GetAttributeInt(const std::string& name, int& value) const {
  auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    long val = 0;
    const auto parsed = std::from_chars(it->second.data(),
                                        it->second.data() + it->second.size(),
                                        val, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == it->second.data() + it->second.size()) {
      value = static_cast<int>(val);
      return true;
    }
  }
  return false;
}

bool XMLNode::GetAttributeFloat(const std::string& name, float& value) const {
  auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    char* endptr;
    float val = std::strtof(it->second.c_str(), &endptr);
    if (*endptr == '\0') {
      value = val;
      return true;
    }
  }
  return false;
}

bool XMLNode::GetAttributeBool(const std::string& name, bool& value) const {
  auto it = attributes_.find(name);
  if (it != attributes_.end()) {
    const std::string& str = it->second;
    if (str == "true" || str == "1" || str == "yes") {
      value = true;
      return true;
    } else if (str == "false" || str == "0" || str == "no") {
      value = false;
      return true;
    }
  }
  return false;
}

void XMLNode::SetAttribute(const std::string& name, const std::string& value) {
  attributes_[name] = value;
}

void XMLNode::AddChild(XMLNodePtr child) {
  if (child) {
    child->SetParent(this);
    children_.push_back(child);
  }
}

std::vector<XMLNodePtr> XMLNode::GetChildren(const std::string& name) const {
  std::vector<XMLNodePtr> result;
  for (const auto& child : children_) {
    if (child && child->GetName() == name) {
      result.push_back(child);
    }
  }
  return result;
}

XMLNodePtr XMLNode::GetChild(const std::string& name) const {
  for (const auto& child : children_) {
    if (child && child->GetName() == name) {
      return child;
    }
  }
  return nullptr;
}

XMLNodePtr XMLNode::GetFirstChild() const {
  if (!children_.empty()) {
    return children_.front();
  }
  return nullptr;
}

XMLNodePtr XMLNode::FindNode(const std::string& path) const {
  if (path.empty()) {
    return nullptr;
  }

  // Split path by '/'
  size_t pos = path.find('/');
  std::string first = (pos == std::string::npos) ? path : path.substr(0, pos);
  std::string rest = (pos == std::string::npos) ? "" : path.substr(pos + 1);

  // Find child with matching name
  for (const auto& child : children_) {
    if (child && child->GetName() == first) {
      if (rest.empty()) {
        return child;
      } else {
        return child->FindNode(rest);
      }
    }
  }

  return nullptr;
}

std::vector<XMLNodePtr> XMLNode::FindNodes(const std::string& path) const {
  std::vector<XMLNodePtr> result;

  if (path.empty()) {
    return result;
  }

  // Split path by '/'
  size_t pos = path.find('/');
  std::string first = (pos == std::string::npos) ? path : path.substr(0, pos);
  std::string rest = (pos == std::string::npos) ? "" : path.substr(pos + 1);

  // Find all children with matching name
  for (const auto& child : children_) {
    if (child && child->GetName() == first) {
      if (rest.empty()) {
        result.push_back(child);
      } else {
        auto sub_results = child->FindNodes(rest);
        result.insert(result.end(), sub_results.begin(), sub_results.end());
      }
    }
  }

  return result;
}

// XMLDocument implementation

bool XMLDocument::ParseString(const std::string& xml_string) {
  return ParseMemory(xml_string.c_str(), xml_string.size());
}

bool XMLDocument::ParseMemory(const char* data, size_t size) {
  XMLTokenizer tokenizer;

  if (!tokenizer.Initialize(data, size)) {
    error_ = "Failed to initialize tokenizer: " + tokenizer.GetError();
    return false;
  }

  // Skip any processing instructions at the beginning
  Token token;
  while (tokenizer.NextToken(token)) {
    if (token.type == TokenType::ProcessingInstruction) {
      // Skip XML declaration
      continue;
    } else if (token.type == TokenType::StartTag) {
      // Found root element
      root_ = std::make_shared<XMLNode>(token.name);

      // Parse attributes of root element
      if (!ParseAttributes(tokenizer, root_)) {
        return false;
      }

      // Parse children
      current_depth_ = 1;
      if (!ParseNode(tokenizer, root_)) {
        return false;
      }

      break;
    } else if (token.type == TokenType::EndOfDocument) {
      error_ = "No root element found";
      return false;
    }
  }

  if (!root_) {
    error_ = "Failed to parse root element";
    return false;
  }

  return true;
}

bool XMLDocument::ParseAttributes(XMLTokenizer& tokenizer, XMLNodePtr node) {
  Token token;

  while (tokenizer.NextToken(token)) {
    if (token.type == TokenType::Attribute) {
      node->SetAttribute(token.name, token.value);
    } else if (token.type == TokenType::SelfClosingTag) {
      // Node is self-closing, no children
      return true;
    } else {
      // End of attributes, put token back for next parse
      // Since we can't put back, we'll handle this in ParseNode
      break;
    }
  }

  return true;
}

bool XMLDocument::ParseNode(XMLTokenizer& tokenizer, XMLNodePtr parent) {
  if (current_depth_ > MAX_DEPTH) {
    error_ = "Maximum nesting depth exceeded";
    return false;
  }

  Token token;
  std::string accumulated_text;

  while (tokenizer.NextToken(token)) {
    switch (token.type) {
      case TokenType::StartTag: {
        // Save any accumulated text first
        if (!accumulated_text.empty()) {
          // Trim whitespace
          size_t start = accumulated_text.find_first_not_of(" \t\n\r");
          size_t end = accumulated_text.find_last_not_of(" \t\n\r");
          if (start != std::string::npos && end != std::string::npos) {
            parent->SetText(accumulated_text.substr(start, end - start + 1));
          }
          accumulated_text.clear();
        }

        // Create new child node
        auto child = std::make_shared<XMLNode>(token.name);
        parent->AddChild(child);

        // Parse attributes
        bool self_closing = false;
        Token attr_token;
        while (tokenizer.NextToken(attr_token)) {
          if (attr_token.type == TokenType::Attribute) {
            child->SetAttribute(attr_token.name, attr_token.value);
          } else if (attr_token.type == TokenType::SelfClosingTag) {
            self_closing = true;
            break;
          } else {
            // Not an attribute, this starts the content
            // We need to handle this token
            if (attr_token.type == TokenType::Text) {
              // This is text content for the child
              child->SetText(attr_token.value);
            } else if (attr_token.type == TokenType::StartTag) {
              // This is a nested child, parse recursively
              auto nested = std::make_shared<XMLNode>(attr_token.name);
              child->AddChild(nested);

              // Parse nested attributes
              if (!ParseAttributes(tokenizer, nested)) {
                return false;
              }

              // Parse nested children
              current_depth_++;
              if (!ParseNode(tokenizer, nested)) {
                return false;
              }
              current_depth_--;
            } else if (attr_token.type == TokenType::EndTag) {
              // This ends the child element
              if (attr_token.name != child->GetName()) {
                error_ = "Mismatched end tag: expected </" + child->GetName() +
                        "> but got </" + attr_token.name + ">";
                return false;
              }
              break;
            }
            break;
          }
        }

        if (!self_closing) {
          // Parse children recursively
          current_depth_++;
          if (!ParseNode(tokenizer, child)) {
            return false;
          }
          current_depth_--;
        }
        break;
      }

      case TokenType::EndTag:
        // Save any accumulated text first
        if (!accumulated_text.empty()) {
          // Trim whitespace
          size_t start = accumulated_text.find_first_not_of(" \t\n\r");
          size_t end = accumulated_text.find_last_not_of(" \t\n\r");
          if (start != std::string::npos && end != std::string::npos) {
            parent->SetText(accumulated_text.substr(start, end - start + 1));
          }
        }

        if (token.name != parent->GetName()) {
          error_ = "Mismatched end tag: expected </" + parent->GetName() +
                  "> but got </" + token.name + ">";
          return false;
        }
        return true;

      case TokenType::Text:
      case TokenType::CDATA:
        accumulated_text += token.value;
        break;

      case TokenType::Comment:
        // Ignore comments
        break;

      case TokenType::EndOfDocument:
        // Unexpected end of document
        error_ = "Unexpected end of document while parsing <" + parent->GetName() + ">";
        return false;

      case TokenType::Error:
        error_ = "Tokenizer error";
        return false;

      case TokenType::SelfClosingTag:
      case TokenType::Attribute:
      case TokenType::ProcessingInstruction:
        // These are handled within other token processing
        error_ = "Unexpected token type at this position";
        return false;
    }
  }

  return true;
}

XMLNodePtr XMLDocument::FindNode(const std::string& path) const {
  if (root_) {
    return root_->FindNode(path);
  }
  return nullptr;
}

std::vector<XMLNodePtr> XMLDocument::FindNodes(const std::string& path) const {
  if (root_) {
    return root_->FindNodes(path);
  }
  return {};
}

// MaterialXParser implementation

bool MaterialXParser::Parse(const std::string& xml_string) {
  if (!document_.ParseString(xml_string)) {
    error_ = document_.GetError();
    return false;
  }

  // Check if root is materialx
  auto root = document_.GetRoot();
  if (!root || root->GetName() != "materialx") {
    error_ = "Root element must be <materialx>";
    return false;
  }

  // Validate version
  std::string version = root->GetAttribute("version");
  if (version.empty()) {
    error_ = "Missing version attribute in <materialx>";
    return false;
  }

  if (!ValidateVersion(version)) {
    warning_ = "Unknown MaterialX version: " + version;
  }

  return true;
}

bool MaterialXParser::ParseFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    error_ = "Failed to open file: " + filename;
    return false;
  }

  // Read file content
  std::stringstream buffer;
  buffer << file.rdbuf();

  return Parse(buffer.str());
}

bool MaterialXParser::Validate() {
  auto root = document_.GetRoot();
  if (!root) {
    error_ = "No document to validate";
    return false;
  }

  // Validate all nodes recursively
  return ValidateNode(root);
}

std::string MaterialXParser::GetVersion() const {
  auto root = document_.GetRoot();
  if (root) {
    return root->GetAttribute("version");
  }
  return "";
}

std::string MaterialXParser::GetColorSpace() const {
  auto root = document_.GetRoot();
  if (root) {
    return root->GetAttribute("colorspace");
  }
  return "";
}

std::string MaterialXParser::GetNamespace() const {
  auto root = document_.GetRoot();
  if (root) {
    return root->GetAttribute("namespace");
  }
  return "";
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif

bool MaterialXParser::ValidateVersion(const std::string& version) {
  // MaterialX versions we support
  static const std::vector<std::string> supported_versions = {
    "1.39", "1.38", "1.37", "1.36"
  };

  return std::find(supported_versions.begin(), supported_versions.end(), version) !=
         supported_versions.end();
}

bool MaterialXParser::ValidateNode(XMLNodePtr root_node) {
  // Iterative pre-order DFS (heap worklist) so deeply nested MaterialX node
  // trees cannot overflow the call stack. Children are pushed in reverse so
  // they are visited left-to-right, preserving warning-accumulation order.
  std::vector<XMLNodePtr> stack;
  stack.push_back(root_node);

  while (!stack.empty()) {
    XMLNodePtr node = stack.back();
    stack.pop_back();

    if (!node) return false;

  const std::string& name = node->GetName();

  // Validate known MaterialX elements
  static const std::vector<std::string> valid_elements = {
    "materialx", "nodegraph", "node", "input", "output", "token",
    "variant", "variantset", "variantassign", "visibility",
    "collection", "geom", "material", "surfacematerial",
    "volumematerial", "look", "property", "propertyset",
    "propertyassign", "materialassign", "geominfo", "geomprop",
    "implementation", "nodeDef", "typedef", "member", "unit",
    "unitdef", "unittypedef", "targetdef", "attributedef"
  };

  bool valid = std::find(valid_elements.begin(), valid_elements.end(), name) !=
               valid_elements.end();

  if (!valid) {
    warning_ += "Unknown element: <" + name + ">\n";
  }

  // Validate type attribute if present
  std::string type = node->GetAttribute("type");
  if (!type.empty() && !ValidateType(type)) {
    warning_ += "Unknown type: " + type + " in <" + name + ">\n";
  }

  // Enqueue children in reverse for left-to-right pre-order traversal.
  const auto& children = node->GetChildren();
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    stack.push_back(*it);
  }
  }  // while

  return true;
}

bool MaterialXParser::ValidateType(const std::string& type_name) {
  static const std::vector<std::string> valid_types = {
    "integer", "boolean", "float", "color3", "color4",
    "vector2", "vector3", "vector4", "matrix33", "matrix44",
    "string", "filename", "integerarray", "floatarray",
    "vector2array", "vector3array", "vector4array",
    "color3array", "color4array", "stringarray",
    "surfaceshader", "displacementshader", "volumeshader",
    "lightshader", "geomname", "geomnamearray"
  };

  return std::find(valid_types.begin(), valid_types.end(), type_name) !=
         valid_types.end();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace mtlx
} // namespace lightusd
