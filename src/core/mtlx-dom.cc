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

void AppendXmlEscaped(std::ostringstream *out, const std::string &text) {
  for (char c : text) {
    switch (c) {
      case '&': *out << "&amp;"; break;
      case '<': *out << "&lt;"; break;
      case '>': *out << "&gt;"; break;
      case '"': *out << "&quot;"; break;
      case '\'': *out << "&apos;"; break;
      default: *out << c; break;
    }
  }
}

void AppendXmlNode(std::ostringstream *out, const XMLNodePtr &node) {
  if (!out || !node) return;
  *out << '<' << node->GetName();
  for (const auto &attr : node->GetAttributes()) {
    *out << ' ' << attr.first << "=\"";
    AppendXmlEscaped(out, attr.second);
    *out << '"';
  }
  const auto &children = node->GetChildren();
  if (children.empty() && node->GetText().empty()) {
    *out << "/>";
    return;
  }
  *out << '>';
  AppendXmlEscaped(out, node->GetText());
  for (const XMLNodePtr &child : children) AppendXmlNode(out, child);
  *out << "</" << node->GetName() << '>';
}

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

void UpgradeMtlxElements(MtlxElem *elem, uint64_t minor_version,
                         MtlxElem *parent = nullptr) {
  if (!elem) return;
  if (minor_version == 37 && elem->AttrOr("type") == "color2")
    elem->SetAttr("type", "vector2");

  const size_t original_child_count = elem->children.size();
  std::vector<MtlxElem> generated;
  auto make_input = [](const char *name, const char *type,
                       const std::string &node_name,
                       const std::string &output = std::string()) {
    MtlxElem input;
    input.tag = "input";
    input.SetAttr("name", name);
    input.SetAttr("type", type);
    input.SetAttr("nodename", node_name);
    if (!output.empty()) input.SetAttr("output", output);
    return input;
  };
  auto unique_name = [&](const std::string &base_name) {
    std::string candidate = base_name;
    for (size_t suffix = 1;; ++suffix) {
      const bool exists = parent &&
          std::any_of(parent->children.begin(), parent->children.end(),
                      [&](const MtlxElem &child) {
                        return child.Name() == candidate;
                      });
      if (!exists) return candidate;
      candidate = base_name + "_" + std::to_string(suffix);
    }
  };

  for (size_t i = 0; i < original_child_count; ++i)
    UpgradeMtlxElements(&elem->children[i], minor_version, elem);

  if (minor_version == 37 && IsMtlxNodeElem(*elem)) {
    static const std::pair<const char *, const char *> kNodeRenames[] = {
        {"diffuse_brdf", "oren_nayar_diffuse_bsdf"},
        {"burley_diffuse_brdf", "burley_diffuse_bsdf"},
        {"diffuse_btdf", "translucent_bsdf"},
        {"subsurface_brdf", "subsurface_bsdf"},
        {"sheen_brdf", "sheen_bsdf"},
        {"generalized_schlick_brdf", "generalized_schlick_bsdf"},
        {"thin_film_brdf", "thin_film_bsdf"},
        {"dielectric_brdf", "dielectric_bsdf"},
        {"dielectric_btdf", "dielectric_bsdf"}};
    const std::string old_category = elem->tag;
    const bool was_dielectric_btdf = old_category == "dielectric_btdf";
    const bool needs_layer = old_category == "dielectric_brdf" ||
                             old_category == "generalized_schlick_brdf" ||
                             old_category == "sheen_brdf" ||
                             old_category == "thin_film_brdf";
    const bool was_conductor = old_category == "conductor_brdf";
    for (const auto &rename : kNodeRenames) {
      if (elem->tag == rename.first) {
        elem->tag = rename.second;
        break;
      }
    }
    if (was_dielectric_btdf) {
      elem->children.erase(
          std::remove_if(elem->children.begin(), elem->children.end(),
                         [](const MtlxElem &child) {
                           return child.tag == "input" &&
                                  child.AttrOr("name") == "interior";
                         }),
          elem->children.end());
      bool has_scatter_mode = false;
      for (const MtlxElem &child : elem->children) {
        if (child.tag == "input" &&
            child.AttrOr("name") == "scatter_mode") {
          has_scatter_mode = true;
          break;
        }
      }
      if (!has_scatter_mode) {
        MtlxElem scatter_mode;
        scatter_mode.tag = "input";
        scatter_mode.SetAttr("name", "scatter_mode");
        scatter_mode.SetAttr("type", "string");
        scatter_mode.SetAttr("value", "T");
        elem->children.push_back(std::move(scatter_mode));
      }
    }
    if (was_conductor) {
      elem->tag = "conductor_bsdf";
      const std::string node_name = elem->AttrOr("name");
      const std::string artistic_name =
          unique_name(node_name + "__artistic_ior");
      MtlxElem artistic;
      artistic.tag = "artistic_ior";
      artistic.SetAttr("name", artistic_name);
      artistic.SetAttr("type", "multioutput");
      artistic.children.push_back(MtlxElem{});
      artistic.children.back().tag = "output";
      artistic.children.back().SetAttr("name", "ior");
      artistic.children.back().SetAttr("type", "color3");
      artistic.children.push_back(MtlxElem{});
      artistic.children.back().tag = "output";
      artistic.children.back().SetAttr("name", "extinction");
      artistic.children.back().SetAttr("type", "color3");
      for (const char *input_name : {"reflectivity", "edge_color"}) {
        for (const MtlxElem &child : elem->children) {
          if (child.tag == "input" && child.AttrOr("name") == input_name) {
            artistic.children.push_back(child);
            break;
          }
        }
      }
      elem->children.erase(
          std::remove_if(elem->children.begin(), elem->children.end(),
                         [](const MtlxElem &child) {
                           return child.tag == "input" &&
                                  (child.AttrOr("name") == "reflectivity" ||
                                   child.AttrOr("name") == "edge_color");
                         }),
          elem->children.end());
      elem->children.push_back(make_input("ior", "color3", artistic_name, "ior"));
      elem->children.push_back(
          make_input("extinction", "color3", artistic_name, "extinction"));
      generated.push_back(std::move(artistic));
    }
    if (needs_layer) {
      auto base = std::find_if(elem->children.begin(), elem->children.end(),
                               [](const MtlxElem &child) {
                                 return child.tag == "input" &&
                                        child.AttrOr("name") == "base";
                               });
      if (base != elem->children.end()) {
        const bool has_binding = base->Attr("nodename") ||
                                 base->Attr("nodegraph") ||
                                 base->Attr("interfacename");
        MtlxElem layer_base = *base;
        layer_base.SetAttr("name", "base");
        layer_base.SetAttr("type", "BSDF");
        elem->children.erase(base);
        if (has_binding) {
          const std::string layer_name = elem->AttrOr("name");
          const std::string top_name =
              unique_name(layer_name + "__layer_top");
          elem->SetAttr("name", top_name);
          MtlxElem layer;
          layer.tag = "layer";
          layer.SetAttr("name", layer_name);
          layer.SetAttr("type", "BSDF");
          layer.children.push_back(make_input("top", "BSDF", top_name));
          layer.children.push_back(std::move(layer_base));
          generated.push_back(std::move(layer));
        }
      }
    }
  }

  if (IsMtlxNodeElem(*elem) && elem->tag == "atan2") {
    if (minor_version == 37) {
      // 1.37 -> 1.38 swaps the old positional inputs; 1.38 -> 1.39 then
      // names those positions `iny` and `inx`.
      MtlxElem *in1 = nullptr;
      MtlxElem *in2 = nullptr;
      for (MtlxElem &child : elem->children) {
        if (child.tag != "input") continue;
        if (child.AttrOr("name") == "in1") in1 = &child;
        else if (child.AttrOr("name") == "in2") in2 = &child;
      }
      if (in1 && in2) {
        in1->SetAttr("name", "");
        in2->SetAttr("name", "in1");
        in1->SetAttr("name", "in2");
      } else if (in1) {
        in1->SetAttr("name", "in2");
      } else if (in2) {
        in2->SetAttr("name", "in1");
      }
    } else if (minor_version == 38) {
      for (MtlxElem &child : elem->children) {
        if (child.tag != "input") continue;
        const std::string name = child.AttrOr("name");
        if (name == "in1") child.SetAttr("name", "iny");
        else if (name == "in2") child.SetAttr("name", "inx");
      }
    }
  }

  // MaterialX 1.39 disallows authored values on connected node inputs. This
  // cleanup belongs to the 1.38 -> 1.39 stage; applying it to 1.37 before
  // that stage would erase data before the intervening upgrade can interpret it.
  if (minor_version == 38 && IsMtlxNodeElem(*elem)) {
    for (MtlxElem &child : elem->children) {
      if (child.tag == "input" && child.Attr("value") &&
          (child.Attr("nodename") || child.Attr("nodegraph") ||
           child.Attr("interfacename") || child.Attr("output"))) {
        child.attrs.erase(
            std::remove_if(child.attrs.begin(), child.attrs.end(),
                           [](const auto &attr) { return attr.first == "value"; }),
            child.attrs.end());
      }
    }

    if (elem->tag == "subsurface_bsdf") {
      MtlxElem *radius = nullptr;
      for (MtlxElem &child : elem->children) {
        if (child.tag == "input" && child.AttrOr("name") == "radius") {
          radius = &child;
          break;
        }
      }
      if (radius && radius->AttrOr("type") == "vector3") {
        const std::string convert_name = unique_name("convert");
        MtlxElem convert;
        convert.tag = "convert";
        convert.SetAttr("name", convert_name);
        convert.SetAttr("type", "color3");
        MtlxElem convert_input = *radius;
        convert_input.SetAttr("name", "in");
        convert_input.SetAttr("type", "vector3");
        convert.children.push_back(std::move(convert_input));
        generated.push_back(std::move(convert));

        radius->attrs.erase(
            std::remove_if(radius->attrs.begin(), radius->attrs.end(),
                           [](const auto &attr) {
                             return attr.first == "value" ||
                                    attr.first == "nodename" ||
                                    attr.first == "nodegraph" ||
                                    attr.first == "output" ||
                                    attr.first == "interfacename" ||
                                    attr.first == "channels";
                           }),
            radius->attrs.end());
        radius->SetAttr("type", "color3");
        radius->SetAttr("nodename", convert_name);
      }
    } else if (elem->tag == "switch") {
      for (MtlxElem &child : elem->children) {
        if (child.tag != "input" || child.AttrOr("name") != "which") continue;
        const std::string value = child.AttrOr("value");
        MtlxTypedValue parsed;
        if (child.AttrOr("type") == "integer" &&
            ParseMtlxValue("integer", value, &parsed) && parsed.i >= 5) {
          child.SetAttr("value", "0");
        } else if (child.AttrOr("type") == "float" &&
                   ParseMtlxValue("float", value, &parsed) &&
                   parsed.num >= 5.0) {
          child.SetAttr("value", "0");
        }
      }
    }
  }

  MtlxElem *owner = parent ? parent : elem;
  for (MtlxElem &child : generated)
    owner->children.push_back(std::move(child));
}

