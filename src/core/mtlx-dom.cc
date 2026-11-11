// SPDX-License-Identifier: Apache 2.0

#include "core/native-io.hh"
#include "core/mtlx-dom.hh"
#include "core/mtlx-resolve.hh"
#include "core/security-policy.hh"
#include "resolver/asset-resolver.hh"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>

namespace lightusd {
namespace mtlx {

namespace {

XMLNodePtr MtlxElemToXMLNode(const MtlxElem &elem) {
  auto node = std::make_shared<XMLNode>(elem.tag);
  for (const auto &attr : elem.attrs) node->SetAttribute(attr.first, attr.second);
  for (const MtlxElem &child : elem.children)
    node->AddChild(MtlxElemToXMLNode(child));
  return node;
}

MtlxElem XMLNodeToMtlxElem(XMLNodePtr node) {
  MtlxElem elem;
  if (!node) return elem;
  elem.tag = node->GetName();
  for (const auto &attr : node->GetAttributes())
    elem.attrs.emplace_back(attr.first, attr.second);
  for (const XMLNodePtr &child : node->GetChildren())
    elem.children.push_back(XMLNodeToMtlxElem(child));
  return elem;
}

void ApplyMtlxNodeDefDefaults(MtlxElem *elem, const MtlxLibrary &library,
                              const MtlxScope &outer) {
  if (!elem) return;
  if (IsMtlxNodeElem(*elem)) {
    if (const MtlxNodeDefInfo *def = library.ResolveReference(*elem, outer)) {
      for (const MtlxPort &port : library.ActiveInputs(*def)) {
        if (!port.has_value) continue;
        MtlxElem *input = nullptr;
        for (MtlxElem &child : elem->children) {
          if (child.tag == "input" && child.Name() == port.name) {
            input = &child;
            break;
          }
        }
        if (!input) {
          elem->children.emplace_back();
          input = &elem->children.back();
          input->tag = "input";
          input->SetAttr("name", port.name);
          input->SetAttr("type", port.type);
          input->SetAttr("value", port.value);
          continue;
        }
        if (input->Attr("value") || input->Attr("nodename") ||
            input->Attr("nodegraph") || input->Attr("interfacename")) {
          continue;
        }
        if (input->Attr("type") == nullptr) input->SetAttr("type", port.type);
        input->SetAttr("value", port.value);
      }
    }
  }
  MtlxScope child_scope;
  child_scope.reserve(outer.size() + 1);
  child_scope.push_back(elem);
  child_scope.insert(child_scope.end(), outer.begin(), outer.end());
  for (MtlxElem &child : elem->children)
    ApplyMtlxNodeDefDefaults(&child, library, child_scope);
}

void ApplyMtlxNodeDefDefaults(MtlxElem *root) {
  if (!root) return;
  MtlxLibrary library;
  library.AddFrom(*root);
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children)
    ApplyMtlxNodeDefDefaults(&child, library, outer);
}

std::string MtlxIncludeAnchor(const std::string &root_path,
                             const std::string &including_href) {
  if (including_href.empty()) return root_path;
  const size_t slash = root_path.find_last_of("/\\");
  if (slash == std::string::npos) return including_href;
  return root_path.substr(0, slash + 1) + including_href;
}

}  // namespace

// Helper function to parse vector values
static std::vector<float> ParseFloatVector(const std::string& str) {
  std::vector<float> result;
  std::stringstream ss(str);
  std::string token;

  while (std::getline(ss, token, ',')) {
    // Trim whitespace
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);

    if (!token.empty()) {
      char* endptr;
      float val = std::strtof(token.c_str(), &endptr);
      if (*endptr == '\0') {
        result.push_back(val);
      }
    }
  }

  return result;
}

static std::vector<int> ParseIntVector(const std::string& str) {
  std::vector<int> result;
  std::stringstream ss(str);
  std::string token;

  while (std::getline(ss, token, ',')) {
    // Trim whitespace
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);

    if (!token.empty()) {
      long val = 0;
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), val, 10);
      if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
        result.push_back(static_cast<int>(val));
      }
    }
  }

  return result;
}

