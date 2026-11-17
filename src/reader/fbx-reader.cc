// SPDX-License-Identifier: Apache-2.0
#include "fbx-reader.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "external/ufbx/ufbx.h"

namespace lightusd {
namespace {
using skel::Mat;

Mat ToMat(const ufbx_matrix& m) {
  // ufbx: column vectors (cols[0..2] basis, cols[3] translation) -> USD rows.
  return {m.cols[0].x, m.cols[0].y, m.cols[0].z, 0, m.cols[1].x, m.cols[1].y, m.cols[1].z, 0,
          m.cols[2].x, m.cols[2].y, m.cols[2].z, 0, m.cols[3].x, m.cols[3].y, m.cols[3].z, 1};
}

std::string Str(const ufbx_string& s) { return std::string(s.data, s.length); }

// Joint world transforms (row form) of `scene`'s copy of the joints.
std::vector<Mat> JointWorlds(const ufbx_scene* scene, const std::vector<uint32_t>& node_ids) {
  std::vector<Mat> w;
  w.reserve(node_ids.size());
  for (uint32_t id : node_ids) w.push_back(ToMat(scene->nodes.data[id]->node_to_world));
  return w;
}

// Parent-relative transforms: L = W_joint * inverse(W_parent) (row vectors).
std::vector<Mat> Locals(const std::vector<Mat>& worlds, const std::vector<int>& parent) {
  std::vector<Mat> out(worlds.size());
  for (size_t i = 0; i < worlds.size(); ++i) {
    if (parent[i] < 0) {
      out[i] = worlds[i];
      continue;
    }
    Mat inv;
    if (!skel::Invert(worlds[size_t(parent[i])], &inv)) inv = skel::Identity();
    out[i] = skel::Mul(worlds[i], inv);
  }
  return out;
}

void Set(PrimSpec* p, const std::string& name, Value value, const std::string& type,
         bool uniform = false) {
  p->upsert_property(name, std::move(value), uniform ? PropSlot::kFlagUniform : 0);
  p->set_property_type_name(name, type);
}
void Interpolation(PrimSpec* p, const std::string& name, const std::string& value,
                   int elements = 1) {
  auto& m = p->ensure_property_meta(name);
  m.interpolation = value;
  m.elementSize = elements;
  m.authored |= PropMeta::kInterpolation | PropMeta::kElementSize;
}
PrimSpec* Define(Layer* layer, const std::string& path, const std::string& type) {
  auto id = layer->define_prim_at_path(path, type);
  return id == UINT32_MAX ? nullptr : layer->prim_mutable(id);
}
std::string Identifier(const std::string& s) {
  std::string out;
  for (unsigned char c : s) out += (std::isalnum(c) || c == '_') ? char(c) : '_';
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
  return out;
}
}  // namespace

bool ReadFBXScene(const uint8_t* data, size_t size, const std::string& take, bool with_meshes,
                  FBXScene* out, std::string* warn, std::string* err, size_t max_frames) {
  *out = FBXScene{};
  ufbx_load_opts opts = {};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0;
  opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
  ufbx_error uerr;
  ufbx_scene* scene = ufbx_load_memory(data, size, &opts, &uerr);
  if (!scene) {
    if (err) *err = "FBX: " + Str(uerr.description);
    return false;
  }
  struct Free {
    ufbx_scene* s;
    ~Free() { ufbx_free_scene(s); }
  } free_scene{scene};

  // Joints: bone nodes and skin-cluster bones, parents first (node DFS order).
  std::set<const ufbx_node*> bones;
  for (size_t i = 0; i < scene->nodes.count; ++i)
    if (scene->nodes.data[i]->bone) bones.insert(scene->nodes.data[i]);
  for (size_t i = 0; i < scene->skin_deformers.count; ++i) {
    const ufbx_skin_deformer* sk = scene->skin_deformers.data[i];
    for (size_t c = 0; c < sk->clusters.count; ++c)
      if (sk->clusters.data[c]->bone_node) bones.insert(sk->clusters.data[c]->bone_node);
  }
  std::vector<uint32_t> joint_nodes;  // element ids into scene->nodes
  std::map<const ufbx_node*, int> joint_of;
  std::vector<const ufbx_node*> stack{scene->root_node};
  while (!stack.empty()) {  // iterative pre-order DFS
    const ufbx_node* n = stack.back();
    stack.pop_back();
    if (bones.count(n)) {
      int parent = -1;
      for (const ufbx_node* a = n->parent; a; a = a->parent) {
        auto it = joint_of.find(a);
        if (it != joint_of.end()) {
          parent = it->second;
          break;
        }
      }
      joint_of[n] = static_cast<int>(joint_nodes.size());
      joint_nodes.push_back(n->typed_id);
      out->skeleton.names.push_back(Str(n->name));
      out->skeleton.parent.push_back(parent);
    }
    for (size_t c = n->children.count; c-- > 0;) stack.push_back(n->children.data[c]);
  }
  const size_t nj = joint_nodes.size();
  const std::vector<Mat> rest_world = JointWorlds(scene, joint_nodes);
  out->skeleton.rest = Locals(rest_world, out->skeleton.parent);
  out->skeleton.bind = rest_world;
  std::vector<char> bound(nj, 0);

  // Meshes (and bind transforms from their skin clusters).
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* node = scene->nodes.data[i];
    const ufbx_mesh* mesh = node->mesh;
    if (!mesh) continue;
    const ufbx_matrix& g2w = node->geometry_to_world;
    const ufbx_skin_deformer* skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : nullptr;
    if (skin)
      for (size_t c = 0; c < skin->clusters.count; ++c) {
        const ufbx_skin_cluster* cl = skin->clusters.data[c];
        auto it = joint_of.find(cl->bone_node);
        if (it == joint_of.end() || bound[size_t(it->second)]) continue;
        Mat inv;
        if (!skel::Invert(ToMat(cl->geometry_to_bone), &inv)) continue;
        // bind = geometry_to_world * inverse(geometry_to_bone) (column form)
        out->skeleton.bind[size_t(it->second)] = skel::Mul(inv, ToMat(g2w));
        bound[size_t(it->second)] = 1;
      }
    if (!with_meshes) continue;
    FBXMesh m;
    m.name = Str(node->name);
    for (size_t v = 0; v < mesh->vertices.count; ++v) {
      const ufbx_vec3 p = ufbx_transform_position(&g2w, mesh->vertices.data[v]);
      m.points.insert(m.points.end(), {float(p.x), float(p.y), float(p.z)});
    }
    for (size_t f = 0; f < mesh->faces.count; ++f) {
      const ufbx_face face = mesh->faces.data[f];
      if (face.num_indices < 3) continue;
      m.counts.push_back(static_cast<int32_t>(face.num_indices));
      for (uint32_t k = 0; k < face.num_indices; ++k) {
        const uint32_t ix = face.index_begin + k;
        m.indices.push_back(static_cast<int32_t>(mesh->vertex_indices.data[ix]));
        if (mesh->vertex_normal.exists)
          m.normal_indices.push_back(static_cast<int32_t>(mesh->vertex_normal.indices.data[ix]));
        if (mesh->vertex_uv.exists)
          m.uv_indices.push_back(static_cast<int32_t>(mesh->vertex_uv.indices.data[ix]));
      }
    }
    if (mesh->vertex_normal.exists) {
      const ufbx_matrix nm = ufbx_matrix_for_normals(&g2w);
      for (size_t k = 0; k < mesh->vertex_normal.values.count; ++k) {
        ufbx_vec3 n = ufbx_transform_direction(&nm, mesh->vertex_normal.values.data[k]);
        const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 0) n.x /= len, n.y /= len, n.z /= len;
        m.normals.insert(m.normals.end(), {float(n.x), float(n.y), float(n.z)});
      }
    }
    if (mesh->vertex_uv.exists)
      for (size_t k = 0; k < mesh->vertex_uv.values.count; ++k)
        m.uvs.insert(m.uvs.end(), {float(mesh->vertex_uv.values.data[k].x),
                                   float(mesh->vertex_uv.values.data[k].y)});
    if (skin && nj) {
      uint32_t inf = 0;
      for (size_t v = 0; v < mesh->vertices.count && v < skin->vertices.count; ++v)
        inf = std::max(inf, skin->vertices.data[v].num_weights);
      m.influences = static_cast<int32_t>(inf);
      m.joint_indices.assign(mesh->vertices.count * inf, 0);
      m.joint_weights.assign(mesh->vertices.count * inf, 0.0f);
      for (size_t v = 0; v < mesh->vertices.count && v < skin->vertices.count; ++v) {
        const ufbx_skin_vertex sv = skin->vertices.data[v];
        for (uint32_t k = 0; k < sv.num_weights; ++k) {
          const ufbx_skin_weight w = skin->weights.data[sv.weight_begin + k];
          auto it = joint_of.find(skin->clusters.data[w.cluster_index]->bone_node);
          if (it == joint_of.end()) continue;
          m.joint_indices[v * inf + k] = it->second;
          m.joint_weights[v * inf + k] = static_cast<float>(w.weight);
        }
        // UsdSkel expects normalized weights (FBX files often do not store
        // them normalized; DCCs normalize when deforming).
        float sum = 0.0f;
        for (uint32_t k = 0; k < inf; ++k) sum += m.joint_weights[v * inf + k];
        if (sum > 0.0f)
          for (uint32_t k = 0; k < inf; ++k) m.joint_weights[v * inf + k] /= sum;
      }
    }
    out->meshes.push_back(std::move(m));
  }

  // Animation: sample the chosen stack at the scene frame rate.
  const ufbx_anim_stack* stack_sel = nullptr;
  for (size_t i = 0; i < scene->anim_stacks.count; ++i) {
    const ufbx_anim_stack* s = scene->anim_stacks.data[i];
    if (take.empty() || Str(s->name) == take) {
      stack_sel = s;
      break;
    }
  }
  if (!take.empty() && !stack_sel && warn)
    *warn += "FBX: animation '" + take + "' not found\n";
  if (stack_sel && nj) {
    const double fps = scene->settings.frames_per_second > 0 ? scene->settings.frames_per_second : 24.0;
    const double f0 = std::round(stack_sel->time_begin * fps);
    const double f1 = std::round(stack_sel->time_end * fps);
    size_t frames = f1 >= f0 ? size_t(f1 - f0) + 1 : 0;
    if (frames > max_frames) {
      if (warn) *warn += "FBX: animation truncated to " + std::to_string(max_frames) + " frames\n";
      frames = max_frames;
    }
    out->animation.name = Str(stack_sel->name);
    out->animation.fps = fps;
    for (size_t k = 0; k < frames; ++k) {
      const double frame = f0 + double(k);
      ufbx_scene* ev = ufbx_evaluate_scene(scene, stack_sel->anim, frame / fps, nullptr, &uerr);
      if (!ev) {
        if (err) *err = "FBX: " + Str(uerr.description);
        return false;
      }
      const std::vector<Mat> locals = Locals(JointWorlds(ev, joint_nodes), out->skeleton.parent);
      ufbx_free_scene(ev);
      std::vector<float> t(3 * nj), r(4 * nj), s(3 * nj);
      for (size_t j = 0; j < nj; ++j) skel::Decompose(locals[j], &t[3 * j], &r[4 * j], &s[3 * j]);
      out->animation.times.push_back(frame);
      out->animation.translations.push_back(std::move(t));
      out->animation.rotations.push_back(std::move(r));
      out->animation.scales.push_back(std::move(s));
    }
    out->has_animation = !out->animation.times.empty();
  }
  return true;
}

