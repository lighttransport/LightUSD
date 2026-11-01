// SPDX-License-Identifier: Apache-2.0
// Macro expansion: <attach>, cable composites, <replicate>, frames, default inlining.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
// --- MJCF <attach> sub-model composition -----------------------------------
// `<asset><model name file>` declares a child model; `<attach model body
// prefix>` grafts that child's body subtree at the attach point, prefixing
// every name + reference. We expand it at the XML level (after include
// expansion) so the rest of the pipeline sees one flat model. Used by
// iit_softfoot/scene.xml (the only menagerie model using <attach>).

// Attributes whose value is a name or a reference to one (prefixed on attach).
const std::set<std::string>& AttachRefAttrs() {
  static const std::set<std::string> s = {
      "name",   "class",   "childclass", "material", "mesh",
      "hfield", "texture", "joint",      "joint1",   "joint2",
      "site",   "site1",   "site2",      "refsite",  "sidesite",
      "tendon", "tendon1", "tendon2",    "body",     "body1",
      "body2",  "geom",    "geom1",      "geom2",    "objname"};
  return s;
}

void PrefixAttachNode(pugi::xml_node node, const std::string& prefix,
                      double angle_scale) {
  for (pugi::xml_attribute a : node.attributes()) {
    if (AttachRefAttrs().count(a.name())) {
      const std::string v = a.value();
      if (!v.empty()) a.set_value((prefix + v).c_str());
    }
  }
  if (angle_scale != 1.0) {
    const double k = angle_scale;
    if (pugi::xml_attribute e = node.attribute("euler")) {
      const auto v = ParseDoubles(e.value());
      if (v.size() == 3) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g", v[0] * k, v[1] * k,
                      v[2] * k);
        e.set_value(buf);
      }
    }
    if (pugi::xml_attribute aa = node.attribute("axisangle")) {
      const auto v = ParseDoubles(aa.value());
      if (v.size() == 4) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g %.10g", v[0], v[1],
                      v[2], v[3] * k);
        aa.set_value(buf);
      }
    }
    // Angular joint ranges and reference coordinates use compiler angle units;
    // slide ranges and actuator controls use length/radian units independently.
    const std::string tag = node.name();
    const std::string type = Attr(node, "type", "hinge");
    if (tag == "joint" && (type == "hinge" || type == "ball")) {
      for (const char* key : {"range", "ref", "springref"}) {
        auto attr = node.attribute(key);
        if (!attr) continue;
        std::ostringstream converted;
        converted.precision(17);
        const auto values = ParseDoubles(attr.value());
        for (size_t i = 0; i < values.size(); ++i) {
          if (i) converted << ' ';
          converted << values[i] * k;
        }
        attr.set_value(converted.str().c_str());
      }
    }
  }
}

// Every node of the subtree is rewritten independently (explicit stack).
void PrefixAttachSubtree(pugi::xml_node node, const std::string& prefix,
                         double angle_scale) {
  std::vector<pugi::xml_node> stack{node};
  while (!stack.empty()) {
    const pugi::xml_node n = stack.back();
    stack.pop_back();
    PrefixAttachNode(n, prefix, angle_scale);
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) stack.push_back(c);
  }
}

// <attach> elements below `n`, in document (pre)order (explicit stack).
void CollectAttachNodes(pugi::xml_node n, std::vector<pugi::xml_node>* out) {
  std::vector<pugi::xml_node> stack;
  for (pugi::xml_node c = n.last_child(); c; c = c.previous_sibling()) stack.push_back(c);
  while (!stack.empty()) {
    const pugi::xml_node c = stack.back();
    stack.pop_back();
    if (std::string(c.name()) == "attach") out->push_back(c);
    for (pugi::xml_node d = c.last_child(); d; d = d.previous_sibling()) stack.push_back(d);
  }
}

// First <body> named `name` below `n`, in preorder (explicit stack).
pugi::xml_node FindBodyByName(pugi::xml_node n, const std::string& name) {
  std::vector<pugi::xml_node> stack;
  const auto push_bodies = [&](pugi::xml_node parent) {
    for (pugi::xml_node b = parent.last_child(); b; b = b.previous_sibling())
      if (std::string(b.name()) == "body") stack.push_back(b);
  };
  push_bodies(n);
  while (!stack.empty()) {
    const pugi::xml_node b = stack.back();
    stack.pop_back();
    if (std::string(b.attribute("name").as_string()) == name) return b;
    push_bodies(b);
  }
  return pugi::xml_node();
}

bool ChargeExpansion(size_t elements, const char* what, std::string* err) {
  if (elements > active_reader->expansion_elements_left) {
    if (err) *err = std::string("MJCF ") + what + " expansion exceeds the element budget";
    return false;
  }
  active_reader->expansion_elements_left -= elements;
  return active_reader->ChargeArray(elements, kExpandedElementBytes, err);
}

size_t CountElements(pugi::xml_node node) {
  size_t count = 0;
  std::vector<pugi::xml_node> stack{node};
  while (!stack.empty()) {
    const pugi::xml_node n = stack.back();
    stack.pop_back();
    ++count;
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) stack.push_back(c);
  }
  return count;
}

