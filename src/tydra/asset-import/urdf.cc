// SPDX-License-Identifier: Apache-2.0
// URDF frontend.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
std::vector<double> URDFPose(const pugi::xml_node& origin) {
  auto xyz = ParseDouble3(Attr(origin, "xyz"), {{0, 0, 0}});
  Context ctx;
  ctx.angle_to_rad = 1.0;
  // URDF uses fixed-axis roll/pitch/yaw: Rz(yaw) * Ry(pitch) * Rx(roll).
  ctx.eulerseq = "XYZ";
  auto q = OrientationQuat("", "", Attr(origin, "rpy"), "", "", ctx);
  return MatrixFromPosQuat(xyz, q);
}

// URDF OBJ/DAE meshes keep every part and their own materials.
bool LoadURDFObj(const std::string& file, const std::string& source, OBJ* obj,
                 std::string* resolved, std::string* err) {
  std::vector<uint8_t> data;
  if (!active_reader->Read(file, source, &data, resolved, err)) return false;
  const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
  return ToLower(fs::path(file).extension().string()) == ".dae"
             ? ReadDAE(text, *resolved, *active_reader, obj, err)
             : ReadOBJ(text, *resolved, *active_reader, true, obj, err);
}

PreloadKey URDFObjKey(const std::string& file, const std::string& source) {
  return {"urdf-obj\n" + source + "\n" + file, true};
}

// Resolve a URDF mesh without reading it; the mesh loader reads it once. A
// read-only resolver hook cannot resolve separately, and a failed
// resolution reads to report the failure as before.
bool ResolveURDFMesh(const std::string& file, const std::string& source, std::string* resolved,
                     std::string* err) {
  Reader& reader = *active_reader;
  if (!reader.options.read || reader.options.open) {
    AssetSource asset;
    std::string why;
    if (reader.Open(file, source, &asset, &why)) {
      *resolved = asset.identifier;
      return true;
    }
  }
  std::vector<uint8_t> data;
  return reader.Read(file, source, &data, resolved, err);
}

bool URDFGeometry(const pugi::xml_node& node, const std::string& source,
                  Json* geometries, std::string* err) {
  const auto geometry = node.child("geometry");
  const auto pose = URDFPose(node.child("origin"));
  std::string name = Attr(node, "name", "geometry");
  Json j{{"name", name}, {"matrix", Array(pose)}};
  const bool collision = std::string(node.name()) == "collision" &&
                         !active_reader->options.tessellate_collision_shapes;
  // Non-legacy profiles keep primitive visuals native too (as
  // urdf-usd-converter authors Cube/Sphere/Cylinder visuals).
  const bool native = collision || (std::string(node.name()) == "visual" &&
                                    active_reader->options.profile != "legacy");
  // Geometry the importer cannot convert (zero-size primitives, unknown mesh
  // formats) is skipped under the warn/preserve policies, as Newton's
  // add_urdf and urdf-usd-converter skip it.
  auto skip = [&](const std::string& why) {
    std::string what = "URDF ";
    what += std::string(node.name()) == "collision" ? "collision" : "visual";
    what += " `" + name + "` " + why;
    if (!active_reader->Unsupported(what, "urdf.geometry_unsupported", "urdf.geometry", source, 0,
                                    Json{{"element", name}}.dump(), "json")) {
      if (err) *err = active_reader->failure.empty() ? what : active_reader->failure;
      return false;
    }
    return true;
  };
  if (auto mesh = geometry.child("mesh")) {
    std::string file = Attr(mesh, "filename");
    {
      const std::string ext = ToLower(fs::path(file).extension().string());
      if (ext != ".obj" && ext != ".dae" && ext != ".stl")
        return skip("has an unsupported mesh format `" + (ext.empty() ? file : ext) + "`");
    }
    std::string resolved;
    auto scale = ParseDouble3(Attr(mesh, "scale"), {{1, 1, 1}});
    const auto matrix = MultiplyMatrix(pose, ScaleMatrix(scale));
    const std::string ext = ToLower(fs::path(file).extension().string());
    if (ext == ".obj" || ext == ".dae") {
      // Embedded OBJ/DAE materials take priority over URDF <material>.
      OBJ obj;
      PreloadedMesh preloaded;
      if (TakePreloaded(URDFObjKey(file, source), &preloaded, err, true)) {
        obj = std::move(preloaded.obj);
        resolved = std::move(preloaded.resolved);
      } else if (!active_reader->failure.empty() ||
                 !LoadURDFObj(file, source, &obj, &resolved, err)) {
        return false;
      }
      int32_t offset = static_cast<int32_t>(active_materials->size());
      for (auto& m : obj.materials) {
        m["name"] = "obj_" + std::to_string(active_materials->size()) + "_" +
                    m["name"].get<std::string>();
        active_materials->push_back(m);
      }
      for (auto& part : obj.meshes) {
        for (auto& mi : part.materials)
          if (mi >= 0) mi += offset;
        Json part_json = MeshRefJSON(part, *active_reader);
        part_json["name"] = name + "_" + part.name;
        part_json["matrix"] = Array(matrix);
        geometries->push_back(std::move(part_json));
      }
      return true;
    }
    if (!ResolveURDFMesh(file, source, &resolved, err)) return false;
    Mesh m;
    if (!LoadMeshFile(fs::path(resolved), false, &m, err)) return false;
    j = MeshRefJSON(m, *active_reader);
    j["name"] = name;
    j["matrix"] = Array(matrix);
  } else {
    Json shape = Json::object();
    Mesh generated;
    if (auto box = geometry.child("box")) {
      auto size = ParseDouble3(Attr(box, "size"), {{1, 1, 1}});
      if (!(size[0] > 0 && size[1] > 0 && size[2] > 0)) return skip("has a non-positive box size");
      if (native) {
        shape["type"] = "box";
        j["matrix"] = Array(MultiplyMatrix(
            pose, ScaleMatrix({{size[0] / 2, size[1] / 2, size[2] / 2}})));
      } else
        generated = MakeBoxMesh({{size[0] / 2, size[1] / 2, size[2] / 2}});
    } else if (auto sphere = geometry.child("sphere")) {
      double radius = ParseDoubleAttr(sphere, "radius", 0);
      if (!(radius > 0)) return skip("has a non-positive sphere radius");
      if (native)
        shape = Json{{"type", "sphere"}, {"radius", radius}};
      else
        generated = MakeSphereMesh(radius);
    } else if (auto cylinder = geometry.child("cylinder")) {
      double radius = ParseDoubleAttr(cylinder, "radius", 0),
             length = ParseDoubleAttr(cylinder, "length", 0);
      if (!(radius > 0 && length > 0)) return skip("has non-positive cylinder dimensions");
      if (native)
        shape = Json{{"type", "cylinder"},
                     {"radius", radius},
                     {"height", length},
                     {"axis", "Z"}};
      else
        generated = MakeCylinderMesh(radius, length / 2);
    } else if (auto capsule = geometry.child("capsule");
               capsule && active_reader->options.profile != "legacy") {
      // Non-standard but common (Gazebo, Newton's add_urdf): `length` or
      // `height` is the cylindrical part, along Z.
      const double radius = ParseDoubleAttr(capsule, "radius", 0),
                   length = ParseDoubleAttr(capsule, HasAttr(capsule, "length") ? "length" : "height", 0);
      if (!(radius > 0 && length >= 0)) return skip("has non-positive capsule dimensions");
      if (native)
        shape = Json{{"type", "capsule"}, {"radius", radius}, {"height", length}, {"axis", "Z"}};
      else
        generated = MakeCapsuleMesh(radius, length / 2);
    } else {
      // Missing or unknown geometry (urdf-usd-converter warns and continues).
      return skip("has no supported geometry");
    }
    if (native)
      j["shape"] = std::move(shape);
    else {
      Json built = MeshRefJSON(generated, *active_reader);
      j["geometry"] = std::move(built["geometry"]);
      if (built.contains("meshRef")) j["meshRef"] = std::move(built["meshRef"]);
    }
  }
  geometries->push_back(std::move(j));
  return true;
}

