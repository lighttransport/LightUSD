// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Shared render-oriented extraction helpers for next::Stage.

#pragma once

#include <cstdint>
#include <cstddef>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>
#include <deque>

#include "next/schema/geom-point-instancer.hh"
#include "next/stage/stage.hh"
#include "next/types/value-view.hh"

namespace lightusd {
namespace tydra {
namespace next {

enum class RenderPrimKind {
  Other,
  Mesh,
  PointInstancer,
  NativeInstance,
  Light,
  Camera,
  Material,
  Volume,
  Curve,
  Skeleton
};

bool IsAnalyticGeomTypeName(const std::string& type_name);
bool IsMeshRenderableTypeName(const std::string& type_name);
bool IsUnsupportedRenderableTypeName(const std::string& type_name);
// UsdGeomImageable computed purpose ("default", "render", "proxy" or
// "guide"): the nearest authored purpose on the prim or an ancestor.
std::string ComputeInheritedPurpose(const ::lightusd::next::UsdPrim& prim);

struct RenderPrimRecord {
  ::lightusd::next::UsdPrim prim;
  RenderPrimKind kind = RenderPrimKind::Other;
  std::string path;
  std::string type_name;
  std::string purpose = "default";
  std::string material_path;
  std::string native_prototype;
  bool has_reset_xform = false;
  // True when this prim or any transform ancestor has time-varying xform
  // opinions. The extractor carries this down its traversal so consumers do
  // not repeatedly walk the same ancestry for every mesh.
  bool animated_world = false;
  double local[16];
  double world[16];
};

// Non-owning category index into RenderExtractResult::storage. Records are
// stored once at stable addresses in traversal order; category views no longer copy every string,
// prim handle, and pair of transforms.
class RenderRecordRefs {
 public:
  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = RenderPrimRecord;
    using difference_type = std::ptrdiff_t;
    using pointer = RenderPrimRecord*;
    using reference = RenderPrimRecord&;
    explicit iterator(std::vector<RenderPrimRecord*>::iterator i) : i_(i) {}
    reference operator*() const { return **i_; }
    pointer operator->() const { return *i_; }
    iterator& operator++() { ++i_; return *this; }
    bool operator==(const iterator& other) const { return i_ == other.i_; }
    bool operator!=(const iterator& other) const { return !(*this == other); }
   private:
    std::vector<RenderPrimRecord*>::iterator i_;
  };
  class const_iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = RenderPrimRecord;
    using difference_type = std::ptrdiff_t;
    using pointer = const RenderPrimRecord*;
    using reference = const RenderPrimRecord&;
    explicit const_iterator(std::vector<RenderPrimRecord*>::const_iterator i)
        : i_(i) {}
    reference operator*() const { return **i_; }
    pointer operator->() const { return *i_; }
    const_iterator& operator++() { ++i_; return *this; }
    bool operator==(const const_iterator& other) const { return i_ == other.i_; }
    bool operator!=(const const_iterator& other) const { return !(*this == other); }
   private:
    std::vector<RenderPrimRecord*>::const_iterator i_;
  };
  void push_back(RenderPrimRecord* record) { refs_.push_back(record); }
  const RenderPrimRecord& operator[](size_t i) const { return *refs_[i]; }
  const RenderPrimRecord& front() const { return *refs_.front(); }
  size_t size() const { return refs_.size(); }
  bool empty() const { return refs_.empty(); }
  void reserve(size_t n) { refs_.reserve(n); }
  iterator begin() { return iterator(refs_.begin()); }
  iterator end() { return iterator(refs_.end()); }
  const_iterator begin() const { return const_iterator(refs_.begin()); }
  const_iterator end() const { return const_iterator(refs_.end()); }
  void clear() { std::vector<RenderPrimRecord*>().swap(refs_); }
 private:
  std::vector<RenderPrimRecord*> refs_;
};

struct RenderExtractOptions {
  double time_code = 0.0;
  // Defensive traversal ceiling for composed or programmatically-created
  // stages. Zero keeps the historical unlimited behavior.
  size_t max_depth = 256;
  size_t max_records = 0;
  bool include_inactive = false;
  bool stop_at_point_instancers = false;
  bool stop_at_native_instances = false;
  bool collect_other = false;
  // Keep a combined traversal-order reference list in addition to kind lists.
  // Category membership and traversal order reference shared storage.
  bool collect_records = true;
  bool collect_categories = true;
};

struct RenderExtractResult {
  // Composed instance proxies can exceed Stage::GetPrimCount. Category views
  // retain pointers while extraction grows, so storage must not relocate.
  std::deque<RenderPrimRecord> storage;
  RenderRecordRefs records;
  RenderRecordRefs meshes;
  // Points have mesh-like topology but a separate converter/data container.
  // Keeping this category reference list lets streaming conversion release
  // traversal-order references before decoding large point payloads.
  RenderRecordRefs points;
  RenderRecordRefs point_instancers;
  RenderRecordRefs native_instances;
  RenderRecordRefs lights;
  RenderRecordRefs cameras;
  RenderRecordRefs materials;
  RenderRecordRefs volumes;
  RenderRecordRefs curves;
  RenderRecordRefs skeletons;
  std::unordered_set<std::string> native_prototype_holders;
  bool limit_exceeded = false;

