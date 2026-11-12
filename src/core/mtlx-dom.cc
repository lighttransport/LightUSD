// SPDX-License-Identifier: Apache 2.0

#include "core/native-io.hh"
#include "core/mtlx-dom.hh"
#include "core/mtlx-resolve.hh"
#include "core/mtlx-upgrade.hh"
#include "core/security-policy.hh"
#include "resolver/asset-resolver.hh"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>

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
  auto unique_numbered_name = [&](const std::string &base_name) {
    std::string candidate = base_name;
    for (size_t suffix = 2;; ++suffix) {
      const bool exists = parent &&
          std::any_of(parent->children.begin(), parent->children.end(),
                      [&](const MtlxElem &child) {
                        return child.Name() == candidate;
                      });
      if (!exists) return candidate;
      candidate = base_name + std::to_string(suffix);
    }
  };

  for (size_t i = 0; i < original_child_count; ++i)
    UpgradeMtlxElements(&elem->children[i], minor_version, elem);

  if (minor_version == 36 && IsMtlxNodeElem(*elem)) {
    const std::string type = elem->AttrOr("type");
    const std::string old_category = elem->tag;
    if (old_category == "invert") {
      if (type == "matrix33" || type == "matrix44") {
        elem->tag = "invertmatrix";
      }
    } else if (old_category == "rotate") {
      if (type == "vector2") elem->tag = "rotate2d";
      else if (type == "vector3") elem->tag = "rotate3d";
    } else if (old_category == "compare") {
      elem->tag = "ifgreatereq";
      for (MtlxElem &child : elem->children) {
        if (child.AttrOr("name") == "intest")
          child.SetAttr("name", "value1");
        else if (child.AttrOr("name") == "cutoff")
          child.SetAttr("name", "value2");
        else if (child.AttrOr("name") == "in1")
          child.SetAttr("name", "__upgrade_compare_in1");
      }
      for (MtlxElem &child : elem->children)
        if (child.AttrOr("name") == "in2") child.SetAttr("name", "in1");
      for (MtlxElem &child : elem->children)
        if (child.AttrOr("name") == "__upgrade_compare_in1")
          child.SetAttr("name", "in2");
    } else if (old_category == "transformpoint" ||
               old_category == "transformvector" ||
               old_category == "transformnormal") {
      const bool has_matrix = std::any_of(
          elem->children.begin(), elem->children.end(),
          [](const MtlxElem &child) {
            return child.AttrOr("name") == "mat";
          });
      if (has_matrix) elem->tag = "transformmatrix";
    } else if (old_category == "combine") {
      size_t count = 0;
      if (type == "vector2" || type == "color2") count = 2;
      else if (type == "vector3" || type == "color3") count = 3;
      else if (type == "vector4" || type == "color4") count = 4;
      if (count) elem->tag = "combine" + std::to_string(count);
    } else if (old_category == "separate") {
      size_t count = 0;
      for (const MtlxElem &child : elem->children) {
        if (child.tag != "input" || child.AttrOr("name") != "in") continue;
        const std::string source_type = child.AttrOr("type");
        if (source_type == "vector2" || source_type == "color2") count = 2;
        else if (source_type == "vector3" || source_type == "color3") count = 3;
        else if (source_type == "vector4" || source_type == "color4") count = 4;
        break;
      }
      if (count) elem->tag = "separate" + std::to_string(count);
    } else if (old_category == "geomattrvalue") {
      elem->tag = "geompropvalue";
      if (const std::string *attrname = elem->Attr("attrname")) {
        const std::string value = *attrname;
        elem->attrs.erase(
            std::remove_if(elem->attrs.begin(), elem->attrs.end(),
                           [](const auto &attr) {
                             return attr.first == "attrname";
                           }),
            elem->attrs.end());
        elem->SetAttr("geomprop", value);
      }
    }
  }

  if (minor_version == 37 && IsMtlxNodeElem(*elem)) {
    for (MtlxElem &child : elem->children)
      if (child.tag == "parameter") child.tag = "input";
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
    } else if (elem->tag == "normalmap") {
      bool object_space = false;
      for (const MtlxElem &child : elem->children) {
        if (child.tag == "input" && child.AttrOr("name") == "space" &&
            child.AttrOr("value") == "object") {
          object_space = true;
          break;
        }
      }
      if (object_space) {
        const std::string multiply_name = unique_name("multiply");
        const std::string subtract_name = unique_name("subtract");

        MtlxElem multiply;
        multiply.tag = "multiply";
        multiply.SetAttr("name", multiply_name);
        multiply.SetAttr("type", "vector3");
        for (const MtlxElem &child : elem->children) {
          if (child.tag != "input" || child.AttrOr("name") != "in") continue;
          MtlxElem multiply_input = child;
          multiply_input.SetAttr("name", "in1");
          multiply.children.push_back(std::move(multiply_input));
          break;
        }
        MtlxElem multiply_factor;
        multiply_factor.tag = "input";
        multiply_factor.SetAttr("name", "in2");
        multiply_factor.SetAttr("type", "float");
        multiply_factor.SetAttr("value", "2");
        multiply.children.push_back(std::move(multiply_factor));

        MtlxElem subtract;
        subtract.tag = "subtract";
        subtract.SetAttr("name", subtract_name);
        subtract.SetAttr("type", "vector3");
        subtract.children.push_back(
            make_input("in1", "vector3", multiply_name));
        MtlxElem subtract_offset;
        subtract_offset.tag = "input";
        subtract_offset.SetAttr("name", "in2");
        subtract_offset.SetAttr("type", "float");
        subtract_offset.SetAttr("value", "1");
        subtract.children.push_back(std::move(subtract_offset));

        elem->children.erase(
            std::remove_if(elem->children.begin(), elem->children.end(),
                           [](const MtlxElem &child) {
                             return child.tag == "input";
                           }),
            elem->children.end());
        elem->tag = "normalize";
        if (elem->Attr("nodedef"))
          elem->SetAttr("nodedef", "ND_normalize_vector3");
        elem->children.push_back(make_input("in", "vector3", subtract_name));
        generated.push_back(std::move(multiply));
        generated.push_back(std::move(subtract));
      } else {
        elem->children.erase(
            std::remove_if(elem->children.begin(), elem->children.end(),
                           [](const MtlxElem &child) {
                             return child.tag == "input" &&
                                    child.AttrOr("name") == "space";
                           }),
            elem->children.end());
        auto has_input = [&](const char *input_name) {
          return std::any_of(elem->children.begin(), elem->children.end(),
                             [&](const MtlxElem &child) {
                               return child.tag == "input" &&
                                      child.AttrOr("name") == input_name;
                             });
        };
        const bool has_normal = has_input("normal");
        const bool has_tangent = has_input("tangent");
        const bool has_bitangent = has_input("bitangent");
        if ((has_normal || has_tangent) && !has_bitangent) {
          const std::string cross_name =
              unique_numbered_name("normalmap_cross");
          const std::string normalize_name =
              unique_numbered_name("normalmap_cross_norm");
          MtlxElem cross;
          cross.tag = "crossproduct";
          cross.SetAttr("name", cross_name);
          cross.SetAttr("type", "vector3");
          std::vector<MtlxElem> geom_nodes;
          auto copy_or_geom = [&](const char *source_name,
                                  const char *dest_name,
                                  const char *geom_category) {
            for (const MtlxElem &child : elem->children) {
              if (child.tag != "input" ||
                  child.AttrOr("name") != source_name) continue;
              MtlxElem input = child;
              input.SetAttr("name", dest_name);
              cross.children.push_back(std::move(input));
              return;
            }
            const std::string geom_name =
                unique_numbered_name(std::string("normalmap_") + geom_category);
            MtlxElem geom;
            geom.tag = geom_category;
            geom.SetAttr("name", geom_name);
            geom.SetAttr("type", "vector3");
            MtlxElem space;
            space.tag = "input";
            space.SetAttr("name", "space");
            space.SetAttr("type", "string");
            space.SetAttr("value", "world");
            geom.children.push_back(std::move(space));
            geom_nodes.push_back(std::move(geom));
            cross.children.push_back(make_input(dest_name, "vector3", geom_name));
          };
          copy_or_geom("normal", "in1", "normal");
          copy_or_geom("tangent", "in2", "tangent");
          generated.push_back(std::move(cross));
          for (MtlxElem &geom : geom_nodes)
            generated.push_back(std::move(geom));

          MtlxElem normalize;
          normalize.tag = "normalize";
          normalize.SetAttr("name", normalize_name);
          normalize.SetAttr("type", "vector3");
          normalize.children.push_back(make_input("in", "vector3", cross_name));
          generated.push_back(std::move(normalize));
          elem->children.push_back(
              make_input("bitangent", "vector3", normalize_name));
        }
        if (elem->AttrOr("nodedef") == "ND_normalmap")
          elem->SetAttr("nodedef", "ND_normalmap_float");
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

std::string UniqueMtlxNumberedChildName(
    const MtlxElem &scope, const std::vector<MtlxElem> &generated,
    const std::string &base) {
  for (size_t suffix = 1;; ++suffix) {
    const std::string candidate =
        suffix == 1 ? base : base + std::to_string(suffix);
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

  static const std::pair<const char *, size_t> kConvertPatterns[] = {
      {"rgb", 3}, {"rgb", 4}, {"rgba", 4}, {"xyz", 3},
      {"xyz", 4}, {"xyzw", 4}, {"rr", 1}, {"rrr", 1},
      {"xx", 1}, {"xxx", 1}};
  bool direct_convert = false;
  for (const auto &pattern : kConvertPatterns)
    if (channels == pattern.first && source_count == pattern.second)
      direct_convert = true;
  if (direct_convert) {
    const std::string convert_name =
        UniqueMtlxChildName(*scope, *generated, "swizzle");
    MtlxElem convert;
    convert.tag = "convert";
    convert.SetAttr("name", convert_name);
    convert.SetAttr("type", dest_type);
    MtlxElem convert_input = *port;
    convert_input.tag = "input";
    convert_input.SetAttr("name", "in");
    convert_input.SetAttr("type", source_type);
    EraseMtlxAttr(&convert_input, "channels");
    convert.children.push_back(std::move(convert_input));
    generated->push_back(std::move(convert));
    RewireToGeneratedNode(port, convert_name);
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

void UpgradeMtlxSwizzleNode(MtlxElem *node, MtlxElem *scope,
                            std::vector<MtlxElem> *generated) {
  if (!node || !scope || !generated || node->tag != "swizzle") return;
  MtlxElem *source = nullptr;
  std::string channels;
  for (MtlxElem &child : node->children) {
    if (child.tag != "input") continue;
    if (child.AttrOr("name") == "in") source = &child;
    else if (child.AttrOr("name") == "channels")
      channels = child.AttrOr("value");
  }
  if (!source || channels.empty()) return;

  const std::string dest_type = node->AttrOr("type");
  const size_t dest_count = MtlxChannelCount(dest_type);
  if (dest_count == 0) return;
  bool all_constants = true;
  for (size_t i = 0; i < dest_count; ++i) {
    const char channel = i < channels.size() ? channels[i] : '\0';
    if (!MtlxChannelConstant(channel, nullptr)) {
      all_constants = false;
      break;
    }
  }
  const bool has_connection = !source->AttrOr("nodename").empty() ||
                              !source->AttrOr("nodegraph").empty() ||
                              !source->AttrOr("interfacename").empty();
  if (all_constants || !has_connection) {
    MtlxElem value;
    value.tag = "input";
    value.SetAttr("name", "value");
    value.SetAttr("type", dest_type);
    value.SetAttr("value", SwizzledConstantValue(*source, channels, dest_count));
    node->tag = "constant";
    EraseMtlxAttr(node, "nodedef");
    node->children.assign(1, std::move(value));
    return;
  }

  std::string source_type = ConnectedNodeType(*scope, *source);
  if (source_type.empty() || source_type == "multioutput")
    source_type = source->AttrOr("type");
  if (source_type.empty()) source_type = InferChannelSourceType(channels, dest_type);
  const size_t source_count = MtlxChannelCount(source_type);
  if (source_count == 0) return;

  MtlxElem source_input = *source;
  EraseMtlxAttr(&source_input, "channels");
  if (dest_count == 1) {
    const int index = MtlxChannelIndex(channels[0]);
    if (index < 0 || source_count == 1) return;
    node->tag = "extract";
    EraseMtlxAttr(node, "nodedef");
    source_input.SetAttr("name", "in");
    source_input.SetAttr("type", source_type);
    MtlxElem index_input;
    index_input.tag = "input";
    index_input.SetAttr("name", "index");
    index_input.SetAttr("type", "integer");
    index_input.SetAttr("value", std::to_string(index));
    node->children.clear();
    node->children.push_back(std::move(source_input));
    node->children.push_back(std::move(index_input));
    return;
  }

  std::string separate_name;
  if (source_count > 1) {
    separate_name =
        UniqueMtlxNumberedChildName(*scope, *generated, "separate");
    MtlxElem separate;
    separate.tag = "separate" + std::to_string(source_count);
    separate.SetAttr("name", separate_name);
    separate.SetAttr("type", "multioutput");
    source_input.SetAttr("name", "in");
    source_input.SetAttr("type", source_type);
    separate.children.push_back(std::move(source_input));
    generated->push_back(std::move(separate));
  }

  std::vector<MtlxElem> inputs;
  for (size_t i = 0; i < dest_count; ++i) {
    const char channel = i < channels.size() ? channels[i] : channels[0];
    MtlxElem input;
    input.tag = "input";
    input.SetAttr("name", "in" + std::to_string(i + 1));
    input.SetAttr("type", "float");
    const char *constant = nullptr;
    if (MtlxChannelConstant(channel, &constant)) {
      input.SetAttr("value", channel == '0' ? "0" : "1");
    } else if (source_count == 1) {
      input = *source;
      EraseMtlxAttr(&input, "channels");
      input.SetAttr("name", "in" + std::to_string(i + 1));
      input.SetAttr("type", "float");
    } else {
      const int index = MtlxChannelIndex(channel);
      const char *color_channels = "rgba";
      const char *vector_channels = "xyzw";
      const bool is_color = source_type == "color3" || source_type == "color4";
      const char output_channel = index < 0
          ? (is_color ? 'r' : 'x')
          : (is_color ? color_channels[index] : vector_channels[index]);
      input.SetAttr("nodename", separate_name);
      input.SetAttr("output", std::string("out") + output_channel);
    }
    inputs.push_back(std::move(input));
  }
  node->tag = "combine" + std::to_string(dest_count);
  EraseMtlxAttr(node, "nodedef");
  node->children = std::move(inputs);
}

void UpgradeMtlxChannels(MtlxElem *scope) {
  if (!scope) return;
  for (size_t child_index = 0; child_index < scope->children.size(); ++child_index) {
    std::vector<MtlxElem> generated;
    MtlxElem &child = scope->children[child_index];
    if (child.tag == "input" || child.tag == "output")
      UpgradeChannelPort(&child, scope, &generated);
    if (IsMtlxNodeElem(child)) {
      if (child.tag == "swizzle")
        UpgradeMtlxSwizzleNode(&child, scope, &generated);
      for (MtlxElem &port : child.children)
        if (port.tag == "input" || port.tag == "output")
          UpgradeChannelPort(&port, scope, &generated);
    }
    for (size_t i = generated.size(); i > 0; --i)
      scope->children.insert(scope->children.begin() +
                                 static_cast<std::ptrdiff_t>(child_index),
                             std::move(generated[i - 1]));
    child_index += generated.size();
  }
  for (MtlxElem &child : scope->children)
    if (child.tag == "nodegraph" || child.tag == "material")
      UpgradeMtlxChannels(&child);
}

void UpgradeMtlxLegacyOpgraphs(MtlxElem *root) {
  if (!root || root->tag != "materialx") return;
  for (MtlxElem &graph : root->children) {
    if (graph.tag != "opgraph") continue;
    graph.tag = "nodegraph";
    for (MtlxElem &child : graph.children) {
      if (child.tag == "output") {
        for (const MtlxElem &parameter : child.children) {
          if (parameter.tag != "parameter" ||
              parameter.AttrOr("name") != "in")
            continue;
          const std::string node_name = parameter.AttrOr("value");
          if (!node_name.empty()) child.SetAttr("nodename", node_name);
          break;
        }
        child.children.clear();
        continue;
      }
      if (!IsMtlxNodeElem(child)) continue;
      std::vector<MtlxElem> upgraded_inputs;
      for (MtlxElem &parameter : child.children) {
        if (parameter.tag != "parameter") {
          upgraded_inputs.push_back(std::move(parameter));
          continue;
        }
        parameter.tag = "input";
        if (child.tag == "constant" && parameter.AttrOr("name") == "color")
          parameter.SetAttr("name", "value");
        EraseMtlxAttr(&parameter, "publicname");
        upgraded_inputs.push_back(std::move(parameter));
      }
      child.children = std::move(upgraded_inputs);
    }
  }
}

void UpgradeMtlxLegacyShaderFormat(MtlxElem *root) {
  if (!root || root->tag != "materialx") return;
  std::vector<std::pair<std::string, MtlxElem>> shaders;
  for (const MtlxElem &child : root->children) {
    if (child.tag != "shader") continue;
    const std::string shader_name = child.Name();
    const std::string category = child.AttrOr(
        "shaderprogram", child.AttrOr("shadername", shader_name));
    const std::string shader_type = child.AttrOr("shadertype");

    MtlxElem node = child;
    node.tag = category;
    EraseMtlxAttr(&node, "shaderprogram");
    EraseMtlxAttr(&node, "shadername");
    EraseMtlxAttr(&node, "shadertype");
    node.SetAttr("type", shader_type == "surface" ? "surfaceshader" :
                          shader_type == "displacement" ? "displacementshader" :
                          shader_type == "volume" ? "volumeshader" : shader_type);
    for (MtlxElem &input : node.children) {
      if (input.tag != "input") continue;
      const std::string graph_output = input.AttrOr("graphoutput");
      if (!graph_output.empty()) {
        input.SetAttr("output", graph_output);
        EraseMtlxAttr(&input, "graphoutput");
      }
      const std::string graph_name = input.AttrOr("graphname");
      if (!graph_name.empty()) {
        input.SetAttr("nodegraph", graph_name);
        EraseMtlxAttr(&input, "graphname");
      }
    }
    shaders.emplace_back(shader_name, std::move(node));
  }

  std::vector<std::pair<std::string, MtlxElem>> materials;
  for (const MtlxElem &material : root->children) {
    if (material.tag != "material") continue;
    MtlxElem surface_material;
    surface_material.tag = "surfacematerial";
    surface_material.SetAttr("name", material.Name());
    surface_material.SetAttr("type", "material");

    for (const MtlxElem &child : material.children) {
      if (child.tag == "override") {
        for (const MtlxElem &shader_ref : material.children) {
          if (shader_ref.tag != "shaderref") continue;
          const std::string shader_name = shader_ref.AttrOr("name");
          auto shader = std::find_if(
              shaders.begin(), shaders.end(), [&](const auto &item) {
                return item.first == shader_name;
              });
          if (shader == shaders.end()) continue;
          const std::string public_name = child.Name();
          for (MtlxElem &input : shader->second.children) {
            if (input.tag != "input") continue;
            if (input.AttrOr("publicname") != public_name &&
                input.Name() != public_name)
              continue;
            for (const auto &attr : child.attrs)
              if (attr.first != "name") input.SetAttr(attr.first, attr.second);
          }
        }
        continue;
      }
      if (child.tag != "shaderref") continue;
      const std::string shader_name = child.AttrOr("name");
      auto shader = std::find_if(
          shaders.begin(), shaders.end(), [&](const auto &item) {
            return item.first == shader_name;
          });
      const std::string shader_type = shader == shaders.end()
          ? "surfaceshader" : shader->second.AttrOr("type", "surfaceshader");
      MtlxElem input;
      input.tag = "input";
      input.SetAttr("name", shader_type);
      input.SetAttr("type", shader_type);
      input.SetAttr("nodename", shader_name);
      surface_material.children.push_back(std::move(input));
    }
    materials.emplace_back(material.Name(), std::move(surface_material));
  }

  std::vector<MtlxElem> upgraded;
  for (const MtlxElem &child : root->children) {
    if (child.tag == "shader") {
      auto shader = std::find_if(
          shaders.begin(), shaders.end(), [&](const auto &item) {
            return item.first == child.Name();
          });
      if (shader != shaders.end()) {
        for (MtlxElem &input : shader->second.children)
          if (input.tag == "input") EraseMtlxAttr(&input, "publicname");
        upgraded.push_back(shader->second);
      }
    } else if (child.tag == "material") {
      auto material = std::find_if(
          materials.begin(), materials.end(), [&](const auto &item) {
            return item.first == child.Name();
          });
      if (material != materials.end()) upgraded.push_back(material->second);
    } else {
      upgraded.push_back(child);
    }
  }
  root->children = std::move(upgraded);
}

void UpgradeMtlxShaderRefs(MtlxElem *root) {
  if (!root || root->tag != "materialx") return;
  std::vector<MtlxElem> upgraded;
  std::vector<MtlxElem> migrated_materials;
  for (MtlxElem &child : root->children) {
    if (child.tag != "material") {
      upgraded.push_back(child);
      continue;
    }

    std::vector<MtlxElem> shader_inputs;
    std::vector<MtlxElem> shader_nodes;
    for (const MtlxElem &shader_ref : child.children) {
      if (shader_ref.tag != "shaderref") continue;
      const std::string category = shader_ref.AttrOr("node");
      const std::string shader_name = shader_ref.Name();
      const MtlxElem *nodedef = nullptr;
      for (const MtlxElem &candidate : root->children) {
        if (candidate.tag == "nodedef" &&
            candidate.AttrOr("node") == category) {
          nodedef = &candidate;
          break;
        }
      }

      std::string shader_type = nodedef ? nodedef->AttrOr("type") : std::string();
      if (shader_type.empty() && nodedef) {
        for (const MtlxElem &output : nodedef->children) {
          if (output.tag == "output") {
            shader_type = output.AttrOr("type");
            if (!shader_type.empty()) break;
          }
        }
      }
      if (shader_type.empty()) shader_type = "surfaceshader";

      MtlxElem shader;
      shader.tag = category;
      for (const auto &attr : shader_ref.attrs)
        if (attr.first != "node" && attr.first != "name" &&
            attr.first != "type" && attr.first != "nodedef")
          shader.SetAttr(attr.first, attr.second);
      shader.SetAttr("name", shader_name);
      shader.SetAttr("type", shader_type);
      if (nodedef) shader.SetAttr("nodedef", nodedef->Name());
      for (const MtlxElem &binding : shader_ref.children) {
        if (binding.tag != "bindinput") continue;
        MtlxElem input = binding;
        input.tag = "input";
        shader.children.push_back(std::move(input));
      }
      shader_nodes.push_back(std::move(shader));

      MtlxElem input;
      input.tag = "input";
      input.SetAttr("name", shader_type);
      input.SetAttr("type", shader_type);
      input.SetAttr("nodename", shader_name);
      shader_inputs.push_back(std::move(input));
    }

    if (shader_nodes.empty()) {
      upgraded.push_back(child);
      continue;
    }
    for (MtlxElem &node : shader_nodes)
      migrated_materials.push_back(std::move(node));
    child.tag = "surfacematerial";
    child.SetAttr("type", "material");
    child.children = std::move(shader_inputs);
    migrated_materials.push_back(std::move(child));
  }
  for (MtlxElem &elem : migrated_materials)
    upgraded.push_back(std::move(elem));
  root->children = std::move(upgraded);
}

void UpgradeMtlxThinFilmLayers(MtlxElem *scope) {
  if (!scope) return;
  std::vector<std::string> removed_nodes;
  std::vector<std::string> layer_names;
  for (const MtlxElem &child : scope->children)
    if (child.tag == "layer") layer_names.push_back(child.Name());

  static const char *const kThinFilmBsdfs[] = {
      "dielectric_bsdf", "conductor_bsdf", "generalized_schlick_bsdf"};
  auto find_node = [&](const std::string &name) -> MtlxElem * {
    for (MtlxElem &child : scope->children)
      if (IsMtlxNodeElem(child) && child.Name() == name) return &child;
    return nullptr;
  };
  auto find_input = [](MtlxElem *node, const char *name) -> MtlxElem * {
    if (!node) return nullptr;
    for (MtlxElem &child : node->children)
      if (child.tag == "input" && child.AttrOr("name") == name) return &child;
    return nullptr;
  };

  for (const std::string &layer_name : layer_names) {
    MtlxElem *layer = find_node(layer_name);
    MtlxElem *top_input = find_input(layer, "top");
    MtlxElem *base_input = find_input(layer, "base");
    if (!top_input || !base_input) continue;
    const std::string top_name = top_input->AttrOr("nodename");
    const std::string base_name = base_input->AttrOr("nodename");
    MtlxElem *top = find_node(top_name);
    if (!top || top->tag != "thin_film_bsdf" || base_name.empty()) continue;
    const MtlxElem top_copy = *top;

    std::vector<std::string> pending{base_name};
    std::vector<std::string> visited;
    while (!pending.empty()) {
      const std::string name = pending.back();
      pending.pop_back();
      if (std::find(visited.begin(), visited.end(), name) != visited.end())
        continue;
      visited.push_back(name);
      MtlxElem *node = find_node(name);
      if (!node) continue;

      bool supported_bsdf = false;
      for (const char *category : kThinFilmBsdfs)
        if (node->tag == category) supported_bsdf = true;
      MtlxElem *scatter_mode = find_input(node, "scatter_mode");
      if (supported_bsdf &&
          (!scatter_mode || scatter_mode->AttrOr("value") != "T")) {
        for (const auto &binding :
             {std::pair<const char *, const char *>{"thickness", "thinfilm_thickness"},
              {"ior", "thinfilm_ior"}}) {
          const MtlxElem *source = nullptr;
          for (const MtlxElem &child : top_copy.children)
            if (child.tag == "input" &&
                child.AttrOr("name") == binding.first) {
              source = &child;
              break;
            }
          if (!source) continue;
          MtlxElem replacement = *source;
          replacement.SetAttr("name", binding.second);
          if (MtlxElem *destination = find_input(node, binding.second))
            *destination = std::move(replacement);
          else
            node->children.push_back(std::move(replacement));
        }
      }
      for (const MtlxElem &child : node->children) {
        if ((child.tag == "input" || child.tag == "output") &&
            !child.AttrOr("nodename").empty())
          pending.push_back(child.AttrOr("nodename"));
      }
    }

    for (MtlxElem &child : scope->children) {
      if (IsMtlxNodeElem(child)) {
        for (MtlxElem &port : child.children)
          if ((port.tag == "input" || port.tag == "output") &&
              port.AttrOr("nodename") == layer_name)
            port.SetAttr("nodename", base_name);
      } else if (child.tag == "output" &&
                 child.AttrOr("nodename") == layer_name) {
        child.SetAttr("nodename", base_name);
      }
    }
    removed_nodes.push_back(layer_name);
    removed_nodes.push_back(top_name);
  }

  scope->children.erase(
      std::remove_if(scope->children.begin(), scope->children.end(),
                     [&](const MtlxElem &child) {
                       return std::find(removed_nodes.begin(),
                                        removed_nodes.end(), child.Name()) !=
                              removed_nodes.end();
                     }),
      scope->children.end());
  for (MtlxElem &child : scope->children)
    if (child.tag == "nodegraph" || child.tag == "material")
      UpgradeMtlxThinFilmLayers(&child);
}

}  // namespace

void UpgradeMtlxVersionImpl(MtlxElem *root) {
  if (!root || root->tag != "materialx") return;
  uint64_t major = 0, minor = 0;
  if (!ParseMtlxVersion(root->AttrOr("version"), &major, &minor) ||
      major != 1 || (minor != 22 && minor != 25 && minor != 36 && minor != 37 &&
                     minor != 38)) {
    return;
  }
  if (minor == 22) {
    UpgradeMtlxLegacyOpgraphs(root);
    UpgradeMtlxLegacyShaderFormat(root);
    minor = 36;
  }
  if (minor == 25) {
    UpgradeMtlxLegacyShaderFormat(root);
    minor = 36;
  }
  if (minor == 36) {
    UpgradeMtlxElements(root, 36);
    minor = 37;
  }
  if (minor == 37) {
    UpgradeColor2ChannelAliases(root);
    UpgradeMtlxElements(root, 37);
    UpgradeMtlxShaderRefs(root);
    minor = 38;
  }
  if (minor == 38) {
    UpgradeMtlxElements(root, 38);
    UpgradeMtlxShaderRefs(root);
    UpgradeMtlxChannels(root);
    UpgradeMtlxThinFilmLayers(root);
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

using MtlxUnitScales = std::map<std::string, float>;

bool ParseFiniteFloat(const std::string &text, float *value) {
  if (!value || text.empty()) return false;
  char *end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(parsed))
    return false;
  *value = parsed;
  return true;
}

void CollectMtlxUnitMetadata(const MtlxElem &elem,
                            const std::string &requested_type,
                            std::set<std::string> *unit_types,
                            std::map<std::string, MtlxUnitScales> *scales,
                            std::string *error) {
  if (elem.tag == "unittypedef") {
    const std::string name = elem.Name();
    if (!name.empty()) unit_types->insert(name);
  } else if (elem.tag == "unitdef") {
    const std::string type = elem.AttrOr("unittype");
    if (type == requested_type) {
      MtlxUnitScales &type_scales = (*scales)[type];
      for (const MtlxElem &unit : elem.children) {
        if (unit.tag != "unit") continue;
        const std::string name = unit.Name();
        if (name.empty()) continue;
        float scale = 1.0f;
        if (const std::string *authored_scale = unit.Attr("scale")) {
          if (!ParseFiniteFloat(*authored_scale, &scale)) {
            *error = "Invalid scale for MaterialX unit '" + name + "'";
            return;
          }
        }
        type_scales[name] = scale;
      }
    }
  }
  for (const MtlxElem &child : elem.children) {
    if (!error->empty()) return;
    CollectMtlxUnitMetadata(child, requested_type, unit_types, scales, error);
  }
}

bool IsMtlxStructuralElement(const std::string &tag) {
  static const char *const structural[] = {
      "materialx", "nodegraph", "nodedef", "typedef", "unittypedef",
      "unitdef", "targetdef", "attributedef", "geompropdef",
      "implementation", "look", "collection", "geominfo", "materialassign",
      "input", "output", "unit", "include"};
  for (const char *candidate : structural)
    if (tag == candidate) return true;
  return false;
}

std::string FormatMtlxFloat(float value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
  return out.str();
}

bool ConvertMtlxInput(MtlxElem *input, const std::string &unit_type,
                      const std::string &target_unit,
                      const MtlxUnitScales &scales, bool *converted,
                      std::string *error) {
  const std::string *authored_type = input->Attr("unittype");
  const std::string *authored_unit = input->Attr("unit");
  const std::string *value_text = input->Attr("value");
  const std::string type = input->AttrOr("type");
  if (!authored_type || *authored_type != unit_type || !authored_unit ||
      !value_text || (type != "float" && type != "vector2" &&
                      type != "vector3" && type != "vector4"))
    return true;

  MtlxTypedValue parsed;
  if (!ParseMtlxValue(type, *value_text, &parsed)) {
    *error = "Invalid value on MaterialX input '" + input->Name() + "'";
    return false;
  }

  float ratio = 1.0f;
  if (*authored_unit != target_unit) {
    const auto from = scales.find(*authored_unit);
    const auto to = scales.find(target_unit);
    if (from == scales.end()) {
      *error = "Unknown source MaterialX unit '" + *authored_unit + "'";
      return false;
    }
    if (to == scales.end()) {
      *error = "Unknown target MaterialX unit '" + target_unit + "'";
      return false;
    }
    if (to->second == 0.0f) {
      *error = "Target MaterialX unit has a zero scale: '" + target_unit + "'";
      return false;
    }
    ratio = from->second / to->second;
    if (!std::isfinite(ratio)) {
      *error = "Invalid conversion ratio for MaterialX units '" +
               *authored_unit + "' and '" + target_unit + "'";
      return false;
    }
  }

  std::vector<float> values;
  if (type == "float") {
    values.push_back(static_cast<float>(parsed.num));
  } else {
    values.reserve(parsed.nums.size());
    for (double value : parsed.nums) values.push_back(static_cast<float>(value));
  }
  for (float &value : values) {
    value *= ratio;
    if (!std::isfinite(value)) {
      *error = "Non-finite result converting MaterialX input '" + input->Name() +
               "'";
      return false;
    }
  }

  std::string converted_text;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) converted_text += ", ";
    converted_text += FormatMtlxFloat(values[i]);
  }
  input->SetAttr("value", converted_text);
  input->attrs.erase(
      std::remove_if(input->attrs.begin(), input->attrs.end(),
                     [](const std::pair<std::string, std::string> &attr) {
                       return attr.first == "unit" || attr.first == "unittype";
                     }),
      input->attrs.end());
  *converted = true;
  return true;
}

bool ConvertMtlxNodeInputs(MtlxElem *elem, const std::string &unit_type,
                           const std::string &target_unit,
                           const MtlxUnitScales &scales, bool *converted,
                           std::string *error) {
  if (!IsMtlxStructuralElement(elem->tag)) {
    for (MtlxElem &child : elem->children) {
      if (child.tag == "input" &&
          !ConvertMtlxInput(&child, unit_type, target_unit, scales, converted,
                            error))
        return false;
    }
  }
  for (MtlxElem &child : elem->children) {
    if (child.tag == "input") continue;
    if (!ConvertMtlxNodeInputs(&child, unit_type, target_unit, scales,
                               converted, error))
      return false;
  }
  return true;
}

std::vector<std::string> SplitMtlxList(const std::string &text) {
  std::string normalized = text;
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::istringstream input(normalized);
  std::vector<std::string> result;
  std::string token;
  while (input >> token) result.push_back(std::move(token));
  return result;
}

std::vector<std::string> MtlxGeomPathParts(const std::string &path) {
  std::vector<std::string> parts;
  std::istringstream input(path);
  std::string part;
  while (std::getline(input, part, '/'))
    if (!part.empty()) parts.push_back(std::move(part));
  return parts;
}

bool MtlxGeomPathMatches(const std::string &selector,
                         const std::string &geometry, bool contains) {
  if (selector.empty() || geometry.empty()) return false;
  const std::vector<std::string> selector_parts = MtlxGeomPathParts(selector);
  const std::vector<std::string> geometry_parts =
      MtlxGeomPathParts(geometry);
  if (contains && selector_parts.size() > geometry_parts.size()) return false;
  const size_t common = std::min(selector_parts.size(), geometry_parts.size());
  for (size_t i = 0; i < common; ++i)
    if (selector_parts[i] != geometry_parts[i]) return false;
  return true;
}

bool MtlxGeomStringMatches(const std::string &selectors,
                           const std::string &geometry, bool contains) {
  for (const std::string &selector : SplitMtlxList(selectors))
    if (MtlxGeomPathMatches(selector, geometry, contains)) return true;
  return false;
}

bool MtlxCollectionMatchesGeometry(const MtlxDocument &document,
                                   const std::string &collection_name,
                                   const std::string &geometry,
                                   std::set<std::string> *active,
                                   bool *matches, std::string *error) {
  const MtlxCollectionPtr collection = document.FindCollection(collection_name);
  if (!collection) {
    *error = "Unknown MaterialX collection '" + collection_name + "'";
    return false;
  }
  if (!active->insert(collection_name).second) {
    *error = "Cycle in MaterialX collection includes at '" + collection_name +
             "'";
    return false;
  }

  *matches = false;
  if (MtlxGeomStringMatches(collection->GetExcludeGeom(), geometry, true)) {
    active->erase(collection_name);
    return true;
  }
  if (MtlxGeomStringMatches(collection->GetIncludeGeom(), geometry, false)) {
    *matches = true;
    active->erase(collection_name);
    return true;
  }
  for (const std::string &included : collection->GetIncludedCollections()) {
    bool included_match = false;
    if (!MtlxCollectionMatchesGeometry(document, included, geometry, active,
                                       &included_match, error)) {
      active->erase(collection_name);
      return false;
    }
    if (included_match) {
      *matches = true;
      active->erase(collection_name);
      return true;
    }
  }
  active->erase(collection_name);
  return true;
}

}  // namespace

void UpgradeMtlxVersion(MtlxElem *root) {
  UpgradeMtlxVersionImpl(root);
}

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
        volume_nodegraph_ = nodegraph;
        volume_output_ = output.empty() ? "out" : output;
      }
    }
  }

  return true;
}

