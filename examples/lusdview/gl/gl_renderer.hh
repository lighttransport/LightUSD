// SPDX-License-Identifier: Apache-2.0
// lusdview - OpenGL 3.3 backend. Renders the scene into an offscreen FBO whose
// color texture is shown by the GUI via ImGui::Image.
#pragma once

#include <glad/glad.h>

#include <vector>

#include "gpu_scene.hh"
#include "raster_lighting.hh"
#include "renderer.hh"

namespace lusdview {

class GLRenderer final : public Renderer {
 public:
  GLRenderer() = default;
  ~GLRenderer() override;

  bool init(GLFWwindow* window, std::string* err) override;
  bool initImGui(std::string* err) override;
  void beginScene(const std::vector<DrawMaterialCPU>& materials, int textureCount) override;
  void syncSceneResources(const std::vector<DrawMaterialCPU>& materials,
                          int textureCount) override;
  bool updateMaterialConstants(int materialId,
                               const DrawMaterialCPU& material,
                               std::string* err) override;
  void appendMesh(const DrawMeshCPU& mesh) override;
  void appendPoints(const DrawPointsCPU& points) override;
  void appendCurves(const DrawCurvesCPU& curves) override;
  void appendMeshSurface(const DrawMeshCPU& mesh) override;
  void uploadMeshAux(size_t meshIndex, const DrawMeshCPU& mesh) override;
  void uploadTexture(int slot, const DrawTextureCPU& tex) override;
  void evictTexture(int slot) override;
  size_t textureResidentBytes(int slot) const override;
  bool updateTextureRegion(int slot, int x, int y, int w, int h,
                           const uint8_t* rgba,
                           size_t rowBytes = 0) override;
  bool updatePtexFaceRect(int slot, uint32_t face,
                          const DrawPtexFaceRectCPU& rect) override;
  bool updateTextureRegions(
      int slot, const std::vector<TextureRegionUpdate>& updates) override;
  void setLights(const std::vector<DrawLightCPU>& lights,
                 size_t meshCount) override;
  void uploadSkinningFrame(const SkinningFrameCPU& skin) override;
  void updateMeshVertices(int meshIndex,
                          const std::vector<DrawVertex>& verts) override;
  void updateMorphWeights(int meshIndex,
                          const std::vector<float>& coeffs) override;
  void updateInstanceVisibility(size_t meshIndex, const float* xforms,
                                const float* colors, const float* opacities,
                                uint32_t count) override;
  bool supportsProxyDraw() const override { return true; }
  void updateProxyInstances(const float* xforms, const float* tints,
                            uint32_t count) override;
  void updateMeshWorld(int meshIndex, const float world[16]) override;
  bool applyMeshSlotUpdatesTransactional(
      const std::vector<MeshSlotUpdate>& updates, std::string* error) override;
  bool replaceMeshSlotsTransactional(
      const std::vector<MeshSlotReplacement>& replacements,
      std::string* error) override;
  bool applyMaterialUpdatesTransactional(
      const std::vector<MaterialSlotUpdate>& updates,
      std::string* error) override;
  bool replaceTextureSlotsTransactional(
      const std::vector<TextureSlotReplacement>& replacements,
      std::string* error) override;
  int meshCount() const override { return static_cast<int>(meshes_.size()); }
  void resizeViewport(int width, int height) override;
  void newFrame() override;
  void renderFrame(const RenderFrameParams& params) override;
  ViewportTexHandle viewportTexture() const override;
  bool uploadViewportImage(const uint8_t* rgba, int w, int h) override;
  void present() override;
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  void presentThreaded(ImDrawData* drawData, int fbW, int fbH) override;
  bool initImGuiPlatform(GLFWwindow* window, std::string* err) override;
  bool initImGuiBackend(std::string* err) override;
#endif
  bool captureViewport(std::vector<uint8_t>* rgba, int* w, int* h) override;
  bool gpuMemoryMB(size_t* usedMB, size_t* totalMB) const override;
  void requestWindowCapture() override { wantWindowCapture_ = true; }
  bool captureWindow(std::vector<uint8_t>* rgba, int* w, int* h) override;
  const RendererCaps& caps() const override { return caps_; }
  void appendVolume(const DrawVolumeCPU& vol) override;
  void shutdown() override;

