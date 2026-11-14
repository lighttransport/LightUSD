// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Shared render-oriented extraction helpers for ::lightusd::Stage.

#include "render-extract.hh"
#include "layer/prim-spec.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "schema/geom-point-instancer.hh"
#include "schema/geom-xform.hh"
#include "schema/physics-api.hh"
#include "schema/usd-shade.hh"
#include "schema/usd-skel.hh"
#include "eval/xform-eval.hh"
#include <unordered_map>
#include "scene-access.hh"

namespace lightusd {
namespace tydra {
namespace {

void Identity(double m[16]) {
  std::memset(m, 0, sizeof(double) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.0;
}

void MulRowMajor(const double a[16], const double b[16], double out[16]) {
  double r[16];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                     a[i * 4 + 1] * b[1 * 4 + j] +
                     a[i * 4 + 2] * b[2 * 4 + j] +
                     a[i * 4 + 3] * b[3 * 4 + j];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

bool IsLightType(const std::string& t) {
  return t == "RectLight" || t == "SphereLight" || t == "DiskLight" ||
         t == "CylinderLight" || t == "DistantLight" || t == "DomeLight" ||
         t == "DomeLight_1" || t == "PointLight" ||
         t == "GeometryLight" || t == "PortalLight" ||
         t == "PluginLight" || t == "LightFilter" ||
         t == "PluginLightFilter";
}

bool IsCurveType(const std::string& t) {
  return t == "BasisCurves" || t == "NurbsCurves" ||
         t == "HermiteCurves";
}

RenderPrimKind Classify(const ::lightusd::UsdPrim& prim,
                        const std::string& type_name,
                        std::string* native_prototype) {
  const ::lightusd::PrimSpec* spec = prim.GetPrimSpec();
  if (spec && !spec->meta().instance_prototype().empty()) {
    if (native_prototype) *native_prototype = spec->meta().instance_prototype();
    return RenderPrimKind::NativeInstance;
  }
  if (IsMeshRenderableTypeName(type_name)) return RenderPrimKind::Mesh;
  if (type_name == "PointInstancer") return RenderPrimKind::PointInstancer;
  if (IsLightType(type_name)) return RenderPrimKind::Light;
  if (type_name == "Camera") return RenderPrimKind::Camera;
  if (type_name == "Material") return RenderPrimKind::Material;
  if (type_name == "Volume") return RenderPrimKind::Volume;
  if (IsCurveType(type_name)) return RenderPrimKind::Curve;
  if (type_name == "Skeleton") return RenderPrimKind::Skeleton;
  if (IsUnsupportedRenderableTypeName(type_name)) return RenderPrimKind::Other;
  return RenderPrimKind::Other;
}

std::string PurposeForPrim(const ::lightusd::UsdPrim& prim,
                           const std::string& inherited) {
  // Purpose is inherited. Inspect only an AUTHORED local opinion here;
  // UsdPrim::GetPropertyValue also exposes the schema fallback "default",
  // which must not shadow an ancestor's authored "render/proxy/guide".
  const ::lightusd::PrimSpec* spec = prim.GetPrimSpec();
  if (const ::lightusd::Value* v =
          spec ? spec->property_value("purpose") : nullptr) {
    if (auto s = v->as_token()) {
      if (*s == "render" || *s == "proxy" || *s == "guide") return *s;
      if (*s == "default") return "default";
    }
  }
  return inherited.empty() ? std::string("default") : inherited;
}

void PushRecord(RenderPrimRecord&& rec, const RenderExtractOptions& options,
                RenderExtractResult* out) {
  if (rec.kind == RenderPrimKind::NativeInstance &&
      !rec.native_prototype.empty()) {
    out->native_prototype_holders.insert(rec.native_prototype);
  }
  if (!options.collect_records && !options.collect_categories) return;
  out->storage.push_back(std::move(rec));
  RenderPrimRecord* stored = &out->storage.back();
  if (options.collect_records) out->records.push_back(stored);
  if (!options.collect_categories) return;
  if (stored->type_name == "Points" ||
      stored->type_name == "ParticleField3DGaussianSplat" ||
      stored->type_name == "ParticleField") {
    out->points.push_back(stored);
    return;
  }
  switch (stored->kind) {
    case RenderPrimKind::Mesh: out->meshes.push_back(stored); break;
    case RenderPrimKind::PointInstancer:
      out->point_instancers.push_back(stored);
      break;
    case RenderPrimKind::NativeInstance:
      out->native_instances.push_back(stored);
      break;
    case RenderPrimKind::Light: out->lights.push_back(stored); break;
    case RenderPrimKind::Camera: out->cameras.push_back(stored); break;
    case RenderPrimKind::Material:
      out->materials.push_back(stored);
      break;
    case RenderPrimKind::Volume: out->volumes.push_back(stored); break;
    case RenderPrimKind::Curve: out->curves.push_back(stored); break;
    case RenderPrimKind::Skeleton:
      out->skeletons.push_back(stored);
      break;
    default: break;
  }
}

void CollectRec(const ::lightusd::UsdPrim& root,
                const RenderExtractOptions& options,
                const double parent_world[16],
                const std::string& inherited_purpose,
                const std::string& inherited_material,
                const std::string& inherited_strong_material,
                RenderExtractResult* out) {
  struct Frame {
    ::lightusd::UsdPrim prim;
    std::string purpose;
    std::string material;
    std::string strong_material;
    double parent_world[16];
    size_t depth = 0;
    // IsActive() walks all ancestors. Carry the accumulated state instead so
    // a deep hierarchy remains O(number of prims), not O(depth squared).
    bool active = true;
    bool animated_world = false;
  };

  std::vector<Frame> stack;
  size_t emitted_records = 0;
  Frame first;
  first.prim = root;
  first.purpose = inherited_purpose;
  first.material = inherited_material;
  first.strong_material = inherited_strong_material;
  std::memcpy(first.parent_world, parent_world, sizeof(first.parent_world));
  first.active = options.include_inactive || root.IsActive();
  stack.push_back(std::move(first));

  while (!stack.empty()) {
    Frame frame = std::move(stack.back());
    stack.pop_back();
    if (options.max_depth != 0 && frame.depth > options.max_depth) {
      out->limit_exceeded = true;
      continue;
    }
    const ::lightusd::UsdPrim& prim = frame.prim;
    if (!frame.active && !options.include_inactive) continue;

    RenderPrimRecord rec;
    rec.prim = prim;
    rec.path = prim.GetPath().str();
    rec.type_name = prim.GetTypeName();
    rec.purpose = PurposeForPrim(prim, frame.purpose);
    rec.animated_world =
        frame.animated_world ||
        ::lightusd::UsdGeomXform(prim).HasAnimatedTransform() ||
        ::lightusd::HasPhysicsRigidBodyAPI(prim);
    const std::string local_material =
        ::lightusd::GetBoundMaterialPath(prim);
    const std::string nearest_material =
        local_material.empty() ? frame.material : local_material;
    std::string strong_material = frame.strong_material;
    if (strong_material.empty() && !local_material.empty() &&
        ::lightusd::BindingIsStrongerThanDescendants(prim)) {
      strong_material = local_material;
    }
    rec.material_path = strong_material.empty() ? nearest_material
                                                 : strong_material;
    ComputeLocalTransform(prim, rec.local, options.time_code);
    rec.has_reset_xform = HasResetXformStack(prim);
    if (rec.has_reset_xform) {
      std::memcpy(rec.world, rec.local, sizeof(rec.world));
    } else {
      MulRowMajor(rec.local, frame.parent_world, rec.world);
    }
    rec.kind = Classify(prim, rec.type_name, &rec.native_prototype);
    const bool stop_children =
        (rec.kind == RenderPrimKind::PointInstancer &&
         options.stop_at_point_instancers) ||
        (rec.kind == RenderPrimKind::NativeInstance &&
         options.stop_at_native_instances);
    const bool collect_this_record =
        rec.kind != RenderPrimKind::Other || options.collect_other;
    if (collect_this_record) {
      if (options.max_records != 0 && emitted_records >= options.max_records) {
        out->limit_exceeded = true;
        continue;
      }
      ++emitted_records;
    }

    // GetChildren() materializes a vector for every prim.  The iterative
    // extractor already owns its traversal stack, so walk the indexed child
    // handles directly and avoid one temporary allocation per prim.
    if (!stop_children) {
      const size_t child_count = prim.GetChildCount();
      for (size_t child_index = child_count; child_index > 0; --child_index) {
        const ::lightusd::UsdPrim child_prim =
            prim.GetChildAt(child_index - 1);
        if (!child_prim.IsValid()) continue;
        Frame child_frame;
        child_frame.prim = child_prim;
        child_frame.purpose = rec.purpose;
        child_frame.material = nearest_material;
        child_frame.strong_material = strong_material;
        std::memcpy(child_frame.parent_world, rec.world,
                    sizeof(child_frame.parent_world));
        child_frame.depth = frame.depth + 1;
        child_frame.active = options.include_inactive ||
                             (frame.active && child_prim.GetMeta().active);
        child_frame.animated_world = rec.animated_world;
        stack.push_back(std::move(child_frame));
      }
    }
    if (collect_this_record) PushRecord(std::move(rec), options, out);
  }
}

const ::lightusd::Value* ValueAtOrDefault(
    const ::lightusd::UsdPrim& prim,
    const ::lightusd::PropNameId& name_id, double time,
    ::lightusd::Value* hold) {
  if (!std::isnan(time)) {
    // Linear interpolation between samples (pxr semantics). The interpolated
    // value is parked in the caller-owned `hold` slot (the ValueArrayRead's
    // scratch) so the returned view stays valid while other attributes are
    // read. A shared thread_local slot here previously dangled the first
    // view whenever a caller read two animated attributes before consuming
    // the first (e.g. TetMesh points + tetVertexIndices).
    ::lightusd::Value v = prim.GetInterpolatedValue(name_id, time);
    if (!v.is_empty()) {
      *hold = std::move(v);
      return hold;
    }
    if (const ::lightusd::Value* held =
            prim.GetValueAtTime(name_id, time)) {
      return held;
    }
  }
  return prim.GetPropertyValue(name_id);
}

}  // namespace

std::string ComputeInheritedPurpose(const ::lightusd::UsdPrim& prim) {
  // Walk up to the nearest AUTHORED opinion; the schema fallback "default"
  // must not stop the walk (see PurposeForPrim).
  for (::lightusd::UsdPrim p = prim; p.IsValid(); p = p.GetParent()) {
    const ::lightusd::PrimSpec* spec = p.GetPrimSpec();
    const ::lightusd::Value* v =
        spec ? spec->property_value("purpose") : nullptr;
    auto token = v ? v->as_token() : nullptr;
    if (token && (*token == "default" || *token == "render" ||
                  *token == "proxy" || *token == "guide")) {
      return *token;
    }
  }
  return "default";
}

bool IsAnalyticGeomTypeName(const std::string& type_name) {
  return type_name == "Cube" || type_name == "Sphere" ||
         type_name == "Cone" || type_name == "Cylinder" ||
         type_name == "Capsule" || type_name == "Plane" ||
         type_name == "Cylinder_1" || type_name == "Capsule_1";
}

bool IsMeshRenderableTypeName(const std::string& type_name) {
  return type_name == "Mesh" || type_name == "TetMesh" ||
         IsAnalyticGeomTypeName(type_name);
}

bool IsUnsupportedRenderableTypeName(const std::string& type_name) {
  return type_name == "Points" ||
         type_name == "ParticleField3DGaussianSplat" || type_name == "ParticleField" ||
         type_name == "NurbsPatch";
}

bool CollectRenderPrims(const ::lightusd::Stage& stage,
                        const RenderExtractOptions& options,
                        RenderExtractResult* out) {
  if (!out) return false;
  *out = RenderExtractResult();
  if (options.collect_records || options.collect_categories) {
    const size_t estimated_records = stage.GetPrimCount();
    if (estimated_records > 0) {
      out->records.reserve(estimated_records);
    }
  }
  double identity[16];
  Identity(identity);
  for (const ::lightusd::UsdPrim& root : stage.GetRootPrims()) {
    CollectRec(root, options, identity, "default", std::string(),
               std::string(), out);
  }
  return true;
}

bool ReadPointInstancerData(const ::lightusd::UsdPrim& prim,
                            double time_code,
                            PointInstancerData* out,
                            bool compute_transforms) {
  if (!out) return false;
  *out = PointInstancerData();
  ::lightusd::UsdGeomPointInstancer pi(prim);
  if (!pi) {
    out->prim = prim;
    out->path = prim.IsValid() ? prim.GetPath().str() : std::string();
    out->validation_error = "not a PointInstancer";
    return false;
  }
  out->prim = prim;
  out->path = prim.GetPath().str();
  out->prototypes = pi.GetPrototypes();
  out->proto_indices = pi.GetProtoIndices(time_code);
  out->positions = pi.GetPositions(time_code);
  out->orientations = pi.GetOrientations(time_code);
  out->scales = pi.GetScales(time_code);
  out->velocities = pi.GetVelocities(time_code);
  out->angular_velocities = pi.GetAngularVelocities(time_code);
  out->ids = pi.GetIds(time_code);
  out->invisible_ids = pi.GetInvisibleIds(time_code);
  out->inactive_ids = pi.GetInactiveIds();
  if (compute_transforms) {
    out->transforms = pi.ComputeInstanceTransforms(time_code);
  }
  out->valid = pi.HasValidInstanceArrays(time_code, &out->validation_error);
  return true;
}

void GatherMeshPrims(const ::lightusd::UsdPrim& root,
                     std::vector<::lightusd::UsdPrim>* out) {
  if (!out || !root.IsActive()) return;

  // Keep traversal iterative: composed/programmatically-created stages can
  // contain thousands of nested Xforms, and recursion here would turn input
  // depth into native-stack exhaustion. Push children in reverse so the
  // resulting order remains the same as the recursive (ascending child index)
  // implementation.
  struct Entry {
    ::lightusd::UsdPrim prim;
    bool active = true;
  };
  std::vector<Entry> stack;
  stack.reserve(32);
  stack.push_back(Entry{root, true});
  while (!stack.empty()) {
    const Entry entry = std::move(stack.back());
    stack.pop_back();
    const ::lightusd::UsdPrim& prim = entry.prim;
    if (!entry.active) continue;
    if (prim.GetTypeName() == "Mesh") out->push_back(prim);

    const size_t child_count = prim.GetChildCount();
    for (size_t i = child_count; i > 0; --i) {
      const ::lightusd::UsdPrim child = prim.GetChildAt(i - 1);
      if (child.IsValid()) {
        // IsActive() walks every ancestor. The parent entry is already known
        // active, so one local metadata check preserves its result without
        // turning a deep chain into O(depth^2) work.
        stack.push_back(Entry{child, entry.active && child.GetMeta().active});
      }
    }
  }
}

void CollectPrototypePaths(const ::lightusd::Stage& stage,
                           std::unordered_set<std::string>* out) {
  if (!out) return;
  RenderExtractOptions options;
  options.stop_at_point_instancers = true;
  options.stop_at_native_instances = true;
  options.collect_records = false;
  options.collect_categories = false;
  RenderExtractResult result;
  if (!CollectRenderPrims(stage, options, &result)) return;
  out->insert(result.native_prototype_holders.begin(),
              result.native_prototype_holders.end());
}

bool MeshFloatAttribute::TupleIndex(size_t point, size_t corner, size_t face,
                                   size_t* index) const {
  if (!index || !components) return false;
  size_t slot = point;
  if (interpolation == "constant") slot = 0;
  else if (interpolation == "uniform") slot = face;
  else if (interpolation == "faceVarying") slot = corner;
  else if (interpolation != "vertex" && interpolation != "varying") return false;
  if (!indices.empty()) {
    if (slot >= indices.size() || indices[slot] < 0) return false;
    slot = static_cast<size_t>(indices[slot]);
  }
  if (slot >= values.size() / components) return false;
  *index = slot;
  return true;
}

// Evaluate standard linear blend skinning in mesh-local space. Binding
// relationships and influence primvars may be inherited from an ancestor.
bool EvaluateSkinnedPoints(const ::lightusd::Stage& stage,
                           const ::lightusd::UsdPrim& mesh, double time,
                           const ValueArrayRead<float>& points,
                           std::vector<float>* output, std::string* error,
                           std::vector<double>* point_normal_matrices) {
  namespace n = ::lightusd;
  output->clear();
  if (point_normal_matrices) point_normal_matrices->clear();
  auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };
  auto binding = [&](const char* name) {
    for (auto p = mesh; p.IsValid(); p = p.GetParent()) {
      const auto* targets = p.GetRelationship(name);
      if (targets && !targets->empty())
        return stage.GetPrimAtPath((*targets)[0]);
    }
    return n::UsdPrim();
  };
  auto skel = binding("skel:skeleton");
  if (!skel.IsValid()) return true;
  auto owner = [&](const char* name) {
    for (auto p = mesh; p.IsValid(); p = p.GetParent())
      if (p.GetPropertyValue(name)) return p;
    return n::UsdPrim();
  };
  const auto index_owner = owner("primvars:skel:jointIndices");
  const auto weight_owner = owner("primvars:skel:jointWeights");
  if (!index_owner.IsValid() || !weight_owner.IsValid())
    return fail("missing skin influences");
  auto method_owner = owner("primvars:skel:skinningMethod");
  if (method_owner.IsValid()) {
    const auto* method =
        method_owner.GetPropertyValue("primvars:skel:skinningMethod");
    const auto token = method->as_token();
    if (token && *token != "classicLinear")
      return fail("unsupported skinning method");
  }
  n::SkeletonData skeleton;
  if (!n::GetSkeletonData(stage, skel, &skeleton))
    return fail("invalid skeleton");
  const size_t count = skeleton.joints.size();
  if (!count || skeleton.bindTransforms.size() != count * 16 ||
      skeleton.restTransforms.size() != count * 16)
    return fail("invalid skeleton bind/rest transforms");
  std::vector<int> parents;
  std::string topology_error;
  if (!n::BuildSkelTopology(skeleton.joints, parents, &topology_error))
    return fail("invalid skeleton topology");
  auto inverse = [](const double* m, double* out) {
    double a[4][8] = {};
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        a[r][c] = m[r * 4 + c];
        a[r][c + 4] = (r == c);
      }
    for (int c = 0; c < 4; ++c) {
      int pivot = c;
      for (int r = c + 1; r < 4; ++r)
        if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
      if (!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c]) < 1e-15)
        return false;
      for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[c][k]);
      double divisor = a[c][c];
      for (int k = 0; k < 8; ++k) a[c][k] /= divisor;
      for (int r = 0; r < 4; ++r)
        if (r != c) {
          double factor = a[r][c];
          for (int k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
  };
  auto animation = binding("skel:animationSource");
  for (auto p = skel; !animation.IsValid() && p.IsValid(); p = p.GetParent()) {
    const auto* targets = p.GetRelationship("skel:animationSource");
    if (targets && !targets->empty())
      animation = stage.GetPrimAtPath((*targets)[0]);
  }
  n::SkelAnimationData sample;
  if (animation.IsValid() &&
      !n::GetSkelAnimationDataAtTime(stage, animation, &sample, time))
    return fail("invalid animation sample");
  std::unordered_map<std::string, size_t> animation_order;
  for (size_t i = 0; i < sample.joints.size(); ++i)
    animation_order[sample.joints[i]] = i;
  std::vector<double> current(count * 16), palette(count * 16);
  for (size_t j = 0; j < count; ++j) {
    double local[16];
    std::copy_n(skeleton.restTransforms.data() + 16 * j, 16, local);
    auto it = animation_order.find(skeleton.joints[j]);
    if (it != animation_order.end()) {
      const size_t k = it->second;
      // Partial TRS channels require decomposing rest transforms. Refuse them
      // explicitly until that path is implemented, rather than invent identity.
      if (sample.translations.size() < (k + 1) * 3 ||
          sample.rotations.size() < (k + 1) * 4 ||
          sample.scales.size() < (k + 1) * 3)
        return fail("partial animation TRS channels unsupported");
      const float* q = sample.rotations.data() + 4 * k;
      double w = q[0], x = q[1], y = q[2], z = q[3];
      double length = std::sqrt(w * w + x * x + y * y + z * z);
      if (!std::isfinite(length) || length < 1e-12)
        return fail("invalid animation quaternion");
      w /= length;
      x /= length;
      y /= length;
      z /= length;
      Identity(local);
      local[0] = 1 - 2 * (y * y + z * z);
      local[1] = 2 * (x * y + w * z);
      local[2] = 2 * (x * z - w * y);
      local[4] = 2 * (x * y - w * z);
      local[5] = 1 - 2 * (x * x + z * z);
      local[6] = 2 * (y * z + w * x);
      local[8] = 2 * (x * z + w * y);
      local[9] = 2 * (y * z - w * x);
      local[10] = 1 - 2 * (x * x + y * y);
      for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c)
          local[4 * r + c] *= sample.scales[3 * k + r];
        local[12 + r] = sample.translations[3 * k + r];
      }
    }
    if (parents[j] >= 0) {
      if (size_t(parents[j]) >= j) return fail("unordered skeleton topology");
      MulRowMajor(local, current.data() + 16 * size_t(parents[j]),
                  current.data() + 16 * j);
    } else
      std::copy_n(local, 16, current.data() + 16 * j);
    double invbind[16];
    if (!inverse(skeleton.bindTransforms.data() + 16 * j, invbind))
      return fail("singular bind transform");
    MulRowMajor(invbind, current.data() + 16 * j, palette.data() + 16 * j);
  }
  double mesh_world[16], skel_world[16], invmesh[16], to_mesh[16],
      geom_bind[16];
  Identity(geom_bind);
  if (!n::ComputeWorldTransform(stage, mesh, mesh_world, time) ||
      !n::ComputeWorldTransform(stage, skel, skel_world, time) ||
      !inverse(mesh_world, invmesh))
    return fail("invalid skin world transforms");
  MulRowMajor(skel_world, invmesh, to_mesh);
  auto geom_owner = owner("primvars:skel:geomBindTransform");
  if (geom_owner.IsValid()) {
    const auto* value =
        geom_owner.GetPropertyValue("primvars:skel:geomBindTransform");
    const double* matrix = value->as_matrix4d();
    if (!matrix) return fail("invalid geom bind transform");
    std::copy_n(matrix, 16, geom_bind);
  }
  std::vector<size_t> joint_order;
  auto joints_owner = owner("skel:joints");
  if (joints_owner.IsValid()) {
    const auto* names = joints_owner.GetPropertyValue("skel:joints");
    const auto tokens = names->as_token_array();
    if (!tokens) return fail("invalid binding joint order");
    std::unordered_map<std::string, size_t> order;
    for (size_t j = 0; j < count; ++j) order[skeleton.joints[j]] = j;
    for (const auto& name : *tokens) {
      auto found = order.find(std::string(name));
      if (found == order.end()) return fail("unknown binding joint");
      joint_order.push_back(found->second);
    }
  } else {
    for (size_t j = 0; j < count; ++j) joint_order.push_back(j);
  }
  std::vector<double> normal_palette;
  double geom_normal[16], mesh_normal[16];
  if (point_normal_matrices) {
    normal_palette.resize(count * 16);
    auto normal_matrix = [&](const double* matrix, double* normal) {
      double inv[16];
      if (!inverse(matrix, inv)) return false;
      Identity(normal);
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c) normal[4 * r + c] = inv[4 * c + r];
      return true;
    };
    if (!normal_matrix(geom_bind, geom_normal) ||
        !normal_matrix(to_mesh, mesh_normal))
      return fail("singular skin normal transform");
    for (size_t j = 0; j < count; ++j)
      if (!normal_matrix(palette.data() + 16 * j,
                         normal_palette.data() + 16 * j))
        return fail("singular joint normal transform");
    point_normal_matrices->resize(points.size() * 3);
  }
  ValueArrayRead<int32_t> indices, index_rows, weight_rows;
  ValueArrayRead<float> weights;
  if (!ReadIntArray(index_owner, "primvars:skel:jointIndices", time,
                    &indices) ||
      !ReadFloatArray(weight_owner, "primvars:skel:jointWeights", time,
                      &weights))
    return fail("invalid skin influence arrays");
  const auto* index_spec = index_owner.GetPrimSpec();
  const auto* weight_spec = weight_owner.GetPrimSpec();
  const auto* meta =
      index_spec ? index_spec->property_meta("primvars:skel:jointIndices")
                 : nullptr;
  const auto* weight_meta =
      weight_spec ? weight_spec->property_meta("primvars:skel:jointWeights")
                  : nullptr;
  if ((meta && meta->elementSize <= 0) ||
      (weight_meta && weight_meta->elementSize <= 0))
    return fail("invalid skin element size");
  const size_t influences = meta ? size_t(meta->elementSize) : 1;
  if (weight_meta && size_t(weight_meta->elementSize) != influences)
    return fail("skin element size mismatch");
  const bool constant = meta && meta->interpolation == "constant";
  const bool weight_constant =
      weight_meta && weight_meta->interpolation == "constant";
  for (const auto* m : {meta, weight_meta})
    if (m && !m->interpolation.empty() && m->interpolation != "constant" &&
        m->interpolation != "vertex" && m->interpolation != "varying")
      return fail("unsupported skin interpolation");
  const bool indexed = index_owner.GetPropertyValue(
                           "primvars:skel:jointIndices:indices") != nullptr;
  const bool weights_indexed =
      weight_owner.GetPropertyValue("primvars:skel:jointWeights:indices") !=
      nullptr;
  if (indexed &&
      !ReadIntArray(index_owner, "primvars:skel:jointIndices:indices", time,
                    &index_rows))
    return fail("invalid indexed skin influences");
  if (weights_indexed &&
      !ReadIntArray(weight_owner, "primvars:skel:jointWeights:indices", time,
                    &weight_rows))
    return fail("invalid indexed skin weights");
  if (points.size() % 3 || indices.size() % influences ||
      weights.size() % influences ||
      (indexed ? index_rows.size() : indices.size() / influences) !=
          (constant ? 1 : points.size() / 3) ||
      (weights_indexed ? weight_rows.size() : weights.size() / influences) !=
          (weight_constant ? 1 : points.size() / 3))
    return fail("skin influence count mismatch");
  output->resize(points.size());
  for (size_t p = 0; p < points.size() / 3; ++p) {
    double blended[16] = {}, blended_normal[16] = {};
    double total = 0;
    const size_t index_slot = constant ? 0 : p,
                 weight_slot = weight_constant ? 0 : p;
    const int64_t index_row =
        indexed ? int64_t(index_rows[index_slot]) : int64_t(index_slot);
    const int64_t weight_row = weights_indexed
                                   ? int64_t(weight_rows[weight_slot])
                                   : int64_t(weight_slot);
    if (index_row < 0 || uint64_t(index_row) >= indices.size() / influences ||
        weight_row < 0 || uint64_t(weight_row) >= weights.size() / influences)
      return fail("skin indirection out of range");
    for (size_t k = 0; k < influences; ++k) {
      const int32_t index = indices[size_t(index_row) * influences + k];
      double weight = weights[size_t(weight_row) * influences + k];
      if (index < 0 || size_t(index) >= joint_order.size() ||
          !std::isfinite(weight) || weight < 0)
        return fail("invalid skin influence");
      total += weight;
      const double* matrix = palette.data() + 16 * joint_order[size_t(index)];
      for (size_t c = 0; c < 16; ++c) blended[c] += weight * matrix[c];
      if (point_normal_matrices)
        for (size_t c = 0; c < 16; ++c)
          blended_normal[c] +=
              weight * normal_palette[16 * joint_order[size_t(index)] + c];
    }
    if (!std::isfinite(total) || total < 1e-12)
      return fail("zero skin influence weight");
    for (double& v : blended) v /= total;
    if (point_normal_matrices) {
      for (double& v : blended_normal) v /= total;
      double bound_normal[16], final_normal[16];
      MulRowMajor(geom_normal, blended_normal, bound_normal);
      MulRowMajor(bound_normal, mesh_normal, final_normal);
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c)
          (*point_normal_matrices)[p * 9 + r * 3 + c] = final_normal[4 * r + c];
    }
    double bound[16], final[16];
    MulRowMajor(geom_bind, blended, bound);
    MulRowMajor(bound, to_mesh, final);
    for (size_t c = 0; c < 3; ++c) {
      double v = final[12 + c];
      for (size_t r = 0; r < 3; ++r) v += points[3 * p + r] * final[4 * r + c];
      if (!std::isfinite(v)) return fail("nonfinite skinned point");
      (*output)[3 * p + c] = float(v);
    }
  }
  return true;
}