bool ExpandAttachments(pugi::xml_node root, const fs::path& base_dir,
                       std::string* err) {
  std::map<std::string, fs::path> models;
  for (pugi::xml_node asset : root.children("asset")) {
    for (pugi::xml_node m : asset.children("model")) {
      const std::string name = m.attribute("name").as_string();
      const std::string file = m.attribute("file").as_string();
      if (!name.empty() && !file.empty()) {
        std::error_code wc_ec;
        models[name] = fs::weakly_canonical(base_dir / file, wc_ec);
      }
    }
  }
  std::vector<pugi::xml_node> attaches;
  CollectAttachNodes(root, &attaches);
  if (attaches.empty()) return true;

  bool parent_radian = false;
  if (pugi::xml_node comp = root.child("compiler")) {
    parent_radian =
        ToLower(comp.attribute("angle").as_string("degree")) == "radian";
  }

  for (pugi::xml_node attach : attaches) {
    const std::string model_name = attach.attribute("model").as_string();
    const std::string body_name = attach.attribute("body").as_string();
    const std::string prefix = attach.attribute("prefix").as_string();
    const auto it = models.find(model_name);
    if (it == models.end()) {
      active_reader->Warn("<attach> references unknown model: " + model_name);
      continue;
    }
    std::string child_xml;
    if (!ReadFile(it->second, &child_xml, err)) return false;
    std::set<fs::path> seen;
    std::string child_expanded;
    if (!ExpandIncludes(child_xml, it->second.parent_path(), &seen,
                        &child_expanded, err, it->second.string())) {
      return false;
    }
    pugi::xml_document child_doc;
    if (!child_doc.load_string(child_expanded.c_str())) {
      if (err) *err = "Failed to parse attached model: " + it->second.string();
      return false;
    }
    pugi::xml_node child_root = child_doc.child("mujoco");
    if (!child_root) continue;
    if (!ChargeExpansion(CountElements(child_root), "<attach>", err)) return false;

    const fs::path child_dir = it->second.parent_path();
    std::string child_meshdir;
    bool child_radian = false;
    if (pugi::xml_node cc = child_root.child("compiler")) {
      child_meshdir = cc.attribute("meshdir").as_string();
      if (child_meshdir.empty())
        child_meshdir = cc.attribute("assetdir").as_string();
      child_radian =
          ToLower(cc.attribute("angle").as_string("degree")) == "radian";
    }
    const double angle_scale = child_radian == parent_radian ? 1.0
                               : child_radian ? 57.295779513082323
                                              : 0.017453292519943295;

    // Make the child's mesh/hfield file paths absolute so they still resolve
    // after the assets are merged into the (differently-rooted) parent model.
    for (pugi::xml_node asset : child_root.children("asset")) {
      for (const char* tag : {"mesh", "hfield", "skin"}) {
        for (pugi::xml_node a : asset.children(tag)) {
          pugi::xml_attribute f = a.attribute("file");
          if (f && !std::string(f.value()).empty() &&
              fs::path(f.value()).is_relative()) {
            std::error_code wc_ec;
            const fs::path abs = fs::weakly_canonical(
                child_dir / child_meshdir / f.value(), wc_ec);
            f.set_value(abs.string().c_str());
          }
        }
      }
    }

    InlineMJCFDefaults(child_root);
    PrefixAttachSubtree(child_root, prefix, angle_scale);

    pugi::xml_node found;
    for (pugi::xml_node wb : child_root.children("worldbody")) {
      found = FindBodyByName(wb, prefix + body_name);
      if (found) break;
    }
    if (!found) {
      active_reader->Warn("<attach> body " + body_name +
                          " not found in model " + model_name);
      continue;
    }

    pugi::xml_node parent = attach.parent();
    parent.insert_copy_before(found, attach);
    parent.remove_child(attach);

    // Merge the child's model-level sections (prefixed) into the parent.
    for (const char* sec : {"default", "asset", "tendon", "actuator", "sensor",
                            "equality", "contact", "keyframe"}) {
      pugi::xml_node child_sec = child_root.child(sec);
      if (!child_sec) continue;
      pugi::xml_node main_sec = root.child(sec);
      if (!main_sec) main_sec = root.append_child(sec);
      for (pugi::xml_node c : child_sec.children()) {
        if (std::string(c.name()) == "model") continue;  // don't recurse models
        main_sec.append_copy(c);
      }
    }
  }
  return true;
}

// --- MJCF <frame> flattening -----------------------------------------------
// A <frame> is a pure coordinate transform applied to all its direct children
// (it is NOT a body). MuJoCo 3 uses it to group/offset geoms/bodies. We
// dissolve each frame by composing its transform into every child's pose and
// moving the children up to the frame's parent, so the rest of the pipeline
// never sees a <frame>. Used by apptronik_apollo (frames wrap finger-mesh
// geoms).

void SetNodeAttr(pugi::xml_node n, const char* name, const char* val) {
  pugi::xml_attribute a = n.attribute(name);
  if (a)
    a.set_value(val);
  else
    n.append_attribute(name).set_value(val);
}