 private:
  void appendMaterials(const std::vector<DrawMaterialCPU>& materials,
                       size_t first);
  void resizeTextureSlots(int textureCount);
  void appendMeshImpl(const DrawMeshCPU& mesh, bool includeAux);
  // UsdVol volume (OpenVDB): density grid as a GL_TEXTURE_3D, raymarched in a
  // proxy-box pass over its object-space AABB.
  struct GLVolume {
    GLuint tex3d{0};
    GLuint emissionTex3d{0};
    GLuint temperatureTex3d{0};
    float world[16];     // object -> world (column-major)
    float invWorld[16];  // world -> object (column-major)
    float bmin[3]{0, 0, 0};
    float bmax[3]{0, 0, 0};
    float densityScale{1.0f};
    float albedo[3]{0.6f, 0.6f, 0.65f};
    float emission[3]{0, 0, 0};
    float background{0.0f};
  };
  struct GLMesh {
    GLuint vao{0}, vbo{0}, ebo{0}, jointVbo{0}, weightVbo{0};
    GLuint influenceVbo{0}, influenceTex{0};
    GLuint instanceVbo{0};   // per-instance 3x4 model matrices; 0 = none
    int instanceCount{0};    // >0 => drawn with glDrawElementsInstanced
    int drawInstanceCount{0};  // visible subset drawn this frame (per-instance cull)
    GLuint instanceColorVbo{0};      // per-instance displayColor; 0 = none
    GLuint instanceOpacityVbo{0};    // per-instance displayOpacity; 0 = none
    bool hasInstanceColors{false};   // true => attrib 9 is array-backed
    bool hasInstanceOpacities{false};  // true => attrib 11 is array-backed
    bool hasTranslucentInstances{false};
    float flatColor[3]{0.8f, 0.8f, 0.8f};  // per-draw color when no per-instance
    float flatOpacity{1.0f};
    GLuint vertexColorVbo{0};        // RGBA: displayColor + displayOpacity; 0 = none
    bool hasVertexOpacity{false};     // varying opacity needs authored two-sided folds
    GLuint uv1Vbo{0};                // 2nd texcoord set (attrib 6, non-instanced); 0 = none
    GLuint morphInflVbo{0};          // blendshape influence (attrib 7, non-instanced); 0 = none
    GLuint faceIdBuf{0};             // texture buffer: per-triangle source face id; 0 = none
    GLuint faceIdTex{0};             // GL_TEXTURE_BUFFER view (R32UI) of faceIdBuf
    GLuint wireEbo{0};               // GL_LINES indices: original polygon edges; 0 = none
    GLsizei wireCount{0};            // index count in wireEbo (2 per edge)
    // GPU blendshape morph (raster): per-vertex (offset,count) attribute (loc 8) +
    // a static delta texture-buffer (RGBA32F: channelId,dx,dy,dz) + a tiny per-frame
    // coefficient texture-buffer (R32F). hasMorph gates it on; 0 handles = none.
    GLuint morphOffsetVbo{0};        // attrib 8: uvec2 (offset, count); 0 = none
    GLuint morphDeltaBuf{0}, morphDeltaTex{0};   // RGBA16F GL_TEXTURE_BUFFER
    GLuint morphCoeffBuf{0}, morphCoeffTex{0};   // R32F GL_TEXTURE_BUFFER (dynamic)
    GLuint morphChanBuf{0}, morphChanTex{0};     // R16UI: per-entry channelId (skip)
    int morphChannelCount{0};
    bool hasMorph{false};
    size_t indexCount{0};  // base surface indices; CPU copy may be released
    bool geometricNormal{false};     // shade with screen-derivative normal
    bool rasterDisplacementBaked{false};
    int purposeId{0};                // USD purpose AOV: 0=default/1=render/2=proxy/3=guide
    int kindId{0};                   // USD kind AOV: 0=none/1=component/2=group/3=assembly/4=subcomponent
    std::vector<DrawSubmesh> submeshes;
    float world[16];
    bool doubleSided{false};
    bool skinned{false};
    bool extendedSkinned{false};
    int influenceTexWidth{0};
    size_t vertexCount{0};  // for per-frame morph vertex re-upload guard
    float localCentroid[3]{0, 0, 0};  // mesh-space bbox center (translucency sort)
  };

