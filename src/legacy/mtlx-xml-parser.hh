// SPDX-License-Identifier: Apache 2.0
// MaterialX XML Parser - DOM-style parser for MaterialX documents

#pragma once

#include "mtlx-xml-tokenizer.hh"
#include <memory>
#include <map>

namespace lightusd {
namespace mtlx {

class XMLNode;
using XMLNodePtr = std::shared_ptr<XMLNode>;

// XML Attribute
struct XMLAttribute {
  std::string name;
  std::string value;
};

// XML Node representing an element in the DOM
class XMLNode {
public:
  XMLNode() = default;
  explicit XMLNode(const std::string& name) : name_(name) {}

  // Node properties
  const std::string& GetName() const { return name_; }
  void SetName(const std::string& name) { name_ = name; }

  const std::string& GetText() const { return text_; }
  void SetText(const std::string& text) { text_ = text; }

  // Attributes
  bool HasAttribute(const std::string& name) const;
  std::string GetAttribute(const std::string& name, const std::string& default_value = "") const;
  bool GetAttributeInt(const std::string& name, int& value) const;
  bool GetAttributeFloat(const std::string& name, float& value) const;
  bool GetAttributeBool(const std::string& name, bool& value) const;
  void SetAttribute(const std::string& name, const std::string& value);
  const std::map<std::string, std::string>& GetAttributes() const { return attributes_; }

  // Children
  void AddChild(XMLNodePtr child);
  const std::vector<XMLNodePtr>& GetChildren() const { return children_; }
  std::vector<XMLNodePtr> GetChildren(const std::string& name) const;
  XMLNodePtr GetChild(const std::string& name) const;
  XMLNodePtr GetFirstChild() const;

  // Parent
  XMLNode* GetParent() const { return parent_; }
  void SetParent(XMLNode* parent) { parent_ = parent; }

  // Utilities
  bool IsEmpty() const { return children_.empty() && text_.empty(); }
  size_t GetChildCount() const { return children_.size(); }

  // Path-based access (e.g., "nodegraph/input")
  XMLNodePtr FindNode(const std::string& path) const;
  std::vector<XMLNodePtr> FindNodes(const std::string& path) const;

private:
  std::string name_;
  std::string text_;
  std::map<std::string, std::string> attributes_;
  std::vector<XMLNodePtr> children_;
  XMLNode* parent_ = nullptr;
};

// XML Document
class XMLDocument {
public:
  XMLDocument() = default;
  ~XMLDocument() = default;

  // Parse XML from string
  bool ParseString(const std::string& xml_string);
  bool ParseMemory(const char* data, size_t size);

  // Get root node
  XMLNodePtr GetRoot() const { return root_; }

  // Get parse error if any
  const std::string& GetError() const { return error_; }

  // Utility methods
  XMLNodePtr FindNode(const std::string& path) const;
  std::vector<XMLNodePtr> FindNodes(const std::string& path) const;

private:
  bool ParseNode(XMLTokenizer& tokenizer, XMLNodePtr parent);
  bool ParseAttributes(XMLTokenizer& tokenizer, XMLNodePtr node);

  XMLNodePtr root_;
  std::string error_;

  // Security limits
  static constexpr size_t MAX_DEPTH = 1000;
  size_t current_depth_ = 0;
};

// MaterialX-specific parser built on top of XMLDocument
class MaterialXParser {
public:
  MaterialXParser() = default;
  ~MaterialXParser() = default;

  // Parse MaterialX document
  bool Parse(const std::string& xml_string);
  bool ParseFile(const std::string& filename);

  // Get parsed document
  XMLDocument& GetDocument() { return document_; }
  const XMLDocument& GetDocument() const { return document_; }

  // MaterialX-specific validation
  bool Validate();

  // Get errors/warnings
  const std::string& GetError() const { return error_; }
  const std::string& GetWarning() const { return warning_; }

  // MaterialX version info
  std::string GetVersion() const;
  std::string GetColorSpace() const;
  std::string GetNamespace() const;

private:
  XMLDocument document_;
  std::string error_;
  std::string warning_;

  bool ValidateVersion(const std::string& version);
  bool ValidateNode(XMLNodePtr node);
  bool ValidateType(const std::string& type_name);
};

} // namespace mtlx
} // namespace lightusd