// Column-major 4x4 -> translation + quaternion {w,x,y,z}.
void DecomposePosQuat(const std::vector<double>& m, std::array<double, 3>* pos,
                      Quat* q) {
  *pos = {{m[12], m[13], m[14]}};
  const double trace = m[0] + m[5] + m[10];
  double w, x, y, z;
  if (trace > 0.0) {
    const double S = 2.0 * std::sqrt(trace + 1.0);
    w = 0.25 * S;
    x = (m[6] - m[9]) / S;
    y = (m[8] - m[2]) / S;
    z = (m[1] - m[4]) / S;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[0] - m[5] - m[10]);
    w = (m[6] - m[9]) / S;
    x = 0.25 * S;
    y = (m[4] + m[1]) / S;
    z = (m[8] + m[2]) / S;
  } else if (m[5] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[5] - m[0] - m[10]);
    w = (m[8] - m[2]) / S;
    x = (m[4] + m[1]) / S;
    y = 0.25 * S;
    z = (m[9] + m[6]) / S;
  } else {
    const double S = 2.0 * std::sqrt(1.0 + m[10] - m[0] - m[5]);
    w = (m[1] - m[4]) / S;
    x = (m[8] + m[2]) / S;
    y = (m[9] + m[6]) / S;
    z = 0.25 * S;
  }
  *q = QuatNormalize({{w, x, y, z}});
}

