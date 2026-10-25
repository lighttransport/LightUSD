// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.
//
// Experimental USDC (Crate) File Writer
// Bare framework for writing USD Layer/PrimSpec to binary Crate format
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <fstream>

// LightUSD crate format definitions
#include "crate-format.hh"
#include "stage.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdMtlx.hh"  // For MtlxOpenPBRSurface
#include "usdSkel.hh"
#include "usdLux.hh"

// Path sorting and encoding library
#include "crate-path-utils/path_interface.hh"
#include "crate-path-utils/path_sort.hh"
#include "crate-path-utils/tree_encode.hh"

namespace lightusd {
namespace experimental {

// Maximum prim nesting depth supported by the USDC writer. The path-tree
// builder recurses once per nesting level on the native stack, so this bounds
// recursion to a stack-safe value. ConvertPrimIterative enforces this as the
// authoritative, early check (clear error before the path-tree pass); the
// path-tree builder keeps a slightly larger backstop (property/root paths add
// one extra level), so a too-deep stage always fails at prim conversion first
// rather than cryptically during path-tree building.
static constexpr uint32_t kMaxPrimNestingDepth = 512;

///
/// ErrorContextStack - Tracks error context during crate writing operations
///
/// Provides detailed error messages with full operation history and path context
///
class ErrorContextStack {
 public:
  ErrorContextStack(int max_depth = 10) : max_depth_(max_depth) {}

  /// Push an operation context onto the stack
  void Push(const std::string& operation) {
    if (static_cast<int>(stack_.size()) < max_depth_) {
      stack_.push_back(operation);
    }
  }

  /// Pop an operation context from the stack
  void Pop() {
    if (!stack_.empty()) {
      stack_.pop_back();
    }
  }

  /// Get the current full context path
  std::string GetContextPath() const {
    if (stack_.empty()) return "";

    std::string result;
    for (size_t i = 0; i < stack_.size(); ++i) {
      if (i > 0) result += " -> ";
      result += stack_[i];
    }
    return result;
  }

  /// Get detailed error message with context
  std::string GetDetailedErrorMessage(const std::string& base_error) const {
    if (stack_.empty()) {
      return base_error;
    }

    std::string detailed = base_error + "\nContext Stack:\n";
    for (size_t i = 0; i < stack_.size(); ++i) {
      detailed += "  [" + std::to_string(i) + "] " + stack_[i] + "\n";
    }
    return detailed;
  }

  /// Check if stack is at maximum depth
  bool IsMaxDepth() const {
    return static_cast<int>(stack_.size()) >= max_depth_;
  }

  /// Clear the entire stack
  void Clear() { stack_.clear(); }

  /// Get stack size
  size_t Size() const { return stack_.size(); }

 private:
  std::vector<std::string> stack_;
  int max_depth_;
};

///
/// IOutputStream - Abstract output stream for CrateWriter I/O
///
/// Allows CrateWriter to write to files or memory buffers interchangeably.
///
class IOutputStream {
public:
  virtual ~IOutputStream();
  virtual bool Open(std::string* err) = 0;
  virtual void Close() = 0;
  virtual bool IsOpen() const = 0;
  virtual int64_t Tell() = 0;
  virtual bool Seek(int64_t pos) = 0;
  virtual bool Write(const void* data, size_t size) = 0;
  virtual bool Flush() = 0;
};

///
/// MemoryOutputStream - Writes to an in-memory buffer
///
/// Used by SaveAsUSDCToMemory to avoid temp-file round-trips.
///
class MemoryOutputStream : public IOutputStream {
public:
  MemoryOutputStream() = default;
  ~MemoryOutputStream() override;
  bool Open(std::string*) override { pos_ = 0; buffer_.clear(); return true; }
  void Close() override {}
  bool IsOpen() const override { return true; }
  int64_t Tell() override { return static_cast<int64_t>(pos_); }
  bool Seek(int64_t pos) override {
    if (pos < 0) return false;
    size_t p = static_cast<size_t>(pos);
    if (p > buffer_.size()) buffer_.resize(p, 0);
    pos_ = p;
    return true;
  }
  bool Write(const void* data, size_t size) override {
    if (pos_ + size > buffer_.size()) buffer_.resize(pos_ + size);
    std::memcpy(buffer_.data() + pos_, data, size);
    pos_ += size;
    return true;
  }
  bool Flush() override { return true; }

  std::vector<uint8_t> TakeBuffer() { return std::move(buffer_); }
  const std::vector<uint8_t>& GetBuffer() const { return buffer_; }
private:
  std::vector<uint8_t> buffer_;
  size_t pos_ = 0;
};

///
/// CrateWriter - Experimental framework for writing USDC binary files
///
/// This is a bare-bones implementation focusing on core structure.
/// Implements Crate format version 0.8.0 (stable, production-ready)
///
/// Key features:
/// - Bootstrap header with magic "PXR-USDC"
/// - Table of Contents (TOC) structure
/// - Structural sections: TOKENS, STRINGS, FIELDS, FIELDSETS, PATHS, SPECS
/// - Token/String/Path/Value deduplication
/// - Integration with path sorting/encoding library
///
/// Current limitations (experimental):
/// - No compression (will add in future)
/// - Limited type support (basic types only)
/// - No async I/O
/// - No zero-copy optimization
///
class CrateWriter {
public:
  /// Create a new crate writer for the given file path
  explicit CrateWriter(const std::string& filepath);

  /// Create a new crate writer with a custom output stream
  explicit CrateWriter(std::unique_ptr<IOutputStream> stream);

  ~CrateWriter();

  ///
  /// Initialize the writer and prepare for writing
  ///
  bool Open(std::string* err = nullptr);

  ///
  /// Add a spec (Prim, Property, etc.) to the file
  ///
  /// @param path Path for this spec (e.g., "/Root/Geo/Mesh")
  /// @param spec_type Type of spec (Prim, Attribute, etc.)
  /// @param fields Map of field name -> value for this spec
  ///
  /// `fields` is taken by value: pass with std::move() where possible —
  /// field values can carry large arrays and are stored, not inspected.
  bool AddSpec(const Path& path,
               SpecType spec_type,
               crate::FieldValuePairVector fields,
               std::string* err = nullptr);

