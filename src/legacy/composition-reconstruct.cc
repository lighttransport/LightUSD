// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
//
// PrimSpec→Prim reconstruction, Stage building, variant extraction/selection.
// Split from composition.cc.

#include "composition.hh"

#include <atomic>
#include <functional>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif

#include "common-macros.inc"
#include "layer.hh"
#include "prim-pprint.hh"
#include "prim-reconstruct.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "stage.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdPhysics.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "mjcPhysics.hh"
#include "newtonPhysics.hh"
#include "value-types.hh"

#define PushError(s) \
  if (err) {         \
    (*err) += s;     \
  }

#define PushWarn(s) \
  if (warn) {       \
    (*warn) += s;   \
  }

namespace lightusd {

namespace prim {

// template specialization forward decls.
// implimentations will be located in prim-reconstruct.cc
#define RECONSTRUCT_PRIM_DECL(__ty)                                   \
  template <>                                                         \
  bool ReconstructPrim<__ty>(PrimSpec &, __ty *, std::string *, \
                             std::string *, const PrimReconstructOptions &)

RECONSTRUCT_PRIM_DECL(Xform);
RECONSTRUCT_PRIM_DECL(Model);
RECONSTRUCT_PRIM_DECL(Scope);
RECONSTRUCT_PRIM_DECL(RenderSettings);
RECONSTRUCT_PRIM_DECL(RenderProduct);
RECONSTRUCT_PRIM_DECL(RenderVar);
RECONSTRUCT_PRIM_DECL(GenerativeProcedural);
RECONSTRUCT_PRIM_DECL(GeomPoints);
RECONSTRUCT_PRIM_DECL(GeomMesh);
RECONSTRUCT_PRIM_DECL(Volume);
RECONSTRUCT_PRIM_DECL(VolumeFieldBase);
RECONSTRUCT_PRIM_DECL(FieldBase);
RECONSTRUCT_PRIM_DECL(VolumeFieldAsset);
RECONSTRUCT_PRIM_DECL(FieldAsset);
RECONSTRUCT_PRIM_DECL(OpenVDBAsset);
RECONSTRUCT_PRIM_DECL(Field3DAsset);
RECONSTRUCT_PRIM_DECL(GeomCapsule);
RECONSTRUCT_PRIM_DECL(GeomCube);
RECONSTRUCT_PRIM_DECL(GeomCone);
RECONSTRUCT_PRIM_DECL(GeomCylinder);
RECONSTRUCT_PRIM_DECL(GeomSphere);
RECONSTRUCT_PRIM_DECL(GeomBasisCurves);
RECONSTRUCT_PRIM_DECL(GeomPointInstancer);
RECONSTRUCT_PRIM_DECL(GeomCamera);
RECONSTRUCT_PRIM_DECL(GeomSubset);
RECONSTRUCT_PRIM_DECL(SphereLight);
RECONSTRUCT_PRIM_DECL(DomeLight);
RECONSTRUCT_PRIM_DECL(DiskLight);
RECONSTRUCT_PRIM_DECL(DistantLight);
RECONSTRUCT_PRIM_DECL(RectLight);
RECONSTRUCT_PRIM_DECL(CylinderLight);
RECONSTRUCT_PRIM_DECL(GeometryLight);
RECONSTRUCT_PRIM_DECL(PortalLight);
RECONSTRUCT_PRIM_DECL(LightFilter);
RECONSTRUCT_PRIM_DECL(PluginLightFilter);
RECONSTRUCT_PRIM_DECL(SkelRoot);
RECONSTRUCT_PRIM_DECL(SkelAnimation);
RECONSTRUCT_PRIM_DECL(Skeleton);
RECONSTRUCT_PRIM_DECL(BlendShape);
RECONSTRUCT_PRIM_DECL(Material);
RECONSTRUCT_PRIM_DECL(Shader);
RECONSTRUCT_PRIM_DECL(NodeGraph);
RECONSTRUCT_PRIM_DECL(PhysicsScene);
RECONSTRUCT_PRIM_DECL(PhysicsJoint);
RECONSTRUCT_PRIM_DECL(PhysicsRevoluteJoint);
RECONSTRUCT_PRIM_DECL(PhysicsPrismaticJoint);
RECONSTRUCT_PRIM_DECL(PhysicsSphericalJoint);
RECONSTRUCT_PRIM_DECL(PhysicsFixedJoint);
RECONSTRUCT_PRIM_DECL(PhysicsDistanceJoint);
RECONSTRUCT_PRIM_DECL(PhysicsCollisionGroup);
RECONSTRUCT_PRIM_DECL(MjcActuator);
RECONSTRUCT_PRIM_DECL(MjcTendon);
RECONSTRUCT_PRIM_DECL(MjcKeyframe);
RECONSTRUCT_PRIM_DECL(MjcSensor);
RECONSTRUCT_PRIM_DECL(NewtonActuator);

#undef RECONSTRUCT_PRIM_DECL

}  // namespace prim

namespace detail {

static nonstd::optional<Prim> ReconstructPrimFromPrimSpec(
    PrimSpec &primspec, std::string *warn, std::string *err) {
  (void)warn;

  // - propertyNames()
  // - primChildrenNames()


/* The PrimSpec is consumed (callers hand LayerToStage an rvalue Layer), so
   metas are moved out and the typed prim is moved into the boxed Value —
   this path previously copied the fully-built typed prim twice (into the
   Value, then into the Prim). */
#define RECONSTRUCT_PRIM(__primty)                                       \
  if (primspec.typeName() == value::TypeTraits<__primty>::type_name()) { \
    __primty typed_prim;                                                 \
    if (!prim::ReconstructPrim(primspec, &typed_prim, warn, err)) {      \
      PUSH_ERROR("Failed to reconstruct Prim from PrimSpec "             \
                 << primspec.typeName()                                  \
                 << " elementName: " << primspec.name());                \
      return nonstd::nullopt;                                            \
    }                                                                    \
    typed_prim.meta = std::move(primspec.metas());                       \
    typed_prim.name = primspec.name();                                   \
    typed_prim.spec = primspec.specifier();                              \
    /*typed_prim.propertyNames() = properties; */                        \
    /*typed_prim.primChildrenNames() = primChildren;*/                   \
    Prim prim(primspec.name(), value::Value(std::move(typed_prim)));     \
    prim.prim_type_name() = primspec.typeName();                         \
    /* also add primChildren to Prim */                                  \
    /* prim.metas().primChildren = primChildren; */                      \
    return std::move(prim);                                              \
  } else

  if (primspec.typeName().empty() || primspec.typeName() == "Model") {
    // Code is mostly identical to RECONSTRUCT_PRIM.
    // Difference is store primTypeName to Model class itself.
    Model typed_prim;
    if (!prim::ReconstructPrim(primspec, &typed_prim, warn, err)) {
      PUSH_ERROR("Failed to reconstruct Model");
      return nonstd::nullopt;
    }
    typed_prim.meta = std::move(primspec.metas());
    typed_prim.name = primspec.name();
    typed_prim.prim_type_name = primspec.typeName();
    typed_prim.spec = primspec.specifier();
    // typed_prim.propertyNames() = properties;
    // typed_prim.primChildrenNames() = primChildren;
    Prim prim(primspec.name(), value::Value(std::move(typed_prim)));
    prim.prim_type_name() = primspec.typeName();
    /* also add primChildren to Prim */
    // prim.metas().primChildren = primChildren;
    return std::move(prim);
  } else

    RECONSTRUCT_PRIM(Xform)
  RECONSTRUCT_PRIM(Model)
  RECONSTRUCT_PRIM(Scope)
  RECONSTRUCT_PRIM(RenderSettings)
  RECONSTRUCT_PRIM(RenderProduct)
  RECONSTRUCT_PRIM(RenderVar)
  RECONSTRUCT_PRIM(GenerativeProcedural)
  RECONSTRUCT_PRIM(GeomMesh)
  RECONSTRUCT_PRIM(Volume)
  RECONSTRUCT_PRIM(VolumeFieldBase)
  RECONSTRUCT_PRIM(FieldBase)
  RECONSTRUCT_PRIM(VolumeFieldAsset)
  RECONSTRUCT_PRIM(FieldAsset)
  RECONSTRUCT_PRIM(OpenVDBAsset)
  RECONSTRUCT_PRIM(Field3DAsset)
  RECONSTRUCT_PRIM(GeomPoints)
  RECONSTRUCT_PRIM(GeomCylinder)
  RECONSTRUCT_PRIM(GeomCube)
  RECONSTRUCT_PRIM(GeomCone)
  RECONSTRUCT_PRIM(GeomSphere)
  RECONSTRUCT_PRIM(GeomCapsule)
  RECONSTRUCT_PRIM(GeomBasisCurves)
  RECONSTRUCT_PRIM(GeomPointInstancer)
  RECONSTRUCT_PRIM(GeomCamera)
  RECONSTRUCT_PRIM(GeomSubset)
  RECONSTRUCT_PRIM(SphereLight)
  RECONSTRUCT_PRIM(DomeLight)
  RECONSTRUCT_PRIM(CylinderLight)
  RECONSTRUCT_PRIM(DiskLight)
  RECONSTRUCT_PRIM(DistantLight)
  RECONSTRUCT_PRIM(RectLight)
  RECONSTRUCT_PRIM(GeometryLight)
  RECONSTRUCT_PRIM(PortalLight)
  RECONSTRUCT_PRIM(LightFilter)
  RECONSTRUCT_PRIM(PluginLightFilter)
  RECONSTRUCT_PRIM(SkelRoot)
  RECONSTRUCT_PRIM(Skeleton)
  RECONSTRUCT_PRIM(SkelAnimation)
  RECONSTRUCT_PRIM(BlendShape)
  RECONSTRUCT_PRIM(Shader)
  RECONSTRUCT_PRIM(NodeGraph)
  RECONSTRUCT_PRIM(PhysicsScene)
  RECONSTRUCT_PRIM(PhysicsJoint)
  RECONSTRUCT_PRIM(PhysicsRevoluteJoint)
  RECONSTRUCT_PRIM(PhysicsPrismaticJoint)
  RECONSTRUCT_PRIM(PhysicsSphericalJoint)
  RECONSTRUCT_PRIM(PhysicsFixedJoint)
  RECONSTRUCT_PRIM(PhysicsDistanceJoint)
  RECONSTRUCT_PRIM(PhysicsCollisionGroup)
  RECONSTRUCT_PRIM(MjcActuator)
  RECONSTRUCT_PRIM(MjcTendon)
  RECONSTRUCT_PRIM(MjcKeyframe)
  RECONSTRUCT_PRIM(MjcSensor)
  RECONSTRUCT_PRIM(NewtonActuator)
  RECONSTRUCT_PRIM(Material) {
    PUSH_WARN("TODO or unsupported prim type: " << primspec.typeName());
    return nonstd::nullopt;
  }

#undef RECONSTRUCT_PRIM
}

static nonstd::optional<Prim> ReconstructPrimFromPrimSpecRec(
    PrimSpec &primspec, std::string *warn, std::string *err,
    uint32_t depth = 0) {

  // This recurses once per namespace level (over children), so the cap must be
  // a stack-safe recursion depth, NOT the 1M iteration limit — a crafted deep
  // PrimSpec tree at 1M would overflow the native stack. Real USD namespace
  // depth never approaches this.
  constexpr uint32_t kMaxReconstructDepth = 1024;
  if (depth > kMaxReconstructDepth) {
    if (err) {
      (*err) += "ReconstructPrimFromPrimSpecRec: recursion too deep.\n";
    }
    return nonstd::nullopt;
  }

  auto pprim = ReconstructPrimFromPrimSpec(primspec, warn, err);

  if (pprim) {
    for (size_t i = 0; i < primspec.children().size(); i++) {
      if (auto pv = ReconstructPrimFromPrimSpecRec(primspec.children()[i], warn, err, depth + 1)) {
        pprim.value().children().emplace_back(std::move(pv.value()));
      }
    }
  }

  return pprim;
}

}  // namespace detail

bool LayerToStage(Layer &&layer, Stage *stage_out, std::string *warn,
                  std::string *err) {
  if (!stage_out) {
    if (err) {
      (*err) += "`stage_ptr` is nullptr.";
    }
    return false;
  }

  Stage stage;

  stage.metas() = layer.metas();

#if defined(LIGHTUSD_ENABLE_THREAD)
  // Large layers: reconstruct prim subtrees on worker threads, assembled in
  // DFS order (unit's own prim, then its child units in child order) — the
  // Prim tree, warning text and error text come out identical to the serial
  // walk. Children of a failed prim are dropped without merging their
  // diagnostics, matching the serial "skip subtree on failure" behavior.
  {
    size_t approx_prims = 0;
    for (auto &kv : layer.primspecs()) {
      approx_prims += 1;
      for (auto &c1 : kv.second.children()) {
        approx_prims += 1 + c1.children().size();
      }
    }
    if ((std::thread::hardware_concurrency() > 1) && (approx_prims > 64)) {
      struct RecUnit {
        PrimSpec* ps = nullptr;
        bool whole_subtree = true;
        uint32_t depth = 0;
        nonstd::optional<Prim> prim;
        std::vector<size_t> children;  // unit indices, in child order
        std::string unit_warn;
        std::string unit_err;
      };
      // Shells above this depth fan out single-root scenes; whole subtrees
      // below become the parallel jobs.
      constexpr uint32_t kShellDepth = 3;

      std::vector<RecUnit> units;
      units.reserve(1024);
      std::vector<size_t> roots;
      std::function<size_t(PrimSpec&, uint32_t)> plan =
          [&](PrimSpec& ps, uint32_t depth) -> size_t {
        const size_t idx = units.size();
        units.push_back(RecUnit());
        RecUnit& u = units.back();
        u.ps = &ps;
        u.depth = depth;
        if (depth < kShellDepth && !ps.children().empty()) {
          u.whole_subtree = false;
          std::vector<size_t> child_ids;
          child_ids.reserve(ps.children().size());
          for (auto& child : ps.children()) {
            child_ids.push_back(plan(child, depth + 1));
          }
          units[idx].children = std::move(child_ids);
        }
        return idx;
      };
      for (auto& primspec : layer.primspecs()) {
        roots.push_back(plan(primspec.second, 0));
      }

      {
        const uint32_t hw = std::thread::hardware_concurrency();
        const uint32_t nworkers = static_cast<uint32_t>(
            (std::min)(size_t(hw > 0 ? hw : 4), units.size()));
        std::atomic<size_t> next_unit{0};
        auto work = [&]() {
          for (;;) {
            const size_t i = next_unit.fetch_add(1);
            if (i >= units.size()) break;
            RecUnit& u = units[i];
            if (u.whole_subtree) {
              u.prim = detail::ReconstructPrimFromPrimSpecRec(
                  *u.ps, &u.unit_warn, &u.unit_err, u.depth);
            } else {
              u.prim = detail::ReconstructPrimFromPrimSpec(*u.ps, &u.unit_warn,
                                                           &u.unit_err);
            }
          }
        };
        std::vector<std::thread> workers;
        workers.reserve(nworkers);
        for (uint32_t t = 1; t < nworkers; t++) {
          workers.emplace_back(work);
        }
        work();
        for (auto& th : workers) th.join();
      }

      std::function<nonstd::optional<Prim>(size_t)> assemble =
          [&](size_t idx) -> nonstd::optional<Prim> {
        RecUnit& u = units[idx];
        if (warn && !u.unit_warn.empty()) (*warn) += u.unit_warn;
        if (err && !u.unit_err.empty()) (*err) += u.unit_err;
        if (!u.prim) {
          return nonstd::nullopt;
        }
        Prim p = std::move(u.prim.value());
        for (size_t c : u.children) {
          if (auto cv = assemble(c)) {
            p.children().emplace_back(std::move(cv.value()));
          }
        }
        return nonstd::optional<Prim>(std::move(p));
      };
      for (size_t r : roots) {
        if (auto pv = assemble(r)) {
          stage.add_root_prim(std::move(pv.value()));
        }
      }

      (*stage_out) = std::move(stage);
      return true;
    }
  }
#endif  // LIGHTUSD_ENABLE_THREAD

  // TODO: primChildren metadatum
  for (auto &primspec : layer.primspecs()) {
    if (auto pv =
            detail::ReconstructPrimFromPrimSpecRec(primspec.second, warn, err)) {
      stage.add_root_prim(std::move(pv.value()));
    }
  }

  (*stage_out) = std::move(stage);

  return true;
}

namespace detail {

// In-place conversion helper: Move PrimSpec data to Prim and free source memory
static std::unique_ptr<Prim> ReconstructPrimFromPrimSpecInPlace(
    std::unique_ptr<PrimSpec> primspec, std::string *warn, std::string *err,
    int depth = 0) {

  if (!primspec) {
    if (err) {
      (*err) += "PrimSpec is null";
    }
    return nullptr;
  }

  if (depth > 4096) {
    if (err) {
      (*err) += "PrimSpec tree too deep (max 4096).";
    }
    return nullptr;
  }

  // First reconstruct the prim normally
  auto prim_opt = ReconstructPrimFromPrimSpec(*primspec, warn, err);

  if (!prim_opt) {
    return nullptr;
  }

  auto result = std::make_unique<Prim>(std::move(prim_opt.value()));

  // Now we can clear the primspec data to free memory
  // The data has been copied to the Prim, so we can safely clear it

  // Clear properties (these can be large)
  primspec->props().clear();

  // Clear metadata
  primspec->metas() = PrimMeta();

  // Clear variant sets
  primspec->variantSets().clear();

  // Process children recursively
  for (auto& child : primspec->children()) {
    auto child_ptr = std::make_unique<PrimSpec>(std::move(child));
    if (auto child_prim = ReconstructPrimFromPrimSpecInPlace(std::move(child_ptr), warn, err, depth + 1)) {
      result->children().emplace_back(std::move(*child_prim));
    }
  }

  // Clear children vector
  primspec->children().clear();
  primspec->children().shrink_to_fit();

  return result;
}

} // namespace detail

bool LayerToStageInPlace(std::unique_ptr<Layer> layer, Stage *stage_out,
                         std::string *warn, std::string *err) {
  if (!stage_out) {
    if (err) {
      (*err) += "`stage_out` is nullptr.";
    }
    return false;
  }

  if (!layer) {
    if (err) {
      (*err) += "`layer` is nullptr.";
    }
    return false;
  }

  Stage stage;

  // Move metadata (cheap operation)
  stage.metas() = std::move(layer->metas());

  // Convert primspecs in-place
  // We need to iterate carefully since we're modifying the map
  auto& primspecs = layer->primspecs();
  std::vector<std::string> paths_to_process;

  for (const auto& item : primspecs) {
    paths_to_process.push_back(item.first);
  }

  for (const auto& path : paths_to_process) {
    auto it = primspecs.find(path);
    if (it != primspecs.end()) {
      // Extract the PrimSpec from the map
      auto primspec_ptr = std::make_unique<PrimSpec>(std::move(it->second));

      // Remove from map immediately to free memory
      primspecs.erase(it);

      // Convert to Prim in-place
      if (auto pv = detail::ReconstructPrimFromPrimSpecInPlace(std::move(primspec_ptr), warn, err)) {
        stage.add_root_prim(std::move(*pv));
      }
    }
  }

  // Clear the layer completely
  layer->primspecs().clear();
  layer.reset();  // Release the Layer object itself

  (*stage_out) = std::move(stage);

  return true;
}

bool PrimSpecToPrimInPlace(std::unique_ptr<PrimSpec> primspec, Prim *prim_out,
                           std::string *warn, std::string *err) {
  if (!prim_out) {
    if (err) {
      (*err) += "`prim_out` is nullptr.";
    }
    return false;
  }

  if (!primspec) {
    if (err) {
      (*err) += "`primspec` is nullptr.";
    }
    return false;
  }

  auto prim = detail::ReconstructPrimFromPrimSpecInPlace(std::move(primspec), warn, err);

  if (!prim) {
    return false;
  }

  (*prim_out) = std::move(*prim);

  return true;
}

namespace {

// Keep recursive variant discovery within the same defensive namespace ceiling
// used by next::ParseOptions and the Tydra next renderer. The former 1024*1024
// call-site limit did not protect the native stack from hostile hierarchies.
constexpr uint32_t kMaxVariantExtractionDepth = 256;

bool ExtractVariantsRec(uint32_t depth, const std::string &root_path,
                        const PrimSpec &ps, Dictionary &dict,
                        const uint32_t max_depth, std::string *err) {
  if (depth >= max_depth) {
    if (err) {
      (*err) += "Too deep\n";
    }
    return false;
  }

  Dictionary variantInfos;

  if (ps.name().empty()) {
    if (err) {
      (*err) += "PrimSpec name is empty.\n";
    }
    return false;
  }

  std::string full_prim_path = root_path + "/" + ps.name();

  if (ps.metas().variantSets) {
    // Collect all variant sets from all listops
    std::vector<std::string> vsets;
    for (const auto &variantSets_op : ps.metas().variantSets.value()) {
      const auto &items = variantSets_op.second;
      for (const auto &vs : items) {
        vsets.push_back(vs);
      }
    }
    MetaVariable var;
    var.set_value(vsets);
    variantInfos["variantSets"] = var;
  }

  if (ps.metas().variants) {
    Dictionary values;

    const VariantSelectionMap &vsmap = ps.metas().variants.value();
    for (const auto &item : vsmap) {
      MetaVariable var;
      var.set_value(item.second);

      values[item.first] = item.second;
    }

    variantInfos["variants"] = values;
  }

  if (variantInfos.size()) {
    dict[full_prim_path] = variantInfos;
  }

  // Traverse children
  for (const auto &child : ps.children()) {
    if (!ExtractVariantsRec(depth + 1, full_prim_path, child, dict, max_depth,
                            err)) {
      return false;
    }
  }

  return true;
}

bool ExtractVariantsRec(uint32_t depth, const std::string &root_path,
                        const Prim &prim, Dictionary &dict,
                        const uint32_t max_depth, std::string *err) {
  if (depth > max_depth) {
    if (err) {
      (*err) += "Too deep\n";
    }
    return false;
  }

  Dictionary variantInfos;

  if (prim.element_name().empty()) {
    if (err) {
      (*err) += "Prim name is empty.\n";
    }
    return false;
  }

  std::string full_prim_path = root_path + "/" + prim.element_name();

  if (prim.metas().variantSets) {
    // Collect all variant sets from all listops
    std::vector<std::string> vsets;
    for (const auto &variantSets_op : prim.metas().variantSets.value()) {
      const auto &items = variantSets_op.second;
      for (const auto &vs : items) {
        vsets.push_back(vs);
      }
    }
    MetaVariable var;
    var.set_value(vsets);
    variantInfos["variantSets"] = var;
  }

  if (prim.metas().variants) {
    Dictionary values;

    const VariantSelectionMap &vsmap = prim.metas().variants.value();
    for (const auto &item : vsmap) {
      MetaVariable var;
      var.set_value(item.second);

      values[item.first] = item.second;
    }

    variantInfos["variants"] = values;
  }

  // variantSetChildren Prim metadataum supercedes Prim's variantSets Stmt
  if (prim.metas().variantSetChildren) {
    const std::vector<value::token> &vsets =
        prim.metas().variantSetChildren.value();
    // to string
    std::vector<std::string> vsetchildren;
    for (const auto &item : vsets) {
      if (!item.valid()) {
        if (err) {
          (*err) += "Invalid variantSetChildren token found.\n";
        }
        return false;
      }
      vsetchildren.push_back(item.str());
    }
    variantInfos["variantSet"] = vsetchildren;
  } else if (prim.variantSets().size()) {
    Dictionary vsetdict;

    for (const auto &item : prim.variantSets()) {
      if (item.second.variantSet.size()) {
        std::vector<std::string> variantStmtNames;

        if (item.second.name.empty()) {
          if (err) {
            (*err) += "Invalid variantSets Statements found.\n";
          }
          return false;
        }

        for (const auto &v : item.second.variantSet) {
          variantStmtNames.push_back(v.first);
        }

        vsetdict[item.first] = variantStmtNames;
      }
    }

    if (vsetdict.size()) {
      variantInfos["variantSet"] = vsetdict;
    }
  }

  if (variantInfos.size()) {
    dict[full_prim_path] = variantInfos;
  }

  // Traverse children
  for (const auto &child : prim.children()) {
    if (!ExtractVariantsRec(depth + 1, full_prim_path, child, dict, max_depth,
                            err)) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool ExtractVariants(const Layer &layer, Dictionary *dict, std::string *err) {
  if (!dict) {
    if (err) {
      (*err) += "`dict` argument is nullptr.\n";
    }

    return false;
  }

  for (const auto &primspec : layer.primspecs()) {
    if (!ExtractVariantsRec(/* depth */ 0, /* root path */ "", primspec.second,
                            (*dict), kMaxVariantExtractionDepth, err)) {
      return false;
    }
  }

  return true;
}

bool ExtractVariants(const Stage &stage, Dictionary *dict, std::string *err) {
  if (!dict) {
    if (err) {
      (*err) += "`dict` argument is nullptr.\n";
    }

    return false;
  }

  for (const auto &prim : stage.root_prims()) {
    if (!ExtractVariantsRec(/* depth */ 0, /* root path */ "", prim, (*dict),
                            kMaxVariantExtractionDepth, err)) {
      return false;
    }
  }

  return true;
}

bool VariantSelectPrimSpec(
    PrimSpec &dst, const PrimSpec &src,
    const std::map<std::string, std::string> &variant_selection,
    std::string *warn, std::string *err) {
  if (src.metas().variants && src.metas().variantSets) {
    // do variant compsotion
  } else if (src.metas().variants) {
    if (warn) {
      (*warn) +=
          "`variants` are authored, but `variantSets` is not authored.\n";
    }
    dst = src;
    dst.metas().variants.reset();
    dst.metas().variantSets.reset();
    dst.variantSets().clear();
    return true;
  } else if (src.metas().variantSets) {
    if (warn) {
      (*warn) +=
          "`variantSets` are authored, but `variants` is not authored.\n";
    }
    dst = src;
    dst.metas().variants.reset();
    dst.metas().variantSets.reset();
    dst.variantSets().clear();
    // nothing to do.
    return true;
  } else {
    dst = src;
    return true;
  }

  // Collect all variant set names from all listops
  std::vector<std::string> allVariantSetNames;
  for (const auto &variantSets_op : src.metas().variantSets.value()) {
    // TODO: handle different list edit qualifiers appropriately
    const auto &items = variantSets_op.second;
    for (const auto &vs : items) {
      allVariantSetNames.push_back(vs);
    }
  }

  dst = src;

  // Temp PrimSpec collecting the selected variants' opinions only.
  // NOTE: Do NOT init with `src`: local(direct) opinions must stay separate so
  // they can win over variant opinions per LIVRPS (Local > VariantSets).
  PrimSpec ps;

  // A SELECTED variant block may author its own NESTED variantSets (e.g. ALab's
  // `render_high` geo variant contains a `geo_vis` variantSet that supplies the
  // proxy mesh). Those nested sets -- and their selections -- must SURVIVE this
  // pass (which only consumes the OUTER sets) so a subsequent CompositeVariant
  // pass resolves them. Collect them here; re-established after the clear below.
  std::map<std::string, VariantSetSpec> promoted_vsets;
  VariantSelectionMap promoted_selections;

  // Evaluate from the last element.
  for (int64_t i = int64_t(allVariantSetNames.size()) - 1; i >= 0; i--) {
    const auto &variantSetName = allVariantSetNames[size_t(i)];

    // 1. look into `variant_selection`.
    // 2. look into variant setting in this PrimSpec.

    std::string variantName;
    if (variant_selection.count(variantSetName)) {
      variantName = variant_selection.at(variantSetName);
    } else if (dst.current_variant_selection(variantSetName, &variantName)) {
      // ok
    } else {
      continue;
    }

    if (dst.variantSets().count(variantSetName)) {
      const auto &vss = dst.variantSets().at(variantSetName);

      if (vss.variantSet.count(variantName)) {
        const PrimSpec &vs = vss.variantSet.at(variantName);

        DCOUT(fmt::format("variantSet[{}] Select variant: {}", variantSetName,
                          variantName));

        //
        // Promote variant content to PrimSpec.
        //

        // over-like operation
        ps.metas().update_from(vs.metas(), /* override_authored */ true);

        for (const auto &prop : vs.props()) {
          DCOUT("prop: " << prop.first);
          // override existing prop
          ps.props()[prop.first] = prop.second;
        }

        // New (not-yet-present) children contributed by the selected variant,
        // in variant-block order. OpenUSD inserts variant-selected children
        // BEFORE the prim's local children (locals keep authored order), so we
        // collect them and prepend after the merge loop rather than appending.
        std::vector<PrimSpec> new_variant_children;

        for (const auto &child : vs.children()) {
          // Variant child opinions are authored as primspec opinions, so an
          // `over` child in the selected variant must merge into the existing
          // child rather than replace the whole subtree.
          auto it = std::find_if(ps.children().begin(), ps.children().end(),
                                 [&child](const PrimSpec &item) {
                                   return (item.name() == child.name());
                                 });

          if (it != ps.children().end()) {
            // LIVRPS strength: a prim's directly-authored Local opinion (L) is
            // STRONGER than an opinion from a Variant (V). The existing child
            // `*it` is the prim's local opinion (e.g. a UE-exported mesh's deep
            // `over .../UnrealShader` that sets one sourceAsset); the variant's
            // `child` is the weaker base (a different sourceAsset). OverridePrimSpec(d,
            // s) applies `s` as the STRONGER override onto `d`, so merge the
            // variant `child` as the base and let the local `*it` win -- not the
            // reverse, which would let the variant override the local opinion.
            PrimSpec merged = child;  // variant content = weaker base
            if (!OverridePrimSpec(merged, *it, warn, err)) {
              PUSH_ERROR_AND_RETURN("Failed to override variant child PrimSpec.");
            }
            *it = std::move(merged);
          } else {
            new_variant_children.push_back(child);
          }
        }

        // Prepend the new variant children before the local children (OpenUSD
        // order) and keep the `primChildren` order metadatum in sync so the
        // writers reproduce the composed order. The variant's own children come
        // first; existing primChildren (local order) follow.
        if (!new_variant_children.empty()) {
          // Capture names BEFORE moving the children into ps.
          if (!ps.metas().primChildren.empty()) {
            std::vector<value::token> new_pc;
            new_pc.reserve(new_variant_children.size() +
                           ps.metas().primChildren.size());
            for (const auto &c : new_variant_children) {
              new_pc.push_back(value::token(c.name()));
            }
            for (const auto &t : ps.metas().primChildren) {
              new_pc.push_back(t);
            }
            ps.metas().primChildren = std::move(new_pc);
          }

          ps.children().insert(
              ps.children().begin(),
              std::make_move_iterator(new_variant_children.begin()),
              std::make_move_iterator(new_variant_children.end()));
        }

        // Promote the selected variant block's OWN (nested) variantSets and
        // their selections so they outlive the wholesale clear below and a later
        // pass can resolve them (the variant content -- not just metadata --
        // lives in vs.variantSets(), which the prop/child merge above does not
        // touch).
        for (const auto &nvs : vs.variantSets()) {
          promoted_vsets[nvs.first] = nvs.second;
        }
        if (vs.metas().variants) {
          for (const auto &sel : vs.metas().variants.value()) {
            promoted_selections[sel.first] = sel.second;
          }
        }
      }
    }
  }

  DCOUT("Variant resolved prim: " << prim::print_primspec(ps));

  // Local properties/metadatum win against properties/metadataum from Variant
  // (LIVRPS: Local > VariantSets), so merge the variant opinions underneath
  // the local ones.
  if (!InheritPrimSpec(dst, ps, warn, err)) {
    PUSH_ERROR_AND_RETURN("Failed to merge variant PrimSpec.");
  }

  // OverridePrimSpec APPENDS children that are new in `ps` (the variant-selected
  // children) to `dst`, which loses the OpenUSD order (variant children first).
  // `ps` already holds the composed order, so reorder `dst`'s children to match
  // it and adopt its maintained `primChildren`. A no-op when nothing reordered.
  {
    std::vector<PrimSpec> &dc = dst.children();
    std::vector<bool> used(dc.size(), false);
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < dc.size(); i++) {
      idx.emplace(dc[i].name(), i);  // first occurrence wins
    }
    std::vector<PrimSpec> reordered;
    reordered.reserve(dc.size());
    for (const auto &pc : ps.children()) {
      auto it = idx.find(pc.name());
      if (it != idx.end() && !used[it->second]) {
        used[it->second] = true;
        reordered.push_back(std::move(dc[it->second]));
      }
    }
    for (size_t i = 0; i < dc.size(); i++) {
      if (!used[i]) reordered.push_back(std::move(dc[i]));
    }
    dc = std::move(reordered);
    dst.metas().primChildren = ps.metas().primChildren;
  }

  // The OUTER variantSets resolved in this pass are consumed: clear ALL variant
  // metadata/content, then re-establish only the NESTED variantSets promoted
  // from the selected variant block(s). Without the re-establish, a variant
  // whose content contains a nested variantSet (ALab render_high -> geo_vis ->
  // proxy mesh) would silently lose that nested content on selection.
  dst.metas().variants.reset();
  dst.metas().variantSets.reset();
  dst.variantSets().clear();

  if (!promoted_vsets.empty()) {
    std::vector<std::string> names;
    names.reserve(promoted_vsets.size());
    for (auto &kv : promoted_vsets) {
      names.push_back(kv.first);
      dst.variantSets()[kv.first] = std::move(kv.second);
    }
    dst.metas().variantSets =
        std::vector<std::pair<ListEditQual, std::vector<std::string>>>{
            {ListEditQual::ResetToExplicit, std::move(names)}};
    if (!promoted_selections.empty()) {
      dst.metas().variants = std::move(promoted_selections);
    }
  }

  return true;
}

bool VariantSelectPrimSpec(
    PrimSpec &dst, PrimSpec &&src,
    const std::map<std::string, std::string> &variant_selection,
    std::string *warn, std::string *err) {
  // Copy-fallback for the move-in overload: this branch's variant selection
  // was rewritten for LIVRPS correctness (local opinions kept separate from
  // variant opinions, nested variantSets promoted) and does not implement the
  // per-prim move-donation fast path — bind to the copying overload. The big
  // move win (whole-layer) lives in CompositeVariantInPlace.
  const PrimSpec &csrc = src;
  return VariantSelectPrimSpec(dst, csrc, variant_selection, warn, err);
}

}  // namespace lightusd
