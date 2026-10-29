// SPDX-License-Identifier: Apache-2.0
// lusdrender — the RenderContext aggregator (persistent render state shared by
// the next-loader pipeline) plus the cross-TU function prototype surface (added
// as the monolith is split into per-module .cc files).
#pragma once

#include <functional>

#include "next/eval/value-clip.hh"
#include "tydra/next/render-extract.hh"
#include "lusdr_types.hh"
#ifdef HAVE_CUDA_RT
#include "gpu_scene.hh"
#endif

namespace lusdr {

struct RenderContext {
  lightusd::next::Stage stage;  // keeps the lazy point/index arrays alive
  // Loader used by procedural/value-clip curves. It is populated while the
  // composed session is still alive so relative clip assets can use its layer
  // dependency anchors.
  lightusd::next::ValueClipStageLoader clip_stage_loader;
  // Flat (no-instance) path buffers: default allocator so the shared
  // Shade/RenderImage signatures stay std::vector. The big, OOM-prone instanced
  // geometry lives in the budget-tracked Blas buffers below.
  std::vector<float> vertices;  // packed triangle positions (flat-path BVH input)
  std::vector<FlatTri> tris;    // slim per-triangle: geometry + purpose + mat_id
  std::vector<TriMat> flat_mats;  // flat-path material table (one per mesh-job)
  std::vector<lightusd::tydra::LightRtOpenPBRParams> flat_openpbr_mats;
  std::vector<Texture> textures;  // diffuse textures referenced by flat_mats[].tex_id
  std::vector<float> tri_uvs;  // 6 floats/tri (parallel to tris); empty if none
  ByteVec tri_colors;  // 12 bytes/tri (per-corner RGBA8); empty if none
  std::vector<float> tri_normals;  // 9 floats/tri (per-corner normals); empty if none
  std::vector<VolumeData> volumes;  // UsdVol volumes (OpenVDB) for raymarching
  Bounds bounds;
  Bounds geometry_bounds;
  RTPreviewStats stats;
  lrt_tri_scene *scene{nullptr};  // owned flat BVH (no-instance path)
  std::vector<TriangleSceneChunk> triangle_chunks;
  // Two-level (instanced) BVH path: built when the composed scene has native
  // instances. blas[0] is the base (non-instanced) geometry; blas[1..] are the
  // unique prototypes. instances[] place them; tlas is the top-level BVH.
  std::vector<Blas> blas;
  std::vector<InstanceRT> instances;
  lrt_tlas *tlas{nullptr};
  bool use_tlas{false};
  DirectScene direct;             // empty for the next path
  LightCache lights;              // empty -> camera-headlight fallback
  IblCache ibl;                   // image-based lighting (--env / DomeLight)
  lightusd::Axis up_axis{lightusd::Axis::Y};
  CameraFrame camera;
  std::vector<BackPlateImage> backplates;
  Options opt;  // mutable render parameters (width/height/ambient/bg/...)
  int width{960};
  int height{540};
  // Time at which geometry + transforms are evaluated (NaN = default value).
  double frame_time{std::numeric_limits<double>::quiet_NaN()};
  // Once the initial extraction has proved that rendered Mesh geometry is
  // static, authored non-time-sampled geometry can be dropped from the
  // composed Stage. Animated scenes retain it for per-frame re-streaming.
  bool geometry_animated{false};
  size_t released_static_geometry_bytes{0};
  double load_seconds{0.0}, stream_seconds{0.0}, bvh_seconds{0.0};