// Inverse of a rigid column-major transform: [R t] -> [R^T  -R^T t].
std::vector<double> InverseRigidMatrix(const std::vector<double>& m) {
  std::vector<double> r(16, 0.0);
  for (int c = 0; c < 3; ++c)
    for (int k = 0; k < 3; ++k) r[size_t(c * 4 + k)] = m[size_t(k * 4 + c)];
  for (int k = 0; k < 3; ++k)
    r[size_t(12 + k)] = -(r[size_t(k)] * m[12] + r[size_t(4 + k)] * m[13] + r[size_t(8 + k)] * m[14]);
  r[15] = 1.0;
  return r;
}

// urdf-usd-converter's ghost links: a link with no (or zero) inertia, no
// visuals and no collisions on a fixed joint (or the root) is a pure frame.
// It is authored without RigidBodyAPI, its fixed joint is dropped, and joints
// from it re-anchor on the nearest rigid ancestor (or the world). A link on a
// fixed joint that a mimic joint references is kept.
// KDL-style dummy inertia (urdf-usd-converter, Gazebo lumping): a parent
// with geometry but no inertia and an inertial-only, childless child on a
// fixed joint. The first such child's inertia moves into the parent frame
// (com = R c + t, I = R I R^T) and the child link and joint are dropped.
size_t MergeDummyInertiaLinks(Json* links, Json* joints,
                              const std::map<std::string, std::string>& parent,
                              const std::map<std::string, std::vector<double>>& origins) {
  std::map<std::string, Json*> by_name;
  for (auto& link : *links) by_name[link["name"].get<std::string>()] = &link;
  auto zero_inertia = [](const Json& link) {
    if (!link.contains("inertial")) return true;
    const Json& in = link["inertial"];
    if (in["mass"].get<double>() > 0.0) return false;
    for (const auto& v : in["fullInertia"])
      if (v.get<double>() != 0.0) return false;
    return true;
  };
  auto has_geometry = [](const Json& link) {
    return !link["visuals"].empty() || !link["collisions"].empty() || link.contains("shapeElements");
  };
  std::set<std::string> has_children, mimic_children, merged_into;
  std::map<std::string, std::string> joint_child;
  for (const auto& j : *joints) {
    has_children.insert(j["parent"].get<std::string>());
    joint_child[j["name"].get<std::string>()] = j["child"].get<std::string>();
  }
  for (const auto& j : *joints)
    if (j.contains("mimic")) {
      auto child = joint_child.find(j["mimic"]["joint"].get<std::string>());
      if (child != joint_child.end()) mimic_children.insert(child->second);
    }
  std::set<std::string> dropped;
  for (const auto& j : *joints) {
    const std::string child = j["child"].get<std::string>();
    const std::string from = j["parent"].get<std::string>();
    if (j["type"].get<std::string>() != "fixed" || !by_name.count(child) || !by_name.count(from) ||
        merged_into.count(from) || has_children.count(child) || mimic_children.count(child))
      continue;
    Json& p = *by_name[from];
    const Json& c = *by_name[child];
    if (!zero_inertia(p) || !has_geometry(p) || has_geometry(c) || zero_inertia(c)) continue;
    const std::vector<double>& m = origins.at(child);  // child frame in parent
    const Json& in = c["inertial"];
    double com[3] = {in["centerOfMass"][0].get<double>(), in["centerOfMass"][1].get<double>(),
                     in["centerOfMass"][2].get<double>()};
    double t[3][3];
    const double f[6] = {in["fullInertia"][0].get<double>(), in["fullInertia"][1].get<double>(),
                         in["fullInertia"][2].get<double>(), in["fullInertia"][3].get<double>(),
                         in["fullInertia"][4].get<double>(), in["fullInertia"][5].get<double>()};
    const double full[3][3] = {{f[0], f[3], f[4]}, {f[3], f[1], f[5]}, {f[4], f[5], f[2]}};
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) {
        t[a][b] = 0;
        for (int k = 0; k < 3; ++k)
          for (int l = 0; l < 3; ++l) t[a][b] += m[size_t(k * 4 + a)] * full[k][l] * m[size_t(l * 4 + b)];
      }
    const Json moved_com = Json::array({m[0] * com[0] + m[4] * com[1] + m[8] * com[2] + m[12],
                                        m[1] * com[0] + m[5] * com[1] + m[9] * com[2] + m[13],
                                        m[2] * com[0] + m[6] * com[1] + m[10] * com[2] + m[14]});
    p["inertial"] = Json{{"mass", in["mass"].get<double>()},
                         {"centerOfMass", moved_com},
                         {"fullInertia", Json::array({t[0][0], t[1][1], t[2][2], t[0][1], t[0][2], t[1][2]})}};
    merged_into.insert(from);
    dropped.insert(child);
  }
  if (dropped.empty()) return 0;
  Json kept_links = Json::array(), kept_joints = Json::array();
  for (auto& link : *links)
    if (!dropped.count(link["name"].get<std::string>())) kept_links.push_back(std::move(link));
  for (auto& j : *joints)
    if (!dropped.count(j["child"].get<std::string>())) kept_joints.push_back(std::move(j));
  *links = std::move(kept_links);
  *joints = std::move(kept_joints);
  (void)parent;
  return dropped.size();
}