namespace {

void EraseMtlxAttr(MtlxElem *elem, const std::string &key) {
  elem->attrs.erase(std::remove_if(elem->attrs.begin(), elem->attrs.end(),
                                   [&](const auto &a) { return a.first == key; }),
                    elem->attrs.end());
}

std::string UniqueMtlxChildName(const MtlxElem &scope,
                                const std::vector<MtlxElem> &generated,
                                const std::string &base) {
  for (size_t suffix = 0;; ++suffix) {
    const std::string candidate =
        suffix == 0 ? base : base + "_" + std::to_string(suffix);
    const bool in_scope = std::any_of(
        scope.children.begin(), scope.children.end(),
        [&](const MtlxElem &child) { return child.Name() == candidate; });
    const bool in_generated = std::any_of(
        generated.begin(), generated.end(), [&](const MtlxElem &child) {
          return child.Name() == candidate;
        });
    if (!in_scope && !in_generated) return candidate;
  }
}

std::string RemapColor2Channels(const std::string &channels) {
  std::string result = channels;
  for (char &c : result) {
    if (c == 'r') c = 'x';
    else if (c == 'a') c = 'y';
  }
  return result;
}

void UpgradeColor2ChannelAliases(MtlxElem *scope) {
  if (!scope) return;
  std::vector<std::string> color2_nodes;
  for (const MtlxElem &child : scope->children) {
    if (IsMtlxNodeElem(child) && child.AttrOr("type") == "color2")
      color2_nodes.push_back(child.Name());
  }
  for (const std::string &source_name : color2_nodes) {
    for (MtlxElem &child : scope->children) {
      if (IsMtlxNodeElem(child)) {
        for (MtlxElem &port : child.children) {
          if (port.tag != "input" && port.tag != "output") continue;
          if (port.AttrOr("nodename") != source_name) continue;
          if (const std::string *channels = port.Attr("channels"))
            port.SetAttr("channels", RemapColor2Channels(*channels));
          if (const std::string *output = port.Attr("output"))
            port.SetAttr("output", RemapColor2Channels(*output));
        }
      }
      if (child.Name() != source_name) continue;
      for (MtlxElem &input : child.children) {
        if (input.tag == "input" && input.Name() == "channels") {
          if (const std::string *value = input.Attr("value"))
            input.SetAttr("value", RemapColor2Channels(*value));
        }
      }
    }
  }
  for (MtlxElem &child : scope->children)
    UpgradeColor2ChannelAliases(&child);
}

size_t MtlxChannelCount(const std::string &type) {
  if (type == "float") return 1;
  if (type == "vector2") return 2;
  if (type == "vector3" || type == "color3") return 3;
  if (type == "vector4" || type == "color4") return 4;
  return 0;
}

std::string ConnectedNodeType(const MtlxElem &scope, const MtlxElem &port) {
  const std::string source_name = port.AttrOr("nodename");
  if (!source_name.empty()) {
    for (const MtlxElem &node : scope.children) {
      if (!IsMtlxNodeElem(node) || node.Name() != source_name) continue;
      const std::string output_name = port.AttrOr("output");
      if (!output_name.empty()) {
        for (const MtlxElem &output : node.children)
          if (output.tag == "output" && output.Name() == output_name)
            return output.AttrOr("type");
      }
      return node.AttrOr("type");
    }
  }
  const std::string interface_name = port.AttrOr("interfacename");
  if (!interface_name.empty()) {
    for (const MtlxElem &input : scope.children)
      if (input.tag == "input" && input.Name() == interface_name)
        return input.AttrOr("type");
  }
  return {};
}

std::string InferChannelSourceType(const std::string &channels,
                                   const std::string &dest_type) {
  if (channels == "xx" || channels == "xxx" || channels == "xxxx" ||
      channels == "rr" || channels == "rrr") return "float";
  if (channels == "xyz" || channels == "x" || channels == "y" ||
      channels == "z") return "vector3";
  if (channels == "rgba" || channels == "a") return "color4";
  return dest_type == "color3" ? "color4" : "color3";
}

std::vector<std::string> SplitChannelValue(const std::string &value) {
  std::vector<std::string> tokens;
  std::string token;
  for (char c : value) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' ||
        c == '\n') {
      if (!token.empty()) {
        tokens.push_back(std::move(token));
        token.clear();
      }
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty()) tokens.push_back(std::move(token));
  return tokens;
}