bool MeshUsesAuthoredNormals(const ::lightusd::UsdPrim& prim) {
  if (prim.GetTypeName() != "Mesh") return true;
  const auto* scheme = prim.GetPropertyValue("subdivisionScheme");
  // UsdGeom.Mesh's schema fallback is catmullClark, not none.
  return scheme && scheme->as_token() && *scheme->as_token() == "none";
}

bool MeshUsesGeneratedSmoothNormals(const ::lightusd::UsdPrim& prim) {
  if (prim.GetTypeName() != "Mesh") return false;
  const auto* scheme = prim.GetPropertyValue("subdivisionScheme");
  if (!scheme || !scheme->as_token()) return true;
  return *scheme->as_token() != "none" && *scheme->as_token() != "bilinear";
}

bool ComputeMeshSmoothNormals(const ValueArrayRead<float>& points,
                              const ValueArrayRead<int32_t>& counts,
                              const ValueArrayRead<int32_t>& indices,
                              bool left_handed, std::vector<float>* normals) {
  if (!normals) return false;
  normals->clear();
  if (points.size() % 3 || counts.empty()) return false;
  size_t corners = 0;
  for (int32_t count : counts) {
    if (count < 0 || size_t(count) > indices.size() - corners) return false;
    corners += size_t(count);
  }
  if (corners != indices.size()) return false;
  for (int32_t index : indices)
    if (index < 0 || size_t(index) >= points.size() / 3) return false;
  for (float value : points)
    if (!std::isfinite(value)) return false;
  normals->assign(points.size(), 0.0f);
  size_t first = 0;
  const float sign = left_handed ? -1.0f : 1.0f;
  for (int32_t count : counts) {
    const size_t n = size_t(count);
    if (n >= 3)
      for (size_t corner = 0; corner < n; ++corner) {
        const size_t current = size_t(indices[first + corner]) * 3;
        const size_t previous =
            size_t(indices[first + (corner + n - 1) % n]) * 3;
        const size_t next = size_t(indices[first + (corner + 1) % n]) * 3;
        float a[3], b[3];
        for (size_t k = 0; k < 3; ++k) {
          a[k] = points[next + k] - points[current + k];
          b[k] = points[previous + k] - points[current + k];
        }
        for (size_t k = 0; k < 3; ++k)
          (*normals)[current + k] += sign * (a[(k + 1) % 3] * b[(k + 2) % 3] -
                                             a[(k + 2) % 3] * b[(k + 1) % 3]);
      }
    first += n;
  }
  for (size_t i = 0; i < normals->size(); i += 3) {
    const double length =
        std::sqrt(double((*normals)[i]) * (*normals)[i] +
                  double((*normals)[i + 1]) * (*normals)[i + 1] +
                  double((*normals)[i + 2]) * (*normals)[i + 2]);
    if (!std::isfinite(length)) {
      normals->clear();
      return false;
    }
    if (length > 1e-20)
      for (size_t k = 0; k < 3; ++k)
        (*normals)[i + k] = float((*normals)[i + k] / length);
  }
  return true;
}