bool ExpandCableComposites(pugi::xml_node root, const Context& ctx, std::string* err) {
  auto norm = [](std::array<double, 3>& v) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n >= 1e-14 && std::abs(n - 1) > 1e-14) for (double& x : v) x /= n;
    return n;
  };
  auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::array<double, 3>{{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                  a[0] * b[1] - a[1] * b[0]}};
  };
  auto rotate = [](const Quat& q, const std::array<double, 3>& v) {
    const std::vector<double> m = MatrixFromPosQuat({{0, 0, 0}}, q);
    return std::array<double, 3>{{m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
                                  m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
                                  m[2] * v[0] + m[6] * v[1] + m[10] * v[2]}};
  };
  auto fmt = [](std::initializer_list<double> values) {
    std::string out;
    char buf[40];
    for (double v : values) {
      std::snprintf(buf, sizeof(buf), "%.17g", v);
      if (!out.empty()) out += ' ';
      out += buf;
    }
    return out;
  };
  std::vector<pugi::xml_node> composites;
  // <composite> elements in document order, not descending into them
  // (explicit stack).
  for (pugi::xml_node worldbody : root.children("worldbody")) {
    std::vector<pugi::xml_node> stack;
    for (pugi::xml_node c = worldbody.last_child(); c; c = c.previous_sibling()) stack.push_back(c);
    while (!stack.empty()) {
      const pugi::xml_node c = stack.back();
      stack.pop_back();
      if (std::string(c.name()) == "composite") {
        composites.push_back(c);
        continue;
      }
      for (pugi::xml_node d = c.last_child(); d; d = d.previous_sibling()) stack.push_back(d);
    }
  }
  for (pugi::xml_node comp : composites) {
    const std::string type = ToLower(Attr(comp, "type"));
    if (type != "cable") {
      if (err) *err = "MJCF composite type `" + type + "` is not supported (MuJoCo only builds cables)";
      return false;
    }
    const std::string prefix = Attr(comp, "prefix");
    const pugi::xml_node geom = Child(comp, "geom");
    const std::string geom_type = ToLower(Attr(geom, "type", "sphere"));
    if (geom_type != "capsule" && geom_type != "cylinder" && geom_type != "box") {
      if (err) *err = "MJCF cable composite geom must be a capsule, cylinder or box";
      return false;
    }
    std::vector<double> vert = ParseDoubles(Attr(comp, "vertex"));
    if (vert.empty()) {
      const auto counts = ParseDoubles(Attr(comp, "count"));
      const int count = counts.empty() ? 1 : ToInt32(counts[0], "count");
      // size may list fewer than three values (e.g. size="1").
      std::array<double, 3> size{{0, 0, 0}};
      const auto sizes = ParseDoubles(Attr(comp, "size"));
      for (size_t k = 0; k < 3 && k < sizes.size(); ++k) size[k] = sizes[k];
      std::istringstream curves(Attr(comp, "curve"));
      std::array<std::string, 3> curve{{"0", "0", "0"}};
      for (size_t k = 0; k < 3 && (curves >> curve[k]); ++k) {
        if (curve[k] != "s" && curve[k] != "cos(s)" && curve[k] != "sin(s)" && curve[k] != "0") {
          if (err) *err = "MJCF cable composite curve `" + curve[k] + "` is invalid";
          return false;
        }
      }
      const std::vector<double> q = ParseDoubles(Attr(comp, "quat"));
      const Quat cq = q.size() >= 4 ? QuatNormalize({{q[0], q[1], q[2], q[3]}}) : Quat{{1, 0, 0, 0}};
      if (count < 2 || count > kMaxCableVertices) {
        if (err) *err = "MJCF cable composite needs between 2 and 512 vertices";
        return false;
      }
      for (int ix = 0; ix < count; ++ix) {
        std::array<double, 3> v{};
        for (size_t k = 0; k < 3; ++k) {
          const double t = kPi * ix * size[2] / (count - 1);
          v[k] = curve[k] == "s"      ? ix * size[0] / (count - 1)
                 : curve[k] == "cos(s)" ? size[1] * std::cos(t)
                 : curve[k] == "sin(s)" ? size[1] * std::sin(t)
                                        : 0.0;
        }
        v = rotate(cq, v);
        vert.insert(vert.end(), v.begin(), v.end());
      }
    }
    const int n = static_cast<int>(vert.size() / 3);
    if (n < 2 || n > kMaxCableVertices || vert.size() % 3) {
      if (err) *err = "MJCF cable composite needs at least two vertices";
      return false;
    }
    // Per vertex: a body, its joint, geom, site and contact exclude (bound).
    if (!ChargeExpansion(size_t(n) * 8, "cable composite", err)) return false;
    // mujoco.elasticity.cable: bend torque E*I*(w - w0)/L between segments,
    // twist G*J*(w - w0)/L. A ball-joint spring toward the initial pose with
    // k = E*I/L reproduces bending; twist uses the same isotropic stiffness.
    double bend_modulus = 0.0;
    bool flat = false;
    if (const pugi::xml_node plugin = Child(comp, "plugin")) {
      double twist_modulus = 0.0;
      for (const auto& config : Children(plugin, "config")) {
        const std::string key = Attr(config, "key");
        if (key == "bend") bend_modulus = ParseDoubleAttr(config, "value", 0.0);
        if (key == "twist") twist_modulus = ParseDoubleAttr(config, "value", 0.0);
        if (key == "flat") flat = MjcBoolAttr(Attr(config, "value", "false"));
      }
      const bool exact = bend_modulus > 0 && !flat && twist_modulus == 0.0;
      if (!exact &&
          !UnsupportedXML(comp, bend_modulus > 0
                                    ? "Cable elasticity is approximated by isotropic ball-joint stiffness E*I/L"
                                    : "Cable elasticity plugin has no inline bend config; it is dropped",
                          "mjcf.cable_plugin_approximated", "mjcf.composite",
                          active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
      if (flat) bend_modulus = 0.0;  // a straight rest shape has no joint-spring form
    }
    double second_moment = 0.0;
    {
      const auto gs = ParseDoubles(Attr(geom, "size"));
      if (geom_type == "box") {
        const double h = gs.size() > 1 ? gs[1] : 0.0, w = gs.size() > 2 ? gs[2] : 0.0;
        second_moment = 0.5 * (std::pow(2 * w, 3) * 2 * h + std::pow(2 * h, 3) * 2 * w) / 12.0;
      } else if (!gs.empty()) {
        second_moment = kPi * std::pow(gs[0], 4) / 4.0;
      }
    }
    const auto offset = ParseDouble3(Attr(comp, "offset"), {{0, 0, 0}});
    const std::string initial = Attr(comp, "initial", "ball");
    pugi::xml_node joint_def;
    for (pugi::xml_node j : comp.children("joint"))
      if (Attr(j, "kind", "main") == "main") joint_def = j;
    const pugi::xml_node site_def = Child(comp, "site");
    pugi::xml_node contact = root.child("contact");
    if (!contact) contact = root.append_child("contact");
    auto at = [&](int i) { return std::array<double, 3>{{vert[3 * i], vert[3 * i + 1], vert[3 * i + 2]}}; };
    std::array<double, 3> normal{{0, 1, 0}};
    Quat prev{{1, 0, 0, 0}};
    pugi::xml_node parent = comp.parent();
    pugi::xml_node anchor = comp;
    const int last = n - 2;
    auto body_name = [&](int i) {
      return prefix + (i == 0 ? "B_first" : i == last ? "B_last" : "B_" + std::to_string(i));
    };
    for (int ix = 0; ix <= last; ++ix) {
      const bool first = ix == 0, is_last = ix == last;
      std::array<double, 3> edge, tprev{}, tnext{};
      for (size_t k = 0; k < 3; ++k) edge[k] = at(ix + 1)[k] - at(ix)[k];
      double length_prev = 0;
      if (!first) {
        for (size_t k = 0; k < 3; ++k) tprev[k] = at(ix)[k] - at(ix - 1)[k];
        length_prev = norm(tprev);
      }
      if (!is_last) {
        for (size_t k = 0; k < 3; ++k) tnext[k] = at(ix + 2)[k] - at(ix + 1)[k];
        norm(tnext);
      }
      std::array<double, 3> tangent = edge;
      const double length = norm(tangent);
      std::array<double, 3> binormal;
      if (first) {
        binormal = cross(tangent, tnext);
        norm(binormal);
        normal = cross(binormal, tangent);
        norm(normal);
      } else {
        binormal = cross(tprev, tangent);
        const double bn = norm(binormal);
        const double angle = std::atan2(bn, tprev[0] * tangent[0] + tprev[1] * tangent[1] + tprev[2] * tangent[2]);
        normal = rotate(QuatFromAxisAngle(binormal, angle), normal);
        norm(normal);
        binormal = cross(tangent, normal);
        norm(binormal);
      }
      const Quat quat = QuatFromBasis(tangent, normal, binormal);
      pugi::xml_node body =
          anchor ? parent.insert_child_before("body", anchor) : parent.append_child("body");
      body.append_attribute("name").set_value(body_name(ix).c_str());
      if (first) {
        body.append_attribute("pos").set_value(
            fmt({offset[0] + vert[0], offset[1] + vert[1], offset[2] + vert[2]}).c_str());
        body.append_attribute("quat").set_value(fmt({quat[0], quat[1], quat[2], quat[3]}).c_str());
        if (comp.attribute("childclass")) body.append_attribute("childclass").set_value(Attr(comp, "childclass").c_str());
      } else {
        const Quat d = QuatMul({{prev[0], -prev[1], -prev[2], -prev[3]}}, quat);
        body.append_attribute("pos").set_value(fmt({length_prev, 0, 0}).c_str());
        body.append_attribute("quat").set_value(fmt({d[0], d[1], d[2], d[3]}).c_str());
      }
      if (!first || initial != "none") {
        pugi::xml_node j = body.append_child("joint");
        const bool free = first && initial == "free";
        if (joint_def && !free)
          for (pugi::xml_attribute a : joint_def.attributes())
            if (std::string(a.name()) != "kind") j.append_attribute(a.name()).set_value(a.value());
        j.append_attribute("name").set_value(
            (prefix + (first ? "J_first" : is_last ? "J_last" : "J_" + std::to_string(ix))).c_str());
        if (j.attribute("type")) j.remove_attribute("type");
        j.append_attribute("type").set_value(free ? "free" : "ball");
        if (!first && bend_modulus > 0 && length_prev > 0 && !j.attribute("stiffness")) {
          char buf[40];
          std::snprintf(buf, sizeof(buf), "%.17g", bend_modulus * second_moment / length_prev);
          j.append_attribute("stiffness").set_value(buf);
        }
      }
      pugi::xml_node g = body.append_copy(geom);
      g.remove_attribute("name");
      g.append_attribute("name").set_value((prefix + "G" + std::to_string(ix)).c_str());
      if (geom_type == "box") {
        const auto size = ParseDoubles(Attr(geom, "size"));
        g.remove_attribute("size");
        g.remove_attribute("pos");
        g.append_attribute("pos").set_value(fmt({length / 2, 0, 0}).c_str());
        g.append_attribute("size").set_value(
            fmt({length / 2, size.size() > 1 ? size[1] : 0.0, size.size() > 2 ? size[2] : 0.0}).c_str());
      } else {
        g.remove_attribute("fromto");
        g.append_attribute("fromto").set_value(fmt({0, 0, 0, length, 0, 0}).c_str());
      }
      if (first || is_last) {
        pugi::xml_node site = site_def ? body.append_copy(site_def) : body.append_child("site");
        site.remove_attribute("name");
        site.remove_attribute("pos");
        site.append_attribute("name").set_value((prefix + (is_last ? "S_last" : "S_first")).c_str());
        site.append_attribute("pos").set_value(fmt({is_last ? length : 0.0, 0, 0}).c_str());
      }
      if (!is_last) {
        pugi::xml_node ex = contact.append_child("exclude");
        ex.append_attribute("body1").set_value(body_name(ix).c_str());
        ex.append_attribute("body2").set_value(body_name(ix + 1).c_str());
      }
      prev = quat;
      parent = body;
      anchor = pugi::xml_node();
    }
    comp.parent().remove_child(comp);
  }
  (void)ctx;
  return true;
}

// MJCF <replicate count offset euler sep> as MuJoCo's XML reader expands it:
// copy i sits in a frame at the accumulated offset, rotated by i*euler, and
// every named element gets the suffix sep + i (zero-padded to the digits of
// count; inner replicates append first). Elements elsewhere that reference a
// replicated name (tendons, actuators, sensors, equalities, contacts) are
// replicated and renamed with each copy.
bool ExpandReplicates(pugi::xml_node root, const Context& ctx, std::string* err) {
  static const char* const kRefs[] = {
      "site",   "joint",   "body",    "geom",     "tendon",   "body1",
      "body2",  "joint1",  "joint2",  "site1",    "site2",    "geom1",
      "geom2",  "objname", "refname", "target",   "sidesite", "actuator",
      "jointinparent",     "slidersite",          "cranksite"};
  // Every node of a subtree (including its root), explicit stack.
  const auto for_subtree = [](pugi::xml_node top, const std::function<void(pugi::xml_node)>& fn) {
    std::vector<pugi::xml_node> stack{top};
    while (!stack.empty()) {
      const pugi::xml_node n = stack.back();
      stack.pop_back();
      fn(n);
      for (pugi::xml_node d = n.first_child(); d; d = d.next_sibling()) stack.push_back(d);
    }
  };
  // Replicates expand innermost first (post-order), so a copied subtree is
  // already expanded. Expanding one only rewrites its own parent's children
  // and the reference sections, so the order is taken up front.
  std::vector<pugi::xml_node> replicates;
  for (pugi::xml_node worldbody : root.children("worldbody")) {
    std::vector<pugi::xml_node> order, stack{worldbody};
    while (!stack.empty()) {
      const pugi::xml_node n = stack.back();
      stack.pop_back();
      order.push_back(n);
      for (pugi::xml_node d = n.first_child(); d; d = d.next_sibling()) stack.push_back(d);
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
      if (*it != worldbody && std::string(it->name()) == "replicate") replicates.push_back(*it);
  }
  for (const pugi::xml_node c : replicates) {
    pugi::xml_node parent = c.parent();
    const int count = ToInt32(ParseDoubleAttr(c, "count", 0.0), "count");
    if (count <= 0) {
      if (err) *err = "MJCF <replicate> count must be positive";
      return false;
    }
    const auto offset = ParseDouble3(Attr(c, "offset"), {{0, 0, 0}});
    const auto euler = ParseDouble3(Attr(c, "euler"), {{0, 0, 0}});
    const std::string sep = Attr(c, "sep");
    std::set<std::string> names;
    size_t nodes = 0;
    for_subtree(c, [&](pugi::xml_node d) {
      if (d == c) return;
      ++nodes;
      if (d.attribute("name")) names.insert(d.attribute("name").as_string());
    });
    const size_t digits = std::to_string(count).size();
    auto suffix = [&](int i) {
      std::string s = std::to_string(i);
      return sep + std::string(digits > s.size() ? digits - s.size() : 0, '0') + s;
    };
    const auto rename = [&](pugi::xml_node top, const std::string& sfx, bool own_names) {
      for_subtree(top, [&](pugi::xml_node n) {
        if (own_names)
          if (auto a = n.attribute("name")) a.set_value((std::string(a.value()) + sfx).c_str());
        for (const char* key : kRefs)
          if (auto a = n.attribute(key))
            if (names.count(a.value())) a.set_value((std::string(a.value()) + sfx).c_str());
      });
    };
    auto references = [&](pugi::xml_node n) {
      bool found = false;
      for_subtree(n, [&](pugi::xml_node m) {
        for (const char* key : kRefs)
          if (auto a = m.attribute(key)) found |= names.count(a.value()) > 0;
      });
      return found;
    };
    // Each copy adds a <frame> plus the subtree (charged even when empty).
    if (nodes + 1 > active_reader->expansion_elements_left / static_cast<size_t>(count)) {
      if (err) *err = "MJCF <replicate> expansion exceeds the element budget";
      return false;
    }
    if (!ChargeExpansion((nodes + 1) * static_cast<size_t>(count), "<replicate>", err))
      return false;
    std::array<double, 3> pos{{0, 0, 0}};
    for (int i = 0; i < count; ++i) {
      char buf[200];
      std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", i * euler[0], i * euler[1],
                    i * euler[2]);
      const Quat q = OrientationQuat("", "", buf, "", "", ctx);
      pugi::xml_node frame = parent.insert_child_before("frame", c);
      if (c.attribute("childclass"))
        frame.append_attribute("childclass").set_value(c.attribute("childclass").value());
      frame.append_attribute("euler").set_value(buf);
      std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", pos[0], pos[1], pos[2]);
      frame.append_attribute("pos").set_value(buf);
      for (pugi::xml_node d : c.children()) rename(frame.append_copy(d), suffix(i), true);
      const std::vector<double> m = MatrixFromPosQuat({{0, 0, 0}}, q);
      for (int k = 0; k < 3; ++k)
        pos[size_t(k)] += m[size_t(k)] * offset[0] + m[size_t(4 + k)] * offset[1] +
                          m[size_t(8 + k)] * offset[2];
    }
    parent.remove_child(c);
    for (const char* section : {"tendon", "actuator", "sensor", "equality", "contact"}) {
      for (pugi::xml_node sec : root.children(section)) {
        for (pugi::xml_node e = sec.first_child(); e;) {
          pugi::xml_node after = e.next_sibling();
          if (e.type() == pugi::node_element && references(e)) {
            const size_t copy = CountElements(e);
            if (copy > active_reader->expansion_elements_left / static_cast<size_t>(count) ||
                !ChargeExpansion(copy * static_cast<size_t>(count), "<replicate>", err)) {
              if (err && err->empty())
                *err = "MJCF <replicate> expansion exceeds the element budget";
              return false;
            }
            for (int i = 0; i < count; ++i) rename(sec.insert_copy_before(e, e), suffix(i), true);
            sec.remove_child(e);
          }
          e = after;
        }
      }
    }
  }
  return true;
}

// Dissolve the <frame> children of `parent` (whose own frames are already
// flattened) into it.
void DissolveFramesUnder(pugi::xml_node parent, const Context& ctx) {
  pugi::xml_node child = parent.first_child();
  while (child) {
    pugi::xml_node next = child.next_sibling();
    if (std::string(child.name()) == "frame") {
      const std::array<double, 3> fpos =
          ParseDouble3(Attr(child, "pos"), {{0, 0, 0}});
      const Quat fq = OrientationQuat(
          Attr(child, "quat"), Attr(child, "axisangle"), Attr(child, "euler"),
          Attr(child, "xyaxes"), Attr(child, "zaxis"), ctx);
      const std::vector<double> fm = MatrixFromPosQuat(fpos, fq);
      const std::string fcc = Attr(child, "childclass");
      for (pugi::xml_node gc : child.children()) {
        const std::array<double, 3> cpos =
            ParseDouble3(Attr(gc, "pos"), {{0, 0, 0}});
        const Quat cq = OrientationQuat(Attr(gc, "quat"), Attr(gc, "axisangle"),
                                        Attr(gc, "euler"), Attr(gc, "xyaxes"),
                                        Attr(gc, "zaxis"), ctx);
        const std::vector<double> cm =
            MultiplyMatrix(fm, MatrixFromPosQuat(cpos, cq));
        std::array<double, 3> npos;
        Quat nq;
        DecomposePosQuat(cm, &npos, &nq);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", npos[0], npos[1],
                      npos[2]);
        SetNodeAttr(gc, "pos", buf);
        // Joints have an axis and lights a direction instead of an
        // orientation: rotate those vectors into the parent frame.
        const std::string gname = gc.name();
        const char* vector_key = gname == "joint" ? "axis" : gname == "light" ? "dir" : nullptr;
        if (vector_key) {
          const auto v = ParseDouble3(
              Attr(gc, vector_key, gname == "joint" ? "0 0 1" : "0 0 -1"), {{0, 0, 1}});
          const std::array<double, 3> r{{fm[0] * v[0] + fm[4] * v[1] + fm[8] * v[2],
                                         fm[1] * v[0] + fm[5] * v[1] + fm[9] * v[2],
                                         fm[2] * v[0] + fm[6] * v[1] + fm[10] * v[2]}};
          std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", r[0], r[1], r[2]);
          SetNodeAttr(gc, vector_key, buf);
        } else {
          std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g %.17g", nq[0], nq[1],
                        nq[2], nq[3]);
          SetNodeAttr(gc, "quat", buf);
          gc.remove_attribute("euler");
          gc.remove_attribute("axisangle");
          gc.remove_attribute("xyaxes");
          gc.remove_attribute("zaxis");
        }
        // A capsule/cylinder fromto is in the frame's coordinates too.
        if (pugi::xml_attribute ft = gc.attribute("fromto")) {
          const auto v = ParseDoubles(ft.value());
          if (v.size() == 6) {
            auto tp = [&](double a, double b, double c) {
              return std::array<double, 3>{
                  {fm[0] * a + fm[4] * b + fm[8] * c + fm[12],
                   fm[1] * a + fm[5] * b + fm[9] * c + fm[13],
                   fm[2] * a + fm[6] * b + fm[10] * c + fm[14]}};
            };
            const auto p0 = tp(v[0], v[1], v[2]);
            const auto p1 = tp(v[3], v[4], v[5]);
            std::snprintf(buf, sizeof(buf),
                          "%.17g %.17g %.17g %.17g %.17g %.17g", p0[0], p0[1],
                          p0[2], p1[0], p1[1], p1[2]);
            SetNodeAttr(gc, "fromto", buf);
          }
        }
        // Propagate the frame's childclass to children that don't set one.
        if (!fcc.empty() && !gc.attribute("childclass") &&
            !gc.attribute("class")) {
          SetNodeAttr(gc, "childclass", fcc.c_str());
        }
        parent.insert_copy_before(gc, child);
      }
      parent.remove_child(child);
    }
    child = next;
  }
}

