// SPDX-License-Identifier: Apache-2.0
#if defined(__clang__)
// Exact equality is required for authored time keys and discrete opinions.
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#include "udim-layer.hh"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

#include "safe-arithmetic.hh"
#include "udim-layer-geometry.hh"
namespace lightusd::udim {
LayerAccess::~LayerAccess() = default;
bool LayerAccess::decodeDiscrete(const std::string& key, const Numeric& src,
                                 std::vector<std::string>* out) const {
  const auto table = discrete_.find(key);
  if (table == discrete_.end() || src.components != 1 ||
      src.values.size() > memory_budget_ / 64)
    return false;
  size_t bytes = src.values.size() * 32;
  out->reserve(src.values.size());
  for (double code : src.values) {
    if (!std::isfinite(code) || code < 0 || std::floor(code) != code ||
        code >= double(table->second.values.size()))
      return false;
    const auto& text = table->second.values[size_t(code)];
    if (text.size() > memory_budget_ - bytes) return false;
    bytes += text.size();
    out->push_back(text);
  }
  return true;
}
namespace {
bool Fail(std::string* e, const std::string& s) {
  if (e) *e = "UDIM bake: " + s;
  return false;
}
std::string PrimPath(const std::string& p) {
  const auto dot = p.find('.');
  return p.substr(0, dot);
}
std::string Parent(const std::string& p) {
  const auto n = p.rfind('/');
  return n == 0 || n == std::string::npos ? "" : p.substr(0, n);
}
bool TraceUV(const LayerAccess& a, const std::string& texture,
             std::string* name, std::array<double, 6>* affine, std::string* e) {
  *affine = {1, 0, 0, 1, 0, 0};
  std::string current = texture;
  std::string input = "inputs:st";
  std::set<std::string> seen;
  for (size_t depth = 0; depth < 64; ++depth) {
    const auto links = a.targets(current, input);
    if (links.size() != 1)
      return Fail(e, "dense baking requires one UV connection at " + current);
    current = PrimPath(links[0]);
    if (!seen.insert(current).second)
      return Fail(e, "cyclic UV network at " + texture);
    const auto id = a.text(current, "info:id");
    if (id == "UsdPrimvarReader_float2") {
      if (a.dynamic(current, "inputs:varname"))
        return Fail(e, "animated primvar reader at " + current);
      *name = a.text(current, "inputs:varname");
      if (name->empty()) return Fail(e, "missing primvar name at " + current);
      return true;
    }
    if (id != "UsdTransform2d")
      return Fail(e, "unsupported dense UV source at " + current);
    for (const auto& prop :
         {"inputs:scale", "inputs:translation", "inputs:rotation"})
      if (a.dynamic(current, prop) || !a.targets(current, prop).empty())
        return Fail(e, "dynamic UV transform at " + current);
    Numeric scale, trans, rot;
    double sx = 1, sy = 1, tx = 0, ty = 0, r = 0;
    if (a.numeric(current, "inputs:scale", &scale) &&
        scale.values.size() == 2) {
      sx = scale.values[0];
      sy = scale.values[1];
    }
    if (a.numeric(current, "inputs:translation", &trans) &&
        trans.values.size() == 2) {
      tx = trans.values[0];
      ty = trans.values[1];
    }
    if (a.numeric(current, "inputs:rotation", &rot) && rot.values.size() == 1)
      r = rot.values[0] * 3.14159265358979323846 / 180;
    const double c = std::cos(r), s = std::sin(r);
    const auto b = *affine;
    *affine = {b[0] * c * sx + b[1] * s * sx, -b[0] * s * sy + b[1] * c * sy,
               b[2] * c * sx + b[3] * s * sx, -b[2] * s * sy + b[3] * c * sy,
               b[0] * tx + b[1] * ty + b[4],  b[2] * tx + b[3] * ty + b[5]};
    input = "inputs:in";
  }
  return Fail(e, "UV network depth exceeds limit");
}
bool ExpandUV(const LayerAccess& a, const std::string& mesh,
              const std::string& prop, const std::vector<int32_t>& counts,
              const std::vector<int32_t>& indices,
              const std::array<double, 6>& transform, std::vector<double>* out,
              std::string* e,
              double time = std::numeric_limits<double>::infinity()) {
  Numeric n, lookup;
  if (!a.numeric(mesh, prop, &n, time) || n.components != 2)
    return Fail(e, "missing float2 UVs " + mesh + "." + prop);
  const bool indexed = a.numeric(mesh, prop + ":indices", &lookup, time);
  auto interp = a.interpolation(mesh, prop);
  if (interp.empty()) interp = "constant";
  if (interp != "constant" && interp != "uniform" && interp != "vertex" &&
      interp != "varying" && interp != "faceVarying")
    return Fail(e, "invalid UV interpolation");
  if (a.dynamic(mesh, prop + ":indices")) {
    for (double sample_time : a.times(mesh, prop + ":indices")) {
      Numeric sample;
      if (!a.numeric(mesh, prop + ":indices", &sample, sample_time) ||
          sample.values != lookup.values)
        return Fail(e,
                    "animated primvar indices change UV ownership at " + mesh);
    }
  }
  out->resize(indices.size() * 2);
  size_t corner = 0;
  for (size_t f = 0; f < counts.size(); ++f)
    for (int32_t j = 0; j < counts[f]; ++j, ++corner) {
      size_t index = interp == "constant"      ? 0
                     : interp == "uniform"     ? f
                     : interp == "faceVarying" ? corner
                                               : size_t(indices[corner]);
      if (indexed) {
        if (index >= lookup.values.size() ||
            !std::isfinite(lookup.values[index]) || lookup.values[index] < 0 ||
            std::floor(lookup.values[index]) != lookup.values[index] ||
            lookup.values[index] >= double(SIZE_MAX))
          return Fail(e, "UV index out of range");
        index = size_t(lookup.values[index]);
      }
      if (index >= n.values.size() / 2)
        return Fail(e, "UV cardinality mismatch at " + mesh);
      const double u = n.values[index * 2], v = n.values[index * 2 + 1];
      (*out)[corner * 2] = transform[0] * u + transform[1] * v + transform[4];
      (*out)[corner * 2 + 1] =
          transform[2] * u + transform[3] * v + transform[5];
    }
  return true;
}
bool Reachable(const LayerAccess& a, const std::string& material,
               const std::string& shader) {
  std::vector<std::string> todo{material};
  std::set<std::string> visited;
  while (!todo.empty()) {
    if (visited.size() >= 4096 || todo.size() >= 65536) {
      a.accessFail("UDIM bake: material graph exceeds traversal limit");
      return false;
    }
    const auto p = todo.back();
    todo.pop_back();
    if (p == shader) return true;
    if (!visited.insert(p).second) continue;
    for (const auto& prop : a.properties(p))
      for (const auto& target : a.targets(p, prop))
        todo.push_back(PrimPath(target));
  }
  return false;
}
std::string BoundMaterial(const LayerAccess& a, std::string path) {
  std::string material;
  for (size_t i = 0; i < 512 && !path.empty(); ++i, path = Parent(path)) {
    const auto t = a.targets(path, "material:binding");
    if (t.size() == 1 && (material.empty() ||
                          a.bindingStrength(path) == "strongerThanDescendants"))
      material = PrimPath(t[0]);
  }
  if (!path.empty())
    a.accessFail("UDIM bake: material binding depth exceeds limit");
  return material;
}
}  // namespace
void NormalizeNormals(const std::string& prop, Numeric* n) {
  if ((prop != "normals" && prop != "primvars:normals") || n->components != 3)
    return;
  for (size_t i = 0; i + 2 < n->values.size(); i += 3) {
    const double length =
        std::hypot(n->values[i], n->values[i + 1], n->values[i + 2]);
    if (length > 0 && std::isfinite(length))
      for (size_t j = 0; j < 3; ++j) n->values[i + j] /= length;
  }
}
bool DescribeLayer(const LayerAccess& a, std::vector<Site>* sites,
                   std::string* e) {
  sites->clear();
  const auto paths = a.paths();
  if (!a.accessError().empty()) return Fail(e, a.accessError());
  for (const auto& p : paths) {
    std::vector<double> times = a.times(p, "inputs:file");
    times.insert(times.begin(), std::numeric_limits<double>::infinity());
    bool has_udim = false;
    for (double time : times) {
      std::string pre, post;
      if (SplitPattern(a.text(p, "inputs:file", time), &pre, &post))
        has_udim = true;
    }
    if (!has_udim) continue;
    if (a.text(p, "info:id") != "UsdUVTexture")
      return Fail(e, "unsupported texture shader at " + p);
    if (!a.targets(p, "inputs:file").empty() ||
        a.dynamic(p, "inputs:sourceColorSpace"))
      return Fail(e, "connected file or animated color space at " + p);
    const auto cs = a.text(p, "inputs:sourceColorSpace");
    for (double time : times) {
      const auto pattern = a.text(p, "inputs:file", time);
      if (pattern.empty()) continue;
      std::string pre, post;
      if (!SplitPattern(pattern, &pre, &post))
        return Fail(e,
                    "animated texture mixes tiled and untiled files at " + p);
      const bool hdr =
          post.size() >= 4 && (post.substr(post.size() - 4) == ".exr" ||
                               post.substr(post.size() - 4) == ".hdr");
      sites->push_back({p, pattern,
                        cs == "sRGB" || ((cs.empty() || cs == "auto") && !hdr),
                        time});
    }
  }
  return a.accessError().empty() || Fail(e, a.accessError());
}
static bool ApplyStaticPlans(LayerAccess& a, const std::vector<Plan>& plans,
                             const Options& o, std::string* e) {
  if (!ValidateOptions(o, e)) return false;
  if (o.mode == BakeMode::Grid) {
    for (const auto& p : plans)
      if (!a.grid(p, e)) return false;
    return true;
  }
  if (o.mode != BakeMode::Dense) return Fail(e, "baking disabled");
  const auto paths = a.paths();
  std::map<std::string, std::vector<std::string>> subsets;
  std::set<std::string> authored_uv_names;
  for (const auto& path : paths) {
    if (a.type(path) == "GeomSubset") subsets[Parent(path)].push_back(path);
    if (a.type(path) == "Mesh")
      for (const auto& prop : a.properties(path))
        authored_uv_names.insert(prop);
  }
  std::vector<std::pair<std::string, std::string>> reference_bindings;
  for (const auto& path : paths)
    for (const auto& root : a.referenceRoots(path)) {
      const auto material = BoundMaterial(a, path);
      if (!material.empty()) reference_bindings.emplace_back(root, material);
    }
  if (!a.accessError().empty()) return Fail(e, a.accessError());
  std::vector<std::string> generated_names;
  for (size_t i = 0; i < plans.size(); ++i) {
    const std::string base = "_udimAtlas" + std::to_string(i);
    std::string name = base;
    size_t suffix = 0;
    while (authored_uv_names.count("primvars:" + name))
      name = base + "_" + std::to_string(++suffix);
    authored_uv_names.insert("primvars:" + name);
    generated_names.push_back(name);
  }
  std::map<std::pair<std::string, std::string>, bool> reach_cache;
  auto reachable = [&](const std::string& material, const std::string& shader) {
    const auto key = std::make_pair(material, shader);
    const auto found = reach_cache.find(key);
    if (found != reach_cache.end()) return found->second;
    const bool result = Reachable(a, material, shader);
    reach_cache.emplace(key, result);
    return result;
  };
  auto mesh_reachable = [&](const std::string& mesh,
                            const std::string& shader) {
    if (reachable(BoundMaterial(a, mesh), shader)) return true;
    for (const auto& binding : reference_bindings)
      if ((mesh == binding.first || mesh.find(binding.first + "/") == 0) &&
          reachable(binding.second, shader))
        return true;
    return false;
  };
  for (const auto& mesh : paths) {
    if (a.type(mesh) != "Mesh") continue;
    bool affected = false;
    for (const auto& plan : plans) {
      if (mesh_reachable(mesh, plan.site.path)) affected = true;
      for (const auto& subset : subsets[mesh])
        if (reachable(BoundMaterial(a, subset), plan.site.path))
          affected = true;
    }
    if (!a.accessError().empty()) return Fail(e, a.accessError());
    if (!affected) continue;
    Numeric point, count, index;
    if (!a.numeric(mesh, "points", &point) || point.components != 3 ||
        !a.numeric(mesh, "faceVertexCounts", &count) ||
        !a.numeric(mesh, "faceVertexIndices", &index))
      return Fail(e, "incomplete mesh " + mesh);
    if (count.values.size() > o.memory_budget_bytes / 64 ||
        index.values.size() > o.memory_budget_bytes / 128 ||
        point.values.size() > o.memory_budget_bytes / 32)
      return Fail(e, "geometry exceeds working-memory limit");
    std::vector<int32_t> counts, indices;
    for (double v : count.values) {
      if (v < 0 || v > INT32_MAX || std::floor(v) != v)
        return Fail(e, "invalid face count");
      counts.push_back(int32_t(v));
    }
    for (double v : index.values) {
      if (v < 0 || v > INT32_MAX || std::floor(v) != v)
        return Fail(e, "invalid vertex index");
      indices.push_back(int32_t(v));
    }
    size_t total = 0;
    for (int32_t v : counts) {
      if (v < 3 || size_t(v) > indices.size() - std::min(total, indices.size()))
        return Fail(e, "invalid face topology");
      total += size_t(v);
    }
    if (total != indices.size())
      return Fail(e, "topology cardinality mismatch");
    for (int32_t v : indices)
      if (size_t(v) >= point.values.size() / 3)
        return Fail(e, "point index out of range");
    std::vector<UVSet> sets;
    std::vector<const Plan*> used;
    std::vector<std::string> source_props;
    std::vector<std::array<double, 6>> transforms;
    for (size_t pi = 0; pi < plans.size(); ++pi) {
      const auto& plan = plans[pi];
      std::vector<uint8_t> active(counts.size(), 0);
      if (mesh_reachable(mesh, plan.site.path))
        std::fill(active.begin(), active.end(), 1);
      for (const auto& subset : subsets[mesh]) {
        // A directly bound face subset overrides the mesh material on its
        // faces. Unbound subsets inherit the mesh binding and do not mask it
        // out.
        if (a.targets(subset, "material:binding").empty()) continue;
        if (a.text(subset, "elementType") != "face")
          return Fail(e, "material subset must have face elementType");
        const bool uses = reachable(BoundMaterial(a, subset), plan.site.path);
        Numeric faces;
        if (!a.numeric(subset, "indices", &faces))
          return Fail(e, "missing subset indices");
        for (double v : faces.values) {
          if (v < 0 || v >= double(active.size()) || std::floor(v) != v)
            return Fail(e, "invalid subset index");
          active[size_t(v)] = uint8_t(uses);
        }
      }
      if (!a.accessError().empty()) return Fail(e, a.accessError());
      if (std::find(active.begin(), active.end(), 1) == active.end()) continue;
      std::string uv;
      std::array<double, 6> matrix;
      if (!TraceUV(a, plan.site.path, &uv, &matrix, e)) return false;
      UVSet set;
      set.name = "primvars:" + generated_names[pi];
      set.layout = plan.layout;
      set.active_faces = std::move(active);
      const auto prop = "primvars:" + uv;
      if (!ExpandUV(a, mesh, prop, counts, indices, matrix, &set.values, e))
        return false;
      sets.push_back(std::move(set));
      used.push_back(&plan);
      source_props.push_back(prop);
      transforms.push_back(matrix);
    }
    if (sets.empty()) continue;
    if (a.dynamic(mesh, "faceVertexCounts") ||
        a.dynamic(mesh, "faceVertexIndices"))
      return Fail(
          e, "animated topology is incompatible with dense baking at " + mesh);
    GeometryEdits geometry;
    const bool subdivide = a.text(mesh, "subdivisionScheme") != "none";
    bool prepared = false;
    if (subdivide) {
      if (!PrepareGeometry(a, mesh, o, &geometry, e)) return false;
      prepared = true;
      std::vector<uint32_t> provenance;
      if (!RefineGeometry(a, mesh, o, &provenance, e)) return false;
      if (!a.numeric(mesh, "points", &point) ||
          !a.numeric(mesh, "faceVertexCounts", &count) ||
          !a.numeric(mesh, "faceVertexIndices", &index))
        return Fail(e, "refined mesh is incomplete");
      counts.clear();
      indices.clear();
      for (double v : count.values) counts.push_back(int32_t(v));
      for (double v : index.values) indices.push_back(int32_t(v));
      if (!geometry.skin_joints.empty()) {
        Numeric joint;
        joint.integral = true;
        for (size_t v = 0; v < point.values.size() / 3; ++v)
          for (int32_t id : geometry.skin_joints) joint.values.push_back(id);
        if (!a.writeNumeric(mesh, "primvars:skel:jointIndices", joint))
          return Fail(e, "refined skin index authoring failed");
        for (double time : a.times(mesh, "primvars:skel:jointIndices"))
          if (!a.writeNumeric(mesh, "primvars:skel:jointIndices", joint, time))
            return Fail(e, "refined skin index sample authoring failed");
      }
      for (size_t si = 0; si < sets.size(); ++si) {
        auto active = sets[si].active_faces;
        sets[si].active_faces.clear();
        for (uint32_t f : provenance)
          sets[si].active_faces.push_back(active[f]);
        if (!ExpandUV(a, mesh, source_props[si], counts, indices,
                      transforms[si], &sets[si].values, e))
          return false;
      }
      for (const auto& subset : paths)
        if (a.type(subset) == "GeomSubset" && Parent(subset) == mesh) {
          Numeric old;
          if (!a.numeric(subset, "indices", &old)) continue;
          std::set<uint32_t> ids;
          for (double v : old.values) ids.insert(uint32_t(v));
          Numeric now;
          now.integral = true;
          for (size_t f = 0; f < provenance.size(); ++f)
            if (ids.count(provenance[f])) now.values.push_back(double(f));
          if (!a.writeNumeric(subset, "indices", now))
            return Fail(e, "refined subset authoring failed");
        }
    }
    MeshRemap result;
    if (!RemapDenseMesh(point.values, counts, indices, sets, o, &result, e))
      return false;
    if (result.topology_changed && !prepared) {
      if (!PrepareGeometry(a, mesh, o, &geometry, e)) return false;
      prepared = true;
    }

    // Animated UVs are accepted only if the same face-to-tile ownership remains
    // valid throughout linear interpolation; topology-changing UV motion fails.
    std::map<std::string, std::vector<std::pair<double, Numeric>>> uv_samples;
    for (size_t si = 0; si < sets.size(); ++si)
      for (double time : a.times(mesh, source_props[si])) {
        auto animated = sets;
        if (!ExpandUV(a, mesh, source_props[si], counts, indices,
                      transforms[si], &animated[si].values, e, time))
          return false;
        for (size_t k = 0; k < sets[si].values.size(); k += 2)
          if (TileAt(float(sets[si].values[k]),
                     float(sets[si].values[k + 1])) !=
              TileAt(float(animated[si].values[k]),
                     float(animated[si].values[k + 1])))
            return Fail(e, "UV motion changes UDIM ownership at " + mesh);
        MeshRemap r;
        if (result.topology_changed ||
            !RemapDenseMesh(point.values, counts, indices, animated, o, &r,
                            e) ||
            r.topology_changed)
          return Fail(e,
                      "UV motion requires changing dense topology at " + mesh);
        uv_samples[sets[si].name].push_back(
            {time, Numeric{r.uv_values[sets[si].name], 2, false}});
      }
    if (result.topology_changed) {
      // All authored numeric samples follow the same static clipping stencils.
      // Unsupported data fails before output rather than losing an opinion.
      for (const auto& prop : a.properties(mesh)) {
        std::string interp = a.interpolation(mesh, prop);
        if (prop == "points" || prop == "velocities" || prop == "accelerations")
          interp = "vertex";
        if (prop == "normals" && interp.empty()) interp = "varying";
        if (interp.empty() || interp == "constant" ||
            prop == "faceVertexCounts" || prop == "faceVertexIndices" ||
            prop == "extent")
          continue;
        if (prop.size() > 8 && prop.substr(prop.size() - 8) == ":indices")
          continue;
        Numeric n;
        if (!a.numeric(mesh, prop, &n))
          return Fail(
              e, "unsupported interpolated attribute " + mesh + "." + prop);
        if (a.dynamic(mesh, prop + ":indices"))
          return Fail(e, "animated primvar indices at " + mesh);
        Numeric lookup;
        if (a.numeric(mesh, prop + ":indices", &lookup)) {
          std::vector<double> expanded;
          if (lookup.values.size() >
              o.memory_budget_bytes / (n.components * sizeof(double)))
            return Fail(e, "indexed primvar exceeds memory limit");
          for (double v : lookup.values) {
            if (v < 0 || v >= double(n.values.size() / n.components) ||
                std::floor(v) != v)
              return Fail(e, "invalid primvar index");
            for (size_t c = 0; c < n.components; ++c)
              expanded.push_back(n.values[size_t(v) * n.components + c]);
          }
          n.values = std::move(expanded);
        }
        auto remap = [&](Numeric& src, double time) {
          Numeric dest;
          dest.components = src.components;
          dest.integral = src.integral;
          size_t lanes = 0;
          if (!safe::mul(src.components, a.elementSize(mesh, prop), &lanes) ||
              !lanes || lanes > 4096)
            return Fail(e, "invalid primvar elementSize");
          if (!RemapNumeric(src.values, lanes, interp, src.integral, result,
                            o.memory_budget_bytes, &dest.values, e))
            return false;
          NormalizeNormals(prop, &dest);
          return a.writeNumeric(mesh, prop, dest, time);
        };
        if (!remap(n, std::numeric_limits<double>::infinity()))
          return Fail(e, e && !e->empty() ? *e : "attribute remap failed");
        for (double time : a.times(mesh, prop)) {
          Numeric sample;
          if (!a.numeric(mesh, prop, &sample, time))
            return Fail(e, "time sample read failed");
          if (!lookup.values.empty()) {
            std::vector<double> expanded;
            for (double v : lookup.values) {
              if (v < 0 ||
                  v >= double(sample.values.size() / sample.components))
                return Fail(e, "animated primvar index out of range");
              for (size_t c = 0; c < sample.components; ++c)
                expanded.push_back(
                    sample.values[size_t(v) * sample.components + c]);
            }
            sample.values = std::move(expanded);
          }
          if (!remap(sample, time))
            return Fail(e, "time sample remap failed at " + mesh + "." + prop);
        }
      }
      for (const auto& prop : a.properties(mesh))
        if (prop.size() > 8 && prop.substr(prop.size() - 8) == ":indices")
          a.erase(mesh, prop);
      Numeric c, i;
      c.integral = i.integral = true;
      for (int32_t v : result.counts) c.values.push_back(v);
      for (int32_t v : result.indices) i.values.push_back(v);
      if (!a.writeNumeric(mesh, "faceVertexCounts", c) ||
          !a.writeNumeric(mesh, "faceVertexIndices", i))
        return Fail(e, "topology edit failed");
      for (const auto& subset : paths)
        if (a.type(subset) == "GeomSubset" && Parent(subset) == mesh) {
          Numeric old;
          if (!a.numeric(subset, "indices", &old)) continue;
          std::set<uint32_t> ids;
          for (double v : old.values) ids.insert(uint32_t(v));
          Numeric now;
          now.integral = true;
          for (size_t f = 0; f < result.face_sources.size(); ++f)
            if (ids.count(result.face_sources[f]))
              now.values.push_back(double(f));
          if (!a.writeNumeric(subset, "indices", now))
            return Fail(e, "subset remap failed");
        }
    }
    if (prepared && !FinishGeometry(a, mesh, geometry, o, e)) return false;
    for (size_t si = 0; si < sets.size(); ++si) {
      Numeric n{result.uv_values[sets[si].name], 2, false};
      if (!a.writeNumeric(mesh, sets[si].name, n))
        return Fail(e, "generated UV edit failed");
      a.setInterpolation(mesh, sets[si].name, "faceVarying");
      for (const auto& sample : uv_samples[sets[si].name])
        if (!a.writeNumeric(mesh, sets[si].name, sample.second, sample.first))
          return Fail(e, "generated UV animation edit failed");
    }
  }
  for (size_t pi = 0; pi < plans.size(); ++pi)
    if (!a.reader(plans[pi], generated_names[pi], e)) return false;
  return true;
}
bool ApplyPlans(LayerAccess& a, const std::vector<Plan>& plans,
                const Options& o, std::string* e) {
  a.setMemoryBudget(o.memory_budget_bytes);
  std::map<std::string, size_t> groups;
  std::vector<Plan> unique;
  for (const auto& p : plans) {
    const auto found = groups.find(p.site.path);
    if (found == groups.end()) {
      groups[p.site.path] = unique.size();
      unique.push_back(p);
    } else {
      const auto& l = unique[found->second].layout;
      if (l.width != p.layout.width || l.height != p.layout.height ||
          l.cells.size() != p.layout.cells.size())
        return Fail(e, "animated texture requires a common atlas layout");
      for (size_t i = 0; i < l.cells.size(); ++i)
        if (l.cells[i].id != p.layout.cells[i].id)
          return Fail(e, "animated texture requires common tile IDs");
    }
  }
  if (!ApplyStaticPlans(a, unique, o, e)) return false;
  for (const auto& p : plans)
    if (!a.writeAsset(p.site.path, p.asset, p.site.time))
      return Fail(e, "animated texture path edit failed");
  return true;
}
std::string DescribeJSON(const LayerAccess& a) {
  std::vector<Site> sites;
  std::string error;
  minijson::Value out = minijson::Value::object();
  out["success"] = DescribeLayer(a, &sites, &error);
  out["error"] = error;
  out["sites"] = minijson::Value::array();
  const auto paths = a.paths();
  std::map<std::pair<std::string, std::string>, bool> reachable;
  for (const auto& s : sites) {
    minijson::Value consumers = minijson::Value::array();
    for (const auto& path : paths) {
      const auto type = a.type(path);
      if (type != "Mesh" && type != "GeomSubset" &&
          a.referenceRoots(path).empty()) continue;
      const auto material = BoundMaterial(a, path);
      const auto key = std::make_pair(material, s.path);
      auto found = reachable.find(key);
      if (found == reachable.end())
        found = reachable.emplace(key, Reachable(a, material, s.path)).first;
      if (found->second) consumers.push_back(path);
    }
    out["sites"].push_back(minijson::Value{
        {"path", s.path},
        {"pattern", s.pattern},
        {"srgb", s.srgb},
        {"consumers", std::move(consumers)},
        {"time", std::isfinite(s.time) ? minijson::Value(s.time)
                                       : minijson::Value(nullptr)}});
  }
  if (!a.accessError().empty()) {
    out["success"] = false;
    out["error"] = a.accessError();
  }
  return out.dump();
}
std::string ApplyJSON(LayerAccess& a, const std::string& input) {
  std::string error;
  Options o;
  minijson::Value args;
  std::vector<Plan> plans;
  bool ok = input.size() < 4 * 1024 * 1024 &&
            minijson::Parse(input, &args, nullptr, minijson::ParseOptions{}) &&
            args.is_object();
  if (ok) {
    const auto mode = args["options"]["mode"].get_string();
    o.mode = mode == "grid"    ? BakeMode::Grid
             : mode == "dense" ? BakeMode::Dense
                               : BakeMode::Off;
    const auto& opts = args["options"];
    auto number = [&](const char* key, size_t fallback) {
      const auto* v = opts.find(key);
      return v && v->is_number_integer() ? size_t(v->get_uint64()) : fallback;
    };
    for (const auto& bound : std::vector<std::pair<std::string, uint64_t>>{
             {"maxTiles", 8999},
             {"maxAtlasSize", 32768},
             {"memoryBudgetBytes", SIZE_MAX},
             {"densePadding", 1024},
             {"subdivisionLevel", 8}}) {
      if (const auto* value = opts.find(bound.first)) {
        uint64_t n = 0;
        if (!value->is_number_integer() || !value->as_uint64(&n) ||
            n > bound.second)
          ok = false;
      }
    }
    if (!ok)
      return minijson::Value{{"success", false},
                             {"error", "UDIM bake: invalid numeric option"}}
          .dump();
    const auto policy = opts["crossTile"].get_string();
    if (!policy.empty() && policy != "split" && policy != "reject")
      return minijson::Value{{"success", false},
                             {"error", "UDIM bake: invalid crossing policy"}}
          .dump();
    o.max_tiles = number("maxTiles", 100);
    o.max_atlas_size = int(number("maxAtlasSize", 8192));
    o.memory_budget_bytes = number("memoryBudgetBytes", size_t(512) << 20);
    o.dense_padding = int(number("densePadding", 2));
    o.subdivision_level = int(number("subdivisionLevel", 2));
    o.cross_tile = opts["crossTile"].get_string() == "split"
                       ? CrossTilePolicy::Split
                       : CrossTilePolicy::Reject;
    const auto* list = args["plans"].array_items();
    ok = list && list->size() <= 4096 && ValidateOptions(o, &error);
    a.setMemoryBudget(o.memory_budget_bytes);
    std::vector<Site> sites;
    if (ok) ok = DescribeLayer(a, &sites, &error);
    if (ok && args.find("shaderPaths")) {
      const auto* selected = args["shaderPaths"].array_items();
      std::set<std::string> paths;
      if (!selected || selected->empty() || selected->size() > 4096) {
        ok = Fail(&error, "invalid shader selection");
      } else {
        for (const auto& value : *selected) {
          const auto path = value.get_string();
          if (!value.is_string() || path.empty() || !paths.insert(path).second ||
              std::none_of(sites.begin(), sites.end(), [&](const Site& site) {
                return site.path == path;
              })) {
            ok = Fail(&error, "unknown or duplicate selected shader");
            break;
          }
        }
        sites.erase(std::remove_if(sites.begin(), sites.end(),
                                  [&](const Site& site) {
                                    return !paths.count(site.path);
                                  }), sites.end());
      }
    }
    std::set<std::string> planned_paths;
    if (ok)
      for (const auto& value : *list) {
        Plan p;
        p.site.path = value["path"].get_string();
        p.site.pattern = value["pattern"].get_string();
        p.site.time = value["time"].is_null()
                          ? std::numeric_limits<double>::infinity()
                          : value["time"].get_double();
        p.asset = value["asset"].get_string();
        auto site =
            std::find_if(sites.begin(), sites.end(), [&](const Site& s) {
              return s.path == p.site.path && s.pattern == p.site.pattern &&
                     s.time == p.site.time;
            });
        if (!planned_paths
                 .insert(p.site.path + "|" + std::to_string(p.site.time))
                 .second ||
            site == sites.end() || p.asset.empty() || p.asset[0] == '/' ||
            p.asset.find("..") != std::string::npos) {
          ok = Fail(&error, "invalid bake site or asset name");
          break;
        }
        p.site = *site;
        std::vector<uint32_t> ids;
        const auto* cells = value["layout"]["cells"].array_items();
        if (!cells || cells->size() > o.max_tiles) {
          ok = Fail(&error, "invalid layout cell list");
          break;
        }
        for (const auto& cell : *cells) {
          uint64_t id = cell["id"].get_uint64();
          if (id < 1001 || id > 9999) {
            ok = false;
            break;
          }
          ids.push_back(uint32_t(id));
        }
        uint64_t width = 0, height = 0;
        if (!value["layout"]["tileWidth"].as_uint64(&width) ||
            !value["layout"]["tileHeight"].as_uint64(&height) ||
            width > uint64_t(o.max_atlas_size) ||
            height > uint64_t(o.max_atlas_size)) {
          ok = Fail(&error, "invalid tile dimensions");
          break;
        }
        if (!ok ||
            !MakeLayout(ids, int(width), int(height), o, &p.layout, &error)) {
          ok = false;
          break;
        }
        plans.push_back(std::move(p));
      }
    if (ok && plans.size() != sites.size())
      ok = Fail(&error, "incomplete bake plans");
    if (ok) ok = ApplyPlans(a, plans, o, &error);
  }
  minijson::Value remaining = minijson::Value::array();
  if (ok) {
    std::set<std::string> references;
    for (const auto& path : a.paths())
      for (const auto& prop : a.properties(path)) {
        auto times = a.times(path, prop);
        times.push_back(std::numeric_limits<double>::infinity());
        for (double time : times) {
          const auto text = a.text(path, prop, time);
          if (!text.empty()) references.insert(text);
        }
      }
    for (const auto& reference : references) remaining.push_back(reference);
  }
  return minijson::Value{
      {"success", ok},
      {"remainingReferences", std::move(remaining)},
      {"error", error.empty() && !ok ? "UDIM bake: invalid edit plan" : error}}
      .dump();
}
}  // namespace lightusd::udim