  // Drop traversal-order references after hierarchy/animation processing;
  // category lists keep the shared records alive through conversion.
  void release_records() { records.clear(); }
  void release_storage() {
    records.clear();
    meshes.clear(); points.clear(); point_instancers.clear();
    native_instances.clear(); lights.clear(); cameras.clear();
    materials.clear(); volumes.clear(); curves.clear(); skeletons.clear();
    std::deque<RenderPrimRecord>().swap(storage);
  }

  // Category lists are also temporary after their conversion phase. Keeping
  // explicit release points lets streaming callers return record storage to
  // the allocator before the next large payload is decoded.
  static void release_list(RenderRecordRefs* list) {
    if (!list) return;
    for (RenderPrimRecord& rec : *list) {
      // Category membership is disjoint, so releasing a phase also frees its
      // large path/material strings before the next payload is decoded.
      rec = RenderPrimRecord();
    }
    list->clear();
  }
};

struct PointInstancerData {
  ::lightusd::next::UsdPrim prim;
  std::string path;
  std::vector<::lightusd::next::Path> prototypes;
  std::vector<int32_t> proto_indices;
  std::vector<float> positions;
  std::vector<float> orientations;
  std::vector<float> scales;
  std::vector<float> velocities;
  std::vector<float> angular_velocities;
  std::vector<int64_t> ids;
  std::vector<int64_t> invisible_ids;
  std::vector<int64_t> inactive_ids;
  std::vector<::lightusd::next::PointInstancerTransform> transforms;
  bool valid = false;
  std::string validation_error;
};

bool CollectRenderPrims(const ::lightusd::next::Stage& stage,
                        const RenderExtractOptions& options,
                        RenderExtractResult* out);

bool ReadPointInstancerData(const ::lightusd::next::UsdPrim& prim,
                            double time_code,
                            PointInstancerData* out,
                            bool compute_transforms = true);

void GatherMeshPrims(const ::lightusd::next::UsdPrim& root,
                     std::vector<::lightusd::next::UsdPrim>* out);

void CollectPrototypePaths(const ::lightusd::next::Stage& stage,
                           std::unordered_set<std::string>* out);

template <typename T>
struct ValueArrayRead {
  ::lightusd::next::ArrayScratch<T> scratch;
  ::lightusd::next::ArrayView<T> view;

  bool empty() const { return view.empty(); }
  size_t size() const { return view.size; }
  const T& operator[](size_t i) const { return view[i]; }
  const T* begin() const { return view.begin(); }
  const T* end() const { return view.end(); }
};

bool ReadFloatArray(const ::lightusd::next::UsdPrim& prim, const char* name,
                    double time, ValueArrayRead<float>* out);
bool ReadFloatArray(const ::lightusd::next::UsdPrim& prim,
                    const ::lightusd::next::PropNameId& name,
                    double time, ValueArrayRead<float>* out);
bool ReadIntArray(const ::lightusd::next::UsdPrim& prim, const char* name,
                  double time, ValueArrayRead<int32_t>* out);
bool ReadIntArray(const ::lightusd::next::UsdPrim& prim,
                  const ::lightusd::next::PropNameId& name,
                  double time, ValueArrayRead<int32_t>* out);
bool ReadInt64Array(const ::lightusd::next::UsdPrim& prim, const char* name,
                    double time, ValueArrayRead<int64_t>* out);
bool ReadInt64Array(const ::lightusd::next::UsdPrim& prim,
                    const ::lightusd::next::PropNameId& name,
                    double time, ValueArrayRead<int64_t>* out);
bool ReadUIntArray(const ::lightusd::next::UsdPrim& prim, const char* name,
                   double time, ValueArrayRead<uint32_t>* out);
bool ReadUIntArray(const ::lightusd::next::UsdPrim& prim,
                   const ::lightusd::next::PropNameId& name,
                   double time, ValueArrayRead<uint32_t>* out);
bool ReadUInt64Array(const ::lightusd::next::UsdPrim& prim, const char* name,
                     double time, ValueArrayRead<uint64_t>* out);
bool ReadUInt64Array(const ::lightusd::next::UsdPrim& prim,
                     const ::lightusd::next::PropNameId& name,
                     double time, ValueArrayRead<uint64_t>* out);

std::vector<float> ReadFloatArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                      const char* name, double time);
std::vector<float> ReadFloatArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                      const ::lightusd::next::PropNameId& name,
                                      double time);
std::vector<int32_t> ReadIntArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                      const char* name, double time);
std::vector<int32_t> ReadIntArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                      const ::lightusd::next::PropNameId& name,
                                      double time);
std::vector<int64_t> ReadInt64ArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                        const char* name, double time);
std::vector<int64_t> ReadInt64ArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                        const ::lightusd::next::PropNameId& name,
                                        double time);
std::vector<uint32_t> ReadUIntArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                        const char* name, double time);
std::vector<uint32_t> ReadUIntArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                        const ::lightusd::next::PropNameId& name,
                                        double time);
std::vector<uint64_t> ReadUInt64ArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                          const char* name, double time);
std::vector<uint64_t> ReadUInt64ArrayCopy(const ::lightusd::next::UsdPrim& prim,
                                          const ::lightusd::next::PropNameId& name,
                                          double time);

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