  // Which alpha class to draw in a drawMeshes() pass. Translucent materials
  // (alphaMode == Blend) render in a separate depth-write-off blended pass,
  // sorted back-to-front by world centroid, after the opaque pass.
  enum class AlphaPass { All, Opaque, Translucent };
  struct GLMaterial {
    float baseColor[3]{0.8f, 0.8f, 0.8f};
    float metallic{0.0f}, roughness{0.5f};
    float emissive[3]{0, 0, 0};
    float alpha{1.0f};
    int alphaMode{0};
    float alphaCutoff{0.5f};
    // Specular workflow + IOR for F0 (T12).
    bool useSpecularWorkflow{false};
    bool openPbrSpecularModel{false};
    float specularColor[3]{0.0f, 0.0f, 0.0f};
    float ior{1.5f};
    float occlusion{1.0f};
    float coatWeight{0.0f};
    float coatColor[3]{1.0f, 1.0f, 1.0f};
    float coatRoughness{0.1f};
    float coatIor{1.5f};
    // Texture slot indices into textures_ (-1 = none). Resolved at draw time so
    // lazily-uploaded textures appear without re-touching materials.
    int baseColorTex{-1}, metallicTex{-1}, roughnessTex{-1};
    int normalTex{-1}, emissiveTex{-1};
    int opacityTex{-1}, occlusionTex{-1};
    // Coat lobe + specular-workflow F0 maps (previously constant-only).
    int coatWeightTex{-1}, coatColorTex{-1}, coatRoughnessTex{-1};
    int coatNormalTex{-1};
    int specularColorTex{-1};
    DrawTexSampleCPU coatWeightSample;
    DrawTexSampleCPU coatColorSample;
    DrawTexSampleCPU coatRoughnessSample;
    DrawTexSampleCPU coatNormalSample;
    DrawTexSampleCPU specularColorSample;
    DrawTexSampleCPU baseColorSample;
    DrawTexSampleCPU metallicSample;
    DrawTexSampleCPU roughnessSample;
    DrawTexSampleCPU normalSample;
    DrawTexSampleCPU emissiveSample;
    DrawTexSampleCPU opacitySample;
    DrawTexSampleCPU occlusionSample;
    DrawLightRtOpenPBRCPU lightRtOpenPBR;
    int opacityChannel{0};
    float opacityTexScale{1.0f};
    float opacityTexBias{0.0f};
    int occlusionChannel{0};
    float occlusionTexScale{1.0f};
    float occlusionTexBias{0.0f};
    int metallicChannel{2};
    int roughnessChannel{1};
    float metallicTexScale{1.0f};
    float metallicTexBias{0.0f};
    float roughnessTexScale{1.0f};
    float roughnessTexBias{0.0f};
    int displacementTex{-1};
    DrawUvXformCPU displacementUv;
    float displacementConst{0.0f};
    float displacementTexScale{1.0f};
    float displacementTexBias{0.0f};
    MaterialXGraphRuntimeCPU materialXGraph;
    bool hasDisplacement() const { return displacementTex >= 0 || displacementConst != 0.0f; }
  };

  GLMaterial makeMaterial(const DrawMaterialCPU& material) const;

  void destroyScene();
  void destroyMesh(GLMesh& mesh);
  void buildTessProgram();  // GL>=4.0 tessellation displacement program (best-effort)
  void ensureFbo(int w, int h);
  void drawMeshes(const RenderFrameParams& params, bool wireframe,
                  const float* overrideEmissive,
                  AlphaPass alphaPass = AlphaPass::All);
  void renderShadowMap(const RenderFrameParams& params);
  // Draw each mesh's original-polygon edge set (wireEbo) as GL_LINES, both the
  // non-instanced and instanced prototypes, in a flat color with a small NDC depth
  // bias so the lines sit just in front of the surface.
  void buildWireProgram();
  void drawWireframe(const RenderFrameParams& params, const float wireColor[3]);
#if defined(LUSDVIEW_ENABLE_GL_THREAD)
  void presentImpl(ImDrawData* drawData, int fbw, int fbh);
#endif

