// SPDX-License-Identifier: Apache-2.0
// lusdrender — the legacy eager loader (tydra RenderScene path): geometry/mesh
// collection, the direct-primitive builder, legacy camera framing + the animated-
// attribute eval helpers, plus the shared utilities used by both loader paths
// (WorkerThreadCount, PurposeVisible, MakeCameraFrame, ...).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>

#include "asset-resolution.hh"
#include "composition.hh"
#include "image-loader.hh"
#include "mmap-array-ref.hh"
#include "tsd/tinysubdiv.hh"
#include "tydra/attribute-eval.hh"
#include "usdGeom.hh"
#include "lusdr_context.hh"

namespace lusdr {

// ===========================================================================
// Legacy stage load WITH composition.
//
// `lightusd::LoadUSDFromFile` parses a single layer; it does NOT expand
// composition arcs. The legacy path used it directly, so every prim contributed
// by a reference / payload / sublayer / inherit / variant simply did not exist:
// a Material referenced from a look layer was missing (the mesh fell back to the
// default gray, dropping its textures) and payload-gated geometry rendered as
// "no renderable geometry". Compose to a fixed point first, mirroring the
// viewer's loader (examples/lusdview/scene_loader.cc ComposeToFixedPoint).
// ===========================================================================

namespace {

bool LayerHasCompositionArcs(const lightusd::Layer &layer) {
  return !layer.metas().subLayers.empty() ||
         layer.check_unresolved_references() ||
         layer.check_unresolved_payload() ||
         layer.check_unresolved_inherits() ||
         layer.check_unresolved_variant() ||
         layer.check_unresolved_specializes();
}

// LIVRPS to a fixed point. lusdrender's legacy path is eager: every payload and
// reference loads (no deferred-arc policy — that lives on the `next` path).
bool ComposeToFixedPoint(lightusd::AssetResolutionResolver &resolver,
                         lightusd::Layer &&src, lightusd::Layer *composed,
                         std::string *warn, std::string *err) {
  lightusd::Layer work = std::move(src);

  // A layer-relative asset path may climb with '..' — real layer stacks reach
  // sibling asset directories that way (ALab's looks live several levels up), and
  // rejecting them fails composition outright. Matches lusdview's default and the
  // `next` resolver's `allow_parent_paths`.
  lightusd::ReferencesCompositionOptions ref_opts;
  ref_opts.allow_parent_relative_paths = true;
  lightusd::PayloadCompositionOptions pl_opts;
  pl_opts.allow_parent_relative_paths = true;

  constexpr int kMaxIteration = 64;
  for (int i = 0; i < kMaxIteration; i++) {
    bool has_unresolved = false;

    if (work.check_unresolved_references()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeReferences(resolver, work, &tmp, warn, err,
                                         ref_opts)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (work.check_unresolved_payload()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositePayload(resolver, work, &tmp, warn, err,
                                      pl_opts)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (work.check_unresolved_inherits()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeInherits(work, &tmp, warn, err)) return false;
      work = std::move(tmp);
    }

    if (work.check_unresolved_variant()) {
      has_unresolved = true;
      // Resolve variants only once references + payloads have settled (AOUSD
      // Core Spec 10.3.2.5): a variant's CONTENT often arrives THROUGH an arc,
      // so selecting early picks an empty option and the geometry is lost.
      const bool arcs_settled = !work.check_unresolved_references() &&
                                !work.check_unresolved_payload();
      if (arcs_settled) {
        lightusd::Layer tmp;
        if (!lightusd::CompositeVariant(work, &tmp, warn, err)) return false;
        work = std::move(tmp);
      }
    }

    if (work.check_unresolved_specializes()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeSpecializes(work, &tmp, warn, err)) return false;
      work = std::move(tmp);
    }

    if (!has_unresolved) {
      *composed = std::move(work);
      return true;
    }
  }

  if (err) *err += "Composition did not converge before the iteration limit.\n";
  return false;
}

}  // namespace

bool LoadStageComposedLegacy(const std::string &path,
                             const lightusd::USDLoadOptions &load_options,
                             lightusd::Stage *stage, std::string *warn,
                             std::string *err) {
  if (!stage) return false;

  // .usdz keeps the direct parser path (its arcs resolve inside the archive, and
  // the zero-copy/asset handling here is the loader's own).
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  const bool is_usdz =
      lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".usdz") == 0;
  if (is_usdz) {
    return lightusd::LoadUSDFromFile(path, stage, warn, err, load_options);
  }

  lightusd::Layer root;
  std::string lwarn, lerr;
  if (!lightusd::LoadLayerFromFile(path, &root, &lwarn, &lerr)) {
    // Not layer-loadable (or an unsupported flavor) — fall back to the direct
    // parser rather than failing the render outright.
    return lightusd::LoadUSDFromFile(path, stage, warn, err, load_options);
  }

  if (!LayerHasCompositionArcs(root)) {
    // No arcs: keep the direct parser path. LayerToStage drops some less-common
    // concrete schemas (e.g. NurbsPatch) and the direct load retains zero-copy
    // USDC storage, so composing here would only lose things.
    return lightusd::LoadUSDFromFile(path, stage, warn, err, load_options);
  }

  if (warn) *warn += lwarn;

  lightusd::AssetResolutionResolver resolver;
  resolver.set_search_paths({DirName(path)});

  if (!root.metas().subLayers.empty()) {
    lightusd::SublayersCompositionOptions sl_opts;
    sl_opts.allow_parent_relative_paths = true;
    lightusd::Layer tmp;
    if (!lightusd::CompositeSublayers(resolver, root, &tmp, warn, err,
                                      sl_opts)) {
      return false;
    }
    root = std::move(tmp);
  }

  lightusd::Layer composed;
  if (!ComposeToFixedPoint(resolver, std::move(root), &composed, warn, err)) {
    return false;
  }
  return lightusd::LayerToStage(std::move(composed), stage, warn, err);
}


using lightusd::value::color3f;
using lightusd::value::float3;
using lightusd::value::matrix4d;
using lightusd::tydra::Node;
using lightusd::tydra::NodeType;
using lightusd::tydra::RenderCamera;
using lightusd::tydra::RenderLight;
using lightusd::tydra::RenderMaterial;
using lightusd::tydra::RenderMesh;
using lightusd::tydra::RenderScene;

// ===========================================================================
// Memory budget / manager.
//
// A process-wide cap that keeps lusdrender from being OOM-killed on huge scenes
// (e.g. fully instance-expanded Caldera maps). The cap defaults to
// min(32 GiB, 0.5 * system MemAvailable) and is overridable with -maxMem <GiB>.
// It is enforced two ways:
//   1. Phase guards (GuardPhase) check the live process RSS + an estimate of the
//      next phase's allocation and abort cleanly BEFORE the allocation that would
//      bust the cap (covers composition + LightRT BVH, which allocate outside our
//      allocator).
//   2. The pool allocator (PoolAlloc, below) accounts every render-buffer byte
//      into `tracked_` and throws std::bad_alloc when our allocations would push
//      RSS past the cap (covers the triangle stream precisely, mid-flight).
// ===========================================================================

// ===========================================================================
// Legacy texture bridge.
//
// tydra's eager converter already RESOLVES and DECODES every UsdUVTexture into
// RenderScene::{textures,images,buffers} — this path simply never consumed it:
// materials were flattened to a constant `base_color` and every `tex_id` stayed
// -1, so any .usda/.usdz (which does not route to the `next` path) rendered
// untextured. Convert tydra's decoded images into the renderer's `Texture` and
// bind them per material, exactly as the `next` path does.
// ===========================================================================

namespace {

WrapMode ToTusdrWrapLegacy(lightusd::tydra::UVTexture::WrapMode m) {
  using W = lightusd::tydra::UVTexture::WrapMode;
  switch (m) {
    case W::REPEAT: return WrapMode::Repeat;
    case W::MIRROR: return WrapMode::Mirror;
    case W::CLAMP_TO_BORDER: return WrapMode::Black;
    case W::CLAMP_TO_EDGE: break;
  }
  return WrapMode::Clamp;
}

// Source channel of a scalar (roughness/metallic/occlusion/opacity) texture:
// 0=r, 1=g, 2=b, 3=a — matching TriInfo::{rough,metal,occ,opacity}_ch.
uint8_t ScalarChannel(const lightusd::tydra::UVTexture &tex) {
  using C = lightusd::tydra::UVTexture::Channel;
  switch (tex.connectedOutputChannel) {
    case C::G: return 1;
    case C::B: return 2;
    case C::A: return 3;
    case C::R:
    case C::RGB:
    case C::RGBA: break;
  }
  return 0;
}

// Decode a tydra TextureImage's buffer to RGBA8. Mirrors the viewer's decoder
// (examples/lusdview/mesh_build.cc DecodeToRGBA8): tydra may hand back UInt8,
// UInt16, Half or Float texels depending on the source image.
bool DecodeLegacyImageRGBA8(const RenderScene &scene,
                            const lightusd::tydra::TextureImage &img,
                            Texture *out) {
  if (img.buffer_id < 0 || size_t(img.buffer_id) >= scene.buffers.size()) {
    return false;
  }
  if (!img.decoded || img.width <= 0 || img.height <= 0 || img.channels <= 0) {
    return false;
  }
  const lightusd::tydra::BufferData &buf =
      scene.buffers[size_t(img.buffer_id)];
  const size_t w = size_t(img.width);
  const size_t h = size_t(img.height);
  const size_t ch = size_t(img.channels);
  const size_t npix = w * h;
  if (npix == 0) return false;

  out->width = img.width;
  out->height = img.height;
  out->channels = 4;
  out->pixels.assign(npix * 4, 255);

  auto clamp8 = [](float v) -> uint8_t {
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return uint8_t(v + 0.5f);
  };
  // Replicate a 1-channel source across RGB (gray), and treat a 2-channel one as
  // gray+alpha — the same expansion the viewer does.
  auto store = [&](size_t i, float c0, float c1, float c2, float c3) {
    if (ch == 1) { c1 = c0; c2 = c0; c3 = 255.0f; }
    else if (ch == 2) { c3 = c1; c1 = c0; c2 = c0; }
    out->pixels[i * 4 + 0] = clamp8(c0);
    out->pixels[i * 4 + 1] = clamp8(c1);
    out->pixels[i * 4 + 2] = clamp8(c2);
    out->pixels[i * 4 + 3] = clamp8(c3);
  };
  auto at = [&](const auto *p, size_t i, size_t c, float scale) -> float {
    return c < ch ? float(p[i * ch + c]) * scale : 0.0f;
  };

  using CT = lightusd::tydra::ComponentType;
  switch (img.texelComponentType) {
    case CT::UInt8: {
      if (buf.data.size() < npix * ch) return false;
      const uint8_t *p = buf.data.data();
      for (size_t i = 0; i < npix; ++i) {
        store(i, at(p, i, 0, 1.0f), at(p, i, 1, 1.0f), at(p, i, 2, 1.0f),
              ch > 3 ? at(p, i, 3, 1.0f) : 255.0f);
      }
      return true;
    }
    case CT::UInt16: {
      if (buf.data.size() < npix * ch * sizeof(uint16_t)) return false;
      const uint16_t *p =
          reinterpret_cast<const uint16_t *>(buf.data.data());
      constexpr float k = 255.0f / 65535.0f;
      for (size_t i = 0; i < npix; ++i) {
        store(i, at(p, i, 0, k), at(p, i, 1, k), at(p, i, 2, k),
              ch > 3 ? at(p, i, 3, k) : 255.0f);
      }
      return true;
    }
    case CT::Float: {
      if (buf.data.size() < npix * ch * sizeof(float)) return false;
      const float *p = reinterpret_cast<const float *>(buf.data.data());
      for (size_t i = 0; i < npix; ++i) {
        store(i, at(p, i, 0, 255.0f), at(p, i, 1, 255.0f), at(p, i, 2, 255.0f),
              ch > 3 ? at(p, i, 3, 255.0f) : 255.0f);
      }
      return true;
    }
    default:
      // Half and the integer signed types are not produced by tydra's decoder
      // for texture images today; skip rather than misread the buffer.
      return false;
  }
}

bool SetLegacyTextureColorTransform(lightusd::tydra::ColorSpace color_space,
                                    Texture *texture) {
  if (!texture) return false;
  const char *source = nullptr;
  using LegacyColorSpace = lightusd::tydra::ColorSpace;
  switch (color_space) {
    case LegacyColorSpace::sRGB:
    case LegacyColorSpace::sRGB_Texture:
      source = "srgb_rec709_scene";
      break;
    case LegacyColorSpace::Lin_sRGB:
    case LegacyColorSpace::Lin_Rec709:
      source = "lin_rec709_scene";
      break;
    case LegacyColorSpace::g22_Rec709:
      source = "g22_rec709_scene";
      break;
    case LegacyColorSpace::g18_Rec709:
      source = "g18_rec709_scene";
      break;
    case LegacyColorSpace::Raw:
      source = "raw";
      break;
    case LegacyColorSpace::Lin_ACEScg:
      source = "lin_ap1_scene";
      break;
    case LegacyColorSpace::ACES2065_1:
      source = "lin_ap0_scene";
      break;
    case LegacyColorSpace::Lin_Rec2020:
      source = "lin_rec2020_scene";
      break;
    case LegacyColorSpace::Lin_DisplayP3:
      source = "lin_p3d65_scene";
      break;
    case LegacyColorSpace::sRGB_DisplayP3:
      source = "srgb_p3d65_scene";
      break;
    default:
      return false;
  }
  lightusd::color::ColorSpaceDesc source_desc;
  lightusd::color::ColorSpaceDesc display_desc;
  if (!lightusd::color::GetBuiltinColorSpace(source, &source_desc) ||
      !lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                             &display_desc) ||
      !lightusd::color::BuildColorTransform(
          source_desc, display_desc, &texture->color_transform)) {
    return false;
  }
  // The full transform owns transfer decoding, including bypassed linear/data
  // cases. Avoid applying the compatibility sRGB decoder a second time.
  texture->srgb = false;
  return true;
}

}  // namespace