std::shared_ptr<Layer> LoadFBXLayerFromMemory(const std::string& source, const uint8_t* data,
                                              size_t size,
                                              const tydra::import_asset::Options& options,
                                              std::string* warn, std::string* err) {
  if ((!data && size) || size > options.max_input || size > options.max_memory) {
    if (err) *err = !data && size ? "Null FBX data" : "FBX input limit exceeded";
    return nullptr;
  }
  FBXScene fbx;
  std::string w;
  if (!ReadFBXScene(data, size, "", true, &fbx, &w, err)) return nullptr;
  if (warn) *warn += w;
  auto layer = std::make_shared<Layer>();
  layer->meta().defaultPrim = "World";
  layer->meta().upAxis = "Y";
  layer->meta().upAxis_set = true;
  layer->meta().metersPerUnit = 1.0;
  layer->meta().metersPerUnit_set = true;
  const bool skinned = !fbx.skeleton.names.empty();
  if (!Define(layer.get(), "/World", skinned ? "SkelRoot" : "Xform")) {
    if (err) *err = "Cannot define /World";
    return nullptr;
  }
  const std::string skel_path = "/World/Skel";
  if (skinned &&
      !skel::AuthorSkeleton(layer.get(), skel_path, fbx.skeleton,
                            fbx.has_animation ? &fbx.animation : nullptr, err))
    return nullptr;
  Define(layer.get(), "/World/Geometry", "Xform");
  std::set<std::string> used;
  for (FBXMesh& m : fbx.meshes) {
    std::string name = Identifier(m.name.empty() ? "mesh" : m.name);
    for (int k = 2; used.count(name); ++k) name = Identifier(m.name) + "_" + std::to_string(k);
    used.insert(name);
    PrimSpec* p = Define(layer.get(), "/World/Geometry/" + name, "Mesh");
    if (!p) {
      if (err) *err = "Cannot define FBX mesh prim";
      return nullptr;
    }
    p->meta().displayName() = m.name;
    const size_t np = m.points.size() / 3;
    Set(p, "points", Value::MakeFloatCompArray(m.points, TypeId::Point3f, 3), "point3f[]");
    Set(p, "faceVertexCounts", Value::MakeIntArray(m.counts), "int[]");
    Set(p, "faceVertexIndices", Value::MakeIntArray(m.indices), "int[]");
    Set(p, "subdivisionScheme", Value::MakeToken("none"), "token", true);
    if (!m.normal_indices.empty() && m.normal_indices.size() == m.indices.size()) {
      Set(p, "primvars:normals", Value::MakeFloatCompArray(m.normals, TypeId::Normal3f, 3),
          "normal3f[]");
      Interpolation(p, "primvars:normals", "faceVarying");
      Set(p, "primvars:normals:indices", Value::MakeIntArray(m.normal_indices), "int[]");
    }
    if (!m.uv_indices.empty() && m.uv_indices.size() == m.indices.size()) {
      Set(p, "primvars:st", Value::MakeFloatCompArray(m.uvs, TypeId::Texcoord2f, 2),
          "texCoord2f[]");
      Interpolation(p, "primvars:st", "faceVarying");
      Set(p, "primvars:st:indices", Value::MakeIntArray(m.uv_indices), "int[]");
    }
    if (skinned && m.influences > 0 && m.joint_weights.size() == np * size_t(m.influences)) {
      skel::BindMesh(p, skel_path);
      Set(p, "primvars:skel:jointIndices", Value::MakeIntArray(m.joint_indices), "int[]");
      Set(p, "primvars:skel:jointWeights", Value::MakeFloatArray(m.joint_weights), "float[]");
      Interpolation(p, "primvars:skel:jointIndices", "vertex", m.influences);
      Interpolation(p, "primvars:skel:jointWeights", "vertex", m.influences);
    }
  }
  (void)source;
  return layer;
}

std::shared_ptr<Layer> LoadFBXLayerFromFile(const std::string& source,
                                            const tydra::import_asset::Options& options,
                                            std::string* warn, std::string* err) {
  tydra::import_asset::Reader reader(options);
  tydra::import_asset::MappedAsset asset;
  std::string resolved;
  if (!reader.Map(source, "", &asset, &resolved, err)) return nullptr;
  return LoadFBXLayerFromMemory(resolved, asset.data(), asset.size(), options, warn, err);
}

}  // namespace lightusd
