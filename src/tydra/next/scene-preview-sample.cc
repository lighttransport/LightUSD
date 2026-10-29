// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include "color-transform.hh"
#include "next/eval/xform-eval.hh"
#include "next/layer/asset-anchor.hh"
#include "next/layer/prim-spec.hh"
#include "next/schema/geom-point-instancer.hh"
#include "scene-preview-internal.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
double Length(const double* a) {
  return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}
void Normalize(float* a) {
  const double n = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] +
                             double(a[2]) * a[2]);
  if (n > 1e-20)
    for (int k = 0; k < 3; ++k) a[k] = static_cast<float>(a[k] / n);
}
bool Prefix(const std::string& path, const std::string& root) {
  return path == root ||
         (path.size() > root.size() &&
          path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}
uint64_t Rank(const Sample& s) {
  uint64_t h = 14695981039346656037ull;
  const auto* p = reinterpret_cast<const unsigned char*>(s.p);
  for (size_t i = 0; i < sizeof(s.p); ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
Key Cell(const Sample& s, double size) {
  Key k;
  const double cap = double(INT64_MAX / 4);
  k.x = static_cast<int64_t>(
      std::max(-cap, std::min(cap, std::floor(s.p[0] / size))));
  k.y = static_cast<int64_t>(
      std::max(-cap, std::min(cap, std::floor(s.p[1] / size))));
  k.z = static_cast<int64_t>(
      std::max(-cap, std::min(cap, std::floor(s.p[2] / size))));
  int a = 0;
  if (std::abs(s.n[1]) > std::abs(s.n[a])) a = 1;
  if (std::abs(s.n[2]) > std::abs(s.n[a])) a = 2;
  k.direction = a * 2 + (s.n[a] < 0 ? 1 : 0);
  return k;
}
float Linear(float x) {
  return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}
float Attribute(const FloatChunked& data, Interpolation mode, uint32_t vertex,
                uint32_t corner, uint32_t face, size_t components,
                size_t channel, float fallback) {
  size_t i = 0;
  if (mode == Interpolation::Vertex || mode == Interpolation::Varying)
    i = vertex;
  else if (mode == Interpolation::FaceVarying)
    i = corner;
  else if (mode == Interpolation::Uniform)
    i = face;
  return i < data.size() / components ? data[i * components + channel]
                                      : fallback;
}
bool Visible(usd::UsdPrim prim, double time) {
  for (size_t depth = 0; prim && depth < 1024;
       ++depth, prim = prim.GetParent()) {
    if (!prim.GetMeta().active ||
        prim.GetSpecifier() == usd::PrimSpecifier::Class)
      return false;
    const auto visibility = prim.GetValueAtTime("visibility", time);
    const auto purpose = prim.GetPropertyValue("purpose");
    if (visibility && visibility->as_token() == "invisible") return false;
    if (purpose && purpose->as_token() == "guide") return false;
  }
  return true;
}
}  // namespace
std::array<double, 16> Identity() {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
void Multiply(const double* a, const double* b, double* result) {
  double t[16] = {};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      for (int k = 0; k < 4; ++k) t[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
  std::copy(t, t + 16, result);
}
bool Inverse(const double* m, double* out) {
  double a[4][8] = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = m[c * 4 + r];
    a[r][4 + r] = 1;
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (std::abs(a[pivot][c]) < 1e-20) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double d = a[c][c];
    for (double& v : a[c]) v /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double v = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= v * a[c][k];
      }
  }
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) out[c * 4 + r] = a[r][4 + c];
  return true;
}
namespace {
struct SampleTransform {
  const double* matrix;
  double inverse[16] = {};
  double scale = 0;
  bool invertible;
  explicit SampleTransform(const double* m)
      : matrix(m), invertible(Inverse(m, inverse)) {
    for (int c = 0; c < 3; ++c) scale = std::max(scale, Length(m + c * 4));
  }
  Sample operator()(const Sample& s) const {
    Sample t = s;
    for (int r = 0; r < 3; ++r)
      t.p[r] = matrix[r] * s.p[0] + matrix[4 + r] * s.p[1] +
               matrix[8 + r] * s.p[2] + matrix[12 + r];
    if (invertible)
      for (int r = 0; r < 3; ++r)
        t.n[r] = static_cast<float>(inverse[r * 4] * s.n[0] +
                                    inverse[r * 4 + 1] * s.n[1] +
                                    inverse[r * 4 + 2] * s.n[2]);
    Normalize(t.n);
    t.radius = static_cast<float>(s.radius * scale);
    return t;
  }
};
}  // namespace
Sample Transform(const Sample& s, const double* m) {
  return SampleTransform(m)(s);
}
std::string Build::prototype_file(const std::string& key) const {
  std::ostringstream s;
  s << std::hex << Hash(key);
  return (directory / "prototypes" / (s.str() + ".samples")).string();
}
bool Build::flush_prototype(const std::string& key) {
  auto it = prototype_buffers.find(key);
  if (it == prototype_buffers.end() || it->second.empty()) return true;
  std::ofstream f(prototype_file(key), std::ios::binary | std::ios::app);
  f.write(reinterpret_cast<const char*>(it->second.data()),
          static_cast<std::streamsize>(it->second.size() * sizeof(Sample)));
  if (!f) {
    result.error = "preview prototype spill write failed";
    return false;
  }
  it->second.clear();
  return true;
}
bool Build::insert(const Sample& s) {
  if (!std::isfinite(s.p[0]) || !std::isfinite(s.p[1]) ||
      !std::isfinite(s.p[2]) || !std::isfinite(s.radius) || s.radius <= 0 ||
      s.opacity <= 0)
    return true;
  for (int k = 0; k < 3; ++k)
    if (!std::isfinite(s.n[k]) || !std::isfinite(s.color[k])) return true;
  if (!std::isfinite(s.opacity)) return true;
  if (cell_size <= 0) cell_size = std::max(1e-6, double(s.radius));
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(s.p[axis] / cell_size) ||
        std::abs(s.p[axis] / cell_size) > 9e18) {
      result.error = "preview coordinates exceed the spatial grid range";
      return false;
    }
  }
  const Key k = Cell(s, cell_size);
  const uint64_t rank = Rank(s);
  auto it = cells.find(k);
  if (it == cells.end())
    cells.emplace(k, Accum{s, rank});
  else if (rank < it->second.rank)
    it->second = {s, rank};
  while (cells.size() > max_cells) {
    if (opts.resolution) {
      result.error =
          "explicit preview resolution exceeds the renderer budget; reduce "
          "resolution or raise --preview-budget-mib";
      return false;
    }
    cell_size *= 2;
    std::unordered_map<Key, Accum, KeyHash> coarse;
    coarse.reserve(cells.size() / 2);
    for (const auto& item : cells) {
      const Key key = Cell(item.second.representative, cell_size);
      auto found = coarse.find(key);
      if (found == coarse.end())
        coarse.emplace(key, item.second);
      else if (item.second.rank < found->second.rank)
        found->second = item.second;
    }
    cells.swap(coarse);
  }
  return true;
}
bool Build::emit(
    const Sample& s,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors) {
  bool prototype_only = false;
  for (const auto& a : ancestors)
    prototype_only = prototype_only || point_prototypes.count(a.first) != 0;
  if (!prototype_only && !insert(s)) return false;
  for (const auto& a : ancestors) {
    auto& buffer = prototype_buffers[a.first];
    buffer.push_back(Transform(s, a.second.data()));
    if (buffer.size() >= 256 && !flush_prototype(a.first)) return false;
  }
  return true;
}
bool Build::replay(
    const std::string& key, const double* world,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors) {
  if (!flush_prototype(key)) return false;
  std::ifstream f(prototype_file(key), std::ios::binary);
  if (!f) {
    result.error = "missing cross-batch preview prototype: " + key;
    return false;
  }
  const SampleTransform transform(world);
  Sample buffer[4096];
  size_t count = 0;
  while (f.read(reinterpret_cast<char*>(buffer), sizeof(buffer)) ||
         f.gcount()) {
    const auto bytes = f.gcount();
    if (bytes % sizeof(Sample)) {
      result.error = "truncated preview prototype spill";
      return false;
    }
    for (size_t i = 0; i < static_cast<size_t>(bytes) / sizeof(Sample); ++i)
      if (!emit(transform(buffer[i]), ancestors)) return false;
    count += static_cast<size_t>(bytes) / sizeof(Sample);
    if (!check("prototype-samples", count)) return false;
  }
  if (!f.eof()) {
    result.error = "preview prototype spill read failed";
    return false;
  }
  return true;
}
void Build::prepare_material(const usd::Stage& stage,
                             const usd::UsdPrim& prim) {
  const std::string path = prim.GetPath().str();
  if (materials.count(path)) return;
  auto& m = materials[path];
  RenderSceneConverter convert;
  convert.SetTimeCode(opts.time_code);
  if (!convert.ConvertMaterial(stage, prim, &m.value, &m.scene)) {
    if (result.warning.size() < 16384)
      result.warning += "Preview material fallback: " + path + "\n";
  }
}
std::string Build::binding(usd::UsdPrim prim) const {
  std::string selected;
  for (size_t depth = 0; prim && depth < 1024;
       ++depth, prim = prim.GetParent()) {
    for (const char* name : {"material:binding:preview", "material:binding"}) {
      const auto* target = prim.GetRelationship(name);
      if (!target || target->empty()) continue;
      const std::string path = target->front().str();
      if (materials.count(path)) {
        if (selected.empty()) selected = path;
        const auto* meta = prim.GetPropertyMeta(name);
        if (meta && meta->bindMaterialAs == "strongerThanDescendants")
          selected = path;
        break;
      }
    }
  }
  return selected;
}
bool Build::plan(usd::pcp::ComposedBatch& batch) {
  if (!batch.layer) return true;
  usd::Stage stage;
  stage.SetRootLayer(std::move(*batch.layer));
  meta = stage.GetRootLayer()->meta();
  const auto& prims = stage.GetRootLayer()->prims();
  for (size_t i = 0; i < prims.size(); ++i) {
    usd::UsdPrim prim(&prims[i], stage.GetRootLayer(),
                      static_cast<uint32_t>(i));
    if (!prim.GetMeta().instance_prototype().empty()) {
      prototypes.insert(prim.GetMeta().instance_prototype());
      if (prim.GetMeta().instance_prototype() != prim.GetPath().str())
        native_prototypes[prim.GetPath().str()] =
            prim.GetMeta().instance_prototype();
    }
    if (prim.GetTypeName() == "PointInstancer") {
      for (const auto& p : usd::UsdGeomPointInstancer(prim).GetPrototypes()) {
        prototypes.insert(p.str());
        point_prototypes.insert(p.str());
      }
    }
    const std::string& type = prim.GetTypeName();
    if (type == "Material" || type == "Shader" || type == "NodeGraph") {
      // Discovery may split a Material and its Shader children across batches.
      // Resolve complete graphs only after the planning pass has collected all
      // their small, non-geometry records.
      const auto path = prim.GetPath().str();
      if (!material_specs.count(path))
        material_specs.emplace(path, prims[i].Clone());
    }
    if ((type == "Camera" || type.find("Light") != std::string::npos) &&
        (i >= batch.repeated.size() || !batch.repeated[i]) &&
        Visible(prim, opts.time_code)) {
      auto copy = prims[i].Clone();
      for (const auto& name : prim.GetPropertyNames()) {
        if (name.rfind("xformOp:", 0) == 0 || name == "xformOpOrder")
          copy.remove_property(name);
        else if (const auto* v = prim.GetValueAtTime(name, opts.time_code)) {
          if (v->type_id() == usd::TypeId::AssetPath) {
            // Baked previews must not retain paths into the original asset
            // tree. Keep an untextured light when its environment/IES texture
            // cannot be represented by the baked surface colors.
            copy.remove_property(name);
            if (result.warning.size() < 16384)
              result.warning +=
                  "Preview uses untextured light: " + prim.GetPath().str() +
                  "\n";
          } else
            copy.upsert_property(name, *v);
        }
      }
      auto world = Identity();
      usd::ComputeWorldTransform(stage, prim, world.data(), opts.time_code);
      copy.upsert_property("xformOp:transform",
                           usd::Value::MakeMatrix4d(world.data()));
      copy.upsert_property("xformOpOrder",
                           usd::Value::MakeTokenArray(
                               std::vector<std::string>{"xformOp:transform"}));
      copy.clear_child_indices();
      extras.push_back(std::move(copy));
    }
    if (prim.GetTypeName() == "Mesh" && Visible(prim, opts.time_code)) {
      const auto* extent = prim.GetPropertyValue("extent");
      if (extent && extent->array_size() == 2) {
        auto view = extent->view().as_float_array();
        if (view.size() >= 6) {
          auto m = Identity();
          usd::ComputeWorldTransform(stage, prim, m.data(), opts.time_code);
          std::array<double, 6> b = {1e300,  1e300,  1e300,
                                     -1e300, -1e300, -1e300};
          for (int c = 0; c < 8; ++c) {
            Sample p;
            for (int k = 0; k < 3; ++k)
              p.p[k] = view[(c & (1 << k) ? 3 : 0) + k];
            p = Transform(p, m.data());
            for (int k = 0; k < 3; ++k) {
              b[k] = std::min(b[k], p.p[k]);
              b[k + 3] = std::max(b[k + 3], p.p[k]);
            }
          }
          ++seen_extents;
          if (extent_reservoir.size() < 4096)
            extent_reservoir.push_back(b);
          else {
            const uint64_t slot = Hash(prim.GetPath().str()) % seen_extents;
            if (slot < 4096) extent_reservoir[slot] = b;
          }
        }
      }
    }
  }
  batch.layer = stage.ReleaseRootLayer();
  return check("plan", prims.size());
}
std::string Build::resolve_prototype(std::string path) const {
  for (size_t depth = 0; depth < 1024; ++depth) {
    std::string ancestor = path;
    bool changed = false;
    while (!ancestor.empty()) {
      const auto it = native_prototypes.find(ancestor);
      if (it != native_prototypes.end()) {
        path = it->second + path.substr(ancestor.size());
        changed = true;
        break;
      }
      const auto slash = ancestor.rfind('/');
      if (slash == std::string::npos || slash == 0) break;
      ancestor.resize(slash);
    }
    if (!changed) return path;
  }
  return {};
}
bool Build::finish_plan() {
  std::set<std::string> canonical;
  for (const auto& path : point_prototypes) {
    const auto resolved = resolve_prototype(path);
    if (resolved.empty()) {
      result.error = "cyclic preview prototype aliases";
      return false;
    }
    canonical.insert(resolved);
    prototypes.insert(resolved);
  }
  point_prototypes.swap(canonical);
  usd::Layer catalog;
  catalog.meta() = meta;
  usd::LayerBuilder builder(catalog);
  for (const auto& item : material_specs) {
    const std::string& path = item.first;
    size_t first = 1, depth = 0;
    while (first < path.size()) {
      const auto end = path.find('/', first);
      const auto name =
          path.substr(first, end == std::string::npos ? end : end - first);
      builder.begin_prim(
          name, end == std::string::npos ? item.second.type_name() : "");
      ++depth;
      if (end == std::string::npos) {
        auto copy = item.second.Clone();
        copy.clear_child_indices();
        *builder.current() = std::move(copy);
        break;
      }
      first = end + 1;
    }
    while (depth--) builder.end_prim();
  }
  builder.finalize();
  usd::Stage stage;
  stage.SetRootLayer(std::move(catalog));
  for (const auto& item : material_specs)
    if (item.second.type_name() == "Material")
      prepare_material(stage, stage.GetPrimAtPath(usd::Path(item.first)));
  material_specs.clear();
  return check("materials", materials.size());
}

