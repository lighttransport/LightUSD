// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Tydra - MaterialX Support Implementation

#include "core/native-io.hh"
#include "materialx.hh"
#include "render-converter.hh"
#include "core/mtlx-dom.hh"
#include "schema/usd-shade.hh"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

namespace lightusd {
namespace tydra {

MtlxConverter::MtlxConverter() = default;
MtlxConverter::~MtlxConverter() = default;

namespace {

// Helper to set ShaderParam from float values
void SetShaderParam(ShaderParam& param, float v) {
  param.texture_id = -1;
  param.value = {v, 0, 0, 1};
}

void SetShaderParam(ShaderParam& param, float r, float g, float b) {
  param.texture_id = -1;
  param.value = {r, g, b, 1};
}

bool GetStringLikeProperty(const lightusd::UsdPrim& prim,
                           const std::string& name,
                           std::string* out) {
  if (!out) return false;

  const lightusd::Value* value = prim.GetPropertyValue(name);
  if (!value) return false;

  if (auto s = value->as_string()) {
    *out = *s;
    return true;
  }
  if (auto s = value->as_token()) {
    *out = *s;
    return true;
  }
  if (auto s = value->as_asset_path()) {
    *out = *s;
    return true;
  }

  return false;
}

bool HasPrefix(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string GetMtlxSurfaceShaderPath(const lightusd::Stage& stage,
                                     const lightusd::UsdPrim& material) {
  std::string path = lightusd::GetSurfaceShader(stage, material);
  if (!path.empty()) return path;

  lightusd::AttributeEval eval(&stage);
  if (eval.HasConnection(material, "outputs:surface")) {
    path = eval.GetConnectionPath(material, "outputs:surface");
    if (!path.empty()) return path;
  }

  const std::vector<lightusd::Path>* targets =
      material.GetRelationship("outputs:mtlx:surface");
  if (targets && !targets->empty()) {
    return (*targets)[0].str();
  }

  if (eval.HasConnection(material, "outputs:mtlx:surface")) {
    path = eval.GetConnectionPath(material, "outputs:mtlx:surface");
    if (!path.empty()) return path;
  }

  return "";
}

std::string StripPropertyPath(const std::string& path) {
  size_t dot_pos = path.find(".outputs:");
  if (dot_pos == std::string::npos) {
    dot_pos = path.find(".inputs:");
  }
  if (dot_pos == std::string::npos) {
    dot_pos = path.rfind('.');
  }
  if (dot_pos == std::string::npos) return path;
  return path.substr(0, dot_pos);
}

struct MtlxEvalValue {
  bool valid = false;
  bool is_texture = false;
  float scalar = 0.0f;
  std::vector<float> vector;
  MtlxTextureData texture;
};

MtlxNodeInfo BuildNodeInfo(const mtlx::MtlxNodePtr& node) {
  MtlxNodeInfo info;
  if (!node) return info;

  info.name = node->GetName();
  info.category = node->GetCategory();
  info.type = node->GetType();
  info.colorspace = node->GetColorSpace();

  for (const auto& input : node->GetInputs()) {
    const std::string input_name = input->GetName();
    const auto& value = input->GetValue();
    if (value.type == mtlx::MtlxValue::TYPE_FLOAT) {
      info.input_floats[input_name] = value.float_val;
    } else if (value.type == mtlx::MtlxValue::TYPE_FLOAT_VECTOR) {
      info.input_vectors[input_name] = value.float_vec;
    } else if (value.type == mtlx::MtlxValue::TYPE_STRING) {
      info.input_strings[input_name] = value.string_val;
    }

    if (!input->GetNodeName().empty()) {
      std::string conn = input->GetNodeName();
      if (!input->GetOutput().empty()) {
        conn += "." + input->GetOutput();
      }
      info.input_connections[input_name] = conn;
    } else if (!input->GetNodeGraph().empty()) {
      std::string conn = input->GetNodeGraph();
      if (!input->GetOutput().empty()) {
        conn += "." + input->GetOutput();
      }
      info.input_connections[input_name] = conn;
    }
  }

  return info;
}

bool TextureDataFromNodeInfo(const MtlxNodeInfo& info, MtlxTextureData* out) {
  if (!out) return false;

  auto it = info.input_strings.find("file");
  if (it != info.input_strings.end()) out->file = it->second;

  it = info.input_strings.find("uaddressmode");
  if (it != info.input_strings.end()) out->uaddressmode = it->second;

  it = info.input_strings.find("vaddressmode");
  if (it != info.input_strings.end()) out->vaddressmode = it->second;

  it = info.input_strings.find("filtertype");
  if (it != info.input_strings.end()) out->filtertype = it->second;

  if (!info.colorspace.empty()) out->colorspace = info.colorspace;

  auto vit = info.input_vectors.find("default");
  if (vit == info.input_vectors.end()) vit = info.input_vectors.find("defaultvalue");
  if (vit != info.input_vectors.end()) {
    const size_t n = std::min<size_t>(vit->second.size(), 4);
    for (size_t i = 0; i < n; ++i) out->default_value[i] = vit->second[i];
  } else {
    auto fit = info.input_floats.find("default");
    if (fit == info.input_floats.end()) fit = info.input_floats.find("defaultvalue");
    if (fit != info.input_floats.end()) {
      out->default_value[0] = fit->second;
      out->default_value[1] = fit->second;
      out->default_value[2] = fit->second;
      out->default_value[3] = 1.0f;
    }
  }

  return !out->file.empty();
}

MtlxEvalValue ValueFromInput(const mtlx::MtlxInputPtr& input) {
  MtlxEvalValue out;
  if (!input) return out;

  const auto& value = input->GetValue();
  if (value.type == mtlx::MtlxValue::TYPE_FLOAT) {
    out.valid = true;
    out.scalar = value.float_val;
    out.vector = {value.float_val, value.float_val, value.float_val};
  } else if (value.type == mtlx::MtlxValue::TYPE_FLOAT_VECTOR) {
    out.valid = true;
    out.vector = value.float_vec;
    out.scalar = value.float_vec.empty() ? 0.0f : value.float_vec[0];
  }
  return out;
}

float ComponentOr(const MtlxEvalValue& v, size_t idx, float fallback) {
  if (!v.valid) return fallback;
  if (idx < v.vector.size()) return v.vector[idx];
  return v.scalar;
}

MtlxEvalValue BinaryOp(const MtlxEvalValue& a,
                       const MtlxEvalValue& b,
                       const std::string& op) {
  MtlxEvalValue out;
  if (!a.valid || !b.valid || a.is_texture || b.is_texture) return out;

  out.valid = true;
  const size_t n = std::max<size_t>(3, std::max(a.vector.size(), b.vector.size()));
  out.vector.resize(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    const float av = ComponentOr(a, i, 0.0f);
    const float bv = ComponentOr(b, i, 0.0f);
    if (op == "add") out.vector[i] = av + bv;
    else if (op == "subtract") out.vector[i] = av - bv;
    else if (op == "divide") out.vector[i] = (std::abs(bv) > 1.0e-8f) ? av / bv : 0.0f;
    else out.vector[i] = av * bv;
  }
  out.scalar = out.vector.empty() ? 0.0f : out.vector[0];
  return out;
}

mtlx::MtlxNodePtr ResolveNodeGraphOutput(const mtlx::MtlxDocument& doc,
                                         const std::string& nodegraph_name,
                                         const std::string& output_name,
                                         std::string* source_output) {
  mtlx::MtlxNodeGraphPtr graph = doc.FindNodeGraph(nodegraph_name);
  if (!graph) return nullptr;

  for (const auto& output : graph->GetOutputs()) {
    if (!output) continue;
    if (!output_name.empty() && output->GetName() != output_name) continue;
    if (source_output) *source_output = output->GetOutput();
    return graph->GetNode(output->GetNodeName());
  }

  return nullptr;
}

bool SplitMtlxConnection(const std::string& conn,
                         std::string* node_or_graph,
                         std::string* output) {
  if (!node_or_graph || !output) return false;
  size_t dot = conn.find('.');
  if (dot == std::string::npos) {
    *node_or_graph = conn;
    output->clear();
  } else {
    *node_or_graph = conn.substr(0, dot);
    *output = conn.substr(dot + 1);
  }
  return !node_or_graph->empty();
}

MtlxEvalValue EvalMtlxNode(const mtlx::MtlxDocument& doc,
                           const mtlx::MtlxNodePtr& node,
                           const std::string& output_name,
                           std::set<std::string>* visiting,
                           int depth);

MtlxEvalValue EvalMtlxConnection(const mtlx::MtlxDocument& doc,
                                 const std::string& conn,
                                 std::set<std::string>* visiting,
                                 int depth) {
  MtlxEvalValue out;
  if (depth > 16) return out;

  std::string node_name;
  std::string output_name;
  if (!SplitMtlxConnection(conn, &node_name, &output_name)) return out;

  mtlx::MtlxNodePtr node = doc.FindNode(node_name);
  if (!node) {
    std::string source_output;
    node = ResolveNodeGraphOutput(doc, node_name, output_name, &source_output);
    if (!source_output.empty()) output_name = source_output;
  }
  return EvalMtlxNode(doc, node, output_name, visiting, depth + 1);
}

MtlxEvalValue EvalInputOrConnection(const mtlx::MtlxDocument& doc,
                                    const mtlx::MtlxInputPtr& input,
                                    std::set<std::string>* visiting,
                                    int depth) {
  if (!input) return MtlxEvalValue{};
  if (!input->GetNodeName().empty()) {
    std::string conn = input->GetNodeName();
    if (!input->GetOutput().empty()) conn += "." + input->GetOutput();
    return EvalMtlxConnection(doc, conn, visiting, depth + 1);
  }
  if (!input->GetNodeGraph().empty()) {
    std::string conn = input->GetNodeGraph();
    if (!input->GetOutput().empty()) conn += "." + input->GetOutput();
    return EvalMtlxConnection(doc, conn, visiting, depth + 1);
  }
  return ValueFromInput(input);
}

MtlxEvalValue EvalMtlxNode(const mtlx::MtlxDocument& doc,
                           const mtlx::MtlxNodePtr& node,
                           const std::string& output_name,
                           std::set<std::string>* visiting,
                           int depth) {
  MtlxEvalValue out;
  if (!node || !visiting || depth > 16) return out;

  const std::string key = node->GetName() + "." + output_name;
  if (visiting->count(key)) return out;
  visiting->insert(key);

  const std::string category = node->GetCategory();
  if (category == "image" || category == "tiledimage") {
    MtlxNodeInfo info = BuildNodeInfo(node);
    out.valid = true;
    out.is_texture = true;
    TextureDataFromNodeInfo(info, &out.texture);
  } else if (category == "constant") {
    out = EvalInputOrConnection(doc, node->GetInput("value"), visiting, depth + 1);
  } else if (category == "add" || category == "subtract" ||
             category == "multiply" || category == "divide") {
    MtlxEvalValue a = EvalInputOrConnection(doc, node->GetInput("in1"), visiting, depth + 1);
    MtlxEvalValue b = EvalInputOrConnection(doc, node->GetInput("in2"), visiting, depth + 1);
    out = BinaryOp(a, b, category);
  } else if (category == "mix") {
    MtlxEvalValue a = EvalInputOrConnection(doc, node->GetInput("fg"), visiting, depth + 1);
    MtlxEvalValue b = EvalInputOrConnection(doc, node->GetInput("bg"), visiting, depth + 1);
    MtlxEvalValue mix = EvalInputOrConnection(doc, node->GetInput("mix"), visiting, depth + 1);
    if (a.valid && b.valid && mix.valid && !a.is_texture && !b.is_texture) {
      out.valid = true;
      out.vector.resize(3, 0.0f);
      const float t = ComponentOr(mix, 0, 0.5f);
      for (size_t i = 0; i < 3; ++i) {
        out.vector[i] = ComponentOr(b, i, 0.0f) * (1.0f - t) +
                        ComponentOr(a, i, 0.0f) * t;
      }
      out.scalar = out.vector[0];
    }
  } else {
    mtlx::MtlxInputPtr value = node->GetInput("value");
    if (!value) value = node->GetInput("out");
    if (!value) value = node->GetInput("in");
    out = EvalInputOrConnection(doc, value, visiting, depth + 1);
  }

  visiting->erase(key);
  return out;
}

bool ApplyEvalToNodeInfoInput(const std::string& input_name,
                              const MtlxEvalValue& value,
                              MtlxNodeInfo* node_info,
                              std::map<std::string, MtlxTextureData>* textures) {
  if (!node_info || !value.valid) return false;
  if (value.is_texture) {
    node_info->input_connections[input_name] = value.texture.file;
    if (textures) (*textures)[input_name] = value.texture;
    return true;
  }

  if (!value.vector.empty()) {
    node_info->input_vectors[input_name] = value.vector;
  } else {
    node_info->input_floats[input_name] = value.scalar;
  }
  return true;
}

}  // namespace

bool MtlxConverter::ConvertToRenderMaterial(const std::string& mtlx_content,
                                             const std::string& material_name,
                                             RenderMaterial* out,
                                             const std::string& anchor_path) {
  if (!out) {
    error_ = "Output material is null";
    return false;
  }

  // Parse MaterialX document
  mtlx::MtlxDocument doc;
  const bool parsed = anchor_path.empty()
                          ? doc.ParseFromXML(mtlx_content)
                          : doc.ParseFromXML(mtlx_content, anchor_path);
  if (!parsed) {
    error_ = "Failed to parse MaterialX: " + doc.GetError();
    return false;
  }

  // Find the target material
  mtlx::MtlxMaterialPtr mat;
  if (material_name.empty()) {
    // Use first material
    const auto& materials = doc.GetMaterials();
    if (materials.empty()) {
      error_ = "No materials found in MaterialX document";
      return false;
    }
    mat = materials[0];
  } else {
    mat = doc.FindMaterial(material_name);
    if (!mat) {
      error_ = "Material not found: " + material_name;
      return false;
    }
  }

  out->name = mat->GetName();

  // Find surface shader node
  std::string surface_shader = mat->GetSurfaceShader();
  if (surface_shader.empty() && !mat->GetSurfaceNodeGraph().empty()) {
    const auto graph = doc.FindNodeGraph(mat->GetSurfaceNodeGraph());
    if (graph) {
      const std::string outputName = mat->GetSurfaceOutput().empty()
                                         ? "out"
                                         : mat->GetSurfaceOutput();
      for (const auto& output : graph->GetOutputs()) {
        if (output && output->GetName() == outputName) {
          surface_shader = output->GetNodeName();
          break;
        }
      }
    }
  }
  if (surface_shader.empty()) {
    error_ = "Material has no surface shader";
    return false;
  }

  // Look up the shader node
  mtlx::MtlxNodePtr shader_node = doc.FindNode(surface_shader);
  if (!shader_node) {
    // Try finding in node graphs
    for (const auto& ng : doc.GetNodeGraphs()) {
      shader_node = ng->GetNode(surface_shader);
      if (shader_node) break;
    }
  }

  if (!shader_node) {
    error_ = "Surface shader node not found: " + surface_shader;
    return false;
  }

  // Get shader category
  std::string category = shader_node->GetCategory();

  // Convert based on shader category
  if (category == "standard_surface") {
    StandardSurfaceData ss_data;
    MtlxNodeInfo node_info = BuildNodeInfo(shader_node);
    std::map<std::string, MtlxTextureData> textures;

    for (const auto& input : shader_node->GetInputs()) {
      if (!input->GetNodeName().empty() || !input->GetNodeGraph().empty()) {
        std::set<std::string> visiting;
        MtlxEvalValue value = EvalInputOrConnection(doc, input, &visiting, 0);
        ApplyEvalToNodeInfoInput(input->GetName(), value, &node_info, &textures);
      }
    }

    if (!ParseStandardSurface(node_info, &ss_data)) {
      return false;
    }

    // Convert to preview surface
    MtlxPreviewSurfaceData ps_data;
    if (!ConvertStandardSurface(ss_data, &ps_data)) {
      return false;
    }

    // Populate render material
    PopulateRenderMaterial(ps_data, textures, out);

  } else if (category == "UsdPreviewSurface") {
    // Already UsdPreviewSurface - direct conversion
    MtlxPreviewSurfaceData ps_data;
    std::map<std::string, MtlxTextureData> textures;

    for (const auto& input : shader_node->GetInputs()) {
      std::string input_name = input->GetName();
      const auto& value = input->GetValue();
      MtlxEvalValue eval_value;
      if (!input->GetNodeName().empty() || !input->GetNodeGraph().empty()) {
        std::set<std::string> visiting;
        eval_value = EvalInputOrConnection(doc, input, &visiting, 0);
        if (eval_value.is_texture) {
          textures[input_name] = eval_value.texture;
        }
      }

      if (input_name == "diffuseColor") {
        if (eval_value.valid && !eval_value.is_texture) {
          ps_data.diffuse_color[0] = ComponentOr(eval_value, 0, ps_data.diffuse_color[0]);
          ps_data.diffuse_color[1] = ComponentOr(eval_value, 1, ps_data.diffuse_color[1]);
          ps_data.diffuse_color[2] = ComponentOr(eval_value, 2, ps_data.diffuse_color[2]);
        } else if (value.type == mtlx::MtlxValue::TYPE_FLOAT_VECTOR &&
                   value.float_vec.size() >= 3) {
          ps_data.diffuse_color[0] = value.float_vec[0];
          ps_data.diffuse_color[1] = value.float_vec[1];
          ps_data.diffuse_color[2] = value.float_vec[2];
        }
      } else if (input_name == "metallic") {
        if (eval_value.valid && !eval_value.is_texture) ps_data.metallic = eval_value.scalar;
        else if (value.type == mtlx::MtlxValue::TYPE_FLOAT) ps_data.metallic = value.float_val;
      } else if (input_name == "roughness") {
        if (eval_value.valid && !eval_value.is_texture) ps_data.roughness = eval_value.scalar;
        else if (value.type == mtlx::MtlxValue::TYPE_FLOAT) ps_data.roughness = value.float_val;
      } else if (input_name == "opacity") {
        if (eval_value.valid && !eval_value.is_texture) ps_data.opacity = eval_value.scalar;
        else if (value.type == mtlx::MtlxValue::TYPE_FLOAT) ps_data.opacity = value.float_val;
      } else if (input_name == "ior") {
        if (eval_value.valid && !eval_value.is_texture) ps_data.ior = eval_value.scalar;
        else if (value.type == mtlx::MtlxValue::TYPE_FLOAT) ps_data.ior = value.float_val;
      }

      // Texture connections
      if (!input->GetNodeName().empty()) {
        if (input_name == "diffuseColor") {
          ps_data.diffuse_texture = input->GetNodeName();
        } else if (input_name == "normal") {
          ps_data.normal_texture = input->GetNodeName();
        } else if (input_name == "metallic") {
          ps_data.metallic_texture = input->GetNodeName();
        } else if (input_name == "roughness") {
          ps_data.roughness_texture = input->GetNodeName();
        }
      }
    }

    PopulateRenderMaterial(ps_data, textures, out);

  } else {
    warning_ = "Unknown shader category: " + category + ", using defaults";
    // Set basic defaults using PreviewSurface
    out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
    out->preview_surface = std::make_unique<PreviewSurfaceShader>();
    SetShaderParam(out->preview_surface->diffuse_color, 0.5f, 0.5f, 0.5f);
  }

  return true;
}

bool MtlxConverter::ApplyLookToRenderScene(const std::string& mtlx_content,
                                            const std::string& look_name,
                                            RenderScene* scene,
                                            const std::string& anchor_path) {
  error_.clear();
  if (!scene) {
    error_ = "Output scene is null";
    return false;
  }
  mtlx::MtlxDocument doc;
  if (!(anchor_path.empty() ? doc.ParseFromXML(mtlx_content)
                            : doc.ParseFromXML(mtlx_content, anchor_path))) {
    error_ = "Failed to parse MaterialX: " + doc.GetError();
    return false;
  }
  if (!doc.FindLook(look_name)) {
    error_ = "MaterialX look not found: " + look_name;
    return false;
  }

  std::vector<RenderMaterial> new_materials;
  std::map<std::string, size_t> material_indices;
  std::vector<std::pair<int32_t*, size_t>> bindings;
  std::vector<std::pair<size_t, size_t>> mesh_bindings;
  std::map<std::string, std::shared_ptr<const RenderMaterialXVolumeBinding>> volume_materials;
  std::vector<std::pair<RenderVolume*, std::shared_ptr<const RenderMaterialXVolumeBinding>>>
      volume_bindings;
  std::shared_ptr<const std::string> resolved_volume_xml;
  const auto collect = [&](const std::string& path, int32_t* material_id,
                           size_t* selected) {
    std::vector<mtlx::MtlxResolvedMaterialAssignment> matches;
    std::string resolve_error;
    if (!doc.GetMaterialAssignmentsForGeometry(look_name, path, &matches,
                                                &resolve_error)) {
      error_ = resolve_error;
      return false;
    }
    if (matches.empty()) return true;
    const std::string& name = matches.front().material->GetName();
    auto found = material_indices.find(name);
    if (found == material_indices.end()) {
      RenderMaterial material;
      if (!ConvertToRenderMaterial(mtlx_content, name, &material, anchor_path))
        return false;
      const size_t index = new_materials.size();
      new_materials.push_back(std::move(material));
      found = material_indices.emplace(name, index).first;
    }
    bindings.emplace_back(material_id, found->second);
    if (selected) *selected = found->second;
    return true;
  };
  for (size_t i = 0; i < scene->meshes.size(); ++i) {
    RenderMesh& mesh = scene->meshes[i];
    size_t selected = static_cast<size_t>(-1);
    if (!collect(mesh.prim_path, &mesh.material_id, &selected)) return false;
    if (selected != static_cast<size_t>(-1)) mesh_bindings.emplace_back(i, selected);
    for (auto& subset : mesh.material_subsets) {
      const std::string path = subset.name.empty() ? mesh.prim_path :
          mesh.prim_path + "/" + subset.name;
      if (!collect(path, &subset.material_id, nullptr)) return false;
    }
  }
  for (RenderPoints& points : scene->points)
    if (!collect(points.prim_path, &points.material_id, nullptr)) return false;
  for (RenderCurves& curves : scene->curves)
    if (!collect(curves.prim_path, &curves.material_id, nullptr)) return false;
  for (RenderVolume& volume : scene->volumes) {
    std::vector<mtlx::MtlxResolvedMaterialAssignment> matches;
    std::string resolve_error;
    if (!doc.GetMaterialAssignmentsForGeometry(look_name, volume.prim_path,
                                                &matches, &resolve_error)) {
      error_ = resolve_error;
      return false;
    }
    if (matches.empty()) continue;
    const auto& material = matches.front().material;
    const std::string& name = material->GetName();
    auto found = volume_materials.find(name);
    if (found == volume_materials.end()) {
      if (material->GetVolumeShader().empty() && material->GetVolumeNodeGraph().empty()) {
        error_ = "MaterialX volume assignment has no volume shader: " + volume.prim_path;
        return false;
      }
      if (!resolved_volume_xml)
        resolved_volume_xml = std::make_shared<const std::string>(doc.GetResolvedXML());
      if (!CreateVolumeEvaluator(*resolved_volume_xml, name, anchor_path)) {
        error_ = "Invalid MaterialX volume assignment for " + volume.prim_path + ": " + error_;
        return false;
      }
      auto binding = std::make_shared<RenderMaterialXVolumeBinding>();
      binding->resolved_xml = resolved_volume_xml;
      binding->material_name = name;
      binding->anchor_path = anchor_path;
      found = volume_materials.emplace(name, std::move(binding)).first;
    }
    volume_bindings.emplace_back(&volume, found->second);
  }
  const size_t base = scene->materials.size();
  if (base + new_materials.size() > static_cast<size_t>(INT32_MAX)) {
    error_ = "Too many render materials";
    return false;
  }
  for (RenderMaterial& material : new_materials)
    scene->materials.push_back(std::move(material));
  for (const auto& binding : bindings)
    *binding.first = static_cast<int32_t>(base + binding.second);
  for (const auto& binding : volume_bindings) {
    RenderVolume& volume = *binding.first;
    volume.materialx_binding = binding.second;
    volume.material_path.clear();
    volume.material_id = -1;
    const std::string warning =
        "MaterialX volume graph requires renderer evaluation; legacy constants are unchanged";
    if (std::find(volume.warnings.begin(), volume.warnings.end(), warning) == volume.warnings.end())
      volume.warnings.push_back(warning);
  }
  for (auto& draw : scene->point_instance_draws) {
    if (draw.mesh_id < 0) continue;
    for (const auto& binding : mesh_bindings) {
      if (static_cast<size_t>(draw.mesh_id) == binding.first) {
        draw.material_id = static_cast<int32_t>(base + binding.second);
        break;
      }
    }
  }
  return true;
}

bool MtlxConverter::ConvertFileToRenderMaterial(const std::string& filename,
                                                 const std::string& material_name,
                                                 RenderMaterial* out) {
  if (!::lightusd::NativeIOAvailable(&error_)) return false;

  std::ifstream ifs(filename.c_str(), std::ios::in | std::ios::binary);
  if (!ifs) {
    error_ = "Failed to open MaterialX file: " + filename;
    return false;
  }

  std::ostringstream ss;
  ss << ifs.rdbuf();
  if (!ifs.good() && !ifs.eof()) {
    error_ = "Failed to read MaterialX file: " + filename;
    return false;
  }

  return ConvertToRenderMaterial(ss.str(), material_name, out, filename);
}

bool MtlxConverter::ConvertUsdMtlxMaterial(const lightusd::Stage& stage,
                                            const lightusd::UsdPrim& material_prim,
                                            RenderMaterial* out,
                                            bool allow_converter_delegation) {
  if (!out) {
    error_ = "Output material is null";
    return false;
  }

  // Check if this is a material
  if (!lightusd::IsMaterial(material_prim)) {
    error_ = "Prim is not a Material";
    return false;
  }

  // Get material name
  out->name = material_prim.GetName();

  // Get surface shader path
  std::string surface_shader_path = GetMtlxSurfaceShaderPath(stage, material_prim);
  if (surface_shader_path.empty()) {
    error_ = "Material has no surface shader";
    return false;
  }

  // Get the shader prim
  lightusd::UsdPrim shader_prim =
      stage.GetPrimAtPath(StripPropertyPath(surface_shader_path));
  if (!shader_prim.IsValid()) {
    error_ = "Surface shader not found: " + surface_shader_path;
    return false;
  }

  // Check shader ID for MaterialX
  std::string shader_id = lightusd::GetShaderId(shader_prim);

  // Prefer the shared next render converter when the shader is material-local.
  // It already handles UsdPreviewSurface, MaterialX UsdPreviewSurface, and
  // OpenPBR inputs consistently with lusdview/lusdrender extraction.
  // Guarded: ConvertMaterial itself falls back to ConvertUsdMtlxMaterial when
  // it cannot convert the shader, so delegating back from that path would
  // recurse until stack overflow.
  if (allow_converter_delegation && shader_prim.GetParent().IsValid() &&
      shader_prim.GetParent().GetPath().str() == material_prim.GetPath().str()) {
    RenderSceneConverter converter;
    if (converter.ConvertMaterial(stage, material_prim, out)) {
      return true;
    }
  }

  // If it's a standard UsdPreviewSurface, use the existing schema fallback.
  if (shader_id == "UsdPreviewSurface") {
    lightusd::PreviewSurfaceData ps_data;
    if (!lightusd::GetPreviewSurfaceData(stage, shader_prim, &ps_data)) {
      error_ = "Failed to get PreviewSurface data";
      return false;
    }

    // Convert PreviewSurfaceData to RenderMaterial
    out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
    out->preview_surface = std::make_unique<PreviewSurfaceShader>();

    SetShaderParam(out->preview_surface->diffuse_color,
                   ps_data.diffuse_color[0], ps_data.diffuse_color[1], ps_data.diffuse_color[2]);
    SetShaderParam(out->preview_surface->metallic, ps_data.metallic);
    SetShaderParam(out->preview_surface->roughness, ps_data.roughness);
    SetShaderParam(out->preview_surface->clearcoat, ps_data.clearcoat);
    SetShaderParam(out->preview_surface->clearcoat_roughness, ps_data.clearcoat_roughness);
    SetShaderParam(out->preview_surface->opacity, ps_data.opacity);
    SetShaderParam(out->preview_surface->ior, ps_data.ior);

    return true;
  }

  // Check for MaterialX shader binding (implementationSource = "mtlx")
  std::string impl_source = lightusd::GetShaderImplementationSource(shader_prim);
  if (impl_source != "mtlx" && shader_id.find("ND_") != 0) {
    // Not MaterialX, and not a shader anyone else claimed either: hand back a
    // neutral stand-in rather than failing, but mark it, or the mesh silently
    // shades gray and no consumer can tell.
    warning_ = "Shader is not MaterialX: " + shader_id;
    out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
    out->preview_surface = std::make_unique<PreviewSurfaceShader>();
    out->default_fallback = true;
    SetShaderParam(out->preview_surface->diffuse_color, 0.5f, 0.5f, 0.5f);
    return true;
  }

  // Non-local MaterialX shader graphs cannot be evaluated here without the
  // material-local graph context. Return a deterministic PreviewSurface fallback
  // instead of a half-populated material.
  warning_ = "MaterialX shader graph is external to the material; using fallback PreviewSurface";
  out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
  out->preview_surface = std::make_unique<PreviewSurfaceShader>();
  out->default_fallback = true;
  SetShaderParam(out->preview_surface->diffuse_color, 0.5f, 0.5f, 0.5f);

  return true;
}

bool MtlxConverter::ParseMtlx(const std::string& mtlx_content,
                               std::vector<MtlxMaterialInfo>* materials) {
  if (!materials) {
    error_ = "Output materials is null";
    return false;
  }

  mtlx::MtlxDocument doc;
  if (!doc.ParseFromXML(mtlx_content)) {
    error_ = "Failed to parse MaterialX: " + doc.GetError();
    return false;
  }

  // Build the document's node list ONCE and share it with every material; it
  // does not vary per material (see MtlxMaterialInfo::shared_nodes).
  auto shared_nodes = std::make_shared<std::vector<MtlxNodeInfo>>();
  {
    shared_nodes->reserve(doc.GetNodes().size());
    for (const auto& node : doc.GetNodes()) {
      MtlxNodeInfo node_info;
      node_info.name = node->GetName();
      node_info.category = node->GetCategory();
      node_info.type = node->GetType();

      for (const auto& input : node->GetInputs()) {
        const auto& value = input->GetValue();
        if (value.type == mtlx::MtlxValue::TYPE_FLOAT) {
          node_info.input_floats[input->GetName()] = value.float_val;
        } else if (value.type == mtlx::MtlxValue::TYPE_FLOAT_VECTOR) {
          node_info.input_vectors[input->GetName()] = value.float_vec;
        } else if (value.type == mtlx::MtlxValue::TYPE_STRING) {
          node_info.input_strings[input->GetName()] = value.string_val;
        }

        if (!input->GetNodeName().empty()) {
          std::string conn = input->GetNodeName();
          if (!input->GetOutput().empty()) {
            conn += "." + input->GetOutput();
          }
          node_info.input_connections[input->GetName()] = conn;
        }
      }

      shared_nodes->push_back(std::move(node_info));
    }
  }

  materials->reserve(doc.GetMaterials().size());
  for (const auto& mat : doc.GetMaterials()) {
    MtlxMaterialInfo info;
    info.name = mat->GetName();
    info.surface_shader = mat->GetSurfaceShader();
    info.displacement_shader = mat->GetDisplacementShader();
    info.volume_shader = mat->GetVolumeShader();
    info.shared_nodes = shared_nodes;
    materials->push_back(std::move(info));
  }

  return true;
}

bool MtlxConverter::ConvertStandardSurface(const StandardSurfaceData& ss,
                                            MtlxPreviewSurfaceData* out) {
  if (!out) return false;

  // Base color -> diffuse (weighted by base)
  out->diffuse_color[0] = ss.base * ss.base_color[0];
  out->diffuse_color[1] = ss.base * ss.base_color[1];
  out->diffuse_color[2] = ss.base * ss.base_color[2];

  // Metalness direct mapping
  out->metallic = ss.metalness;

  // Specular roughness
  out->roughness = ss.specular_roughness;

  // IOR
  out->ior = ss.specular_IOR;

  // Coat -> clearcoat
  out->clearcoat = ss.coat;
  out->clearcoat_roughness = ss.coat_roughness;

  // Opacity (use average of RGB opacity)
  out->opacity = (ss.opacity[0] + ss.opacity[1] + ss.opacity[2]) / 3.0f;

  // Emission
  if (ss.emission > 0.0f) {
    out->emissive_color[0] = ss.emission * ss.emission_color[0];
    out->emissive_color[1] = ss.emission * ss.emission_color[1];
    out->emissive_color[2] = ss.emission * ss.emission_color[2];
  }

  // Texture connections
  out->diffuse_texture = ss.base_color_texture;
  out->normal_texture = ss.normal_texture;
  out->metallic_texture = ss.metalness_texture;
  out->roughness_texture = ss.specular_roughness_texture;
  out->emissive_texture = ss.emission_texture;
  out->occlusion_texture = ss.opacity_texture;

  return true;
}

bool MtlxConverter::ParseStandardSurface(const MtlxNodeInfo& node,
                                          StandardSurfaceData* out) {
  if (!out) return false;

  // Parse float inputs
  auto get_float = [&node](const std::string& name, float default_val) -> float {
    auto it = node.input_floats.find(name);
    return (it != node.input_floats.end()) ? it->second : default_val;
  };

  auto get_color3 = [&node](const std::string& name, float* out3,
                            float r, float g, float b) {
    auto it = node.input_vectors.find(name);
    if (it != node.input_vectors.end() && it->second.size() >= 3) {
      out3[0] = it->second[0];
      out3[1] = it->second[1];
      out3[2] = it->second[2];
    } else {
      out3[0] = r;
      out3[1] = g;
      out3[2] = b;
    }
  };

  auto get_connection = [&node](const std::string& name) -> std::string {
    auto it = node.input_connections.find(name);
    return (it != node.input_connections.end()) ? it->second : "";
  };

  // Base
  out->base = get_float("base", 1.0f);
  get_color3("base_color", out->base_color, 0.8f, 0.8f, 0.8f);
  out->base_color_texture = get_connection("base_color");

  // Specular
  out->specular = get_float("specular", 1.0f);
  get_color3("specular_color", out->specular_color, 1.0f, 1.0f, 1.0f);
  out->specular_roughness = get_float("specular_roughness", 0.2f);
  out->specular_IOR = get_float("specular_IOR", 1.5f);
  out->specular_anisotropy = get_float("specular_anisotropy", 0.0f);
  out->specular_rotation = get_float("specular_rotation", 0.0f);
  out->specular_roughness_texture = get_connection("specular_roughness");

  // Metalness
  out->metalness = get_float("metalness", 0.0f);
  out->metalness_texture = get_connection("metalness");

  // Transmission
  out->transmission = get_float("transmission", 0.0f);
  get_color3("transmission_color", out->transmission_color, 1.0f, 1.0f, 1.0f);
  out->transmission_depth = get_float("transmission_depth", 0.0f);
  out->transmission_dispersion = get_float("transmission_dispersion", 0.0f);

  // Subsurface
  out->subsurface = get_float("subsurface", 0.0f);
  get_color3("subsurface_color", out->subsurface_color, 1.0f, 1.0f, 1.0f);
  out->subsurface_scale = get_float("subsurface_scale", 1.0f);

  // Sheen
  out->sheen = get_float("sheen", 0.0f);
  get_color3("sheen_color", out->sheen_color, 1.0f, 1.0f, 1.0f);
  out->sheen_roughness = get_float("sheen_roughness", 0.3f);

  // Coat
  out->coat = get_float("coat", 0.0f);
  get_color3("coat_color", out->coat_color, 1.0f, 1.0f, 1.0f);
  out->coat_roughness = get_float("coat_roughness", 0.1f);
  out->coat_IOR = get_float("coat_IOR", 1.5f);
  out->coat_normal_texture = get_connection("coat_normal");

  // Thin Film
  out->thin_film_thickness = get_float("thin_film_thickness", 0.0f);
  out->thin_film_IOR = get_float("thin_film_IOR", 1.5f);

  // Emission
  out->emission = get_float("emission", 0.0f);
  get_color3("emission_color", out->emission_color, 1.0f, 1.0f, 1.0f);
  out->emission_texture = get_connection("emission_color");

  // Geometry
  get_color3("opacity", out->opacity, 1.0f, 1.0f, 1.0f);
  out->opacity_texture = get_connection("opacity");
  out->normal_texture = get_connection("normal");
  out->tangent_texture = get_connection("tangent");

  return true;
}

bool MtlxConverter::ParseTextureNode(const MtlxNodeInfo& node,
                                      MtlxTextureData* out) {
  if (!out) return false;

  auto it = node.input_strings.find("file");
  if (it != node.input_strings.end()) {
    out->file = it->second;
  }

  it = node.input_strings.find("uaddressmode");
  if (it != node.input_strings.end()) {
    out->uaddressmode = it->second;
  }

  it = node.input_strings.find("vaddressmode");
  if (it != node.input_strings.end()) {
    out->vaddressmode = it->second;
  }

  it = node.input_strings.find("filtertype");
  if (it != node.input_strings.end()) {
    out->filtertype = it->second;
  }

  if (!node.colorspace.empty()) {
    out->colorspace = node.colorspace;
  }

  auto vit = node.input_vectors.find("default");
  if (vit == node.input_vectors.end()) vit = node.input_vectors.find("defaultvalue");
  if (vit != node.input_vectors.end()) {
    const size_t n = std::min<size_t>(vit->second.size(), 4);
    for (size_t i = 0; i < n; ++i) out->default_value[i] = vit->second[i];
  } else {
    auto fit = node.input_floats.find("default");
    if (fit == node.input_floats.end()) fit = node.input_floats.find("defaultvalue");
    if (fit != node.input_floats.end()) {
      out->default_value[0] = fit->second;
      out->default_value[1] = fit->second;
      out->default_value[2] = fit->second;
      out->default_value[3] = 1.0f;
    }
  }

  return true;
}

void MtlxConverter::PopulateRenderMaterial(
    const MtlxPreviewSurfaceData& data,
    const std::map<std::string, MtlxTextureData>& /* textures */,
    RenderMaterial* out) {

  out->shader_type = RenderMaterial::ShaderType::PreviewSurface;
  out->preview_surface = std::make_unique<PreviewSurfaceShader>();

  SetShaderParam(out->preview_surface->diffuse_color,
                 data.diffuse_color[0], data.diffuse_color[1], data.diffuse_color[2]);

  SetShaderParam(out->preview_surface->emissive_color,
                 data.emissive_color[0], data.emissive_color[1], data.emissive_color[2]);

  SetShaderParam(out->preview_surface->specular_color,
                 data.specular_color[0], data.specular_color[1], data.specular_color[2]);

  SetShaderParam(out->preview_surface->metallic, data.metallic);
  SetShaderParam(out->preview_surface->roughness, data.roughness);
  SetShaderParam(out->preview_surface->clearcoat, data.clearcoat);
  SetShaderParam(out->preview_surface->clearcoat_roughness, data.clearcoat_roughness);
  SetShaderParam(out->preview_surface->opacity, data.opacity);
  SetShaderParam(out->preview_surface->ior, data.ior);

  SetShaderParam(out->preview_surface->normal,
                 data.normal[0], data.normal[1], data.normal[2]);

  out->preview_surface->use_specular_workflow = data.use_specular_workflow;

  // Note: Texture indices would need to be set up by the caller
  // based on the texture data and asset loading
}

// ============================================================
// Utility functions
// ============================================================

bool HasMtlxBinding(const lightusd::UsdPrim& prim) {
  if (!prim.IsValid()) return false;

  if (lightusd::IsShader(prim)) {
    const std::string impl = lightusd::GetShaderImplementationSource(prim);
    if (impl == "mtlx") return true;

    const std::string id = lightusd::GetShaderId(prim);
    if (HasPrefix(id, "ND_") || id == "open_pbr_surface" ||
        id == "UsdPreviewSurface") {
      return true;
    }
  }

  if (!GetMtlxFilePath(prim).empty()) return true;

  const std::vector<std::string> props = prim.GetPropertyNames();
  for (const std::string& name : props) {
    if (HasPrefix(name, "config:mtlx:") || HasPrefix(name, "mtlx:") ||
        HasPrefix(name, "info:mtlx:")) {
      return true;
    }
  }

  if (prim.GetRelationship("outputs:mtlx:surface")) return true;

  const size_t child_count = prim.GetChildCount();
  for (size_t i = 0; i < child_count; ++i) {
    const lightusd::UsdPrim child = prim.GetChildAt(i);
    if (!child.IsValid()) continue;
    if (HasMtlxBinding(child)) return true;
  }

  return false;
}

std::string GetMtlxFilePath(const lightusd::UsdPrim& material_prim) {
  if (!material_prim.IsValid()) return "";

  static const char* kFileProps[] = {
      "config:mtlx:sourceUri",
      "config:mtlx:sourceAsset",
      "config:mtlx:file",
      "mtlx:sourceUri",
      "mtlx:sourceAsset",
      "mtlx:file",
      "info:mtlx:sourceAsset",
      "info:mtlx:file",
  };

  std::string value;
  for (const char* prop : kFileProps) {
    if (GetStringLikeProperty(material_prim, prop, &value) && !value.empty()) {
      return value;
    }
  }

  return "";
}

ColorSpace ParseMtlxColorSpace(const std::string& cs) {
  if (cs == "srgb_texture" || cs == "sRGB") {
    return ColorSpace::sRGB;
  } else if (cs == "lin_rec709" || cs == "linear") {
    return ColorSpace::Linear;
  } else if (cs == "raw" || cs == "none") {
    return ColorSpace::Raw;
  } else if (cs == "acescg" || cs == "ACEScg") {
    return ColorSpace::ACEScg;
  }
  return ColorSpace::sRGB;  // Default
}

}  // namespace tydra
}  // namespace lightusd
