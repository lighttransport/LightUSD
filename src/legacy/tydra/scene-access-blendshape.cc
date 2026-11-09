// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "scene-access.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"
#include "usdSkel.hh"

#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace tydra {

std::vector<std::pair<std::string, const lightusd::BlendShape *>>
GetBlendShapes(const lightusd::Stage &stage, const lightusd::Prim &prim,
               std::string *err) {
  std::vector<std::pair<std::string, const lightusd::BlendShape *>> dst;

  auto *pmesh = prim.as<GeomMesh>();
  if (!pmesh) {
    if (err) {
      (*err) += "Prim must be GeomMesh.\n";
    }
    return dst;
  }

  //
  // BlendShape Prim may not be a child of GeomMesh. So need to search Prim in
  // Stage
  //
  if (pmesh->blendShapes.authored() && pmesh->blendShapeTargets.has_value()) {
    // TODO: connection?
    std::vector<value::token> blendShapeNames;

    if (!pmesh->blendShapes.get_value(&blendShapeNames)) {
      if (err) {
        (*err) += "Failed to get `skel:blendShapes` attribute.\n";
      }
      return dst;
    }

    if (pmesh->blendShapeTargets.value().is_path()) {
      if (blendShapeNames.size() != 1) {
        if (err) {
          (*err) +=
              "Array size mismatch with `skel:blendShapes` and "
              "`skel:blendShapeTargets`.\n";
        }
        return dst;
      }

      const Path &targetPath = pmesh->blendShapeTargets.value().targetPath;
      const Prim *bsprim{nullptr};
      if (!stage.find_prim_at_path(targetPath, bsprim, err)) {
        return dst;
      }
      if (!bsprim) {
        if (err) {
          (*err) += "Internal error. BlendShape Prim is nullptr.\n";
        }
        return dst;
      }

      if (const auto *bs = bsprim->as<BlendShape>()) {
        dst.push_back(std::make_pair(blendShapeNames[0].str(), bs));
      } else {
        if (err) {
          (*err) += fmt::format("{} is not BlendShape Prim.\n",
                                targetPath.full_path_name());
        }
        return dst;
      }

    } else if (pmesh->blendShapeTargets.value().is_pathvector()) {
      if (blendShapeNames.size() !=
          pmesh->blendShapeTargets.value().targetPathVector.size()) {
        if (err) {
          (*err) +=
              "Array size mismatch with `skel:blendShapes` and "
              "`skel:blendShapeTargets`.\n";
        }
        return dst;
      }
    } else {
      if (err) {
        (*err) +=
            "Invalid or unsupported definition of `skel:blendShapeTargets` "
            "relationship.\n";
      }
      return dst;
    }

    for (size_t i = 0;
         i < pmesh->blendShapeTargets.value().targetPathVector.size(); i++) {
      const Path &targetPath =
          pmesh->blendShapeTargets.value().targetPathVector[i];
      const Prim *bsprim{nullptr};
      if (!stage.find_prim_at_path(targetPath, bsprim, err)) {
        return dst;
      }
      if (!bsprim) {
        if (err) {
          (*err) += "Internal error. BlendShape Prim is nullptr.";
        }
        return dst;
      }

      if (const auto *bs = bsprim->as<BlendShape>()) {
        dst.push_back(std::make_pair(blendShapeNames[i].str(), bs));
      } else {
        if (err) {
          (*err) += fmt::format("{} is not BlendShape Prim.",
                                targetPath.full_path_name());
        }
        return dst;
      }
    }
  }

  return dst;
}

}  // namespace tydra
}  // namespace lightusd