std::array<float, 4> Build::shade(const std::string& path,
                                  const RenderMesh& mesh, size_t face,
                                  const float* uv) {
  std::array<float, 4> value = {.5f, .5f, .5f, 1};
  auto found = materials.find(path);
  if (found == materials.end()) return value;
  Material& m = found->second;
  const ShaderParam* color = nullptr;
  const ShaderParam* opacity = nullptr;
  if (m.value.preview_surface) {
    color = &m.value.preview_surface->diffuse_color;
    opacity = &m.value.preview_surface->opacity;
  } else if (m.value.openpbr) {
    color = &m.value.openpbr->base_color;
    opacity = &m.value.openpbr->opacity;
  }
  auto param = [&](const ShaderParam& p, bool is_color) {
    std::array<float, 4> out = {p.value.x, p.value.y, p.value.z, p.value.w};
    if (p.texture_id < 0 ||
        static_cast<size_t>(p.texture_id) >= m.scene.textures.size())
      return out;
    const auto& t = m.scene.textures[static_cast<size_t>(p.texture_id)];
    std::string file = t.asset_path;
    if (t.image_id >= 0 &&
        static_cast<size_t>(t.image_id) < m.scene.images.size())
      file = m.scene.images[static_cast<size_t>(t.image_id)].resolved_path;
    double u = uv[0] * t.scale.x, v = uv[1] * t.scale.y;
    const double x =
        std::cos(t.rotation) * u - std::sin(t.rotation) * v + t.offset.x;
    v = std::sin(t.rotation) * u + std::cos(t.rotation) * v + t.offset.y;
    u = x;
    if (!std::isfinite(u) || !std::isfinite(v)) return out;
    if (t.is_udim || file.find("<UDIM>") != std::string::npos) {
      if (std::abs(u) > 1000000 || std::abs(v) > 1000000) return out;
      const int tile = 1001 + static_cast<int>(std::floor(u)) +
                       10 * static_cast<int>(std::floor(v));
      if (t.udim_texture_id >= 0 && static_cast<size_t>(t.udim_texture_id) <
                                        m.scene.udim_textures.size()) {
        for (const auto& entry :
             m.scene.udim_textures[static_cast<size_t>(t.udim_texture_id)]
                 .tiles)
          if (entry.udim == static_cast<uint32_t>(tile) &&
              entry.image_id >= 0 &&
              static_cast<size_t>(entry.image_id) < m.scene.images.size())
            file = m.scene.images[static_cast<size_t>(entry.image_id)]
                       .resolved_path;
      }
      const auto pos = file.find("<UDIM>");
      if (pos != std::string::npos) file.replace(pos, 6, std::to_string(tile));
      u -= std::floor(u);
      v -= std::floor(v);
    }
    const std::string key = file + (is_color ? ":color" : ":data");
    if (failed_images.count(key)) return out;
    auto image = images.find(key);
    if (image == images.end()) {
      auto decoded = std::make_shared<DecodedImage>();
      if (images.size() >= 64 ||
          decoder.decoded_bytes() > opts.texture_budget_bytes * 3 / 4)
        images.clear();
      const bool srgb = is_color && t.source_color_space != "raw" &&
                        t.source_color_space != "data";
      dependencies.insert(file);
      bool ok = false;
      if (file.size() > 4 && (file.substr(file.size() - 4) == ".ptx" ||
                              file.substr(file.size() - 5) == ".ptex"))
        ok = decoder.DecodePtexFace(file, static_cast<uint32_t>(face), 0, srgb,
                                    decoded.get());
      else
        ok = decoder.Decode(file, srgb, decoded.get());
      if (!ok) {
        failed_images.insert(key);
        if (result.warning.size() < 16384)
          result.warning += "Preview texture unavailable: " + file + "\n";
        return out;
      }
      dependencies.insert(file);
      image = images.emplace(key, std::move(decoded)).first;
    }
    const auto& img = *image->second;
    if (img.empty()) return out;
    auto wrap = [](double q, WrapMode mode) {
      if (mode == WrapMode::Repeat) return q - std::floor(q);
      if (mode == WrapMode::Mirror) {
        q = std::fmod(std::abs(q), 2.0);
        return q > 1 ? 2 - q : q;
      }
      return std::max(0.0, std::min(1.0, q));
    };
    if ((t.wrap_s == WrapMode::Black && (u < 0 || u > 1)) ||
        (t.wrap_t == WrapMode::Black && (v < 0 || v > 1)))
      return std::array<float, 4>{0, 0, 0, 0};
    const double px = wrap(u, t.wrap_s) * (img.width - 1),
                 py = (1 - wrap(v, t.wrap_t)) * (img.height - 1);
    const uint32_t ix = static_cast<uint32_t>(px),
                   iy = static_cast<uint32_t>(py);
    const float fx = static_cast<float>(px - ix),
                fy = static_cast<float>(py - iy);
    ::lightusd::color::ColorTransform color_transform;
    color_transform.bypass = t.color_transform_bypass;
    color_transform.source.kind =
        t.source_color_is_data ? ::lightusd::color::ColorSpaceKind::Data
                               : ::lightusd::color::ColorSpaceKind::Color;
    color_transform.source.gamma = t.source_gamma;
    color_transform.source.linear_bias = t.source_linear_bias;
    std::copy(t.source_to_display_linear, t.source_to_display_linear + 9,
              color_transform.matrix);
    if (img.hdr &&
        (t.source_color_space.empty() || t.source_color_space == "auto")) {
      color_transform.source.gamma = 1;
      color_transform.source.linear_bias = 0;
    }
    out = {0, 0, 0, 0};
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const uint32_t xx = std::min(img.width - 1,
                                     ix + static_cast<uint32_t>(dx)),
                       yy = std::min(img.height - 1,
                                     iy + static_cast<uint32_t>(dy));
        const size_t off =
            (size_t(yy) * img.width + xx) * (img.hdr ? 3 : img.channels);
        const float weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        float texel[4] = {1, 1, 1, 1};
        for (size_t c = 0; c < 4; ++c) {
          if (img.hdr) {
            if (c < 3) texel[c] = img.float_pixels[off + c];
          } else if (c < img.channels)
            texel[c] = img.pixels[off + c] / 255.f;
        }
        if (is_color && t.color_transform_valid)
          ::lightusd::color::TransformRGB(color_transform, texel);
        else if (is_color && !img.hdr && t.source_color_space != "raw" &&
                 t.source_color_space != "data" &&
                 t.source_color_space != "linear")
          for (int c = 0; c < 3; ++c) texel[c] = Linear(texel[c]);
        for (size_t c = 0; c < 4; ++c) out[c] += weight * texel[c];
      }
    const float scales[] = {t.scale_value.x, t.scale_value.y, t.scale_value.z,
                            t.scale_value.w};
    const float bias[] = {t.bias.x, t.bias.y, t.bias.z, t.bias.w};
    for (int c = 0; c < 4; ++c) out[c] = out[c] * scales[c] + bias[c];
    if (t.output_channel != RenderTexture::Channel::RGB &&
        t.output_channel != RenderTexture::Channel::RGBA) {
      size_t c = static_cast<size_t>(t.output_channel);
      const float f = out[std::min(size_t(3), c)];
      out = {f, f, f, f};
    }
    return out;
  };
  if (color) {
    const auto c = param(*color, true);
    for (int k = 0; k < 3; ++k) value[k] = std::max(0.f, c[k]);
  }
  if (opacity)
    value[3] = std::max(0.f, std::min(1.f, param(*opacity, false)[0]));
  (void)mesh;
  return value;
}