  // Free the TLAS before the BLAS scenes it references (blas[] destructs after
  // this body runs).
  ~RenderContext() {
    if (tlas) lrt_tlas_free(tlas);
    if (scene) lrt_tri_scene_free(scene);
  }
  RenderContext() = default;
  RenderContext(const RenderContext &) = delete;
  RenderContext &operator=(const RenderContext &) = delete;
};

// ---- lusdr_args.cc / lusdr_material.cc ----
bool ParseIntStrict(const std::string &s, int *out);

bool ParseFloatStrict(const std::string &s, float *out);

bool ParseDoubleStrict(const std::string &s, double *out);

bool ParseColor(const std::string &s, Vec3 *out);

void PrintUsage(const char *prog);

bool ParseArgs(int argc, char **argv, Options *opt);

void SetupNullAssetResolution(lightusd::AssetResolutionResolver *resolver);

Vec3 MaterialColor(const RenderScene &scene, const RenderMesh &mesh,
                   int material_id);

Vec3 MaterialEmission(const RenderScene &scene, int material_id);

float MaterialRoughness(const RenderScene &scene, int material_id);

float MaterialMetallic(const RenderScene &scene, int material_id);
// Constant opacity (surface opacity x constant displayOpacity) and the
// UsdPreviewSurface alpha-cutout threshold; see lusdr_material.cc.
float MaterialOpacity(const RenderScene &scene, const RenderMesh &mesh,
                      int material_id);
float MaterialOpacityThreshold(const RenderScene &scene, int material_id);

Vec3 MeshLightEmission(const RenderScene &scene, const RenderMesh &mesh,
                       int material_id, float total_area);

// ---- lusdr_lighting.cc ----
void AppendPowerCdf(std::vector<PreviewLight> *lights, std::vector<float> *cdf);

Vec3 DirectionFromLatlong(float u, float v);

void LatlongUV(const Vec3 &dir, float *u, float *v);

Vec3 SampleEnvNearest(const EnvImage &img, float u, float v);

Vec3 SampleEnv(const EnvImage &img, const Vec3 &dir);

bool DecodeTextureToEnvImage(const RenderScene &scene, int texture_id,
                             EnvImage *out);

// Resample a light-probe environment (UsdLux texture:format "mirroredBall" = 2
// or "angular" = 3, the RenderLight::DomeTextureFormat values) into the latlong
// layout every sampler here assumes; other formats pass through untouched.
// Same probe mapping as lusdview's TexToolsProbeToEquirect, so both tools read
// a probe identically.
EnvImage RemapProbeToLatlong(EnvImage &&env, int format);

EnvImage ConvolveDiffuseEnv(const EnvImage &env, int width, int height);

EnvImage PrefilterEnvMip(const EnvImage &env, int width, int height,
                         float roughness);

float RadicalInverseVdc(uint32_t bits);

Vec3 ImportanceSampleGGX(float xi0, float xi1, float roughness, const Vec3 &n);

float GeometrySchlickGGX(float ndotv, float roughness);

float GeometrySmith(float ndotv, float ndotl, float roughness);

void BuildBrdfLut(int size, IblCache *ibl);

bool BuildIblFromEnv(EnvImage &&env, IblCache *ibl);
// -ibl envmap: switch BuildIblFromEnv to the vendored envmap-library backend
// (opt-in; no-op when built without LUSDR_WITH_TEXTOOLS).
void SetIblBackendEnvmap(bool enabled);

bool BuildIblCache(const RenderScene &scene, const LightCache &lights,
                   IblCache *ibl);

bool LoadEnvImageFromFile(const std::string &path, const Vec3 &scale,
                          EnvImage *out);
bool LoadBackPlateImage(const std::string &color_path,
                        const std::string &alpha_path,
                        const std::string &depth_path,
                        BackPlateImage *out);

Vec3 SampleIblMip(const std::vector<EnvImage> &mips, const Vec3 &dir,
                  float roughness);

void SampleBrdfLut(const IblCache &ibl, float ndotv, float roughness, float *a,
                   float *b);

float RectArea(const RenderLight &light);

float DiskArea(const RenderLight &light);

float SphereArea(const RenderLight &light);

float CylinderArea(const RenderLight &light);

Vec3 LightColor(const RenderLight &light);

void AddFiniteLight(const RenderLight &light, PreviewLight::Kind kind,
                    LightCache *cache);

void CollectLights(const RenderScene &scene, LightCache *cache);

// ---- lusdr_geom.cc ----
void ExpandBoundsByVolume(const std::vector<VolumeData> &vols, Bounds *b);
lightusd::Axis GetUpAxis(const std::string &up);
bool BuildNodeMatrixMap(const Node &node,
                        std::unordered_map<std::string, matrix4d> *map);

std::unordered_map<std::string, matrix4d> BuildNodeMatrixMap(
    const RenderScene &scene);

matrix4d MatrixForPath(const std::unordered_map<std::string, matrix4d> &map,
                       const std::string &path);

Vec3 TransformNormal(const matrix4d &inv_world, const Vec3 &n);

int AxisIndex(lightusd::Axis axis);

Vec3 AxisVec(lightusd::Axis axis);

float Coord(const Vec3 &v, int axis);

Vec3 WithCoord(Vec3 v, int axis, float c);

Vec3 RadialPart(Vec3 v, int axis);

bool SolveQuadratic(float a, float b, float c, float *t0, float *t1);

bool IntersectTriangleMT(const Vec3 &o, const Vec3 &d, const Vec3 &a,
                         const Vec3 &b, const Vec3 &c, float tmin,
                         float tmax, float *t);

bool IntersectTetPrim(const TetPrim &tet, const Vec3 &o, const Vec3 &d,
                      float tmin, float tmax, float *best_t, Vec3 *normal);

int TetUserIntersect(const lrt_ray *ray, uint32_t prim_id, void *user,
                     float *t, float *u, float *v);

int TetUserOccluded(const lrt_ray *ray, uint32_t prim_id, void *user);

bool AcceptT(float t, float tmin, float tmax, float *best);

bool IntersectDirectShape(const DirectShape &shape, const Vec3 &ray_org,
                          const Vec3 &ray_dir, float tmin, float tmax,
                          DirectHit *hit);

float TriangleArea(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2);

// ---- lusdr_integrator.cc ----
bool PurposeVisible(uint32_t purpose_bit, uint32_t purpose_mask);
unsigned WorkerThreadCount(int requested);
int PurposeAnyHitFilter(void *user, uint32_t prim_id, float, float, float);

bool IntersectVisibleTriangles(
    lrt_tri_scene *scene, const std::vector<FlatTri> &tris,
    const lrt_ray &ray, uint32_t purpose_mask, lrt_hit *hit,
    size_t tri_first = 0, size_t tri_count = (std::numeric_limits<size_t>::max)());

bool IntersectVisibleTriangleChunks(
    const std::vector<TriangleSceneChunk> &chunks,
    const std::vector<FlatTri> &tris, const lrt_ray &ray,
    uint32_t purpose_mask, lrt_hit *hit);

bool Occluded(lrt_tri_scene *scene, const std::vector<FlatTri> &tris,
              const Vec3 &p, const Vec3 &n, const Vec3 &l, float max_t,
              const DirectScene *direct, uint32_t purpose_mask,
              const std::vector<TriangleSceneChunk> *chunks = nullptr);

bool OccludedTLAS(const lrt_tlas *tlas, const Vec3 &p, const Vec3 &n,
                  const Vec3 &l, float max_t);

bool IntersectDirectScene(const DirectScene *direct, const Vec3 &ray_org,
                          const Vec3 &ray_dir, float tmin, float tmax,
                          DirectHit *best);

Vec3 PerturbNormalStorm(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                        const Vec3 &N, float u0, float v0, float u1, float v1,
                        float u2, float v2, const Vec3 &Nt);

Vec3 SampleTangentNormal(const Texture &nm, float u, float v, float lod);

float TextureLod(float dudx, float dvdx, float dudy, float dvdy, int w, int h);

bool ComputeUVFootprint(const Vec3 &org, const Vec3 &dir, const RayDiff &rd,
                        const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                        const Vec3 &N, float u0, float v0, float u1, float v1,
                        float u2, float v2, float *dudx, float *dvdx,
                        float *dudy, float *dvdy);

bool ResolveTLASHit(const lrt_tlas_hit &th, const std::vector<Blas> &blas,
                    const std::vector<InstanceRT> &instances,
                    const std::vector<Texture> *textures, const Vec3 &ray_org,
                    const Vec3 &ray_dir, const RayDiff &rd, TriInfo *out,
                    const lightusd::tydra::LightRtOpenPBRParams **out_openpbr =
                        nullptr);

Vec3 Shade(lrt_tri_scene *scene, const DirectScene *direct,
           const std::vector<FlatTri> &tris, const std::vector<TriMat> &mats,
           const LightCache &lights, const IblCache *ibl,
           const CameraFrame &camera,
           const Options &opt, const Vec3 &ray_org, const Vec3 &ray_dir,
           const std::vector<Texture> *textures = nullptr,
           const std::vector<float> *tri_uvs = nullptr,
           const lrt_tlas *tlas = nullptr,
           const std::vector<Blas> *blas = nullptr,
           const std::vector<InstanceRT> *instances = nullptr,
           const RayDiff &rd = RayDiff{}, int depth = 0,
           const ByteVec *tri_colors = nullptr,
           const std::vector<float> *tri_normals = nullptr,
           const std::vector<lightusd::tydra::LightRtOpenPBRParams>
               *openpbr_mats = nullptr,
           // This ray is a BSDF-sampled indirect bounce, not a camera/transmission
           // ray. Two contributions are then already accounted for at the surface
           // that spawned it and must NOT be gathered again:
           //   - the environment / dome, which the split-sum IBL term integrates
           //     over the whole lobe, so an escaping bounce contributes nothing;
           //   - the emission of an analytic mesh light, which direct lighting
           //     already delivers (TriInfo::area_light marks those triangles).
           // Without this, both are counted twice in `-materialShading lightrt-bsdf`.
           bool indirect = false,
           const std::vector<TriangleSceneChunk> *triangle_chunks = nullptr,
           float *primary_hit_t = nullptr);

uint8_t ToSRGB8(float linear);

void MakeRay(const CameraFrame &camera, float aspect, float sx, float sy,
             Vec3 *org, Vec3 *dir);

std::vector<VolumeData> BuildVolumes(const RenderScene &scene);

Vec3 CompositeVolumes(const std::vector<VolumeData> &vols, const Vec3 &worg,
                      const Vec3 &wdir, Vec3 bg);

lightusd::Image RenderImage(lrt_tri_scene *scene, const DirectScene *direct,
                            const std::vector<FlatTri> &tris,
                            const std::vector<TriMat> &mats,
                            const LightCache &lights, const IblCache *ibl,
                            const CameraFrame &camera, const Options &opt,
                            int height,
                            const std::vector<Texture> *textures = nullptr,
                            const std::vector<float> *tri_uvs = nullptr,
                            const lrt_tlas *tlas = nullptr,
                            const std::vector<Blas> *blas = nullptr,
                            const std::vector<InstanceRT> *instances = nullptr,
                            const ByteVec *tri_colors = nullptr,
                            const std::vector<float> *tri_normals = nullptr,
                            const std::vector<VolumeData> *volumes = nullptr,
                            const std::vector<lightusd::tydra::LightRtOpenPBRParams>
                                *openpbr_mats = nullptr,
                            const std::vector<TriangleSceneChunk> *triangle_chunks = nullptr,
                            const std::vector<BackPlateImage> *backplates = nullptr);

bool LoadProgress(float progress, void *);

// ---- next-loader / driver (defined in lusdrender.cc for now) ----
bool BuildRenderContext(const Options &opt, RenderContext &ctx);
bool ExtractAndBuildBVH(RenderContext &ctx, double time);
void RefreshVolumesNext(RenderContext &ctx, double time);
bool BuildNextGaussianEllipses(const lightusd::next::Stage &stage,
                               RenderContext &ctx, double time,
                               bool defer_gpu_bvh = false);

// Materialize one deferred Gaussian chunk immediately before a GPU trace.
// The input arrays are released after the LightRT scene builds its acceleration
// data, keeping only one CPU BVH and one input chunk live.
bool BuildDeferredGaussianChunk(const Options &opt, EllipseSceneChunk *chunk);
void PrintRTStats(const RenderContext &ctx);
double RenderFrameTo(RenderContext &ctx, const std::string &path);
void ResolveCameraNext(RenderContext &ctx);

#ifdef LIGHTUSD_WITH_QJS
int RunJSScriptMode(const Options &opt, const std::string &script_path);
int RunMCPMode(const Options &opt);
#endif

#ifdef LUSDRENDER_WITH_STREAM
// lusdr_stream.cc — WebSocket browser streaming server (orbit/pan/dolly from the
// browser, frames pushed as JPEG/QOI/PNG). Blocks until stopped.
int RunStreamServer(const Options &opt);
#endif

#ifdef HAVE_VULKAN
bool RunVulkanLightRT(const Options &opt,
                      const std::vector<Vec3> &base_colors,
                      std::vector<RTPreviewStats::MeshGeometry> &geos,
                      const std::vector<ResolvedMat> &materials,
                      const std::vector<Texture> &textures,
                      const LightCache &lights,
                      const CameraFrame &camera, int height);
bool RunVulkanGaussianLightRT(const Options &opt, DirectScene *direct,
                              const CameraFrame &camera, int height);
#endif

#ifdef HAVE_D3D11
bool RunD3D11LightRT(const Options &opt,
                      const std::vector<Vec3> &base_colors,
                      std::vector<RTPreviewStats::MeshGeometry> &geos,
                      const CameraFrame &camera, int height);
#endif

#ifdef HAVE_HIP
bool RunHipLightRT(const Options &opt,
                   const std::vector<Vec3> &base_colors,
                   std::vector<RTPreviewStats::MeshGeometry> &geos,
                   const CameraFrame &camera, int height);
bool RunHipSharedRT(const Options &opt,
                    const std::vector<Vec3> &base_colors,
                    std::vector<RTPreviewStats::MeshGeometry> &geos,
                    const std::vector<ResolvedMat> &materials,
                    const std::vector<Texture> &textures,
                    const LightCache &lights,
                    const CameraFrame &camera, int height);
bool RunHipGaussianLightRT(const Options &opt, DirectScene *direct,
                           const CameraFrame &camera, int height);
#endif

#ifdef HAVE_CUDA_RT
bool BuildSharedDrawScene(
    const std::vector<Vec3> &base_colors,
    const std::vector<RTPreviewStats::MeshGeometry> &geos,
    const std::vector<ResolvedMat> &materials,
    const std::vector<Texture> &textures, const LightCache &lights,
    const std::string &asset_path, lusdview::DrawScene *scene);
bool RunCudaSharedRT(const Options &opt,
                     const std::vector<Vec3> &base_colors,
                     std::vector<RTPreviewStats::MeshGeometry> &geos,
                     const std::vector<ResolvedMat> &materials,
                     const std::vector<Texture> &textures,
                     const LightCache &lights,
                     const CameraFrame &camera, int height);
#endif

// ---- lusdr_next.cc (next loader + driver) ----
unsigned WorkerThreadCount(int requested);
bool PurposeVisible(uint32_t purpose_bit, uint32_t purpose_mask);
CameraFrame MakeCameraFrame(const RenderScene &scene, const Options &opt,
                            const Bounds &bounds, int height,
                            lightusd::Axis up_axis);
matrix4d Mat4FromArray(const double d[16]);
uint32_t PurposeBit(lightusd::Purpose purpose);

std::vector<float> ReadFloatArrayLazy(const lightusd::next::UsdPrim &prim,
                                      const char *name, double time);

bool ReadFloatArrayViewLazy(
    const lightusd::next::UsdPrim &prim, const char *name, double time,
    lightusd::tydra::next::ValueArrayRead<float> *out);

// Large compressed SH payloads are optional for preview: only the DC RGB
// coefficient is consumed, so avoid materializing an oversized array.
bool AllowGaussianSHDecode(const lightusd::next::UsdPrim &prim);

std::vector<int32_t> ReadIntArrayLazy(const lightusd::next::UsdPrim &prim,
                                      const char *name, double time);

std::vector<int64_t> ReadInt64ArrayLazy(const lightusd::next::UsdPrim &prim,
                                        const char *name, double time);

std::string DirName(const std::string &path);

bool UsdzEntryMatches(const std::string &entry, const std::string &asset);


lightusd::next::UsdPrim ConnectedPrimNext(const lightusd::next::Stage &stage,
                                          const lightusd::next::UsdPrim &prim,
                                          const std::string &prop);

WrapMode ParseWrapMode(const std::string &s);

void ResolveScalarTextureNext(const lightusd::next::Stage &stage,
                              const lightusd::next::UsdPrim &surf,
                              const std::string &input, TextureCache &tc,
                              ScalarTex *out);

UvXform ResolveUvXform(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &uvtex);

void ResolveMeshMaterialNext(const lightusd::next::Stage &stage,
                             const lightusd::next::UsdPrim &mesh,
                             TextureCache &tc, Vec3 *base_color, int32_t *tex_id,
                             float *roughness, float *metallic,
                             int32_t *normal_tex_id, UvXform *uv_xform,
                             ScalarTex *rough_tex, ScalarTex *metal_tex,
                             Vec3 *emission, int32_t *emission_tex_id,
                             float *occlusion, ScalarTex *occ_tex,
                             float *opacity = nullptr,
                             ScalarTex *opacity_tex = nullptr,
                             float *opacity_threshold = nullptr,
                             float *clearcoat = nullptr,
                             float *clearcoat_roughness = nullptr,
                             ScalarTex *clearcoat_tex = nullptr,
                             ScalarTex *clearcoat_rough_tex = nullptr,
                             Vec3 *specular_color = nullptr,
                             int32_t *specular_tex_id = nullptr,
                             float *ior = nullptr,
                             uint8_t *use_specular_workflow = nullptr,
                             bool *vertex_color = nullptr,
                             float *displacement = nullptr,
                             ScalarTex *displacement_tex = nullptr);

void ResolveMeshMaterialCached(
    const lightusd::next::Stage &stage, const lightusd::next::UsdPrim &mesh,
    TextureCache &tc, std::unordered_map<std::string, ResolvedMat> &cache,
    MeshJobNext *job);

bool PathMatchesMask(const std::string &path,
                     const std::vector<std::string> &mask);

bool PrimHasAnimatedXform(const lightusd::next::UsdPrim &prim);

bool MeshHasAnimatedGeom(const lightusd::next::UsdPrim &prim);

bool SubtreeGeometryAnimated(const lightusd::next::UsdPrim &prim,
                             const std::vector<std::string> &mask,
                             bool ancestor_xform_animated);

bool SceneGeometryAnimated(const lightusd::next::Stage &stage,
                           const std::vector<std::string> &mask);

// When `expand_instancers` is true, UsdGeomPointInstancer prims are EXPANDED in
// place: every visible instance's prototype meshes are emitted as world-space
// MeshJobNext (transform = prototype-local * instance TRS * instancer world),
// recursively (nested instancers expand too). This is the GPU flatten path's
// only way to render instanced geometry (no GPU TLAS). Default false keeps the
// two-level proto-collection callers byte-identical (they stop at instancers and
// place prototype BLAS via the TLAS instead).
//
// `max_jobs` (0 = unlimited) caps the emitted-job count: once `jobs` reaches it,
// instancer expansion stops early. This bounds host memory on scenes with tens of
// millions of instances (e.g. Moana island) -- the -vkInstanced collector passes
// a budget so a huge instancer yields a bounded preview instead of OOMing.
void CollectRTPreviewMeshesNext(const lightusd::next::Stage &stage,
                                const lightusd::next::UsdPrim &prim,
                                const matrix4d &parent_world,
                                lightusd::Purpose inherited_purpose, double time,
                                const std::vector<std::string> &mask,
                                std::vector<MeshJobNext> *jobs,
                                bool expand_instancers = false,
                                size_t max_jobs = 0);

// Streaming instance sink: called once per emitted placement with the prototype
// mesh prim, its world transform, and purpose. Same (prim, world, purpose) an
// expanded MeshJobNext would carry -- the two-level -vkInstanced collector uses
// this to group placements on the fly instead of materializing one MeshJobNext
// per instance (each ~392 B); a large instanced scene then costs ~88 B/placement
// of host memory during collection instead of ~392 B, so Moana island fits in far
// less RAM. See CollectRTInstancePlacementsNext.
using RtInstanceSink = std::function<void(const lightusd::next::UsdPrim & /*prim*/,
                                          const matrix4d & /*world*/,
                                          lightusd::Purpose /*purpose*/)>;

// Streaming counterpart of CollectRTPreviewMeshesNext(expand_instancers=true):
// traverses `prim`, expands PointInstancer / native instances, and delivers each
// placement to `sink` instead of appending a MeshJobNext. Stops after
// `max_placements` sink calls (0 = unlimited). Returns the number of placements
// emitted (so the caller can report a hit budget).
size_t CollectRTInstancePlacementsNext(const lightusd::next::Stage &stage,
                                       const lightusd::next::UsdPrim &prim,
                                       const matrix4d &parent_world,
                                       lightusd::Purpose inherited_purpose,
                                       double time,
                                       const std::vector<std::string> &mask,
                                       const RtInstanceSink &sink,
                                       size_t max_placements);

float ReadCamFloatNext(const lightusd::next::UsdPrim &prim, const char *name,
                       float fallback);

bool FindNextCameraFrameRecursive(const lightusd::next::Stage &stage,
                                  const lightusd::next::UsdPrim &prim,
                                  const matrix4d &parent_world,
                                  const std::string &query, double time,
                                  CameraFrame *frame, float *aspect);

bool FindNextCameraFrame(const lightusd::next::Stage &stage,
                         const std::string &query, double time,
                         CameraFrame *frame, float *aspect);

CameraFrame MakeUsdRecordCamera(const Bounds &bounds, lightusd::Axis up_axis,
                                int width, int *out_height);

bool IsCurvePrimNext(const lightusd::next::UsdPrim &prim);

std::vector<lightusd::value::point3f> ReadCurvePointsNext(
    const lightusd::next::UsdPrim &prim, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader);

bool BuildNextCurves(RenderContext &ctx, const std::vector<CurveJobNext> &jobs,
                     double time, bool include_flat = true);

// Convert authored flat/ribbon curves to bounded camera-facing triangle
// carriers for GPU backends. The CPU path continues to use LightRT's native
// flat-curve intersector.
bool BuildNextFlatCurveMeshes(
    const std::vector<CurveJobNext> &jobs, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader,
    const CameraFrame &camera, std::vector<RTPreviewStats::MeshGeometry> *geos,
    std::vector<Vec3> *base_colors);

bool BuildNextFlatCurveBounds(
    const std::vector<CurveJobNext> &jobs, double time,
    const lightusd::next::ValueClipStageLoader &clip_loader, Bounds *bounds);

matrix4d InstanceTRS(const float *pos, const float *quat_xyzw,
                     const float *scale3);

void CollectCurvesNextRec(const lightusd::next::UsdPrim &prim,
                          const matrix4d &parent_world,
                          lightusd::Purpose inherited_purpose, double time,
                          std::vector<CurveJobNext> *out);

void CollectProtoCurves(const lightusd::next::Stage &stage,
                        const std::string &proto_path,
                        lightusd::Purpose start_purpose, double time,
                        std::vector<CurveJobNext> *out);

int32_t ReserveCurveProto(const lightusd::next::Stage &stage,
                          const std::string &proto_path,
                          lightusd::Purpose purpose, double time,
                          CurveProtoCollect *curve_inst);

void CollectPointInstancer(const lightusd::next::Stage &stage,
                           const lightusd::next::UsdPrim &instancer,
                           const matrix4d &instancer_world,
                           lightusd::Purpose purpose, double time,
                           const std::vector<std::string> &mask,
                           std::vector<InstanceRT> *instances,
                           std::unordered_map<std::string, uint32_t> *proto_ids,
                           std::vector<ProtoBuildReq> *protos,
                           CurveProtoCollect *curve_inst, RTPreviewStats *stats,
                           // When set, curve placements go here (in `instancer_world`
                           // space) instead of curve_inst->instances -- used to
                           // capture a NESTED instancer's curve placements per
                           // prototype for later flattening. Curve prototypes are
                           // still deduped into curve_inst.
                           std::vector<CurveInstanceRT> *curve_out = nullptr);

void CollectSceneSplit(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &prim,
                       const matrix4d &parent_world,
                       lightusd::Purpose inherited_purpose, double time,
                       const std::vector<std::string> &mask,
                       std::vector<MeshJobNext> *base_jobs,
                       std::vector<InstanceRT> *instances,
                       std::unordered_map<std::string, uint32_t> *proto_ids,
                       std::vector<ProtoBuildReq> *protos,
                       std::vector<CurveJobNext> *curve_jobs,
                       CurveProtoCollect *curve_inst, RTPreviewStats *stats,
                       const std::unordered_set<std::string> *proto_holders);

void CollectProtoMeshNestingRec(
    const lightusd::next::Stage &stage, const lightusd::next::UsdPrim &prim,
    const matrix4d &parent_world, lightusd::Purpose inherited_purpose, double time,
    const std::vector<std::string> &mask, std::vector<InstanceRT> *nested,
    std::vector<CurveInstanceRT> *nested_curves,
    std::unordered_map<std::string, uint32_t> *proto_ids,
    std::vector<ProtoBuildReq> *protos, CurveProtoCollect *curve_inst,
    RTPreviewStats *stats,
    const std::unordered_set<std::string> *proto_holders);

void CollectProtoMeshNesting(
    const lightusd::next::Stage &stage, const std::string &proto_path,
    lightusd::Purpose purpose, double time, const std::vector<std::string> &mask,
    std::vector<InstanceRT> *nested, std::vector<CurveInstanceRT> *nested_curves,
    std::unordered_map<std::string, uint32_t> *proto_ids,
    std::vector<ProtoBuildReq> *protos, CurveProtoCollect *curve_inst,
    RTPreviewStats *stats,
    const std::unordered_set<std::string> *proto_holders);

void CollectPrototypePaths(const lightusd::next::UsdPrim &prim,
                           std::unordered_set<std::string> *out);

void CollectProtoJobs(const lightusd::next::Stage &stage,
                      const std::string &proto_path,
                      lightusd::Purpose start_purpose, double time,
                      std::vector<MeshJobNext> *jobs);

inline size_t EstimateTrisForJob(const lightusd::next::UsdPrim &prim,
                                 double time);

void ExpandBoundsByTransformedO2W(Bounds *g, const Bounds &local,
                                  const float o2w[12]);

void CollectLightsNext(const lightusd::next::Stage &stage,
                       const lightusd::next::UsdPrim &prim,
                       const matrix4d &parent_world, double time,
                       LightCache *cache);

lightusd::next::UsdPrim FindDomeLightRec(const lightusd::next::UsdPrim &prim);

bool BuildNextIbl(const lightusd::next::Stage &stage, const Options &opt,
                  const std::string &base_dir, double time, IblCache *ibl);

bool ParseFrameSpec(const std::string &spec, std::vector<double> *times);

std::string SubstituteFrame(const std::string &path, long frame);

int RunRTPreviewNext(const Options &opt);

// Compose through the persistent next-core session with aggregate accounting,
// then release transient PCP caches before returning the retained Stage.
bool LoadNextStageBudgeted(const Options &opt, lightusd::next::Stage *stage,
                           std::string *warn, std::string *err,
                           lightusd::next::ValueClipStageLoader *clip_loader = nullptr);

// ---- lusdr_lod.cc (view-dependent district LOD pre-pass) ----
// Largest DEVICE_LOCAL Vulkan heap (VRAM) in bytes; 0 if no Vulkan/device.
size_t QueryDeviceLocalVRAMBytes();

// When opt.lod_stream is set, compose the scene in proxy LOD, rank districts by
// camera distance, and write a wrapper layer that promotes the nearest districts
// to districtLod=full within the host/VRAM budgets. On success rewrites
// opt->input to the generated wrapper path (so the normal render flow loads it)
// and returns true. Returns false (leaving opt->input unchanged) if no districts
// were found or composition failed -- the caller renders the scene as-is.
bool PrepareLodStream(Options *opt, std::string *generated_wrapper);

// ---- shared helpers (defined in lusdrender.cc) ----
void AppendLinearCurveStrands(const std::vector<lightusd::value::point3f> &points,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds);
void AppendLinearCurveStrands(const float *points, size_t point_count,
                              const std::vector<int> &counts,
                              const std::vector<float> &widths,
                              const matrix4d &world,
                              std::vector<float> *curve_points,
                              std::vector<float> *curve_radii,
                              std::vector<uint32_t> *first,
                              std::vector<uint32_t> *count,
                              std::vector<TriInfo> *info,
                              Bounds *bounds);
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
                               Bounds *bounds);
