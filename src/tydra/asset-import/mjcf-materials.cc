// SPDX-License-Identifier: Apache-2.0
// Builtin textures, materials, skins and keyframes.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
// Generate a builtin texture as PNG into the reader's texture cache under
// "textures/builtin_<name>.png": type="2d" as MuJoCo's Builtin2D, and (with
// `skybox`) a type="skybox" cube resampled to a latitude-longitude image for
// a DomeLight. Cube textures on materials have no UsdPreviewSurface mapping
// and are reported instead; the legacy profile keeps its historical output.
bool GenerateBuiltinTexture(const pugi::xml_node& node, const std::string& name, bool skybox,
                            std::string* key, std::string* why) {
  constexpr int64_t kMaxBuiltinPixels = int64_t(4096) * 4096;
  Reader& reader = *active_reader;
  if (reader.options.profile == "legacy") {
    *why = "legacy profile";
    return false;
  }
  const std::string type = HasAttr(node, "type") ? Attr(node, "type") : "cube";
  if (type != "2d" && !(skybox && type == "skybox")) {
    *why = "type=\"" + type + "\" is not supported";
    return false;
  }
  BuiltinTexture t;
  t.builtin = Attr(node, "builtin");
  if (t.builtin != "checker" && t.builtin != "gradient" && t.builtin != "flat") {
    *why = "unknown builtin \"" + t.builtin + "\"";
    return false;
  }
  t.mark = HasAttr(node, "mark") ? Attr(node, "mark") : "none";
  const auto color = [&](const char* attr, double* out) {
    const auto v = ParseDoubles(Attr(node, attr));
    if (v.size() >= 3) std::copy(v.begin(), v.begin() + 3, out);
  };
  color("rgb1", t.rgb1);
  color("rgb2", t.rgb2);
  color("markrgb", t.markrgb);
  t.random = ParseDoubleAttr(node, "random", 0.01);
  const bool cube = type != "2d";
  const double w = ParseDoubleAttr(node, "width", 0);
  // A cube texture is width x 6*width; MuJoCo ignores its height.
  const double h = cube ? 6 * w : ParseDoubleAttr(node, "height", 0);
  if (!(w >= 1 && h >= 1) || w * h > double(kMaxBuiltinPixels) || w != std::floor(w) ||
      h != std::floor(h)) {
    *why = "width/height must be positive integers with at most 4096x4096 pixels";
    return false;
  }
  t.width = static_cast<int>(w);
  t.height = static_cast<int>(h);
  // Latitude-longitude output: 4w x 2w, at most 4096 x 2048.
  const int lat_w = cube ? std::min(4 * t.width, 4096) : t.width;
  const int lat_h = cube ? std::max(1, lat_w / 2) : t.height;
  std::string err;
  const size_t pixels = size_t(t.width) * t.height * 3 + (cube ? size_t(lat_w) * lat_h * 3 : 0);
  if (!reader.Charge(pixels, &err)) {
    *why = err;
    return false;
  }
  std::vector<uint8_t> rgb, png;
  if (cube) {
    std::vector<uint8_t> faces;
    GenerateBuiltinCube(t, &faces);
    CubeToLatLong(faces, t.width, lat_w, lat_h, &rgb);
  } else {
    GenerateBuiltin2D(t, &rgb);
  }
  EncodePNG(rgb.data(), uint32_t(lat_w), uint32_t(lat_h), 3, &png);
  if (!reader.Charge(png.size(), &err)) {
    *why = err;
    return false;
  }
  std::string file = name.empty() ? (cube ? "skybox" : "builtin") : name;
  for (char& c : file)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
  *key = "textures/builtin_" + file + ".png";
  for (int k = 1; reader.textures.count(*key); ++k)
    *key = "textures/builtin_" + file + "_" + std::to_string(k) + ".png";
  reader.textures[*key] = std::make_shared<const std::vector<uint8_t>>(std::move(png));
  return true;
}