std::vector<LegacyMaterialTex> BuildLegacyTextures(const RenderScene &scene,
                                                   std::vector<Texture> *out) {
  std::vector<LegacyMaterialTex> bindings(scene.materials.size());
  if (!out) return bindings;

  // tydra texture index -> renderer texture index (-1 = undecodable). Cached so
  // a texture shared by several materials is decoded once.
  std::vector<int32_t> by_tydra_id(scene.textures.size(), -2);

  auto resolve = [&](int32_t tydra_id) -> int32_t {
    if (tydra_id < 0 || size_t(tydra_id) >= scene.textures.size()) return -1;
    int32_t &slot = by_tydra_id[size_t(tydra_id)];
    if (slot != -2) return slot;  // decoded (or already known-bad)
    slot = -1;

    const lightusd::tydra::UVTexture &tex = scene.textures[size_t(tydra_id)];
    if (tex.texture_image_id < 0 ||
        size_t(tex.texture_image_id) >= scene.images.size()) {
      return slot;
    }
    Texture t;
    if (!DecodeLegacyImageRGBA8(scene, scene.images[size_t(tex.texture_image_id)],
                                &t)) {
      return slot;
    }
    t.wrap_s = ToTusdrWrapLegacy(tex.wrapS);
    t.wrap_t = ToTusdrWrapLegacy(tex.wrapT);
    const lightusd::tydra::TextureImage &image =
        scene.images[size_t(tex.texture_image_id)];
    const lightusd::tydra::ColorSpace image_color_space = image.colorSpace;
    t.srgb = image_color_space == lightusd::tydra::ColorSpace::sRGB;
    if (image.colorTransformValid && !image.colorTransformApplied) {
      lightusd::color::ColorTransform transform;
      transform.source.name = image.sourceColorSpaceName;
      transform.source.gamma = image.sourceGamma;
      transform.source.linear_bias = image.sourceLinearBias;
      transform.source.kind = image.sourceColorIsData
          ? lightusd::color::ColorSpaceKind::Data
          : lightusd::color::ColorSpaceKind::Color;
      (void)lightusd::color::GetBuiltinColorSpace(
          "lin_rec709_scene", &transform.destination);
      transform.bypass = image.colorTransformBypass;
      std::copy(image.sourceToDisplayLinear,
                image.sourceToDisplayLinear + 9, transform.matrix);
      t.color_transform = transform;
      t.srgb = !image.sourceColorIsData &&
          std::fabs(image.sourceGamma - 2.4f) < 1.0e-5f &&
          std::fabs(image.sourceLinearBias - 0.055f) < 1.0e-5f;
    } else {
      (void)SetLegacyTextureColorTransform(image_color_space, &t);
    }
    // UsdUVTexture inputs:scale / inputs:bias (e.g. (2,2,2)/(-1,-1,-1) to unpack
    // a normal map); the integrator applies them post-sample.
    t.scale = Vec3{tex.scale[0], tex.scale[1], tex.scale[2]};
    t.bias = Vec3{tex.bias[0], tex.bias[1], tex.bias[2]};
    t.build_mips();

    slot = int32_t(out->size());
    out->push_back(std::move(t));
    return slot;
  };

  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const lightusd::tydra::RenderMaterial &mat = scene.materials[i];
    LegacyMaterialTex &b = bindings[i];

    auto chan = [&](int32_t tydra_id) -> uint8_t {
      if (tydra_id < 0 || size_t(tydra_id) >= scene.textures.size()) return 0;
      return ScalarChannel(scene.textures[size_t(tydra_id)]);
    };
    // Remember which UV primvar each textured lane samples. The mesh export
    // stores one `texcoords[slot]` per referenced primvar, so this is what lets
    // AddMeshTriangles pick the right slot per lane instead of always slot 0.
    auto note_uv = [&](uint32_t lane, int32_t tydra_id) {
      if (tydra_id < 0 || size_t(tydra_id) >= scene.textures.size()) return;
      b.uv_names[lane] = scene.textures[size_t(tydra_id)].varname_uv;
    };
    if (mat.surfaceShader.has_value()) {
      const lightusd::tydra::PreviewSurfaceShader &s = *mat.surfaceShader;
      b.diffuse = resolve(s.diffuseColor.texture_id);
      b.emissive = resolve(s.emissiveColor.texture_id);
      b.normal = resolve(s.normal.texture_id);
      b.roughness = resolve(s.roughness.texture_id);
      b.metallic = resolve(s.metallic.texture_id);
      b.occlusion = resolve(s.occlusion.texture_id);
      b.opacity = resolve(s.opacity.texture_id);
      b.roughness_ch = chan(s.roughness.texture_id);
      b.metallic_ch = chan(s.metallic.texture_id);
      b.occlusion_ch = chan(s.occlusion.texture_id);
      b.opacity_ch = chan(s.opacity.texture_id);
      note_uv(kUvLaneDiffuse, s.diffuseColor.texture_id);
      note_uv(kUvLaneEmissive, s.emissiveColor.texture_id);
      note_uv(kUvLaneNormal, s.normal.texture_id);
      note_uv(kUvLaneRough, s.roughness.texture_id);
      note_uv(kUvLaneMetal, s.metallic.texture_id);
      note_uv(kUvLaneOcc, s.occlusion.texture_id);
      note_uv(kUvLaneOpacity, s.opacity.texture_id);
    } else if (mat.openPBRShader.has_value()) {
      // MaterialX/OpenPBR textures use the same renderer slots as their
      // Preview Surface counterparts. Previously only constants were read from
      // openPBRShader, leaving every MaterialX texture unbound even though
      // Tydra had decoded it and resolved its colorspace transform.
      const lightusd::tydra::OpenPBRSurfaceShader &s = *mat.openPBRShader;
      b.diffuse = resolve(s.base_color.texture_id);
      b.normal = resolve(s.normal.texture_id);
      b.roughness = resolve(s.base_roughness.texture_id);
      b.metallic = resolve(s.base_metalness.texture_id);
      b.opacity = resolve(s.opacity.texture_id);
      b.roughness_ch = chan(s.base_roughness.texture_id);
      b.metallic_ch = chan(s.base_metalness.texture_id);
      b.opacity_ch = chan(s.opacity.texture_id);
      note_uv(kUvLaneDiffuse, s.base_color.texture_id);
      note_uv(kUvLaneNormal, s.normal.texture_id);
      note_uv(kUvLaneRough, s.base_roughness.texture_id);
      note_uv(kUvLaneMetal, s.base_metalness.texture_id);
      note_uv(kUvLaneOpacity, s.opacity.texture_id);
    }
  }
  return bindings;
}


std::vector<int> FaceMaterialIds(const RenderMesh &mesh) {
  const std::vector<uint32_t> &counts = mesh.faceVertexCounts();
  std::vector<int> ids(counts.size(), mesh.material_id);
  for (const auto &kv : mesh.material_subsetMap) {
    const lightusd::tydra::MaterialSubset &subset = kv.second;
    const std::vector<int> &faces = subset.indices();
    for (int f : faces) {
      if (f >= 0 && size_t(f) < ids.size()) {
        ids[size_t(f)] = subset.material_id;
      }
    }
  }
  return ids;
}