void MergeStats(RTPreviewStats *dst, const RTPreviewStats &src);
void MergeBounds(Bounds *dst, const Bounds &src);
inline const Vec3 kCurveColor{0.62f, 0.50f, 0.34f};

// ---- lusdr_legacy.cc (legacy loader + shared utils) ----

// Load `path` into `stage` WITH composition (sublayers / references / payloads /
// inherits / variants / specializes composed to a fixed point).
// `lightusd::LoadUSDFromFile` alone parses a single layer and expands no arcs, so
// anything contributed by a reference or payload — a Material in a look layer,
// payload-gated geometry — was simply absent from the legacy render. Falls back to
// the direct parser for .usdz and for layers with no arcs (which keeps zero-copy
// USDC storage and the schemas LayerToStage does not carry).
bool LoadStageComposedLegacy(const std::string &path,
                             const lightusd::USDLoadOptions &load_options,
                             lightusd::Stage *stage, std::string *warn,
                             std::string *err);

std::vector<int> FaceMaterialIds(const RenderMesh &mesh);

// Textures bound by one legacy RenderMaterial, as indices into the `Texture`
// table filled by BuildLegacyTextures. -1 = not textured.
struct LegacyMaterialTex {
  int32_t diffuse{-1};
  int32_t emissive{-1};
  int32_t normal{-1};
  int32_t roughness{-1};
  int32_t metallic{-1};
  int32_t occlusion{-1};
  int32_t opacity{-1};
  uint8_t roughness_ch{0};  // scalar source channel: 0=r,1=g,2=b,3=a
  uint8_t metallic_ch{0};
  uint8_t occlusion_ch{0};
  uint8_t opacity_ch{0};
};

