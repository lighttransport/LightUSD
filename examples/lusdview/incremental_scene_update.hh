// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu_scene.hh"
#include "next/stage/change-set.hh"

namespace lusdview {

struct IncrementalSceneUpdatePlan {
  bool compatible{false};
  std::string reason;
  std::vector<size_t> vertexUpdates;
  std::vector<size_t> worldUpdates;
  std::vector<size_t> instanceUpdates;
  std::vector<size_t> replacementUpdates;
  std::vector<size_t> materialUpdates;
  std::vector<size_t> textureUpdates;
  size_t estimatedUploadBytes{0};
};

// Records the renderer-visible mesh streams before large CPU arrays are freed.
void CaptureMeshUploadIdentity(DrawMeshCPU* mesh);
void CaptureTextureUploadIdentity(DrawTextureCPU* texture);

// Validate a replacement DrawScene without touching the renderer. A compatible
// plan can be committed atomically because every operation targets an existing
// mesh slot; rejection leaves the caller free to perform a full upload.
IncrementalSceneUpdatePlan PlanIncrementalSceneUpdate(
    const DrawScene& current, DrawScene* next,
    const lightusd::next::StageChangeSet& changes,
    uint64_t displayedRevision, int rendererMeshCount);

}  // namespace lusdview
