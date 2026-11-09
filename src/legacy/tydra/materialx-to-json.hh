// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// MaterialX NodeGraph to JSON Converter
// Converts MaterialX node-based shading networks to JSON format
// for reconstruction in JavaScript/WebAssembly environments (Three.js, etc.)
//

#pragma once

#include <string>
#include <vector>
#include <map>

#include "nonstd/expected.hpp"
#include "value-types.hh"  // For value::float2

namespace lightusd {

// Forward declarations
class Prim;
class Stage;
class Path;

namespace mtlx {
  class MtlxNodeGraph;
  class MtlxNode;
  class MtlxInput;
  class MtlxOutput;
}

namespace tydra {

///
/// MaterialX Node Graph JSON Schema
///
/// Follows MaterialX XML structure as closely as possible for compatibility
/// Schema format (JSON):
/// {
///   "version": "1.39",           // MaterialX version (Blender 4.5+ compatible)
///   "nodegraph": {
///     "name": "NG_shader1",      // NodeGraph name
///     "nodes": [                 // Array of nodes
///       {
///         "name": "image1",
///         "category": "image",    // Node type (image, multiply, mix, etc.)
///         "type": "color3",       // Output type
///         "inputs": [
///           {
///             "name": "file",
///             "type": "filename",
///             "value": "texture.png",
///             "colorspace": "srgb_texture"  // Optional, omitted if lin_rec709_scene
///           }
///         ],
///         "outputs": [
///           {
///             "name": "out",
///             "type": "color3"
///           }
///         ]
///       }
///     ],
///     "outputs": [               // NodeGraph outputs
///       {
///         "name": "base_color_output",
///         "type": "color3",
///         "nodename": "image1",
///         "output": "out"
///       }
///     ]
///   },
///   "connections": [             // Shader input connections
///     {
///       "input": "base_color",   // Shader parameter name
///       "nodegraph": "NG_shader1",
///       "output": "base_color_output"
///     }
///   ]
/// }
///

///
/// Convert MaterialX NodeGraph Prim to JSON string
///
/// @param[in] nodegraph_prim NodeGraph Prim from USD/MaterialX
/// @param[out] json_str Output JSON string
/// @param[out] err Error message if conversion fails
/// @return true on success, false on failure
///
bool ConvertNodeGraphToJson(
    const Prim &nodegraph_prim,
    std::string *json_str,
    std::string *err = nullptr);

///
/// Convert MaterialX Shader Prim with NodeGraph connections to JSON
/// Includes both the nodegraph structure and shader connections
///
/// @param[in] shader_prim Shader Prim (e.g., MtlxOpenPBRSurface)
/// @param[in] shader_abs_path Absolute path to the shader prim
/// @param[in] stage USD Stage for resolving references
/// @param[out] json_str Output JSON string
/// @param[out] err Error message if conversion fails
/// @return true on success, false on failure
///
bool ConvertShaderWithNodeGraphToJson(
    const Prim &shader_prim,
    const Path &shader_abs_path,
    const Stage &stage,
    std::string *json_str,
    std::string *err = nullptr);

///
/// Convert MaterialX DOM NodeGraph to JSON string
/// For use with MaterialX DOM (mtlx-dom.hh) structures
///
/// @param[in] nodegraph MaterialX DOM NodeGraph object
/// @param[out] json_str Output JSON string
/// @param[out] err Error message if conversion fails
/// @return true on success, false on failure
///
bool ConvertMtlxNodeGraphToJson(
    const mtlx::MtlxNodeGraph &nodegraph,
    std::string *json_str,
    std::string *err = nullptr);

///
/// Helper: Escape JSON string (handles quotes, newlines, etc.)
///
std::string EscapeJsonString(const std::string &input);

///
/// Helper: Convert float vector to JSON array string
/// e.g., [0.5, 0.8, 1.0]
///
std::string FloatVectorToJsonArray(const std::vector<float> &vec);

///
/// Helper: Convert int vector to JSON array string
/// e.g., [1, 2, 3]
///
std::string IntVectorToJsonArray(const std::vector<int> &vec);

///
/// Helper: Convert string vector to JSON array string
/// e.g., ["a", "b", "c"]
///
std::string StringVectorToJsonArray(const std::vector<std::string> &vec);

// ============================================================================
// LTE SpectralAPI JSON Conversion
// Converts spectral data to JSON with spd_ prefix for MaterialX
// ============================================================================

// Forward declarations
struct SpectralData;
struct SpectralIOR;
struct SpectralEmission;
enum class SpectralInterpolation;
enum class IlluminantPreset;
enum class WavelengthUnit;

///
/// Convert SpectralData to JSON object string
/// Output format:
/// {
///   "samples": [[wavelength, value], ...],
///   "interpolation": "linear",
///   "unit": "nanometers"
/// }
///
std::string SpectralDataToJson(const SpectralData &data);

///
/// Convert SpectralIOR to JSON object string
/// Output format:
/// {
///   "samples": [[wavelength, ior], ...],
///   "interpolation": "linear" | "sellmeier",
///   "unit": "nanometers",
///   "sellmeier": {"B": [B1, B2, B3], "C": [C1, C2, C3]}  // optional
/// }
///
std::string SpectralIORToJson(const SpectralIOR &data);

///
/// Convert SpectralEmission to JSON object string
/// Output format:
/// {
///   "samples": [[wavelength, irradiance], ...],
///   "interpolation": "linear",
///   "unit": "nanometers",
///   "preset": "d65"  // optional, only if preset != None
/// }
///
std::string SpectralEmissionToJson(const SpectralEmission &data);

///
/// Convert vec2 array (wavelength, value pairs) to JSON array
/// e.g., [[450.0, 0.2], [550.0, 0.4], [650.0, 0.9]]
///
std::string Vec2ArrayToJsonArray(const std::vector<value::float2> &vec);

} // namespace tydra
} // namespace lightusd