// Bottom-up: frames nested inside a child are flattened before the frames
// directly under its parent. Dissolving only rewrites a node's own children,
// so a post-order list taken up front (explicit stack) is the recursive order.
void FlattenFramesIn(pugi::xml_node parent, const Context& ctx) {
  std::vector<pugi::xml_node> order, stack{parent};
  while (!stack.empty()) {
    const pugi::xml_node n = stack.back();
    stack.pop_back();
    order.push_back(n);
    for (pugi::xml_node c = n.first_child(); c; c = c.next_sibling()) stack.push_back(c);
  }
  for (auto it = order.rbegin(); it != order.rend(); ++it) DissolveFramesUnder(*it, ctx);
}

bool IsSolverArray(std::string_view name) {
  for (const char* key : {"solimp", "solimplimit", "solimpfriction", "o_solimp",
                          "solref", "solreflimit", "solreffriction", "o_solref"})
    if (name == key) return true;
  return false;
}

std::string CompleteSolverArray(const std::string& name, const std::string& value,
                                const std::string& inherited, const std::string& kind) {
  std::vector<double> defaults;
  if (name == "solimp" || name == "solimplimit" || name == "solimpfriction" || name == "o_solimp")
    defaults = {0.9, 0.95, 0.001, 0.5, 2.0};
  else if (name == "solref" || name == "solreflimit" || name == "solreffriction" || name == "o_solref")
    defaults = name == "solreffriction" && kind == "pair" ? std::vector<double>{0, 0} : std::vector<double>{0.02, 1};
  else return value;
  const auto authored = ParseDoubles(value);
  if (authored.empty()) return value;
  if (authored.size() > defaults.size()) {
    active_reader->failure = "Too many MJCF solver values for " + name;
    return value;
  }
  if (authored.size() == defaults.size()) return value;
  const auto parent = ParseDoubles(inherited);
  for (size_t i = 0; i < std::min(parent.size(), defaults.size()); ++i) defaults[i] = parent[i];
  for (size_t i = 0; i < authored.size(); ++i) defaults[i] = authored[i];
  std::string completed;
  for (double number : defaults) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.17g", number);
    if (!completed.empty()) completed += ' ';
    completed += text;
  }
  if (active_reader->options.report) {
    auto& report = *active_reader->options.report;
    auto& count = report.counts["completed_solver_arrays"];
    const auto previous = count.is_number() ? count.get<uint64_t>() : uint64_t(0);
    count = previous == UINT64_MAX ? previous : previous + 1;
    report.Diagnostic("info", "mjcf.solver_defaults", name + " completed using inherited/MJCF defaults", active_reader->root_source);
  }
  return completed;
}

