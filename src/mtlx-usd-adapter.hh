// SPDX-License-Identifier: Apache 2.0
// MaterialX to USD adapter - replaces pugixml with our secure parser

#pragma once

#include "mtlx-simple-parser.hh"
#include <string>
#include <functional>

namespace lightusd {
namespace mtlx {

// Adapter to replace pugixml with our parser
// This provides a pugixml-like interface for easy migration

class XMLAttribute {
public:
  XMLAttribute() : valid_(false) {}
  XMLAttribute(const std::string& value) : value_(value), valid_(true) {}

  operator bool() const { return valid_; }
  const char* as_string() const { return value_.c_str(); }

private:
  std::string value_;
  bool valid_;
};

class XMLNode {
public:
  XMLNode() : node_(nullptr) {}
  explicit XMLNode(SimpleXMLNodePtr n) : node_(n) {}

  operator bool() const { return node_ != nullptr; }

  XMLAttribute attribute(const char* name) const {
    if (!node_) return XMLAttribute();

    auto it = node_->attributes.find(name);
    if (it != node_->attributes.end()) {
      return XMLAttribute(it->second);
    }
    return XMLAttribute();
  }

  XMLNode child(const char* name) const {
    if (!node_) return XMLNode();

    for (const auto& c : node_->children) {
      if (c && c->name == name) {
        return XMLNode(c);
      }
    }
    return XMLNode();
  }

  const char* name() const {
    return node_ ? node_->name.c_str() : "";
  }

  const char* child_value() const {
    return node_ ? node_->text.c_str() : "";
  }

  const std::map<std::string, std::string> &attributes() const {
    static const std::map<std::string, std::string> empty;
    return node_ ? node_->attributes : empty;
  }

  // Iterator support
  class iterator {
  public:
    iterator() : children_(nullptr), pos_(0) {}
    iterator(const std::vector<SimpleXMLNodePtr>* children, size_t pos = 0)
      : children_(children), pos_(pos) {}

    iterator& operator++() {
      ++pos_;
      return *this;
    }

    bool operator!=(const iterator& other) const {
      return pos_ != other.pos_;
    }

    XMLNode operator*() const {
      if (children_ && pos_ < children_->size()) {
        return XMLNode((*children_)[pos_]);
      }
      return XMLNode();
    }

  private:
    const std::vector<SimpleXMLNodePtr>* children_;
    size_t pos_;
  };

  iterator begin() const {
    return node_ ? iterator(&node_->children) : iterator();
  }

  iterator end() const {
    return node_ ? iterator(&node_->children, node_->children.size()) : iterator();
  }

  // Get children with specific name
  std::vector<XMLNode> children(const char* name) const {
    std::vector<XMLNode> result;
    if (node_) {
      for (const auto& c : node_->children) {
        if (c && c->name == name) {
          result.push_back(XMLNode(c));
        }
      }
    }
    return result;
  }

private:
  SimpleXMLNodePtr node_;
};

class XMLDocument {
public:
  struct ParseResult {
    bool success;
    const char* description() const { return error_.c_str(); }
    operator bool() const { return success; }
    std::string error_;
  };

  ParseResult load_string(const char* xml) {
    ParseResult result;
    SimpleXMLParser parser;

    if (parser.Parse(xml)) {
      root_ = XMLNode(parser.GetRoot());
      result.success = true;
    } else {
      result.success = false;
      result.error_ = parser.GetError();
    }

    return result;
  }

  XMLNode child(const char* name) const {
    if (root_) {
      if (std::string(root_.name()) == name) {
        return root_;
      }
      return root_.child(name);
    }
    return XMLNode();
  }

private:
  XMLNode root_;
};

// Namespace aliases to match pugixml
namespace pugi = mtlx;
using xml_document = XMLDocument;
using xml_node = XMLNode;
using xml_attribute = XMLAttribute;
using xml_parse_result = XMLDocument::ParseResult;

} // namespace mtlx
} // namespace lightusd