size_t CollapseGhostLinks(Json* links, Json* joints, const std::map<std::string, std::string>& parent,
                          const std::map<std::string, std::vector<double>>& worlds) {
  std::map<std::string, std::string> parent_joint_type, joint_child;
  for (const auto& j : *joints) {
    parent_joint_type[j["child"].get<std::string>()] = j["type"].get<std::string>();
    joint_child[j["name"].get<std::string>()] = j["child"].get<std::string>();
  }
  std::set<std::string> mimic_children;
  for (const auto& j : *joints)
    if (j.contains("mimic")) {
      const std::string leader = j["mimic"]["joint"].get<std::string>();
      auto child = joint_child.find(leader);
      if (child != joint_child.end() && parent_joint_type[child->second] == "fixed")
        mimic_children.insert(child->second);
    }
  std::set<std::string> ghosts;
  for (auto& link : *links) {
    if (link.contains("static") && link["static"].is_boolean() && link["static"].get<bool>()) continue;
    const std::string name = link["name"].get<std::string>();
    bool massless = true;
    if (link.contains("inertial")) {
      const Json& in = link["inertial"];
      const double mass = in["mass"].get<double>();
      massless = mass <= 0.0;
      if (massless && in.contains("fullInertia"))
        for (const auto& v : in["fullInertia"]) massless &= v.get<double>() == 0.0;
    }
    // urdf-usd-converter judges ghosts by the <visual>/<collision> elements,
    // so a link whose geometry could not be converted is not a ghost.
    const bool shapes = !link["visuals"].empty() || !link["collisions"].empty() ||
                        link.contains("shapeElements");
    const auto type = parent_joint_type.find(name);
    const bool root = !parent.count(name);
    if (!massless || shapes || mimic_children.count(name) ||
        !(root || (type != parent_joint_type.end() && type->second == "fixed")))
      continue;
    ghosts.insert(name);
    link["static"] = true;
    link["ghost"] = true;
    link["articulationRoot"] = false;
  }
  if (ghosts.empty()) return 0;
  auto rigid_ancestor = [&](std::string name) {
    while (ghosts.count(name)) {
      auto up = parent.find(name);
      if (up == parent.end()) return active_world_name;
      name = up->second;
    }
    return name;
  };
  Json kept = Json::array();
  std::set<std::string> new_roots;
  for (auto& j : *joints) {
    const std::string child = j["child"].get<std::string>();
    std::string from = j["parent"].get<std::string>();
    if (ghosts.count(child)) continue;  // the ghost's own fixed joint
    if (ghosts.count(from)) {
      const std::string anchor = rigid_ancestor(from);
      auto read = [&](const char* key, std::initializer_list<double> fallback) {
        std::vector<double> v(fallback);
        if (j.contains(key) && j[key].is_array() && j[key].size() == v.size())
          for (size_t k = 0; k < v.size(); ++k) v[k] = j[key][k].get<double>();
        return v;
      };
      const auto pos = read("localPos0", {0, 0, 0});
      const auto rot = read("localRot0", {1, 0, 0, 0});
      const std::vector<double> frame = MultiplyMatrix(
          worlds.at(from), MatrixFromPosQuat({{pos[0], pos[1], pos[2]}},
                                             {{rot[0], rot[1], rot[2], rot[3]}}));
      const std::vector<double> anchor_world =
          anchor == active_world_name ? IdentityMatrix() : worlds.at(anchor);
      std::array<double, 3> p;
      Quat q;
      DecomposePosQuat(MultiplyMatrix(InverseRigidMatrix(anchor_world), frame), &p, &q);
      j["parent"] = anchor;
      j["localPos0"] = Json::array({p[0], p[1], p[2]});
      j["localRot0"] = Json::array({q[0], q[1], q[2], q[3]});
      if (anchor == active_world_name) new_roots.insert(child);
    }
    kept.push_back(std::move(j));
  }
  *joints = std::move(kept);
  for (auto& link : *links)
    if (new_roots.count(link["name"].get<std::string>())) link["articulationRoot"] = true;
  return ghosts.size();
}