  ///
  /// Finalize and write the file
  ///
  /// This performs:
  /// 1. Sort all paths
  /// 2. Encode path tree
  /// 3. Write all sections
  /// 4. Write TOC
  /// 5. Write bootstrap header
  ///
  bool Finalize(std::string* err = nullptr);

  ///
  /// Close the file (called automatically by destructor)
  ///
  void Close();

  ///
  /// Convert a LightUSD Stage to Crate specs and add them to the file
  ///
  /// This traverses the stage hierarchy and converts all prims and
  /// their properties to Crate format specs.
  ///
  /// @param stage The LightUSD Stage to convert
  /// @param err Optional error message output
  /// @return true on success, false on failure
  ///
  bool ConvertStageToSpecs(const Stage& stage, std::string* err = nullptr);

  ///
  /// Convert a LightUSD Layer to Crate specs and add them to the file
  ///
  /// This traverses the Layer's PrimSpecs and converts them along with
  /// their properties, relationships, and connections to Crate format.
  ///
  /// @param layer The LightUSD Layer to convert
  /// @param err Optional error message output
  /// @return true on success, false on failure
  ///
  bool ConvertLayerToSpecs(const Layer& layer, std::string* err = nullptr);

  // Resource tracking and limit enforcement
  ///
  /// Get current file size in bytes
  ///
  int64_t GetBytesWritten() const { return bytes_written_; }

  ///
  /// Get estimated memory usage
  ///
  int64_t GetMemoryUsageEstimate() const {
    return memory_used_estimate_.load(std::memory_order_relaxed);
  }

  ///
  /// Check if file size limit would be exceeded
  ///
  bool WouldExceedFileSizeLimit(int64_t additional_bytes) const {
    return (bytes_written_ + additional_bytes) > options_.max_file_size_bytes;
  }

  ///
  /// Check if memory limit would be exceeded
  ///
  bool WouldExceedMemoryLimit(int64_t additional_bytes) const {
    return (memory_used_estimate_.load(std::memory_order_relaxed) +
            additional_bytes) > options_.max_memory_bytes;
  }

  // Error context tracking
  ///
  /// Get the current error context path
  ///
  std::string GetErrorContextPath() const {
    return error_context_.GetContextPath();
  }

  ///
  /// Get detailed error message with full context
  ///
  std::string GetDetailedErrorMessage(const std::string& error) const {
    return error_context_.GetDetailedErrorMessage(error);
  }

  ///
  /// Clear the error context stack
  ///
  void ClearErrorContext() {
    error_context_.Clear();
  }

  // Validation methods
  ///
  /// Validate a Stage before writing
  /// Checks for common issues like invalid paths, missing prims, cycles, etc.
  ///
  /// @param stage The Stage to validate
  /// @param err Optional error message output
  /// @return true if valid, false if validation fails
  ///
  bool ValidateStage(const Stage& stage, std::string* err = nullptr);

  ///
  /// Validate a Layer before writing
  /// Checks for common issues in PrimSpecs and properties
  ///
  /// @param layer The Layer to validate
  /// @param err Optional error message output
  /// @return true if valid, false if validation fails
  ///
  bool ValidateLayer(const Layer& layer, std::string* err = nullptr);

  ///
  /// Get validation results summary
  ///
  std::string GetValidationSummary() const;

  // Configuration options
  struct Options {
    uint8_t version_major = 0;
    uint8_t version_minor = 8;  // Default to 0.8.0 (stable)
    uint8_t version_patch = 0;

    bool enable_compression = true;   // Phase 4: LZ4 compression enabled by default
    bool enable_deduplication = true; // Deduplicate tokens/strings/paths/values
    bool enable_validation = true;    // Phase 5: Pre-write validation enabled by default

    // OpenUSD-compatible tagged compression for float[]/double[] arrays
    // (code 'i' = integers, 't' = lookup table; see doc/crate-writer.md).
    // Default OFF: float/double arrays are written uncompressed. Requires
    // enable_compression as well (it gates the integer-stream compressor used
    // by the 'i'/'t' payloads).
    bool enable_float_array_compression = false;

    // Memory and file size limits (with WASM-specific defaults)
    // These prevent resource exhaustion when processing untrusted USD files
#ifdef __wasm__
    // WASM environment has stricter limits
    int64_t max_memory_bytes = 256 * 1024 * 1024;      // 256 MB default for WASM
    int64_t max_file_size_bytes = 100 * 1024 * 1024;   // 100 MB default for WASM
#else
    // Desktop/server environment limits
    int64_t max_memory_bytes = 32 * 1024 * 1024 * 1024LL;  // 32 GB default
    int64_t max_file_size_bytes = 1024 * 1024 * 1024LL;    // 1 GB default
#endif

    // Error context depth (how many stack frames to include in error messages)
    int error_context_depth = 5;
  };

  void SetOptions(const Options& opts) { options_ = opts; }

  /// Raise the minimum emitted crate header version. Called when a value that
  /// requires a newer crate version is written (e.g. SdfPathExpression needs
  /// 0.10.0, TsSpline needs 0.12.0). The boot header emits
  /// max(options_ version, requested version). Mirrors OpenUSD's
  /// RequestWriteVersionUpgrade.
  void RequestCrateVersionUpgrade(uint8_t maj, uint8_t min, uint8_t patch) {
    const bool higher =
        (maj > required_version_major_) ||
        (maj == required_version_major_ && min > required_version_minor_) ||
        (maj == required_version_major_ && min == required_version_minor_ &&
         patch > required_version_patch_);
    if (higher) {
      required_version_major_ = maj;
      required_version_minor_ = min;
      required_version_patch_ = patch;
    }
  }

private:
  // Minimum crate version required by values written so far (0.0.0 = no extra
  // requirement beyond options_). See RequestCrateVersionUpgrade().
  uint8_t required_version_major_{0};
  uint8_t required_version_minor_{0};
  uint8_t required_version_patch_{0};

