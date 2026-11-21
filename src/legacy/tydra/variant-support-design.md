# Tydra RenderScene Variant Support Design

## Overview

This document describes the design for adding variant support to Tydra RenderScene. The approach borrows concepts from glTF's KHR_materials_variants extension while adapting to USD's nested and hierarchical nature.

## Motivation

USD supports complex variant hierarchies where variants can contain:
- Different geometry (meshes)
- Different materials
- Different properties
- Nested variants
- Complete scene branches

glTF's variant approach is optimized for:
- Flat variant lists
- Material assignments
- Efficient runtime switching

LightUSD's Tydra RenderScene needs variant support to:
1. Preserve variant information from USD files
2. Enable efficient variant switching at runtime
3. Support rendering different variants without reloading
4. Map USD variants to renderable content

## Design Goals

1. **Preservation**: Capture variant information from USD without losing structure
2. **Efficiency**: Enable fast variant switching via simple index/ID lookups
3. **Flexibility**: Support both simple (material) and complex (geometry) variants
4. **Compatibility**: Align with glTF-like approaches for web/game engines
5. **Nestability**: Support nested variants like USD does
6. **Simplicity**: Keep API simple while supporting complex scenarios

## Core Concepts

### VariantOption

A single option within a variant set. Maps to USD variant content.

```cpp
struct VariantOption {
  std::string name;           // Variant name (e.g., "red", "blue")
  std::string description;    // Optional description

  // Content references - what changes in this variant
  std::vector<int32_t> mesh_ids;           // Meshes to show (indices into RenderScene::meshes)
  std::vector<int32_t> material_ids;       // Materials to apply
  std::vector<int32_t> node_ids;           // Nodes with visibility overrides
  std::vector<int32_t> animation_ids;      // Animations specific to this variant

  // Property overrides
  std::map<std::string, std::string> property_overrides;  // String-based property values

  // Nested variants
  std::map<std::string, VariantSet> nested_variants;
};
```

### VariantSet

A set of mutually exclusive variant options.

```cpp
struct VariantSet {
  std::string name;                           // VariantSet name (e.g., "color", "lod")
  std::vector<VariantOption> options;         // All variants in this set
  int32_t default_option{0};                  // Index to default variant option

  // For nested variants
  int32_t parent_prim_id{-1};                 // Parent prim/node if variant is nested
};
```

### VariantGroup

Groups variants together at specific nodes/meshes.

```cpp
struct VariantGroup {
  std::string prim_path;                      // USD prim path (e.g., "/root/geo")
  std::vector<VariantSet> variant_sets;       // Available variant sets for this prim
  int32_t affected_node_id{-1};               // Node that contains these variants
};
```

### VariantSelection

Current active variant selection state.

```cpp
struct VariantSelection {
  std::string variant_set_name;
  std::string variant_option_name;

  bool operator==(const VariantSelection& other) const {
    return variant_set_name == other.variant_set_name &&
           variant_option_name == other.variant_option_name;
  }
};
```

## Integration with RenderScene

```cpp
class RenderScene {
  // ... existing fields ...

  // Variant support
  std::vector<VariantGroup> variant_groups;           // Groups of variants
  std::vector<VariantSelection> active_selections;    // Current variant selections
  std::map<std::string, int32_t> variant_group_map;   // prim_path -> VariantGroup index
};
```

## API Usage Examples

### Example 1: Material Variants
```cpp
// USD structure:
// Prim "Car" with variantSet "color" = { "red", "blue", "green" }

// RenderScene will have:
// - One VariantGroup for "/Car"
// - One VariantSet "color" with 3 VariantOptions
// - Each option references different material_ids

// Switch to red car:
scene.SelectVariant("color", "red");
```

### Example 2: Nested Variants
```cpp
// USD structure:
// Prim "Building" variantSet "style" = {
//   "modern" {
//     variantSet "detail" = { "simple", "detailed" }
//   }
// }

// RenderScene will have:
// - VariantGroup for "/Building"
// - VariantSet "style" with "modern" and "classic" options
// - VariantSet "detail" nested within "modern" option
```

### Example 3: Geometry Variants
```cpp
// USD structure:
// Prim "Character" variantSet "lod" = {
//   "high" { Mesh with high poly geo }
//   "low" { Mesh with low poly geo }
// }

// RenderScene will have:
// - VariantGroup for "/Character"
// - VariantSet "lod" with "high" and "low" options
// - Different mesh_ids in each option
```

## Conversion Process (RenderSceneConverter)

### Step 1: Identify Variants
- Traverse Stage looking for variantSet metadata
- Record variant locations and structures
- Map to Prim indices

### Step 2: Create VariantGroups
- For each variantSet in USD, create a VariantGroup
- Store prim_path and affected_node_id
- Create VariantSet with all options

### Step 3: Map Content
For each variant option:
- Collect affected mesh IDs
- Collect affected material IDs
- Store node visibility information
- Collect property values

### Step 4: Support Selection
- Provide API to select active variant
- Update node visibility based on selection
- Apply material/property overrides

## Implementation Phases

### Phase 1: Basic Structure (Current)
- Define data structures
- Add to RenderScene
- Create converter methods

### Phase 2: Variant Flattening (Next)
- For selected variant, create flattened scene view
- Hide/show nodes based on variant selection
- Apply material assignments

### Phase 3: Nested Support (Later)
- Support nested variants properly
- Cascade selections through hierarchy
- Update variant-dependent content

### Phase 4: Advanced Features
- Property override system
- Animation variant switching
- Custom variant callbacks

## Design Decisions

1. **Lazy vs. Eager Content Creation**: Store variant content lazily (refs to meshes/materials) rather than duplicating data

2. **Visibility Model**: Use node visibility flags rather than mesh removal for variant switching

3. **String-based Selection**: Use string names for variant/option selection (human readable) with caching for fast lookup

4. **Separate Data Structure**: Keep variants separate from core scene data to avoid bloating RenderScene

5. **Optional Feature**: Variant support is optional - apps can ignore if not needed

## Performance Considerations

- VariantSelection uses O(1) lookup with caching
- Variant switching involves visibility/material updates only
- No mesh data duplication - shared across variants
- Minimal memory overhead for variant metadata

## Future Extensions

1. **Variant Fallback**: Define fallback behavior when variant doesn't exist
2. **Variant Scripting**: Call custom functions on variant selection
3. **Variant Animation**: Transition between variants smoothly
4. **Variant Blending**: Blend between variants (60% red, 40% blue)
5. **Performance Variants**: Store pre-baked LOD variants

## Compatibility

- glTF: Can export simple variants as KHR_materials_variants
- Pixar USD: Full variant structure preserved
- Unreal Engine: Maps well to material instances
- Unity: Compatible with variant systems
