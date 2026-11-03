// SPDX-License-Identifier: Apache 2.0
// MaterialX Document Object Model

#pragma once

#include "mtlx-xml-parser.hh"
#include <vector>
#include <map>

namespace lightusd {
namespace mtlx {

// Forward declarations
class MtlxElement;
class MtlxNode;
class MtlxInput;
class MtlxOutput;
class MtlxNodeGraph;
class MtlxMaterial;
class MtlxDocument;

using MtlxElementPtr = std::shared_ptr<MtlxElement>;
using MtlxNodePtr = std::shared_ptr<MtlxNode>;
using MtlxInputPtr = std::shared_ptr<MtlxInput>;
using MtlxOutputPtr = std::shared_ptr<MtlxOutput>;
using MtlxNodeGraphPtr = std::shared_ptr<MtlxNodeGraph>;
using MtlxMaterialPtr = std::shared_ptr<MtlxMaterial>;
using MtlxDocumentPtr = std::shared_ptr<MtlxDocument>;

// MaterialX value types - using tagged union for C++14 compatibility
struct MtlxValue {
  enum Type {
    TYPE_NONE,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_FLOAT_VECTOR,
    TYPE_INT_VECTOR,
    TYPE_STRING_VECTOR
  };

  Type type = TYPE_NONE;

  // Value storage
  bool bool_val = false;
  int int_val = 0;
  float float_val = 0.0f;
  std::string string_val;
  std::vector<float> float_vec;
  std::vector<int> int_vec;
  std::vector<std::string> string_vec;

  MtlxValue() = default;
  explicit MtlxValue(bool v) : type(TYPE_BOOL), bool_val(v) {}
  explicit MtlxValue(int v) : type(TYPE_INT), int_val(v) {}
  explicit MtlxValue(float v) : type(TYPE_FLOAT), float_val(v) {}
  explicit MtlxValue(const std::string& v) : type(TYPE_STRING), string_val(v) {}
  explicit MtlxValue(const std::vector<float>& v) : type(TYPE_FLOAT_VECTOR), float_vec(v) {}
  explicit MtlxValue(const std::vector<int>& v) : type(TYPE_INT_VECTOR), int_vec(v) {}
  explicit MtlxValue(const std::vector<std::string>& v) : type(TYPE_STRING_VECTOR), string_vec(v) {}
};

// Base class for all MaterialX elements
class MtlxElement {
public:
  MtlxElement() = default;
  virtual ~MtlxElement() = default;

  // Common attributes
  const std::string& GetName() const { return name_; }
  void SetName(const std::string& name) { name_ = name; }

  const std::string& GetType() const { return type_; }
  void SetType(const std::string& type) { type_ = type; }

  const std::string& GetNodeDef() const { return nodedef_; }
  void SetNodeDef(const std::string& nodedef) { nodedef_ = nodedef; }

  // Value access
  const MtlxValue& GetValue() const { return value_; }
  void SetValue(const MtlxValue& value) { value_ = value; }

  // Get value as specific type
  bool GetValueAsBool(bool& out) const {
    if (value_.type == MtlxValue::TYPE_BOOL) {
      out = value_.bool_val;
      return true;
    }
    return false;
  }

  bool GetValueAsInt(int& out) const {
    if (value_.type == MtlxValue::TYPE_INT) {
      out = value_.int_val;
      return true;
    }
    return false;
  }

  bool GetValueAsFloat(float& out) const {
    if (value_.type == MtlxValue::TYPE_FLOAT) {
      out = value_.float_val;
      return true;
    }
    return false;
  }

  bool GetValueAsString(std::string& out) const {
    if (value_.type == MtlxValue::TYPE_STRING) {
      out = value_.string_val;
      return true;
    }
    return false;
  }

  bool GetValueAsFloatVector(std::vector<float>& out) const {
    if (value_.type == MtlxValue::TYPE_FLOAT_VECTOR) {
      out = value_.float_vec;
      return true;
    }
    return false;
  }

  // ColorSpace access
  std::string GetColorSpace() const {
    auto it = extra_attributes_.find("colorspace");
    if (it != extra_attributes_.end()) {
      return it->second;
    }
    return "";
  }

  // Parse from XML node
  virtual bool ParseFromXML(XMLNodePtr xml_node);

  // Get element type name
  virtual std::string GetElementType() const { return "element"; }

protected:
  std::string name_;
  std::string type_;
  std::string nodedef_;
  MtlxValue value_;
  std::map<std::string, std::string> extra_attributes_;
};

// Input element
class MtlxInput : public MtlxElement {
public:
  MtlxInput() = default;

  // Input-specific attributes
  const std::string& GetNodeName() const { return nodename_; }
  void SetNodeName(const std::string& nodename) { nodename_ = nodename; }

  const std::string& GetNodeGraph() const { return nodegraph_; }
  void SetNodeGraph(const std::string& nodegraph) { nodegraph_ = nodegraph; }

  const std::string& GetOutput() const { return output_; }
  void SetOutput(const std::string& output) { output_ = output; }

  const std::string& GetInterfaceName() const { return interfacename_; }
  void SetInterfaceName(const std::string& name) { interfacename_ = name; }

  const std::string& GetChannels() const { return channels_; }
  void SetChannels(const std::string& channels) { channels_ = channels; }

