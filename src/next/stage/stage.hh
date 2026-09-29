// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Stage
// Composed scene representation built on top of Layer

#pragma once

#include "../prim/path.hh"
#include "../types/value.hh"
#include <string>
#include <vector>
#include <memory>
#include <type_traits>
#include <utility>

namespace lightusd {
namespace next {

class Layer;
class LayerBuilder;
class PrimSpec;
struct PrimSpecMeta;
struct PropMeta;
struct PropNameId;
enum class PrimSpecifier : uint8_t;

/// Stage metadata (derived from root layer)
struct StageMeta {
  std::string defaultPrim;
  bool defaultPrim_set = false;
  std::string upAxis = "Y";
  double metersPerUnit = 0.01;
  double timeCodesPerSecond = 24.0;
  double startTimeCode = 0.0;
  double endTimeCode = 0.0;
  bool upAxis_set = false;
  bool metersPerUnit_set = false;
  bool timeCodesPerSecond_set = false;
  bool startTimeCode_set = false;
  bool endTimeCode_set = false;
  double framesPerSecond = 24.0;
  bool framesPerSecond_set = false;
  double kilogramsPerUnit = 1.0;
  bool kilogramsPerUnit_set = false;
  std::string colorConfiguration;
  std::string colorManagementSystem;
  bool colorConfiguration_set = false;
  bool colorManagementSystem_set = false;
  std::string renderSettingsPrimPath;
  bool renderSettingsPrimPath_set = false;
  std::string doc;
  std::string comment;
  std::string owner;
  bool doc_set = false;
  bool comment_set = false;
  bool owner_set = false;
  bool autoPlay = true;
  bool autoPlay_set = false;
  std::string playbackMode;
  bool playbackMode_set = false;
  std::string copyright;
};

/// Prim handle for stage traversal
/// Lightweight reference to a prim in a specific layer
class UsdPrim {
public:
  UsdPrim() = default;
  UsdPrim(const PrimSpec* spec, const Layer* layer, uint32_t index)
      : spec_(spec), layer_(layer), index_(index) {}
  UsdPrim(const PrimSpec* spec, const Layer* layer, uint32_t index,
          Path proxy_path, std::string prototype_root,
          std::string instance_root)
      : spec_(spec), layer_(layer), index_(index),
        proxy_path_(std::move(proxy_path)),
        prototype_root_(std::move(prototype_root)),
        instance_root_(std::move(instance_root)) {}

  /// Check if this prim handle is valid
  bool IsValid() const { return spec_ != nullptr; }
  explicit operator bool() const { return IsValid(); }

  /// Get prim name
  const std::string& GetName() const;

  /// Get prim type name (e.g., "Mesh", "Xform")
  const std::string& GetTypeName() const;

  /// Get prim path
  const Path& GetPath() const;

  /// Get specifier (Def, Over, Class)
  PrimSpecifier GetSpecifier() const;

  /// Check whether this prim and all ancestors resolve active=true.
  bool IsActive() const;

  /// Whether this prim and all ancestors have no deferred payload.
  bool IsLoaded() const;

  /// Check whether this prim and all ancestors have defining specifiers
  /// (`def` or `class`).
  bool IsDefined() const;

  /// Check whether this prim or any ancestor is abstract (`class`).
  bool IsAbstract() const;

  /// Check whether this prim and all ancestors are concretely defining (`def`).
  bool IsConcretelyDefined() const;

  /// Check whether this prim participates in the contiguous model hierarchy
  /// (`group`/`assembly` ancestry ending in group/assembly/component).
  bool IsInModelHierarchy() const;

  // ============================================================
  // Properties
  // ============================================================

  /// Check if prim has a property
  bool HasProperty(const std::string& name) const;
  bool HasProperty(PropNameId name_id) const;

  /// Check if a property is authored on the prim (or its instance-proxy
  /// source), excluding schema fallback declarations.
  bool HasAuthoredProperty(const std::string& name) const;

  /// Resolve at USD DefaultTime: authored default, then schema fallback.
  /// Time samples are never consulted.
  const Value* GetPropertyValue(const std::string& name) const;

  /// Get property value by pre-registered ID (faster)
  const Value* GetPropertyValue(PropNameId name_id) const;

  /// Compatibility convenience: DefaultTime resolution, then the earliest
  /// time sample when no default/fallback exists.
  const Value* GetPropertyValueOrEarliestTimeSample(
      const std::string& name) const;
  const Value* GetPropertyValueOrEarliestTimeSample(PropNameId name_id) const;

  /// Earliest time sample's value, or nullptr when the property has no
  /// samples. This is not USD DefaultTime resolution.
  const Value* EarliestTimeSampleValue(PropNameId name_id) const;

  /// Get all property names
  std::vector<std::string> GetPropertyNames() const;

  // ============================================================
  // TimeSamples
  // ============================================================

  /// Fast check if this prim has any time-sampled property.
  bool HasAnyTimeSamples() const;

  /// Check if property has time samples
  bool HasTimeSamples(const std::string& name) const;
  bool HasTimeSamples(PropNameId name_id) const;