// URDF elements and attributes without a USD mapping, kept as urdf:*
// metadata the way urdf-usd-converter keeps "undefined" elements:
// {tag, attributes, text, elements}. Non-legacy profiles only.
Json URDFCustomElement(const pugi::xml_node& node, size_t depth) {
  // Built bottom-up with an explicit stack: each frame finishes its element
  // and appends it to the parent's `elements` (children below depth 64 are
  // dropped, as before).
  struct Frame {
    pugi::xml_node node, next;
    size_t depth;
    Json e;
    std::string text;
    Json elements = Json::array();
  };
  const auto open = [](const pugi::xml_node& n, size_t d) {
    Frame f{n, n.first_child(), d, Json{{"tag", std::string(n.name())}}};
    Json attributes = Json::object();
    for (auto a : n.attributes()) attributes[a.name()] = std::string(a.value());
    if (!attributes.empty()) f.e["attributes"] = std::move(attributes);
    return f;
  };
  std::vector<Frame> stack;
  stack.push_back(open(node, depth));
  Json result;
  while (!stack.empty()) {
    Frame& f = stack.back();
    if (f.next) {
      const pugi::xml_node c = f.next;
      f.next = c.next_sibling();
      if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) f.text += c.value();
      else if (c.type() == pugi::node_element && f.depth < 64) stack.push_back(open(c, f.depth + 1));
      continue;
    }
    const size_t b = f.text.find_first_not_of(" \t\r\n");
    if (b != std::string::npos)
      f.e["text"] = f.text.substr(b, f.text.find_last_not_of(" \t\r\n") - b + 1);
    if (!f.elements.empty()) f.e["elements"] = std::move(f.elements);
    Json done = std::move(f.e);
    stack.pop_back();
    if (stack.empty()) result = std::move(done);
    else stack.back().elements.push_back(std::move(done));
  }
  return result;
}

// Unknown attributes and child elements of `node` (null when none).
Json URDFCustomJson(const pugi::xml_node& node, std::initializer_list<const char*> known_children,
                    std::initializer_list<const char*> known_attributes) {
  auto known = [](std::initializer_list<const char*> names, const char* name) {
    for (const char* n : names)
      if (std::strcmp(n, name) == 0) return true;
    return false;
  };
  Json out = Json::object();
  Json attributes = Json::object();
  for (auto a : node.attributes())
    if (!known(known_attributes, a.name())) attributes[a.name()] = std::string(a.value());
  if (!attributes.empty()) out["attributes"] = std::move(attributes);
  Json elements = Json::array();
  for (auto c : node.children())
    if (c.type() == pugi::node_element && !known(known_children, c.name()))
      elements.push_back(URDFCustomElement(c, 1));
  if (!elements.empty()) out["elements"] = std::move(elements);
  return out;
}

