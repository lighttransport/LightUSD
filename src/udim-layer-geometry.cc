// SPDX-License-Identifier: Apache-2.0
#if defined(__clang__)
// Exact equality is required for authored time keys and discrete opinions.
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif
#include "udim-layer-geometry.hh"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

#include "safe-arithmetic.hh"
#include "tsd/tinysubdiv.hh"
namespace lightusd::udim {
namespace {
bool Fail(std::string* e, const std::string& s) {
  if (e) *e = "UDIM bake: " + s;
  return false;
}
constexpr double Default = std::numeric_limits<double>::infinity();
std::vector<double> AllTimes(const LayerAccess& a, const std::string& p,
                             const std::string& n) {
  auto t = a.times(p, n);
  t.insert(t.begin(), Default);
  return t;
}
bool Fit(size_t n, size_t stride, const Options& o) {
  return stride && n <= o.memory_budget_bytes / stride;
}
std::string Interp(const LayerAccess& a, const std::string& p,
                   const std::string& n) {
  if (n == "points" || n == "velocities" || n == "accelerations")
    return "vertex";
  auto i = a.interpolation(p, n);
  return i.empty() && n == "normals" ? "varying" : i;
}
}  // namespace
bool PrepareGeometry(LayerAccess& a, const std::string& mesh, const Options& o,
                     GeometryEdits* edits, std::string* e) {
  Numeric points;
  if (!a.numeric(mesh, "points", &points))
    return Fail(e, "missing mesh points");
  const size_t vertices = points.values.size() / 3;
  const auto ji = "primvars:skel:jointIndices",
             jw = "primvars:skel:jointWeights";
  Numeric indices, weights;
  const bool has_i = a.numeric(mesh, ji, &indices),
             has_w = a.numeric(mesh, jw, &weights);
  if (has_i != has_w) return Fail(e, "incomplete skin influences at " + mesh);
  if (has_i && Interp(a, mesh, ji) != "constant") {
    const size_t k = a.elementSize(mesh, ji);
    if (Interp(a, mesh, ji) != "vertex" || Interp(a, mesh, jw) != "vertex")
      return Fail(e, "skin influences require matching vertex interpolation");
    if (!k || k > 4096 || k != a.elementSize(mesh, jw) ||
        !Fit(vertices, k * 32, o))
      return Fail(e, "invalid skin elementSize or memory limit");
    std::set<double> times;
    times.insert(Default);
    for (double t : a.times(mesh, ji)) {
      Numeric sample;
      if (!a.numeric(mesh, ji, &sample, t) || sample.values != indices.values)
        return Fail(e,
                    "animated joint indices change skin ownership at " + mesh);
    }
    for (double t : a.times(mesh, jw)) times.insert(t);
    std::set<int32_t> joints;
    for (double time : times) {
      Numeric idx, w;
      if (!a.numeric(mesh, ji, &idx, time) || !a.numeric(mesh, jw, &w, time) ||
          idx.values.size() != vertices * k ||
          w.values.size() != idx.values.size())
        return Fail(e, "skin cardinality mismatch");
      for (size_t i = 0; i < idx.values.size(); ++i) {
        if (idx.values[i] < 0 || idx.values[i] > INT32_MAX ||
            std::floor(idx.values[i]) != idx.values[i] ||
            !std::isfinite(w.values[i]) || w.values[i] < 0)
          return Fail(e, "invalid skin influence");
        if (w.values[i] > 0) joints.insert(int32_t(idx.values[i]));
      }
    }
    if (joints.empty() || !Fit(vertices, joints.size() * 64, o))
      return Fail(e, "skin influence expansion exceeds memory limit");
    edits->skin_joints.assign(joints.begin(), joints.end());
    const size_t j = joints.size();
    std::map<int32_t, size_t> columns;
    size_t col = 0;
    for (int32_t id : joints) columns[id] = col++;
    // Read all samples before replacing defaults or indices; each buffer is
    // charged to the bake budget and no authored animation times are removed.
    std::vector<std::pair<double, Numeric>> expanded;
    size_t animation_stride = 0;
    if (!safe::mul(j * 16, times.size(), &animation_stride) ||
        !Fit(vertices, animation_stride, o))
      return Fail(e, "skin animation expansion exceeds memory limit");
    for (double time : times) {
      Numeric idx, w;
      a.numeric(mesh, ji, &idx, time);
      a.numeric(mesh, jw, &w, time);
      Numeric result;
      result.values.assign(vertices * j, 0);
      for (size_t v = 0; v < vertices; ++v) {
        double sum = 0;
        for (size_t c = 0; c < k; ++c) sum += w.values[v * k + c];
        if (sum <= 0) return Fail(e, "zero-total skin weights");
        for (size_t c = 0; c < k; ++c)
          if (w.values[v * k + c] > 0)
            result.values[v * j + columns[int32_t(idx.values[v * k + c])]] +=
                w.values[v * k + c] / sum;
      }
      expanded.push_back({time, std::move(result)});
    }
    Numeric common;
    common.integral = true;
    common.values.reserve(vertices * j);
    for (size_t v = 0; v < vertices; ++v)
      for (int32_t id : joints) common.values.push_back(id);
    for (const auto& sample : expanded) {
      if (!a.writeNumeric(mesh, jw, sample.second, sample.first) ||
          !a.writeNumeric(mesh, ji, common, Default))
        return Fail(e, "skin column authoring failed");
    }
    for (double t : a.times(mesh, ji))
      if (!a.writeNumeric(mesh, ji, common, t))
        return Fail(e, "skin index sample authoring failed");
    a.setElementSize(mesh, ji, j);
    a.setElementSize(mesh, jw, j);
    a.setInterpolation(mesh, ji, "vertex");
    a.setInterpolation(mesh, jw, "vertex");
  }
  std::vector<std::string> clones;
  const auto shapes = a.targets(mesh, "skel:blendShapeTargets");
  for (size_t si = 0; si < shapes.size(); ++si) {
    const auto& source = shapes[si];
    const auto clone = a.cloneBlendShape(mesh, source);
    if (clone.empty()) return Fail(e, "cannot clone blendshape " + source);
    Numeric lookup;
    const bool sparse = a.numeric(source, "pointIndices", &lookup);
    GeometryEdits::Shape shape;
    shape.path = clone;
    for (const auto& prop : a.properties(source)) {
      const bool offset =
          prop == "offsets" || prop == "normalOffsets" ||
          (prop.size() >= 7 && prop.substr(prop.size() - 7) == "offsets") ||
          (prop.size() >= 13 &&
           prop.substr(prop.size() - 13) == "normalOffsets");
      if (!offset) continue;
      const auto channel =
          "primvars:_udimBlend" + std::to_string(si) + ":" + prop;
      for (double time : AllTimes(a, source, prop)) {
        Numeric n;
        if (!a.numeric(source, prop, &n, time) || n.components != 3)
          return Fail(e, "unsupported blendshape offsets");
        if ((sparse ? lookup.values.size() : vertices) != n.values.size() / 3 ||
            !Fit(vertices, 48, o))
          return Fail(e, "blendshape cardinality or memory limit");
        Numeric full;
        full.components = 3;
        full.values.assign(vertices * 3, 0);
        for (size_t i = 0; i < n.values.size() / 3; ++i) {
          const double id = sparse ? lookup.values[i] : double(i);
          if (id < 0 || id >= double(vertices) || std::floor(id) != id)
            return Fail(e, "blendshape point index out of range");
          for (size_t c = 0; c < 3; ++c)
            full.values[size_t(id) * 3 + c] = n.values[i * 3 + c];
        }
        if (!a.writeNumeric(mesh, channel, full, time))
          return Fail(e, "blendshape channel authoring failed");
      }
      a.setInterpolation(mesh, channel, "vertex");
      shape.channels.push_back({prop, channel});
    }
    edits->shapes.push_back(std::move(shape));
    clones.push_back(clone);
  }
  if (!clones.empty()) a.setTargets(mesh, "skel:blendShapeTargets", clones);
  return true;
}
bool FinishGeometry(LayerAccess& a, const std::string& mesh,
                    const GeometryEdits& edits, const Options& o,
                    std::string* e) {
  Numeric points;
  if (!a.numeric(mesh, "points", &points))
    return Fail(e, "missing remapped points");
  const size_t vertices = points.values.size() / 3;
  if (!edits.skin_joints.empty()) {
    Numeric idx;
    idx.integral = true;
    const size_t j = edits.skin_joints.size();
    if (!Fit(vertices, j * 16, o))
      return Fail(e, "refined skin indices exceed memory limit");
    for (size_t v = 0; v < vertices; ++v)
      for (int32_t id : edits.skin_joints) idx.values.push_back(id);
    for (double time : AllTimes(a, mesh, "primvars:skel:jointWeights")) {
      Numeric weights;
      if (!a.numeric(mesh, "primvars:skel:jointWeights", &weights, time) ||
          weights.values.size() != vertices * j)
        return Fail(e, "refined skin cardinality mismatch");
      for (size_t v = 0; v < vertices; ++v) {
        double sum = 0;
        for (size_t k = 0; k < j; ++k) sum += weights.values[v * j + k];
        if (sum <= 0) return Fail(e, "zero-total refined skin weights");
        for (size_t k = 0; k < j; ++k) weights.values[v * j + k] /= sum;
      }
      if (!a.writeNumeric(mesh, "primvars:skel:jointWeights", weights, time))
        return Fail(e, "refined weights authoring failed");
    }
    for (double time : AllTimes(a, mesh, "primvars:skel:jointIndices"))
      if (!a.writeNumeric(mesh, "primvars:skel:jointIndices", idx, time))
        return Fail(e, "refined indices authoring failed");
  }
  for (const auto& shape : edits.shapes) {
    Numeric idx;
    idx.integral = true;
    for (size_t v = 0; v < vertices; ++v) idx.values.push_back(double(v));
    if (!a.writeNumeric(shape.path, "pointIndices", idx))
      return Fail(e, "blendshape index authoring failed");
    for (const auto& channel : shape.channels) {
      for (double t : AllTimes(a, mesh, channel.second)) {
        Numeric n;
        if (!a.numeric(mesh, channel.second, &n, t) ||
            !a.writeNumeric(shape.path, channel.first, n, t))
          return Fail(e, "blendshape offsets authoring failed");
      }
      a.erase(mesh, channel.second);
    }
  }
  return true;
}
bool RefineGeometry(LayerAccess& a, const std::string& mesh, const Options& o,
                    std::vector<uint32_t>* provenance, std::string* e) {
  for (const auto& prop :
       {"subdivisionScheme", "interpolateBoundary", "creaseMethod",
        "triangleSubdivisionRule", "faceVaryingLinearInterpolation"})
    if (a.dynamic(mesh, prop))
      return Fail(e, "animated subdivision rule at " + mesh);
  const auto scheme = a.text(mesh, "subdivisionScheme");
  if (scheme == "none") return true;
  if (!scheme.empty() && scheme != "catmullClark" && scheme != "loop" &&
      scheme != "bilinear")
    return Fail(e, "unsupported subdivision scheme");
  Numeric p, c, i;
  if (!a.numeric(mesh, "points", &p) ||
      !a.numeric(mesh, "faceVertexCounts", &c) ||
      !a.numeric(mesh, "faceVertexIndices", &i))
    return Fail(e, "incomplete subdivision mesh");
  tsd::Options opts;
  opts.level = o.subdivision_level;
  opts.scheme = scheme == "loop"       ? tsd::Scheme::Loop
                : scheme == "bilinear" ? tsd::Scheme::Bilinear
                                       : tsd::Scheme::CatmullClark;
  const auto boundary = a.text(mesh, "interpolateBoundary");
  opts.boundary = boundary == "none" ? tsd::BoundaryInterpolation::None
                  : boundary == "edgeOnly"
                      ? tsd::BoundaryInterpolation::EdgeOnly
                      : tsd::BoundaryInterpolation::EdgeAndCorner;
  opts.creasing = a.text(mesh, "creaseMethod") == "chaikin"
                      ? tsd::CreasingMethod::Chaikin
                      : tsd::CreasingMethod::Uniform;
  opts.triangle_subdivision =
      a.text(mesh, "triangleSubdivisionRule") == "smooth"
          ? tsd::TriangleSubdivision::Smooth
          : tsd::TriangleSubdivision::CatmullClark;
  size_t faces = c.values.size(), corners = i.values.size();
  for (int level = 0; level < opts.level; ++level) {
    if (!safe::mul(level ? faces : corners, level ? size_t(4) : size_t(1),
                   &faces) ||
        faces > o.memory_budget_bytes / 512)
      return Fail(e, "subdivision exceeds working-memory limit");
  }
  opts.max_faces =
      uint32_t(std::min<size_t>(o.memory_budget_bytes / 512, INT32_MAX));
  opts.max_vertices = opts.max_faces;
  opts.max_face_vertex_indices =
      uint32_t(std::min<size_t>(o.memory_budget_bytes / 128, INT32_MAX));
  std::vector<float> points(p.values.begin(), p.values.end());
  std::vector<uint32_t> counts, indices;
  for (double v : c.values) counts.push_back(uint32_t(v));
  for (double v : i.values) indices.push_back(uint32_t(v));
  tsd::MeshView view;
  view.points = points.data();
  view.num_points = uint32_t(points.size() / 3);
  view.face_vertex_counts = counts.data();
  view.num_faces = uint32_t(counts.size());
  view.face_vertex_indices = indices.data();
  view.num_face_vertex_indices = uint32_t(indices.size());
  std::map<std::string, std::vector<int32_t>> tags_i;
  std::map<std::string, std::vector<float>> tags_f;
  for (const auto& prop :
       {"cornerIndices", "creaseIndices", "creaseLengths", "holeIndices"}) {
    Numeric n;
    if (a.numeric(mesh, prop, &n)) {
      if (a.dynamic(mesh, prop)) return Fail(e, "animated subdivision tags");
      for (double v : n.values) {
        if (!std::isfinite(v) || v < 0 || v > INT32_MAX || std::floor(v) != v)
          return Fail(e, "invalid subdivision index or length");
        tags_i[prop].push_back(int32_t(v));
      }
    }
  }
  for (const auto& prop : {"cornerSharpnesses", "creaseSharpnesses"}) {
    Numeric n;
    if (a.numeric(mesh, prop, &n)) {
      if (a.dynamic(mesh, prop))
        return Fail(e, "animated subdivision sharpnesses");
      tags_f[prop].assign(n.values.begin(), n.values.end());
    }
  }
  view.corner_indices = tags_i["cornerIndices"].data();
  view.num_corners = uint32_t(tags_i["cornerIndices"].size());
  view.corner_sharpnesses = tags_f["cornerSharpnesses"].data();
  view.crease_indices = tags_i["creaseIndices"].data();
  view.num_crease_indices = uint32_t(tags_i["creaseIndices"].size());
  view.crease_lengths = tags_i["creaseLengths"].data();
  view.num_crease_lengths = uint32_t(tags_i["creaseLengths"].size());
  view.crease_sharpnesses = tags_f["creaseSharpnesses"].data();
  view.num_crease_sharpnesses = uint32_t(tags_f["creaseSharpnesses"].size());
  view.hole_indices = tags_i["holeIndices"].data();
  view.num_holes = uint32_t(tags_i["holeIndices"].size());
  if (tags_f["cornerSharpnesses"].size() != view.num_corners)
    return Fail(e, "corner sharpness cardinality mismatch");
  tsd::RefinedMesh topology;
  if (tsd::Refine(view, opts, &topology, e) != tsd::Result::Success)
    return false;
  *provenance = topology.face_source;
  const size_t out_vertices = topology.points.size() / 3,
               out_corners = topology.face_vertex_indices.size();
  const auto fvar_token = a.text(mesh, "faceVaryingLinearInterpolation");
  const auto fvar =
      fvar_token == "none"          ? tsd::FVarLinearInterpolation::None
      : fvar_token == "cornersOnly" ? tsd::FVarLinearInterpolation::CornersOnly
      : fvar_token == "cornersPlus2"
          ? tsd::FVarLinearInterpolation::CornersPlus2
      : fvar_token == "boundaries" ? tsd::FVarLinearInterpolation::Boundaries
      : fvar_token == "all"        ? tsd::FVarLinearInterpolation::All
                                   : tsd::FVarLinearInterpolation::CornersPlus1;
  size_t retained =
      topology.points.size() * 4 + out_corners * 8 + provenance->size() * 4;
  for (const auto& prop : a.properties(mesh)) {
    const auto interp = Interp(a, mesh, prop);
    if (interp.empty() || interp == "constant" || prop == "faceVertexCounts" ||
        prop == "faceVertexIndices" || prop == "primvars:skel:jointIndices" ||
        (prop.size() >= 8 && prop.substr(prop.size() - 8) == ":indices"))
      continue;
    Numeric lookup;
    const bool indexed = a.numeric(mesh, prop + ":indices", &lookup);
    if (a.dynamic(mesh, prop + ":indices"))
      return Fail(e, "animated primvar indices");
    for (double time : AllTimes(a, mesh, prop)) {
      Numeric n;
      if (!a.numeric(mesh, prop, &n, time))
        return Fail(e, "unsupported subdivision attribute " + prop);
      size_t lanes = 0;
      if (!safe::mul(n.components, a.elementSize(mesh, prop), &lanes) ||
          !lanes || lanes > 4096)
        return Fail(e, "invalid subdivision elementSize");
      if (indexed) {
        std::vector<double> expanded;
        if (!Fit(lookup.values.size(), lanes * 8, o))
          return Fail(e, "primvar expansion exceeds memory limit");
        for (double v : lookup.values) {
          if (v < 0 || v >= double(n.values.size() / lanes) ||
              std::floor(v) != v)
            return Fail(e, "invalid indexed primvar");
          for (size_t k = 0; k < lanes; ++k)
            expanded.push_back(n.values[size_t(v) * lanes + k]);
        }
        n.values = std::move(expanded);
      }
      Numeric out;
      out.components = n.components;
      out.integral = n.integral;
      if (interp == "uniform") {
        if (n.values.size() != counts.size() * lanes)
          return Fail(e, "uniform cardinality mismatch");
        for (uint32_t f : topology.face_source)
          for (size_t k = 0; k < lanes; ++k)
            out.values.push_back(n.values[size_t(f) * lanes + k]);
      } else {
        const size_t source_count =
            interp == "faceVarying" ? indices.size() : points.size() / 3;
        const size_t output_count =
            interp == "faceVarying" ? out_corners : out_vertices;
        if (n.values.size() != source_count * lanes ||
            !Fit(output_count, lanes * 48, o))
          return Fail(e, "subdivision attribute cardinality or memory limit");
        if (n.integral) {
          // Discrete data may be copied when all contributing values agree.
          // Reject variation instead of accidentally accepting integer
          // averages.
          for (size_t v = 1; v < source_count; ++v)
            for (size_t k = 0; k < lanes; ++k)
              if (n.values[v * lanes + k] != n.values[k])
                return Fail(
                    e, "subdivision cannot interpolate discrete attribute " +
                           prop);
        }
        out.values.resize(output_count * lanes);
        // Keep codec and subdivision working sets finite, refining at most
        // four components at once instead of all animation samples together.
        for (size_t lane = 0; lane < lanes; lane += 4) {
          const size_t stride = std::min<size_t>(4, lanes - lane);
          std::vector<float> buffer(source_count * stride);
          for (size_t v = 0; v < source_count; ++v)
            for (size_t k = 0; k < stride; ++k)
              buffer[v * stride + k] = float(n.values[v * lanes + lane + k]);
          tsd::RefinedMesh r;
          tsd::Result status;
          if (interp == "faceVarying") {
            tsd::FVarChannelView channel;
            channel.values = buffer.data();
            channel.num_values = uint32_t(source_count);
            channel.stride = uint32_t(stride);
            channel.interpolation = fvar;
            status = tsd::Refine(view, &channel, 1, nullptr, 0, opts, &r, e);
          } else {
            tsd::VertexPrimvarView channel;
            channel.values = buffer.data();
            channel.stride = uint32_t(stride);
            channel.varying = interp == "varying";
            status = tsd::Refine(view, nullptr, 0, &channel, 1, opts, &r, e);
          }
          if (status != tsd::Result::Success) return false;
          const auto& values =
              interp == "faceVarying" ? r.fvar[0] : r.vertex_primvars[0];
          for (size_t v = 0; v < output_count; ++v)
            for (size_t k = 0; k < stride; ++k) {
              const double x = double(values[v * stride + k]);
              if (n.integral && std::floor(x) != x)
                return Fail(
                    e, "subdivision cannot interpolate discrete attribute " +
                           prop);
              out.values[v * lanes + lane + k] = x;
            }
        }
      }
      NormalizeNormals(prop, &out);
      const size_t bytes = out.values.size() * 8;
      if (retained > o.memory_budget_bytes ||
          bytes > o.memory_budget_bytes - retained)
        return Fail(e, "subdivision output exceeds aggregate memory limit");
      retained += bytes;
      if (!a.writeNumeric(mesh, prop, out, time))
        return Fail(e, "subdivision attribute authoring failed");
    }
    if (indexed) a.erase(mesh, prop + ":indices");
  }
  Numeric new_c, new_i;
  new_c.integral = new_i.integral = true;
  for (uint32_t v : topology.face_vertex_counts) new_c.values.push_back(v);
  for (uint32_t v : topology.face_vertex_indices) new_i.values.push_back(v);
  if (!a.writeNumeric(mesh, "faceVertexCounts", new_c) ||
      !a.writeNumeric(mesh, "faceVertexIndices", new_i))
    return Fail(e, "subdivision topology edit failed");
  a.setToken(mesh, "subdivisionScheme", "none");
  for (const auto& prop :
       {"cornerIndices", "cornerSharpnesses", "creaseIndices", "creaseLengths",
        "creaseSharpnesses", "holeIndices"})
    a.erase(mesh, prop);
  return true;
}
}  // namespace lightusd::udim