bool ReadMeshFloatAttribute(const ::lightusd::UsdPrim& prim,
                            const std::string& name,
                            const ::lightusd::EvalOptions& options,
                            size_t components, size_t point_count,
                            size_t corner_count, MeshFloatAttribute* out,
                            size_t face_count) {
  if (!out || !components) return false;
  out->components = components;
  out->interpolation.clear();
  out->indices.view = {};
  if (!ReadFloatArray(prim, name.c_str(), options, &out->values) ||
      out->values.empty() || out->values.size() % components != 0)
    return false;
  ReadIntArray(prim, (name + ":indices").c_str(), options, &out->indices);
  const auto* spec = prim.GetPrimSpec();
  const auto* meta = spec ? spec->property_meta(name) : nullptr;
  if (meta && !meta->interpolation.empty()) {
    out->interpolation = meta->interpolation;
  } else {
    const size_t count = out->indices.empty() ? out->values.size() / components
                                              : out->indices.size();
    out->interpolation = count == point_count    ? "vertex"
                         : count == corner_count ? "faceVarying"
                         : face_count > 1 && count == face_count ? "uniform"
                         : count == 1            ? "constant"
                                                 : "";
  }
  return !out->interpolation.empty();
}

bool ReadIntArray(const ::lightusd::UsdPrim& prim, const char* name,
                  const ::lightusd::EvalOptions& options,
                  ValueArrayRead<int32_t>* out) {
  if (!out) return false;
  out->view = {};
  ::lightusd::AttributeEval eval(nullptr);
  auto result = eval.EvalWith(prim, name, options);
  if (!result.success) return false;
  out->scratch.materialized = std::move(result.value);
  return ::lightusd::GetIntArrayView(out->scratch.materialized,
                                           &out->scratch, &out->view);
}