  /// Get all time sample times for a property
  std::vector<double> GetTimeSampleTimes(const std::string& name) const;
  std::vector<double> GetTimeSampleTimes(PropNameId name_id) const;
  const std::vector<std::pair<double, uint32_t>>* GetTimeSamples(
      PropNameId name_id) const;

  /// Get value at specific time (returns closest sample, no interpolation)
  const Value* GetValueAtTime(const std::string& name, double time) const;
  const Value* GetValueAtTime(PropNameId name_id, double time) const;

  /// Get interpolated value at time (linear interpolation for numeric types)
  Value GetInterpolatedValue(const std::string& name, double time) const;
  Value GetInterpolatedValue(PropNameId name_id, double time) const;

  // ============================================================
  // Relationships
  // ============================================================

  /// Get relationship targets
  const std::vector<Path>* GetRelationship(const std::string& name) const;

  /// Resolve relationship-to-relationship target chains. Terminal prim and
  /// attribute paths are returned in first-seen order with duplicates removed.
  /// Cycles are ignored after the first visit. Returns false when this prim has
  /// no relationship with `name` or `targets` is null.
  bool GetForwardedRelationshipTargets(const std::string& name,
                                       std::vector<Path>* targets) const;

  /// Get all relationship names
  std::vector<std::string> GetRelationshipNames() const;

  // ============================================================
  // Hierarchy
  // ============================================================

  /// Get parent prim
  UsdPrim GetParent() const;

  /// Get children
  std::vector<UsdPrim> GetChildren() const;

  /// Get child count
  size_t GetChildCount() const;

  /// Get child by position (no allocation, unlike GetChildren). Returns an
  /// invalid prim when out of range. Follows instance prototypes like
  /// GetChildren.
  UsdPrim GetChildAt(size_t index) const;

  /// Get child by name
  UsdPrim GetChild(const std::string& name) const;

  // ============================================================
  // Metadata
  // ============================================================

  /// Get prim metadata
  const PrimSpecMeta& GetMeta() const;

  /// Get a property's metadata block (interpolation / customData / ...),
  /// or nullptr when none authored. Never allocates.
  const PropMeta* GetPropertyMeta(const std::string& name) const;

  /// Get underlying PrimSpec (for advanced use)
  const PrimSpec* GetPrimSpec() const { return spec_; }

  /// Get the owning layer / prim index (for handle round-tripping in
  /// bindings; pairs with the (spec, layer, index) constructor).
  const Layer* GetLayer() const { return layer_; }
  uint32_t GetIndex() const { return index_; }

private:
  // Resolves to the prototype's spec when this prim is an instance proxy
  // (meta().instance_prototype() set); otherwise returns spec_. Used for child
  // enumeration so instance children come from the prototype.
  const PrimSpec* ChildSourceSpec() const;

  const PrimSpec* spec_ = nullptr;
  const Layer* layer_ = nullptr;
  uint32_t index_ = UINT32_MAX;
  Path proxy_path_;
  std::string prototype_root_;
  std::string instance_root_;

  friend class Stage;
};

/// Stage - composed scene built from layers
/// Provides a unified view of prims across layer stack
class Stage {
public:
  Stage();
  ~Stage();

  // Move only
  Stage(Stage&&) noexcept;
  Stage& operator=(Stage&&) noexcept;
  Stage(const Stage&) = delete;
  Stage& operator=(const Stage&) = delete;

  /// Clone this stage. Array-valued properties retain their copy-on-write or
  /// lazy backing, so snapshots can detach before a destructive compaction
  /// without duplicating the largest payloads.
  Stage Clone() const;

  // ============================================================
  // Loading
  // ============================================================

  /// Set the root layer (takes ownership)
  void SetRootLayer(Layer&& layer);

  /// Get the root layer
  const Layer* GetRootLayer() const { return root_layer_.get(); }
  Layer* GetRootLayer() { return root_layer_.get(); }

  /// Move the root layer out of the stage (transfers ownership). Used by the
  /// pcp LayerRegistry to obtain a shareable Layer from a freshly-loaded Stage.
  /// After this the stage has no root layer.
  std::unique_ptr<Layer> ReleaseRootLayer();

  /// Add a sublayer (for composition)
  void AddSubLayer(Layer&& layer);

  /// Get sublayer count
  size_t GetSubLayerCount() const { return sub_layers_.size(); }

  /// Get sublayer by index
  const Layer* GetSubLayer(size_t index) const;

  // ============================================================
  // Prim Access
  // ============================================================

  /// Get the pseudo-root prim (parent of all root prims)
  UsdPrim GetPseudoRoot() const;

  /// Get prim by path
  UsdPrim GetPrimAtPath(const Path& path) const;
  UsdPrim GetPrimAtPath(const std::string& path) const;

  /// Get the default prim (if set)
  UsdPrim GetDefaultPrim() const;

  /// Get all root prims
  std::vector<UsdPrim> GetRootPrims() const;