// Roots for `package://NAME/...` URDF references that Options::packages does
// not map, found the way Newton's and urdf-usd-converter's URDF importers do:
// the nearest ancestor of the URDF's directory named NAME, otherwise the
// nearest ancestor ADIR for which ADIR/<relative path> exists (relative paths
// with `..` are not searched). Explicit mappings always win.
void DiscoverURDFPackages(const pugi::xml_node& root, const std::string& source) {
  if (source.empty() || active_reader->options.open || active_reader->options.read) return;
  std::error_code ec;
  fs::path start = fs::path(source);
  if (!start.is_absolute()) start = fs::current_path(ec) / start;
  start = fs::weakly_canonical(start, ec).parent_path();
  if (start.empty()) return;
  // ROS package layout: <package>/urdf/robot.urdf referencing ../meshes/...
  // The package directory (with package.xml or meshes/) is an asset root too.
  const std::string start_name = ToLower(start.filename().string());
  const bool ros_layout = start_name == "urdf" || start_name == "robots";
  {
    const fs::path package = start.parent_path();
    if (ros_layout && !package.empty() && package != package.root_path() &&
        (fs::is_regular_file(package / "package.xml", ec) || fs::is_directory(package / "meshes", ec))) {
      active_reader->extra_roots.push_back(package.string());
      active_reader->Warn("URDF in a ROS package layout; " + package.generic_string() +
                              " is an allowed asset root",
                          "urdf.package_root");
    }
  }
  std::vector<pugi::xml_node> stack{root};
  while (!stack.empty()) {
    const pugi::xml_node node = stack.back();
    stack.pop_back();
    for (auto c = node.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
    const std::string tag = node.name();
    if (tag != "mesh" && tag != "texture") continue;
    const std::string file = Attr(node, "filename");
    if (file.rfind("package://", 0) != 0) continue;
    const size_t end = file.find('/', 10);
    if (end == std::string::npos || end == 10 || end + 1 >= file.size()) continue;
    const std::string package = file.substr(10, end - 10);
    if (active_reader->packages.count(package)) continue;
    const fs::path relative = fs::path(file.substr(end + 1)).lexically_normal();
    bool escapes = relative.is_absolute();
    for (const auto& part : relative) escapes = escapes || part == "..";
    // A discovered package becomes an asset root, so it must not widen the
    // readable tree past a real package: never the filesystem root, at most
    // kMaxLevels above the URDF, and above the URDF's own directory only a
    // directory with package.xml or the <package>/urdf parent (as above).
    const int kMaxLevels = 3;
    const auto acceptable = [&](const fs::path& dir, int level) {
      if (dir == dir.root_path()) return false;
      if (level == 0) return true;
      return fs::is_regular_file(dir / "package.xml", ec) || (level == 1 && ros_layout);
    };
    fs::path found;
    int level = 0;
    for (fs::path dir = start; !dir.empty() && level <= kMaxLevels; dir = dir.parent_path(), ++level) {
      if (dir == dir.root_path()) break;
      if (dir.filename() == package) {
        if (acceptable(dir, level)) found = dir;
        break;
      }
    }
    level = 0;
    for (fs::path dir = start; found.empty() && !escapes && !dir.empty() && level <= kMaxLevels;
         dir = dir.parent_path(), ++level) {
      if (dir == dir.root_path()) break;
      if (fs::is_regular_file(dir / relative, ec) && acceptable(dir, level)) found = dir;
    }
    if (found.empty()) continue;
    active_reader->packages[package] = found.string();
    active_reader->Warn("URDF package `" + package + "` resolved to " + found.generic_string() +
                            " (pass packages to map it explicitly)",
                        "urdf.package_discovered");
  }
}

bool BuildURDF(const pugi::xml_node& root, const std::string& source,
               const Options& opts, Json* payload, std::string* err) {
  std::map<std::pair<std::string, bool>, Mesh> mesh_cache;
  struct MeshCacheScope {
    std::map<std::pair<std::string, bool>, Mesh>* previous = active_mesh_cache;
    explicit MeshCacheScope(std::map<std::pair<std::string, bool>, Mesh>* c) { active_mesh_cache = c; }
    ~MeshCacheScope() { active_mesh_cache = previous; }
  } mesh_cache_scope(&mesh_cache);
  std::map<PreloadKey, PreloadedMesh> mesh_preload;
  struct MeshPreloadScope {
    std::map<PreloadKey, PreloadedMesh>* previous = active_mesh_preload;
    explicit MeshPreloadScope(std::map<PreloadKey, PreloadedMesh>* p) { active_mesh_preload = p; }
    ~MeshPreloadScope() { active_mesh_preload = previous; }
  } mesh_preload_scope(&mesh_preload);
  DiscoverURDFPackages(root, source);
  if (!opts.open && !opts.read) {
    // Parse every distinct mesh ahead on worker threads (see PreloadJobs).
    std::vector<std::pair<PreloadKey, PreloadJob>> jobs;
    std::set<PreloadKey> seen;
    for (auto link : root.children("link"))
      for (const char* kind : {"visual", "collision"})
        for (auto geom : link.children(kind)) {
          const auto mesh = geom.child("geometry").child("mesh");
          if (!mesh) continue;
          const std::string file = Attr(mesh, "filename");
          const std::string ext = ToLower(fs::path(file).extension().string());
          if (ext == ".obj" || ext == ".dae") {
            const PreloadKey key = URDFObjKey(file, source);
            if (seen.insert(key).second)
              jobs.emplace_back(key, [file, source](PreloadedMesh* out, std::string* why) {
                return LoadURDFObj(file, source, &out->obj, &out->resolved, why);
              });
          } else if (ext == ".stl") {
            AssetSource asset;
            std::string why;
            if (!active_reader->Open(file, source, &asset, &why)) continue;
            const PreloadKey key{asset.identifier, false};
            if (seen.insert(key).second)
              jobs.emplace_back(key, [key](PreloadedMesh* out, std::string* e) {
                return LoadMeshFileUncached(fs::path(key.first), false, &out->mesh, e);
              });
          }
        }
    PreloadJobs(jobs);
  }
  Json links = Json::array(), joints = Json::array(), materials = Json::array(),
       actuators = Json::array();
  std::map<std::string, pugi::xml_node> nodes;
  std::map<std::string, std::string> parent;
  std::map<std::string, std::vector<double>> origins, worlds;
  std::set<std::string> joint_names;
  for (auto node : root.children("link")) {
    std::string name = Attr(node, "name");
    if (name.empty() || nodes.count(name)) {
      if (err) *err = "URDF link names must be nonempty and unique";
      return false;
    }
    nodes[name] = node;
  }
  active_world_name = "__world";
  while (nodes.count(active_world_name)) active_world_name += "_";
  if (nodes.empty()) {
    if (err) *err = "URDF contains no links";
    return false;
  }
  for (auto node : root.children("joint")) {
    std::string name = Attr(node, "name"),
                pname = Attr(node.child("parent"), "link"),
                child = Attr(node.child("child"), "link");
    if (name.empty() || !joint_names.insert(name).second ||
        !nodes.count(pname) || !nodes.count(child) || parent.count(child)) {
      if (err)
        *err = "URDF has duplicate joints, unknown links or multiple parents";
      return false;
    }
    parent[child] = pname;
    origins[child] = URDFPose(node.child("origin"));
    Json j{{"name", name},
           {"type", Attr(node, "type", "fixed")},
           {"parent", pname},
           {"child", child}};
    auto type = j["type"].get<std::string>();
    if (type == "floating") {
      j["type"] = "floating";
      joints.push_back(std::move(j));
      continue;
    }
    // planar: 2 translations in the plane normal to `axis` + rotation about
    // it. Non-legacy profiles author a D6 joint with the other DOFs locked.
    const bool planar = type == "planar" && opts.profile != "legacy";
    if (type != "fixed" && type != "revolute" && type != "continuous" &&
        type != "prismatic" && !planar) {
      if (err) *err = "Unsupported URDF joint type: " + type;
      return false;
    }
    auto axis = ParseDouble3(Attr(node.child("axis"), "xyz"), {{1, 0, 0}});
    if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-24) {
      // A fixed joint never uses its axis (urdfdom accepts "0 0 0").
      if (type != "fixed") {
        if (err) *err = "URDF joint has zero axis";
        return false;
      }
      axis = {{1, 0, 0}};
    }
    std::array<double, 3> pos;
    Quat rot;
    DecomposePosQuat(origins[child], &pos, &rot);
    Quat align = QuatFromTwoVecs({{1, 0, 0}}, axis), q0 = QuatMul(rot, align);
    j["axisToken"] = "X";
    j["axis"] = Json::array({axis[0], axis[1], axis[2]});
    j["localPos0"] = Json::array({pos[0], pos[1], pos[2]});
    j["localPos1"] = Json::array({0, 0, 0});
    j["localRot0"] = Json::array({q0[0], q0[1], q0[2], q0[3]});
    j["localRot1"] = Json::array({align[0], align[1], align[2], align[3]});
    if (auto l = node.child("limit")) {
      Json limits = Json::object();
      for (const char* key : {"lower", "upper", "effort", "velocity"})
        if (l.attribute(key)) limits[key] = ParseDoubleAttr(l, key, 0);
      j["limit"] = std::move(limits);
    }
    if (auto d = node.child("dynamics")) {
      Json dynamics = Json::object();
      for (const char* key : {"damping", "friction", "stiffness", "armature"})
        if (d.attribute(key)) dynamics[key] = ParseDoubleAttr(d, key, 0);
      j["dynamics"] = std::move(dynamics);
    }
    // URDF metadata without a UsdPhysics/Newton home; authored as urdf:*
    // attributes by non-legacy profiles.
    for (const char* section : {"safety_controller", "calibration"})
      if (auto c = node.child(section)) {
        Json values = Json::object();
        for (auto a : c.attributes())
          values[a.name()] = ParseDoubleAttr(c, a.name(), 0);
        if (!values.empty()) j[section] = std::move(values);
      }
    if (auto m = node.child("mimic"))
      j["mimic"] = Json{{"joint", Attr(m, "joint")},
                        {"offset", ParseDoubleAttr(m, "offset", 0)},
                        {"multiplier", ParseDoubleAttr(m, "multiplier", 1)}};
    if (opts.profile != "legacy") {
      Json custom = URDFCustomJson(node, {"origin", "parent", "child", "axis", "limit", "dynamics",
                                          "mimic", "safety_controller", "calibration"},
                                   {"name", "type"});
      if (!custom.empty()) j["urdfCustom"] = std::move(custom);
    }
    joints.push_back(std::move(j));
  }
  // Resolve link world transforms iteratively: URDF chains (snakes, ropes,
  // cables) can be far deeper than the XML depth limit, and recursion depth
  // must not grow with chain length.
  const size_t max_chain = std::max<size_t>(opts.max_depth, nodes.size() + 1);
  std::function<bool(const std::string&, size_t)> world =
      [&](const std::string& name, size_t) {
        std::vector<std::string> chain;
        std::set<std::string> visiting;
        std::string current = name;
        while (!worlds.count(current)) {
          if (!visiting.insert(current).second || chain.size() > max_chain) {
            if (err) *err = "URDF cycle or hierarchy depth exceeded";
            return false;
          }
          chain.push_back(current);
          auto up = parent.find(current);
          if (up == parent.end()) break;
          current = up->second;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
          auto up = parent.find(*it);
          worlds[*it] = up == parent.end()
                            ? IdentityMatrix()
                            : MultiplyMatrix(worlds[up->second], origins[*it]);
        }
        return true;
      };
  auto material = [&](pugi::xml_node node,
                      const std::string& fallback) -> std::string {
    std::string name = Attr(node, "name", fallback);
    if (!node.child("color") && !node.child("texture")) return name;
    Json m{{"name", name}};
    if (auto c = node.child("color"))
      m["rgba"] = Array(ParseDoubles(Attr(c, "rgba")));
    if (auto t = node.child("texture")) {
      AssetBytes data;
      std::string resolved, why;
      if (active_reader->ReadTexture(Attr(t, "filename"), source, &data, &resolved,
                              &why)) {
        m["texture"] = resolved;
        m["texture_abs"] = resolved;
      } else
        active_reader->Warn("Texture: " + why, "import.texture_unresolved");
    }
    materials.push_back(std::move(m));
    return name;
  };
  for (auto m : root.children("material")) material(m, "material");
  // A synthetic static body anchors fixed roots, while floating joints omit
  // their parent constraint and remain free articulations.
  links.push_back(Json{{"name", active_world_name}, {"static", true}});
  for (const auto& entry : nodes) {
    const auto& name = entry.first;
    auto node = entry.second;
    if (!world(name, 0)) return false;
    Json link{{"name", name},
              {"matrix", Array(worlds[name])},
              {"visuals", Json::array()},
              {"collisions", Json::array()}};
    if (!parent.count(name)) {
      // A URDF without joints is a lone rigid body, not a one-link
      // articulation (urdf-usd-converter 0.3.1).
      link["articulationRoot"] = opts.profile == "legacy" || root.child("joint");
      joints.push_back(Json{{"name", active_world_name + "_to_" + name},
                            {"type", "fixed"},
                            {"parent", active_world_name},
                            {"child", name}});
    }
    if (auto i = node.child("inertial")) {
      auto origin = URDFPose(i.child("origin"));
      auto tensor = i.child("inertia");
      double t[3][3] = {
          {ParseDoubleAttr(tensor, "ixx", 0), ParseDoubleAttr(tensor, "ixy", 0),
           ParseDoubleAttr(tensor, "ixz", 0)},
          {ParseDoubleAttr(tensor, "ixy", 0), ParseDoubleAttr(tensor, "iyy", 0),
           ParseDoubleAttr(tensor, "iyz", 0)},
          {ParseDoubleAttr(tensor, "ixz", 0), ParseDoubleAttr(tensor, "iyz", 0),
           ParseDoubleAttr(tensor, "izz", 0)}};
      double rotated[3][3]{};
      for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
          for (int c = 0; c < 3; c++)
            for (int d = 0; d < 3; d++)
              rotated[a][b] += origin[c * 4 + a] * t[c][d] * origin[d * 4 + b];
      link["inertial"] = Json{
          {"mass", ParseDoubleAttr(i.child("mass"), "value", 0)},
          {"centerOfMass", Json::array({origin[12], origin[13], origin[14]})},
          {"fullInertia",
           Json::array({rotated[0][0], rotated[1][1], rotated[2][2],
                        rotated[0][1], rotated[0][2], rotated[1][2]})}};
      // An explicit all-zero <inertia> with a positive mass stays (near)
      // zero rather than being derived from the colliders (as add_urdf).
      bool zero = static_cast<bool>(tensor);
      for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) zero = zero && t[a][b] == 0.0;
      if (zero && link["inertial"]["mass"].get<double>() > 0.0) link["inertial"]["zeroInertia"] = true;
      // Likewise an explicit <mass value="0"> on a link with colliders.
      if (link["inertial"]["mass"].get<double>() == 0.0) link["inertial"]["zeroMass"] = true;
    }
    for (const char* kind : {"visual", "collision"})
      for (auto geom : node.children(kind)) {
        Json items = Json::array();
        std::string why;
        if (!URDFGeometry(geom, source, &items, &why)) {
          if (!opts.allow_missing) {
            if (err) *err = why;
            return false;
          }
          active_reader->Warn(why);
          continue;
        }
        std::string mat;
        if (auto m = geom.child("material"))
          mat = material(m, name + "_material");
        Json custom = opts.profile != "legacy"
                          ? URDFCustomJson(geom, {"origin", "geometry", "material"}, {"name"})
                          : Json::object();
        for (auto& item : items) {
          if (!mat.empty()) item["material"] = mat;
          if (!custom.empty()) item["urdfCustom"] = custom;
          link[kind == std::string("visual") ? "visuals" : "collisions"]
              .push_back(std::move(item));
        }
      }
    if (opts.profile != "legacy") {
      if (node.child("visual") || node.child("collision")) link["shapeElements"] = true;
      Json custom = URDFCustomJson(node, {"inertial", "visual", "collision"}, {"name"});
      if (!custom.empty()) link["urdfCustom"] = std::move(custom);
    }
    links.push_back(std::move(link));
  }
  for (auto& j : joints)
    if (j["type"].get<std::string>() == "floating")
      for (auto& l : links)
        if (l["name"].get<std::string>() == j["child"].get<std::string>()) {
          l["floating"] = true;
          l["articulationRoot"] = true;
        }
  Json filtered = Json::array();
  for (auto& j : joints)
    if (j["type"].get<std::string>() != "floating")
      filtered.push_back(std::move(j));
  joints = std::move(filtered);
  if (opts.profile != "legacy") {
    const size_t dummies = MergeDummyInertiaLinks(&links, &joints, parent, origins);
    if (dummies && opts.report) {
      auto& count = opts.report->counts["merged_dummy_inertia_links"];
      count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + dummies;
    }
    const size_t ghosts = CollapseGhostLinks(&links, &joints, parent, worlds);
    if (ghosts && opts.report) {
      auto& count = opts.report->counts["ghost_links"];
      count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + ghosts;
    }
    // A mimic whose leader is itself a mimic follower (hand linkages):
    // Newton's add_usd rejects such chains, so follow them to the leader that
    // is not a follower, composing q = m1 * (m2 * q_root + o2) + o1.
    std::map<std::string, Json*> by_name;
    for (auto& j : joints) by_name[j["name"].get<std::string>()] = &j;
    size_t resolved = 0;
    for (auto& j : joints) {
      if (!j.contains("mimic")) continue;
      Json& mimic = j["mimic"];
      double multiplier = mimic["multiplier"].get<double>(), offset = mimic["offset"].get<double>();
      std::string leader = mimic["joint"].get<std::string>();
      std::set<std::string> seen{j["name"].get<std::string>()};
      bool chained = false;
      for (auto it = by_name.find(leader); it != by_name.end() && it->second->contains("mimic");
           it = by_name.find(leader)) {
        if (!seen.insert(leader).second) break;  // a cycle: keep the direct leader
        const Json& next = (*it->second)["mimic"];
        offset += multiplier * next["offset"].get<double>();
        multiplier *= next["multiplier"].get<double>();
        leader = next["joint"].get<std::string>();
        chained = true;
      }
      if (!chained || seen.count(leader)) continue;
      mimic = Json{{"joint", leader}, {"offset", offset}, {"multiplier", multiplier}};
      ++resolved;
    }
    if (resolved)
      active_reader->Warn(std::to_string(resolved) + " URDF mimic joint(s) follow another mimic joint; "
                              "written against the chain's leading joint",
                          "urdf.mimic_chain_resolved");
  }
  for (auto transmission : root.children("transmission")) {
    auto joint = transmission.child("joint"),
         act = transmission.child("actuator");
    std::string target = Attr(joint, "name");
    if (!joint_names.count(target)) {
      active_reader->Warn("URDF transmission has unknown joint " + target);
      continue;
    }
    if (opts.profile != "legacy") {
      // Neither URDF importer of Newton nor urdf-usd-converter infers a
      // controller from a transmission (and Newton's add_usd rejects
      // actuators on joints that are not single-DOF): keep it as urdf:*
      // metadata on the joint.
      Json* owner = nullptr;
      for (auto& j : joints)
        if (j.contains("name") && j["name"].is_string() && j["name"].get<std::string>() == target)
          owner = &j;
      if (!owner) continue;
      if (owner->contains("transmission")) {
        active_reader->Warn("URDF joint " + target + " has several transmissions; keeping the first",
                            "urdf.transmission_duplicate");
        continue;
      }
      Json t = Json::object();
      auto text = [](const pugi::xml_node& n, const char* tag) {
        std::string v = n.child(tag).text().as_string();
        const size_t b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
      };
      if (HasAttr(transmission, "name")) t["name"] = Attr(transmission, "name");
      std::string type = text(transmission, "type");
      if (type.empty()) type = Attr(transmission, "type");
      if (!type.empty()) t["type"] = type;
      if (act && HasAttr(act, "name")) t["actuator"] = Attr(act, "name");
      std::string hardware = text(joint, "hardwareInterface");
      if (hardware.empty()) hardware = text(act, "hardwareInterface");
      if (!hardware.empty()) t["hardwareInterface"] = hardware;
      if (act && act.child("mechanicalReduction"))
        t["mechanicalReduction"] = act.child("mechanicalReduction").text().as_double(1.0);
      (*owner)["transmission"] = std::move(t);
      continue;
    }
    Json a{{"name", Attr(act, "name", target + "_actuator")},
           {"joint", target},
           {"control", "pd"}};
    active_reader->Warn(
        "URDF transmission hardware/reduction is retained as metadata; no "
        "controller dynamics are inferred");
    a["mechanicalReduction"] =
        act.child("mechanicalReduction").text().as_double(1.0);
    actuators.push_back(std::move(a));
  }
  for (auto& m : *active_materials) materials.push_back(m);
  *payload = Json{{"name", Attr(root, "name", "robot")},
                  {"upAxis", opts.up_axis}, {"profile", opts.profile},
                  {"layout", opts.layout}, {"sourceFormat", "urdf"},
                  {"gravity", opts.up_axis == "Z" ? Json::array({0, 0, -1})
                                                  : Json::array({0, -1, 0})}};
  // Assign the large arrays by move (a braced list would copy them).
  (*payload)["links"] = std::move(links);
  (*payload)["joints"] = std::move(joints);
  (*payload)["materials"] = std::move(materials);
  (*payload)["actuators"] = std::move(actuators);
  if (opts.profile != "legacy") {
    // Robot-level extensions (<gazebo>, <ros2_control>, ...).
    // (Root attributes are name/version and xmlns declarations.)
    Json custom = URDFCustomJson(root, {"link", "joint", "material", "transmission"}, {});
    if (custom.contains("elements"))
      (*payload)["urdfCustom"] = Json{{"elements", std::move(custom["elements"])}};
  }
  return true;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