int MtlxChannelIndex(char channel) {
  switch (channel) {
    case 'r': case 'x': return 0;
    case 'g': case 'y': return 1;
    case 'b': case 'z': return 2;
    case 'a': case 'w': return 3;
    default: return -1;
  }
}

bool MtlxChannelConstant(char channel, const char **value) {
  if (channel == '0') {
    if (value) *value = "0.000000";
    return true;
  }
  if (channel == '1') {
    if (value) *value = "1.000000";
    return true;
  }
  return false;
}

std::string SwizzledConstantValue(const MtlxElem &port,
                                  const std::string &channels,
                                  size_t dest_count) {
  const std::vector<std::string> source =
      SplitChannelValue(port.AttrOr("value", "0"));
  std::string result;
  for (size_t i = 0; i < dest_count; ++i) {
    if (i) result += ", ";
    const char channel = i < channels.size() ? channels[i] : '\0';
    const char *constant = nullptr;
    if (MtlxChannelConstant(channel, &constant)) {
      result += constant;
      continue;
    }
    const int index = MtlxChannelIndex(channel);
    if (index >= 0 && static_cast<size_t>(index) < source.size()) {
      result += source[static_cast<size_t>(index)];
    } else if (!source.empty()) {
      result += source[0];
    } else {
      result += "0";
    }
  }
  return result;
}

