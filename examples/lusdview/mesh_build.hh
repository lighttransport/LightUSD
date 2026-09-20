// SPDX-License-Identifier: Apache-2.0
// lusdview - convert a Tydra RenderScene into a backend-neutral DrawScene.
#pragma once

#include "gpu_scene.hh"
#include "load_control.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-converter.hh"

namespace lightusd {
class Stage;
}

namespace lusdview {

// Fill DrawMeshCPU::purpose from the source Stage prims. This is intentionally
// separate from RenderScene conversion because RenderMesh does not carry USD
// purpose today.
void ApplyMeshPurposes(const lightusd::Stage& stage, DrawScene* draw);

// Derive the raster preview key light (draw->previewLightDir/Color/hasPreview
// Light) from draw->lights. Called by BuildDrawScene; also usable by the `next`
// loader, which assembles its own DrawScene.
void UpdatePreviewLight(DrawScene* draw);

// Structured tally of load-time diagnostics, derived from the converter's free
// -text warning blob plus the draw-side skipped list. Lets the app print a
// greppable, machine-parseable end-of-load summary (renderer-parity work) and
// lets the smoke harness distinguish a full material fallback (degraded) from a
// benign missing normal-map texture.
struct LoadDiagnostics {
  int degraded_material = 0;   // material rendered through a degraded surface
  int missing_texture = 0;     // a texture/image failed to load or resolve
  int unsupported_mtlx = 0;    // a MaterialX node could not be evaluated
  int unsupported_lobes = 0;   // material outputs not evaluated in real time
  int skipped = 0;             // draw-side skipped items (UDIM/undecoded/empty)
  int other = 0;               // any other warning line
  // Authored light inventory and light-specific diagnostics. These remain
  // separate from `skipped` so reports identify the affected USD feature and
  // still preserve the legacy aggregate counters used by smoke tests.
  int geometry_lights = 0;
  int portal_lights = 0;
  int ies_profiles = 0;
  int emissive_mesh_lights = 0;
  std::vector<std::string> examples;  // a few representative lines (capped)
  int total() const {
    return degraded_material + missing_texture + unsupported_mtlx +
           unsupported_lobes + skipped + other;
  }
  // Diagnostics that indicate a real rendering shortfall (as opposed to the
  // `other` bucket, which is dominated by tydra's informational MaterialX
  // progress messages). The app only emits a summary when this is non-zero.
  int actionable() const {
    return degraded_material + missing_texture + unsupported_mtlx +
           unsupported_lobes + skipped;
  }
};

// Categorize the converter warning blob (newline-joined) + the draw-side skipped
// list into a LoadDiagnostics tally by matching the stable tydra message texts.
LoadDiagnostics CategorizeLoadWarnings(const std::string& warn_blob,
                                       const std::vector<std::string>& skipped);

// Add authored light feature counts to an existing warning classification.
// Geometry/portal counts describe the scene inventory; IES and emissive-mesh
// counts describe payloads that reached the backend-neutral light record.
void AddLightDiagnostics(const DrawScene& draw, LoadDiagnostics* diagnostics);

// Compatibility hook for material diagnostics. OpenPBR surface and homogeneous
// volume controls are currently packed and consumed by all preview backends,
// so this intentionally does not add a skipped/degraded record.
void DiagnoseUnsupportedRealtimeLobes(const DrawMaterialCPU& material,
                                      DrawScene* draw);

// Convert `rs` (already triangulated + single-indexed by the converter) into a
// renderable DrawScene: interleaved vertices, per-material submeshes, world
// transforms (from the node hierarchy), decoded RGBA8 textures and a world-space
// AABB. Unsupported items (UDIM/undecoded textures, empty meshes) are skipped
// and recorded in out->skipped.
//
// `ctrl` (optional) makes the build cancellable and bounds it by the triangle /
// vertex-byte budget; when a budget is hit the build stops and out->truncated
// is set (prevents per-frame freeze and VRAM thrashing on huge scenes).
// `stage` (optional) supplies the source Stage so per-mesh extras not carried by
// RenderMesh can be read -- currently blendshape in-between shapes (read from the
// BlendShape prims and remapped to DrawVertex order alongside the primary target).
void BuildDrawScene(const lightusd::tydra::RenderScene& rs, DrawScene* out,
                    LoadControl* ctrl = nullptr,
                    const lightusd::Stage* stage = nullptr,
                    const TextureRuntimeOptions& textureOptions = {});

// Build DrawVolumeCPU entries from RenderScene::volumes (UsdVol / OpenVDB).
void BuildDrawVolumes(const lightusd::tydra::RenderScene& rs, DrawScene* out);

// Build a DrawCameraCPU from a row-major world matrix and a tydra::RenderCamera
// (legacy loader). Caller sets `name`, `absPath`, `displayName`.
DrawCameraCPU MakeDrawCameraFromTydra(
    const lightusd::value::matrix4d& worldMatrix,
    const lightusd::tydra::RenderCamera& cam);

// Build camera records from a legacy Tydra RenderScene by walking its
// node tree for Camera nodes and extracting world pose + lens from the parallel
// RenderCamera. Used for loader-equivalence testing.
void BuildDrawCameras(const lightusd::tydra::RenderScene& rs, DrawScene* out);

// Streaming variant: run `converter.ConvertToRenderSceneStreaming` and build the
// DrawScene incrementally as elements are produced (mesh geometry as each mesh
// converts, world placement when the node hierarchy is built, textures and
// materials on completion). Produces the same DrawScene as
// ConvertToRenderScene + BuildDrawScene, while also fully populating `render`.
// Returns the conversion result. `ctrl` bounds the build by the triangle /
// vertex-byte budget (draw-side truncation; conversion still completes).
bool BuildDrawSceneStreaming(lightusd::tydra::RenderSceneConverter& converter,
                             const lightusd::tydra::RenderSceneConverterEnv& env,
                             lightusd::tydra::RenderScene* render, DrawScene* out,
                             LoadControl* ctrl = nullptr,
                             const TextureRuntimeOptions& textureOptions = {});

// --- Texture post-passes, shared by the legacy and `--next` scene loaders ----
void ClassifyTextureUsage(DrawScene* out);
// Encode the already-decoded DrawScene textures to a GPU block format
// (`--texture-compress`), cap-gated by TextureRuntimeOptions::caps. Split out of
// ApplyTextureRuntimeOptions (which also does the size cap / byte budget) so the
// `--next` loader — whose own texture decoder already applies those — can call
// just this one.
void ApplyTextureCompression(const TextureRuntimeOptions& opt, DrawScene* out);
void CompressDrawTexture(const TextureRuntimeOptions& opt, DrawTextureCPU* texture);
// Release the process-wide texture GPU processor while the Vulkan loader and
// renderer are still alive. The processor owns a private Vulkan device table;
// deferring its destructor to static shutdown can outlive the renderer's volk
// state on Vulkan.
void ShutdownTextureGpu();

// Classify texture usage from the built materials, then build the content-aware
// CPU mip chains (and per-level compressed payloads when compression is on).
// Must run after the materials exist.
void FinalizeDrawTextures(const TextureRuntimeOptions& opt, DrawScene* out);

#if defined(LUSDVIEW_WITH_TEXTOOLS)
// Kept-compressed KTX2 passthrough from raw `.ktx2` bytes: parse it, adapt its
// blocks to what the device can sample (upload as-is / transcode uni / decode),
// carry its mip chain, and fill `tex` — no decode + re-encode. Returns false when
// the file can't be used (caller falls back to the normal decode path). Used by
// the `--next` loader, which reads the asset itself; the legacy path has an
// equivalent that starts from the tydra RenderScene's block buffer.
bool BuildKeptCompressedFromKtx2(const uint8_t* data, size_t size,
                                 const TextureRuntimeOptions& opt,
                                 DrawTextureCPU* tex);
#endif

}  // namespace lusdview