bool Build::mesh(
    const usd::Stage& stage, const usd::UsdPrim& prim, const double* world,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors) {
  RenderMesh geometry;
  converter.SetTimeCode(opts.time_code);
  if (!converter.ConvertRenderableMesh(stage, prim, &geometry)) return true;
  if (geometry.points.empty() || geometry.triangulated_indices.empty())
    return true;
  if (cell_size <= 0) {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t v = 0; v < geometry.point_count(); ++v)
      for (int k = 0; k < 3; ++k) {
        const double p = geometry.points[v * 3 + static_cast<size_t>(k)];
        lo[k] = std::min(lo[k], p);
        hi[k] = std::max(hi[k], p);
      }
    double extent = 0;
    for (int k = 0; k < 3; ++k) extent = std::max(extent, hi[k] - lo[k]);
    double scale = 0;
    for (int k = 0; k < 3; ++k) scale = std::max(scale, Length(world + k * 4));
    cell_size = std::max(
        1e-9, extent * scale / (opts.resolution ? opts.resolution : 256));
  }
  ++result.source_meshes;
  const std::string material = binding(prim);
  const size_t triangles = geometry.triangulated_indices.size() / 3;
  // Each prototype mesh gets a bounded reusable sample set. The global grid
  // adapts independently, so repeated placements never duplicate triangles.
  const size_t per_mesh =
      std::min(size_t(65536), std::max(size_t(64), max_cells / 64));
  const size_t stride =
      std::max(size_t(1), (triangles + per_mesh - 1) / per_mesh);
  std::vector<std::pair<std::string, std::set<int32_t>>> subsets;
  usd::AttributeEval eval(&stage);
  eval.SetTime(opts.time_code);
  for (const auto& child : prim.GetChildren())
    if (child.GetTypeName() == "GeomSubset") {
      auto values = eval.Eval(child, "indices");
      if (!values.success) continue;
      const auto indices = values.value.view().as_int_array();
      std::set<int32_t> faces;
      for (size_t k = 0; k < indices.size(); ++k) faces.insert(indices[k]);
      subsets.emplace_back(binding(child), std::move(faces));
    }
  const SampleTransform transform(world);
  size_t face = 0;
  for (size_t ti = 0; ti < triangles; ti += stride) {
    while (face + 1 < geometry.face_triangle_offsets.size() &&
           geometry.face_triangle_offsets[face + 1] <= ti)
      ++face;
    uint32_t vi[3], ci[3];
    double p[3][3];
    for (int c = 0; c < 3; ++c) {
      vi[c] = geometry.triangulated_indices[ti * 3 + static_cast<size_t>(c)];
      ci[c] =
          geometry.triangulated_face_vertex_indices.empty()
              ? vi[c]
              : geometry
                    .triangulated_face_vertex_indices[ti * 3 +
                                                      static_cast<size_t>(c)];
      if (vi[c] >= geometry.point_count()) {
        result.error = "preview triangulation index out of bounds";
        return false;
      }
      for (int k = 0; k < 3; ++k)
        p[c][k] = geometry.points[vi[c] * 3 + static_cast<size_t>(k)];
    }
    double a[3], b[3], normal[3];
    for (int k = 0; k < 3; ++k) {
      a[k] = p[1][k] - p[0][k];
      b[k] = p[2][k] - p[0][k];
    }
    normal[0] = a[1] * b[2] - a[2] * b[1];
    normal[1] = a[2] * b[0] - a[0] * b[2];
    normal[2] = a[0] * b[1] - a[1] * b[0];
    const double twice_area = Length(normal);
    if (twice_area < 1e-20) continue;
    const double area = twice_area * .5;
    double spacing = cell_size > 0 ? cell_size : std::sqrt(area);
    // Explicit refinement increases per-triangle texture/surface detail while
    // preserving a bounded prototype working set. Convert world spacing back
    // into the mesh's local units when evaluating the requested resolution.
    const double sample_limit = opts.resolution ? 4096.0 : 64.0;
    if (opts.resolution && transform.scale > 0) spacing /= transform.scale;
    const size_t count = std::max(
        size_t(1), static_cast<size_t>(std::min(
                       sample_limit, std::ceil(area / (spacing * spacing)))));
    for (size_t j = 0; j < count; ++j) {
      const double u = std::sqrt((j + .5) / count),
                   v = std::fmod((j + .5) * .6180339887498949, 1.0);
      const float w[3] = {static_cast<float>(1 - u),
                          static_cast<float>(u * (1 - v)),
                          static_cast<float>(u * v)};
      Sample s;
      float uv[2] = {};
      for (int k = 0; k < 3; ++k) {
        s.p[k] = 0;
        s.n[k] = 0;
        for (int c = 0; c < 3; ++c) {
          s.p[k] += w[c] * p[c][k];
          s.n[k] +=
              w[c] * Attribute(geometry.normals, geometry.normals_interp, vi[c],
                               ci[c], static_cast<uint32_t>(face), 3,
                               static_cast<size_t>(k),
                               static_cast<float>(normal[k] / twice_area));
        }
      }
      Normalize(s.n);
      for (int k = 0; k < 2; ++k)
        for (int c = 0; c < 3; ++c)
          uv[k] += w[c] * Attribute(geometry.texcoords_0,
                                    geometry.texcoords_0_interp, vi[c], ci[c],
                                    static_cast<uint32_t>(face), 2,
                                    static_cast<size_t>(k), 0);
      std::string mat = material;
      for (const auto& subset : subsets)
        if (subset.second.count(static_cast<int32_t>(face))) {
          mat = subset.first;
          break;
        }
      auto color = shade(mat, geometry, face, uv);
      for (int k = 0; k < 3; ++k) {
        float display = 0;
        for (int c = 0; c < 3; ++c)
          display += w[c] * Attribute(geometry.colors, geometry.colors_interp,
                                      vi[c], ci[c], static_cast<uint32_t>(face),
                                      3, static_cast<size_t>(k), 1);
        s.color[k] = color[k] * display;
      }
      float alpha = 0;
      for (int c = 0; c < 3; ++c)
        alpha += w[c] * Attribute(geometry.opacities, geometry.opacities_interp,
                                  vi[c], ci[c], static_cast<uint32_t>(face), 1,
                                  0, 1);
      s.opacity = color[3] * alpha;
      s.radius = static_cast<float>(
          std::sqrt(area * stride / (3.141592653589793 * count)) * 1.5);
      if (!emit(transform(s), ancestors)) return false;
    }
    ++triangle_count;
    if ((triangle_count & 4095) == 0 && !check("sample", triangle_count))
      return false;
  }
  return true;
}

