// SPDX-License-Identifier: Apache 2.0
// Example program to parse MaterialX files

#include "../include/mtlx-dom.hh"
#include <iostream>
#include <iomanip>

using namespace lightusd::mtlx;

using namespace lightusd::legacy::mtlx;  // NOLINT

void print_indent(int level) {
  for (int i = 0; i < level; ++i) {
    std::cout << "  ";
  }
}

void print_value(const MtlxValue& value) {
  switch (value.type) {
    case MtlxValue::TYPE_BOOL:
      std::cout << (value.bool_val ? "true" : "false");
      break;
    case MtlxValue::TYPE_INT:
      std::cout << value.int_val;
      break;
    case MtlxValue::TYPE_FLOAT:
      std::cout << value.float_val;
      break;
    case MtlxValue::TYPE_STRING:
      std::cout << "\"" << value.string_val << "\"";
      break;
    case MtlxValue::TYPE_FLOAT_VECTOR:
      std::cout << "[";
      for (size_t i = 0; i < value.float_vec.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << value.float_vec[i];
      }
      std::cout << "]";
      break;
    case MtlxValue::TYPE_INT_VECTOR:
      std::cout << "[";
      for (size_t i = 0; i < value.int_vec.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << value.int_vec[i];
      }
      std::cout << "]";
      break;
    case MtlxValue::TYPE_STRING_VECTOR:
      std::cout << "[";
      for (size_t i = 0; i < value.string_vec.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << "\"" << value.string_vec[i] << "\"";
      }
      std::cout << "]";
      break;
    default:
      std::cout << "(none)";
      break;
  }
}

void print_input(MtlxInputPtr input, int indent) {
  print_indent(indent);
  std::cout << "Input: " << input->GetName();

  if (!input->GetType().empty()) {
    std::cout << " (type: " << input->GetType() << ")";
  }

  if (!input->GetNodeName().empty()) {
    std::cout << " -> node: " << input->GetNodeName();
    if (!input->GetOutput().empty()) {
      std::cout << "." << input->GetOutput();
    }
  } else if (!input->GetInterfaceName().empty()) {
    std::cout << " -> interface: " << input->GetInterfaceName();
  } else {
    std::cout << " = ";
    print_value(input->GetValue());
  }

  std::cout << std::endl;
}

void print_node(MtlxNodePtr node, int indent) {
  print_indent(indent);
  std::cout << "Node: " << node->GetName()
            << " [" << node->GetCategory() << "]";

  if (!node->GetType().empty()) {
    std::cout << " -> " << node->GetType();
  }

  std::cout << std::endl;

  for (const auto& input : node->GetInputs()) {
    print_input(input, indent + 1);
  }
}

void print_nodegraph(MtlxNodeGraphPtr ng, int indent) {
  print_indent(indent);
  std::cout << "NodeGraph: " << ng->GetName() << std::endl;

  // Print inputs
  if (!ng->GetInputs().empty()) {
    print_indent(indent + 1);
    std::cout << "Inputs:" << std::endl;
    for (const auto& input : ng->GetInputs()) {
      print_input(input, indent + 2);
    }
  }

  // Print nodes
  if (!ng->GetNodes().empty()) {
    print_indent(indent + 1);
    std::cout << "Nodes:" << std::endl;
    for (const auto& node : ng->GetNodes()) {
      print_node(node, indent + 2);
    }
  }

  // Print outputs
  if (!ng->GetOutputs().empty()) {
    print_indent(indent + 1);
    std::cout << "Outputs:" << std::endl;
    for (const auto& output : ng->GetOutputs()) {
      print_indent(indent + 2);
      std::cout << "Output: " << output->GetName();
      if (!output->GetType().empty()) {
        std::cout << " (type: " << output->GetType() << ")";
      }
      if (!output->GetNodeName().empty()) {
        std::cout << " -> node: " << output->GetNodeName();
        if (!output->GetOutput().empty()) {
          std::cout << "." << output->GetOutput();
        }
      }
      std::cout << std::endl;
    }
  }
}