// Decode tydra's already-resolved UsdUVTextures into renderer `Texture`s and
// return the per-material bindings, indexed by RenderScene material id. The
// legacy path previously ignored these entirely, so .usda/.usdz rendered with a
// flat constant base color.
std::vector<LegacyMaterialTex> BuildLegacyTextures(const RenderScene &scene,
                                                   std::vector<Texture> *out);

// Absolute prim path -> inherited purpose bit; 0 = visibility="invisible"
// (subtree pruned). Built by BuildLegacyPurposeVisibility.
using PurposeVisibilityMap = std::unordered_map<std::string, uint32_t>;
void BuildLegacyPurposeVisibility(const lightusd::Stage &stage,
                                  PurposeVisibilityMap *out);

// `tri_uvs` (when non-null) receives 6 floats per emitted triangle — the raw USD
// per-corner UVs, parallel to *tris, as the integrator expects. Textures are
// bound per triangle from `mat_tex` (null = untextured, the old behavior).
// `pv` (when non-null) stamps each mesh's inherited purpose bit and prunes
// invisible meshes.
void AddMeshTriangles(const RenderScene &scene, const RenderMesh &mesh,
                      const matrix4d &world, std::vector<float> *vertices,
                      std::vector<TriInfo> *tris, Bounds *bounds,
                      LightCache *lights = nullptr,
                      std::vector<float> *tri_uvs = nullptr,
                      const std::vector<LegacyMaterialTex> *mat_tex = nullptr,
                      const PurposeVisibilityMap *pv = nullptr);

