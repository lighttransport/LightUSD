// SPDX-License-Identifier: Apache-2.0
// Optional GLB export stays outside the core render C translation unit so
// static consumers that never export do not link the glTF writer.
#include "c-render-internal.hh"

#include <filesystem>
#include <limits>
#include <new>
#include <utility>

#include "c-internal.hh"
#include "reader/usdz-reader.hh"
#include "tydra/gltf-export.hh"

static lightusd_status ExportGLBWithResolver(
    const lightusd_render_scene* scene, const char* source_path,
    lightusd_asset_resolver* supplied_resolver,
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

  lightusd::ResolverConfig config;
  config.allow_filesystem = false;
  lightusd::AssetResolver portable_resolver(config);
  const auto* resolver = supplied_resolver
      ? lightusd_internal::AssetResolverForLoad(supplied_resolver) : &portable_resolver;
  lightusd::tydra::GltfExportOptions options;
  options.max_output_bytes = max_bytes;
  options.fail_on_loss = strict != 0;
  if (source_path[0]) {
    options.resolver = resolver;
    options.asset_anchor = resolver->Resolve(source_path).resolved_path;
    if (std::filesystem::path(options.asset_anchor).extension() == ".usdz") {
      lightusd::USDZReader package;
      lightusd::USDZReadOptions read;
      read.max_archive_size = max_bytes;
      std::vector<uint8_t> bytes;
      std::string error;
      const bool opened = resolver->RequiresResolverReads()
          ? resolver->ReadAsset(options.asset_anchor, &bytes, &error) &&
                package.Open(bytes.data(), bytes.size(), read)
          : package.OpenFile(options.asset_anchor, read);
      if (opened) {
        const int root = package.FindRootLayer();
        if (root >= 0) {
          options.asset_anchor += "[" + package.EntryName(size_t(root)) + "]";
        }
      }
    }
  }
  const auto* data = lightusd_internal::RenderSceneForExport(scene);
  lightusd::tydra::GltfExportResult result =
      lightusd::tydra::ExportGLB(*data, options);
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

extern "C" lightusd_status lightusd_render_export_glb(
    const lightusd_render_scene* scene, const char* source_path,
    uint8_t strict, uint64_t max_output_bytes, lightusd_string** glb,
    lightusd_strlist** losses) {
  return ExportGLBWithResolver(scene, source_path, nullptr, strict,
                               max_output_bytes, glb, losses);
}
extern "C" lightusd_status lightusd_render_export_glb_with_resolver(
    const lightusd_render_scene* scene, const char* source_path,
    lightusd_asset_resolver* resolver, uint8_t strict, uint64_t max_output_bytes,
    lightusd_string** glb, lightusd_strlist** losses) {
  if (!resolver) return lightusd_internal::Fail(LIGHTUSD_ERR_INVALID_ARG, "resolver is null");
  return ExportGLBWithResolver(scene, source_path, resolver, strict,
                               max_output_bytes, glb, losses);
}
