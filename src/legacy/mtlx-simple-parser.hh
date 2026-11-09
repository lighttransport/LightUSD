// SPDX-License-Identifier: Apache 2.0
// Simple, robust MaterialX XML parser

#pragma once

#include "mtlx-xml-tokenizer.hh"
#include <memory>
#include <map>
#include <stack>

namespace lightusd {
namespace mtlx {

// Forward declaration
class SimpleXMLNode;
using SimpleXMLNodePtr = std::shared_ptr<SimpleXMLNode>;

// Simple XML node
class SimpleXMLNode {
public:
  std::string name;
  std::string text;
  std::map<std::string, std::string> attributes;
  std::vector<SimpleXMLNodePtr> children;

  SimpleXMLNode() = default;
  explicit SimpleXMLNode(const std::string& n) : name(n) {}

  SimpleXMLNodePtr GetChild(const std::string& n) const {
    for (const auto& child : children) {
      if (child && child->name == n) {
        return child;
      }
    }
    return nullptr;
  }

  std::string GetAttribute(const std::string& n, const std::string& def = "") const {
    auto it = attributes.find(n);
    return (it != attributes.end()) ? it->second : def;
  }
};

// Simple XML parser
class SimpleXMLParser {
public:
  bool Parse(const std::string& xml);
  SimpleXMLNodePtr GetRoot() const { return root_; }
  const std::string& GetError() const { return error_; }

private:
  SimpleXMLNodePtr root_;
  std::string error_;
};

} // namespace mtlx
} // namespace lightusd