// AddMaterialFromJson -> UsdShade Material (UsdPreviewSurface).
// Color/PBR-scalar only; texture maps are a documented follow-on.
// MJCF <asset><material> -> JSON material entries consumed by the converter's
// AddMaterialFromJson -> UsdShade Material (UsdPreviewSurface). The first
// builtin skybox texture becomes `skybox` (a DomeLight in the converter).
void AddMujocoMaterialsJson(const pugi::xml_node& root,
                            const fs::path& base_dir, Json* materials,
                            Json* skybox, Stats* stats) {
  if (!materials) return;
  const auto asset = Child(root, "asset");
  if (!asset) return;
  // Resolve <texture name file=...> to an absolute path so a material's
  // `texture` becomes a UsdUVTexture in the converter. Builtin (file-less)
  // textures are skipped (no image to reference).
  const fs::path tex_dir = CompilerAssetDir(root, base_dir, "texturedir");
  std::map<std::string, std::string> tex_files;
  std::set<std::string> generated, referenced;
  for (const auto& m_node : Children(asset, "material"))
    if (HasAttr(m_node, "texture")) referenced.insert(Attr(m_node, "texture"));
  for (const auto& t_node : Children(asset, "texture")) {
    const std::string tname = Attr(t_node, "name");
    const std::string tfile = Attr(t_node, "file");
    if (tfile.empty() && HasAttr(t_node, "builtin") && Attr(t_node, "builtin") != "none" &&
        active_reader) {
      // Material textures and the first skybox (MuJoCo renders only that
      // one) map onto USD; unused textures are skipped without pixels.
      const bool is_skybox = Attr(t_node, "type") == "skybox";
      if (!referenced.count(tname) && !is_skybox) continue;
      if (is_skybox && skybox && !skybox->empty()) continue;
      std::string key, why;
      if (GenerateBuiltinTexture(t_node, tname, is_skybox && skybox, &key, &why)) {
        if (is_skybox && skybox) {
          *skybox = {{"name", tname.empty() ? std::string("skybox") : tname},
                     {"texture", key}, {"texture_abs", key}, {"texture_generated", true}};
        } else {
          tex_files[tname] = key;
          generated.insert(key);
        }
      } else {
        active_reader->Warn("MJCF builtin texture" +
                                (tname.empty() ? std::string() : " `" + tname + "`") +
                                " is not generated (" + why +
                                (is_skybox ? ")" : "); materials using it keep their rgba"),
                            "mjcf.texture_builtin_unsupported");
      }
      continue;
    }
    if (!tfile.empty() && Attr(t_node, "type") == "skybox" && skybox && skybox->empty() &&
        active_reader && active_reader->options.profile != "legacy") {
      active_reader->Warn("MJCF skybox texture" +
                              (tname.empty() ? std::string() : " `" + tname + "`") +
                              " is not converted (file-based skyboxes need a cube layout)",
                          "mjcf.texture_builtin_unsupported");
      continue;
    }
    if (tname.empty() || tfile.empty()) continue;
    tex_files[tname] =
        ResolveMeshPath(base_dir, tex_dir, tfile).string();
  }
  for (const auto& m_node : Children(asset, "material")) {
    const std::string name = Attr(m_node, "name");
    if (name.empty()) continue;
    Json mat = {{"name", name}};
    const bool legacy = !active_reader || active_reader->options.profile == "legacy";
    auto rgba = ParseDoubles(Attr(m_node, "rgba"));
    if (!legacy) {
      // As mujoco-usd-converter: MuJoCo's material color (default white) is
      // sRGB, UsdPreviewSurface's diffuseColor linear; roughness is
      // 1 - shininess (default 0.5) unless set explicitly.
      if (rgba.size() < 4) rgba = {1.0, 1.0, 1.0, 1.0};
      for (size_t i = 0; i < 3; ++i) {
        const double c = rgba[i];
        rgba[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
      }
      if (!HasAttr(m_node, "roughness"))
        mat["roughness"] = 1.0 - ParseDoubleAttr(m_node, "shininess", 0.5);
    }
    if (rgba.size() >= 4)
      mat["rgba"] = Json::array({rgba[0], rgba[1], rgba[2], rgba[3]});
    if (HasAttr(m_node, "metallic"))
      mat["metallic"] = ParseDoubleAttr(m_node, "metallic", 0.0);
    if (HasAttr(m_node, "roughness"))
      mat["roughness"] = ParseDoubleAttr(m_node, "roughness", 0.5);
    if (HasAttr(m_node, "specular"))
      mat["specular"] = ParseDoubleAttr(m_node, "specular", 0.0);
    if (HasAttr(m_node, "emission"))
      mat["emission"] = ParseDoubleAttr(m_node, "emission", 0.0);
    if (HasAttr(m_node, "reflectance"))
      mat["reflectance"] = ParseDoubleAttr(m_node, "reflectance", 0.0);
    const auto repeat = ParseDoubles(Attr(m_node, "texrepeat"));
    if (repeat.size() >= 2 && (repeat[0] != 1.0 || repeat[1] != 1.0))
      mat["texrepeat"] = Json::array({repeat[0], repeat[1]});
    const std::string tref = Attr(m_node, "texture");
    if (!tref.empty()) {
      const auto it = tex_files.find(tref);
      if (it != tex_files.end() && generated.count(it->second)) {
        // Retained in the reader's texture cache; written beside the output
        // layer on save and packaged into USDZ.
        mat["texture"] = it->second;
        mat["texture_abs"] = it->second;
        mat["texture_generated"] = true;
      } else if (it != tex_files.end()) {
        if (active_reader && (active_reader->options.read || active_reader->options.open)) {
          // Resolver identifiers are not filesystem paths. Keep the authored
          // texture identifier anchored to the source instead of absolutizing
          // a URI against the process working directory.
          mat["texture"] = it->second;
          mat["texture_abs"] = it->second;
          materials->push_back(std::move(mat));
          if (stats) stats->materials++;
          continue;
        }
        // Emit a portable, source-relative reference (e.g. "assets/foo.png")
        // for inputs:file so the exported usda/usdc opens beside its assets
        // instead of carrying a machine-specific absolute path. Keep the
        // absolute source in `texture_abs` (ignored by the converter) so usdz
        // export can embed the bytes into the package.
        std::error_code ec;
        const fs::path abs_tex = fs::absolute(it->second, ec);
        const fs::path rel = fs::relative(abs_tex, base_dir, ec);
        const std::string rel_s = rel.generic_string();
        mat["texture"] = (ec || rel_s.empty() || rel_s.rfind("..", 0) == 0)
                             ? abs_tex.filename().string()
                             : rel_s;
        mat["texture_abs"] = abs_tex.string();
      }
    }
    materials->push_back(std::move(mat));
    if (stats) stats->materials++;
  }
}

bool ParseSKN(const std::vector<uint8_t>& bytes, MjcfSkinData* skin, std::string* err) {
  auto word = [&](size_t index, void* out) { std::memcpy(out, bytes.data() + 4 * index, 4); };
  if (bytes.size() < 16 || bytes.size() % 4) {
    *err = "SKN file is too small or not a whole number of 4-byte words";
    return false;
  }
  const size_t words = bytes.size() / 4;
  int32_t n[4];
  for (int i = 0; i < 4; ++i) word(size_t(i), &n[i]);
  if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[3] < 0) {
    *err = "negative size in SKN header";
    return false;
  }
  size_t at = 4;
  const size_t need = 3 * size_t(n[0]) + 2 * size_t(n[1]) + 3 * size_t(n[2]) + 18 * size_t(n[3]);
  if (need > words - at) {
    *err = "insufficient data in SKN file";
    return false;
  }
  auto floats = [&](std::vector<float>* out, size_t count) {
    out->resize(count);
    if (count) std::memcpy(out->data(), bytes.data() + 4 * at, 4 * count);
    at += count;
  };
  floats(&skin->vert, 3 * size_t(n[0]));
  floats(&skin->texcoord, 2 * size_t(n[1]));
  skin->face.resize(3 * size_t(n[2]));
  if (!skin->face.empty()) std::memcpy(skin->face.data(), bytes.data() + 4 * at, 4 * skin->face.size());
  at += skin->face.size();
  for (int32_t b = 0; b < n[3]; ++b) {
    if (words - at < 18) {
      *err = "insufficient data in SKN file, bone " + std::to_string(b);
      return false;
    }
    char name[40];
    std::memcpy(name, bytes.data() + 4 * at, 39);
    name[39] = '\0';
    skin->body.emplace_back(name);
    at += 10;
    std::vector<float> pose;
    floats(&pose, 7);
    skin->bindpos.insert(skin->bindpos.end(), pose.begin(), pose.begin() + 3);
    skin->bindquat.insert(skin->bindquat.end(), pose.begin() + 3, pose.end());
    int32_t count = 0;
    word(at++, &count);
    if (count < 1 || size_t(count) > (words - at) / 2) {
      *err = "invalid vertex count in SKN file, bone " + std::to_string(b);
      return false;
    }
    skin->vertid.emplace_back(size_t(count));
    std::memcpy(skin->vertid.back().data(), bytes.data() + 4 * at, 4 * size_t(count));
    at += size_t(count);
    skin->vertweight.emplace_back();
    floats(&skin->vertweight.back(), size_t(count));
  }
  if (at != words) {
    *err = "unexpected buffer size in SKN file";
    return false;
  }
  return true;
}