  GLFWwindow* window_{nullptr};
  RendererCaps caps_{};
  bool imguiInited_{false};

  GLuint program_{0};
  // uniform locations
  GLint uMVP_{-1}, uModel_{-1}, uNormalMat_{-1}, uCameraPos_{-1};
  GLint uLightDir_{-1}, uLightColor_{-1}, uGeometricNormal_{-1};
  GLint uRenderMode_{-1}, uMatId_{-1}, uDepthScale_{-1};  // AOV visualizations
  GLint uSceneMin_{-1}, uSceneExtent_{-1};                // position AOV bounds
  GLint uMeshId_{-1}, uDoubleSided_{-1};                  // mesh-id / double-sided AOV
  GLint uPurpose_{-1};                                    // purpose AOV (per-draw)
  GLint uKind_{-1};                                       // kind AOV (per-draw)
  GLint uFaceIdTex_{-1}, uFaceBase_{-1}, uHasFaceId_{-1}; // source-face-id AOV
  GLint uBasePtex_{-1}, uBasePtexGrid_{-1};              // Ptex atlas sampling
  GLint uMetallicPtexGrid_{-1}, uRoughnessPtexGrid_{-1};
  GLint uNormalPtexGrid_{-1}, uEmissivePtexGrid_{-1};
  GLint uOpacityPtexGrid_{-1}, uOcclusionPtexGrid_{-1};
  GLint uSpecularColorPtexGrid_{-1}, uCoatWeightPtexGrid_{-1};
  GLint uCoatColorPtexGrid_{-1}, uCoatRoughnessPtexGrid_{-1};
  GLint uBaseColor_{-1}, uMetallic_{-1}, uRoughness_{-1}, uEmissive_{-1}, uAlpha_{-1};
  GLint uAlphaMode_{-1}, uAlphaCutoff_{-1};
  GLint uUseSpecularWorkflow_{-1}, uOpenPbrSpecularModel_{-1};
  GLint uSpecularColor_{-1}, uIor_{-1};  // F0 (T12)
  GLint uOcclusion_{-1}, uCoatWeight_{-1}, uCoatColor_{-1};
  GLint uCoatRoughness_{-1}, uCoatIor_{-1};
  GLint uTransmission_{-1}, uTransmissionColor_{-1};
  GLint uTransmissionDepth_{-1}, uTransmissionScatter_{-1};
  GLint uTransmissionScatterAnisotropy_{-1};
  GLint uVolumeDensity_{-1}, uVolumeAlbedo_{-1};
  GLint uVolumeEmission_{-1}, uVolumeEmissionScale_{-1};
  GLint uDiffuseRoughness_{-1};
  GLint uTransmissionDispersionAbbeNumber_{-1};
  GLint uSubsurfaceAnisotropy_{-1}, uSubsurfaceScatterAnisotropy_{-1};
  GLint uSpecularAnisotropy_{-1}, uSpecularRotation_{-1};
  GLint uSpecularRoughnessAnisotropy_{-1};
  GLint uCoatAnisotropy_{-1}, uCoatRotation_{-1};
  GLint uCoatRoughnessAnisotropy_{-1};
  GLint uTransmissionDispersion_{-1}, uTransmissionDispersionScale_{-1};
  GLint uCoatAffectColor_{-1}, uCoatAffectRoughness_{-1}, uCoatDarkening_{-1};
  GLint uBaseWeight_{-1}, uSpecularWeight_{-1};
  GLint uSubsurface_{-1}, uSubsurfaceColor_{-1};
  GLint uSubsurfaceScale_{-1}, uSubsurfaceRadius_{-1};
  GLint uSheenWeight_{-1}, uSheenColor_{-1}, uSheenRoughness_{-1};
  GLint uThinFilmWeight_{-1}, uThinFilmThickness_{-1}, uThinFilmIor_{-1};
  GLint uExposure_{-1};
  GLint uHasBaseColorTex_{-1}, uHasMetallicTex_{-1}, uHasRoughnessTex_{-1};
  GLint uHasNormalTex_{-1}, uHasEmissiveTex_{-1};
  GLint uHasOpacityTex_{-1}, uHasOcclusionTex_{-1};
  GLint uBaseColorTexIsUdim_{-1}, uMetallicTexIsUdim_{-1}, uRoughnessTexIsUdim_{-1};
  GLint uNormalTexIsUdim_{-1}, uEmissiveTexIsUdim_{-1}, uOpacityTexIsUdim_{-1};
  GLint uOcclusionTexIsUdim_{-1};
  GLint uBaseColorUv0_{-1}, uBaseColorUv1_{-1};
  GLint uMetallicUv0_{-1}, uMetallicUv1_{-1};
  GLint uRoughnessUv0_{-1}, uRoughnessUv1_{-1};
  GLint uNormalUv0_{-1}, uNormalUv1_{-1};
  GLint uEmissiveUv0_{-1}, uEmissiveUv1_{-1};
  GLint uOpacityUv0_{-1}, uOpacityUv1_{-1};
  GLint uOcclusionUv0_{-1}, uOcclusionUv1_{-1};
  GLint uUvSet_{-1};  // per-slot UV set (base, metallic, normal, emissive)
  GLint uRoughnessUvSet_{-1};
  GLint uBaseColorTexScale_{-1}, uBaseColorTexBias_{-1};
  GLint uNormalTexScale_{-1}, uNormalTexBias_{-1};
  GLint uEmissiveTexScale_{-1}, uEmissiveTexBias_{-1};
  GLint uMetallicChannel_{-1}, uRoughnessChannel_{-1};
  GLint uMetallicTexScale_{-1}, uMetallicTexBias_{-1};
  GLint uRoughnessTexScale_{-1}, uRoughnessTexBias_{-1};
  GLint uOpacityUvSet_{-1}, uOpacityChannel_{-1};
  GLint uOpacityTexScale_{-1}, uOpacityTexBias_{-1};
  GLint uOcclusionUvSet_{-1}, uOcclusionChannel_{-1};
  GLint uOcclusionTexScale_{-1}, uOcclusionTexBias_{-1};
  // Coat lobe + specular-workflow F0 map uniforms.
  GLint uHasSpecularColorTex_{-1};
  GLint uSpecularColorUv0_{-1}, uSpecularColorUv1_{-1};
  GLint uSpecularColorUvSet_{-1};
  GLint uSpecularColorScale_{-1}, uSpecularColorBias_{-1};
  GLint uHasCoatNormalTex_{-1};
  GLint uCoatNormalUv0_{-1}, uCoatNormalUv1_{-1}, uCoatNormalUvSet_{-1};
  GLint uCoatNormalScale_{-1}, uCoatNormalBias_{-1};
  GLint uHasCoatWeightTex_{-1}, uHasCoatColorTex_{-1};
  GLint uHasCoatRoughnessTex_{-1};
  GLint uCoatWeightUv0_{-1}, uCoatWeightUv1_{-1};
  GLint uCoatColorUv0_{-1}, uCoatColorUv1_{-1};
  GLint uCoatRoughnessUv0_{-1}, uCoatRoughnessUv1_{-1};
  GLint uCoatWeightUvSet_{-1}, uCoatColorUvSet_{-1};
  GLint uCoatRoughnessUvSet_{-1};
  GLint uCoatWeightChannel_{-1}, uCoatRoughnessChannel_{-1};
  GLint uCoatWeightScale_{-1}, uCoatWeightBias_{-1};
  GLint uCoatColorScale_{-1}, uCoatColorBias_{-1};
  GLint uCoatRoughnessScale_{-1}, uCoatRoughnessBias_{-1};
  GLint uAdvancedTexIsUdim_{-1}, uAdvancedUdimRoutes_{-1};
  GLint uAdvancedUdimSlots_{-1};
  GLint uCoatNormalTexIsUdim_{-1}, uCoatNormalUdimRoute_{-1};
  GLint uCoatNormalUdimSlot_{-1};
  GLint uGraphNodeCount_{-1}, uGraphOutputs_{-1};
  GLint uGraphUsable_{-1};
  GLint uGraphNode0_{-1}, uGraphNode1_{-1}, uGraphNode2_{-1};
  GLint uGraphNode3_{-1}, uGraphNode4_{-1}, uGraphNode5_{-1};
  GLint uGraphTex_[8]{-1, -1, -1, -1, -1, -1, -1, -1};
  GLint uGraphUdimRoutes_{-1};
  GLint uUdimSlots_{-1}, uOpacityUdimSlot_{-1}, uRoughnessUdimSlot_{-1};
  GLint uOcclusionUdimSlot_{-1};
  GLint uHasDisplacement_{-1}, uHasDisplacementTex_{-1};  // displacement (coarse)
  GLint uDisplacementConst_{-1}, uDisplacementScale_{-1};
  GLint uDisplacementTexScale_{-1}, uDisplacementTexBias_{-1};
  GLint uHasMorph_{-1};  // GPU blendshape morph enable (per-draw)
  GLint iHasMorph_{-1};  // GPU morph enable in the instanced program (per-draw)
  // Skeletal skinning in the instanced program (prototype-local, bone texture on
  // unit 4 -- the same scene-wide texture the mesh program samples).
  GLint iSkinningEnabled_{-1}, iBoneTexWidth_{-1}, iBoneMatrixCount_{-1};