void AddMeshTriangles(const RenderScene &scene, const RenderMesh &mesh,
                      const matrix4d &world, std::vector<float> *vertices,
                      std::vector<TriInfo> *tris, Bounds *bounds,
                      LightCache *lights, std::vector<float> *tri_uvs,
                      std::vector<float> *tri_uvs1,
                      const std::vector<LegacyMaterialTex> *mat_tex,
                      const PurposeVisibilityMap *pv) {
  // Resolved purpose/visibility (BuildLegacyPurposeVisibility): value 0 means an
  // ancestor (or the mesh) authored visibility=invisible -- emit nothing. Any
  // other value is the inherited purpose bit for every triangle of this mesh.
  // No map / no entry keeps the old behavior (default purpose, visible).
  uint32_t purpose_bit = kPurposeDefaultBit;
  if (pv) {
    auto it = pv->find(mesh.abs_path);
    if (it != pv->end()) {
      if (it->second == 0u) return;  // invisible
      purpose_bit = it->second;
    }
  }
  if (!vertices || !tris || !bounds) return;
  const std::vector<uint32_t> &indices = mesh.faceVertexIndices();
  const std::vector<uint32_t> &counts = mesh.faceVertexCounts();
  std::vector<int> material_ids = FaceMaterialIds(mesh);

  // UV streams. tydra exports each mesh UV primvar as `texcoords[slot]`; slot 0
  // is the first UV name the bound material references. A textured lane whose
  // UsdPrimvarReader names the second primvar samples `tri_uvs1`; every other
  // lane (and all lanes when no second set exists) samples `tri_uvs`.
  const lightusd::tydra::VertexAttribute *uv_attrs[2] = {nullptr, nullptr};
  if (tri_uvs) {
    for (uint32_t slot = 0; slot < 2; ++slot) {
      auto it = mesh.texcoords.find(slot);
      if (it != mesh.texcoords.end() && !it->second.empty())
        uv_attrs[slot] = &it->second;
    }
    if (!uv_attrs[0]) {
      // Defensive: a mesh whose first primvar landed in a higher slot still
      // renders with that set rather than dropping every lookup to (0, 0).
      for (const auto &kv : mesh.texcoords) {
        if (!kv.second.empty()) {
          uv_attrs[0] = &kv.second;
          break;
        }
      }
    }
  }
  const bool uv_facevarying[2] = {uv_attrs[0] && uv_attrs[0]->is_facevarying(),
                                  uv_attrs[1] && uv_attrs[1]->is_facevarying()};
  // `fv` is the face-corner ordinal, `pi` the point index; pick whichever the
  // attribute is indexed by. Returns raw USD UVs — Texture::sample() does the
  // v-flip, so do not pre-flip here (matches the `next` path).
  auto uv_at = [&](uint32_t stream, size_t fv,
                   uint32_t pi) -> std::pair<float, float> {
    const lightusd::tydra::VertexAttribute *attr = uv_attrs[stream];
    if (!attr) return {0.0f, 0.0f};
    const size_t i = uv_facevarying[stream] ? fv : size_t(pi);
    if (i >= attr->vertex_count()) return {0.0f, 0.0f};
    const size_t esz = attr->format_size();
    const std::vector<uint8_t> &d = attr->get_data();
    if ((i + 1) * esz > d.size() || esz < 2 * sizeof(float)) return {0.0f, 0.0f};
    float uv[2];
    std::memcpy(uv, d.data() + i * esz, sizeof(uv));
    return {uv[0], uv[1]};
  };
  // Lane -> texcoord slot for one material, cached per mesh. A lane whose UV
  // primvar is not the second slot stays on the primary stream.
  std::unordered_map<int, uint32_t> lane_slot_cache;
  auto lane_slots = [&](int mat_id) -> uint32_t {
    auto found = lane_slot_cache.find(mat_id);
    if (found != lane_slot_cache.end()) return found->second;
    uint32_t bits = 0;
    if (mat_tex && mat_id >= 0 && size_t(mat_id) < mat_tex->size()) {
      const LegacyMaterialTex &mt = (*mat_tex)[size_t(mat_id)];
      for (uint32_t lane = 0; lane < uint32_t(kUvLaneCount); ++lane) {
        if (mt.uv_names[lane].empty() || !uv_attrs[0]) continue;
        const uint32_t slot =
            (uv_attrs[1] && uv_attrs[1]->name == mt.uv_names[lane]) ? 1u : 0u;
        SetUvLaneSlot(&bits, lane, slot);
      }
    }
    lane_slot_cache.emplace(mat_id, bits);
    return bits;
  };
  float mesh_area = 0.0f;
  if (mesh.is_area_light) {
    size_t area_cursor = 0;
    for (size_t face = 0; face < counts.size(); face++) {
      uint32_t nverts = counts[face];
      if (nverts < 3 || area_cursor + nverts > indices.size()) {
        area_cursor += nverts;
        continue;
      }
      for (uint32_t k = 1; k + 1 < nverts; k++) {
        uint32_t i0 = indices[area_cursor + 0];
        uint32_t i1 = indices[area_cursor + k];
        uint32_t i2 = indices[area_cursor + k + 1];
        if (i0 >= mesh.points.size() || i1 >= mesh.points.size() ||
            i2 >= mesh.points.size()) {
          continue;
        }
        mesh_area += TriangleArea(TransformPoint(world, FromFloat3(mesh.points[i0])),
                                  TransformPoint(world, FromFloat3(mesh.points[i1])),
                                  TransformPoint(world, FromFloat3(mesh.points[i2])));
      }
      area_cursor += nverts;
    }
  }
  // True once the second UV stream has been allocated. A previous mesh may
  // already have allocated it, in which case every later triangle must append
  // to stay parallel to `tris` even when it only uses the primary set.
  bool second_emitted = tri_uvs1 && !tri_uvs1->empty();
  size_t cursor = 0;
  for (size_t face = 0; face < counts.size(); face++) {
    uint32_t nverts = counts[face];
    if (nverts < 3 || cursor + nverts > indices.size()) {
      cursor += nverts;
      continue;
    }
    int mat_id = (face < material_ids.size()) ? material_ids[face] : mesh.material_id;
    for (uint32_t k = 1; k + 1 < nverts; k++) {
      uint32_t i0 = indices[cursor + 0];
      uint32_t i1 = indices[cursor + k];
      uint32_t i2 = indices[cursor + k + 1];
      if (i0 >= mesh.points.size() || i1 >= mesh.points.size() ||
          i2 >= mesh.points.size()) {
        continue;
      }
      Vec3 p0 = TransformPoint(world, FromFloat3(mesh.points[i0]));
      Vec3 p1 = TransformPoint(world, FromFloat3(mesh.points[i1]));
      Vec3 p2 = TransformPoint(world, FromFloat3(mesh.points[i2]));
      Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
      if (!mesh.is_rightHanded) {
        n = Mul(n, -1.0f);
      }
      TriInfo tri;
      tri.purpose_bit = purpose_bit;
      tri.p0 = p0;
      tri.p1 = p1;
      tri.p2 = p2;
      tri.n = n;
      // Authored doubleSided (RenderMesh default false = single-sided): drives
      // back-face culling in the flat integrator, matching the raster backends.
      tri.double_sided = mesh.doubleSided ? 1 : 0;
      tri.base_color = MaterialColor(scene, mesh, mat_id);
      tri.emission = MaterialEmission(scene, mat_id);
      tri.roughness = MaterialRoughness(scene, mat_id);
      tri.metallic = MaterialMetallic(scene, mat_id);
      tri.opacity = MaterialOpacity(scene, mesh, mat_id);
      tri.opacity_threshold = MaterialOpacityThreshold(scene, mat_id);
      tri.uv_lane_slot = lane_slots(mat_id);
      if (mesh.is_area_light) {
        tri.emission = MeshLightEmission(scene, mesh, mat_id, mesh_area);
      }
      // Bind this material's textures. The integrator MULTIPLIES the sampled
      // texel by the constant, so a textured channel's constant becomes white —
      // otherwise the UsdPreviewSurface fallback (0.18 gray) would darken it.
      if (mat_tex && mat_id >= 0 && size_t(mat_id) < mat_tex->size()) {
        const LegacyMaterialTex &mt = (*mat_tex)[size_t(mat_id)];
        if (mt.diffuse >= 0) {
          tri.tex_id = mt.diffuse;
          tri.base_color = Vec3{1.0f, 1.0f, 1.0f};
        }
        if (mt.emissive >= 0 && !mesh.is_area_light) {
          tri.emission_tex_id = mt.emissive;
          tri.emission = Vec3{1.0f, 1.0f, 1.0f};
        }
        tri.normal_tex_id = mt.normal;
        tri.rough_tex_id = mt.roughness;
        tri.rough_ch = mt.roughness_ch;
        tri.metal_tex_id = mt.metallic;
        tri.metal_ch = mt.metallic_ch;
        tri.occ_tex_id = mt.occlusion;
        tri.occ_ch = mt.occlusion_ch;
        tri.opacity_tex_id = mt.opacity;
        tri.opacity_ch = mt.opacity_ch;
      }
      if (tri_uvs) {
        // 6 floats/tri, parallel to *tris, in the same fan order as i0/i1/i2.
        const auto uv0 = uv_at(0, cursor + 0, i0);
        const auto uv1 = uv_at(0, cursor + size_t(k), i1);
        const auto uv2 = uv_at(0, cursor + size_t(k) + 1, i2);
        tri_uvs->insert(tri_uvs->end(), {uv0.first, uv0.second, uv1.first,
                                         uv1.second, uv2.first, uv2.second});
        if (tri_uvs1) {
          // Lazily allocate so scenes that never reference a second UV set pay
          // no extra memory. `assign` backfills earlier triangles with zeros;
          // the flag (not emptiness) tracks allocation, since the very first
          // triangle assigns zero elements.
          if (!second_emitted && tri.uv_lane_slot != 0) {
            tri_uvs1->assign(tris->size() * 6, 0.0f);
            second_emitted = true;
          }
          if (second_emitted) {
            const auto s0 = uv_at(1, cursor + 0, i0);
            const auto s1 = uv_at(1, cursor + size_t(k), i1);
            const auto s2 = uv_at(1, cursor + size_t(k) + 1, i2);
            tri_uvs1->insert(tri_uvs1->end(), {s0.first, s0.second, s1.first,
                                               s1.second, s2.first, s2.second});
          }
        }
      }
      vertices->push_back(p0.x);
      vertices->push_back(p0.y);
      vertices->push_back(p0.z);
      vertices->push_back(p1.x);
      vertices->push_back(p1.y);
      vertices->push_back(p1.z);
      vertices->push_back(p2.x);
      vertices->push_back(p2.y);
      vertices->push_back(p2.z);
      int tri_id = int(tris->size());
      tris->push_back(tri);
      if (lights && mesh.is_area_light && Luminance(tri.emission) > 1.0e-6f) {
        float area = TriangleArea(p0, p1, p2);
        if (area > 1.0e-10f) {
          // Mark the triangle as an analytic light so a BSDF-bounce ray landing
          // on it does not add its emission on top of the direct-lighting term
          // that already delivers it.
          (*tris)[size_t(tri_id)].area_light = 1;
          PreviewLight ml;
          ml.kind = PreviewLight::Kind::Mesh;
          ml.position = Mul(Add(Add(p0, p1), p2), 1.0f / 3.0f);
          ml.normal = n;
          ml.direction = Mul(n, -1.0f);
          ml.radiance = tri.emission;
          ml.area = area;
          ml.power = std::max(0.0f, Luminance(ml.radiance) * area);
          ml.tri_id = tri_id;
          lights->mesh.push_back(ml);
        }
      }
      Expand(bounds, p0);
      Expand(bounds, p1);
      Expand(bounds, p2);
    }
    cursor += nverts;
  }
}

void CollectGeometry(const RenderScene &scene, const Node &node,
                     std::vector<float> *vertices, std::vector<TriInfo> *tris,
                     Bounds *bounds,
                     const std::unordered_set<std::string> *skip_paths,
                     LightCache *lights, std::vector<float> *tri_uvs,
                     std::vector<float> *tri_uvs1,
                     const std::vector<LegacyMaterialTex> *mat_tex,
                     const PurposeVisibilityMap *pv) {
  if (node.nodeType == NodeType::Mesh && node.id >= 0 &&
      size_t(node.id) < scene.meshes.size()) {
    const RenderMesh &mesh = scene.meshes[size_t(node.id)];
    if (!skip_paths || !skip_paths->count(mesh.abs_path)) {
      AddMeshTriangles(scene, mesh, node.global_matrix, vertices, tris, bounds,
                       lights, tri_uvs, tri_uvs1, mat_tex, pv);
    }
  }
  for (const Node &child : node.children) {
    CollectGeometry(scene, child, vertices, tris, bounds, skip_paths, lights,
                    tri_uvs, tri_uvs1, mat_tex, pv);
  }
}

void CollectAllGeometry(const RenderScene &scene, std::vector<float> *vertices,
                        std::vector<TriInfo> *tris, Bounds *bounds,
                        const std::unordered_set<std::string> *skip_paths,
                        LightCache *lights, std::vector<float> *tri_uvs,
                        std::vector<float> *tri_uvs1,
                        const std::vector<LegacyMaterialTex> *mat_tex,
                        const PurposeVisibilityMap *pv) {
  for (const Node &root : scene.nodes) {
    CollectGeometry(scene, root, vertices, tris, bounds, skip_paths, lights,
                    tri_uvs, tri_uvs1, mat_tex, pv);
  }
  for (const lightusd::tydra::RenderInstance &inst : scene.instances) {
    if (inst.mesh_id >= 0 && size_t(inst.mesh_id) < scene.meshes.size() &&
        inst.visible) {
      const RenderMesh &mesh = scene.meshes[size_t(inst.mesh_id)];
      AddMeshTriangles(scene, mesh, inst.global_matrix, vertices, tris, bounds,
                       lights, tri_uvs, tri_uvs1, mat_tex, pv);
    }
  }
}



template <typename T>
bool BorrowMMapArray(const lightusd::Stage &stage, const std::string &prim_path,
                     const std::string &attr_name, BorrowedArrayView<T> *out) {
  if (!out || !stage.has_mmap_zero_copy()) return false;
  const lightusd::MMapArrayRef *ref =
      stage.mmap_table()->find_compatible(prim_path, attr_name);
  if (!ref) return false;
  const lightusd::MMapDataSource *source = stage.mmap_source();
  if (!source || !source->is_valid()) return false;
  if (ref->element_size != sizeof(T)) return false;
  if (ref->element_count > (UINT64_MAX / sizeof(T))) return false;
  uint64_t byte_count = ref->element_count * sizeof(T);
  if (ref->byte_offset > source->size()) return false;
  if (byte_count > (source->size() - ref->byte_offset)) return false;
  const uint8_t *bytes = source->addr() + ref->byte_offset;
  const T *ptr = nullptr;
  if (reinterpret_cast<uintptr_t>(bytes) % alignof(T) == 0) {
    ptr = reinterpret_cast<const T *>(bytes);
  }
  // On 64-bit hosts this is always false (size_t == uint64_t), but the guard
  // is needed for 32-bit targets where size_t < uint64_t.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-type-limit-compare"
#endif
  if (ref->element_count > uint64_t((std::numeric_limits<size_t>::max)())) {
    return false;
  }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  out->data = ptr;
  out->bytes = bytes;
  out->count = static_cast<size_t>(ref->element_count);
  out->mmap = true;
  out->owned.clear();
  return true;
}