bool Build::consume(usd::pcp::ComposedBatch& batch) {
  if (!batch.layer) return true;
  if (cell_size <= 0 && !extent_reservoir.empty()) {
    double longest = 0;
    for (int k = 0; k < 3; ++k) {
      std::vector<double> lows, highs;
      for (const auto& b : extent_reservoir) {
        lows.push_back(b[k]);
        highs.push_back(b[k + 3]);
      }
      std::sort(lows.begin(), lows.end());
      std::sort(highs.begin(), highs.end());
      const size_t trim = lows.size() / 100;
      longest = std::max(longest, highs[highs.size() - 1 - trim] - lows[trim]);
    }
    if (longest > 0)
      cell_size = longest / (opts.resolution ? opts.resolution : 2048);
  }
  usd::Stage stage;
  stage.SetRootLayer(std::move(*batch.layer));
  const auto* layer = stage.GetRootLayer();
  const auto& prims = layer->prims();
  std::vector<std::pair<std::string, std::array<double, 16>>> ancestors;
  for (size_t i = 0; i < prims.size(); ++i) {
    if (i < batch.repeated.size() && batch.repeated[i]) continue;
    usd::UsdPrim prim(&prims[i], layer, static_cast<uint32_t>(i));
    const std::string path = prim.GetPath().str();
    ++result.source_prims;
    while (!ancestors.empty() && !Prefix(path, ancestors.back().first)) {
      if (!flush_prototype(ancestors.back().first)) return false;
      prototype_buffers.erase(ancestors.back().first);
      ancestors.pop_back();
    }
    const auto& type = prim.GetTypeName();
    const auto& proto = prim.GetMeta().instance_prototype();
    const bool native_instance = !proto.empty() && proto != path;
    const bool proto_root = prototypes.count(path) != 0;
    const bool renderable = type == "Mesh" || type == "Cube" ||
                            type == "Sphere" || type == "Cylinder" ||
                            type == "Cone" || type == "Capsule" ||
                            type == "Plane";
    if (!renderable && !native_instance && !proto_root &&
        type != "PointInstancer" && type != "Camera" &&
        type.find("Light") == std::string::npos)
      continue;
    if (!Visible(prim, opts.time_code)) continue;
    auto world = Identity();
    usd::ComputeWorldTransform(stage, prim, world.data(), opts.time_code);
    if (proto_root && !native_instance) {
      auto inv = Identity();
      if (!Inverse(world.data(), inv.data())) {
        result.error = "singular preview prototype transform: " + path;
        return false;
      }
      if (point_prototypes.count(path)) {
        auto local = Identity();
        usd::ComputeLocalTransform(prim, local.data(), opts.time_code);
        point_prototype_local[path] = local;
      }
      ancestors.emplace_back(path, inv);
      std::ofstream create(prototype_file(path), std::ios::binary);
      if (!create) {
        result.error = "cannot create preview prototype spill";
        return false;
      }
    }
    if (native_instance) {
      ++result.source_instances;
      if (!queue_placement(proto, world.data(), ancestors)) return false;
    } else if (renderable) {
      if (!mesh(stage, prim, world.data(), ancestors)) return false;
    } else if (type == "PointInstancer") {
      usd::UsdGeomPointInstancer pi(prim);
      const auto targets = pi.GetPrototypes();
      const auto indices = pi.GetProtoIndices(opts.time_code);
      const auto transforms = pi.ComputeInstanceTransforms(opts.time_code);
      const auto mask = pi.ComputeMaskAtTime(opts.time_code);
      for (size_t j = 0; j < std::min(indices.size(), transforms.size()); ++j) {
        if (j < mask.size() && !mask[j]) continue;
        if (indices[j] < 0 || static_cast<size_t>(indices[j]) >= targets.size())
          continue;
        const std::string target =
            targets[static_cast<size_t>(indices[j])].str();
        double placed[16];
        Multiply(world.data(), transforms[j].matrix, placed);
        ++result.source_instances;
        if (!queue_placement(target, placed, ancestors, true)) return false;
      }
    }
    if ((i & 4095) == 0 && !check("compose", result.source_prims)) return false;
  }
  for (const auto& pending : prototype_buffers)
    if (!flush_prototype(pending.first)) return false;
  prototype_buffers.clear();
  return check("batch", result.source_prims);
}