  // ======================================================================
  // Internal structures
  // ======================================================================

  /// Bootstrap header (64 bytes, at file offset 0)
  struct BootStrap {
    char ident[8];       // "PXR-USDC"
    uint8_t version[8];  // [major, minor, patch, 0, 0, 0, 0, 0]
    int64_t toc_offset;  // File offset to table of contents
    int64_t reserved[6]; // Reserved for future use
  };

  /// Internal spec representation before writing
  struct SpecData {
    Path path;
    SpecType spec_type;  // Store the spec type for later use
    crate::Spec spec;
    crate::FieldValuePairVector fields;
    // Number of fields present when AddSpec ran. The parallel stage->specs
    // token replay registers only this prefix: fields appended AFTER AddSpec
    // (e.g. the post-hoc `properties` field) were never pre-registered by the
    // serial writer either — their tokens are first seen during Finalize's
    // field packing, and replaying them early would shift every later token
    // index (byte-diff).
    uint32_t fields_at_addspec = 0;
  };

  // ======================================================================
  // Section writing
  // ======================================================================

  /// Write the TOKENS section (token string pool)
  bool WriteTokensSection(std::string* err);

  /// Write the STRINGS section (string -> token index mapping)
  bool WriteStringsSection(std::string* err);

  /// Write the FIELDS section (field name + value pairs)
  bool WriteFieldsSection(std::string* err);

  /// Write the FIELDSETS section (lists of field indices)
  bool WriteFieldSetsSection(std::string* err);

  /// Write the PATHS section (compressed path tree)
  bool WritePathsSection(std::string* err);

  /// Write the SPECS section (spec data)
  bool WriteSpecsSection(std::string* err);

  /// Write the Table of Contents
  bool WriteTableOfContents(std::string* err);

  /// Write the Bootstrap header
  bool WriteBootStrap(std::string* err);

  // ======================================================================
  // Stage conversion helpers
  // ======================================================================

  /// Convert a Prim and its children recursively
  /// Convert a single prim (no children) and add its specs.
  /// Returns true on success. Does not recurse into children.
  bool ConvertSinglePrim(const Prim& prim, const Path& parent_path, std::string* err);

  /// Iterative depth-first conversion of a prim tree.
  bool ConvertPrimIterative(const Prim& root_prim, const Path& parent_path, std::string* err);

