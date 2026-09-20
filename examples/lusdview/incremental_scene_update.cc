// SPDX-License-Identifier: Apache-2.0
#include "incremental_scene_update.hh"

#include <cstring>
#include <unordered_map>

namespace lusdview {
namespace {

uint64_t HashUploadBytes(uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= uint64_t(bytes[i]);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

IncrementalSceneUpdatePlan Reject(const char* reason) {
  IncrementalSceneUpdatePlan plan;
  plan.reason = reason;
  return plan;
}

bool AtOrBelow(const std::string& path, const std::string& root) {
  return path == root ||
         (path.size() > root.size() &&
          path.compare(0, root.size(), root) == 0 &&
          path[root.size()] == '/');
}

bool MaterialLayoutCompatible(const DrawMaterialCPU& a,
                              const DrawMaterialCPU& b) {
  return a.absPath == b.absPath &&
      a.hasUsdPreviewSurface == b.hasUsdPreviewSurface &&
      a.hasOpenPBRSurface == b.hasOpenPBRSurface &&
      a.hasDisplacementOutput == b.hasDisplacementOutput &&
      a.hasVolumeOutput == b.hasVolumeOutput &&
      a.displacementShaderPath == b.displacementShaderPath &&
      a.volumeShaderPath == b.volumeShaderPath &&
      a.materialXNodeGraphJson == b.materialXNodeGraphJson &&
      a.volumeMaterialXNodeGraphJson == b.volumeMaterialXNodeGraphJson &&
      a.materialXVolumeGraph == b.materialXVolumeGraph &&
      a.baseColorTex == b.baseColorTex && a.metallicTex == b.metallicTex &&
      a.roughnessTex == b.roughnessTex && a.normalTex == b.normalTex &&
      a.coatNormalTex == b.coatNormalTex &&
      a.emissiveTex == b.emissiveTex && a.opacityTex == b.opacityTex &&
      a.occlusionTex == b.occlusionTex &&
      a.specularColorTex == b.specularColorTex &&
      a.coatWeightTex == b.coatWeightTex &&
      a.coatColorTex == b.coatColorTex &&
      a.coatRoughnessTex == b.coatRoughnessTex &&
      a.displacementTex == b.displacementTex;
}

template <typename T>
void HashVector(uint64_t* hash, const std::vector<T>& values) {
  const size_t size = values.size();
  *hash = HashUploadBytes(*hash, &size, sizeof(size));
  if (!values.empty()) {
    *hash = HashUploadBytes(*hash, values.data(), values.size() * sizeof(T));
  }
}

void HashString(uint64_t* hash, const std::string& value) {
  const size_t size = value.size();
  *hash = HashUploadBytes(*hash, &size, sizeof(size));
  if (!value.empty()) *hash = HashUploadBytes(*hash, value.data(), value.size());
}

uint64_t DeformationLayoutIdentity(const DrawMeshCPU& mesh) {
  uint64_t hash = UINT64_C(1469598103934665603);
  HashVector(&hash, mesh.jointIdx);
  HashVector(&hash, mesh.jointWt);
  HashVector(&hash, mesh.influenceOffsetCount);
  HashVector(&hash, mesh.influenceTexels);
  HashVector(&hash, mesh.morphOffsetCount);
  HashVector(&hash, mesh.morphDeltaHalf);
  HashVector(&hash, mesh.morphChannelId);
  hash = HashUploadBytes(hash, &mesh.morphChannelCount,
                         sizeof(mesh.morphChannelCount));
  for (const MorphTargetCPU& target : mesh.morphs) {
    HashString(&hash, target.name);
    HashVector(&hash, target.vtx);
    HashVector(&hash, target.dpos);
    for (const MorphInbetweenCPU& inbetween : target.inbetweens) {
      hash = HashUploadBytes(hash, &inbetween.weight,
                             sizeof(inbetween.weight));
      HashVector(&hash, inbetween.dpos);
    }
  }
  for (const MorphTargetChannelsCPU& target : mesh.morphTargetChannels) {
    HashString(&hash, target.name);
    HashVector(&hash, target.usdWeights);
    HashVector(&hash, target.channelIds);
  }
  return hash;
}

bool IsDeformable(const DrawMeshCPU& mesh) {
  return !mesh.jointIdx.empty() || !mesh.influenceOffsetCount.empty() ||
         !mesh.morphDeltaHalf.empty() || !mesh.morphs.empty();
}

bool HasTranslucentInstances(const DrawMeshCPU& mesh) {
  constexpr float kOpaqueThreshold = 1.0f - 1.0e-6f;
  if (mesh.flatOpacity < kOpaqueThreshold) return true;
  for (float opacity : mesh.instanceOpacities) {
    if (opacity < kOpaqueThreshold) return true;
  }
  for (float opacity : mesh.vertexAlpha) {
    if (opacity < kOpaqueThreshold) return true;
  }
  return false;
}

size_t TexturePayloadBytes(const DrawTextureCPU& texture) {
  size_t bytes = texture.image.data.size() + texture.compressed.data.size();
  for (const light3d::Image& mip : texture.mipImages) bytes += mip.data.size();
  for (const DrawCompressedMipCPU& mip : texture.compressed.mips)
    bytes += mip.data.size();
  return bytes;
}

size_t MeshUploadBytes(const DrawMeshCPU& mesh) {
  return mesh.vertices.size() * sizeof(DrawVertex) +
         mesh.indices.size() * sizeof(uint32_t) +
         mesh.instanceXforms.size() * sizeof(float) +
         mesh.instanceColors.size() * sizeof(float) +
         mesh.instanceOpacities.size() * sizeof(float) +
         mesh.jointIdx.size() * sizeof(uint32_t) +
         mesh.jointWt.size() * sizeof(float) +
         mesh.influenceOffsetCount.size() * sizeof(uint32_t) +
         mesh.influenceTexels.size() * sizeof(float) +
         mesh.morphOffsetCount.size() * sizeof(uint32_t) +
         mesh.morphDeltaHalf.size() * sizeof(uint16_t) +
         mesh.morphChannelId.size() * sizeof(uint16_t);
}

}  // namespace

void CaptureMeshUploadIdentity(DrawMeshCPU* mesh) {
  if (!mesh || mesh->vertices.empty()) return;
  uint64_t vertices = UINT64_C(1469598103934665603);
  vertices = HashUploadBytes(vertices, mesh->vertices.data(),
                            mesh->vertices.size() * sizeof(DrawVertex));
  uint64_t topology = UINT64_C(1469598103934665603);
  topology = HashUploadBytes(topology, mesh->indices.data(),
                            mesh->indices.size() * sizeof(uint32_t));
  for (const DrawSubmesh& submesh : mesh->submeshes) {
    topology = HashUploadBytes(topology, &submesh.indexOffset,
                               sizeof(submesh.indexOffset));
    topology = HashUploadBytes(topology, &submesh.indexCount,
                               sizeof(submesh.indexCount));
    topology = HashUploadBytes(topology, &submesh.materialId,
                               sizeof(submesh.materialId));
    topology = HashUploadBytes(topology, &submesh.backfaceMaterialId,
                               sizeof(submesh.backfaceMaterialId));
  }
  const size_t vertex_count = mesh->vertices.size();
  topology = HashUploadBytes(topology, &vertex_count, sizeof(vertex_count));
  mesh->uploadedVertexFingerprint = vertices;
  mesh->uploadedTopologyFingerprint = topology;
  mesh->uploadedVertexCount = vertex_count;
}

void CaptureTextureUploadIdentity(DrawTextureCPU* texture) {
  if (!texture || texture->isUdim || texture->isPtex) return;
  const bool has_payload = !texture->image.data.empty() ||
                           !texture->compressed.data.empty() ||
                           !texture->mipImages.empty() ||
                           !texture->compressed.mips.empty();
  if (!has_payload && texture->uploadedFingerprint != 0) return;
  uint64_t hash = UINT64_C(1469598103934665603);
  const auto add = [&](const void* data, size_t size) {
    hash = HashUploadBytes(hash, data, size);
  };
  add(&texture->image.width, sizeof(texture->image.width));
  add(&texture->image.height, sizeof(texture->image.height));
  add(&texture->image.channels, sizeof(texture->image.channels));
  add(&texture->srgb, sizeof(texture->srgb));
  add(&texture->wrapS, sizeof(texture->wrapS));
  add(&texture->wrapT, sizeof(texture->wrapT));
  add(&texture->requestedCompressed, sizeof(texture->requestedCompressed));
  add(&texture->streamingMutable, sizeof(texture->streamingMutable));
  add(&texture->compressed.format, sizeof(texture->compressed.format));
  add(&texture->compressed.width, sizeof(texture->compressed.width));
  add(&texture->compressed.height, sizeof(texture->compressed.height));
  if (!texture->image.data.empty())
    add(texture->image.data.data(), texture->image.data.size());
  if (!texture->compressed.data.empty())
    add(texture->compressed.data.data(), texture->compressed.data.size());
  for (const light3d::Image& mip : texture->mipImages) {
    add(&mip.width, sizeof(mip.width));
    add(&mip.height, sizeof(mip.height));
    if (!mip.data.empty()) add(mip.data.data(), mip.data.size());
  }
  for (const DrawCompressedMipCPU& mip : texture->compressed.mips) {
    add(&mip.width, sizeof(mip.width));
    add(&mip.height, sizeof(mip.height));
    if (!mip.data.empty()) add(mip.data.data(), mip.data.size());
  }
  texture->uploadedFingerprint = hash;
}

IncrementalSceneUpdatePlan PlanIncrementalSceneUpdate(
    const DrawScene& current, DrawScene* next,
    const lightusd::next::StageChangeSet& changes,
    uint64_t displayedRevision, int rendererMeshCount) {
  using Flag = lightusd::next::StageChangeFlag;
  if (!next) return Reject("missing replacement scene");
  if (changes.base_revision != displayedRevision ||
      changes.new_revision <= changes.base_revision) {
    return Reject("change set does not follow the displayed revision");
  }
  if (changes.full_resync || changes.stage_metadata_changed ||
      changes.prims.empty()) {
    return Reject("change set requires full scene processing");
  }
  if (rendererMeshCount != static_cast<int>(current.meshes.size())) {
    return Reject("renderer mesh slots do not match replacement scene");
  }
  if (next->materials.size() != current.materials.size() ||
      next->textures.size() != current.textures.size()) {
    return Reject("material or texture catalog changed");
  }
  if (!current.points.empty() || !next->points.empty() ||
      !current.curves.empty() || !next->curves.empty() ||
      !current.volumes.empty() || !next->volumes.empty()) {
    return Reject("non-mesh draw data requires full scene processing");
  }

  const uint32_t allowed = static_cast<uint32_t>(Flag::Transform) |
                           static_cast<uint32_t>(Flag::Topology) |
                           static_cast<uint32_t>(Flag::Primvar) |
                           static_cast<uint32_t>(Flag::Resync) |
                           static_cast<uint32_t>(Flag::Material) |
                           static_cast<uint32_t>(Flag::Texture);
  bool topology_change = false;
  bool structural_change = false;
  bool material_change = false;
  bool texture_change = false;
  for (const auto& change : changes.prims) {
    const uint32_t flags = static_cast<uint32_t>(change.flags);
    if (flags == 0 || (flags & ~allowed) != 0) {
      return Reject("change set contains an unsupported structural edit");
    }
    topology_change = topology_change ||
        lightusd::next::HasStageChange(change.flags, Flag::Topology);
    structural_change = structural_change ||
        lightusd::next::HasStageChange(change.flags, Flag::Resync);
    const bool changes_texture =
        lightusd::next::HasStageChange(change.flags, Flag::Texture);
    texture_change = texture_change || changes_texture;
    bool changes_material_constants =
        lightusd::next::HasStageChange(change.flags, Flag::Material);
    // Shader inputs:file is classified as both Material and Texture because it
    // lives on a Shader prim. If every reported property is texture-specific,
    // the material binding/constant record is unchanged and only its texture
    // slot needs replacement.
    if (changes_material_constants && changes_texture &&
        !change.properties.empty()) {
      changes_material_constants = std::any_of(
          change.properties.begin(), change.properties.end(),
          [](const std::string& property) {
            return property.find("file") == std::string::npos &&
                   property.find("texture") == std::string::npos &&
                   property.find("sourceColorSpace") == std::string::npos;
          });
    }
    material_change = material_change || changes_material_constants;
  }

  IncrementalSceneUpdatePlan plan;
  if (texture_change) {
    for (size_t i = 0; i < next->textures.size(); ++i) {
      DrawTextureCPU& incoming = next->textures[i];
      const DrawTextureCPU& resident = current.textures[i];
      if (incoming.isUdim || incoming.isPtex || resident.isUdim ||
          resident.isPtex) {
        return Reject("UDIM or Ptex replacement requires full processing");
      }
      CaptureTextureUploadIdentity(&incoming);
      uint64_t resident_identity = resident.uploadedFingerprint;
      if (resident_identity == 0) {
        DrawTextureCPU copy = resident;
        CaptureTextureUploadIdentity(&copy);
        resident_identity = copy.uploadedFingerprint;
      }
      if (resident_identity == 0 || incoming.uploadedFingerprint == 0) {
        return Reject("texture upload identity is unavailable");
      }
      if (incoming.uploadedFingerprint != resident_identity)
        plan.textureUpdates.push_back(i);
    }
    if (plan.textureUpdates.empty()) {
      return Reject("texture change did not alter a renderer texture slot");
    }
  }
  if (material_change) {
    for (size_t i = 0; i < next->materials.size(); ++i) {
      if (!MaterialLayoutCompatible(current.materials[i], next->materials[i])) {
        return Reject("material shader or texture layout changed");
      }
      const std::string& material_path = next->materials[i].absPath;
      if (material_path.empty()) continue;
      for (const auto& change : changes.prims) {
        if (!lightusd::next::HasStageChange(change.flags, Flag::Material)) {
          continue;
        }
        const std::string path = change.path.str();
        if (AtOrBelow(path, material_path) ||
            AtOrBelow(material_path, path)) {
          plan.materialUpdates.push_back(i);
          break;
        }
      }
    }
    if (plan.materialUpdates.empty()) {
      return Reject("changed material could not be mapped to a renderer slot");
    }
  }

  const bool has_tombstones = std::any_of(
      current.meshes.begin(), current.meshes.end(),
      [](const DrawMeshCPU& mesh) { return mesh.absPath.empty(); });
  if (structural_change || has_tombstones) {
    std::unordered_map<std::string, size_t> resident_slots;
    for (size_t i = 0; i < current.meshes.size(); ++i) {
      const std::string& path = current.meshes[i].absPath;
      if (path.empty()) continue;
      if (!resident_slots.emplace(path, i).second) {
        return Reject("duplicate resident mesh paths cannot be remapped");
      }
    }
    std::vector<DrawMeshCPU> remapped(current.meshes.size());
    std::vector<uint8_t> occupied(current.meshes.size(), 0);
    std::vector<DrawMeshCPU> additions;
    for (DrawMeshCPU& mesh : next->meshes) {
      const auto found = resident_slots.find(mesh.absPath);
      if (found != resident_slots.end()) {
        if (occupied[found->second]) {
          return Reject("duplicate replacement mesh paths cannot be remapped");
        }
        remapped[found->second] = std::move(mesh);
        occupied[found->second] = 1;
      } else {
        additions.push_back(std::move(mesh));
      }
    }
    size_t addition = 0;
    for (size_t i = 0; i < remapped.size() && addition < additions.size(); ++i) {
      if (occupied[i]) continue;
      remapped[i] = std::move(additions[addition++]);
      occupied[i] = 1;
    }
    while (addition < additions.size()) {
      remapped.push_back(std::move(additions[addition++]));
    }
    for (size_t i = 0; i < occupied.size(); ++i) {
      if (occupied[i]) continue;
      std::memset(remapped[i].world, 0, sizeof(remapped[i].world));
      remapped[i].world[0] = remapped[i].world[5] =
          remapped[i].world[10] = remapped[i].world[15] = 1.0f;
    }
    next->meshes = std::move(remapped);
  } else if (next->meshes.size() != current.meshes.size()) {
    return Reject("mesh slot count changed without a structural change");
  }

  plan.vertexUpdates.reserve(next->meshes.size());
  plan.worldUpdates.reserve(next->meshes.size());
  for (size_t i = 0; i < next->meshes.size(); ++i) {
    DrawMeshCPU& incoming = next->meshes[i];
    if (i >= current.meshes.size()) {
      CaptureMeshUploadIdentity(&incoming);
      plan.replacementUpdates.push_back(i);
      continue;
    }
    const DrawMeshCPU& resident = current.meshes[i];
    if ((IsDeformable(incoming) || IsDeformable(resident)) &&
        DeformationLayoutIdentity(incoming) !=
            DeformationLayoutIdentity(resident)) {
      return Reject("deformable mesh layout changed");
    }
    CaptureMeshUploadIdentity(&incoming);
    if (incoming.absPath.empty() || resident.absPath.empty()) {
      if (incoming.absPath != resident.absPath) {
        plan.replacementUpdates.push_back(i);
      }
      continue;
    }
    if (incoming.absPath != resident.absPath) {
      if (structural_change) {
        plan.replacementUpdates.push_back(i);
        continue;
      }
      return Reject("mesh order changed without a structural change");
    }
    if (resident.uploadedTopologyFingerprint == 0) {
      return Reject("mesh order changed or resident identity is unavailable");
    }
    if (incoming.uploadedTopologyFingerprint !=
            resident.uploadedTopologyFingerprint ||
        incoming.uploadedVertexCount != resident.uploadedVertexCount) {
      if (!topology_change && !structural_change) {
        return Reject("mesh topology changed unexpectedly");
      }
      plan.replacementUpdates.push_back(i);
      continue;
    }
    const bool instance_colors_changed =
        incoming.instanceColors != resident.instanceColors;
    const bool instance_opacities_changed =
        incoming.instanceOpacities != resident.instanceOpacities;
    const size_t instance_count = incoming.instanceXforms.size() / 12u;
    if (instance_colors_changed &&
        (incoming.instanceColors.size() != resident.instanceColors.size() ||
         (!incoming.instanceColors.empty() &&
          incoming.instanceColors.size() != instance_count * 3u))) {
      plan.replacementUpdates.push_back(i);
      continue;
    }
    if (instance_opacities_changed &&
        (incoming.instanceOpacities.size() != resident.instanceOpacities.size() ||
         (!incoming.instanceOpacities.empty() &&
          incoming.instanceOpacities.size() != instance_count))) {
      plan.replacementUpdates.push_back(i);
      continue;
    }
    if (instance_opacities_changed &&
        HasTranslucentInstances(incoming) !=
            HasTranslucentInstances(resident)) {
      plan.replacementUpdates.push_back(i);
      continue;
    }
    if (incoming.instanceXforms != resident.instanceXforms ||
        instance_colors_changed || instance_opacities_changed) {
      if (incoming.instanceXforms.empty() || resident.instanceXforms.empty() ||
          incoming.instanceXforms.size() != resident.instanceXforms.size()) {
        plan.replacementUpdates.push_back(i);
        continue;
      }
      plan.instanceUpdates.push_back(i);
    }
    if (incoming.uploadedVertexFingerprint !=
        resident.uploadedVertexFingerprint) {
      plan.vertexUpdates.push_back(i);
    }
    if (std::memcmp(incoming.world, resident.world,
                    sizeof(incoming.world)) != 0) {
      plan.worldUpdates.push_back(i);
    }
  }
  // One structural transaction must cover every changed slot. Promote ordinary
  // vertex/world updates to replacements so a replacement allocation failure
  // cannot follow already-published in-place writes.
  if (!plan.replacementUpdates.empty()) {
    for (size_t index : plan.instanceUpdates) {
      if (std::find(plan.replacementUpdates.begin(),
                    plan.replacementUpdates.end(), index) ==
          plan.replacementUpdates.end()) {
        plan.replacementUpdates.push_back(index);
      }
    }
    for (size_t index : plan.vertexUpdates) {
      if (std::find(plan.replacementUpdates.begin(),
                    plan.replacementUpdates.end(), index) ==
          plan.replacementUpdates.end()) {
        plan.replacementUpdates.push_back(index);
      }
    }
    for (size_t index : plan.worldUpdates) {
      if (std::find(plan.replacementUpdates.begin(),
                    plan.replacementUpdates.end(), index) ==
          plan.replacementUpdates.end()) {
        plan.replacementUpdates.push_back(index);
      }
    }
    plan.vertexUpdates.clear();
    plan.worldUpdates.clear();
    plan.instanceUpdates.clear();
  }
  if (!plan.materialUpdates.empty() &&
      (!plan.replacementUpdates.empty() || !plan.vertexUpdates.empty() ||
       !plan.worldUpdates.empty() || !plan.instanceUpdates.empty())) {
    return Reject("mixed material and mesh transaction requires full processing");
  }
  if (!plan.textureUpdates.empty() &&
      (!plan.materialUpdates.empty() || !plan.replacementUpdates.empty() ||
       !plan.vertexUpdates.empty() || !plan.worldUpdates.empty() ||
       !plan.instanceUpdates.empty())) {
    return Reject("mixed texture and scene transaction requires full processing");
  }
  for (size_t index : plan.vertexUpdates)
    plan.estimatedUploadBytes +=
        next->meshes[index].vertices.size() * sizeof(DrawVertex);
  plan.estimatedUploadBytes += plan.worldUpdates.size() * sizeof(float) * 16u;
  for (size_t index : plan.instanceUpdates)
    plan.estimatedUploadBytes +=
        (next->meshes[index].instanceXforms.size() +
         next->meshes[index].instanceColors.size() +
         next->meshes[index].instanceOpacities.size()) * sizeof(float);
  for (size_t index : plan.replacementUpdates)
    plan.estimatedUploadBytes += MeshUploadBytes(next->meshes[index]);
  for (size_t index : plan.textureUpdates)
    plan.estimatedUploadBytes += TexturePayloadBytes(next->textures[index]);
  plan.estimatedUploadBytes +=
      plan.materialUpdates.size() * sizeof(DrawMaterialCPU);
  plan.compatible = true;
  return plan;
}

}  // namespace lusdview