  /// Check if a prim exists at path
  bool HasPrimAtPath(const Path& path) const;
  bool HasPrimAtPath(const std::string& path) const;

  // ============================================================
  // Traversal
  // ============================================================

  /// Traverse all prims in depth-first order
  /// Callback signature: bool(const UsdPrim& prim)
  /// Return false from callback to stop traversal
  using VisitFn = bool (*)(void* context, const UsdPrim& prim);
  // Synchronous: context is borrowed for this call only.
  void Traverse(void* context, VisitFn callback) const;
  template<typename Fn>
  void Traverse(Fn&& callback) const;

  /// Traverse prims matching a predicate
  template<typename Fn, typename Pred>
  void TraverseIf(Fn&& callback, Pred&& predicate) const;

  /// Get all prims of a specific type
  std::vector<UsdPrim> GetPrimsOfType(const std::string& typeName) const;

  /// Flatten to a single layer (apply all composition)
  Layer Flatten() const;

  // ============================================================
  // Time
  // ============================================================

  /// Get start time code
  double GetStartTimeCode() const;

  /// Get end time code
  double GetEndTimeCode() const;

  /// Get time codes per second
  double GetTimeCodesPerSecond() const;

  /// Check if stage has time samples
  bool HasTimeSamples() const;

  /// Check if any prim in the stage carries value-clip metadata (`clips`
  /// dictionary on the prim spec). Lightweight metadata-only scan.
  bool HasValueClips() const;

  // ============================================================
  // Metadata
  // ============================================================

  /// Get stage metadata
  const StageMeta& GetMeta() const { return meta_; }
  StageMeta& GetMeta() { return meta_; }

  /// Get up axis ("Y" or "Z")
  const std::string& GetUpAxis() const { return meta_.upAxis; }

  /// Get meters per unit
  double GetMetersPerUnit() const { return meta_.metersPerUnit; }

  // ============================================================
  // Statistics
  // ============================================================

  /// Get total prim count
  size_t GetPrimCount() const;

  /// Get memory usage
  size_t GetMemoryUsage() const;

  struct Stats {
    size_t prim_count;
    size_t layer_count;
    size_t total_properties;
    size_t memory_bytes;
  };
  Stats GetStats() const;

  struct StaticGeometryReleaseStats {
    size_t property_count{0};
    size_t element_count{0};
    size_t estimated_payload_bytes{0};
    size_t stage_bytes_before{0};
    size_t stage_bytes_after{0};
  };

  /// Drop large, non-time-sampled geometry defaults after a renderer has made
  /// its own copy. Prim hierarchy, property slots/types/metadata, transforms,
  /// cameras, materials, skeletons and animation stay resident. This is lossy;
  /// use it only when the Stage can be reconstructed from its source layers.
  StaticGeometryReleaseStats ReleaseStaticGeometryArrays(
      size_t min_array_elements = 256);
  /// Last-use variant for streaming converters. The caller must guarantee no
  /// worker still reads this prim. Stage byte totals are left zero to keep this
  /// O(properties) rather than rescanning the whole stage per prim.
  StaticGeometryReleaseStats ReleaseStaticGeometryArraysForPrim(
      const UsdPrim& prim, size_t min_array_elements = 256);

private:
  std::unique_ptr<Layer> root_layer_;
  std::vector<std::unique_ptr<Layer>> sub_layers_;
  StageMeta meta_;

  // Internal helpers
  void UpdateMetaFromRootLayer();
  bool TraverseImpl(uint32_t prim_index, const Layer* layer,
                    void* context, VisitFn callback) const;
};

// ============================================================
// Template implementations
// ============================================================

template<typename Fn>
void Stage::Traverse(Fn&& callback) const {
  using Callback = typename std::decay<Fn>::type;
  Callback local(std::forward<Fn>(callback));
  Traverse(&local, [](void* context, const UsdPrim& prim) {
    return (*static_cast<Callback*>(context))(prim);
  });
}

template<typename Fn, typename Pred>
void Stage::TraverseIf(Fn&& callback, Pred&& predicate) const {
  Traverse([&](const UsdPrim& prim) {
    if (predicate(prim)) {
      return callback(prim);
    }
    return true;  // Continue traversal
  });
}

// ============================================================
// Stage Builder - helper for constructing stages
// ============================================================

class StageBuilder {
public:
  StageBuilder();
  ~StageBuilder();

  /// Set stage metadata
  void SetDefaultPrim(const std::string& primName);
  void SetUpAxis(const std::string& axis);
  void SetMetersPerUnit(double value);
  void SetTimeCodesPerSecond(double fps);
  void SetStartTimeCode(double time);
  void SetEndTimeCode(double time);

  /// Get the layer builder for constructing prims
  LayerBuilder& GetLayerBuilder() { return *layer_builder_; }

  /// Build the stage (moves ownership of layer to stage)
  Stage Build();

private:
  std::unique_ptr<Layer> layer_;
  std::unique_ptr<LayerBuilder> layer_builder_;
  StageMeta meta_;
};

}  // namespace next
}  // namespace lightusd
