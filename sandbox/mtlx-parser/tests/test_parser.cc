// SPDX-License-Identifier: Apache 2.0
// Test program for MaterialX parser

#include "../include/mtlx-dom.hh"
#include <iostream>
#include <cassert>

using namespace lightusd::mtlx;

using namespace lightusd::legacy::mtlx;  // NOLINT

void test_tokenizer() {
  std::cout << "Testing XML Tokenizer..." << std::endl;

  const char* xml = R"(<?xml version="1.0"?>
    <materialx version="1.38">
      <!-- This is a comment -->
      <nodegraph name="test_graph">
        <input name="base_color" type="color3" value="0.8, 0.8, 0.8"/>
        <image name="diffuse_texture" type="color3">
          <input name="file" type="filename" value="textures/diffuse.png"/>
        </image>
      </nodegraph>
    </materialx>
  )";

  XMLTokenizer tokenizer;
  assert(tokenizer.Initialize(xml, std::strlen(xml)));

  Token token;
  int token_count = 0;

  while (tokenizer.NextToken(token)) {
    if (token.type == TokenType::EndOfDocument) break;
    token_count++;

    switch (token.type) {
      case TokenType::ProcessingInstruction:
        std::cout << "  PI: " << token.name << std::endl;
        break;
      case TokenType::StartTag:
        std::cout << "  Start: <" << token.name << ">" << std::endl;
        break;
      case TokenType::EndTag:
        std::cout << "  End: </" << token.name << ">" << std::endl;
        break;
      case TokenType::Attribute:
        std::cout << "    Attr: " << token.name << "=\"" << token.value << "\"" << std::endl;
        break;
      case TokenType::Comment:
        std::cout << "  Comment: " << token.value << std::endl;
        break;
      default:
        break;
    }
  }

  assert(token_count > 0);
  std::cout << "  Tokenizer test passed (" << token_count << " tokens)" << std::endl;
}

void test_xml_parser() {
  std::cout << "Testing XML Parser..." << std::endl;

  const char* xml = R"(<?xml version="1.0"?>
    <materialx version="1.38" colorspace="lin_rec709">
      <nodegraph name="test_graph">
        <input name="base_color" type="color3" value="0.8, 0.8, 0.8"/>
        <image name="diffuse_texture" type="color3">
          <input name="file" type="filename" value="textures/diffuse.png"/>
          <input name="uaddressmode" type="string" value="periodic"/>
        </image>
        <output name="out" type="color3" nodename="diffuse_texture"/>
      </nodegraph>
    </materialx>
  )";

  XMLDocument doc;
  assert(doc.ParseString(xml));

  auto root = doc.GetRoot();
  assert(root);
  assert(root->GetName() == "materialx");
  assert(root->GetAttribute("version") == "1.38");
  assert(root->GetAttribute("colorspace") == "lin_rec709");

  auto nodegraph = root->GetChild("nodegraph");
  assert(nodegraph);
  assert(nodegraph->GetAttribute("name") == "test_graph");

  auto image = nodegraph->GetChild("image");
  assert(image);
  assert(image->GetAttribute("name") == "diffuse_texture");

  auto inputs = image->GetChildren("input");
  assert(inputs.size() == 2);

  std::cout << "  XML parser test passed" << std::endl;
}

void test_materialx_parser() {
  std::cout << "Testing MaterialX Parser..." << std::endl;

  const char* xml = R"(<?xml version="1.0"?>
    <materialx version="1.38" colorspace="lin_rec709">
      <standard_surface name="SR_default" type="surfaceshader">
        <input name="base" type="float" value="1.0"/>
        <input name="base_color" type="color3" value="0.8, 0.8, 0.8"/>
        <input name="specular" type="float" value="1.0"/>
        <input name="specular_roughness" type="float" value="0.2"/>
        <input name="metalness" type="float" value="0.0"/>
      </standard_surface>

      <surfacematerial name="M_default" type="material">
        <shaderref name="surfaceshader" node="SR_default"/>
      </surfacematerial>
    </materialx>
  )";

  MaterialXParser parser;
  assert(parser.Parse(xml));
  assert(parser.GetVersion() == "1.38");
  assert(parser.GetColorSpace() == "lin_rec709");

  // Validate the document
  assert(parser.Validate());

  std::cout << "  MaterialX parser test passed" << std::endl;
}

