#include "shader-network.hh"
#include "prim-apply.hh"

#include "common-macros.inc"
#include "tiny-format.hh"
#include "core/prim.hh"
#include "usdShade.hh"
#include "prim-pprint.hh"
#include "value-pprint.hh"
#include "stage.hh"
#include "common-macros.inc"
#include "tydra/scene-access.hh"


#define PushError(msg) { \
  if (err) { \
    (*err) += msg; \
  } \
}

namespace lightusd {
namespace tydra {

namespace {

// TODO: There are lots of duplicated codes with EvaluateAttribute()
// Use EvaluateAttribute and deprecate EvaluateShaderAttribute?

bool EvaluateUsdPreviewSurfaceAttribute(
  const Stage &stage,
  const UsdPreviewSurface &shader,
  const std::string &attr_name,
  const uint32_t req_type_id,
  value::Value &out_val,
  std::string *err,
  const value::TimeCode timeCode) {

  (void)stage;

  if ((attr_name == "diffuseColor") && (req_type_id == value::TypeTraits<value::color3f>::type_id())) {
    if (shader.diffuseColor.authored()) {

    } else {
      value::color3f col;
      if (shader.diffuseColor.get_value().get_scalar(&col)) {
        out_val = col;
        return true;
      }
    }
  }

  (void)err;
  (void)timeCode;

  return false;
}


} // namespace local


template<typename T>
bool EvaluateShaderAttribute(
  const Stage &stage,
  const Shader &shader, const std::string &attr_name,
  T * out_val,
  std::string *err,
  const value::TimeCode timeCode) {

  if (!out_val) {
    return false;
  }

  uint32_t tyid = value::TypeTraits<T>::type_id();
  value::Value outval;

  bool result = false;

  if (const auto *psurf = shader.value.as<UsdPreviewSurface>()) {
    result = EvaluateUsdPreviewSurfaceAttribute(stage, *psurf, attr_name, tyid, outval, err, timeCode);
    if (const auto pt = outval.as<T>()) {
      (*out_val) = (*pt);
    } else {
      if (err) {
        (*err) += "[InternalError] Type mismatch.\n";
      }
      return false;
    }
  } else {
    if (err) {
      (*err) += "Unsupported shader type: " + shader.value.type_name() + "\n";
    }
    return false;
  }

  return result;
}

#define INSTANCIATE_EVAL_SHADER(__ty) \
template bool EvaluateShaderAttribute( const Stage &stage, const Shader &shader, const std::string &attr_name, __ty * out_val, std::string *err, const value::TimeCode timeCode)

INSTANCIATE_EVAL_SHADER(value::token);
INSTANCIATE_EVAL_SHADER(std::string);
INSTANCIATE_EVAL_SHADER(value::half);
INSTANCIATE_EVAL_SHADER(value::half2);
INSTANCIATE_EVAL_SHADER(value::half3);
INSTANCIATE_EVAL_SHADER(value::half4);
INSTANCIATE_EVAL_SHADER(int32_t);
INSTANCIATE_EVAL_SHADER(value::int2);
INSTANCIATE_EVAL_SHADER(value::int3);
INSTANCIATE_EVAL_SHADER(value::int4);
INSTANCIATE_EVAL_SHADER(uint32_t);
INSTANCIATE_EVAL_SHADER(value::uint2);
INSTANCIATE_EVAL_SHADER(value::uint3);
INSTANCIATE_EVAL_SHADER(value::uint4);
INSTANCIATE_EVAL_SHADER(float);
INSTANCIATE_EVAL_SHADER(value::float2);
INSTANCIATE_EVAL_SHADER(value::float3);
INSTANCIATE_EVAL_SHADER(value::float4);
INSTANCIATE_EVAL_SHADER(double);
INSTANCIATE_EVAL_SHADER(value::double2);
INSTANCIATE_EVAL_SHADER(value::double3);
INSTANCIATE_EVAL_SHADER(value::double4);
INSTANCIATE_EVAL_SHADER(value::quath);
INSTANCIATE_EVAL_SHADER(value::quatf);
INSTANCIATE_EVAL_SHADER(value::quatd);
INSTANCIATE_EVAL_SHADER(value::color3h);
INSTANCIATE_EVAL_SHADER(value::color3f);
INSTANCIATE_EVAL_SHADER(value::color3d);
INSTANCIATE_EVAL_SHADER(value::color4h);
INSTANCIATE_EVAL_SHADER(value::color4f);
INSTANCIATE_EVAL_SHADER(value::color4d);
INSTANCIATE_EVAL_SHADER(value::vector3h);
INSTANCIATE_EVAL_SHADER(value::vector3f);
INSTANCIATE_EVAL_SHADER(value::vector3d);
INSTANCIATE_EVAL_SHADER(value::point3h);
INSTANCIATE_EVAL_SHADER(value::point3f);
INSTANCIATE_EVAL_SHADER(value::point3d);
INSTANCIATE_EVAL_SHADER(value::normal3h);
INSTANCIATE_EVAL_SHADER(value::normal3f);
INSTANCIATE_EVAL_SHADER(value::normal3d);
INSTANCIATE_EVAL_SHADER(value::matrix2d);
INSTANCIATE_EVAL_SHADER(value::matrix3d);
INSTANCIATE_EVAL_SHADER(value::matrix4d);

// instanciations

namespace {

bool GetSinglePath(const Relationship &rel, Path *path) {
  if (!path) {
    return false;
  }

  if (rel.is_path()) {
    (*path) = rel.targetPath;
    return true;
  } else if (rel.is_pathvector()) {
    if (rel.targetPathVector.size() > 0) {
      (*path) = rel.targetPathVector[0];
      return true;
    }
  }

  return false;
}

} // namespace local

bool GetDirectlyBoundMaterial(
  const Stage &_stage,
  const Prim &prim,
  const std::string &purpose,
  lightusd::Path *materialPath,
  const Material **material,
  std::string *err) {

  if (!materialPath) {
    PUSH_ERROR_AND_RETURN("`materialPath` ptr is null.");
  }

  if (!material) {
    PUSH_ERROR_AND_RETURN("`material` ptr is null.");
  }

  auto apply_fun = [&](const Stage &stage, const MaterialBinding *mb) -> bool {

    Relationship mat_rel;
    if (!mb->get_materialBinding(value::token(purpose), &mat_rel)) {
      return false;
    }

    if (!GetSinglePath(mat_rel, materialPath)) {
      // A `rel material:binding` with no target paths (declaration-only,
      // e.g. `rel material:binding ( bindMaterialAs = "weakerThanDescendants" )`)
      // or `rel material:binding = None` is a valid USD pattern — it
      // means "no material bound at this prim; inherit from parent".
      // Return false so the caller's parent-walk handles it. (The
      // previous code raised an error with a misleading "must be
      // single targetPath" message; GetSinglePath actually picks
      // the first target on multi-target paths, so the original
      // wording never matched the failure mode.)
      return false;
    }

    const Prim *p{nullptr};
    // Swallow find_prim_at_path's error: a `material:binding` target
    // that doesn't resolve in the stage is a valid USD pattern (the
    // material was authored in a stronger layer that was later
    // dropped, or the asset references it in another sublayer). The
    // caller's parent-walk handles the "no binding here" case
    // gracefully — propagating the "Cannot find path" error sabotages
    // an otherwise loadable asset.
    std::string lookup_err;
    if (stage.find_prim_at_path(*materialPath, p, &lookup_err)) {
      if (p->is<Material>()) {
        (*material) = p->as<Material>();
        return true;
      }
    }

    (*material) = nullptr;
    return false;
  };

  bool ret = ApplyToMaterialBinding(_stage, prim, apply_fun);

  return ret;
}

bool GetDirectlyBoundMaterial(
  const Stage &stage,
  const Path &abs_path,
  const std::string &purpose,
  lightusd::Path *materialPath,
  const Material **material,
  std::string *err) {

  const Prim *p{nullptr};
  if (stage.find_prim_at_path(abs_path, p, err)) {
    return GetDirectlyBoundMaterial(stage, *p, purpose, materialPath, material, err);
  }

  return false;
}

bool GetDirectCollectionMaterialBinding(
  const Stage &_stage,
  const Prim &prim,
  const std::string &purpose,
  lightusd::Path *materialPath,
  const Material **material,
  std::string *err) {

  if (!materialPath) {
    PUSH_ERROR_AND_RETURN("`materialPath` ptr is null.");
  }

  if (!material) {
    PUSH_ERROR_AND_RETURN("`material` ptr is null.");
  }

  (void)err;

  auto apply_fun = [&](const Stage &stage, const MaterialBinding *mb) -> bool {

    Relationship mat_rel;
    if (!mb->get_materialBinding(value::token(purpose), &mat_rel)) {
      return false;
    }

    if (!GetSinglePath(mat_rel, materialPath)) {
      return false;
    }

    const Prim *p;
    if (stage.find_prim_at_path(*materialPath, p, err)) {
      if (p->is<Material>() && (material != nullptr)) {
        (*material) = p->as<Material>();
      } else {
        (*material) = nullptr;
      }
    }

    return false;
  };

  bool ret = ApplyToMaterialBinding(_stage, prim, apply_fun);

  return ret;
}

bool DirectBindingStrongerThanDescendants(
  const Stage &_stage,
  const Prim &prim,
  const std::string &purpose)
{
  auto apply_fun = [&](const Stage &stage, const MaterialBinding *mb) -> bool {

    (void)stage;

    Relationship mat_rel;
    if (!mb->get_materialBinding(value::token(purpose), &mat_rel)) {
      return false;
    }

    const value::token strength = mat_rel.metas().has_bindMaterialAs() ? mat_rel.metas().get_bindMaterialAs() : value::token(kWeakerThanDescendants);
    return strength.str() == kStrongerThanDescendants;

  };

  bool ret = ApplyToMaterialBinding(_stage, prim, apply_fun);

  return ret;
}

bool DirectBindingStrongerThanDescendants(
  const Stage &stage,
  const Path &abs_path,
  const std::string &purpose) {

  const Prim *p{nullptr};
  if (stage.find_prim_at_path(abs_path, p)) {
    return DirectBindingStrongerThanDescendants(stage, *p, purpose);
  }

  return false;
}


bool GetBoundMaterial(
  const Stage &_stage,
  const Path &abs_path,
  const std::string &materialPurpose,
  lightusd::Path *materialPath,
  const Material **material,
  std::string *err) {

  if (materialPath == nullptr) {
    return false;
  }

  if (material == nullptr) {
    return false;
  }

  std::vector<value::token> purposes;
  if (materialPurpose.empty()) {
    // No purpose requested: this is a preview/realtime consumer, so use the same
    // fallback chain as the `next` resolver (lightusd::next::GetBoundMaterialPath)
    // -- `material:binding:preview`, then the all-purpose `material:binding`, then
    // `material:binding:full`.
    //
    // Trying ONLY the all-purpose binding (the previous behavior) silently loses
    // the material on every scene that binds purpose-scoped and never authors a
    // plain `material:binding` -- which is how production assets (e.g. ALab) bind.
    // The material, and with it every texture, was dropped and the prim fell back
    // to the default gray.
    purposes.push_back(value::token("preview"));
    purposes.push_back(value::token(""));  // all-purpose
    purposes.push_back(value::token("full"));
  } else {
    purposes.push_back(value::token(materialPurpose));
    purposes.push_back(value::token("")); // all-purpose
  }

  // for purpose : purposes:
  //
  //   boundMaterial = None
  //
  //   for p = prim, p != Root, p = p.GetParent():
  //
  //     if DirectBindingStrongerThanDescendants(p, purpose) or not boundMaterial:
  //
  //       if dicrectBind = GetDirectlyBoundMaterial(p, purpose):
  //
  //         boundMaterial = directBound
  //
  //     for collBinding : GetCollectionMaterialBindings(p, purpose):
  //
  //       if (collBinding.GetCollection().Contains(prim) and
  //           collBinding.IsStrongerThanDescendants() or not boundMaterial):
  //
  //         boundMaterial = collBinding.GetMaterial()
  //         break
  //
  //
  //   if boundMaterial:
  //     return boundMaterial
  //
  //
  // return False(or return default material)

  for (const auto &purpose : purposes) {

    Path currentPath = abs_path;
    Path boundMaterialPath;
    const Material *boundMaterial{nullptr};

    // We need to climb up to the root in any case.
    // TODO: Cache result.
    uint32_t depth = 0;
    while (size_t(depth) < kMaxDefaultTraversalLimit) { // to avoid infinite loop.

      if (!currentPath.is_valid() || currentPath.is_root_path()) {
        break;
      }

      const Prim *prim{nullptr};
      bool ret = _stage.find_prim_at_path(currentPath, prim, err);
      if (!ret) {
        return false;
      }

      if (!boundMaterial || DirectBindingStrongerThanDescendants(_stage, *prim, purpose.str())) {

        std::string _err;
        Path directMaterialPath;
        const Material *directMaterial{nullptr};
        bool has_directBound = GetDirectlyBoundMaterial(_stage, *prim, purpose.str(), &directMaterialPath, &directMaterial, &_err);

        if (has_directBound && directMaterial) {

          boundMaterial = directMaterial;
          boundMaterialPath = directMaterialPath;

        } else if (_err.size()) {
          if (err) {
            (*err) += _err;
          }
          return false;
        }
      }

      // Collection bindings on this ancestor are ordered by collection name.
      // The first matching binding is strongest at this namespace level.
      // A strongerThanDescendants collection may replace a descendant result;
      // otherwise it fills only an as-yet-unbound result.
      const MaterialBinding *material_binding = nullptr;
      ApplyToMaterialBinding(
          _stage, *prim,
          [&](const Stage &stage, const MaterialBinding *mb) -> bool {
            material_binding = mb;
            return true;
          });
      const Collection *collection = nullptr;
      if (material_binding && GetCollection(*prim, &collection) && collection) {
        for (const auto &binding :
             material_binding->materialBindingCollectionMap()) {
          const Relationship *relationship = nullptr;
          if (!binding.second.at(purpose.str(), &relationship) ||
              !relationship) {
            continue;
          }
          const CollectionInstance *instance = nullptr;
          if (!collection->get_instance(binding.first, &instance) || !instance) {
            continue;
          }
          const CollectionMembershipQuery query =
              BuildCollectionMembershipQuery(
                  _stage, *instance, prim->absolute_path().prim_part());
          if (!IsPathIncluded(query, _stage, abs_path)) continue;

          const bool stronger =
              relationship->metas().has_bindMaterialAs() &&
              relationship->metas().get_bindMaterialAs().str() ==
                  kStrongerThanDescendants;
          if (boundMaterial && !stronger) break;

          Path collectionMaterialPath;
          if (!GetSinglePath(*relationship, &collectionMaterialPath)) continue;
          const Prim *materialPrim = nullptr;
          std::string lookupError;
          if (!_stage.find_prim_at_path(collectionMaterialPath, materialPrim,
                                        &lookupError) ||
              !materialPrim || !materialPrim->is<Material>()) {
            continue;
          }
          boundMaterialPath = collectionMaterialPath;
          boundMaterial = materialPrim->as<Material>();
          break;
        }
      }

      Path parentPath = currentPath.get_parent_prim_path();
      DCOUT("search parent: " << parentPath.full_path_name());

      currentPath = parentPath;

      depth++;

    }

    if (boundMaterial) {
      (*material) = boundMaterial;
      (*materialPath) = boundMaterialPath;
      return true;
    }
  }

  return false;
}

} // namespace tydra
} // namespace lightusd