  // GPU tessellation displacement program (built only on GL >= 4.0). Adaptive
  // sub-triangle subdivision in the TCS + per-sample displacement in the TES, so a
  // coarse mesh shows fine height-map detail without any persistent extra geometry.
  // Used for displaced submeshes in Shaded mode when Max-tess > 1; otherwise the
  // coarse per-vertex program above is used.
  bool tessAvailable_{false};
  GLuint tessProgram_{0};
  GLint tMVP_{-1}, tModel_{-1}, tNormalMat_{-1}, tCameraPos_{-1};
  GLint tLightDir_{-1}, tLightColor_{-1};
  GLint tHasIbl_{-1}, tIblColor_{-1}, tEnvRotation_{-1};  // dome IBL (diffuse)
  GLint tExposure_{-1};
  GLint tBaseColor_{-1}, tHasBaseColorTex_{-1};
  GLint tHasDisplacementTex_{-1}, tDisplacementConst_{-1}, tDisplacementScale_{-1};
  GLint tDisplacementTexScale_{-1}, tDisplacementTexBias_{-1};
  GLint tMaxTessLevel_{-1};
  GLint tHasMorph_{-1};  // GPU morph in the tess vertex stage
  // Skinning in the tess vertex stage (mirrors the coarse program's uniforms).
  GLint tSkinningEnabled_{-1}, tExtendedSkinningEnabled_{-1};
  GLint tBoneTexWidth_{-1}, tBoneMatrixCount_{-1}, tInfluenceTexWidth_{-1};
  GLint uSkinningEnabled_{-1};
  GLint uExtendedSkinningEnabled_{-1};
  GLint uBoneTexWidth_{-1}, uBoneMatrixCount_{-1}, uInfluenceTexWidth_{-1};