template <typename T>
T ReadBorrowedArrayValue(const BorrowedArrayView<T> &view, size_t index) {
  if (view.data) return view.data[index];
  T value{};
  std::memcpy(&value, view.bytes + index * sizeof(T), sizeof(T));
  return value;
}

// Zero-copy const-ref access to an in-memory array attribute. Returns the
// underlying vector without copying when the attribute holds a static (non
// time-sampled, non-connected, non-blocked) default value. Returns nullptr for
// time-sampled/connected/blocked attributes (the caller should fall back to the
// copying EvalAnim() path) or for deferred mmap arrays (empty vector in
// mmap_zero_copy mode; the caller should try BorrowMMapArray() first).
template <typename T>
const std::vector<T> *BorrowScalarVector(
    const lightusd::TypedAttribute<lightusd::Animatable<std::vector<T>>> &attr) {
  if (attr.is_blocked() || attr.has_connections()) return nullptr;
  const auto &opt = attr.get_value_ref();
  if (!opt) return nullptr;
  const lightusd::Animatable<std::vector<T>> &anim = opt.value();
  if (anim.is_scalar() && anim.has_default()) {
    return &anim.get_scalar_ref();
  }
  return nullptr;
}

// Number of worker threads to use for the embarrassingly-parallel mesh-stream
// and render passes. `requested` is the user's -threads value (<=0 means auto).
unsigned WorkerThreadCount(int requested) {
  if (requested > 0) return unsigned(requested);
  unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? hw : 1u;
}

const lightusd::Xformable *AsPreviewXformable(const lightusd::Prim &prim) {
  if (const lightusd::Xform *x = prim.as<lightusd::Xform>()) return x;
  if (const lightusd::GeomMesh *m = prim.as<lightusd::GeomMesh>()) return m;
  if (const lightusd::GeomCamera *c = prim.as<lightusd::GeomCamera>()) return c;
  return nullptr;
}

const lightusd::GPrim *AsPreviewGPrim(const lightusd::Prim &prim) {
  if (const lightusd::Xform *p = prim.as<lightusd::Xform>()) return p;
  if (const lightusd::GeomMesh *p = prim.as<lightusd::GeomMesh>()) return p;
  if (const lightusd::GeomCamera *p = prim.as<lightusd::GeomCamera>()) return p;
  if (const lightusd::GeomCube *p = prim.as<lightusd::GeomCube>()) return p;
  if (const lightusd::GeomSphere *p = prim.as<lightusd::GeomSphere>()) return p;
  if (const lightusd::GeomCone *p = prim.as<lightusd::GeomCone>()) return p;
  if (const lightusd::GeomCylinder *p = prim.as<lightusd::GeomCylinder>()) return p;
  if (const lightusd::GeomCapsule *p = prim.as<lightusd::GeomCapsule>()) return p;
  if (const lightusd::GeomPlane *p = prim.as<lightusd::GeomPlane>()) return p;
  if (const lightusd::GeomTetMesh *p = prim.as<lightusd::GeomTetMesh>()) return p;
  if (const lightusd::GeomNurbsPatch *p = prim.as<lightusd::GeomNurbsPatch>()) {
    return p;
  }
  if (const lightusd::GeomBasisCurves *p = prim.as<lightusd::GeomBasisCurves>()) {
    return p;
  }
  if (const lightusd::GeomHermiteCurves *p =
          prim.as<lightusd::GeomHermiteCurves>()) {
    return p;
  }
  if (const lightusd::GeomNurbsCurves *p = prim.as<lightusd::GeomNurbsCurves>()) {
    return p;
  }
  if (const lightusd::GeomPoints *p = prim.as<lightusd::GeomPoints>()) return p;
  if (const lightusd::GeomPointInstancer *p =
          prim.as<lightusd::GeomPointInstancer>()) {
    return p;
  }
  return nullptr;
}

matrix4d LocalMatrixOrIdentity(const lightusd::Xformable *xformable, double time,
                               bool *reset) {
  if (reset) *reset = false;
  if (!xformable) return matrix4d::identity();
  bool local_reset = false;
  auto ret = xformable->GetLocalMatrix(
      time, lightusd::value::TimeSampleInterpolationType::Linear, &local_reset);
  if (reset) *reset = local_reset;
  if (!ret) return matrix4d::identity();
  return ret.value();
}

template <typename T>
bool EvalAnim(const lightusd::Stage &stage,
              const lightusd::TypedAttribute<lightusd::Animatable<T>> &attr,
              const std::string &name, double time, T *out) {
  std::string err;
  return lightusd::tydra::EvaluateTypedAnimatableAttribute(
      stage, attr, name, out, &err, time);
}

template <typename T>
bool EvalAnimFallback(
    const lightusd::Stage &stage,
    const lightusd::TypedAttributeWithFallback<lightusd::Animatable<T>> &attr,
    const std::string &name, double time, T *out) {
  std::string err;
  return lightusd::tydra::EvaluateTypedAnimatableAttribute(
      stage, attr, name, out, &err, time);
}

uint32_t PurposeBit(lightusd::Purpose purpose) {
  switch (purpose) {
    case lightusd::Purpose::Render:
      return kPurposeRenderBit;
    case lightusd::Purpose::Proxy:
      return kPurposeProxyBit;
    case lightusd::Purpose::Guide:
      return kPurposeGuideBit;
    case lightusd::Purpose::Default:
    default:
      return kPurposeDefaultBit;
  }
}

lightusd::Purpose ResolvePurpose(const lightusd::Prim &prim,
                                 lightusd::Purpose inherited) {
  if (const lightusd::GPrim *gprim = AsPreviewGPrim(prim)) {
    lightusd::Purpose purpose = gprim->purpose.get_value();
    if (purpose != lightusd::Purpose::Default) return purpose;
  }
  return inherited;
}

bool PurposeVisible(uint32_t purpose_bit, uint32_t purpose_mask) {
  return (purpose_bit & purpose_mask) != 0;
}

// Resolve inherited purpose + visibility for every prim of the (legacy) Stage,
// keyed by absolute prim path. Value = the purpose bit the subtree inherits, or
// 0 when an ancestor (or the prim itself) authored visibility="invisible". The
// tydra RenderScene carries neither, so without this the legacy shaded path
// drew guide/proxy geometry unconditionally (-purpose/-hideProxy/... were
// no-ops) and rendered invisible prims.
void BuildLegacyPurposeVisibility(const lightusd::Stage &stage,
                                  PurposeVisibilityMap *out) {
  if (!out) return;
  std::function<void(const lightusd::Prim &, lightusd::Purpose, bool)> walk =
      [&](const lightusd::Prim &prim, lightusd::Purpose inherited,
          bool invisible) {
        if (const lightusd::GPrim *gp = AsPreviewGPrim(prim)) {
          lightusd::Visibility vis = lightusd::Visibility::Inherited;
          if (gp->visibility.get_value().get_default(&vis) &&
              vis == lightusd::Visibility::Invisible) {
            invisible = true;
          }
        }
        inherited = ResolvePurpose(prim, inherited);
        const std::string path = prim.absolute_path().full_path_name();
        (*out)[path] = invisible ? 0u : PurposeBit(inherited);
        for (const lightusd::Prim &child : prim.children()) {
          walk(child, inherited, invisible);
        }
      };
  for (const lightusd::Prim &root : stage.root_prims()) {
    walk(root, lightusd::Purpose::Default, false);
  }
}


bool AddRTPreviewMesh(const lightusd::Stage &stage, const std::string &prim_path,
                      const lightusd::GeomMesh &mesh, const matrix4d &world,
                      double time, lightusd::Purpose purpose,
                      uint32_t purpose_mask,
                      std::vector<float> *vertices, std::vector<TriInfo> *tris,
                      Bounds *bounds, RTPreviewStats *stats) {
  if (!vertices || !tris || !bounds || !stats) return false;
  BorrowedArrayView<lightusd::value::point3f> points;
  if (BorrowMMapArray(stage, prim_path, "points", &points)) {
    stats->meshes_with_mmap_points++;
  } else if (const std::vector<lightusd::value::point3f> *pv =
                 BorrowScalarVector(mesh.points)) {
    // Zero-copy: in-memory (materialized) points vector.
    points.data = pv->data();
    points.bytes = reinterpret_cast<const uint8_t *>(pv->data());
    points.count = pv->size();
    points.mmap = false;
    stats->meshes_with_owned_points++;
  } else {
    // Fallback (time-sampled/connected): copy via attribute evaluation.
    if (!EvalAnim(stage, mesh.points, "points", time, &points.owned)) {
      stats->skipped_meshes++;
      return false;
    }
    points.data = points.owned.data();
    points.bytes = reinterpret_cast<const uint8_t *>(points.owned.data());
    points.count = points.owned.size();
    points.mmap = false;
    stats->meshes_with_owned_points++;
    stats->copied_point_bytes +=
        uint64_t(points.count) * sizeof(lightusd::value::point3f);
  }

  // Topology: prefer zero-copy const-ref to the in-memory vectors; fall back to
  // a copy only for time-sampled/connected attributes.
  std::vector<int32_t> counts_owned;
  std::vector<int32_t> indices_owned;
  const std::vector<int32_t> *counts_ptr =
      BorrowScalarVector(mesh.faceVertexCounts);
  if (!counts_ptr) {
    if (!EvalAnim(stage, mesh.faceVertexCounts, "faceVertexCounts", time,
                  &counts_owned)) {
      stats->skipped_meshes++;
      return false;
    }
    counts_ptr = &counts_owned;
    stats->copied_topology_bytes += uint64_t(counts_owned.size()) * sizeof(int32_t);
  }
  const std::vector<int32_t> *indices_ptr =
      BorrowScalarVector(mesh.faceVertexIndices);
  if (!indices_ptr) {
    if (!EvalAnim(stage, mesh.faceVertexIndices, "faceVertexIndices", time,
                  &indices_owned)) {
      stats->skipped_meshes++;
      return false;
    }
    indices_ptr = &indices_owned;
    stats->copied_topology_bytes += uint64_t(indices_owned.size()) * sizeof(int32_t);
  }
  const std::vector<int32_t> &counts = *counts_ptr;
  const std::vector<int32_t> &indices = *indices_ptr;
  if ((!points.data && !points.bytes) || points.count == 0 || counts.empty() ||
      indices.empty()) {
    stats->skipped_meshes++;
    return false;
  }

  // Reserve output buffers up-front from the exact triangle-fan estimate to
  // avoid repeated reallocation while appending.
  size_t tri_estimate = 0;
  for (int32_t c : counts) {
    if (c >= 3) tri_estimate += size_t(c - 2);
  }
  if (tri_estimate) {
    vertices->reserve(vertices->size() + tri_estimate * 9);
    tris->reserve(tris->size() + tri_estimate);
  }

  size_t cursor = 0;
  for (int32_t c : counts) {
    if (c < 3 || cursor + size_t(c) > indices.size()) {
      cursor += size_t(std::max<int32_t>(0, c));
      continue;
    }
    for (int32_t k = 1; k + 1 < c; k++) {
      int32_t i0 = indices[cursor + 0];
      int32_t i1 = indices[cursor + size_t(k)];
      int32_t i2 = indices[cursor + size_t(k + 1)];
      if (i0 < 0 || i1 < 0 || i2 < 0 || size_t(i0) >= points.count ||
          size_t(i1) >= points.count || size_t(i2) >= points.count) {
        continue;
      }
      Vec3 p0 = TransformPoint(
          world, FromPoint3(ReadBorrowedArrayValue(points, size_t(i0))));
      Vec3 p1 = TransformPoint(
          world, FromPoint3(ReadBorrowedArrayValue(points, size_t(i1))));
      Vec3 p2 = TransformPoint(
          world, FromPoint3(ReadBorrowedArrayValue(points, size_t(i2))));
      Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
      if (Length(n) < 1.0e-12f) continue;

      TriInfo tri;
      tri.p0 = p0;
      tri.p1 = p1;
      tri.p2 = p2;
      tri.n = n;
      tri.base_color = Vec3{0.55f, 0.55f, 0.55f};
      tri.purpose_bit = PurposeBit(purpose);
      if (tri.purpose_bit == kPurposeRenderBit) {
        stats->purpose_render_triangles++;
      } else if (tri.purpose_bit == kPurposeProxyBit) {
        stats->purpose_proxy_triangles++;
      } else if (tri.purpose_bit == kPurposeGuideBit) {
        stats->purpose_guide_triangles++;
      } else {
        stats->purpose_default_triangles++;
      }
      const bool visible_for_fit = PurposeVisible(tri.purpose_bit, purpose_mask);
      vertices->insert(vertices->end(),
                       {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z});
      tris->push_back(tri);
      stats->triangles++;
      if (visible_for_fit) {
        Expand(bounds, p0);
        Expand(bounds, p1);
        Expand(bounds, p2);
      }
    }
    cursor += size_t(c);
  }
  return true;
}