bool ReadFloatArray(const ::lightusd::UsdPrim& prim, const char* name,
                    const ::lightusd::EvalOptions& options,
                    ValueArrayRead<float>* out) {
  if (!out) return false;
  out->view = {};
  ::lightusd::AttributeEval eval(nullptr);
  auto result = eval.EvalWith(prim, name, options);
  if (!result.success) return false;
  out->scratch.materialized = std::move(result.value);
  return ::lightusd::GetFloatArrayView(
      out->scratch.materialized, &out->scratch, &out->view);
}

bool ReadFloatArray(const ::lightusd::UsdPrim& prim, const char* name,
                  double time, ValueArrayRead<float>* out) {
  const auto name_id = ::lightusd::GetPropNameTable().find(name);
  if (!name_id.is_valid()) {
    return false;
  }
  return ReadFloatArray(prim, name_id, time, out);
}

bool ReadFloatArray(const ::lightusd::UsdPrim& prim,
                   const ::lightusd::PropNameId& name_id,
                   double time, ValueArrayRead<float>* out) {
  if (!out) return false;
  const ::lightusd::Value* v =
      ValueAtOrDefault(prim, name_id, time, &out->scratch.materialized);
  if (!v) return false;
  return ::lightusd::GetFloatArrayView(*v, &out->scratch, &out->view);
}