  // Instanced flat-shaded program (GPU instancing for PointInstancer prototypes):
  // per-instance 3x4 model (attribs 6-8) + displayColor (attrib 9); view-proj +
  // camera uniforms. The fragment shader is a self-contained flat shader using
  // the per-instance vColor (no material uniforms).
  GLuint instProgram_{0};
  GLint iUViewProj_{-1}, iCameraPos_{-1};
  GLint iLightDir_{-1}, iLightColor_{-1}, iEmissive_{-1};
  GLint iHasIbl_{-1}, iIblColor_{-1}, iEnvRotation_{-1};  // dome IBL (diffuse)
  GLint iExposure_{-1};
  GLint iHasShadowMap_{-1}, iShadowLightSlot_{-1}, iShadowViewProj_{-1};
  GLint iHasPointShadowMap_{-1}, iPointShadowLightPos_{-1}, iPointShadowViewProj_{-1};
  // Instanced-program debug-AOV uniforms (mirror the non-instanced material shader).
  GLint iRenderMode_{-1}, iDepthScale_{-1}, iSceneMin_{-1}, iSceneExtent_{-1};
  GLint iMeshId_{-1}, iGeometricNormal_{-1}, iDoubleSided_{-1}, iPurpose_{-1}, iKind_{-1};

  // Wireframe programs: a geometry shader expands each polygon edge (GL_LINES from
  // wireEbo) into a thin screen-space quad with analytic edge-distance AA, giving
  // crisp sub-pixel-thin anti-aliased lines (usdview look). Non-instanced takes a
  // full MVP; instanced reuses the per-instance 3x4 rows (attribs 3/4/5) + view-proj.
  GLuint wireProgram_{0};
  GLint wMVP_{-1}, wWireColor_{-1}, wDepthBias_{-1}, wViewport_{-1}, wHalfWidth_{-1};
  GLint wHasMorph_{-1}, wSkinningEnabled_{-1}, wExtendedSkinningEnabled_{-1};
  GLint wBoneTexWidth_{-1}, wBoneMatrixCount_{-1}, wInfluenceTexWidth_{-1};
  GLuint wireInstProgram_{0};
  GLint wiViewProj_{-1}, wiWireColor_{-1}, wiDepthBias_{-1}, wiViewport_{-1}, wiHalfWidth_{-1};