void CollectGeometry(const RenderScene &scene, const Node &node,
                     std::vector<float> *vertices, std::vector<TriInfo> *tris,
                     Bounds *bounds,
                     const std::unordered_set<std::string> *skip_paths,
                     LightCache *lights,
                     std::vector<float> *tri_uvs = nullptr,
                     const std::vector<LegacyMaterialTex> *mat_tex = nullptr,
                     const PurposeVisibilityMap *pv = nullptr);

void CollectAllGeometry(const RenderScene &scene, std::vector<float> *vertices,
                        std::vector<TriInfo> *tris, Bounds *bounds,
                        const std::unordered_set<std::string> *skip_paths,
                        LightCache *lights,
                        std::vector<float> *tri_uvs = nullptr,
                        const std::vector<LegacyMaterialTex> *mat_tex = nullptr,
                        const PurposeVisibilityMap *pv = nullptr);

matrix4d LocalMatrixOrIdentity(const lightusd::Xformable *xformable, double time,
                               bool *reset);

lightusd::Purpose ResolvePurpose(const lightusd::Prim &prim,
                                 lightusd::Purpose inherited);

bool AddRTPreviewMesh(const lightusd::Stage &stage, const std::string &prim_path,
                      const lightusd::GeomMesh &mesh, const matrix4d &world,
                      double time, lightusd::Purpose purpose,
                      uint32_t purpose_mask,
                      std::vector<float> *vertices, std::vector<TriInfo> *tris,
                      Bounds *bounds, RTPreviewStats *stats);