bool ReadInt64Array(const ::lightusd::UsdPrim& prim, const char* name,
                    double time, ValueArrayRead<int64_t>* out) {
  const auto name_id = ::lightusd::GetPropNameTable().find(name);
  if (!name_id.is_valid()) {
    return false;
  }
  return ReadInt64Array(prim, name_id, time, out);
}

bool ReadIntArray(const ::lightusd::UsdPrim& prim, const char* name,
                  double time, ValueArrayRead<int32_t>* out) {
  const auto name_id = ::lightusd::GetPropNameTable().find(name);
  if (!name_id.is_valid()) {
    return false;
  }
  return ReadIntArray(prim, name_id, time, out);
}

bool ReadIntArray(const ::lightusd::UsdPrim& prim,
                  const ::lightusd::PropNameId& name_id,
                  double time, ValueArrayRead<int32_t>* out) {
  if (!out) return false;
  const ::lightusd::Value* v =
      ValueAtOrDefault(prim, name_id, time, &out->scratch.materialized);
  if (!v) return false;
  return ::lightusd::GetIntArrayView(*v, &out->scratch, &out->view);
}

bool ReadUIntArray(const ::lightusd::UsdPrim& prim, const char* name,
                   double time, ValueArrayRead<uint32_t>* out) {
  const auto name_id = ::lightusd::GetPropNameTable().find(name);
  if (!name_id.is_valid()) {
    return false;
  }
  return ReadUIntArray(prim, name_id, time, out);
}

