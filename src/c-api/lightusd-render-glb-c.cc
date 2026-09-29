// SPDX-License-Identifier: Apache-2.0
// Optional GLB export stays outside the core render C translation unit so
// static consumers that never export do not link the glTF writer.
#include "c-render-internal.hh"

#include <filesystem>
#include <limits>
#include <new>
#include <utility>

#include "c-internal.hh"
#include "next/reader/usdz-reader.hh"
#include "tydra/next/gltf-export.hh"

extern "C" lightusd_status lightusd_render_export_glb(
    const lightusd_render_scene* scene, const char* source_path,
    uint8_t strict, uint64_t max_output_bytes, lightusd_string** glb,
    lightusd_strlist** losses) {
  using lightusd_internal::Fail;
  if (!scene || !source_path || !glb || !losses) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid GLB export arguments");
  }
  *glb = nullptr;
  *losses = nullptr;
  if (max_output_bytes == 0) {
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "invalid GLB output limit");
  }
  size_t max_bytes = (std::numeric_limits<size_t>::max)();
  if (max_output_bytes != LIGHTUSD_LIMIT_UNLIMITED) {
#if SIZE_MAX < UINT64_MAX
    if (max_output_bytes > SIZE_MAX)
      return Fail(LIGHTUSD_ERR_OVERFLOW, "GLB output limit overflow");
#endif
    max_bytes = static_cast<size_t>(max_output_bytes);
  }

  lightusd::next::AssetResolver resolver;
  lightusd::tydra::next::GltfExportOptions options;
  options.max_output_bytes = max_bytes;
  options.fail_on_loss = strict != 0;
  if (source_path[0]) {
    options.resolver = &resolver;
    options.asset_anchor = resolver.Resolve(source_path).resolved_path;
    if (std::filesystem::path(options.asset_anchor).extension() == ".usdz") {
      lightusd::next::USDZReader package;
      lightusd::next::USDZReadOptions read;
      read.max_archive_size = max_bytes;
      if (package.OpenFile(options.asset_anchor, read)) {
        const int root = package.FindRootLayer();
        if (root >= 0) {
          options.asset_anchor += "[" + package.EntryName(size_t(root)) + "]";
        }
      }
    }
  }
  const auto* data = lightusd_internal::RenderSceneForExport(scene);
  lightusd::tydra::next::GltfExportResult result =
      lightusd::tydra::next::ExportGLB(*data, options);
  lightusd_strlist* loss_list = new (std::nothrow) lightusd_strlist();
  if (!loss_list) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "GLB loss list alloc failed");
  loss_list->items = std::move(result.losses);
  const auto* warnings = lightusd_internal::RenderWarningsForExport(scene);
  loss_list->items.insert(loss_list->items.end(), warnings->begin(), warnings->end());
  *losses = loss_list;
  if (!result.success) {
    return Fail(LIGHTUSD_ERR_UNSUPPORTED,
                result.error.empty() ? "GLB export failed" : result.error);
  }
  if (strict && !loss_list->items.empty()) {
    return Fail(LIGHTUSD_ERR_UNSUPPORTED,
                "strict export refuses conversion losses");
  }
  lightusd_string* bytes = new (std::nothrow) lightusd_string();
  if (!bytes) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "GLB alloc failed");
  bytes->s.assign(reinterpret_cast<const char*>(result.glb.data()),
                  result.glb.size());
  *glb = bytes;
  return LIGHTUSD_OK;
}