  // DomeLight split-sum IBL (uploaded from DomeIblCPU by setLights; sampled by
  // the material shader's ambient term on units 19/20/21).
  GLuint iblIrrTex_{0}, iblSpecTex_{0}, iblLutTex_{0};
  RasterLightSet rasterLights_;
  int iblSpecLods_{0};
  bool iblActive_{false};
  float iblColor_[3]{1.0f, 1.0f, 1.0f};
  float iblRotation_[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};  // world->env, column-major
  GLint uHasIbl_{-1}, uIblColor_{-1}, uEnvRotation_{-1}, uPrefilteredLods_{-1};
  GLint uHasShadowMap_{-1}, uShadowLightSlot_{-1}, uShadowViewProj_{-1};
  GLint uHasPointShadowMap_{-1}, uPointShadowLightPos_{-1}, uPointShadowViewProj_{-1};
  GLuint shadowProgram_{0}, shadowInstProgram_{0}, shadowFbo_{0}, shadowDepthTex_{0},
      pointShadowDepthTex_{0};
  GLint sMVP_{-1}, sModel_{-1}, sNormalMat_{-1};
  GLint sSkinningEnabled_{-1}, sExtendedSkinningEnabled_{-1};
  GLint sBoneTexWidth_{-1}, sBoneMatrixCount_{-1}, sInfluenceTexWidth_{-1};
  GLint sHasMorph_{-1}, sHasDisplacement_{-1}, sHasDisplacementTex_{-1};
  GLint sDisplacementConst_{-1}, sDisplacementScale_{-1};
  GLint sDisplacementTexScale_{-1}, sDisplacementTexBias_{-1};
  GLint sAlphaMode_{-1}, sAlpha_{-1}, sAlphaCutoff_{-1};
  GLint sHasBaseAlphaTex_{-1}, sHasOpacityTex_{-1};
  GLint sBaseAlphaIsUdim_{-1}, sOpacityIsUdim_{-1};
  GLint sBaseAlphaUdimSlot_{-1}, sOpacityUdimSlot_{-1};
  GLint sBaseAlphaUv0_{-1}, sBaseAlphaUv1_{-1}, sBaseAlphaUvSet_{-1};
  GLint sBaseAlphaScale_{-1}, sBaseAlphaBias_{-1};
  GLint sOpacityUv0_{-1}, sOpacityUv1_{-1}, sOpacityUvSet_{-1};
  GLint sOpacityChannel_{-1}, sOpacityScale_{-1}, sOpacityBias_{-1};
  GLint siViewProj_{-1}, siSkinningEnabled_{-1}, siBoneTexWidth_{-1};
  GLint siBoneMatrixCount_{-1}, siHasMorph_{-1};
  RasterShadowCamera shadowCamera_;
  RasterPointShadowCameras pointShadowCameras_;
  void destroyIblTextures();

  GLuint whiteTex_{0};
  GLuint boneTex_{0};
  int boneTexWidth_{0}, boneTexHeight_{0}, boneMatrixCount_{0};
  int maxTextureSize_{4096};
  float maxTextureAnisotropy_{1.0f};
  std::vector<float> boneUploadScratch_;
  bool skinningFrameEnabled_{false};