bool ReadInt64Array(const ::lightusd::UsdPrim& prim,
                   const ::lightusd::PropNameId& name_id,
                   double time, ValueArrayRead<int64_t>* out) {
  if (!out) return false;
  const ::lightusd::Value* v =
      ValueAtOrDefault(prim, name_id, time, &out->scratch.materialized);
  if (!v) return false;
  return ::lightusd::GetInt64ArrayView(*v, &out->scratch, &out->view);
}

bool ReadUInt64Array(const ::lightusd::UsdPrim& prim, const char* name,
                     double time, ValueArrayRead<uint64_t>* out) {
  const auto name_id = ::lightusd::GetPropNameTable().find(name);
  if (!name_id.is_valid()) {
    return false;
  }
  return ReadUInt64Array(prim, name_id, time, out);
}

bool ReadUIntArray(const ::lightusd::UsdPrim& prim,
                  const ::lightusd::PropNameId& name_id, double time,
                  ValueArrayRead<uint32_t>* out) {
  if (!out) return false;
  const ::lightusd::Value* v =
      ValueAtOrDefault(prim, name_id, time, &out->scratch.materialized);
  if (!v) return false;
  return ::lightusd::GetUIntArrayView(*v, &out->scratch, &out->view);
}

bool ReadUInt64Array(const ::lightusd::UsdPrim& prim,
                    const ::lightusd::PropNameId& name_id, double time,
                    ValueArrayRead<uint64_t>* out) {
  if (!out) return false;
  const ::lightusd::Value* v =
      ValueAtOrDefault(prim, name_id, time, &out->scratch.materialized);
  if (!v) return false;
  return ::lightusd::GetUInt64ArrayView(*v, &out->scratch, &out->view);
}