  bool ParseFromXML(XMLNodePtr xml_node) override;
  std::string GetElementType() const override { return "input"; }

private:
  std::string nodename_;
  std::string nodegraph_;
  std::string output_;
  std::string interfacename_;
  std::string channels_;
};

// Output element
class MtlxOutput : public MtlxElement {
public:
  MtlxOutput() = default;

  // Output-specific attributes
  const std::string& GetNodeName() const { return nodename_; }
  void SetNodeName(const std::string& nodename) { nodename_ = nodename; }

  const std::string& GetOutput() const { return output_; }
  void SetOutput(const std::string& output) { output_ = output; }

  bool ParseFromXML(XMLNodePtr xml_node) override;
  std::string GetElementType() const override { return "output"; }

private:
  std::string nodename_;
  std::string output_;
};

// Node element
class MtlxNode : public MtlxElement {
public:
  MtlxNode() = default;

  // Node-specific attributes
  const std::string& GetCategory() const { return category_; }
  void SetCategory(const std::string& category) { category_ = category; }

  // Inputs
  void AddInput(MtlxInputPtr input) { inputs_.push_back(input); }
  const std::vector<MtlxInputPtr>& GetInputs() const { return inputs_; }
  MtlxInputPtr GetInput(const std::string& name) const;

  bool ParseFromXML(XMLNodePtr xml_node) override;
  std::string GetElementType() const override { return "node"; }

private:
  std::string category_;
  std::vector<MtlxInputPtr> inputs_;
};

// NodeGraph element
class MtlxNodeGraph : public MtlxElement {
public:
  MtlxNodeGraph() = default;

  // Nodes
  void AddNode(MtlxNodePtr node) { nodes_.push_back(node); }
  const std::vector<MtlxNodePtr>& GetNodes() const { return nodes_; }
  MtlxNodePtr GetNode(const std::string& name) const;

  // Inputs
  void AddInput(MtlxInputPtr input) { inputs_.push_back(input); }
  const std::vector<MtlxInputPtr>& GetInputs() const { return inputs_; }

  // Outputs
  void AddOutput(MtlxOutputPtr output) { outputs_.push_back(output); }
  const std::vector<MtlxOutputPtr>& GetOutputs() const { return outputs_; }

  bool ParseFromXML(XMLNodePtr xml_node) override;
  std::string GetElementType() const override { return "nodegraph"; }

private:
  std::vector<MtlxNodePtr> nodes_;
  std::vector<MtlxInputPtr> inputs_;
  std::vector<MtlxOutputPtr> outputs_;
};

// Material element (surfacematerial, volumematerial)
class MtlxMaterial : public MtlxElement {
public:
  MtlxMaterial() = default;

  // Shader references
  const std::string& GetSurfaceShader() const { return surface_shader_; }
  void SetSurfaceShader(const std::string& shader) { surface_shader_ = shader; }
  const std::string& GetSurfaceNodeGraph() const { return surface_nodegraph_; }
  const std::string& GetSurfaceOutput() const { return surface_output_; }

  const std::string& GetDisplacementShader() const { return displacement_shader_; }
  void SetDisplacementShader(const std::string& shader) { displacement_shader_ = shader; }

  const std::string& GetVolumeShader() const { return volume_shader_; }
  void SetVolumeShader(const std::string& shader) { volume_shader_ = shader; }

  bool ParseFromXML(XMLNodePtr xml_node) override;
  std::string GetElementType() const override { return "material"; }

private:
  std::string surface_shader_;
  std::string surface_nodegraph_;
  std::string surface_output_;
  std::string displacement_shader_;
  std::string volume_shader_;
};

// MaterialX Document
class MtlxDocument {
public:
  MtlxDocument() = default;

  // Parse from XML
  bool ParseFromXML(const std::string& xml_string);
  bool ParseFromFile(const std::string& filename);

  // Document properties
  const std::string& GetVersion() const { return version_; }
  const std::string& GetColorSpace() const { return colorspace_; }
  const std::string& GetNamespace() const { return namespace_; }

  // Access elements
  const std::vector<MtlxNodePtr>& GetNodes() const { return nodes_; }
  const std::vector<MtlxNodeGraphPtr>& GetNodeGraphs() const { return nodegraphs_; }
  const std::vector<MtlxMaterialPtr>& GetMaterials() const { return materials_; }

  // Find elements by name
  MtlxNodePtr FindNode(const std::string& name) const;
  MtlxNodeGraphPtr FindNodeGraph(const std::string& name) const;
  MtlxMaterialPtr FindMaterial(const std::string& name) const;

  // Get errors
  const std::string& GetError() const { return error_; }
  const std::string& GetWarning() const { return warning_; }

private:
  bool ParseElement(XMLNodePtr xml_node);
  MtlxValue ParseValue(const std::string& type, const std::string& value);

  std::string version_;
  std::string colorspace_;
  std::string namespace_;

  std::vector<MtlxNodePtr> nodes_;
  std::vector<MtlxNodeGraphPtr> nodegraphs_;
  std::vector<MtlxMaterialPtr> materials_;

  std::string error_;
  std::string warning_;
};

} // namespace mtlx
} // namespace lightusd