// MtlxElement implementation

bool MtlxElement::ParseFromXML(XMLNodePtr xml_node) {
  if (!xml_node) return false;

  name_ = xml_node->GetAttribute("name");
  type_ = xml_node->GetAttribute("type");
  nodedef_ = xml_node->GetAttribute("nodedef");

  // Store all other attributes
  for (const auto& attr : xml_node->GetAttributes()) {
    if (attr.first != "name" && attr.first != "type" && attr.first != "nodedef") {
      extra_attributes_[attr.first] = attr.second;
    }
  }

  return true;
}

// MtlxInput implementation

bool MtlxInput::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) {
    return false;
  }

  nodename_ = xml_node->GetAttribute("nodename");
  nodegraph_ = xml_node->GetAttribute("nodegraph");
  output_ = xml_node->GetAttribute("output");
  interfacename_ = xml_node->GetAttribute("interfacename");
  channels_ = xml_node->GetAttribute("channels");

  // Parse value attribute
  std::string value_str = xml_node->GetAttribute("value");
  if (!value_str.empty() && !type_.empty()) {
    // Parse based on type
    if (type_ == "float") {
      char* endptr;
      float val = std::strtof(value_str.c_str(), &endptr);
      if (*endptr == '\0') {
        value_ = MtlxValue(val);
      }
    } else if (type_ == "integer") {
      long val = 0;
      const auto parsed = std::from_chars(value_str.data(), value_str.data() + value_str.size(), val, 10);
      if (parsed.ec == std::errc{} && parsed.ptr == value_str.data() + value_str.size()) {
        value_ = MtlxValue(static_cast<int>(val));
      }
    } else if (type_ == "boolean") {
      value_ = MtlxValue(value_str == "true" || value_str == "1");
    } else if (type_ == "string" || type_ == "filename") {
      value_ = MtlxValue(value_str);
    } else if (type_ == "color3" || type_ == "vector3") {
      value_ = MtlxValue(ParseFloatVector(value_str));
    } else if (type_ == "color4" || type_ == "vector4") {
      value_ = MtlxValue(ParseFloatVector(value_str));
    } else if (type_ == "vector2") {
      value_ = MtlxValue(ParseFloatVector(value_str));
    } else if (type_ == "integerarray") {
      value_ = MtlxValue(ParseIntVector(value_str));
    } else if (type_ == "floatarray") {
      value_ = MtlxValue(ParseFloatVector(value_str));
    }
  }

  return true;
}

// MtlxOutput implementation

bool MtlxOutput::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) {
    return false;
  }

  nodename_ = xml_node->GetAttribute("nodename");
  output_ = xml_node->GetAttribute("output");

  return true;
}

// MtlxNode implementation

bool MtlxNode::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) {
    return false;
  }

  category_ = xml_node->GetAttribute("category");
  if (category_.empty()) {
    // If no category, use the node name as category (for typed nodes)
    category_ = xml_node->GetName();
  }

  // Parse input children
  for (const auto& child : xml_node->GetChildren()) {
    if (child->GetName() == "input") {
      auto input = std::make_shared<MtlxInput>();
      if (input->ParseFromXML(child)) {
        inputs_.push_back(input);
      }
    }
  }

  return true;
}

MtlxInputPtr MtlxNode::GetInput(const std::string& name) const {
  for (const auto& input : inputs_) {
    if (input && input->GetName() == name) {
      return input;
    }
  }
  return nullptr;
}

// MtlxNodeGraph implementation