std::vector<float> ReadFloatArrayCopy(const ::lightusd::UsdPrim& prim,
                                      const char* name, double time) {
  ValueArrayRead<float> r;
  if (!ReadFloatArray(prim, name, time, &r)) return {};
  return std::vector<float>(r.begin(), r.end());
}

std::vector<float> ReadFloatArrayCopy(const ::lightusd::UsdPrim& prim,
                                      const ::lightusd::PropNameId& name,
                                      double time) {
  ValueArrayRead<float> r;
  if (!ReadFloatArray(prim, name, time, &r)) return {};
  return std::vector<float>(r.begin(), r.end());
}

std::vector<int32_t> ReadIntArrayCopy(const ::lightusd::UsdPrim& prim,
                                      const char* name, double time) {
  ValueArrayRead<int32_t> r;
  if (!ReadIntArray(prim, name, time, &r)) return {};
  return std::vector<int32_t>(r.begin(), r.end());
}

std::vector<int32_t> ReadIntArrayCopy(const ::lightusd::UsdPrim& prim,
                                      const ::lightusd::PropNameId& name,
                                      double time) {
  ValueArrayRead<int32_t> r;
  if (!ReadIntArray(prim, name, time, &r)) return {};
  return std::vector<int32_t>(r.begin(), r.end());
}

