// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "tydra/next/scene-access.hh"
namespace lightusd {
namespace web_next {
namespace {
tr::Interpolation ParseMeshPrimvarInterpolation(const std::string& name) {
  if (name == "constant") return tr::Interpolation::Constant;
  if (name == "uniform") return tr::Interpolation::Uniform;
  if (name == "faceVarying") return tr::Interpolation::FaceVarying;
  if (name == "varying") return tr::Interpolation::Varying;
  return tr::Interpolation::Vertex;
}
}  // namespace

const std::vector<RenderStream::MeshOnlyPrimvar>*
RenderStream::meshOnlyPrimvars_(int mesh_id) const {
  if (!mesh_only_ || !loaded_ || mesh_id < 0 || mesh_id >= meshCount())
    return nullptr;
  int source_index = -1;
  if (mesh_merge_) {
    if (static_cast<size_t>(mesh_id) < outputs_.size()) {
      const OutputMesh& output = outputs_[static_cast<size_t>(mesh_id)];
      if (!output.merged) source_index = output.source_index;
    }
  } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
    source_index = mesh_id;
  }
  return sourcePrimvars_(source_index);
}

const std::vector<RenderStream::MeshOnlyPrimvar>*
RenderStream::sourcePrimvars_(int source_index) const {
  static const std::vector<MeshOnlyPrimvar> empty;
  if (source_index < 0 || static_cast<size_t>(source_index) >= meshes_.size())
    return &empty;
  const auto found = source_primvars_.find(source_index);
  if (found != source_primvars_.end()) return &found->second;
  auto inserted = source_primvars_.emplace(
      source_index, std::vector<MeshOnlyPrimvar>{});
  std::vector<MeshOnlyPrimvar>& catalog = inserted.first->second;
  const lightusd::next::UsdPrim& prim =
      meshes_[static_cast<size_t>(source_index)].GetPrim();
  const std::vector<tr::Primvar> authored = tr::GetPrimvars(prim);
  if (authored.empty()) return &catalog;
  const tr::MeshConfig defaults;
  std::string uv_base;
  for (const std::string& candidate : defaults.uv_primvar_names) {
    for (const tr::Primvar& pv : authored) {
      if (pv.name == candidate) { uv_base = candidate; break; }
    }
    if (!uv_base.empty()) break;
  }
  if (uv_base.empty()) {
    uv_base = defaults.uv_primvar_names.empty()
                  ? "st" : defaults.uv_primvar_names.front();
  }
  auto is_float2 = [&](const std::string& name) {
    for (const tr::Primvar& pv : authored) {
      if (pv.name == name && pv.value && pv.value->is_array())
        return lightusd::next::GetComponentCount(pv.value->type_id()) == 2;
    }
    return false;
  };
  std::string uv_second;
  if (is_float2(uv_base + "1")) uv_second = uv_base + "1";
  if (uv_second.empty()) {
    for (const std::string& candidate : defaults.uv_primvar_names) {
      if (candidate != uv_base && is_float2(candidate)) {
        uv_second = candidate;
        break;
      }
    }
  }
  if (uv_second.empty()) {
    for (const tr::Primvar& pv : authored) {
      if (pv.name == uv_base || pv.name.rfind("skel:", 0) == 0 ||
          !pv.value || !pv.value->is_array() ||
          lightusd::next::GetComponentCount(pv.value->type_id()) != 2) continue;
      if (uv_second.empty() || pv.name < uv_second) uv_second = pv.name;
    }
  }
  const bool has_candidate = std::any_of(
      authored.begin(), authored.end(), [&](const tr::Primvar& pv) {
        return pv.value && pv.value->is_array() && pv.name != uv_base &&
            (uv_second.empty() || pv.name != uv_second) &&
            pv.name != "displayColor" && pv.name != "displayOpacity" &&
            pv.name != "normals" && pv.name.rfind("skel:", 0) != 0;
      });
  if (!has_candidate) return &catalog;
  auto array_size = [&](const char* name, size_t divisor) -> size_t {
    const lightusd::next::Value* value = prim.GetPropertyValue(name);
    if (!value) return 0;
    if (const std::vector<float>* floats = value->as_float_array())
      return floats->size() / divisor;
    if (const std::vector<int32_t>* ints = value->as_int_array())
      return ints->size() / divisor;
    return 0;
  };
  const size_t point_count = array_size("points", 3);
  const size_t face_count = array_size("faceVertexCounts", 1);
  const size_t corner_count = array_size("faceVertexIndices", 1);
  for (const tr::Primvar& pv : authored) {
    if (!pv.value || !pv.value->is_array() || pv.name == uv_base ||
        (!uv_second.empty() && pv.name == uv_second) ||
        pv.name == "displayColor" || pv.name == "displayOpacity" ||
        pv.name == "normals" || pv.name.rfind("skel:", 0) == 0) continue;
    const std::vector<float>* floats = pv.value->as_float_array();
    const std::vector<double>* doubles = pv.value->as_double_array();
    const std::vector<int32_t>* ints = pv.value->as_int_array();
    if (!floats && !doubles && !ints) continue;
    const uint32_t components = ints ? 1u : static_cast<uint32_t>(
        lightusd::next::GetComponentCount(pv.value->type_id()));
    if (components != 1 && components != 2 && components != 3 &&
        components != 4 && components != 9 && components != 16) continue;
    const size_t scalars = floats ? floats->size()
                           : doubles ? doubles->size() : ints->size();
    if (!scalars || scalars % components) continue;
    const size_t elements = scalars / components;
    bool valid_indices = true;
    for (int32_t index : pv.indices()) {
      if (index < 0 || static_cast<size_t>(index) >= elements) {
        valid_indices = false;
        break;
      }
    }
    if (!valid_indices) continue;
    MeshOnlyPrimvar item;
    item.name = pv.name;
    item.value = pv.value;
    item.indices = pv.indices_view;
    item.components = components;
    item.scalar_count = scalars;
    const lightusd::next::PropMeta* property_meta =
        prim.GetPropertyMeta("primvars:" + pv.name);
    if (property_meta && property_meta->elementSize > 0) {
      item.element_size = property_meta->elementSize;
    }
    item.format = ints ? tr::VertexFormat::Int
                  : components == 1 ? tr::VertexFormat::Float
                  : components == 2 ? tr::VertexFormat::Vec2
                  : components == 3 ? tr::VertexFormat::Vec3
                  : components == 4 ? tr::VertexFormat::Vec4
                  : components == 9 ? tr::VertexFormat::Matrix33
                                    : tr::VertexFormat::Matrix44;
    const size_t logical_count = pv.indices().empty()
        ? elements : pv.indices().size();
    if (!pv.interpolation_authored && logical_count > 1) {
      if (logical_count == point_count) item.interpolation = tr::Interpolation::Vertex;
      else if (logical_count == corner_count)
        item.interpolation = tr::Interpolation::FaceVarying;
      else if (logical_count == face_count)
        item.interpolation = tr::Interpolation::Uniform;
      else item.interpolation = ParseMeshPrimvarInterpolation(pv.interpolation);
    } else {
      item.interpolation = ParseMeshPrimvarInterpolation(pv.interpolation);
    }
    catalog.push_back(std::move(item));
  }
  return &catalog;
}

int RenderStream::meshPrimvarCount(int mesh_id) const {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return -1;
    const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
    const size_t count = mesh ? mesh->primvars.size() :
        mesh_only_ ? meshOnlyPrimvars_(mesh_id)->size() : 0;
    return count > static_cast<size_t>((std::numeric_limits<int>::max)())
               ? (std::numeric_limits<int>::max)()
               : static_cast<int>(count);
}

int RenderStream::meshPrimvarField(int mesh_id, int primvar_id, uint8_t field) const {
    const int count = meshPrimvarCount(mesh_id);
    if (count < 0 || primvar_id < 0 || primvar_id >= count || field > 4) return -1;
    const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
    if (!mesh && mesh_only_) {
      const MeshOnlyPrimvar& pv =
          (*meshOnlyPrimvars_(mesh_id))[static_cast<size_t>(primvar_id)];
      switch (field) {
        case 0: return static_cast<int>(pv.format);
        case 1: return static_cast<int>(pv.interpolation);
        case 2: return pv.indices && !pv.indices->empty() ? 1 : 0;
        case 3: return pv.scalar_count / pv.components >
            static_cast<size_t>((std::numeric_limits<int>::max)()) ? -1 :
            static_cast<int>(pv.scalar_count / pv.components);
        case 4: return pv.element_size;
      }
    }
    if (!mesh) return -1;
    const auto& primvar = mesh->primvars[static_cast<size_t>(primvar_id)];
    switch (field) {
      case 0: return static_cast<int>(primvar.format);
      case 1: return static_cast<int>(primvar.interpolation);
      case 2: return primvar.has_indices() ? 1 : 0;
      case 3: {
        const size_t elements = primvar.element_count();
        return elements > static_cast<size_t>((std::numeric_limits<int>::max)())
                   ? (std::numeric_limits<int>::max)()
                   : static_cast<int>(elements);
      }
      case 4: {
        const lightusd::next::UsdPrim prim =
            stage_.GetPrimAtPath(mesh->prim_path);
        const lightusd::next::PropMeta* meta =
            prim.GetPropertyMeta("primvars:" + primvar.name);
        return meta && meta->elementSize > 0 ? meta->elementSize : 1;
      }
      default: return -1;
    }
}

int RenderStream::meshPrimvarNameCopy(int mesh_id, int primvar_id,
                                      uint8_t* out, uint32_t cap) const {
    const int count = meshPrimvarCount(mesh_id);
    if (count < 0 || primvar_id < 0 || primvar_id >= count) return -1;
    const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
    if (!mesh && !mesh_only_) return -1;
    const std::string& name = mesh
        ? mesh->primvars[static_cast<size_t>(primvar_id)].name
        : (*meshOnlyPrimvars_(mesh_id))[static_cast<size_t>(primvar_id)].name;
    if (name.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(name.size());
    if (!out || cap < name.size() || name.empty()) return required;
    std::memcpy(out, name.data(), name.size());
    return required;
}

int RenderStream::meshPrimvarBufferCopy(int mesh_id, int primvar_id, uint8_t kind,
                                        uint8_t* out, uint32_t cap) const {
    const int count = meshPrimvarCount(mesh_id);
    if (count < 0 || primvar_id < 0 || primvar_id >= count || kind > 1) return -1;
    const tr::RenderMesh* mesh = sourceRenderMesh_(mesh_id);
    if (!mesh && mesh_only_) {
      const MeshOnlyPrimvar& pv =
          (*meshOnlyPrimvars_(mesh_id))[static_cast<size_t>(primvar_id)];
      const size_t count = kind == 1
          ? (pv.indices ? pv.indices->size() : 0) : pv.scalar_count;
      if (count > static_cast<size_t>((std::numeric_limits<int>::max)()) /
                      sizeof(uint32_t)) return -1;
      const int required = static_cast<int>(count * sizeof(uint32_t));
      if (!out || cap < static_cast<uint32_t>(required) || !required)
        return required;
      if (kind == 1) {
        std::memcpy(out, pv.indices->data(), static_cast<size_t>(required));
      } else if (const std::vector<float>* values = pv.value->as_float_array()) {
        std::memcpy(out, values->data(), static_cast<size_t>(required));
      } else if (const std::vector<int32_t>* values = pv.value->as_int_array()) {
        std::memcpy(out, values->data(), static_cast<size_t>(required));
      } else if (const std::vector<double>* values = pv.value->as_double_array()) {
        for (size_t i = 0; i < count; ++i) {
          const float value = static_cast<float>((*values)[i]);
          std::memcpy(out + i * sizeof(float), &value, sizeof(float));
        }
      } else {
        return -1;
      }
      return required;
    }
    if (!mesh) return -1;
    const auto& primvar = mesh->primvars[static_cast<size_t>(primvar_id)];
    const void* contiguous = nullptr;
    const tr::FloatChunked* floats = nullptr;
    const tr::Int32Chunked* ints = nullptr;
    const tr::UInt32Chunked* uints = nullptr;
    size_t elements = 0;
    size_t element_size = 0;
    if (kind == 1) {
      uints = &primvar.indices;
      elements = uints->size();
      element_size = sizeof(uint32_t);
      if (uints->is_contiguous() && elements) contiguous = uints->chunk_data(0);
    } else if (!primvar.float_data.empty()) {
      floats = &primvar.float_data;
      elements = floats->size();
      element_size = sizeof(float);
      if (floats->is_contiguous() && elements) contiguous = floats->chunk_data(0);
    } else if (!primvar.int_data.empty()) {
      ints = &primvar.int_data;
      elements = ints->size();
      element_size = sizeof(int32_t);
      if (ints->is_contiguous() && elements) contiguous = ints->chunk_data(0);
    } else {
      uints = &primvar.uint_data;
      elements = uints->size();
      element_size = sizeof(uint32_t);
      if (uints->is_contiguous() && elements) contiguous = uints->chunk_data(0);
    }
    const size_t bytes = elements * element_size;
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) return -1;
    const int required = static_cast<int>(bytes);
    if (!out || cap < bytes || bytes == 0) return required;
    if (contiguous) {
      std::memcpy(out, contiguous, bytes);
    } else if (floats) {
      for (size_t i = 0; i < elements; ++i)
        std::memcpy(out + i * element_size, &(*floats)[i], element_size);
    } else if (ints) {
      for (size_t i = 0; i < elements; ++i)
        std::memcpy(out + i * element_size, &(*ints)[i], element_size);
    } else {
      for (size_t i = 0; i < elements; ++i)
        std::memcpy(out + i * element_size, &(*uints)[i], element_size);
    }
    return required;
}

}  // namespace web_next
}  // namespace lightusd