// A single mesh-extraction work item produced by the (serial) tree walk and
// consumed by the parallel mesh-stream workers.

// Serial: resolve world matrices / purpose (parent-dependent) and flatten the
// renderable GeomMesh prims into `jobs`. The per-triangle work happens later in
// parallel; this walk only does cheap per-prim xform/purpose evaluation.
void CollectRTPreviewMeshes(const lightusd::Prim &prim,
                            const matrix4d &parent_world, double time,
                            lightusd::Purpose inherited_purpose,
                            std::vector<MeshJob> *jobs) {
  bool reset = false;
  const matrix4d local =
      LocalMatrixOrIdentity(AsPreviewXformable(prim), time, &reset);
  const matrix4d world = reset ? local : (local * parent_world);
  const lightusd::Purpose purpose = ResolvePurpose(prim, inherited_purpose);
  if (const lightusd::GeomMesh *mesh = prim.as<lightusd::GeomMesh>()) {
    MeshJob job;
    job.mesh = mesh;
    job.world = world;
    job.purpose = purpose;
    job.prim_path = prim.absolute_path().full_path_name();
    jobs->push_back(std::move(job));
  }
  for (const lightusd::Prim &child : prim.children()) {
    CollectRTPreviewMeshes(child, world, time, purpose, jobs);
  }
}

void MergeStats(RTPreviewStats *dst, const RTPreviewStats &src) {
  dst->meshes_with_mmap_points += src.meshes_with_mmap_points;
  dst->meshes_with_owned_points += src.meshes_with_owned_points;
  dst->skipped_meshes += src.skipped_meshes;
  dst->triangles += src.triangles;
  dst->copied_point_bytes += src.copied_point_bytes;
  dst->copied_topology_bytes += src.copied_topology_bytes;
  dst->purpose_default_triangles += src.purpose_default_triangles;
  dst->purpose_render_triangles += src.purpose_render_triangles;
  dst->purpose_proxy_triangles += src.purpose_proxy_triangles;
  dst->purpose_guide_triangles += src.purpose_guide_triangles;
}

void MergeBounds(Bounds *dst, const Bounds &src) {
  if (!src.valid) return;
  Expand(dst, src.lo);
  Expand(dst, src.hi);
}

bool BuildRTPreviewScene(const lightusd::Stage &stage, const Options &opt,
                         std::vector<float> *vertices,
                         std::vector<TriInfo> *tris, Bounds *bounds,
                         RTPreviewStats *stats, std::string *err) {
  if (!vertices || !tris || !bounds || !stats) return false;
  vertices->clear();
  tris->clear();
  *bounds = Bounds();
  *stats = RTPreviewStats();
  if (stage.has_mmap_zero_copy()) {
    stats->mmap_deferred_bytes = stage.mmap_table()->total_deferred_bytes();
  }
  const auto t0 = std::chrono::steady_clock::now();

  // Pass A (serial, cheap): flatten the prim tree into per-mesh jobs.
  std::vector<MeshJob> jobs;
  for (const lightusd::Prim &root : stage.root_prims()) {
    CollectRTPreviewMeshes(root, matrix4d::identity(), opt.timecode,
                           lightusd::Purpose::Default, &jobs);
  }
  stats->meshes = jobs.size();

  // Pass B (parallel): extract + triangulate each mesh into its own result
  // buffer (disjoint writes, no locking). Work-stealing via an atomic cursor
  // balances the highly non-uniform per-mesh cost.
  struct MeshResult {
    std::vector<float> vertices;
    std::vector<TriInfo> tris;
    Bounds bounds;
    RTPreviewStats stats;
  };
  std::vector<MeshResult> results(jobs.size());
  const unsigned nthreads =
      std::min<unsigned>(WorkerThreadCount(opt.threads),
                         jobs.empty() ? 1u : unsigned(jobs.size()));
  std::atomic<size_t> cursor{0};
  auto worker = [&]() {
    for (;;) {
      const size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
      if (i >= jobs.size()) break;
      const MeshJob &job = jobs[i];
      MeshResult &r = results[i];
      AddRTPreviewMesh(stage, job.prim_path, *job.mesh, job.world, opt.timecode,
                       job.purpose, opt.purpose_mask, &r.vertices, &r.tris,
                       &r.bounds, &r.stats);
    }
  };
  if (nthreads <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (std::thread &th : pool) th.join();
  }

  // Pass C (serial merge): concatenate in job order for deterministic output,
  // freeing each chunk as we go to bound peak memory.
  size_t total_floats = 0;
  size_t total_tris = 0;
  for (const MeshResult &r : results) {
    total_floats += r.vertices.size();
    total_tris += r.tris.size();
  }
  vertices->reserve(total_floats);
  tris->reserve(total_tris);
  for (MeshResult &r : results) {
    vertices->insert(vertices->end(), r.vertices.begin(), r.vertices.end());
    tris->insert(tris->end(), r.tris.begin(), r.tris.end());
    MergeBounds(bounds, r.bounds);
    MergeStats(stats, r.stats);
    std::vector<float>().swap(r.vertices);
    std::vector<TriInfo>().swap(r.tris);
  }

  const auto t1 = std::chrono::steady_clock::now();
  stats->build_seconds = std::chrono::duration<double>(t1 - t0).count();
  stats->packed_triangle_bytes = uint64_t(vertices->size()) * sizeof(float);
  if (tris->empty()) {
    if (err) *err = "RT preview found no renderable Mesh triangles.";
    return false;
  }
  return true;
}

bool MatchPrimNameOrPath(const lightusd::Prim &prim, const std::string &query) {
  if (query.empty()) return true;
  const std::string path = prim.absolute_path().full_path_name();
  return path == query || prim.element_name() == query;
}

bool CameraFrameFromGeomCamera(const lightusd::Stage &stage,
                               const lightusd::GeomCamera &cam,
                               const matrix4d &world, double time,
                               CameraFrame *frame) {
  if (!frame) return false;
  float focal_length = 50.0f;
  float vertical_aperture = 15.2908f;
  float horizontal_aperture = 20.955f;
  lightusd::value::float2 clipping_range{0.1f, 1000000.0f};
  lightusd::GeomCamera::Projection projection =
      lightusd::GeomCamera::Projection::Perspective;
  EvalAnimFallback(stage, cam.focalLength, "focalLength", time, &focal_length);
  EvalAnimFallback(stage, cam.verticalAperture, "verticalAperture", time,
                   &vertical_aperture);
  EvalAnimFallback(stage, cam.horizontalAperture, "horizontalAperture", time,
                   &horizontal_aperture);
  EvalAnimFallback(stage, cam.clippingRange, "clippingRange", time,
                   &clipping_range);
  cam.projection.get_value().get_scalar(&projection);

  frame->origin =
      Vec3{float(world.m[3][0]), float(world.m[3][1]), float(world.m[3][2])};
  frame->right = Normalize(TransformVector(world, Vec3{1.0f, 0.0f, 0.0f}));
  frame->up = Normalize(TransformVector(world, Vec3{0.0f, 1.0f, 0.0f}));
  frame->forward = Normalize(TransformVector(world, Vec3{0.0f, 0.0f, -1.0f}));
  frame->yfov = 2.0f * std::atan(0.5f * vertical_aperture /
                                 std::max(1.0e-6f, focal_length));
  frame->xmag = horizontal_aperture * 0.1f;
  frame->ymag = vertical_aperture * 0.1f;
  float offset_x=0, offset_y=0;
  EvalAnimFallback(stage,cam.horizontalApertureOffset,"horizontalApertureOffset",time,&offset_x);
  EvalAnimFallback(stage,cam.verticalApertureOffset,"verticalApertureOffset",time,&offset_y);
  frame->shift_x=2.0f*offset_x/std::max(1.0e-6f,horizontal_aperture);
  frame->shift_y=2.0f*offset_y/std::max(1.0e-6f,vertical_aperture);
  frame->znear = std::max(1.0e-5f, clipping_range[0]);
  frame->zfar = std::max(frame->znear, clipping_range[1]);
  frame->ortho = projection == lightusd::GeomCamera::Projection::Orthographic;
  return true;
}

bool FindStageCameraFrameRecursive(const lightusd::Stage &stage,
                                   const lightusd::Prim &prim,
                                   const std::string &query,
                                   const matrix4d &parent_world, double time,
                                   CameraFrame *frame) {
  bool reset = false;
  const matrix4d local =
      LocalMatrixOrIdentity(AsPreviewXformable(prim), time, &reset);
  const matrix4d world = reset ? local : (local * parent_world);
  if (const lightusd::GeomCamera *cam = prim.as<lightusd::GeomCamera>()) {
    if (MatchPrimNameOrPath(prim, query)) {
      return CameraFrameFromGeomCamera(stage, *cam, world, time, frame);
    }
  }
  for (const lightusd::Prim &child : prim.children()) {
    if (FindStageCameraFrameRecursive(stage, child, query, world, time, frame)) {
      return true;
    }
  }
  return false;
}

bool FindStageCameraFrame(const lightusd::Stage &stage, const std::string &query,
                          double time, CameraFrame *frame) {
  for (const lightusd::Prim &root : stage.root_prims()) {
    if (FindStageCameraFrameRecursive(stage, root, query, matrix4d::identity(),
                                      time, frame)) {
      return true;
    }
  }
  return false;
}

template <typename T>
bool EvalFallback(const lightusd::Stage &stage,
                  const lightusd::TypedAttributeWithFallback<T> &attr,
                  const std::string &name, T *out) {
  std::string err;
  return lightusd::tydra::EvaluateTypedAttribute(stage, attr, name, out, &err);
}

bool EvalAxis(const lightusd::TypedAttributeWithFallback<lightusd::Axis> &attr,
              lightusd::Axis *out) {
  if (!out) return false;
  *out = attr.get_value();
  return true;
}

std::string PrimPathString(const lightusd::Prim &prim) {
  return prim.absolute_path().full_path_name();
}

float ApproxScale(const matrix4d &m) {
  Vec3 sx = TransformVector(m, Vec3{1.0f, 0.0f, 0.0f});
  Vec3 sy = TransformVector(m, Vec3{0.0f, 1.0f, 0.0f});
  Vec3 sz = TransformVector(m, Vec3{0.0f, 0.0f, 1.0f});
  return std::max(1.0e-6f, (Length(sx) + Length(sy) + Length(sz)) / 3.0f);
}

void AddNurbsTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                      std::vector<float> *vertices, std::vector<TriInfo> *tris,
                      Bounds *bounds) {
  Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
  TriInfo tri;
  tri.p0 = p0;
  tri.p1 = p1;
  tri.p2 = p2;
  tri.n = n;
  tri.base_color = Vec3{0.42f, 0.42f, 0.48f};
  vertices->insert(vertices->end(), {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z,
                                     p2.x, p2.y, p2.z});
  tris->push_back(tri);
  Expand(bounds, p0);
  Expand(bounds, p1);
  Expand(bounds, p2);
}

void AddDirectTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                       const Vec3 &color, std::vector<float> *vertices,
                       std::vector<TriInfo> *tris, Bounds *bounds) {
  Vec3 n = Normalize(Cross(Sub(p1, p0), Sub(p2, p0)));
  TriInfo tri;
  tri.p0 = p0;
  tri.p1 = p1;
  tri.p2 = p2;
  tri.n = n;
  tri.base_color = color;
  vertices->insert(vertices->end(), {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z,
                                     p2.x, p2.y, p2.z});
  tris->push_back(tri);
  Expand(bounds, p0);
  Expand(bounds, p1);
  Expand(bounds, p2);
}

void AddDirectCube(double size, const matrix4d &world, std::vector<float> *vertices,
                   std::vector<TriInfo> *tris, Bounds *bounds) {
  float h = float(size * 0.5);
  Vec3 p[8] = {
      TransformPoint(world, Vec3{-h, -h, -h}),
      TransformPoint(world, Vec3{ h, -h, -h}),
      TransformPoint(world, Vec3{ h,  h, -h}),
      TransformPoint(world, Vec3{-h,  h, -h}),
      TransformPoint(world, Vec3{-h, -h,  h}),
      TransformPoint(world, Vec3{ h, -h,  h}),
      TransformPoint(world, Vec3{ h,  h,  h}),
      TransformPoint(world, Vec3{-h,  h,  h}),
  };
  const int f[12][3] = {
      {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
      {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
      {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
  };
  for (const auto &tri : f) {
    AddDirectTriangle(p[tri[0]], p[tri[1]], p[tri[2]],
                      Vec3{0.46f, 0.50f, 0.56f}, vertices, tris, bounds);
  }
}

void AddDirectPlane(double width, double length, lightusd::Axis axis,
                    const matrix4d &world, std::vector<float> *vertices,
                    std::vector<TriInfo> *tris, Bounds *bounds) {
  float hw = float(width * 0.5);
  float hl = float(length * 0.5);
  Vec3 local[4];
  if (axis == lightusd::Axis::X) {
    local[0] = Vec3{0.0f, -hw, -hl};
    local[1] = Vec3{0.0f,  hw, -hl};
    local[2] = Vec3{0.0f,  hw,  hl};
    local[3] = Vec3{0.0f, -hw,  hl};
  } else if (axis == lightusd::Axis::Y) {
    local[0] = Vec3{-hw, 0.0f, -hl};
    local[1] = Vec3{ hw, 0.0f, -hl};
    local[2] = Vec3{ hw, 0.0f,  hl};
    local[3] = Vec3{-hw, 0.0f,  hl};
  } else {
    local[0] = Vec3{-hw, -hl, 0.0f};
    local[1] = Vec3{ hw, -hl, 0.0f};
    local[2] = Vec3{ hw,  hl, 0.0f};
    local[3] = Vec3{-hw,  hl, 0.0f};
  }
  Vec3 p0 = TransformPoint(world, local[0]);
  Vec3 p1 = TransformPoint(world, local[1]);
  Vec3 p2 = TransformPoint(world, local[2]);
  Vec3 p3 = TransformPoint(world, local[3]);
  AddDirectTriangle(p0, p1, p2, Vec3{0.38f, 0.55f, 0.44f}, vertices, tris, bounds);
  AddDirectTriangle(p0, p2, p3, Vec3{0.38f, 0.55f, 0.44f}, vertices, tris, bounds);
}

double BSplineBasis(int i, int degree, double u, const std::vector<double> &knots) {
  if (degree == 0) {
    const bool last = (i + 1 == int(knots.size()) - 1) && (u == knots.back());
    return ((knots[size_t(i)] <= u && u < knots[size_t(i + 1)]) || last) ? 1.0 : 0.0;
  }
  double left = 0.0;
  double denom_l = knots[size_t(i + degree)] - knots[size_t(i)];
  if (std::abs(denom_l) > 1.0e-14) {
    left = (u - knots[size_t(i)]) / denom_l *
           BSplineBasis(i, degree - 1, u, knots);
  }
  double right = 0.0;
  double denom_r = knots[size_t(i + degree + 1)] - knots[size_t(i + 1)];
  if (std::abs(denom_r) > 1.0e-14) {
    right = (knots[size_t(i + degree + 1)] - u) / denom_r *
            BSplineBasis(i + 1, degree - 1, u, knots);
  }
  return left + right;
}

Vec3 EvalNurbsPatchPoint(const std::vector<lightusd::value::point3f> &points,
                         const std::vector<double> &weights, int u_count,
                         int v_count, int u_order, int v_order,
                         const std::vector<double> &u_knots,
                         const std::vector<double> &v_knots, double u, double v) {
  Vec3 sum{0.0f, 0.0f, 0.0f};
  double wsum = 0.0;
  int u_degree = std::max(0, u_order - 1);
  int v_degree = std::max(0, v_order - 1);
  for (int j = 0; j < v_count; j++) {
    double bv = BSplineBasis(j, v_degree, v, v_knots);
    if (bv == 0.0) continue;
    for (int i = 0; i < u_count; i++) {
      double bu = BSplineBasis(i, u_degree, u, u_knots);
      if (bu == 0.0) continue;
      size_t idx = size_t(j) * size_t(u_count) + size_t(i);
      double w = idx < weights.size() ? weights[idx] : 1.0;
      double b = bu * bv * w;
      Vec3 p = FromPoint3(points[idx]);
      sum = Add(sum, Mul(p, float(b)));
      wsum += b;
    }
  }
  if (std::abs(wsum) > 1.0e-20) {
    sum = Mul(sum, float(1.0 / wsum));
  }
  return sum;
}

void AddNurbsPatchTriangles(const lightusd::Stage &stage,
                            const lightusd::GeomNurbsPatch &patch,
                            const matrix4d &world, double time,
                            std::vector<float> *vertices,
                            std::vector<TriInfo> *tris, Bounds *bounds) {
  std::vector<lightusd::value::point3f> points;
  int u_count = 0, v_count = 0, u_order = 0, v_order = 0;
  std::vector<double> u_knots, v_knots, weights;
  if (!EvalAnim(stage, patch.points, "points", time, &points) ||
      !EvalAnim(stage, patch.uVertexCount, "uVertexCount", time, &u_count) ||
      !EvalAnim(stage, patch.vVertexCount, "vVertexCount", time, &v_count) ||
      !EvalAnim(stage, patch.uOrder, "uOrder", time, &u_order) ||
      !EvalAnim(stage, patch.vOrder, "vOrder", time, &v_order) ||
      !EvalAnim(stage, patch.uKnots, "uKnots", time, &u_knots) ||
      !EvalAnim(stage, patch.vKnots, "vKnots", time, &v_knots)) {
    return;
  }
  EvalAnim(stage, patch.pointWeights, "pointWeights", time, &weights);
  if (u_count <= 0 || v_count <= 0 ||
      points.size() < size_t(u_count) * size_t(v_count)) {
    return;
  }
  double u0 = u_knots[size_t(std::max(0, u_order - 1))];
  double u1 = u_knots[u_knots.size() - size_t(std::max(1, u_order))];
  double v0 = v_knots[size_t(std::max(0, v_order - 1))];
  double v1 = v_knots[v_knots.size() - size_t(std::max(1, v_order))];
  lightusd::value::double2 range;
  if (EvalAnim(stage, patch.uRange, "uRange", time, &range)) {
    u0 = range[0];
    u1 = range[1];
  }
  if (EvalAnim(stage, patch.vRange, "vRange", time, &range)) {
    v0 = range[0];
    v1 = range[1];
  }
  constexpr int divs = 24;
  std::vector<Vec3> grid(size_t(divs + 1) * size_t(divs + 1));
  for (int y = 0; y <= divs; y++) {
    double v = v0 + (v1 - v0) * double(y) / double(divs);
    for (int x = 0; x <= divs; x++) {
      double u = u0 + (u1 - u0) * double(x) / double(divs);
      grid[size_t(y) * size_t(divs + 1) + size_t(x)] =
          TransformPoint(world, EvalNurbsPatchPoint(points, weights, u_count,
                                                    v_count, u_order, v_order,
                                                    u_knots, v_knots, u, v));
    }
  }
  for (int y = 0; y < divs; y++) {
    for (int x = 0; x < divs; x++) {
      Vec3 p00 = grid[size_t(y) * size_t(divs + 1) + size_t(x)];
      Vec3 p10 = grid[size_t(y) * size_t(divs + 1) + size_t(x + 1)];
      Vec3 p01 = grid[size_t(y + 1) * size_t(divs + 1) + size_t(x)];
      Vec3 p11 = grid[size_t(y + 1) * size_t(divs + 1) + size_t(x + 1)];
      AddNurbsTriangle(p00, p10, p11, vertices, tris, bounds);
      AddNurbsTriangle(p00, p11, p01, vertices, tris, bounds);
    }
  }
}

// Fixed base color for unmaterialed curve geometry (the `next` path doesn't
// resolve curve displayColor yet). One value for every hair segment.

void AppendLinearCurveStrands(const std::vector<lightusd::value::point3f> &points,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds) {
  size_t cursor = 0;
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > points.size()) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t(c));
    for (int i = 0; i < c; i++) {
      const size_t idx = cursor + size_t(i);
      Vec3 p = TransformPoint(world, FromPoint3(points[idx]));
      const float radius =
          0.5f * ((idx < widths.size()) ? widths[idx] : 0.01f);
      curve_points->insert(curve_points->end(), {p.x, p.y, p.z});
      curve_radii->push_back(std::max(1.0e-5f, radius * ApproxScale(world)));
      Expand(bounds, p);
    }
    if (info) {
      for (int i = 0; i + 1 < c; i++) {
        TriInfo ti;
        const size_t point_base = size_t(first->back()) + size_t(i);
        Vec3 p0{(*curve_points)[point_base * 3 + 0],
                (*curve_points)[point_base * 3 + 1],
                (*curve_points)[point_base * 3 + 2]};
        Vec3 p1{(*curve_points)[(point_base + 1) * 3 + 0],
                (*curve_points)[(point_base + 1) * 3 + 1],
                (*curve_points)[(point_base + 1) * 3 + 2]};
        ti.p0 = p0;
        ti.p1 = p1;
        ti.p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}

void AppendLinearCurveStrands(const float *points, size_t point_count,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds) {
  if (!points || point_count == 0) return;
  size_t cursor = 0;
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > point_count) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t(c));
    for (int i = 0; i < c; i++) {
      size_t idx = cursor + size_t(i);
      Vec3 p = TransformPoint(
          world, Vec3{points[idx * 3 + 0], points[idx * 3 + 1],
                      points[idx * 3 + 2]});
      float radius = 0.5f * ((idx < widths.size()) ? widths[idx] : 0.01f);
      curve_points->insert(curve_points->end(), {p.x, p.y, p.z});
      curve_radii->push_back(std::max(1.0e-5f, radius * ApproxScale(world)));
      Expand(bounds, p);
    }
    // Per-segment TriInfo: only for the DirectScene curve path (info != null).
    // The instanced curve BLAS passes info == null and derives its slim per-
    // segment endpoints straight from curve_points (kCurveColor is the material),
    // skipping this 120 B/segment intermediate entirely.
    if (info) {
      for (int i = 0; i + 1 < c; i++) {
        TriInfo ti;
        size_t point_base = size_t(first->back()) + size_t(i);
        Vec3 p0{(*curve_points)[point_base * 3 + 0],
                (*curve_points)[point_base * 3 + 1],
                (*curve_points)[point_base * 3 + 2]};
        Vec3 p1{(*curve_points)[(point_base + 1) * 3 + 0],
                (*curve_points)[(point_base + 1) * 3 + 1],
                (*curve_points)[(point_base + 1) * 3 + 2]};
        ti.p0 = p0;
        ti.p1 = p1;
        ti.p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}

void AppendHermiteCurveStrands(const float *points, const float *tangents,
                               size_t point_count,
                               const std::vector<int> &counts,
                               const std::vector<float> &widths,
                               unsigned tessellation_segments,
                               const matrix4d &world,
                               std::vector<float> *curve_points,
                               std::vector<float> *curve_radii,
                               std::vector<uint32_t> *first,
                               std::vector<uint32_t> *count,
                               std::vector<TriInfo> *info,
                               Bounds *bounds) {
  if (!points || !tangents || point_count == 0 || !curve_points ||
      !curve_radii || !first || !count) {
    return;
  }
  const unsigned samples = std::max(1u, tessellation_segments);
  size_t cursor = 0;
  auto width_at = [&](size_t index) {
    return index < widths.size() ? widths[index] : 0.01f;
  };
  auto append_sample = [&](const float p[3], float width) {
    const Vec3 world_p = TransformPoint(world, Vec3{p[0], p[1], p[2]});
    curve_points->insert(curve_points->end(), {world_p.x, world_p.y, world_p.z});
    curve_radii->push_back(std::max(1.0e-5f,
                                    0.5f * width * ApproxScale(world)));
    Expand(bounds, world_p);
  };
  for (int c : counts) {
    if (c < 2 || cursor + size_t(c) > point_count) {
      cursor += size_t(std::max(0, c));
      continue;
    }
    first->push_back(uint32_t(curve_points->size() / 3));
    count->push_back(uint32_t((size_t(c) - 1) * samples + 1));
    for (int span = 0; span + 1 < c; ++span) {
      const size_t i0 = cursor + size_t(span);
      const size_t i1 = i0 + 1;
      const float *p0 = points + i0 * 3;
      const float *p1 = points + i1 * 3;
      const float *m0 = tangents + i0 * 3;
      const float *m1 = tangents + i1 * 3;
      for (unsigned sample = 0; sample < samples; ++sample) {
        const float t = float(sample) / float(samples);
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        float p[3];
        for (int k = 0; k < 3; ++k)
          p[k] = h00 * p0[k] + h10 * m0[k] + h01 * p1[k] + h11 * m1[k];
        append_sample(p, width_at(i0) +
                             (width_at(i1) - width_at(i0)) * t);
      }
    }
    const size_t last = cursor + size_t(c) - 1;
    append_sample(points + last * 3, width_at(last));
    if (info) {
      const size_t base = size_t(first->back());
      for (size_t i = 0; i + 1 < size_t(count->back()); ++i) {
        TriInfo ti;
        ti.p0 = Vec3{(*curve_points)[(base + i) * 3 + 0],
                     (*curve_points)[(base + i) * 3 + 1],
                     (*curve_points)[(base + i) * 3 + 2]};
        ti.p1 = Vec3{(*curve_points)[(base + i + 1) * 3 + 0],
                     (*curve_points)[(base + i + 1) * 3 + 1],
                     (*curve_points)[(base + i + 1) * 3 + 2]};
        ti.p2 = Add(ti.p0, Vec3{0.0f, 1.0f, 0.0f});
        ti.base_color = kCurveColor;
        info->push_back(ti);
      }
    }
    cursor += size_t(c);
  }
}

void TraverseDirectPrims(const lightusd::Stage &stage, const lightusd::Prim &prim,
                         const std::unordered_map<std::string, matrix4d> &matrices,
                         double time, DirectScene *direct,
                         std::vector<float> *vertices, std::vector<TriInfo> *tris,
                         Bounds *bounds, std::vector<float> *sphere_data,
                         std::vector<float> *round_points,
                         std::vector<float> *round_radii,
                         std::vector<uint32_t> *round_first,
                         std::vector<uint32_t> *round_count,
                         std::vector<float> *flat_points,
                         std::vector<float> *flat_radii,
                         std::vector<uint32_t> *flat_first,
                         std::vector<uint32_t> *flat_count,
                         std::vector<float> *point_centers,
                         std::vector<float> *point_radii,
                         std::vector<float> *bez_cps,
                         std::vector<float> *tet_aabbs) {
  const std::string path = PrimPathString(prim);
  const matrix4d world = MatrixForPath(matrices, path);
  matrix4d inv_world;
  bool has_inv = lightusd::inverse(world, inv_world, 1.0e-12);

  if (const lightusd::GeomSphere *sphere = prim.as<lightusd::GeomSphere>()) {
    double radius = 2.0;
    EvalAnimFallback(stage, sphere->radius, "radius", time, &radius);
    Vec3 c = TransformPoint(world, Vec3{0.0f, 0.0f, 0.0f});
    float r = float(radius) * ApproxScale(world);
    sphere_data->insert(sphere_data->end(), {c.x, c.y, c.z, r});
    TriInfo ti;
    ti.p0 = c;
    ti.base_color = Vec3{0.35f, 0.48f, 0.80f};
    direct->sphere_info.push_back(ti);
    direct->direct_paths.insert(path);
    Expand(bounds, Add(c, Vec3{r, r, r}));
    Expand(bounds, Sub(c, Vec3{r, r, r}));
  } else if (has_inv) {
    DirectShape shape;
    bool add_shape = false;
    if (const lightusd::GeomCylinder *cyl = prim.as<lightusd::GeomCylinder>()) {
      shape.type = DirectShape::Type::Cylinder;
      EvalAnimFallback(stage, cyl->radius, "radius", time, &shape.radius);
      EvalAnimFallback(stage, cyl->height, "height", time, &shape.height);
      EvalAxis(cyl->axis, &shape.axis);
      add_shape = true;
    } else if (const lightusd::GeomCone *cone = prim.as<lightusd::GeomCone>()) {
      shape.type = DirectShape::Type::Cone;
      EvalAnimFallback(stage, cone->radius, "radius", time, &shape.radius);
      EvalAnimFallback(stage, cone->height, "height", time, &shape.height);
      EvalAxis(cone->axis, &shape.axis);
      add_shape = true;
    } else if (const lightusd::GeomCapsule *cap = prim.as<lightusd::GeomCapsule>()) {
      shape.type = DirectShape::Type::Capsule;
      EvalAnimFallback(stage, cap->radius, "radius", time, &shape.radius);
      EvalAnimFallback(stage, cap->height, "height", time, &shape.height);
      EvalAxis(cap->axis, &shape.axis);
      add_shape = true;
    }
    if (add_shape) {
      shape.world = world;
      shape.inv_world = inv_world;
      direct->shapes.push_back(shape);
      direct->direct_paths.insert(path);
      float e = float(std::max(shape.height * 0.5 + shape.radius, shape.radius)) *
                ApproxScale(world);
      Vec3 c = TransformPoint(world, Vec3{0.0f, 0.0f, 0.0f});
      Expand(bounds, Add(c, Vec3{e, e, e}));
      Expand(bounds, Sub(c, Vec3{e, e, e}));
    }
  }

  if (const lightusd::GeomCube *cube = prim.as<lightusd::GeomCube>()) {
    double size = 2.0;
    EvalAnimFallback(stage, cube->size, "size", time, &size);
    AddDirectCube(size, world, vertices, tris, bounds);
    direct->direct_paths.insert(path);
  } else if (const lightusd::GeomPlane *plane = prim.as<lightusd::GeomPlane>()) {
    double width = 2.0;
    double length = 2.0;
    lightusd::Axis axis = lightusd::Axis::Z;
    EvalAnimFallback(stage, plane->width, "width", time, &width);
    EvalAnimFallback(stage, plane->length, "length", time, &length);
    EvalAxis(plane->axis, &axis);
    AddDirectPlane(width, length, axis, world, vertices, tris, bounds);
    direct->direct_paths.insert(path);
  } else if (const lightusd::GeomPoints *pts = prim.as<lightusd::GeomPoints>()) {
    std::vector<lightusd::value::point3f> points;
    std::vector<float> widths;
    if (EvalAnim(stage, pts->points, "points", time, &points)) {
      EvalAnim(stage, pts->widths, "widths", time, &widths);
      for (size_t i = 0; i < points.size(); i++) {
        Vec3 p = TransformPoint(world, FromPoint3(points[i]));
        float radius = 0.5f * ((i < widths.size()) ? widths[i] : 0.05f) *
                       ApproxScale(world);
        radius = std::max(1.0e-5f, radius);
        point_centers->insert(point_centers->end(), {p.x, p.y, p.z});
        point_radii->push_back(radius);
        TriInfo ti;
        ti.p0 = p;
        ti.base_color = Vec3{0.90f, 0.72f, 0.26f};
        direct->point_info.push_back(ti);
        Expand(bounds, Add(p, Vec3{radius, radius, radius}));
        Expand(bounds, Sub(p, Vec3{radius, radius, radius}));
      }
      direct->direct_paths.insert(path);
    }
  } else if (const lightusd::GeomTetMesh *tet = prim.as<lightusd::GeomTetMesh>()) {
    std::vector<lightusd::value::point3f> points;
    std::vector<lightusd::value::int4> indices;
    if (EvalAnim(stage, tet->points, "points", time, &points) &&
        EvalAnim(stage, tet->tetVertexIndices, "tetVertexIndices", time, &indices)) {
      for (const auto &idx : indices) {
        if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0 || idx[3] < 0 ||
            size_t(idx[0]) >= points.size() || size_t(idx[1]) >= points.size() ||
            size_t(idx[2]) >= points.size() || size_t(idx[3]) >= points.size()) {
          continue;
        }
        TetPrim tp;
        tp.p[0] = TransformPoint(world, FromPoint3(points[size_t(idx[0])]));
        tp.p[1] = TransformPoint(world, FromPoint3(points[size_t(idx[1])]));
        tp.p[2] = TransformPoint(world, FromPoint3(points[size_t(idx[2])]));
        tp.p[3] = TransformPoint(world, FromPoint3(points[size_t(idx[3])]));
        Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
        Vec3 hi{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max()};
        for (const Vec3 &p : tp.p) {
          lo.x = std::min(lo.x, p.x);
          lo.y = std::min(lo.y, p.y);
          lo.z = std::min(lo.z, p.z);
          hi.x = std::max(hi.x, p.x);
          hi.y = std::max(hi.y, p.y);
          hi.z = std::max(hi.z, p.z);
          Expand(bounds, p);
        }
        tet_aabbs->insert(tet_aabbs->end(), {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z});
        direct->tet_prims.push_back(tp);
      }
      direct->direct_paths.insert(path);
    }
  }

  if (const lightusd::GeomBasisCurves *curves = prim.as<lightusd::GeomBasisCurves>()) {
    std::vector<lightusd::value::point3f> points;
    std::vector<int> counts;
    std::vector<float> widths;
    if (EvalAnim(stage, curves->points, "points", time, &points) &&
        EvalAnim(stage, curves->curveVertexCounts, "curveVertexCounts", time, &counts)) {
      EvalAnim(stage, curves->widths, "widths", time, &widths);
      if (curves->normals.authored()) {
        AppendLinearCurveStrands(points, counts, widths, world, flat_points,
                                 flat_radii, flat_first, flat_count,
                                 &direct->flat_curve_info, bounds);
      } else {
        AppendLinearCurveStrands(points, counts, widths, world, round_points,
                                 round_radii, round_first, round_count,
                                 &direct->round_curve_info, bounds);
      }
      direct->direct_paths.insert(path);
    }
  } else if (const lightusd::GeomNurbsCurves *nurbsCurves = prim.as<lightusd::GeomNurbsCurves>()) {
    std::vector<lightusd::value::point3f> points;
    std::vector<int> counts;
    std::vector<float> widths;
    if (EvalAnim(stage, nurbsCurves->points, "points", time, &points) &&
        EvalAnim(stage, nurbsCurves->curveVertexCounts, "curveVertexCounts", time, &counts)) {
      EvalAnim(stage, nurbsCurves->widths, "widths", time, &widths);
      AppendLinearCurveStrands(points, counts, widths, world, round_points,
                               round_radii, round_first, round_count,
                               &direct->round_curve_info, bounds);
      direct->direct_paths.insert(path);
    }
  } else if (const lightusd::GeomNurbsPatch *patch = prim.as<lightusd::GeomNurbsPatch>()) {
    AddNurbsPatchTriangles(stage, *patch, world, time, vertices, tris, bounds);
    direct->direct_paths.insert(path);
  } else if (const lightusd::GeomHermiteCurves *hermiteCurves = prim.as<lightusd::GeomHermiteCurves>()) {
    std::vector<lightusd::value::point3f> points;
    std::vector<lightusd::value::vector3f> tangents;
    std::vector<int> counts;
    std::vector<float> widths;
    if (EvalAnim(stage, hermiteCurves->points, "points", time, &points) &&
        EvalAnim(stage, hermiteCurves->curveVertexCounts, "curveVertexCounts", time, &counts) &&
        EvalAnim(stage, hermiteCurves->tangents, "tangents", time, &tangents)) {
      EvalAnim(stage, hermiteCurves->widths, "widths", time, &widths);
      size_t cursor = 0;
      for (int c : counts) {
        if (c < 2 || cursor + size_t(c) > points.size() ||
            cursor + size_t(c) > tangents.size()) {
          cursor += size_t(std::max(0, c));
          continue;
        }
        for (int i = 0; i + 1 < c; i++) {
          size_t i0 = cursor + size_t(i);
          size_t i1 = i0 + 1;
          Vec3 p0 = TransformPoint(world, FromPoint3(points[i0]));
          Vec3 p1 = TransformPoint(world, FromPoint3(points[i1]));
          Vec3 t0 = TransformVector(world, FromVector3(tangents[i0]));
          Vec3 t1 = TransformVector(world, FromVector3(tangents[i1]));
          float r0 = 0.5f * ((i0 < widths.size()) ? widths[i0] : 0.01f) *
                     ApproxScale(world);
          float r1 = 0.5f * ((i1 < widths.size()) ? widths[i1] : 0.01f) *
                     ApproxScale(world);
          r0 = std::max(1.0e-5f, r0);
          r1 = std::max(1.0e-5f, r1);
          Vec3 b0 = p0;
          Vec3 b1 = Add(p0, Mul(t0, 1.0f / 3.0f));
          Vec3 b2 = Sub(p1, Mul(t1, 1.0f / 3.0f));
          Vec3 b3 = p1;
          bez_cps->insert(bez_cps->end(),
                          {b0.x, b0.y, b0.z, r0, b1.x, b1.y, b1.z, r0,
                           b2.x, b2.y, b2.z, r1, b3.x, b3.y, b3.z, r1});
          TriInfo ti;
          ti.p0 = p0;
          ti.p1 = p1;
          ti.p2 = Add(p0, Vec3{0.0f, 1.0f, 0.0f});
          ti.base_color = Vec3{0.72f, 0.45f, 0.28f};
          direct->bez_curve_info.push_back(ti);
          Expand(bounds, p0);
          Expand(bounds, p1);
        }
        cursor += size_t(c);
      }
      direct->direct_paths.insert(path);
    }
  }

  for (const lightusd::Prim &child : prim.children()) {
    TraverseDirectPrims(stage, child, matrices, time, direct, vertices, tris,
                        bounds, sphere_data, round_points, round_radii,
                        round_first, round_count, flat_points, flat_radii,
                        flat_first, flat_count, point_centers, point_radii,
                        bez_cps, tet_aabbs);
  }
}

bool BuildDirectScene(const lightusd::Stage &stage, const RenderScene &render_scene,
                      const Options &opt, std::vector<float> *vertices,
                      std::vector<TriInfo> *tris, Bounds *bounds,
                      DirectScene *direct, std::string *err) {
  if (!direct || !vertices || !tris || !bounds) return false;
  std::vector<float> sphere_data;
  std::vector<float> round_points, round_radii, flat_points, flat_radii;
  std::vector<float> point_centers, point_radii;
  std::vector<float> bez_cps;
  std::vector<float> tet_aabbs;
  std::vector<uint32_t> round_first, round_count, flat_first, flat_count;
  std::unordered_map<std::string, matrix4d> matrices = BuildNodeMatrixMap(render_scene);
  for (const lightusd::Prim &root : stage.root_prims()) {
    TraverseDirectPrims(stage, root, matrices, opt.timecode, direct, vertices,
                        tris, bounds, &sphere_data, &round_points, &round_radii,
                        &round_first, &round_count, &flat_points, &flat_radii,
                        &flat_first, &flat_count, &point_centers, &point_radii,
                        &bez_cps, &tet_aabbs);
  }

  lrt_tri_build_options build_opts;
  std::memset(&build_opts, 0, sizeof(build_opts));
  build_opts.quality = opt.quality;
  build_opts.layout = LRT_TRI_LAYOUT_AUTO;
  build_opts.num_threads = WorkerThreadCount(opt.threads);
  lrt_result lrt_err = LRT_RESULT_OK;
  if (!sphere_data.empty()) {
    direct->spheres.reset(
        lrt_sphere_scene_build(sphere_data.data(), sphere_data.size() / 4,
                               &build_opts, &lrt_err));
    if (!direct->spheres) {
      if (err) *err = "Failed to build LightRT sphere scene.";
      return false;
    }
  }
  if (!round_first.empty()) {
    lrt_hair_strands strands;
    std::memset(&strands, 0, sizeof(strands));
    strands.points = round_points.data();
    strands.radius = round_radii.data();
    strands.strand_first = round_first.data();
    strands.strand_count = round_count.data();
    strands.nstrands = round_first.size();
    strands.npoints = round_radii.size();
    direct->round_curves.reset(
        lrt_roundcurve_scene_build(&strands, &build_opts, &lrt_err));
    if (!direct->round_curves) {
      if (err) *err = "Failed to build LightRT round curve scene.";
      return false;
    }
  }
  if (!flat_first.empty()) {
    lrt_hair_strands strands;
    std::memset(&strands, 0, sizeof(strands));
    strands.points = flat_points.data();
    strands.radius = flat_radii.data();
    strands.strand_first = flat_first.data();
    strands.strand_count = flat_count.data();
    strands.nstrands = flat_first.size();
    strands.npoints = flat_radii.size();
    direct->flat_curves.reset(
        lrt_flatcurve_scene_build(&strands, &build_opts, &lrt_err));
    if (!direct->flat_curves) {
      if (err) *err = "Failed to build LightRT flat curve scene.";
      return false;
    }
  }
  if (!point_centers.empty()) {
    direct->points.reset(
        lrt_points_scene_build(point_centers.data(), point_radii.data(), nullptr,
                               LRT_POINT_SPHERE, point_radii.size(),
                               &build_opts, &lrt_err));
    if (!direct->points) {
      if (err) *err = "Failed to build LightRT points scene.";
      return false;
    }
  }
  if (!bez_cps.empty()) {
    direct->bez_curves.reset(
        lrt_bezcurve_scene_build(bez_cps.data(), bez_cps.size() / 16,
                                 &build_opts, &lrt_err));
    if (!direct->bez_curves) {
      if (err) *err = "Failed to build LightRT Hermite/Bezier curve scene.";
      return false;
    }
  }
  if (!tet_aabbs.empty()) {
    direct->tets.reset(lrt_user_scene_build(
        tet_aabbs.data(), direct->tet_prims.size(), TetUserIntersect,
        TetUserOccluded, &direct->tet_prims, &build_opts, &lrt_err));
    if (!direct->tets) {
      if (err) *err = "Failed to build LightRT TetMesh user scene.";
      return false;
    }
  }
  return true;
}

bool FindCameraNode(const RenderScene &scene, const Node &node,
                    const std::string &query, const Node **node_out) {
  if (node.nodeType == NodeType::Camera && node.id >= 0 &&
      size_t(node.id) < scene.cameras.size()) {
    const RenderCamera &cam = scene.cameras[size_t(node.id)];
    if (query.empty() || node.abs_path == query || cam.abs_path == query ||
        cam.name == query || node.prim_name == query) {
      *node_out = &node;
      return true;
    }
  }
  for (const Node &child : node.children) {
    if (FindCameraNode(scene, child, query, node_out)) return true;
  }
  return false;
}

const Node *FindCameraNode(const RenderScene &scene, const std::string &query) {
  const Node *result = nullptr;
  for (const Node &root : scene.nodes) {
    if (FindCameraNode(scene, root, query, &result)) return result;
  }
  return nullptr;
}

CameraFrame MakeCameraFrame(const RenderScene &scene, const Options &opt,
                            const Bounds &bounds, int height,
                            lightusd::Axis up_axis) {
  CameraFrame frame;
  const Node *cam_node = FindCameraNode(scene, opt.camera);
  if (!cam_node && !opt.camera.empty()) {
    std::cerr << "WARN: Camera not found: " << opt.camera
              << ". Using auto-fit camera.\n";
  }
  if (cam_node) {
    const RenderCamera &cam = scene.cameras[size_t(cam_node->id)];
    const matrix4d &m = cam_node->global_matrix;
    frame.origin = Vec3{float(m.m[3][0]), float(m.m[3][1]), float(m.m[3][2])};
    frame.right = Normalize(TransformVector(m, Vec3{1.0f, 0.0f, 0.0f}));
    frame.up = Normalize(TransformVector(m, Vec3{0.0f, 1.0f, 0.0f}));
    frame.forward = Normalize(TransformVector(m, Vec3{0.0f, 0.0f, -1.0f}));
    frame.yfov = 2.0f * std::atan(0.5f * cam.verticalAperture /
                                  std::max(1.0e-6f, cam.focalLength));
    frame.xmag = cam.xmag;
    frame.ymag = cam.ymag;
    frame.znear = std::max(1.0e-5f, cam.znear);
    frame.zfar = cam.zfar;
    frame.ortho = cam.projection == lightusd::GeomCamera::Projection::Orthographic;
    return frame;
  }

  Vec3 center{0.0f, 0.0f, 0.0f};
  float radius = 1.0f;
  if (bounds.valid) {
    center = Mul(Add(bounds.lo, bounds.hi), 0.5f);
    radius = std::max(0.001f, Length(Sub(bounds.hi, bounds.lo)) * 0.5f);
  }
  float aspect = (height > 0) ? float(opt.width) / float(height) : 16.0f / 9.0f;
  frame.yfov = 45.0f * 3.14159265358979323846f / 180.0f;
  float distance = radius / std::tan(frame.yfov * 0.5f);
  if (aspect < 1.0f) {
    distance /= aspect;
  }
  Vec3 up_axis_vec{0.0f, 1.0f, 0.0f};
  Vec3 view_dir{0.0f, 0.15f, 1.8f};
  if (up_axis == lightusd::Axis::Z) {
    up_axis_vec = Vec3{0.0f, 0.0f, 1.0f};
    view_dir = Normalize(Vec3{-0.95f, -1.15f, 0.62f});
  } else if (up_axis == lightusd::Axis::X) {
    up_axis_vec = Vec3{1.0f, 0.0f, 0.0f};
    view_dir = Normalize(Vec3{0.62f, -0.95f, -1.15f});
  }
  if (opt.has_view_dir) {
    view_dir = Normalize(opt.view_dir);
  }
  frame.origin = Add(center, Mul(view_dir, distance * opt.fit_scale));
  frame.forward = Normalize(Sub(center, frame.origin));
  frame.right = Normalize(Cross(frame.forward, up_axis_vec));
  if (Length(frame.right) < 1.0e-6f) {
    frame.right = Vec3{1.0f, 0.0f, 0.0f};
  }
  frame.up = Normalize(Cross(frame.right, frame.forward));
  frame.znear = std::max(1.0e-4f, distance * 0.001f);
  frame.zfar = std::max(1000.0f, distance * 10.0f);
  return frame;
}




// ===========================================================================
// `next` lazy-loader RT preview backend (default for USDC inputs).
//
// Loads the USDC with the experimental `next` reader (fast, low-memory, lazy
// arrays) and streams triangles using tydra_next's bit-exact world transforms.
// Produces the byte-identical triangle stream of the legacy path (validated:
// matching per-purpose triangle counts on large scenes). Falls back to the
// legacy eager loader for non-USDC inputs or when -legacyLoad is given.
// ===========================================================================

matrix4d Mat4FromArray(const double d[16]) {
  matrix4d m;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) m.m[i][j] = d[i * 4 + j];
  return m;
}

// Read an array attribute, decoding lazily-stored arrays into a throwaway temp
// (materialized_copy) so the `next` stage's Value stays lazy. This keeps the
// big geometry arrays (points/indices/normals) from being permanently
// materialized into the stage as we stream the scene, bounding peak memory.
// `time` is NaN for the default value, or a frame time for animated (time-
// sampled) arrays (held to the nearest authored sample). Decoding goes through
// materialized_copy so the stage's Value stays lazy.



}  // namespace lusdr