std::vector<int64_t> ReadInt64ArrayCopy(const ::lightusd::UsdPrim& prim,
                                        const char* name, double time) {
  ValueArrayRead<int64_t> r;
  if (!ReadInt64Array(prim, name, time, &r)) return {};
  return std::vector<int64_t>(r.begin(), r.end());
}

std::vector<int64_t> ReadInt64ArrayCopy(const ::lightusd::UsdPrim& prim,
                                        const ::lightusd::PropNameId& name,
                                        double time) {
  ValueArrayRead<int64_t> r;
  if (!ReadInt64Array(prim, name, time, &r)) return {};
  return std::vector<int64_t>(r.begin(), r.end());
}

std::vector<uint32_t> ReadUIntArrayCopy(const ::lightusd::UsdPrim& prim,
                                        const char* name, double time) {
  ValueArrayRead<uint32_t> r;
  if (!ReadUIntArray(prim, name, time, &r)) return {};
  return std::vector<uint32_t>(r.begin(), r.end());
}

std::vector<uint32_t> ReadUIntArrayCopy(const ::lightusd::UsdPrim& prim,
                                        const ::lightusd::PropNameId& name,
                                        double time) {
  ValueArrayRead<uint32_t> r;
  if (!ReadUIntArray(prim, name, time, &r)) return {};
  return std::vector<uint32_t>(r.begin(), r.end());
}

std::vector<uint64_t> ReadUInt64ArrayCopy(const ::lightusd::UsdPrim& prim,
                                          const char* name, double time) {
  ValueArrayRead<uint64_t> r;
  if (!ReadUInt64Array(prim, name, time, &r)) return {};
  return std::vector<uint64_t>(r.begin(), r.end());
}

std::vector<uint64_t> ReadUInt64ArrayCopy(const ::lightusd::UsdPrim& prim,
                                          const ::lightusd::PropNameId& name,
                                          double time) {
  ValueArrayRead<uint64_t> r;
  if (!ReadUInt64Array(prim, name, time, &r)) return {};
  return std::vector<uint64_t>(r.begin(), r.end());
}

}  // namespace tydra
}  // namespace lightusd