void RewireToGeneratedNode(MtlxElem *port, const std::string &node_name) {
  EraseMtlxAttr(port, "nodename");
  EraseMtlxAttr(port, "nodegraph");
  EraseMtlxAttr(port, "output");
  EraseMtlxAttr(port, "interfacename");
  EraseMtlxAttr(port, "channels");
  EraseMtlxAttr(port, "value");
  port->SetAttr("nodename", node_name);
}

void UpgradeChannelPort(MtlxElem *port, MtlxElem *scope,
                        std::vector<MtlxElem> *generated) {
  if (!port || !scope || !generated) return;
  const std::string channels = port->AttrOr("channels");
  if (channels.empty()) return;
  const std::string dest_type = port->AttrOr("type");
  const size_t dest_count = MtlxChannelCount(dest_type);
  std::string source_type = ConnectedNodeType(*scope, *port);
  if (source_type.empty() || source_type == "multioutput")
    source_type = InferChannelSourceType(channels, dest_type);
  const size_t source_count = MtlxChannelCount(source_type);
  if (source_count == 0 || dest_count == 0) return;
  if (source_type == "float" && dest_type == "float") {
    EraseMtlxAttr(port, "channels");
    return;
  }

  // A literal input, an unbound input, or an all-constant selector becomes a
  // constant node in the reference upgrader. A connected input is discarded
  // for the all-constant case, since none of its channels are used.
  bool all_constants = true;
  for (size_t i = 0; i < dest_count; ++i) {
    const char channel = i < channels.size() ? channels[i] : '\0';
    if (!MtlxChannelConstant(channel, nullptr)) {
      all_constants = false;
      break;
    }
  }
  const bool has_connection =
      !port->AttrOr("nodename").empty() || !port->AttrOr("nodegraph").empty() ||
      !port->AttrOr("interfacename").empty();
  if (all_constants || !has_connection) {
    const std::string constant_name =
        UniqueMtlxChildName(*scope, *generated, "swizzle");
    MtlxElem constant;
    constant.tag = "constant";
    constant.SetAttr("name", constant_name);
    constant.SetAttr("type", dest_type);
    MtlxElem value;
    value.tag = "input";
    value.SetAttr("name", "value");
    value.SetAttr("type", dest_type);
    value.SetAttr("value", SwizzledConstantValue(*port, channels, dest_count));
    constant.children.push_back(std::move(value));
    generated->push_back(std::move(constant));
    RewireToGeneratedNode(port, constant_name);
    return;
  }

  if (dest_count == 1) {
    const int index = MtlxChannelIndex(channels[0]);
    if (index < 0) return;
    const std::string extract_name =
        UniqueMtlxChildName(*scope, *generated, "swizzle");
    MtlxElem extract;
    extract.tag = "extract";
    extract.SetAttr("name", extract_name);
    extract.SetAttr("type", dest_type);
    MtlxElem extract_input = *port;
    extract_input.tag = "input";
    extract_input.SetAttr("name", "in");
    extract_input.SetAttr("type", source_type);
    EraseMtlxAttr(&extract_input, "channels");
    extract.children.push_back(std::move(extract_input));
    MtlxElem index_input;
    index_input.tag = "input";
    index_input.SetAttr("name", "index");
    index_input.SetAttr("type", "integer");
    index_input.SetAttr("value", std::to_string(index));
    extract.children.push_back(std::move(index_input));
    generated->push_back(std::move(extract));
    RewireToGeneratedNode(port, extract_name);
    return;
  }

  const std::string combine_name =
      UniqueMtlxChildName(*scope, *generated, "swizzle");
  MtlxElem combine;
  combine.tag = "combine" + std::to_string(dest_count);
  combine.SetAttr("name", combine_name);
  combine.SetAttr("type", dest_type);
  std::string separate_name;
  if (source_count > 1) {
    separate_name = UniqueMtlxChildName(*scope, *generated, "separate");
    MtlxElem separate;
    separate.tag = "separate" + std::to_string(source_count);
    separate.SetAttr("name", separate_name);
    separate.SetAttr("type", "multioutput");
    MtlxElem separate_input = *port;
    separate_input.tag = "input";
    separate_input.SetAttr("name", "in");
    separate_input.SetAttr("type", source_type);
    EraseMtlxAttr(&separate_input, "channels");
    separate.children.push_back(std::move(separate_input));
    generated->push_back(std::move(separate));
  }
  for (size_t i = 0; i < dest_count; ++i) {
    const char channel = i < channels.size() ? channels[i] : channels[0];
    MtlxElem input;
    input.tag = "input";
    input.SetAttr("name", "in" + std::to_string(i + 1));
    input.SetAttr("type", "float");
    const char *constant = nullptr;
    if (MtlxChannelConstant(channel, &constant)) {
      input.SetAttr("value", constant);
    } else if (source_count == 1) {
      MtlxElem scalar_input = *port;
      scalar_input.tag = "input";
      scalar_input.SetAttr("name", input.Name());
      scalar_input.SetAttr("type", "float");
      EraseMtlxAttr(&scalar_input, "channels");
      input = std::move(scalar_input);
    } else {
      const int channel_index = MtlxChannelIndex(channel);
      const char output_channel = channel_index < 0
          ? (source_type == "color3" || source_type == "color4" ? 'r' : 'x')
          : channel;
      input.SetAttr("nodename", separate_name);
      input.SetAttr("output", std::string("out") + output_channel);
    }
    combine.children.push_back(std::move(input));
  }
  generated->push_back(std::move(combine));
  RewireToGeneratedNode(port, combine_name);
}

