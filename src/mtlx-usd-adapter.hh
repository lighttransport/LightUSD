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

class AdapterXMLAttribute {
public:
  AdapterXMLAttribute() : valid_(false) {}
  AdapterXMLAttribute(const std::string& value) : value_(value), valid_(true) {}

  operator bool() const { return valid_; }
  const char* as_string() const { return value_.c_str(); }

private:
  std::string value_;
  bool valid_;
};

class AdapterXMLNode {
public:
  AdapterXMLNode() : node_(nullptr) {}
  explicit AdapterXMLNode(SimpleXMLNodePtr n) : node_(n) {}

  operator bool() const { return node_ != nullptr; }

  AdapterXMLAttribute attribute(const char* name) const {
    if (!node_) return AdapterXMLAttribute();

    auto it = node_->attributes.find(name);
    if (it != node_->attributes.end()) {
      return AdapterXMLAttribute(it->second);
    }
    return AdapterXMLAttribute();
  }

  AdapterXMLNode child(const char* name) const {
    if (!node_) return AdapterXMLNode();

    for (const auto& c : node_->children) {
      if (c && c->name == name) {
        return AdapterXMLNode(c);
      }
    }
    return AdapterXMLNode();
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

    AdapterXMLNode operator*() const {
      if (children_ && pos_ < children_->size()) {
        return AdapterXMLNode((*children_)[pos_]);
      }
      return AdapterXMLNode();
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
  std::vector<AdapterXMLNode> children(const char* name) const {
    std::vector<AdapterXMLNode> result;
    if (node_) {
      for (const auto& c : node_->children) {
        if (c && c->name == name) {
          result.push_back(AdapterXMLNode(c));
        }
      }
    }
    return result;
  }

private:
  SimpleXMLNodePtr node_;
};

class AdapterXMLDocument {
public:
  struct ParseResult {
    bool success = false;
    const char* description() const { return error_.c_str(); }
    operator bool() const { return success; }
    std::string error_;
  };

  ParseResult load_string(const char* xml) {
    ParseResult result;
    SimpleXMLParser parser;

    if (parser.Parse(xml)) {
      root_ = AdapterXMLNode(parser.GetRoot());
      result.success = true;
    } else {
      result.success = false;
      result.error_ = parser.GetError();
    }

    return result;
  }

  AdapterXMLNode child(const char* name) const {
    if (root_) {
      if (std::string(root_.name()) == name) {
        return root_;
      }
      return root_.child(name);
    }
    return AdapterXMLNode();
  }

private:
  AdapterXMLNode root_;
};

// Namespace aliases to match pugixml
namespace pugi = mtlx;
using xml_document = AdapterXMLDocument;
using xml_node = AdapterXMLNode;
using xml_attribute = AdapterXMLAttribute;
using xml_parse_result = AdapterXMLDocument::ParseResult;

} // namespace mtlx
} // namespace lightusd
