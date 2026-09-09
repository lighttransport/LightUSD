#include "js-script.hh"
#include "core/prim.hh"
#include "layer.hh"
#include "lightusd.hh"
#include "value-to-json.hh"

#if defined(LIGHTUSD_WITH_QJS)
// external

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "external/quickjs-ng/quickjs.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include "value-types.hh"
#include "str-util.hh"  // For dragonbox-based dtos()
#endif

namespace lightusd {
namespace tydra {



#if defined(LIGHTUSD_WITH_QJS)

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

static std::string LayerMetasToJSON(const LayerMetas* metas) {
  if (!metas) {
    return "null";
  }

  std::ostringstream oss;
  oss << std::setprecision(17);
  oss << "{";

  // upAxis
  oss << "\"upAxis\":\"";
  switch (metas->upAxis.get_value()) {
    case Axis::X: oss << "X"; break;
    case Axis::Y: oss << "Y"; break;
    case Axis::Z: oss << "Z"; break;
    case Axis::Invalid: oss << "Invalid"; break;
  }
  oss << "\",";

  // defaultPrim
  oss << "\"defaultPrim\":\"" << metas->defaultPrim.str() << "\",";

  // numeric values
  oss << "\"metersPerUnit\":" << metas->metersPerUnit.get_value() << ",";
  oss << "\"timeCodesPerSecond\":" << metas->timeCodesPerSecond.get_value() << ",";
  oss << "\"framesPerSecond\":" << metas->framesPerSecond.get_value() << ",";
  oss << "\"startTimeCode\":" << metas->startTimeCode.get_value() << ",";

  // Handle potentially infinite endTimeCode
  double endTime = metas->endTimeCode.get_value();
  if (std::isinf(endTime)) {
    oss << "\"endTimeCode\":null,";
  } else {
    oss << "\"endTimeCode\":" << endTime << ",";
  }

  oss << "\"kilogramsPerUnit\":" << metas->kilogramsPerUnit.get_value() << ",";

  // comment and doc - escape JSON special characters
  auto escapeJson = [](const std::string& str) -> std::string {
    std::string escaped;
    for (char c : str) {
      switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
      }
    }
    return escaped;
  };

  oss << "\"comment\":\"" << escapeJson(metas->comment.value) << "\",";
  oss << "\"doc\":\"" << escapeJson(metas->doc.value) << "\",";

  // USDZ extension
  oss << "\"autoPlay\":" << (metas->autoPlay.get_value() ? "true" : "false") << ",";
  oss << "\"playbackMode\":\"";
  switch (metas->playbackMode.get_value()) {
    case LayerMetas::PlaybackMode::PlaybackModeNone: oss << "PlaybackModeNone"; break;
    case LayerMetas::PlaybackMode::PlaybackModeLoop: oss << "PlaybackModeLoop"; break;
    //default: oss << "Unknown"; break;
  }
  oss << "\",";

  // subLayers array
  oss << "\"subLayers\":[";
  for (size_t i = 0; i < metas->subLayers.size(); i++) {
    if (i > 0) oss << ",";
    oss << "{";
    oss << "\"assetPath\":\"" << metas->subLayers[i].assetPath.GetAssetPath() << "\",";
    oss << "\"layerOffset\":{";
    oss << "\"offset\":" << metas->subLayers[i].layerOffset._offset << ",";
    oss << "\"scale\":" << metas->subLayers[i].layerOffset._scale;
    oss << "}";
    oss << "}";
  }
  oss << "],";

  // primChildren array
  oss << "\"primChildren\":[";
  for (size_t i = 0; i < metas->primChildren.size(); i++) {
    if (i > 0) oss << ",";
    oss << "\"" << metas->primChildren[i].str() << "\"";
  }
  oss << "]";