void CollectRTPreviewMeshes(const lightusd::Prim &prim,
                            const matrix4d &parent_world, double time,
                            lightusd::Purpose inherited_purpose,
                            std::vector<MeshJob> *jobs);

bool BuildRTPreviewScene(const lightusd::Stage &stage, const Options &opt,
                         std::vector<float> *vertices,
                         std::vector<TriInfo> *tris, Bounds *bounds,
                         RTPreviewStats *stats, std::string *err);

bool MatchPrimNameOrPath(const lightusd::Prim &prim, const std::string &query);

bool CameraFrameFromGeomCamera(const lightusd::Stage &stage,
                               const lightusd::GeomCamera &cam,
                               const matrix4d &world, double time,
                               CameraFrame *frame);

bool FindStageCameraFrameRecursive(const lightusd::Stage &stage,
                                   const lightusd::Prim &prim,
                                   const std::string &query,
                                   const matrix4d &parent_world, double time,
                                   CameraFrame *frame);

bool FindStageCameraFrame(const lightusd::Stage &stage, const std::string &query,
                          double time, CameraFrame *frame);

bool EvalAxis(const lightusd::TypedAttributeWithFallback<lightusd::Axis> &attr,
              lightusd::Axis *out);

std::string PrimPathString(const lightusd::Prim &prim);