bool MtlxMaterialAssignment::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) return false;
  material_ = xml_node->GetAttribute("material");
  geom_ = xml_node->GetAttribute("geom");
  collection_ = xml_node->GetAttribute("collection");
  return true;
}

MtlxMaterialAssignmentPtr MtlxLook::GetMaterialAssignment(
    const std::string& name) const {
  for (const MtlxMaterialAssignmentPtr& assignment : material_assignments_)
    if (assignment && assignment->GetName() == name) return assignment;
  return nullptr;
}

bool MtlxLook::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) return false;
  material_assignments_.clear();
  for (const XMLNodePtr& child : xml_node->GetChildren()) {
    if (!child || child->GetName() != "materialassign") continue;
    auto assignment = std::make_shared<MtlxMaterialAssignment>();
    if (!assignment->ParseFromXML(child)) return false;
    material_assignments_.push_back(std::move(assignment));
  }
  return true;
}

bool MtlxCollection::ParseFromXML(XMLNodePtr xml_node) {
  if (!MtlxElement::ParseFromXML(xml_node)) return false;
  include_geom_ = GetExtraAttribute("includegeom");
  exclude_geom_ = GetExtraAttribute("excludegeom");
  included_collections_ = SplitMtlxList(GetExtraAttribute("includecollection"));
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
  looks_.clear();
  collections_.clear();

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

bool MtlxDocument::ConvertToUnit(const std::string &unit_type,
                                const std::string &target_unit) {
  error_.clear();
  if (!parsed_root_) {
    error_ = "MaterialX document has not been parsed";
    return false;
  }

  MtlxElem candidate = XMLNodeToMtlxElem(parsed_root_);
  std::set<std::string> unit_types;
  std::map<std::string, MtlxUnitScales> all_scales;
  CollectMtlxUnitMetadata(candidate, unit_type, &unit_types, &all_scales,
                          &error_);
  if (!error_.empty()) return false;

  const auto type_scales = all_scales.find(unit_type);
  if (unit_types.find(unit_type) == unit_types.end() ||
      type_scales == all_scales.end() || type_scales->second.empty()) {
    error_ = "No MaterialX unit converter is defined for type '" + unit_type +
             "'";
    return false;
  }

  bool converted = false;
  for (MtlxElem &child : candidate.children) {
    if (!ConvertMtlxNodeInputs(&child, unit_type, target_unit,
                               type_scales->second, &converted, &error_))
      return false;
  }
  if (!converted) return false;

  MtlxDocument updated;
  if (!updated.ParseRoot(MtlxElemToXMLNode(candidate))) {
    error_ = "Failed to rebuild MaterialX document after unit conversion";
    return false;
  }
  updated.warning_ = warning_;
  *this = std::move(updated);
  return true;
}

bool MtlxDocument::GetMaterialAssignmentsForGeometry(
    const std::string &look_name, const std::string &geometry,
    std::vector<MtlxResolvedMaterialAssignment> *assignments,
    std::string *err) const {
  if (err) err->clear();
  if (!assignments) {
    if (err) *err = "Material assignment output is null";
    return false;
  }
  assignments->clear();
  MtlxLookPtr look = FindLook(look_name);
  if (!look) {
    if (err) *err = "MaterialX look not found: " + look_name;
    return false;
  }

  std::vector<MtlxResolvedMaterialAssignment> resolved;
  std::set<std::string> visited_looks;
  while (look) {
    if (!visited_looks.insert(look->GetName()).second) {
      if (err) *err = "Cycle in MaterialX look inheritance at '" + look->GetName() + "'";
      return false;
    }
    for (const MtlxMaterialAssignmentPtr &assignment :
         look->GetMaterialAssignments()) {
      if (!assignment) continue;
      bool matches = MtlxGeomStringMatches(assignment->GetGeom(), geometry,
                                           true);
      if (!matches && !assignment->GetCollection().empty()) {
        std::set<std::string> active;
        std::string collection_error;
        if (!MtlxCollectionMatchesGeometry(*this, assignment->GetCollection(),
                                           geometry, &active, &matches,
                                           &collection_error)) {
          if (err) *err = collection_error;
          return false;
        }
      }
      if (!matches) continue;

      MtlxMaterialPtr material = FindMaterial(assignment->GetMaterial());
      if (!material) {
        if (err)
          *err = "MaterialX material not found for assignment '" +
                 assignment->GetName() + "': " + assignment->GetMaterial();
        return false;
      }
      resolved.push_back({assignment, std::move(material)});
    }
    const std::string inherited = look->GetExtraAttribute("inherit");
    if (inherited.empty()) break;
    look = FindLook(inherited);
    if (!look) {
      if (err) *err = "Inherited MaterialX look not found: " + inherited;
      return false;
    }
  }
  *assignments = std::move(resolved);
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
  } else if (element_name == "look") {
    auto look = std::make_shared<MtlxLook>();
    if (look->ParseFromXML(xml_node)) looks_.push_back(look);
  } else if (element_name == "collection") {
    auto collection = std::make_shared<MtlxCollection>();
    if (collection->ParseFromXML(xml_node)) collections_.push_back(collection);
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

MtlxLookPtr MtlxDocument::FindLook(const std::string& name) const {
  for (const MtlxLookPtr& look : looks_)
    if (look && look->GetName() == name) return look;
  return nullptr;
}

MtlxCollectionPtr MtlxDocument::FindCollection(const std::string &name) const {
  for (const MtlxCollectionPtr &collection : collections_)
    if (collection && collection->GetName() == name) return collection;
  return nullptr;
}

} // namespace mtlx
} // namespace lightusd