namespace {
bool WriteString(std::ostream& stream, const std::string& value) {
  if (value.size() > 1048576) return false;
  const uint32_t size = static_cast<uint32_t>(value.size());
  stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
  stream.write(value.data(), size);
  return bool(stream);
}
bool ReadString(std::istream& stream, std::string* value) {
  uint32_t size = 0;
  stream.read(reinterpret_cast<char*>(&size), sizeof(size));
  if (!stream || size > 1048576) return false;
  value->resize(size);
  stream.read(&(*value)[0], size);
  return bool(stream);
}
}  // namespace
bool Build::queue_placement(
    const std::string& key, const double* world,
    const std::vector<std::pair<std::string, std::array<double, 16>>>&
        ancestors,
    bool point) {
  const auto canonical = resolve_prototype(key);
  if (canonical.empty()) {
    result.error = "cyclic preview prototype aliases";
    return false;
  }
  if (!placements.is_open())
    placements.open(directory / "prototypes" / "placements", std::ios::binary);
  if (!WriteString(placements, canonical)) {
    result.error = "cannot spill preview placements";
    return false;
  }
  const uint8_t is_point = point ? 1 : 0;
  placements.write(reinterpret_cast<const char*>(&is_point), 1);
  placements.write(reinterpret_cast<const char*>(world), 16 * sizeof(double));
  const uint32_t count = static_cast<uint32_t>(ancestors.size());
  placements.write(reinterpret_cast<const char*>(&count), sizeof(count));
  for (const auto& a : ancestors) {
    if (!WriteString(placements, a.first)) return false;
    placements.write(reinterpret_cast<const char*>(a.second.data()),
                     16 * sizeof(double));
    ++pending_prototype_placements[a.first];
  }
  if (!placements) {
    result.error = "cannot spill preview placements";
    return false;
  }
  return true;
}
bool Build::finish_placements() {
  if (!placements.is_open()) return true;
  placements.close();
  const fs::path current = directory / "prototypes" / "placements",
                 next = directory / "prototypes" / "placements.next";
  for (size_t round = 0; round < 1024; ++round) {
    std::ifstream input(current, std::ios::binary);
    std::ofstream deferred(next, std::ios::binary);
    uint64_t done = 0, waiting = 0;
    for (;;) {
      if (input.peek() == std::char_traits<char>::eof()) break;
      std::string key;
      auto world = Identity();
      uint32_t count = 0;
      if (!ReadString(input, &key)) {
        result.error = "invalid preview placement spill";
        return false;
      }
      uint8_t is_point = 0;
      input.read(reinterpret_cast<char*>(&is_point), 1);
      input.read(reinterpret_cast<char*>(world.data()), 16 * sizeof(double));
      input.read(reinterpret_cast<char*>(&count), sizeof(count));
      if (count > 1024) {
        result.error = "preview instance nesting limit exceeded";
        return false;
      }
      std::vector<std::pair<std::string, std::array<double, 16>>> ancestors;
      for (uint32_t i = 0; i < count; ++i) {
        std::string path;
        auto inverse = Identity();
        if (!ReadString(input, &path)) return false;
        input.read(reinterpret_cast<char*>(inverse.data()),
                   16 * sizeof(double));
        ancestors.emplace_back(std::move(path), inverse);
      }
      if (!input) {
        result.error = "truncated preview placement spill";
        return false;
      }
      if (pending_prototype_placements[key] == 0) {
        auto placed = world;
        const auto local = point_prototype_local.find(key);
        if (is_point && local != point_prototype_local.end())
          Multiply(world.data(), local->second.data(), placed.data());
        if (!replay(key, placed.data(), ancestors)) return false;
        for (const auto& a : ancestors) {
          if (!flush_prototype(a.first)) return false;
          --pending_prototype_placements[a.first];
        }
        ++done;
      } else {
        if (!WriteString(deferred, key)) return false;
        deferred.write(reinterpret_cast<const char*>(&is_point), 1);
        deferred.write(reinterpret_cast<const char*>(world.data()),
                       16 * sizeof(double));
        deferred.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (const auto& a : ancestors) {
          if (!WriteString(deferred, a.first)) return false;
          deferred.write(reinterpret_cast<const char*>(a.second.data()),
                         16 * sizeof(double));
        }
        ++waiting;
      }
      if (((done + waiting) & 4095) == 0 && !check("instances", done + waiting))
        return false;
    }
    if (!input.eof() || !deferred) {
      result.error = "preview placement spill I/O failed";
      return false;
    }
    input.close();
    deferred.close();
    std::error_code ec;
    fs::rename(next, current, ec);
    if (ec) {
      result.error = "cannot rotate preview placement spill";
      return false;
    }
    if (waiting == 0) return true;
    if (done == 0) {
      result.error = "cyclic or unresolved preview prototype dependencies";
      return false;
    }
  }
  result.error = "preview prototype dependency depth exceeded";
  return false;
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
