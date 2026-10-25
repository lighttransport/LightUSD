// SPDX-License-Identifier: Apache-2.0
// Next render stream: UsdVol Volumes (resolved fields, dense voxels on
// demand) and ParticleFields (3D Gaussian splats).
#include "binding-next-common.hh"
#include "binding-next-render.hh"
#include <cstring>
#include <sstream>

namespace lightusd {
namespace web_next {

namespace {

void JsonMatrix(std::ostringstream& ss, const double m[4][4]) {
  ss << '[';
  for (int i = 0; i < 16; ++i) ss << (i ? "," : "") << m[i / 4][i % 4];
  ss << ']';
}

void JsonVec3(std::ostringstream& ss, const double v[3]) {
  ss << '[' << v[0] << ',' << v[1] << ',' << v[2] << ']';
}

}  // namespace

int RenderStream::volumesJsonCopy(uint8_t* out, uint32_t cap) const {
  if (!render_scene_valid_) return -1;
  std::ostringstream ss;
  ss.precision(17);
  ss << "{\"volumes\":[";
  for (size_t i = 0; i < render_scene_.volumes.size(); ++i) {
    const tr::RenderVolume& v = render_scene_.volumes[i];
    ss << (i ? "," : "") << "{\"primPath\":\"" << JsonEscape(v.prim_path) << "\",\"worldMatrix\":";
    JsonMatrix(ss, v.world_matrix);
    ss << ",\"materialId\":" << v.material_id << ",\"densityScale\":" << v.density_scale
       << ",\"albedo\":[" << v.albedo[0] << ',' << v.albedo[1] << ',' << v.albedo[2]
       << "],\"emission\":[" << v.emission[0] << ',' << v.emission[1] << ',' << v.emission[2]
       << "],\"emissionIntensity\":" << v.emission_intensity;
    if (v.has_extent) {
      ss << ",\"extent\":[";
      JsonVec3(ss, v.extent_min);
      ss << ',';
      JsonVec3(ss, v.extent_max);
      ss << "],\"extentIsWorld\":" << (v.extent_is_world ? "true" : "false");
    }
    ss << ",\"fields\":[";
    for (size_t f = 0; f < v.fields.size(); ++f) {
      const ::lightusd::tydra::vol::ResolvedField& r = v.fields[f].field;
      ss << (f ? "," : "") << "{\"binding\":\"" << JsonEscape(v.fields[f].binding)
         << "\",\"fieldPrim\":\"" << JsonEscape(r.request.field_prim) << "\",\"filePath\":\""
         << JsonEscape(r.request.file_path) << "\",\"gridName\":\"" << JsonEscape(r.grid.name)
         << "\",\"gridClass\":\"" << JsonEscape(r.grid_class) << "\",\"levelSet\":"
         << (r.level_set ? "true" : "false") << ",\"valueType\":\""
         << JsonEscape(r.value_type) << "\",\"components\":" << r.components
         << ",\"sourceIndex\":" << r.grid.source_index << ",\"indexToWorld\":";
      JsonMatrix(ss, r.index_to_world);
      if (r.has_bounds) {
        ss << ",\"worldBounds\":[";
        JsonVec3(ss, r.world_min);
        ss << ',';
        JsonVec3(ss, r.world_max);
        ss << ']';
      }
      ss << '}';
    }
    ss << "]}";
  }
  ss << "],\"particleFields\":[";
  for (size_t i = 0; i < render_scene_.particle_fields.size(); ++i) {
    const tr::RenderParticleField& p = render_scene_.particle_fields[i];
    ss << (i ? "," : "") << "{\"primPath\":\"" << JsonEscape(p.prim_path) << "\",\"typeName\":\""
       << JsonEscape(p.type_name) << "\",\"kernel\":\"" << JsonEscape(p.kernel)
       << "\",\"projectionModeHint\":\"" << JsonEscape(p.projection_mode_hint)
       << "\",\"sortingModeHint\":\"" << JsonEscape(p.sorting_mode_hint) << "\",\"count\":" << p.count
       << ",\"shDegree\":" << p.sh_degree << ",\"worldMatrix\":";
    JsonMatrix(ss, p.world_matrix);
    ss << '}';
  }
  ss << "]}";
  const std::string s = ss.str();
  if (s.size() > 0x7fffffffu) return -1;
  const int required = static_cast<int>(s.size());
  if (out && cap >= s.size()) std::memcpy(out, s.data(), s.size());
  return required;
}

uintptr_t RenderStream::volumeDense(int volume, int field, uint32_t max_voxels, uint32_t flags,
                                    lightusd_next_volume_dense_info* info) const {
  if (!render_scene_valid_ || !info || !render_scene_.volume_cache || volume < 0 ||
      static_cast<size_t>(volume) >= render_scene_.volumes.size())
    return 0;
  const tr::RenderVolume& v = render_scene_.volumes[static_cast<size_t>(volume)];
  if (field < 0 || static_cast<size_t>(field) >= v.fields.size()) return 0;
  const ::lightusd::tydra::vol::ResolvedField& r = v.fields[static_cast<size_t>(field)].field;
  ::lightusd::usdVol::VDBReadOptions o;
  o.max_voxels = max_voxels;
  o.downsample = (flags & 1u) != 0;
  o.vector_components = (flags & 2u) != 0;
  std::string warn, err;
  const ::lightusd::usdVol::VDBGrid* g = render_scene_.volume_cache->Dense(r, o, &warn, &err);
  if (!g) return 0;
  std::memset(info, 0, sizeof(*info));
  for (int a = 0; a < 3; ++a) {
    info->dims[a] = g->dim[a];
    info->index_origin[a] = g->origin[a];
  }
  info->stride = g->stride;
  info->components = g->components;
  info->background = g->background;
  // cell -> index (stride, origin, box center) composed with index -> world.
  const double s = double(g->stride), off = 0.5 * double(g->stride - 1);
  const double c2i[4][4] = {{s, 0, 0, 0}, {0, s, 0, 0}, {0, 0, s, 0},
                            {g->origin[0] + off, g->origin[1] + off, g->origin[2] + off, 1}};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double acc = 0;
      for (int k = 0; k < 4; ++k) acc += c2i[i][k] * r.index_to_world[k][j];
      info->cell_to_world[4 * i + j] = acc;
    }
  return reinterpret_cast<uintptr_t>(g->data.data());
}

uintptr_t RenderStream::volumeNanovdb(int volume, int field,
                                      lightusd_next_volume_nanovdb_info* info) const {
  if (!render_scene_valid_ || !info || !render_scene_.volume_cache || volume < 0 ||
      static_cast<size_t>(volume) >= render_scene_.volumes.size())
    return 0;
  const tr::RenderVolume& v = render_scene_.volumes[static_cast<size_t>(volume)];
  if (field < 0 || static_cast<size_t>(field) >= v.fields.size()) return 0;
  const ::lightusd::tydra::vol::ResolvedField& r = v.fields[static_cast<size_t>(field)].field;
  ::lightusd::usdVol::VDBNanoVDBInfo converted;
  std::string err;
  const std::vector<uint8_t>* buffer =
      render_scene_.volume_cache->NanoVDB(r, &converted, &err);
  if (!buffer) return 0;
  std::memset(info, 0, sizeof(*info));
  info->size = static_cast<uint64_t>(buffer->size());
  info->grid_type = static_cast<int32_t>(converted.grid_type);
  info->components = static_cast<int32_t>(converted.components);
  std::memcpy(info->index_to_world, converted.index_to_world,
              sizeof(info->index_to_world));
  return reinterpret_cast<uintptr_t>(buffer->data());
}

uintptr_t RenderStream::volumeSample(int volume, int field, const double* points, uint32_t count,
                                     uint32_t flags,
                                     lightusd_next_volume_samples_info* info) const {
  if (!render_scene_valid_ || !info || !render_scene_.volume_cache || volume < 0 ||
      static_cast<size_t>(volume) >= render_scene_.volumes.size() ||
      (count && !points))
    return 0;
  const tr::RenderVolume& v = render_scene_.volumes[static_cast<size_t>(volume)];
  if (field < 0 || static_cast<size_t>(field) >= v.fields.size()) return 0;
  const ::lightusd::tydra::vol::ResolvedField& r = v.fields[static_cast<size_t>(field)].field;
  int components = 0;
  std::string err;
  if (!render_scene_.volume_cache->SampleWorld(r, points, size_t(count), (flags & 1u) != 0,
                                               &volume_sample_scratch_, &components, &err))
    return 0;
  info->count = count;
  info->components = static_cast<uint32_t>(components > 0 ? components : 1);
  return reinterpret_cast<uintptr_t>(volume_sample_scratch_.data());
}

uintptr_t RenderStream::particleArray(int id, uint8_t which, uint32_t* float_count) const {
  if (!render_scene_valid_ || !float_count || id < 0 ||
      static_cast<size_t>(id) >= render_scene_.particle_fields.size())
    return 0;
  const tr::RenderParticleField& p = render_scene_.particle_fields[static_cast<size_t>(id)];
  const std::vector<float>* a = nullptr;
  switch (which) {
    case 0: a = &p.positions; break;
    case 1: a = &p.orientations; break;
    case 2: a = &p.scales; break;
    case 3: a = &p.opacities; break;
    case 4: a = &p.sh_coefficients; break;
    default: return 0;
  }
  *float_count = static_cast<uint32_t>(a->size());
  return reinterpret_cast<uintptr_t>(a->data());
}

}  // namespace web_next
}  // namespace lightusd