void UpgradeMtlxChannels(MtlxElem *scope) {
  if (!scope) return;
  std::vector<MtlxElem> generated;
  for (MtlxElem &child : scope->children) {
    if (child.tag == "input" || child.tag == "output")
      UpgradeChannelPort(&child, scope, &generated);
    if (IsMtlxNodeElem(child)) {
      for (MtlxElem &port : child.children)
        if (port.tag == "input" || port.tag == "output")
          UpgradeChannelPort(&port, scope, &generated);
    }
  }
  for (MtlxElem &node : generated) scope->children.push_back(std::move(node));
  for (MtlxElem &child : scope->children)
    if (child.tag == "nodegraph" || child.tag == "material")
      UpgradeMtlxChannels(&child);
}

}  // namespace

void UpgradeMtlxVersion(MtlxElem *root) {
  if (!root || root->tag != "materialx") return;
  uint64_t major = 0, minor = 0;
  if (!ParseMtlxVersion(root->AttrOr("version"), &major, &minor) ||
      major != 1 || (minor != 37 && minor != 38)) {
    return;
  }
  if (minor == 37) {
    UpgradeColor2ChannelAliases(root);
    UpgradeMtlxElements(root, 37);
    minor = 38;
  }
  if (minor == 38) {
    UpgradeMtlxElements(root, 38);
    UpgradeMtlxChannels(root);
    root->SetAttr("version", "1.39");
  }
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

const std::vector<MtlxElem> &StandardMtlxNodeDefs() {
  static const std::vector<MtlxElem> defs = [] {
    std::vector<MtlxElem> result;
#define MTLX_STD_NODEDEF(name, node, output_type, first_input, input_count) \
    do {                                                                      \
      MtlxElem def;                                                           \
      def.tag = "nodedef";                                                   \
      def.SetAttr("name", name);                                             \
      def.SetAttr("node", node);                                             \
      def.SetAttr("type", output_type);                                      \
      MtlxElem output;                                                        \
      output.tag = "output";                                                 \
      output.SetAttr("name", "out");                                        \
      output.SetAttr("type", output_type);                                   \
      def.children.push_back(std::move(output));                             \
      result.push_back(std::move(def));                                       \
    } while (false);
#define MTLX_STD_INPUT(name, type, has_default, default_value)                \
    do {                                                                      \
      MtlxElem input;                                                         \
      input.tag = "input";                                                   \
      input.SetAttr("name", name);                                           \
      input.SetAttr("type", type);                                           \
      if (has_default)                                                       \
        input.SetAttr("value", default_value);                              \
      result.back().children.push_back(std::move(input));                     \
    } while (false);
#include "../external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
#undef MTLX_STD_INPUT
#undef MTLX_STD_NODEDEF
    return result;
  }();
  return defs;
}

void ApplyMtlxNodeDefDefaults(MtlxElem *root) {
  if (!root) return;
  MtlxLibrary library;
  library.AddFrom(*root);
  for (const MtlxElem &def : StandardMtlxNodeDefs())
    library.AddNodeDef(def, {});
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children)
    ApplyMtlxNodeDefDefaults(&child, library, outer);
}