  // Unlit line program for debug helpers (grid/axes/bbox).
  GLuint lineProgram_{0};
  GLint uLineVP_{-1};
  GLuint lineVao_{0}, lineVbo_{0};
  size_t lineVboCap_{0};
  GLuint highlightEbo_{0};  // dynamic index buffer for GeomSubset highlight

  // Raster LOD box proxies (optimization B): one shared unit-cube mesh drawn with
  // the instanced program, fed a per-frame buffer of box-fit o2w + tints for the
  // distant instances the cull collapsed. boxProxyInstCap_ tracks the VBO capacity
  // so updateProxyInstances only reallocates when the proxy count grows.
  GLuint boxProxyVao_{0}, boxProxyVbo_{0}, boxProxyEbo_{0};
  GLuint boxProxyInstVbo_{0}, boxProxyColorVbo_{0};
  uint32_t boxProxyCount_{0};
  size_t boxProxyInstCap_{0};
  void initBoxProxy();

  // UsdVol volume raymarch pass.
  GLuint volumeProgram_{0};
  GLuint volumeCubeVao_{0}, volumeCubeVbo_{0}, volumeCubeEbo_{0};
  GLint uVolVP_{-1}, uVolModel_{-1}, uVolInvModel_{-1}, uVolCameraPos_{-1};
  GLint uVolBmin_{-1}, uVolBmax_{-1}, uVolDensity_{-1}, uVolDensityScale_{-1};
  GLint uVolEmissionField_{-1}, uVolTemperatureField_{-1};
  GLint uVolHasEmissionField_{-1}, uVolHasTemperatureField_{-1};
  GLint uVolAlbedo_{-1}, uVolEmission_{-1}, uVolBackground_{-1};
  std::vector<GLVolume> volumes_;

  // Offscreen target
  GLuint fbo_{0}, colorTex_{0}, depthTex_{0};
  GLuint dofFbo_{0}, dofTex_{0}, dofProgram_{0}, dofVao_{0};
  bool dofApplied_{false};
  void applyRasterDof(const RenderFrameParams& params);
  int vpW_{0}, vpH_{0};
  // Row-flip scratch for uploadViewportImage (CUDA/HIP/CPU RT overlay).
  std::vector<uint8_t> flippedUploadScratch_;

  struct GLTexture {
    GLuint tex2d{0};
    GLuint arrayTex{0};
    bool isUdim{false};
    bool srgb{false};
    bool regionUpdatable{false};
    int width{0};
    int height{0};
    size_t residentBytes{0};
    size_t ptexRectOffset{0};  // offset in the shared R32UI metadata buffer
    std::vector<uint32_t> ptexRects;
  };
  void rebuildPtexRectBuffer();
  GLuint ptexRectBuf_{0};
  GLuint ptexRectTex_{0};
  GLuint udimLutAtlas_{0};
  std::vector<GLTexture> textures_;
  std::vector<GLMaterial> materials_;
  std::vector<GLMesh> meshes_;

  struct GLNonMeshBatch {
    GLuint vao{0}, vbo{0};
    GLsizei count{0};
    int kind{0};       // 0 point billboard, 1 curve ribbon
    int materialId{-1};
    int carrierId{-1};
    int carrierIndex{-1};
    std::string absPath;
    int purposeId{0};
    bool translucent{false};
  };
  GLuint nonMeshProgram_{0};
  GLint nmViewProj_{-1}, nmCameraPos_{-1}, nmCameraRight_{-1}, nmCameraUp_{-1};
  GLint nmLightDir_{-1}, nmLightColor_{-1}, nmExposure_{-1};
  GLint nmKind_{-1}, nmMaterialId_{-1}, nmCarrierId_{-1}, nmPurpose_{-1};
  GLint nmRenderMode_{-1};
  std::vector<GLNonMeshBatch> nonMeshBatches_;
  void buildNonMeshProgram();
  void drawNonMesh(const RenderFrameParams& params);

  // Window (back buffer) capture grabbed in present() before swap.
  bool wantWindowCapture_{false};
  std::vector<uint8_t> windowCapture_;  // bottom-up RGBA8
  int winCapW_{0}, winCapH_{0};
};

}  // namespace lusdview