void print_material(MtlxMaterialPtr mat, int indent) {
  print_indent(indent);
  std::cout << "Material: " << mat->GetName();

  if (!mat->GetType().empty()) {
    std::cout << " (type: " << mat->GetType() << ")";
  }

  std::cout << std::endl;

  if (!mat->GetSurfaceShader().empty()) {
    print_indent(indent + 1);
    std::cout << "Surface Shader: " << mat->GetSurfaceShader() << std::endl;
  }

  if (!mat->GetDisplacementShader().empty()) {
    print_indent(indent + 1);
    std::cout << "Displacement Shader: " << mat->GetDisplacementShader() << std::endl;
  }

  if (!mat->GetVolumeShader().empty()) {
    print_indent(indent + 1);
    std::cout << "Volume Shader: " << mat->GetVolumeShader() << std::endl;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <materialx_file.mtlx>" << std::endl;

    // If no file provided, create and parse a sample
    std::cout << "\nNo file provided. Parsing sample MaterialX document..." << std::endl;

    const char* sample = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <!-- Texture nodegraph -->
  <nodegraph name="NG_marble_texture">
    <input name="base_color_file" type="filename" value="marble_diffuse.png"/>
    <input name="roughness_file" type="filename" value="marble_roughness.png"/>

    <image name="base_color_image" type="color3">
      <input name="file" type="filename" interfacename="base_color_file"/>
      <input name="uaddressmode" type="string" value="periodic"/>
      <input name="vaddressmode" type="string" value="periodic"/>
    </image>

    <image name="roughness_image" type="float">
      <input name="file" type="filename" interfacename="roughness_file"/>
      <input name="uaddressmode" type="string" value="periodic"/>
      <input name="vaddressmode" type="string" value="periodic"/>
    </image>

    <output name="base_color_out" type="color3" nodename="base_color_image"/>
    <output name="roughness_out" type="float" nodename="roughness_image"/>
  </nodegraph>

  <!-- Shader -->
  <standard_surface name="SR_marble" type="surfaceshader">
    <input name="base" type="float" value="0.8"/>
    <input name="base_color" type="color3" nodename="NG_marble_texture" output="base_color_out"/>
    <input name="specular" type="float" value="1.0"/>
    <input name="specular_roughness" type="float" nodename="NG_marble_texture" output="roughness_out"/>
    <input name="metalness" type="float" value="0.0"/>
    <input name="subsurface" type="float" value="0.3"/>
    <input name="subsurface_color" type="color3" value="0.9, 0.9, 0.8"/>
  </standard_surface>

  <!-- Material -->
  <surfacematerial name="M_marble" type="material">
    <shaderref name="surfaceshader" node="SR_marble"/>
  </surfacematerial>
</materialx>
    )";

    MtlxDocument doc;
    if (!doc.ParseFromXML(sample)) {
      std::cerr << "Error: " << doc.GetError() << std::endl;
      return 1;
    }

    std::cout << "\n=== MaterialX Document ===" << std::endl;
    std::cout << "Version: " << doc.GetVersion() << std::endl;
    if (!doc.GetColorSpace().empty()) {
      std::cout << "ColorSpace: " << doc.GetColorSpace() << std::endl;
    }
    if (!doc.GetNamespace().empty()) {
      std::cout << "Namespace: " << doc.GetNamespace() << std::endl;
    }

    std::cout << "\n--- NodeGraphs ---" << std::endl;
    for (const auto& ng : doc.GetNodeGraphs()) {
      print_nodegraph(ng, 0);
    }

    std::cout << "\n--- Shaders ---" << std::endl;
    for (const auto& node : doc.GetNodes()) {
      print_node(node, 0);
    }

    std::cout << "\n--- Materials ---" << std::endl;
    for (const auto& mat : doc.GetMaterials()) {
      print_material(mat, 0);
    }

    if (!doc.GetWarning().empty()) {
      std::cout << "\nWarnings:\n" << doc.GetWarning() << std::endl;
    }

    return 0;
  }

  // Parse file from command line
  MtlxDocument doc;
  if (!doc.ParseFromFile(argv[1])) {
    std::cerr << "Error: " << doc.GetError() << std::endl;
    return 1;
  }

  std::cout << "=== MaterialX Document: " << argv[1] << " ===" << std::endl;
  std::cout << "Version: " << doc.GetVersion() << std::endl;
  if (!doc.GetColorSpace().empty()) {
    std::cout << "ColorSpace: " << doc.GetColorSpace() << std::endl;
  }
  if (!doc.GetNamespace().empty()) {
    std::cout << "Namespace: " << doc.GetNamespace() << std::endl;
  }

  std::cout << "\n--- NodeGraphs ---" << std::endl;
  for (const auto& ng : doc.GetNodeGraphs()) {
    print_nodegraph(ng, 0);
  }

  std::cout << "\n--- Shaders ---" << std::endl;
  for (const auto& node : doc.GetNodes()) {
    print_node(node, 0);
  }

  std::cout << "\n--- Materials ---" << std::endl;
  for (const auto& mat : doc.GetMaterials()) {
    print_material(mat, 0);
  }

  if (!doc.GetWarning().empty()) {
    std::cout << "\nWarnings:\n" << doc.GetWarning() << std::endl;
  }

  return 0;
}