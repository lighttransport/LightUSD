// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache stage opinion-fill pass.
// Included inside Cache::Impl.

  // Per-prim opinion records collected by the structure pass, drained by
  // FillOpinions. Member (not local) so a single fill can cover every root.
  // SrcCache values live in a deque and remain address-stable across inserts.
  // Retain the resolved source pointer directly instead of copying every path
  // and hashing it again during the opinion pass.
  std::vector<std::pair<uint32_t, const std::vector<Src>*>> fill_;

  static bool IsPreviewProperty(const std::string &name) ;

  // Narrow strong-to-weak opinion pass for spatial startup hints. It never
  // touches topology, point-instancer arrays, materials, textures or Ptex.
  void ComposePreviewOpinions(const std::vector<Src> &srcs, PrimSpec *out) ;

  enum PreviewEndpoint : uint8_t {
    PreviewEndpointNone = 0,
    PreviewEndpointBound = 1,
    PreviewEndpointCamera = 2,
  };

  PreviewEndpoint ClassifyPreviewEndpoint(const std::vector<Src> &srcs) ;

  // Build a compact hierarchy containing only bound/camera endpoints and their
  // ancestors. A full Layer::Clone duplicates every PrimSpec in a large scene
  // and defeated the checkpoint's latency goal even though its values were
  // cheap shared backing.
  Layer BuildPreviewLayer(const Layer &structure) ;

#if defined(LIGHTUSD_ENABLE_THREAD)
  // release_layers_during_fill: source layers to free after each fill batch.
  struct LayerReleasePlan {
    std::vector<std::vector<Layer *>> by_batch;
    size_t layers = 0;
    double ms = 0;
  };
  LayerReleasePlan PlanLayerRelease(size_t opinion_batch, int nt);
#endif
  // OPINION-FILL PASS: compose each slot's opinions from its (already cached)
  // sources. Slots are disjoint PrimSpec objects, sources_cache is only read, and
  // add_prim is done -- so distinct slots fill concurrently. Byte-identical to
  // the serial ComposeInto: the same ComposeOpinions runs on the same (slot,
  // srcs) pairs; only the order across independent slots changes.
  void FillOpinions(Layer *out,
                    const std::vector<const std::vector<Src> *>& shared_srcs) ;