void test_materialx_dom() {
  std::cout << "Testing MaterialX DOM..." << std::endl;

  const char* xml = R"(<?xml version="1.0"?>
    <materialx version="1.38">
      <nodegraph name="NG_texture">
        <input name="file" type="filename" value="default.png"/>
        <image name="image1" type="color3">
          <input name="file" type="filename" interfacename="file"/>
          <input name="uaddressmode" type="string" value="periodic"/>
          <input name="vaddressmode" type="string" value="periodic"/>
        </image>
        <output name="out" type="color3" nodename="image1"/>
      </nodegraph>

      <standard_surface name="SR_marble" type="surfaceshader">
        <input name="base" type="float" value="0.8"/>
        <input name="base_color" type="color3" nodename="NG_texture" output="out"/>
        <input name="specular_roughness" type="float" value="0.1"/>
      </standard_surface>

      <surfacematerial name="M_marble" type="material">
        <shaderref name="surfaceshader" node="SR_marble"/>
      </surfacematerial>
    </materialx>
  )";

  MtlxDocument doc;
  assert(doc.ParseFromXML(xml));
  assert(doc.GetVersion() == "1.38");

  // Check nodegraph
  auto nodegraphs = doc.GetNodeGraphs();
  assert(nodegraphs.size() == 1);

  auto ng = nodegraphs[0];
  assert(ng->GetName() == "NG_texture");
  assert(ng->GetInputs().size() == 1);
  assert(ng->GetNodes().size() == 1);
  assert(ng->GetOutputs().size() == 1);

  // Check node
  auto image_node = ng->GetNode("image1");
  assert(image_node);
  assert(image_node->GetCategory() == "image");
  assert(image_node->GetInputs().size() == 3);

  // Check shader
  auto nodes = doc.GetNodes();
  assert(nodes.size() == 1);

  auto shader = nodes[0];
  assert(shader->GetName() == "SR_marble");
  assert(shader->GetCategory() == "standard_surface");

  // Check material
  auto materials = doc.GetMaterials();
  assert(materials.size() == 1);

  auto material = materials[0];
  assert(material->GetName() == "M_marble");
  assert(material->GetSurfaceShader() == "SR_marble");

  std::cout << "  MaterialX DOM test passed" << std::endl;
}

void test_security_limits() {
  std::cout << "Testing security limits..." << std::endl;

  // Test max string length
  std::string long_xml = R"(<?xml version="1.0"?><materialx version="1.38"><node name=")";
  for (int i = 0; i < 300; ++i) {
    long_xml += "a";
  }
  long_xml += R"("></node></materialx>)";

  XMLTokenizer tokenizer;
  assert(tokenizer.Initialize(long_xml.c_str(), long_xml.size()));

  Token token;
  bool found_error = false;
  while (tokenizer.NextToken(token)) {
    if (token.type == TokenType::Error) {
      found_error = true;
      break;
    }
    if (token.type == TokenType::EndOfDocument) break;
  }

  // Should have hit the name length limit
  assert(found_error || tokenizer.GetError().find("exceeds maximum length") != std::string::npos);

  // Test max nesting depth (this would be a very deep recursion)
  // We'll create a more reasonable test here
  std::string nested_xml = R"(<?xml version="1.0"?><materialx version="1.38">)";
  for (int i = 0; i < 100; ++i) {
    nested_xml += "<node" + std::to_string(i) + ">";
  }
  for (int i = 99; i >= 0; --i) {
    nested_xml += "</node" + std::to_string(i) + ">";
  }
  nested_xml += "</materialx>";

  XMLDocument doc;
  // This should parse successfully as 100 levels is within our limit
  assert(doc.ParseString(nested_xml));

  std::cout << "  Security limits test passed" << std::endl;
}

void test_error_handling() {
  std::cout << "Testing error handling..." << std::endl;

  // Malformed XML
  const char* bad_xml1 = R"(<materialx version="1.38"><node></materialx>)";
  XMLDocument doc1;
  assert(!doc1.ParseString(bad_xml1));
  assert(!doc1.GetError().empty());

  // Missing quotes
  const char* bad_xml2 = R"(<materialx version=1.38></materialx>)";
  XMLDocument doc2;
  assert(!doc2.ParseString(bad_xml2));

  // Unclosed tag
  const char* bad_xml3 = R"(<materialx version="1.38"><node name="test")";
  XMLDocument doc3;
  assert(!doc3.ParseString(bad_xml3));

  std::cout << "  Error handling test passed" << std::endl;
}

int main() {
  std::cout << "Running MaterialX parser tests..." << std::endl;
  std::cout << "=================================" << std::endl;

  test_tokenizer();
  test_xml_parser();
  test_materialx_parser();
  test_materialx_dom();
  test_security_limits();
  test_error_handling();

  std::cout << "=================================" << std::endl;
  std::cout << "All tests passed!" << std::endl;

  return 0;
}