// MJCF <skin> (in <asset> or <deformable>) as MuJoCo's compiler checks and
// normalizes it (mjCSkin::Compile): per-vertex weights normalized over all
// bones, bind quaternions normalized. The payload holds the bind-pose mesh,
// each bone's body and bind transform, and per-vertex joint influences (bone
// indices, zero-weight padded to the largest bone count of a vertex).
bool AddMujocoSkinsJson(const pugi::xml_node& root, const fs::path& base_dir, Json* skins,
                        std::string* err) {
  const fs::path mesh_dir = CompilerAssetDir(root, base_dir, "meshdir");
  std::vector<pugi::xml_node> nodes;
  for (const char* parent : {"asset", "deformable"})
    for (const auto& p : Children(root, parent))
      for (const auto& s : Children(p, "skin")) nodes.push_back(s);
  for (const auto& node : nodes) {
    const std::string file = Attr(node, "file");
    std::string name = Attr(node, "name");
    if (name.empty() && !file.empty()) name = fs::path(file).stem().string();
    auto fail = [&](const std::string& why) {
      if (err) *err = "MJCF skin `" + name + "`: " + why;
      return false;
    };
    MjcfSkinData skin;
    std::vector<pugi::xml_node> bones;
    for (const auto& bone : Children(node, "bone")) bones.push_back(bone);
    if (!file.empty()) {
      if (HasAttr(node, "vertex") || HasAttr(node, "texcoord") || HasAttr(node, "face") ||
          !bones.empty())
        return fail("both skin data and a file are specified");
      if (ToLower(fs::path(file).extension().string()) != ".skn")
        return fail("unknown skin file type " + file);
      std::vector<uint8_t> bytes;
      std::string resolved, why;
      if (!active_reader->Read(ResolveMeshPath(base_dir, mesh_dir, file).string(),
                               active_reader->root_source, &bytes, &resolved, &why))
        return fail(why);
      if (!ParseSKN(bytes, &skin, &why)) return fail(why);
      bool finite = AllFinite(skin.vert) && AllFinite(skin.texcoord) &&
                    AllFinite(skin.bindpos) && AllFinite(skin.bindquat);
      for (const auto& weights : skin.vertweight) finite = finite && AllFinite(weights);
      if (!finite) return fail("SKN file holds non-finite values");
    } else {
      for (double v : ParseDoubles(Attr(node, "vertex"))) skin.vert.push_back(float(v));
      for (double v : ParseDoubles(Attr(node, "texcoord"))) skin.texcoord.push_back(float(v));
      for (double v : ParseDoubles(Attr(node, "face"))) {
        if (!(v >= 0 && v <= double(INT32_MAX)) || v != std::floor(v)) return fail("invalid face index");
        skin.face.push_back(int32_t(v));
      }
      for (const auto& bone : bones) {
        const auto pos = ParseDoubles(Attr(bone, "bindpos"));
        const auto quat = ParseDoubles(Attr(bone, "bindquat"));
        if (Attr(bone, "body").empty() || pos.size() != 3 || quat.size() != 4)
          return fail("a bone needs body, bindpos and bindquat");
        skin.body.push_back(Attr(bone, "body"));
        for (double v : pos) skin.bindpos.push_back(float(v));
        for (double v : quat) skin.bindquat.push_back(float(v));
        skin.vertid.emplace_back();
        for (double v : ParseDoubles(Attr(bone, "vertid"))) {
          if (!(v >= double(INT32_MIN) && v <= double(INT32_MAX)) || v != std::floor(v))
            return fail("invalid vertid");
          skin.vertid.back().push_back(int32_t(v));
        }
        skin.vertweight.emplace_back();
        for (double v : ParseDoubles(Attr(bone, "vertweight"))) skin.vertweight.back().push_back(float(v));
      }
    }
    if (skin.vert.empty() || skin.face.empty() || skin.body.empty()) return fail("missing data in skin");
    if (skin.vert.size() % 3) return fail("vertex data must be a multiple of 3");
    const size_t nvert = skin.vert.size() / 3;
    if (!skin.texcoord.empty() && skin.texcoord.size() != 2 * nvert)
      return fail("vertex and texcoord data have incompatible sizes");
    if (skin.face.size() % 3) return fail("face data must be a multiple of 3");
    for (int32_t f : skin.face)
      if (f < 0 || size_t(f) >= nvert) return fail("face index out of range");
    if (!active_reader->ChargeArray(skin.vert.size() + skin.face.size() + skin.texcoord.size(), 12, err))
      return false;
    // Normalized weights, accumulated per vertex across bones.
    std::vector<float> total(nvert, 0.0f);
    std::vector<uint32_t> count(nvert, 0);
    for (size_t b = 0; b < skin.body.size(); ++b) {
      if (skin.vertweight[b].size() != skin.vertid[b].size() || skin.vertid[b].empty())
        return fail("vertid and vertweight must have the same non-zero size");
      for (size_t j = 0; j < skin.vertid[b].size(); ++j) {
        const int32_t v = skin.vertid[b][j];
        if (v < 0 || size_t(v) >= nvert) return fail("vertid " + std::to_string(v) + " out of range");
        total[size_t(v)] += skin.vertweight[b][j];
        ++count[size_t(v)];
      }
    }
    uint32_t influences = 0;
    for (size_t v = 0; v < nvert; ++v) {
      if (!(total[v] > 1e-15f)) return fail("vertex " + std::to_string(v) + " must have positive total weight");
      influences = std::max(influences, count[v]);
    }
    if (nvert > SIZE_MAX / influences || !active_reader->ChargeArray(nvert * influences, 8, err))
      return false;
    std::vector<int32_t> joint_indices(nvert * influences, 0);
    std::vector<float> joint_weights(nvert * influences, 0.0f);
    std::vector<uint32_t> filled(nvert, 0);
    Json bone_json = Json::array();
    for (size_t b = 0; b < skin.body.size(); ++b) {
      for (size_t j = 0; j < skin.vertid[b].size(); ++j) {
        const size_t v = size_t(skin.vertid[b][j]);
        const size_t slot = v * influences + filled[v]++;
        joint_indices[slot] = int32_t(b);
        joint_weights[slot] = skin.vertweight[b][j] / total[v];
      }
      double q[4] = {skin.bindquat[4 * b], skin.bindquat[4 * b + 1], skin.bindquat[4 * b + 2],
                     skin.bindquat[4 * b + 3]};
      const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (!(norm > 0)) return fail("bone " + skin.body[b] + " has a zero bindquat");
      Quat quat;
      for (int k = 0; k < 4; ++k) quat[size_t(k)] = double(float(q[k] / norm));
      const std::array<double, 3> pos{{skin.bindpos[3 * b], skin.bindpos[3 * b + 1],
                                       skin.bindpos[3 * b + 2]}};
      bone_json.push_back({{"body", skin.body[b]}, {"bindTransform", Array(MatrixFromPosQuat(pos, quat))}});
    }
    const double inflate = ParseDoubleAttr(node, "inflate", 0.0);
    Json geometry = {{"positions", Array(skin.vert)}, {"indices", Array(skin.face)},
                     {"jointIndices", Array(joint_indices)}, {"jointWeights", Array(joint_weights)},
                     {"influences", influences}};
    if (!skin.texcoord.empty()) geometry["uvs"] = Array(skin.texcoord);
    Json entry = {{"name", name.empty() ? std::string("skin") : name},
                  {"geometry", std::move(geometry)},
                  {"bones", std::move(bone_json)}};
    const auto rgba = ParseDoubles(Attr(node, "rgba", "0.5 0.5 0.5 1"));
    if (rgba.size() >= 4) entry["rgba"] = Array(rgba);
    if (!Attr(node, "material").empty()) entry["material"] = Attr(node, "material");
    entry["inflate"] = inflate;
    entry["group"] = ToInt32(ParseDoubleAttr(node, "group", 0.0), "group");
    skins->push_back(std::move(entry));
  }
  return true;
}

void AddMujocoKeyframesJson(const pugi::xml_node& root, Json* keyframes,
                            Stats* stats) {
  if (!keyframes) return;
  const auto kf_root = Child(root, "keyframe");
  if (!kf_root) return;
  for (const auto& key : Children(kf_root, "key")) {
    Json k = Json::object();
    k["name"] = Attr(key, "name", "key_" + std::to_string(keyframes->size()));
    auto arr = [&](const char* attr) {
      auto v = ParseDoubles(Attr(key, attr));
      if (!v.empty()) k[attr] = Array(v);
    };
    arr("qpos");
    arr("qvel");
    arr("act");
    arr("ctrl");
    arr("mpos");
    arr("mquat");
    if (HasAttr(key, "time")) k["time"] = ParseDoubleAttr(key, "time", 0.0);
    keyframes->push_back(std::move(k));
    if (stats) stats->keyframes++;
  }
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