bool MtlxNodeGraph::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) {
    return false;
  }

  // Parse children
  for (const auto& child : xml_node->GetChildren()) {
    const std::string& child_name = child->GetName();

    if (child_name == "input") {
      auto input = std::make_shared<MtlxInput>();
      if (input->ParseFromXML(child)) {
        inputs_.push_back(input);
      }
    } else if (child_name == "output") {
      auto output = std::make_shared<MtlxOutput>();
      if (output->ParseFromXML(child)) {
        outputs_.push_back(output);
      }
    } else {
      // Any other child element is treated as a node
      // (image, tiledimage, place2d, constant, multiply, add, etc.)
      auto node = std::make_shared<MtlxNode>();
      if (node->ParseFromXML(child)) {
        nodes_.push_back(node);
      }
    }
  }

  return true;
}

MtlxNodePtr MtlxNodeGraph::GetNode(const std::string& name) const {
  for (const auto& node : nodes_) {
    if (node && node->GetName() == name) {
      return node;
    }
  }
  return nullptr;
}

// MtlxMaterial implementation

bool MtlxMaterial::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) {
    return false;
  }

  // Parse shader references
  for (const auto& child : xml_node->GetChildren()) {
    if (child->GetName() == "shaderref") {
      std::string shader_name = child->GetAttribute("name");
      std::string shader_node = child->GetAttribute("node");

      if (shader_name == "surfaceshader" || shader_name == "sr") {
        surface_shader_ = shader_node;
      } else if (shader_name == "displacementshader" || shader_name == "dr") {
        displacement_shader_ = shader_node;
      } else if (shader_name == "volumeshader" || shader_name == "vr") {
        volume_shader_ = shader_node;
      }
    } else if (child->GetName() == "input") {
      std::string input_name = child->GetAttribute("name");
      std::string shader_node = child->GetAttribute("nodename");
      if (shader_node.empty()) {
        shader_node = child->GetAttribute("node");
      }
      const std::string nodegraph = child->GetAttribute("nodegraph");
      const std::string output = child->GetAttribute("output");

      if (input_name == "surfaceshader" || input_name == "sr") {
        surface_shader_ = shader_node;
        surface_nodegraph_ = nodegraph;
        surface_output_ = output.empty() ? "out" : output;
      } else if (input_name == "displacementshader" || input_name == "dr") {
        displacement_shader_ = shader_node;
      } else if (input_name == "volumeshader" || input_name == "vr") {
        volume_shader_ = shader_node;
      }
    }
  }

  return true;
}

// MtlxDocument implementation

bool MtlxDocument::ParseFromXML(const std::string& xml_string) {
  MaterialXParser parser;

  if (!parser.Parse(xml_string)) {
    error_ = parser.GetError();
    return false;
  }

  warning_ = parser.GetWarning();
  MtlxElem root = XMLNodeToMtlxElem(parser.GetDocument().GetRoot());
  ApplyMtlxNodeDefDefaults(&root);
  return ParseRoot(MtlxElemToXMLNode(root));
}

bool MtlxDocument::ParseFromXML(const std::string &xml_string,
                                const std::string &anchor_path) {
  MtlxElem root;
  if (!ParseMtlxXml(xml_string, &root, &error_)) {
    error_ = "Failed to parse MaterialX XML: " + error_;
    return false;
  }

  AssetResolver resolver;
  std::string include_error;
  const MtlxAnchoredIncludeLoader loader =
      [&resolver, &anchor_path](const std::string &href,
                               const std::string &including_href,
                               std::string *xml) {
        std::string normalized;
        if (!security_policy::ValidateAndNormalizeAssetPath(href, &normalized))
          return false;
        const std::string source_path =
            MtlxIncludeAnchor(anchor_path, including_href);
        const ResolvedAsset asset = resolver.Resolve(normalized, source_path,
                                                     false);
        if (!asset.exists) return false;
        std::vector<uint8_t> bytes;
        if (!resolver.ReadAsset(asset, &bytes) ||
            bytes.size() > 256u * 1024u * 1024u)
          return false;
        xml->assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return true;
      };
  if (!ExpandMtlxIncludes(&root, loader, &include_error)) {
    error_ = "Failed to resolve MaterialX XInclude: " + include_error;
    return false;
  }

  ApplyMtlxNodeDefDefaults(&root);
  return ParseRoot(MtlxElemToXMLNode(root));
}