  /// Extract properties from a Prim and add as fields
  bool ExtractPrimProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract type-specific properties based on prim type
  bool ExtractTypeSpecificProperties(const Prim& prim, const Path& prim_path, const std::string& type_name,
                                     crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Xform-specific properties (xformOps)
  bool ExtractXformProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Mesh-specific properties (points, normals, etc.)
  bool ExtractMeshProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// UsdVol: extract Volume `field:*` relationships
  bool ExtractVolumeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract VolumeFieldBase inherited attributes only (none of its own)
  bool ExtractVolumeFieldBaseProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract FieldBase (deprecated) inherited attributes only
  bool ExtractFieldBaseProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract VolumeFieldAsset attributes (filePath, fieldName, ...)
  bool ExtractVolumeFieldAssetProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract FieldAsset attributes (filePath, fieldName, ...)
  bool ExtractFieldAssetProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract OpenVDBAsset attributes
  bool ExtractOpenVDBAssetProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  /// UsdVol: extract Field3DAsset attributes
  bool ExtractField3DAssetProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Cube-specific properties (size, extent)
  bool ExtractCubeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Sphere-specific properties (radius)
  bool ExtractSphereProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Cylinder-specific properties (radius, height)
  bool ExtractCylinderProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Cone-specific properties (radius, height, axis)
  bool ExtractConeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Capsule-specific properties (radius, height, axis)
  bool ExtractCapsuleProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Points-specific properties (points, widths, ids, normals, velocities, accelerations)
  bool ExtractPointsProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Camera-specific properties (focal length, aperture, clipping range, etc.)
  bool ExtractCameraProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract BasisCurves-specific properties (type, basis, wrap, points, widths, etc.)
  bool ExtractBasisCurvesProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract NurbsCurves-specific properties (order, knots, ranges, pointWeights, points, etc.)
  bool ExtractNurbsCurvesProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomPlane properties
  bool ExtractGeomPlaneProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomCylinder_1 properties
  bool ExtractGeomCylinder1Properties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomCapsule_1 properties
  bool ExtractGeomCapsule1Properties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomTetMesh properties
  bool ExtractGeomTetMeshProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomNurbsPatch properties
  bool ExtractGeomNurbsPatchProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomHermiteCurves properties
  bool ExtractGeomHermiteCurvesProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract PointInstancer-specific properties (protoIndices, positions, orientations, scales, velocities, etc.)
  bool ExtractPointInstancerProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeomSubset-specific properties (elementType, familyName, indices)
  bool ExtractGeomSubsetProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract BlendShape properties (offsets, normalOffsets, pointIndices)
  bool ExtractBlendShapeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract SphereLight properties (radius, color, intensity, exposure, shaping)
  bool ExtractSphereLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract RectLight properties (width, height, color, intensity, exposure, shaping)
  bool ExtractRectLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract DiskLight properties (radius, color, intensity, exposure, shaping)
  bool ExtractDiskLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract CylinderLight properties (radius, length, color, intensity, exposure, shaping)
  bool ExtractCylinderLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract DistantLight properties (angle, color, intensity, exposure)
  bool ExtractDistantLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract DomeLight properties (texture path, color, intensity, exposure)
  bool ExtractDomeLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract GeometryLight properties (geometry relationship, color, intensity, exposure)
  bool ExtractGeometryLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract PortalLight properties (geometry relationship, color, intensity, exposure)
  bool ExtractPortalLightProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract DomeLight_1 properties
  bool ExtractDomeLight1Properties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract LightFilter properties
  bool ExtractLightFilterProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract PluginLightFilter properties
  bool ExtractPluginLightFilterProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Skeleton properties (jointNames, joints, bindTransforms, restTransforms)
  bool ExtractSkeletonProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract SkelAnimation properties (joints, rotations, translations, scales, blendShapeWeights)
  bool ExtractSkelAnimationProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract SkelRoot properties (visibility, purpose, extent)
  bool ExtractSkelRootProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Physics prim properties
  bool ExtractPhysicsJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsSceneProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsRevoluteJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsPrismaticJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsSphericalJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsFixedJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsDistanceJointProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPhysicsCollisionGroupProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractMjcActuatorProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractNewtonActuatorProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractMjcTendonProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractMjcKeyframeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractMjcSensorProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract AR/Interactive prim properties (Apple Preliminary_*)
  bool ExtractPreliminaryGravitationalForceProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryInfiniteColliderPlaneProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryReferenceImageProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryBehaviorProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryTriggerProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryActionProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  bool ExtractPreliminaryTextProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract usdMedia prim properties
  bool ExtractSpatialAudioProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract PrimMeta fields (kind, active, hidden, customData, apiSchemas, references, payload, inherits, specializes, etc.)
  void ExtractPrimMeta(const PrimMeta& metas, crate::FieldValuePairVector& fields);

  /// Extract common GPrim properties (visibility, purpose, etc.)
  bool ExtractImageableAttrs(const TypedAttributeWithFallback<Animatable<Visibility>>& visibility, const TypedAttributeWithFallback<Purpose>& purpose, crate::FieldValuePairVector& fields, std::string* err);

  bool ExtractScopeProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);
  // Shared by every imageable prim (GPrim, Scope, lights, Material/NodeGraph).
  bool ExtractGPrimProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract Material properties (outputs: surface, displacement, volume)
  bool ExtractMaterialProperties(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Add Material output specs as separate attribute specs (called after Material prim spec is added)
  bool AddMaterialOutputSpecs(const Material* material, const Path& prim_path, std::string* err);

  /// Extract Shader properties (info:id, inputs, outputs)
  bool ExtractShaderProperties(const Prim& prim, crate::FieldValuePairVector& fields, std::string* err);

  /// Extract NodeGraph properties (shader network container)
  bool ExtractNodeGraphProperties(const Prim& prim, crate::FieldValuePairVector& fields, std::string* err);

  /// Add UsdPreviewSurface shader input specs as separate attribute specs (called after Shader prim spec is added)
  bool AddUsdPreviewSurfaceInputSpecs(const UsdPreviewSurface* preview_surface, const Path& prim_path, std::string* err);

  /// Add UsdUVTexture shader input specs as separate attribute specs (called after Shader prim spec is added)
  bool AddUsdUVTextureInputSpecs(const UsdUVTexture* uv_texture, const Path& prim_path, std::string* err);

  /// Add UsdPrimvarReader shader input specs as separate attribute specs (called after Shader prim spec is added)
  /// This works with all UsdPrimvarReader_* variants (float, float2, float3, etc.)
  bool AddUsdPrimvarReaderInputSpecs(const value::Value& shader_value, const std::string& reader_type, const Path& prim_path, std::string* err);

  /// Add UsdTransform2d shader input specs as separate attribute specs (called after Shader prim spec is added)
  bool AddUsdTransform2dInputSpecs(const UsdTransform2d* transform2d, const Path& prim_path, std::string* err);

  /// Add MtlxOpenPBRSurface shader input specs as separate attribute specs.
  /// Mirrors AddUsdPreviewSurfaceInputSpecs (value + connectionPaths); without
  /// this the typed MaterialX OpenPBR inputs are dropped on stage->USDC write.
  bool AddMtlxOpenPBRSurfaceInputSpecs(const MtlxOpenPBRSurface* surface, const Path& prim_path, std::string* err);

  /// Add material binding relationships as separate relationship specs (called after Prim spec is added)
  /// Handles material:binding, material:binding:preview, and material:binding:full relationships
  bool AddMaterialBindingSpecs(const Prim& prim, const Path& prim_path, std::string* err);

  /// Add light filter relationships as separate relationship specs (called after Light prim spec is added)
  /// Handles light:filters relationships for SphereLight, RectLight, DiskLight, CylinderLight, DistantLight, DomeLight
  bool AddLightFilterSpecs(const Prim& prim, const Path& prim_path, std::string* err);

  /// Add PointInstancer prototypes relationship as separate relationship spec
  bool AddPointInstancerPrototypesSpec(const Prim& prim, const Path& prim_path, std::string* err);

  /// Add SkelBindingAPI relationships as separate relationship specs (called after Prim spec is added)
  /// Handles skel:animationSource (Skeleton, SkelRoot), skel:skeleton (SkelRoot, GeomMesh),
  /// and skel:blendShapeTargets (GeomMesh). These are stored as typed optional Relationship
  /// fields, so they are not covered by the generic props_map serialization path.
  bool AddSkelBindingSpecs(const Prim& prim, const Path& prim_path, std::string* err);

  /// Extract xformOps from Xformable (GPrim or Xform)
  /// Creates separate Attribute specs for each xformOp property
  bool ExtractXformOpsFromXformable(const Prim& prim, const Path& prim_path, crate::FieldValuePairVector& fields, std::string* err);

  /// Convert LightUSD value to CrateValue
  bool ConvertValue(const value::Value& val, crate::CrateValue& out, std::string* err);

  /// Convert a value::Value to CrateValue and append to fields.
  bool AddArrayAttribute(const std::string& attr_name, const value::Value& val,
                         crate::FieldValuePairVector& fields, std::string* err);

  /// Like AddArrayAttribute but emits a separate property spec carrying the
  /// supplied AttrMetas (interpolation, customData, displayName, etc.), so
  /// metadata authored on a typed attribute survives USDC roundtrip.
  bool AddArrayAttributeWithMetas(const std::string& attr_name,
                                  const value::Value& val,
                                  const AttrMeta& metas,
                                  const Path& prim_path,
                                  std::string* err);

  /// Convert an enum string to a token and append to fields.
  void AddEnumAttribute(const std::string& attr_name, const std::string& enum_val,
                        crate::FieldValuePairVector& fields);

  /// Convert a typed optional attribute to CrateValue and append to fields.
  /// Silently skips unsupported types rather than failing.
  template<typename T>
  bool AddTypedArrayAttribute(const char* name, const T& typed_attr,
                              crate::FieldValuePairVector& fields);

  /// Extract default value (and time samples if present) from an Animatable<T>
  /// into FieldValuePairVector.  Reduces the 10-line per-attribute boilerplate
  /// in Extract*Properties helpers to a single call.
  template<typename T>
  bool ExtractAnimatableDefault(const Animatable<T>& anim, const char* name,
                                crate::FieldValuePairVector& fields, std::string* err);

  /// Emit one TypedAttributeWithFallback<Animatable<T>>, whole: its value (or,
  /// if it was DECLARED without one, a bare declaration rather than the schema
  /// fallback), plus its metadata block and its connections.
  ///
  /// Always prefer this to calling ExtractAnimatableDefault() off `authored()`:
  /// get_value() returns the SCHEMA FALLBACK for an authored-but-empty
  /// attribute, so doing so writes `double radius = 2` for a bare
  /// `double radius` -- inventing a STRONG opinion the author never authored.
  /// The declared type is derived from T, so call sites cannot get it wrong.
  /// Defined in sconv-geom.cc, alongside the helpers it composes.
  template<typename T>
  bool EmitTypedAnimatableAttr(const char* name,
                               const TypedAttributeWithFallback<Animatable<T>>& attr,
                               crate::FieldValuePairVector& fields,
                               std::string* err);

  // ======================================================================
  // Layer/PrimSpec conversion helpers
  // ======================================================================

  /// Build fields for a PrimSpec (shared by PrimSpec/VariantSpec writers)
  bool BuildPrimSpecFields(const PrimSpec& primspec, const Path& prim_path,
                           crate::FieldValuePairVector& fields, std::string* err);

  /// Convert a PrimSpec and its children recursively
  /// Convert a single PrimSpec (no children) and add its specs.
  bool ConvertSinglePrimSpec(const PrimSpec& primspec, const Path& parent_path, std::string* err);

  /// Iterative depth-first conversion of a PrimSpec tree.
  /// name_override is used when the root PrimSpec has an empty name()
  /// (e.g., when the name lives only in the Layer's primspecs map key).
  bool ConvertPrimSpecIterative(const PrimSpec& primspec, const Path& parent_path,
                                std::string* err, const std::string& name_override = "");

  /// Convert a Property to Fields (handles Attribute, Relationship, Connection)
  bool ConvertPropertyToFields(const std::string& prop_name, const Property& prop,
                               const Path& parent_path, crate::FieldValuePairVector& fields,
                               std::string* err);

  /// Convert an Attribute to separate spec (proper USD format)
  /// Creates a spec with SpecType::Attribute at parent_path.AppendProperty(attr_name)
  bool ConvertAttributeToFields(const std::string& attr_name, const Attribute& attr,
                                const Path& parent_path, bool is_custom, std::string* err);

  /// Convert a Relationship to separate spec (proper USD format)
  /// Creates a spec with SpecType::Relationship at parent_path.AppendProperty(rel_name)
  bool ConvertRelationshipToFields(const std::string& rel_name, const Relationship& rel,
                                   const Path& parent_path, std::string* err,
                                   bool is_custom = false);

  /// Convert an Attribute Connection to separate spec (proper USD format)
  /// Creates a spec with SpecType::Connection
  bool ConvertConnectionToFields(const std::string& conn_name, const Attribute& attr,
                                 const Path& parent_path, std::string* err);

  /// Convert a VariantSet to separate specs (proper USD format)
  /// Creates specs with SpecType::VariantSet for variant selection metadata
  bool ConvertVariantSetToFields(const std::string& variantset_name,
                                 const VariantSet& variantset,
                                 const Path& parent_path, std::string* err,
                                 int depth = 0);

  /// Convert a VariantSetSpec to separate specs (Layer/PrimSpec variant data)
  bool ConvertVariantSetSpecToFields(const std::string& variantset_name,
                                     const VariantSetSpec& variantset,
                                     const Path& parent_path, std::string* err);

  /// Convert a Variant to separate spec (proper USD format)
  /// Creates a spec with SpecType::Variant containing variant properties and children
  bool ConvertVariantToFields(const std::string& variant_name,
                              const Variant& variant,
                              const Path& variantset_path,
                              const std::string& variantset_name,
                              std::string* err,
                              int depth = 0);

  /// Convert a Variant PrimSpec to separate spec (Layer/PrimSpec variant data)
  bool ConvertVariantSpecToFields(const std::string& variant_name,
                                  const PrimSpec& variant_primspec,
                                  const Path& parent_prim_path,
                                  const std::string& variantset_name,
                                  std::string* err);

  // ======================================================================
  // Value encoding
  // ======================================================================

  /// Pack a CrateValue into ValueRep
  /// Returns the ValueRep and may write out-of-line data to file
  crate::ValueRep PackValue(const crate::CrateValue& value, std::string* err);

  /// Pack a TokenVector metadata value into ValueRep.
  /// primChildren / properties / variantSetChildren / variantChildren must be
  /// the dedicated uncompressed TokenVector crate type (CrateDataTypeId 41):
  /// as a Token[] array pxr's hierarchy traversal cannot use them.
  crate::ValueRep PackTokenVectorValue(const std::vector<value::token>& tokens,
                                       std::string* err);

  /// Pack a StringVector metadata value (e.g. `subLayers`) into ValueRep.
  crate::ValueRep PackStringVectorValue(const std::vector<std::string>& strs,
                                        std::string* err);

  /// Pack a LayerOffsetVector metadata value (`subLayerOffsets`) into ValueRep.
  crate::ValueRep PackLayerOffsetVectorValue(
      const std::vector<LayerOffset>& offsets, std::string* err);

  /// Pack a metadata dictionary value into a Crate ValueRep.
  bool PackMetaVariable(const std::string& key, const MetaVariable& meta,
                        crate::ValueRep* value_rep, std::string* err);

  /// Write a value to the value data section
  /// Returns the file offset where the value was written
  int64_t WriteValueData(const crate::CrateValue& value, bool* is_compressed,
                         std::string* err);

  /// The body of WriteValueData: the per-type out-of-line encoding chain,
  /// WITHOUT the seek-to-section prologue / end-offset epilogue. Returns 0 on
  /// success, -1 on error. Split out so pass A of the two-pass Finalize can
  /// run it against a thread-local capture buffer (tls_value_capture) for
  /// PURE values — same encoder, two output targets, byte-equal by
  /// construction.
  int64_t WriteValueBody(const crate::CrateValue& value, bool* is_compressed,
                         std::string* err);

  /// True when WriteValueBody for this value is a pure function of the value
  /// bits routed entirely through Write/WriteBytes: no token/string/path
  /// interning, no Tell/Seek, no value_data_end_offset_ bookkeeping, no crate
  /// version bump. Exactly the numeric scalar/array branches — keep this list
  /// in lockstep with the WriteValueBody chain. Pure values may be encoded
  /// concurrently under a capture sink; everything else must run serially on
  /// the real stream.
  static bool IsPureValueData(const crate::CrateValue& value);

  /// Per-thread value-encoding capture sink. When set, WriteBytes appends to
  /// this buffer instead of the output stream (and does NOT count toward the
  /// file-size limit — the bytes are appended for real, with the limit check,
  /// when the serial pass replays them). Only WriteValueBody on
  /// IsPureValueData values may run under a capture sink.
  static std::vector<char>*& tls_value_capture();

  // ======================================================================
  // Two-pass Finalize field packing (deferred interning, round 14)
  // ======================================================================
  // Pass A runs BuildPackPlan concurrently over spec ranges: it classifies
  // each field value and precomputes everything that is a pure function of
  // the value bits (inline reps without interning; out-of-line byte images
  // via WriteValueBody under tls_value_capture; dedup descriptor + hash).
  // Pass B walks fields in the exact serial order and performs every
  // shared-state effect (interning, dedup table, stream append) — so the
  // token/string tables, dedup decisions and value offsets are byte-identical
  // to the serial writer by construction.
  struct PackPlan {
    enum Kind : uint8_t {
      kSerial = 0,   // pass B runs PackValue unchanged (index-embedding types)
      kInlinePure,   // rep fully computed in pass A (no interning involved)
      kInlineIntern, // inlines but interns; pass B re-runs TryInlineValue
      kOolPure,      // out-of-line pure: bytes prebuilt, dedup precomputed
      kOolDedupHit,  // pass-A probe hit the frozen dedup table: rep is FINAL
                     // (entries are only ever added, one per unique
                     // descriptor, and never mutated — a hit can't change)
    };
    Kind kind = kSerial;
    crate::ValueRep rep;           // kInlinePure/kOolDedupHit: final;
                                   // kOolPure: type+flags only
    bool is_compressed = false;    // kOolPure
    std::vector<char> bytes;       // kOolPure: prebuilt WriteValueBody image
    bool dedup_candidate = false;  // kOolPure
    std::vector<char> dedup_bytes;
    size_t dedup_element_size = 1;
    bool dedup_is_float = false;
    uint32_t dedup_wire_tag = 0;
    size_t dedup_hash = 0;
  };

  /// Shared pass-A window state: dedup-aware prebuild claims. The dedup table
  /// itself is frozen during pass A (pass B, which is the only mutator, runs
  /// after the window barrier), so read-only LookupDeduplicatedValue probes
  /// are safe; `claimed` arbitrates within-window duplicates so each new
  /// unique value is encoded once.
  struct PackPlanContext {
    std::mutex mu;
    std::unordered_set<size_t> claimed;  // dedup hashes claimed this window
  };

  /// Pass A per-field builder. Must not mutate any writer state — it may only
  /// read `options_`/the frozen dedup table and write through
  /// tls_value_capture (thread-local) and `ctx` (internally locked).
  void BuildPackPlan(const crate::CrateValue& value, PackPlan* plan,
                     PackPlanContext* ctx);

  /// Pass B: produce the ValueRep from a plan, byte-identically to what
  /// PackValue would do in the same table/stream state.
  crate::ValueRep PackValueFromPlan(const crate::CrateValue& value,
                                    PackPlan& plan, std::string* err);

  /// Out-of-line rep type/flag dispatch, shared by PackValue and
  /// BuildPackPlan. May bump the crate version for spline/pathexpr/relocates
  /// — those types are never IsPureValueData, so pass A (pure values only)
  /// never reaches a mutating branch.
  void SetOutOfLineRepType(const crate::CrateValue& value, crate::ValueRep* rep);

  /// Intern sink for TryInlineValue: the ONLY writer state the inline
  /// decision touches is token/string interning (token, string and inlined
  /// asset-path values store an index in the ValueRep payload). Routing those
  /// through a sink lets the same decision procedure run in two modes:
  /// - the real sink (default overload below) interns into the writer tables,
  ///   exactly the historical behavior;
  /// - a recording sink (parallel pass A of the deferred-interning two-pass)
  ///   classifies and computes rep type/flags WITHOUT mutating shared state;
  ///   the recorded requests are replayed serially in pass B for byte-identical
  ///   first-seen index assignment.
  /// There must be NO other side effect inside TryInlineValue — anything new
  /// must go through the sink or the two-pass breaks.
  struct InternSink {
    virtual ~InternSink();
    virtual uint32_t InternToken(const std::string& s) = 0;
    virtual uint32_t InternString(const std::string& s) = 0;
  };

  /// Try to inline a value in ValueRep payload (optimization).
  /// The sink-taking overload is the implementation; the two-argument form
  /// interns directly into the writer tables (historical behavior).
  bool TryInlineValue(const crate::CrateValue& value, crate::ValueRep* rep);
  bool TryInlineValue(const crate::CrateValue& value, crate::ValueRep* rep,
                      InternSink& sink);

  // ======================================================================
  // Deduplication
  // ======================================================================

  /// Generic get-or-create for deduplication tables
  template<typename KeyType, typename IndexType, typename MapType, typename VecType>
  IndexType GetOrCreateImpl(const KeyType& key, MapType& map, VecType& vec) {
    auto it = map.find(key);
    if (it != map.end()) return it->second;
    // Guard against uint32_t index overflow: the writer's memory budget
    // (~32GB) effectively prevents reaching 4B entries, but check explicitly
    // for defense-in-depth.
    if (vec.size() >= static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
      return IndexType(~0u);  // sentinel: caller should treat as error
    }
    IndexType idx(static_cast<uint32_t>(vec.size()));
    vec.push_back(key);
    map[key] = idx;
    return idx;
  }

  /// Get or create token index for a token
  crate::TokenIndex GetOrCreateToken(const std::string& token);

  /// Get or create string index for a string
  crate::StringIndex GetOrCreateString(const std::string& str);

  /// Get or create path index for a path
  crate::PathIndex GetOrCreatePath(const Path& path);

  /// Get or create field index for a field
  crate::FieldIndex GetOrCreateField(const crate::Field& field);

  /// Get or create fieldset index for a fieldset
  crate::FieldSetIndex GetOrCreateFieldSet(const std::vector<crate::FieldIndex>& fieldset);

  // ======================================================================
  // Compression (Phase 4)
  // ======================================================================

  /// Compress data using LZ4
  /// Returns true if compression succeeded, false otherwise
  /// If compression fails or expands data, original data is kept
  bool CompressData(const char* input, size_t inputSize,
                    std::vector<char>* compressed, std::string* err);

  /// Write a compressed integer array (32-bit) using Usd_IntegerCompression.
  /// If compression fails or count < 16, writes uncompressed.
  /// Sets `is_compressed` to match the bytes actually written.
  /// The data pointer must point to count uint32_t values.
  /// Returns -1 on I/O error.
  int64_t WriteCompressedArray32(const uint32_t* data, uint64_t count,
                                 const char* typeName, bool* is_compressed,
                                 std::string* err);

  /// Write a compressed integer array (64-bit) using Usd_IntegerCompression64.
  /// If compression fails or count < 16, writes uncompressed.
  /// Sets `is_compressed` to match the bytes actually written.
  /// The data pointer must point to count uint64_t values.
  /// Returns -1 on I/O error.
  int64_t WriteCompressedArray64(const uint64_t* data, uint64_t count,
                                 const char* typeName, bool* is_compressed,
                                 std::string* err);

  /// Write a float array payload using OpenUSD's tagged compression for
  /// floating-point arrays (code 'i' = compressed integers when every value is
  /// an exactly-representable integer; code 't' = lookup table + compressed
  /// indices when the array has few distinct values). Falls back to raw,
  /// uncompressed floats otherwise. The array element count must already have
  /// been written by the caller. Sets `*is_compressed` to match the bytes
  /// written (true only when a code byte + compressed payload was emitted).
  ///
  /// Compression is only attempted when both `options_.enable_float_array_compression`
  /// and `options_.enable_compression` are set (the former defaults to OFF, so
  /// the default output is uncompressed and unchanged). Returns -1 on I/O error.
  int64_t WriteCompressedFloatArray(const float* data, uint64_t count,
                                    bool* is_compressed, std::string* err);

  /// Double-precision counterpart of WriteCompressedFloatArray. Same tagged
  /// format and option gating (the 'i' path reconstructs via int32, matching
  /// the reader); falls back to raw doubles otherwise.
  int64_t WriteCompressedDoubleArray(const double* data, uint64_t count,
                                     bool* is_compressed, std::string* err);

  // ======================================================================
  // I/O utilities
  // ======================================================================

  /// Get current file position
  int64_t Tell();

  /// Seek to file position
  bool Seek(int64_t pos);

  /// Write raw bytes
  bool WriteBytes(const void* data, size_t size);

  /// Write typed value
  template<typename T>
  bool Write(const T& value) {
    return WriteBytes(&value, sizeof(T));
  }

  // ======================================================================
  // Member variables
  // ======================================================================

  std::string filepath_;
  std::unique_ptr<IOutputStream> stream_;
  Options options_;

  bool is_open_ = false;
  bool is_finalized_ = false;

  // Memory and file size tracking for resource limits
  int64_t bytes_written_ = 0;           // Current file size in bytes
  // Estimated memory usage (tokens, strings, paths, specs, etc.). Atomic:
  // the parallel stage->specs conversion accounts from worker threads.
  // (Unconditionally atomic — LIGHTUSD_ENABLE_THREAD is a private compile
  // definition, so class layout must not depend on it.)
  std::atomic<int64_t> memory_used_estimate_{0};

  // Error context stack for detailed error messages
  ErrorContextStack error_context_{options_.error_context_depth};

  // Validation tracking
  size_t validation_prim_count_ = 0;           // Number of prims validated
  size_t validation_property_count_ = 0;       // Number of properties validated
  size_t validation_warnings_count_ = 0;       // Number of validation warnings
  std::vector<std::string> validation_warnings_;  // Collected validation warnings

  // Deduplication tables
  lightusd::HashMap<std::string, crate::TokenIndex> token_to_index_;
  std::vector<std::string> tokens_;  // Index -> token string
  std::map<int32_t, uint32_t> path_tree_token_remap_;  // Maps path tree token index -> our token index

  lightusd::HashMap<std::string, crate::StringIndex> string_to_index_;
  std::vector<std::string> strings_;  // Index -> string

  // Open-addressed index over paths_: slot = (hash32, index into paths_ + 1;
  // 0 = empty). Replaces a Path -> PathIndex hash map whose per-insert Path
  // copy + node allocation was a top profile entry on spec-dense scenes.
  // The cached hash makes growth rehash-free and lets lookups skip the
  // expensive Path equality for non-matching slots.
  std::vector<std::pair<uint32_t, uint32_t>> path_slots_;
  size_t path_slots_used_ = 0;
  // Returns index into paths_, or -1 if absent.
  int64_t FindPathSlot(const Path& path, uint32_t hash) const;
  void InsertPathSlot(uint32_t hash, uint32_t path_index);
  void GrowPathSlots(size_t want);
  std::vector<Path> paths_;  // Index -> path


  lightusd::HashMap<crate::Field, crate::FieldIndex, crate::FieldHasher, crate::FieldKeyEqual> field_to_index_;
  std::vector<crate::Field> fields_;  // Index -> field

  lightusd::HashMap<std::vector<crate::FieldIndex>, crate::FieldSetIndex, crate::FieldSetHasher> fieldset_to_index_;
  std::vector<std::vector<crate::FieldIndex>> fieldsets_;  // Index -> fieldset

  // Spec data (accumulated before writing)
  std::vector<SpecData> spec_data_;

  // --- Parallel stage->specs conversion support -------------------------
  // Per-thread spec sink: when set (worker threads of the parallel
  // conversion), AddSpec appends here instead of spec_data_ and SKIPS
  // field-name token pre-registration — the tokens are replayed serially in
  // DFS spec order after assembly, reproducing the serial writer's token
  // numbering byte-for-byte.
  static std::vector<SpecData>*& tls_spec_sink();
  std::vector<SpecData>& active_spec_buffer() {
    std::vector<SpecData>* sink = tls_spec_sink();
    return sink ? *sink : spec_data_;
  }
  // Parallel whole-stage prim conversion (called by ConvertStageToSpecs when
  // the stage is large and LIGHTUSD_ENABLE_THREAD is on). Byte-identical to
  // the serial DFS.
  bool ConvertRootPrimsParallel(const Stage& stage, std::string* err);

  // Table of contents (filled during writing)
  crate::TableOfContents toc_;

  // Value data offset tracking
  int64_t value_data_start_offset_ = 0;
  int64_t value_data_end_offset_ = 0;

  // Element crate type id of the most recently packed VtArrayEdit's literals
  // array (set by WriteValueData, read by PackValue). Lets the array-edit
  // ValueRep adopt the element type from the packed literals rather than a
  // separately-tracked id (robust even for ref-only edits with empty literals).
  int32_t last_array_edit_elem_type_ = 0;

  // Recursion depth guard for nested dictionary/CustomDataType serialization.
  // Incremented at entry to WriteValueBody's dict/CustomDataType branches,
  // decremented via RAII scope guard. Fails with an error when exceeding 64.
  int dict_nesting_depth_ = 0;

  // Phase 5: authored-value storage deduplication. Floating-point values hash
  // by representation so +0/-0 and NaN payloads remain bit-exact on disk.
  // This is intentionally stricter than ordinary USD numerical equality.
  struct NanAwareHash {
    static size_t hash_float(float v) {
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof(v));
      return std::hash<uint32_t>{}(bits);
    }
    static size_t hash_double(double v) {
      uint64_t bits;
      std::memcpy(&bits, &v, sizeof(v));
      return std::hash<uint64_t>{}(bits);
    }
    static size_t combine(size_t seed, size_t h) {
      return seed ^ (h + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }
    // Hash a buffer with NaN-aware float/double handling.
    // element_size: sizeof(float) or sizeof(double) when is_float is true.
    static size_t hash_buffer(const void *data, size_t byte_count,
                              size_t element_size, bool is_float);
    // Bitwise buffer equality for authored storage.
    static bool buffers_equal(const void *a, const void *b, size_t byte_count,
                              size_t element_size, bool is_float);
  };

  struct ValueDedupEntry {
    std::vector<char> bytes;
    size_t element_size = 1;
    bool is_float = false;
    uint32_t wire_tag = 0;
    uint64_t rep_data = 0;
    size_t retained_bytes = 0;
  };

  static bool ComputeValueDedupDescriptor(const crate::CrateValue& value,
                                          std::vector<char>* bytes,
                                          size_t* element_size,
                                          bool* is_float,
                                          uint32_t* wire_tag);
  bool LookupDeduplicatedValue(const std::vector<char>& bytes,
                               size_t element_size, bool is_float,
                               uint32_t wire_tag, crate::ValueRep* rep) const;
  /// Same lookup with the descriptor hash already computed (the two-pass
  /// Finalize hashes in parallel pass A and must not re-hash serially).
  bool LookupDeduplicatedValueWithHash(size_t hash,
                                       const std::vector<char>& bytes,
                                       size_t element_size, bool is_float,
                                       uint32_t wire_tag,
                                       crate::ValueRep* rep) const;
  bool CanRetainDeduplicatedValue(size_t byte_count) const;
  void RetainDeduplicatedValue(size_t hash, std::vector<char> bytes,
                               size_t element_size, bool is_float,
                               uint32_t wire_tag,
                               const crate::ValueRep& rep);
  size_t GetValueDedupBudgetBytes() const;

  std::unordered_multimap<size_t, ValueDedupEntry> value_dedup_map_;
  size_t value_dedup_bytes_ = 0;

  // Finalize phase profiling, enabled by LIGHTUSD_CRATE_PROFILE=1 (stderr
  // report at the end of Finalize). Accumulators are only touched when
  // profile_finalize_ is set, so the default path pays one branch.
  bool profile_finalize_ = false;
  uint64_t prof_pack_total_ns_ = 0;    // whole PackValue
  uint64_t prof_pack_inline_ns_ = 0;   // TryInlineValue (incl. interning)
  uint64_t prof_pack_dedup_ns_ = 0;    // dedup descriptor + hash + lookup
  uint64_t prof_pack_write_ns_ = 0;    // WriteValueData (out-of-line encode)
  uint64_t prof_field_dedup_ns_ = 0;   // field-name intern + field/fieldset dedup
  uint64_t prof_passa_ns_ = 0;         // two-pass pass A (parallel wall)
  uint64_t prof_passb_ns_ = 0;         // two-pass pass B (serial replay wall)
};

} // namespace experimental
} // namespace lightusd