  oss << "}";
  return oss.str();
}

static const LayerMetas* g_current_layer_metas = nullptr;
static const Attribute* g_current_attribute = nullptr;
static const class Layer* g_current_layer = nullptr;

// JavaScript fp16 library and LightUSDFloat16Array implementation
static const char* fp16_library_js = R"(
// Simple IEEE 754 half-precision (fp16) conversion utilities
(function() {
  'use strict';

  // Convert float32 to half-precision uint16
  function float32ToHalf(f32) {
    var floatView = new Float32Array(1);
    var int32View = new Int32Array(floatView.buffer);
    floatView[0] = f32;
    var f = int32View[0];

    var sign = (f >> 31) & 0x1;
    var exp = (f >> 23) & 0xFF;
    var frac = f & 0x7FFFFF;

    var newExp, newFrac;
    if (exp === 0) {
      // Zero or denormal
      newExp = 0;
      newFrac = 0;
    } else if (exp === 0xFF) {
      // Infinity or NaN
      newExp = 0x1F;
      newFrac = frac ? 0x3FF : 0;
    } else {
      // Normal number
      newExp = exp - 127 + 15; // Convert from float32 bias to half bias
      if (newExp >= 0x1F) {
        // Overflow to infinity
        newExp = 0x1F;
        newFrac = 0;
      } else if (newExp <= 0) {
        // Underflow to zero
        newExp = 0;
        newFrac = 0;
      } else {
        newFrac = frac >> 13; // Keep top 10 bits of fraction
      }
    }

    return (sign << 15) | (newExp << 10) | newFrac;
  }

  // Convert half-precision uint16 to float32
  function halfToFloat32(h) {
    var sign = (h >> 15) & 0x1;
    var exp = (h >> 10) & 0x1F;
    var frac = h & 0x3FF;

    var newExp, newFrac;
    if (exp === 0) {
      if (frac === 0) {
        // Zero
        newExp = 0;
        newFrac = 0;
      } else {
        // Denormal - convert to normal
        newExp = 127 - 15 + 1; // float32 bias - half bias + 1
        newFrac = frac << 13;
        // Normalize
        while ((newFrac & 0x800000) === 0) {
          newFrac <<= 1;
          newExp--;
        }
        newFrac &= 0x7FFFFF;
      }
    } else if (exp === 0x1F) {
      // Infinity or NaN
      newExp = 0xFF;
      newFrac = frac << 13;
    } else {
      // Normal
      newExp = exp - 15 + 127; // Convert from half bias to float32 bias
      newFrac = frac << 13;
    }

    var floatView = new Float32Array(1);
    var int32View = new Int32Array(floatView.buffer);
    int32View[0] = (sign << 31) | (newExp << 23) | newFrac;
    return floatView[0];
  }

  // LightUSDFloat16Array class - similar interface to typed arrays
  function LightUSDFloat16Array(arg) {
    if (typeof arg === 'number') {
      // Create array of given length
      this.length = arg;
      this._buffer = new Uint16Array(arg);
    } else if (arg instanceof Array || arg instanceof Uint16Array) {
      // Create from array
      this.length = arg.length;
      this._buffer = new Uint16Array(arg.length);
      for (var i = 0; i < arg.length; i++) {
        if (arg instanceof Uint16Array) {
          this._buffer[i] = arg[i];
        } else {
          this._buffer[i] = float32ToHalf(arg[i]);
        }
      }
    } else {
      throw new Error('Invalid argument to LightUSDFloat16Array constructor');
    }
  }

  LightUSDFloat16Array.prototype.get = function(index) {
    if (index < 0 || index >= this.length) {
      return undefined;
    }
    return halfToFloat32(this._buffer[index]);
  };

  LightUSDFloat16Array.prototype.set = function(index, value) {
    if (index >= 0 && index < this.length) {
      this._buffer[index] = float32ToHalf(value);
    }
  };

  LightUSDFloat16Array.prototype.getUint16 = function(index) {
    if (index < 0 || index >= this.length) {
      return undefined;
    }
    return this._buffer[index];
  };

  LightUSDFloat16Array.prototype.setUint16 = function(index, value) {
    if (index >= 0 && index < this.length) {
      this._buffer[index] = value & 0xFFFF;
    }
  };

  LightUSDFloat16Array.prototype.toArray = function() {
    var result = new Array(this.length);
    for (var i = 0; i < this.length; i++) {
      result[i] = this.get(i);
    }
    return result;
  };

  LightUSDFloat16Array.prototype.toUint16Array = function() {
    return new Uint16Array(this._buffer);
  };

  // Static methods
  LightUSDFloat16Array.fromFloat32Array = function(arr) {
    return new LightUSDFloat16Array(arr);
  };

  LightUSDFloat16Array.fromUint16Array = function(arr) {
    var result = new LightUSDFloat16Array(arr.length);
    result._buffer = new Uint16Array(arr);
    return result;
  };

  // Export to global scope
  globalThis.LightUSDFloat16Array = LightUSDFloat16Array;
  globalThis.float32ToHalf = float32ToHalf;
  globalThis.halfToFloat32 = halfToFloat32;
})();
)";