bool MtlxDocument::ParseRoot(XMLNodePtr root) {
  if (!root || root->GetName() != "materialx") {
    error_ = "Invalid MaterialX document";
    return false;
  }

  // Parse document attributes
  version_ = root->GetAttribute("version");
  colorspace_ = root->GetAttribute("colorspace");
  namespace_ = root->GetAttribute("namespace");

  // Parse all children
  for (const auto& child : root->GetChildren()) {
    if (!ParseElement(child)) {
      return false;
    }
  }

  return true;
}

bool MtlxDocument::ParseFromFile(const std::string& filename) {
  if (!::lightusd::NativeIOAvailable(&error_)) return false;

  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    error_ = "Failed to open file: " + filename;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();

  return ParseFromXML(buffer.str(), filename);
}

bool MtlxDocument::ParseElement(XMLNodePtr xml_node) {
  if (!xml_node) return false;

  const std::string& element_name = xml_node->GetName();

  if (element_name == "nodegraph") {
    auto nodegraph = std::make_shared<MtlxNodeGraph>();
    if (nodegraph->ParseFromXML(xml_node)) {
      nodegraphs_.push_back(nodegraph);
    }
  } else if (element_name == "surfacematerial" || element_name == "volumematerial") {
    auto material = std::make_shared<MtlxMaterial>();
    if (material->ParseFromXML(xml_node)) {
      materials_.push_back(material);
    }
  } else if (element_name != "look" && element_name != "typedef" &&
             element_name != "geominfo" && element_name != "collection") {
    // Treat as a node (standard_surface, UsdPreviewSurface, open_pbr_surface,
    // image, tiledimage, place2d, constant, etc.)
    auto node = std::make_shared<MtlxNode>();
    if (node->ParseFromXML(xml_node)) {
      nodes_.push_back(node);
    }
  }

  return true;
}

MtlxValue MtlxDocument::ParseValue(const std::string& type, const std::string& value_str) {
  if (type == "float") {
    char* endptr;
    float val = std::strtof(value_str.c_str(), &endptr);
    if (*endptr == '\0') {
      return MtlxValue(val);
    }
  } else if (type == "integer") {
    long val = 0;
    const auto parsed = std::from_chars(value_str.data(), value_str.data() + value_str.size(), val, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == value_str.data() + value_str.size()) {
      return MtlxValue(static_cast<int>(val));
    }
  } else if (type == "boolean") {
    return MtlxValue(value_str == "true" || value_str == "1");
  } else if (type == "string" || type == "filename") {
    return MtlxValue(value_str);
  } else if (type == "color3" || type == "vector3" || type == "color4" ||
             type == "vector4" || type == "vector2" || type == "floatarray") {
    return MtlxValue(ParseFloatVector(value_str));
  } else if (type == "integerarray") {
    return MtlxValue(ParseIntVector(value_str));
  }

  // Default to string
  return MtlxValue(value_str);
}

MtlxNodePtr MtlxDocument::FindNode(const std::string& name) const {
  for (const auto& node : nodes_) {
    if (node && node->GetName() == name) {
      return node;
    }
  }

  // Also search within nodegraphs
  for (const auto& nodegraph : nodegraphs_) {
    if (auto node = nodegraph->GetNode(name)) {
      return node;
    }
  }

  return nullptr;
}

MtlxNodeGraphPtr MtlxDocument::FindNodeGraph(const std::string& name) const {
  for (const auto& nodegraph : nodegraphs_) {
    if (nodegraph && nodegraph->GetName() == name) {
      return nodegraph;
    }
  }
  return nullptr;
}

MtlxMaterialPtr MtlxDocument::FindMaterial(const std::string& name) const {
  for (const auto& material : materials_) {
    if (material && material->GetName() == name) {
      return material;
    }
  }
  return nullptr;
}

} // namespace mtlx
} // namespace lightusd