// Resolve default classes for every supported element, including actuator,
// tendon, site and asset defaults (not only geoms and joints).
void InlineMJCFDefaults(pugi::xml_node root) {
  using Elements = std::map<std::string, AttrMap>;
  std::map<std::string, Elements> classes;
  // Class tables in preorder (a later definition of a name wins), with an
  // explicit stack of (default node, attributes inherited from its parent).
  std::vector<std::pair<pugi::xml_node, Elements>> pending;
  const auto collect = [&](pugi::xml_node node, Elements inherited) {
        for (auto child : node.children()) {
          if (std::string(child.name()) == "default") continue;
          for (auto attr : child.attributes()) {
            auto& previous = inherited[child.name()][attr.name()];
            previous = IsSolverArray(attr.name()) ? CompleteSolverArray(attr.name(), attr.value(), previous, child.name()) : attr.value();
          }
        }
        const std::string name = Attr(node, "class");
        classes[name] = inherited;
        // The top-level <default> is MuJoCo's implicit class "main", whether
        // or not it is named; unclassed elements use it.
        if (std::string(node.parent().name()) != "default") {
          classes[""] = inherited;
          classes["main"] = inherited;
        }
        std::vector<pugi::xml_node> children;
        for (auto child : node.children("default")) children.push_back(child);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
          pending.emplace_back(*it, inherited);
      };
  for (auto node : root.children("default")) {
    pending.emplace_back(node, Elements{});
    while (!pending.empty()) {
      auto item = std::move(pending.back());
      pending.pop_back();
      collect(item.first, std::move(item.second));
    }
  }
  // One element: inline its class attributes. Returns whether to descend, and
  // updates `inherited_class` with the element's childclass for its subtree.
  const auto apply = [&](pugi::xml_node node, std::string& inherited_class) -> bool {
        const std::string kind = node.name();
        if (kind == "default") return false;
        std::string selected = Attr(node, "class", inherited_class);
        auto found = classes.find(selected);
        if (found == classes.end()) {
          if (node.attribute("class") && !classes.empty() && active_reader) {
            const std::string message = "MJCF <" + kind + "> references unknown default class `" +
                                        selected + "`";
            if (active_reader->options.unsupported_policy == UnsupportedPolicy::Error)
              active_reader->failure = message;
            else
              active_reader->Warn(message + "; using the root default", "mjcf.unknown_class");
          }
          found = classes.find("");
        }
        static const char* const kOrientation[] = {"quat", "euler", "axisangle",
                                                   "xyaxes", "zaxis"};
        bool own_orientation = false;
        for (const char* key : kOrientation)
          own_orientation |= static_cast<bool>(node.attribute(key));
        auto merge = [&](const std::string& element) {
          if (found == classes.end()) return;
          auto attrs = found->second.find(element);
          if (attrs == found->second.end()) return;
          for (const auto& attr : attrs->second) {
            // An element's own orientation (in any of the alternative
            // spellings) overrides every orientation inherited from a class.
            if (own_orientation &&
                std::find(std::begin(kOrientation), std::end(kOrientation),
                          attr.first) != std::end(kOrientation))
              continue;
            if (auto own = node.attribute(attr.first.c_str())) {
              if (IsSolverArray(attr.first)) {
                const auto completed = CompleteSolverArray(attr.first, own.value(), attr.second, kind);
                if (completed != own.value()) own.set_value(completed.c_str());
              }
            } else
              node.append_attribute(attr.first.c_str())
                  .set_value(attr.second.c_str());
          }
        };
        // Non-legacy profiles resolve actuator classes in
        // AddMujocoActuatorsJson (MuJoCo's per-class actuator slot and
        // shortcut source rules); inlining by tag would mix shortcuts.
        if (std::string(node.parent().name()) == "actuator" && IsMjcfActuatorTag(kind) &&
            active_reader && active_reader->options.profile != "legacy")
          return false;
        // General actuator parameters are the baseline for actuator shortcuts.
        if (std::string(node.parent().name()) == "actuator" &&
            kind != "general")
          merge("general");
        if (std::string(node.parent().name()) == "tendon") merge("tendon");
        if (std::string(node.parent().name()) == "equality") merge("equality");
        merge(kind);
        for (auto attr : node.attributes()) {
          if (!IsSolverArray(attr.name())) continue;
          const auto completed = CompleteSolverArray(attr.name(), attr.value(), "", kind);
          if (completed != attr.value()) attr.set_value(completed.c_str());
        }
        if (node.attribute("childclass"))
          inherited_class = Attr(node, "childclass");
        return true;
      };
  // Preorder over the document (explicit stack): unknown-class diagnostics
  // keep document order.
  std::vector<std::pair<pugi::xml_node, std::string>> stack{{root, std::string()}};
  while (!stack.empty()) {
    const pugi::xml_node node = stack.back().first;
    std::string inherited_class = std::move(stack.back().second);
    stack.pop_back();
    if (!apply(node, inherited_class)) continue;
    for (pugi::xml_node c = node.last_child(); c; c = c.previous_sibling())
      stack.emplace_back(c, inherited_class);
  }
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