float ApproxScale(const matrix4d &m);

void AddNurbsTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                      std::vector<float> *vertices, std::vector<TriInfo> *tris,
                      Bounds *bounds);

void AddDirectTriangle(const Vec3 &p0, const Vec3 &p1, const Vec3 &p2,
                       const Vec3 &color, std::vector<float> *vertices,
                       std::vector<TriInfo> *tris, Bounds *bounds);

void AddDirectCube(double size, const matrix4d &world, std::vector<float> *vertices,
                   std::vector<TriInfo> *tris, Bounds *bounds);

void AddDirectPlane(double width, double length, lightusd::Axis axis,
                    const matrix4d &world, std::vector<float> *vertices,
                    std::vector<TriInfo> *tris, Bounds *bounds);

double BSplineBasis(int i, int degree, double u, const std::vector<double> &knots);

Vec3 EvalNurbsPatchPoint(const std::vector<lightusd::value::point3f> &points,
                         const std::vector<double> &weights, int u_count,
                         int v_count, int u_order, int v_order,
                         const std::vector<double> &u_knots,
                         const std::vector<double> &v_knots, double u, double v);

void AddNurbsPatchTriangles(const lightusd::Stage &stage,
                            const lightusd::GeomNurbsPatch &patch,
                            const matrix4d &world, double time,
                            std::vector<float> *vertices,
                            std::vector<TriInfo> *tris, Bounds *bounds);

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
                         std::vector<float> *tet_aabbs);

bool BuildDirectScene(const lightusd::Stage &stage, const RenderScene &render_scene,
                      const Options &opt, std::vector<float> *vertices,
                      std::vector<TriInfo> *tris, Bounds *bounds,
                      DirectScene *direct, std::string *err);

bool FindCameraNode(const RenderScene &scene, const Node &node,
                    const std::string &query, const Node **node_out);
const Node *FindCameraNode(const RenderScene &scene, const std::string &query);

}  // namespace lusdr