void InheritMtlxAttributes(MtlxElem *elem, const MtlxScope &outer) {
  if (!elem) return;
  static const char *const kScopedAttributes[] = {
      "colorspace", "fileprefix", "geomprefix", "namespace"};
  for (const char *key : kScopedAttributes) {
    if (elem->Attr(key)) continue;
    std::string value;
    if (ScopedMtlxAttr(key, outer, &value)) elem->SetAttr(key, value);
  }
  MtlxScope child_scope;
  child_scope.reserve(outer.size() + 1);
  child_scope.push_back(elem);
  child_scope.insert(child_scope.end(), outer.begin(), outer.end());
  for (MtlxElem &child : elem->children)
    InheritMtlxAttributes(&child, child_scope);
}

void InheritMtlxAttributes(MtlxElem *root) {
  if (!root) return;
  const MtlxScope outer = {root};
  for (MtlxElem &child : root->children)
    InheritMtlxAttributes(&child, outer);
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
  UpgradeMtlxVersion(&root);
  InheritMtlxAttributes(&root);
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

  UpgradeMtlxVersion(&root);
  InheritMtlxAttributes(&root);
  ApplyMtlxNodeDefDefaults(&root);
  return ParseRoot(MtlxElemToXMLNode(root));
}

bool MtlxDocument::ParseRoot(XMLNodePtr root) {
  if (!root || root->GetName() != "materialx") {
    error_ = "Invalid MaterialX document";
    return false;
  }

  parsed_root_ = root;
  nodes_.clear();
  nodegraphs_.clear();
  materials_.clear();

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

bool MtlxDocument::ResolveNodeGraphs(
    std::vector<std::string> *graph_order, std::string *err) const {
  const MtlxLibrary empty_library;
  return ResolveNodeGraphs(empty_library, graph_order, err);
}

bool MtlxDocument::ResolveNodeGraphs(
    const MtlxLibrary &external_library,
    std::vector<std::string> *graph_order, std::string *err) const {
  if (!parsed_root_) {
    if (err) *err = "MaterialX document has not been parsed";
    return false;
  }
  MtlxElem root = XMLNodeToMtlxElem(parsed_root_);
  MtlxLibrary library;
  library.AddFrom(root);
  library.AddLibrary(external_library);
  for (const MtlxElem &def : StandardMtlxNodeDefs())
    library.AddNodeDef(def, {});
  return ResolveMtlxGraphs(root, library, graph_order, err);
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

std::string MtlxDocument::GetResolvedXML() const {
  if (!parsed_root_) return std::string();
  std::ostringstream out;
  out << "<?xml version=\"1.0\"?>";
  AppendXmlNode(&out, parsed_root_);
  return out.str();
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