static std::string AttributeToJSON(const Attribute* attr) {
  if (!attr) {
    return "null";
  }

  std::ostringstream oss;
  oss << std::setprecision(17);
  oss << "{";

  // Basic attribute info
  oss << "\"name\":\"" << attr->name() << "\",";
  oss << "\"type_name\":\"" << attr->type_name() << "\",";
  oss << "\"type_id\":" << attr->type_id() << ",";
  oss << "\"is_blocked\":" << (attr->is_blocked() ? "true" : "false") << ",";
  oss << "\"has_value\":" << (attr->has_value() ? "true" : "false") << ",";
  oss << "\"is_connection\":" << (attr->is_connection() ? "true" : "false") << ",";
  oss << "\"is_timesamples\":" << (attr->has_timesamples() ? "true" : "false") << ",";

  // Add variability info
  std::string variability_str = "Unknown";
  switch (attr->variability()) {
    case Variability::Varying: variability_str = "Varying"; break;
    case Variability::Uniform: variability_str = "Uniform"; break;
    case Variability::Config: variability_str = "Config"; break;
    case Variability::Invalid: variability_str = "Invalid"; break;
  }
  oss << "\"variability\":\"" << variability_str << "\",";

  // Handle value based on type
  oss << "\"value\":";

  if (attr->is_blocked() || !attr->has_value()) {
    oss << "null";
  } else {
    uint32_t tid = attr->type_id();
    bool handled = false;

    // Handle scalar types
    if (tid == value::TypeTraits<float>::type_id()) {
      auto v = attr->get_value<float>();
      oss << (v ? std::to_string(v.value()) : "null");
      handled = true;
    } else if (tid == value::TypeTraits<double>::type_id()) {
      auto v = attr->get_value<double>();
      oss << (v ? std::to_string(v.value()) : "null");
      handled = true;
    } else if (tid == value::TypeTraits<int>::type_id()) {
      auto v = attr->get_value<int>();
      oss << (v ? std::to_string(v.value()) : "null");
      handled = true;
    } else if (tid == value::TypeTraits<bool>::type_id()) {
      auto v = attr->get_value<bool>();
      oss << (v ? (v.value() ? "true" : "false") : "null");
      handled = true;
    }
    // Handle int compound types
    else if (tid == value::TypeTraits<value::int2>::type_id()) {
      auto v = attr->get_value<value::int2>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::int3>::type_id()) {
      auto v = attr->get_value<value::int3>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::int4>::type_id()) {
      auto v = attr->get_value<value::int4>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "," << v.value()[3] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle compound types with flattened arrays
    else if (tid == value::TypeTraits<value::float3>::type_id()) {
      auto v = attr->get_value<value::float3>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::float2>::type_id()) {
      auto v = attr->get_value<value::float2>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::float4>::type_id()) {
      auto v = attr->get_value<value::float4>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "," << v.value()[3] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::double3>::type_id()) {
      auto v = attr->get_value<value::double3>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::double2>::type_id()) {
      auto v = attr->get_value<value::double2>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::double4>::type_id()) {
      auto v = attr->get_value<value::double4>();
      if (v) {
        oss << "[" << v.value()[0] << "," << v.value()[1] << "," << v.value()[2] << "," << v.value()[3] << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle matrix types (flattened)
    else if (tid == value::TypeTraits<value::matrix4f>::type_id()) {
      auto v = attr->get_value<value::matrix4f>();
      if (v) {
        oss << "[";
        for (int i = 0; i < 4; i++) {
          for (int j = 0; j < 4; j++) {
            if (i > 0 || j > 0) oss << ",";
            oss << v.value().m[i][j];
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::matrix3f>::type_id()) {
      auto v = attr->get_value<value::matrix3f>();
      if (v) {
        oss << "[";
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            if (i > 0 || j > 0) oss << ",";
            oss << v.value().m[i][j];
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::matrix3d>::type_id()) {
      auto v = attr->get_value<value::matrix3d>();
      if (v) {
        oss << "[";
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            if (i > 0 || j > 0) oss << ",";
            oss << dtos(v.value().m[i][j]);
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<value::matrix4d>::type_id()) {
      auto v = attr->get_value<value::matrix4d>();
      if (v) {
        oss << "[";
        for (int i = 0; i < 4; i++) {
          for (int j = 0; j < 4; j++) {
            if (i > 0 || j > 0) oss << ",";
            oss << dtos(v.value().m[i][j]);
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle individual half compound types
    else if (tid == value::TypeTraits<value::half2>::type_id()) {
      auto v = attr->get_value<value::half2>();
      if (v) {
        oss << "[" << dtos(value::half_to_float(v.value()[0])) << "," << dtos(value::half_to_float(v.value()[1])) << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    else if (tid == value::TypeTraits<value::half3>::type_id()) {
      auto v = attr->get_value<value::half3>();
      if (v) {
        oss << "[" << dtos(value::half_to_float(v.value()[0])) << "," << dtos(value::half_to_float(v.value()[1])) << "," << dtos(value::half_to_float(v.value()[2])) << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    else if (tid == value::TypeTraits<value::half4>::type_id()) {
      auto v = attr->get_value<value::half4>();
      if (v) {
        oss << "[" << dtos(value::half_to_float(v.value()[0])) << "," << dtos(value::half_to_float(v.value()[1])) << ","
            << dtos(value::half_to_float(v.value()[2])) << "," << dtos(value::half_to_float(v.value()[3])) << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle string/token types
    else if (tid == value::TypeTraits<value::token>::type_id()) {
      auto v = attr->get_value<value::token>();
      oss << "\"" << (v ? v.value().str() : "") << "\"";
      handled = true;
    } else if (tid == value::TypeTraits<std::string>::type_id()) {
      auto v = attr->get_value<std::string>();
      oss << "\"" << (v ? v.value() : "") << "\"";
      handled = true;
    }
    // Handle array types
    else if (tid == value::TypeTraits<std::vector<float>>::type_id()) {
      auto v = attr->get_value<std::vector<float>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          oss << dtos(v.value()[i]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<int>>::type_id()) {
      auto v = attr->get_value<std::vector<int>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          oss << v.value()[i];
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::float3>>::type_id()) {
      auto v = attr->get_value<std::vector<value::float3>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each float3 into the main array
          oss << dtos(v.value()[i][0]) << "," << dtos(v.value()[i][1]) << "," << dtos(v.value()[i][2]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle double array types
    else if (tid == value::TypeTraits<std::vector<double>>::type_id()) {
      auto v = attr->get_value<std::vector<double>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          oss << dtos(v.value()[i]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::double2>>::type_id()) {
      auto v = attr->get_value<std::vector<value::double2>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each double2 into the main array
          oss << dtos(v.value()[i][0]) << "," << dtos(v.value()[i][1]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::double3>>::type_id()) {
      auto v = attr->get_value<std::vector<value::double3>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each double3 into the main array
          oss << dtos(v.value()[i][0]) << "," << dtos(v.value()[i][1]) << "," << dtos(v.value()[i][2]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::double4>>::type_id()) {
      auto v = attr->get_value<std::vector<value::double4>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each double4 into the main array
          oss << dtos(v.value()[i][0]) << "," << dtos(v.value()[i][1]) << "," << dtos(v.value()[i][2]) << "," << dtos(v.value()[i][3]);
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle int array types
    else if (tid == value::TypeTraits<std::vector<value::int2>>::type_id()) {
      auto v = attr->get_value<std::vector<value::int2>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each int2 into the main array
          oss << v.value()[i][0] << "," << v.value()[i][1];
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::int3>>::type_id()) {
      auto v = attr->get_value<std::vector<value::int3>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each int3 into the main array
          oss << v.value()[i][0] << "," << v.value()[i][1] << "," << v.value()[i][2];
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::int4>>::type_id()) {
      auto v = attr->get_value<std::vector<value::int4>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each int4 into the main array
          oss << v.value()[i][0] << "," << v.value()[i][1] << "," << v.value()[i][2] << "," << v.value()[i][3];
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Handle matrix array types
    else if (tid == value::TypeTraits<std::vector<value::matrix3d>>::type_id()) {
      auto v = attr->get_value<std::vector<value::matrix3d>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each matrix3d into the main array (9 elements per matrix)
          for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
              if (row > 0 || col > 0) oss << ",";
              oss << dtos(v.value()[i].m[row][col]);
            }
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    } else if (tid == value::TypeTraits<std::vector<value::matrix4d>>::type_id()) {
      auto v = attr->get_value<std::vector<value::matrix4d>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); i++) {
          if (i > 0) oss << ",";
          // Flatten each matrix4d into the main array (16 elements per matrix)
          for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
              if (row > 0 || col > 0) oss << ",";
              oss << dtos(v.value()[i].m[row][col]);
            }
          }
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    // Add half array types with flattened output
    else if (tid == value::TypeTraits<std::vector<value::half>>::type_id()) {
      auto v = attr->get_value<std::vector<value::half>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); ++i) {
          if (i > 0) oss << ",";
          oss << dtos(value::half_to_float(v.value()[i]));
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    else if (tid == value::TypeTraits<std::vector<value::half2>>::type_id()) {
      auto v = attr->get_value<std::vector<value::half2>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); ++i) {
          if (i > 0) oss << ",";
          oss << dtos(value::half_to_float(v.value()[i][0])) << "," << dtos(value::half_to_float(v.value()[i][1]));
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    else if (tid == value::TypeTraits<std::vector<value::half3>>::type_id()) {
      auto v = attr->get_value<std::vector<value::half3>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); ++i) {
          if (i > 0) oss << ",";
          oss << dtos(value::half_to_float(v.value()[i][0])) << "," << dtos(value::half_to_float(v.value()[i][1])) << ","
              << dtos(value::half_to_float(v.value()[i][2]));
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }
    else if (tid == value::TypeTraits<std::vector<value::half4>>::type_id()) {
      auto v = attr->get_value<std::vector<value::half4>>();
      if (v) {
        oss << "[";
        for (size_t i = 0; i < v.value().size(); ++i) {
          if (i > 0) oss << ",";
          oss << dtos(value::half_to_float(v.value()[i][0])) << "," << dtos(value::half_to_float(v.value()[i][1])) << ","
              << dtos(value::half_to_float(v.value()[i][2])) << "," << dtos(value::half_to_float(v.value()[i][3]));
        }
        oss << "]";
      } else {
        oss << "null";
      }
      handled = true;
    }

    if (!handled) {
      oss << "\"unsupported_type_" << attr->type_name() << "\"";
    }
  }

  // Add typed array info for JavaScript typed arrays
  oss << ",\"jsTypedArray\":";
  if (!attr->has_value() || attr->is_blocked()) {
    oss << "null";
  } else {
    uint32_t tid = attr->type_id();
    if (tid == value::TypeTraits<value::float3>::type_id() ||
        tid == value::TypeTraits<value::float2>::type_id() ||
        tid == value::TypeTraits<value::float4>::type_id() ||
        tid == value::TypeTraits<value::matrix4f>::type_id() ||
        tid == value::TypeTraits<value::matrix3f>::type_id() ||
        tid == value::TypeTraits<std::vector<float>>::type_id() ||
        tid == value::TypeTraits<std::vector<value::float3>>::type_id()) {
      oss << "\"Float32Array\"";
    } else if (tid == value::TypeTraits<value::double3>::type_id() ||
               tid == value::TypeTraits<value::double2>::type_id() ||
               tid == value::TypeTraits<value::double4>::type_id() ||
               tid == value::TypeTraits<value::matrix3d>::type_id() ||
               tid == value::TypeTraits<value::matrix4d>::type_id() ||
               tid == value::TypeTraits<std::vector<double>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::double2>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::double3>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::double4>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::matrix3d>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::matrix4d>>::type_id()) {
      oss << "\"Float64Array\"";
    } else if (tid == value::TypeTraits<std::vector<int>>::type_id() ||
               tid == value::TypeTraits<value::int2>::type_id() ||
               tid == value::TypeTraits<value::int3>::type_id() ||
               tid == value::TypeTraits<value::int4>::type_id() ||
               tid == value::TypeTraits<std::vector<value::int2>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::int3>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::int4>>::type_id()) {
      oss << "\"Int32Array\"";
    } else if (tid == value::TypeTraits<value::half2>::type_id() ||
               tid == value::TypeTraits<value::half3>::type_id() ||
               tid == value::TypeTraits<value::half4>::type_id() ||
               tid == value::TypeTraits<std::vector<value::half>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::half2>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::half3>>::type_id() ||
               tid == value::TypeTraits<std::vector<value::half4>>::type_id()) {
      oss << "\"LightUSDFloat16Array\"";
    } else {
      oss << "null";
    }
  }

  oss << "}";
  return oss.str();
}

static std::string PrimMetasToJSON(const PrimMeta* metas) {
  if (!metas) {
    return "null";
  }

  std::ostringstream oss;
  oss << std::setprecision(17);
  oss << "{";

  // Basic metadata flags
  oss << "\"active\":";
  if (metas->has_active()) {
    oss << (metas->get_active() ? "true" : "false");
  } else {
    oss << "null";
  }
  oss << ",";

  oss << "\"hidden\":";
  if (metas->has_hidden()) {
    oss << (metas->get_hidden() ? "true" : "false");
  } else {
    oss << "null";
  }
  oss << ",";

  oss << "\"instanceable\":";
  if (metas->has_instanceable()) {
    oss << (metas->get_instanceable() ? "true" : "false");
  } else {
    oss << "null";
  }
  oss << ",";

  // Kind
  oss << "\"kind\":\"" << metas->get_kind() << "\",";

  // Documentation and comment
  oss << "\"documentation\":";
  if (metas->has_doc()) {
    oss << "\"" << metas->get_doc().value << "\"";
  } else {
    oss << "null";
  }
  oss << ",";

  oss << "\"comment\":";
  if (metas->has_comment()) {
    oss << "\"" << metas->get_comment().value << "\"";
  } else {
    oss << "null";
  }
  oss << ",";

  // Display name and scene name (extensions)
  oss << "\"displayName\":";
  if (metas->has_displayName()) {
    oss << "\"" << metas->get_displayName() << "\"";
  } else {
    oss << "null";
  }
  oss << ",";

  oss << "\"sceneName\":";
  if (metas->has_sceneName()) {
    oss << "\"" << metas->get_sceneName() << "\"";
  } else {
    oss << "null";
  }
  oss << ",";

  // References count
  oss << "\"hasReferences\":";
  if (metas->references.has_value() && !metas->references.value().empty()) {
    size_t total_refs = 0;
    for (const auto& p : metas->references.value()) {
      total_refs += p.second.size();
    }
    oss << (total_refs > 0 ? "true" : "false") << ",";
    oss << "\"referencesCount\":" << total_refs;
  } else {
    oss << "false,";
    oss << "\"referencesCount\":0";
  }
  oss << ",";

  // Payload count
  oss << "\"hasPayload\":";
  if (metas->payload.has_value() && !metas->payload.value().empty()) {
    size_t total_payloads = 0;
    for (const auto& p : metas->payload.value()) {
      total_payloads += p.second.size();
    }
    oss << (total_payloads > 0 ? "true" : "false") << ",";
    oss << "\"payloadCount\":" << total_payloads;
  } else {
    oss << "false,";
    oss << "\"payloadCount\":0";
  }
  oss << ",";

  // Inherits count
  oss << "\"hasInherits\":";
  if (metas->inherits.has_value() && !metas->inherits.value().empty()) {
    size_t total_inherits = 0;
    for (const auto& p : metas->inherits.value()) {
      total_inherits += p.second.size();
    }
    oss << (total_inherits > 0 ? "true" : "false") << ",";
    oss << "\"inheritsCount\":" << total_inherits;
  } else {
    oss << "false,";
    oss << "\"inheritsCount\":0";
  }
  oss << ",";

  // Variants info
  oss << "\"hasVariants\":";
  if (metas->variants.has_value() && !metas->variants.value().empty()) {
    oss << "true,";
    oss << "\"variantsCount\":" << metas->variants.value().size() << ",";
    oss << "\"variantNames\":[";
    bool first = true;
    for (const auto& variant : metas->variants.value()) {
      if (!first) oss << ",";
      oss << "\"" << variant.first << "\"";
      first = false;
    }
    oss << "]";
  } else {
    oss << "false,";
    oss << "\"variantsCount\":0,";
    oss << "\"variantNames\":[]";
  }
  oss << ",";

  // VariantSets info
  oss << "\"hasVariantSets\":";
  if (metas->variantSets.has_value() && !metas->variantSets.value().empty()) {
    std::vector<std::string> all_varset_names;
    for (const auto& p : metas->variantSets.value()) {
      for (const auto& varSet : p.second) {
        all_varset_names.push_back(varSet);
      }
    }
    oss << (!all_varset_names.empty() ? "true" : "false") << ",";
    oss << "\"variantSetsCount\":" << all_varset_names.size() << ",";
    oss << "\"variantSetNames\":[";
    bool first = true;
    for (const auto& varSet : all_varset_names) {
      if (!first) oss << ",";
      oss << "\"" << varSet << "\"";
      first = false;
    }
    oss << "]";
  } else {
    oss << "false,";
    oss << "\"variantSetsCount\":0,";
    oss << "\"variantSetNames\":[]";
  }
  oss << ",";

  // Custom data and unregistered metas
  oss << "\"hasCustomData\":" << (metas->has_customData() ? "true" : "false") << ",";
  oss << "\"hasAssetInfo\":" << (metas->has_assetInfo() ? "true" : "false") << ",";
  oss << "\"unregisteredMetasCount\":" << metas->unregisteredMetas.size() << ",";

  // Unregistered metadata names
  oss << "\"unregisteredMetaNames\":[";
  bool first = true;
  for (const auto& meta : metas->unregisteredMetas) {
    if (!first) oss << ",";
    oss << "\"" << meta.first << "\"";
    first = false;
  }
  oss << "],";

  // Authored flag
  oss << "\"authored\":" << (metas->authored() ? "true" : "false");

  oss << "}";
  return oss.str();
}

static std::string PrimSpecToJSON(const PrimSpec* ps, uint32_t max_depth = 1) {
  if (!ps) {
    return "null";
  }

  return lightusd::tydra::PrimSpecToMiniJSON(*ps, max_depth).dump();
}

static JSValue js_getLayerMetas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *func_data) {
  std::string json = LayerMetasToJSON(g_current_layer_metas);

  JSValue result = JS_ParseJSON(ctx, json.c_str(), json.length(), "<layermetas>");
  if (JS_IsException(result)) {
    return JS_EXCEPTION;
  }
  return result;
}

static JSValue js_getAttribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *func_data) {
  std::string json = AttributeToJSON(g_current_attribute);

  JSValue result = JS_ParseJSON(ctx, json.c_str(), json.length(), "<attribute>");
  if (JS_IsException(result)) {
    return JS_EXCEPTION;
  }
  return result;
}

static JSValue js_findPrimSpecByPath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *func_data) {
  if (!g_current_layer) {
    return JS_NULL;
  }

  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "findPrimSpecByPath requires 1 argument (path string)");
  }

  const char *path_str = JS_ToCString(ctx, argv[0]);
  if (!path_str) {
    return JS_EXCEPTION;
  }

  // Parse the path string into a Path object
  std::string prim_path = std::string(path_str);
  lightusd::Path path(prim_path, "");
  if (!path.is_valid()) {
    JS_FreeCString(ctx, path_str);
    return JS_NULL;
  }

  // Find the PrimSpec
  const PrimSpec *ps = nullptr;
  std::string err;
  bool found = g_current_layer->find_primspec_at(path, &ps, &err);

  JS_FreeCString(ctx, path_str);

  if (!found || !ps) {
    return JS_NULL;
  }

  uint32_t max_depth = 1;
  if (argc >= 2 && JS_IsNumber(argv[1])) {
    int32_t depth = 1;
    if (JS_ToInt32(ctx, &depth, argv[1]) >= 0 && depth >= 0) {
      max_depth = static_cast<uint32_t>(depth);
    }
  }

  // Convert PrimSpec to JSON and parse it
  std::string json = PrimSpecToJSON(ps, max_depth);
  JSValue result = JS_ParseJSON(ctx, json.c_str(), json.length(), "<primspec>");
  if (JS_IsException(result)) {
    return JS_EXCEPTION;
  }
  return result;
}

static JSValue js_getPrimSpecMetadata(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *func_data) {
  if (!g_current_layer) {
    return JS_NULL;
  }

  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getPrimSpecMetadata requires 1 argument (path string)");
  }

  const char *path_str = JS_ToCString(ctx, argv[0]);
  if (!path_str) {
    return JS_EXCEPTION;
  }

  // Parse the path string into a Path object
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    JS_FreeCString(ctx, path_str);
    return JS_NULL;
  }

  // Find the PrimSpec
  const PrimSpec *ps = nullptr;
  std::string err;
  bool found = g_current_layer->find_primspec_at(path, &ps, &err);

  JS_FreeCString(ctx, path_str);

  if (!found || !ps) {
    return JS_NULL;
  }

  // Convert PrimSpec metadata to JSON and parse it
  std::string json = PrimMetasToJSON(&ps->metas());
  JSValue result = JS_ParseJSON(ctx, json.c_str(), json.length(), "<primspec_metadata>");
  if (JS_IsException(result)) {
    return JS_EXCEPTION;
  }
  return result;
}

bool RunJSScript(const std::string &js_code, std::string &err) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    err = "Failed to create JavaScript runtime";
    return false;
  }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    err = "Failed to create JavaScript context";
    JS_FreeRuntime(rt);
    return false;
  }

  JSValue result = JS_Eval(ctx, js_code.c_str(), js_code.length(),
                          "<eval>", JS_EVAL_TYPE_GLOBAL);

  bool success = true;
  if (JS_IsException(result)) {
    success = false;

    JSValue exception = JS_GetException(ctx);
    const char *error_str = JS_ToCString(ctx, exception);
    if (error_str) {
      err = std::string("JavaScript error: ") + error_str;
      JS_FreeCString(ctx, error_str);
    } else {
      err = "JavaScript error: unable to get error message";
    }
    JS_FreeValue(ctx, exception);
  }

  JS_FreeValue(ctx, result);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  return success;
}

bool RunJSScriptWithLayerMetas(const std::string &js_code, const LayerMetas* layer_metas, std::string &err) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    err = "Failed to create JavaScript runtime";
    return false;
  }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    err = "Failed to create JavaScript context";
    JS_FreeRuntime(rt);
    return false;
  }

  // Set the global LayerMetas pointer and add the function to the global context
  g_current_layer_metas = layer_metas;
  JSValue global_obj = JS_GetGlobalObject(ctx);
  JSValue func = JS_NewCFunctionData(ctx, js_getLayerMetas, 0, 0, 0, nullptr);
  JS_SetPropertyStr(ctx, global_obj, "getLayerMetas", func);
  JS_FreeValue(ctx, global_obj);

  JSValue result = JS_Eval(ctx, js_code.c_str(), js_code.length(),
                          "<eval>", JS_EVAL_TYPE_GLOBAL);

  bool success = true;
  if (JS_IsException(result)) {
    success = false;

    JSValue exception = JS_GetException(ctx);
    const char *error_str = JS_ToCString(ctx, exception);
    if (error_str) {
      err = std::string("JavaScript error: ") + error_str;
      JS_FreeCString(ctx, error_str);
    } else {
      err = "JavaScript error: unable to get error message";
    }
    JS_FreeValue(ctx, exception);
  }

  JS_FreeValue(ctx, result);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  return success;
}

bool RunJSScriptWithAttribute(const std::string &js_code, const Attribute* attribute, std::string &err) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    err = "Failed to create JavaScript runtime";
    return false;
  }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    err = "Failed to create JavaScript context";
    JS_FreeRuntime(rt);
    return false;
  }

  // Load the fp16 library first
  JSValue fp16_result = JS_Eval(ctx, fp16_library_js, strlen(fp16_library_js), "<fp16_library>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(fp16_result)) {
    err = "Failed to load fp16 library";
    JS_FreeValue(ctx, fp16_result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return false;
  }
  JS_FreeValue(ctx, fp16_result);

  // Set the global Attribute pointer and add the function to the global context
  g_current_attribute = attribute;
  JSValue global_obj = JS_GetGlobalObject(ctx);
  JSValue func = JS_NewCFunctionData(ctx, js_getAttribute, 0, 0, 0, nullptr);
  JS_SetPropertyStr(ctx, global_obj, "getAttribute", func);
  JS_FreeValue(ctx, global_obj);

  JSValue result = JS_Eval(ctx, js_code.c_str(), js_code.length(),
                          "<eval>", JS_EVAL_TYPE_GLOBAL);

  bool success = true;
  if (JS_IsException(result)) {
    success = false;

    JSValue exception = JS_GetException(ctx);
    const char *error_str = JS_ToCString(ctx, exception);
    if (error_str) {
      err = std::string("JavaScript error: ") + error_str;
      JS_FreeCString(ctx, error_str);
    } else {
      err = "JavaScript error: unable to get error message";
    }
    JS_FreeValue(ctx, exception);
  }

  JS_FreeValue(ctx, result);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  return success;
}

bool RunJSScriptWithLayer(const std::string &js_code, const class Layer* layer, std::string &err) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    err = "Failed to create JavaScript runtime";
    return false;
  }

  JSContext *ctx = JS_NewContext(rt);
  if (!ctx) {
    err = "Failed to create JavaScript context";
    JS_FreeRuntime(rt);
    return false;
  }

  // Set the global Layer pointer and add functions to the global context
  g_current_layer = layer;
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JSValue findFunc = JS_NewCFunctionData(ctx, js_findPrimSpecByPath, 1, 0, 0, nullptr);
  JS_SetPropertyStr(ctx, global_obj, "findPrimSpecByPath", findFunc);

  JSValue metaFunc = JS_NewCFunctionData(ctx, js_getPrimSpecMetadata, 1, 0, 0, nullptr);
  JS_SetPropertyStr(ctx, global_obj, "getPrimSpecMetadata", metaFunc);

  JS_FreeValue(ctx, global_obj);

  JSValue result = JS_Eval(ctx, js_code.c_str(), js_code.length(),
                          "<eval>", JS_EVAL_TYPE_GLOBAL);

  bool success = true;
  if (JS_IsException(result)) {
    success = false;

    JSValue exception = JS_GetException(ctx);
    const char *error_str = JS_ToCString(ctx, exception);
    if (error_str) {
      err = std::string("JavaScript error: ") + error_str;
      JS_FreeCString(ctx, error_str);
    } else {
      err = "JavaScript error: unable to get error message";
    }
    JS_FreeValue(ctx, exception);
  }

  JS_FreeValue(ctx, result);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  return success;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#else

bool RunJSScript(const std::string &js_code, std::string &err) {
  err = "JavaScript is not supported in this build.\n";
  (void)js_code;
  return false;
}

bool RunJSScriptWithLayerMetas(const std::string &js_code, const LayerMetas* layer_metas, std::string &err) {
  err = "JavaScript is not supported in this build.\n";
  (void)js_code;
  (void)layer_metas;
  return false;
}

bool RunJSScriptWithAttribute(const std::string &js_code, const Attribute* attribute, std::string &err) {
  err = "JavaScript is not supported in this build.\n";
  (void)js_code;
  (void)attribute;
  return false;
}

bool RunJSScriptWithLayer(const std::string &js_code, const class Layer* layer, std::string &err) {
  err = "JavaScript is not supported in this build.\n";
  (void)js_code;
  (void)layer;
  return false;
}
#endif

} // namespace tydra
} // namespace lightusd
