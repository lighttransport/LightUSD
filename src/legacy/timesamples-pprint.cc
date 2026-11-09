// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.

#include "timesamples-pprint.hh"

#include <sstream>
#include <cstring>
#include <map>
#include "str-util.hh"

#ifdef LIGHTUSD_ENABLE_THREAD
#include <thread>
#include <vector>
#endif

#include "value-types.hh"
#include "value-pprint.hh"
#include "pprint-meta.hh"
#include "timesamples.hh"
#include "stream-writer.hh"
#include "typed-array-core.hh"

namespace lightusd {

///
/// Configuration for threaded printing
///
struct ThreadedPrintConfig {
  /// Minimum number of samples to use threading (default: 1024)
  size_t thread_threshold = 1024;

  /// Number of threads to use (0 = auto-detect from hardware)
  unsigned int num_threads = 0;

  /// Get the actual number of threads to use
  unsigned int get_num_threads() const {
#ifdef LIGHTUSD_ENABLE_THREAD
    if (num_threads > 0) {
      return num_threads;
    }
    // Use hardware_concurrency() - let the hardware dictate thread count
    // More threads = less data per thread = better performance in our implementation
    unsigned int hw_threads = std::thread::hardware_concurrency();
    return (hw_threads > 0) ? hw_threads : 4;  // Fallback to 4 if can't detect
#else
    return 1;
#endif
  }
};

// Global configuration (can be customized)
static ThreadedPrintConfig g_threaded_print_config;

namespace {

// ============================================================================
// Type Traits and Unified Print System
// ============================================================================

// Output abstraction - allows same code to work with StreamWriter or string
class OutputAdapter {
public:
  virtual ~OutputAdapter() = default;
  virtual void write(const std::string& s) = 0;
  virtual void write(double d) = 0;
  virtual void write(float f) = 0;
  virtual void write(int i) = 0;

  template<typename T>
  OutputAdapter& operator<<(const T& value) {
    std::stringstream ss;
    ss << value;
    write(ss.str());
    return *this;
  }
};

class StringOutputAdapter : public OutputAdapter {
  std::stringstream ss_;
public:
  void write(const std::string& s) override { ss_ << s; }
  void write(double d) override { ss_ << d; }
  void write(float f) override { ss_ << f; }
  void write(int i) override { ss_ << i; }
  std::string str() const { return ss_.str(); }
};

class StreamWriterAdapter : public OutputAdapter {
  StreamWriter& writer_;
public:
  explicit StreamWriterAdapter(StreamWriter& w) : writer_(w) {}
  void write(const std::string& s) override { writer_.write(s); }
  void write(double d) override { writer_.write(d); }
  void write(float f) override { writer_.write(f); }
  void write(int i) override { writer_ << i; }
};

// Type traits for value types (those with operator<<)
template<typename T>
struct is_value_type : std::false_type {};

// Specialize for all value types that have operator<<
#define DECLARE_VALUE_TYPE(TYPE) \
  template<> struct is_value_type<TYPE> : std::true_type {}

DECLARE_VALUE_TYPE(value::half);
DECLARE_VALUE_TYPE(value::half2);
DECLARE_VALUE_TYPE(value::half3);
DECLARE_VALUE_TYPE(value::half4);
DECLARE_VALUE_TYPE(value::float2);
DECLARE_VALUE_TYPE(value::float3);
DECLARE_VALUE_TYPE(value::float4);
DECLARE_VALUE_TYPE(value::double2);
DECLARE_VALUE_TYPE(value::double3);
DECLARE_VALUE_TYPE(value::double4);
DECLARE_VALUE_TYPE(value::quath);
DECLARE_VALUE_TYPE(value::quatf);
DECLARE_VALUE_TYPE(value::quatd);
DECLARE_VALUE_TYPE(value::matrix2f);
DECLARE_VALUE_TYPE(value::matrix3f);
DECLARE_VALUE_TYPE(value::matrix4f);
DECLARE_VALUE_TYPE(value::matrix2d);
DECLARE_VALUE_TYPE(value::matrix3d);
DECLARE_VALUE_TYPE(value::matrix4d);
DECLARE_VALUE_TYPE(value::color3h);
DECLARE_VALUE_TYPE(value::color3f);
DECLARE_VALUE_TYPE(value::color3d);
DECLARE_VALUE_TYPE(value::color4h);
DECLARE_VALUE_TYPE(value::color4f);
DECLARE_VALUE_TYPE(value::color4d);
DECLARE_VALUE_TYPE(value::point3h);
DECLARE_VALUE_TYPE(value::point3f);
DECLARE_VALUE_TYPE(value::point3d);
DECLARE_VALUE_TYPE(value::normal3h);
DECLARE_VALUE_TYPE(value::normal3f);
DECLARE_VALUE_TYPE(value::normal3d);
DECLARE_VALUE_TYPE(value::vector3h);
DECLARE_VALUE_TYPE(value::vector3f);
DECLARE_VALUE_TYPE(value::vector3d);
DECLARE_VALUE_TYPE(value::texcoord2h);
DECLARE_VALUE_TYPE(value::texcoord2f);
DECLARE_VALUE_TYPE(value::texcoord2d);
DECLARE_VALUE_TYPE(value::texcoord3h);
DECLARE_VALUE_TYPE(value::texcoord3f);
DECLARE_VALUE_TYPE(value::texcoord3d);
DECLARE_VALUE_TYPE(value::frame4d);

#undef DECLARE_VALUE_TYPE

// Unified print function for value types (those with operator<<)
template<typename T>
typename std::enable_if<is_value_type<T>::value, void>::type
print_type(OutputAdapter& out, const uint8_t* data) {
  T value;
  std::memcpy(&value, data, sizeof(T));
  std::stringstream ss;
  ss << value;
  out.write(ss.str());
}

// Unified print function for simple binary-serializable types
template<typename T>
typename std::enable_if<std::is_arithmetic<T>::value && !is_value_type<T>::value, void>::type
print_type(OutputAdapter& out, const uint8_t* data) {
  T value;
  std::memcpy(&value, data, sizeof(T));
  out << value;
}

// Specialization for bool - normalize to 0 or 1 for safety
template<>
void print_type<bool>(OutputAdapter& out, const uint8_t* data) {
  uint8_t value;
  std::memcpy(&value, data, sizeof(uint8_t));
  // Normalize: any non-zero value becomes 1 (true)
  out.write(value != 0 ? 1 : 0);
}

// Specialization for uint8_t - print as integer, not char
// Note: USD stores bool arrays as uint8_t (TYPE_ID_UCHAR), so we print the actual value.
// Well-formed USD files should only have 0/1 for bool arrays anyway.
template<>
void print_type<uint8_t>(OutputAdapter& out, const uint8_t* data) {
  uint8_t value;
  std::memcpy(&value, data, sizeof(uint8_t));
  out.write(static_cast<int>(value));
}

// Specialization for char - print as integer, not char
template<>
void print_type<char>(OutputAdapter& out, const uint8_t* data) {
  char value;
  std::memcpy(&value, data, sizeof(char));
  out.write(static_cast<int>(value));
}

// Specialization for double - print with full precision using dtoa
template<>
void print_type<double>(OutputAdapter& out, const uint8_t* data) {
  double value;
  std::memcpy(&value, data, sizeof(double));
  char buf[384];
  char *end = dtoa(value, buf);
  *end = '\0';
  out.write(std::string(buf));
}

// Specialization for float - print with full precision using dtoa
template<>
void print_type<float>(OutputAdapter& out, const uint8_t* data) {
  float value;
  std::memcpy(&value, data, sizeof(float));
  char buf[384];
  char *end = dtoa(value, buf);
  *end = '\0';
  out.write(std::string(buf));
}

// Unified print function for vector types
template<typename T, size_t N>
void print_vector(OutputAdapter& out, const uint8_t* data) {
  T values[N];
  std::memcpy(&values, data, sizeof(T) * N);
  out.write("(");
  for (size_t i = 0; i < N; ++i) {
    if (i > 0) out.write(", ");
    out << values[i];
  }
  out.write(")");
}

// Specializations for char types (print as int)
template<>
void print_vector<char, 2>(OutputAdapter& out, const uint8_t* data) {
  char values[2];
  std::memcpy(&values, data, sizeof(char) * 2);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(")");
}

template<>
void print_vector<char, 3>(OutputAdapter& out, const uint8_t* data) {
  char values[3];
  std::memcpy(&values, data, sizeof(char) * 3);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(", ");
  out.write(int(values[2]));
  out.write(")");
}

template<>
void print_vector<char, 4>(OutputAdapter& out, const uint8_t* data) {
  char values[4];
  std::memcpy(&values, data, sizeof(char) * 4);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(", ");
  out.write(int(values[2]));
  out.write(", ");
  out.write(int(values[3]));
  out.write(")");
}

template<>
void print_vector<uint8_t, 2>(OutputAdapter& out, const uint8_t* data) {
  uint8_t values[2];
  std::memcpy(&values, data, sizeof(uint8_t) * 2);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(")");
}

template<>
void print_vector<uint8_t, 3>(OutputAdapter& out, const uint8_t* data) {
  uint8_t values[3];
  std::memcpy(&values, data, sizeof(uint8_t) * 3);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(", ");
  out.write(int(values[2]));
  out.write(")");
}

template<>
void print_vector<uint8_t, 4>(OutputAdapter& out, const uint8_t* data) {
  uint8_t values[4];
  std::memcpy(&values, data, sizeof(uint8_t) * 4);
  out.write("(");
  out.write(int(values[0]));
  out.write(", ");
  out.write(int(values[1]));
  out.write(", ");
  out.write(int(values[2]));
  out.write(", ");
  out.write(int(values[3]));
  out.write(")");
}

// ============================================================================
// Centralized Type Dispatch System
// ============================================================================

// Macro to reduce repetition in switch statements
// Handles both binary-serializable types and value types uniformly
#define DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_NAME, CPP_TYPE, PRINT_FUNC) \
  case value::TYPE_ID_NAME: \
    PRINT_FUNC<CPP_TYPE>(out, data); \
    break;

#define DISPATCH_VALUE_TYPE(TYPE_ID_NAME, VALUE_TYPE) \
  case value::TYPE_ID_NAME: \
    print_type<value::VALUE_TYPE>(out, data); \
    break;

#define DISPATCH_VECTOR_TYPE(TYPE_ID_NAME, CPP_TYPE, DIM) \
  case value::TYPE_ID_NAME: \
    print_vector<CPP_TYPE, DIM>(out, data); \
    break;

// Centralized print dispatch using OutputAdapter
void print_binary_serializable_value_dispatch(OutputAdapter& out, const uint8_t* data, uint32_t type_id) {
  using namespace value;

  // Strip array bit - we're printing a single element
  type_id = type_id & (~TYPE_ID_1D_ARRAY_BIT);

  switch (type_id) {
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_BOOL, bool, print_type)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_CHAR, char, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_CHAR2, char, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_CHAR3, char, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_CHAR4, char, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_UCHAR, uint8_t, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UCHAR2, uint8_t, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UCHAR3, uint8_t, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UCHAR4, uint8_t, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_SHORT, int16_t, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_SHORT2, int16_t, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_SHORT3, int16_t, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_SHORT4, int16_t, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_USHORT, uint16_t, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_USHORT2, uint16_t, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_USHORT3, uint16_t, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_USHORT4, uint16_t, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_INT32, int32_t, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_INT2, int32_t, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_INT3, int32_t, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_INT4, int32_t, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_UINT32, uint32_t, print_type)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UINT2, uint32_t, 2)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UINT3, uint32_t, 3)
    DISPATCH_VECTOR_TYPE(TYPE_ID_UINT4, uint32_t, 4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_INT64, int64_t, print_type)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_UINT64, uint64_t, print_type)
    DISPATCH_VALUE_TYPE(TYPE_ID_HALF, half)
    DISPATCH_VALUE_TYPE(TYPE_ID_HALF2, half2)
    DISPATCH_VALUE_TYPE(TYPE_ID_HALF3, half3)
    DISPATCH_VALUE_TYPE(TYPE_ID_HALF4, half4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_FLOAT, float, print_type)
    DISPATCH_VALUE_TYPE(TYPE_ID_FLOAT2, float2)
    DISPATCH_VALUE_TYPE(TYPE_ID_FLOAT3, float3)
    DISPATCH_VALUE_TYPE(TYPE_ID_FLOAT4, float4)
    DISPATCH_BINARY_SERIALIZABLE_TYPE(TYPE_ID_DOUBLE, double, print_type)
    DISPATCH_VALUE_TYPE(TYPE_ID_DOUBLE2, double2)
    DISPATCH_VALUE_TYPE(TYPE_ID_DOUBLE3, double3)
    DISPATCH_VALUE_TYPE(TYPE_ID_DOUBLE4, double4)
    DISPATCH_VALUE_TYPE(TYPE_ID_QUATH, quath)
    DISPATCH_VALUE_TYPE(TYPE_ID_QUATF, quatf)
    DISPATCH_VALUE_TYPE(TYPE_ID_QUATD, quatd)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX2F, matrix2f)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX3F, matrix3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX4F, matrix4f)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX2D, matrix2d)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX3D, matrix3d)
    DISPATCH_VALUE_TYPE(TYPE_ID_MATRIX4D, matrix4d)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR3H, color3h)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR3F, color3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR3D, color3d)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR4H, color4h)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR4F, color4f)
    DISPATCH_VALUE_TYPE(TYPE_ID_COLOR4D, color4d)
    DISPATCH_VALUE_TYPE(TYPE_ID_POINT3H, point3h)
    DISPATCH_VALUE_TYPE(TYPE_ID_POINT3F, point3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_POINT3D, point3d)
    DISPATCH_VALUE_TYPE(TYPE_ID_NORMAL3H, normal3h)
    DISPATCH_VALUE_TYPE(TYPE_ID_NORMAL3F, normal3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_NORMAL3D, normal3d)
    DISPATCH_VALUE_TYPE(TYPE_ID_VECTOR3H, vector3h)
    DISPATCH_VALUE_TYPE(TYPE_ID_VECTOR3F, vector3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_VECTOR3D, vector3d)
    DISPATCH_VALUE_TYPE(TYPE_ID_FRAME4D, frame4d)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD2H, texcoord2h)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD2F, texcoord2f)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD2D, texcoord2d)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD3H, texcoord3h)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD3F, texcoord3f)
    DISPATCH_VALUE_TYPE(TYPE_ID_TEXCOORD3D, texcoord3d)
    default:
      out.write("[Unknown binary-serializable type: ");
      out.write(static_cast<int>(type_id));
      out.write("]");
      break;
  }
}

#undef DISPATCH_BINARY_SERIALIZABLE_TYPE
#undef DISPATCH_VALUE_TYPE
#undef DISPATCH_VECTOR_TYPE

} // namespace

// Forward declarations
void pprint_binary_serializable_value_by_type(StreamWriter& writer, const uint8_t* data, uint32_t type_id);
size_t get_binary_serializable_type_size(uint32_t type_id);

/// Helper function to print an array of binary-serializable values
/// @param writer Output writer
/// @param data Pointer to the first array element
/// @param type_id Type ID of the array elements
/// @param array_size Number of elements in the array
static void pprint_binary_serializable_array_by_type(StreamWriter& writer, const uint8_t* data, uint32_t type_id, size_t array_size) {
    size_t element_size = get_binary_serializable_type_size(type_id);
    if (element_size == 0) {
        writer.write("/* Unknown type_id: ");
        writer.write(type_id);
        writer.write(" */");
        return;
    }

    writer.write("[");
    for (size_t i = 0; i < array_size; ++i) {
        if (i > 0) {
            writer.write(", ");
        }
        const uint8_t* element_ptr = data + (i * element_size);
        pprint_binary_serializable_value_by_type(writer, element_ptr, type_id);
    }
    writer.write("]");
}

std::string pprint_binary_serializable_value_by_type(const uint8_t* data, uint32_t type_id) {
    // Use unified dispatch system with string output adapter
    StringOutputAdapter adapter;
    print_binary_serializable_value_dispatch(adapter, data, type_id);
    return adapter.str();
}

void pprint_binary_serializable_value_by_type(StreamWriter& writer, const uint8_t* data, uint32_t type_id) {
    // Use unified dispatch system with StreamWriter adapter
    StreamWriterAdapter adapter(writer);
    print_binary_serializable_value_dispatch(adapter, data, type_id);
}

size_t get_binary_serializable_type_size(uint32_t type_id) {
    using namespace value;

    // Strip array bit - we want the element size
    type_id = type_id & (~TYPE_ID_1D_ARRAY_BIT);

#define SIZE_CASE(__type) \
    case TypeTraits<__type>::type_id(): return sizeof(__type);

    switch (type_id) {
        SIZE_CASE(bool)
        SIZE_CASE(char)
        SIZE_CASE(value::char2)
        SIZE_CASE(value::char3)
        SIZE_CASE(value::char4)
        SIZE_CASE(uint8_t)
        SIZE_CASE(value::uchar2)
        SIZE_CASE(value::uchar3)
        SIZE_CASE(value::uchar4)
        SIZE_CASE(int16_t)
        SIZE_CASE(value::short2)
        SIZE_CASE(value::short3)
        SIZE_CASE(value::short4)
        SIZE_CASE(uint16_t)
        SIZE_CASE(value::ushort2)
        SIZE_CASE(value::ushort3)
        SIZE_CASE(value::ushort4)
        SIZE_CASE(int32_t)
        SIZE_CASE(value::int2)
        SIZE_CASE(value::int3)
        SIZE_CASE(value::int4)
        SIZE_CASE(uint32_t)
        SIZE_CASE(value::uint2)
        SIZE_CASE(value::uint3)
        SIZE_CASE(value::uint4)
        SIZE_CASE(int64_t)
        SIZE_CASE(uint64_t)
        SIZE_CASE(value::half)
        SIZE_CASE(value::half2)
        SIZE_CASE(value::half3)
        SIZE_CASE(value::half4)
        SIZE_CASE(float)
        SIZE_CASE(value::float2)
        SIZE_CASE(value::float3)
        SIZE_CASE(value::float4)
        SIZE_CASE(double)
        SIZE_CASE(value::double2)
        SIZE_CASE(value::double3)
        SIZE_CASE(value::double4)
        SIZE_CASE(value::quath)
        SIZE_CASE(value::quatf)
        SIZE_CASE(value::quatd)
        SIZE_CASE(value::matrix2f)
        SIZE_CASE(value::matrix3f)
        SIZE_CASE(value::matrix4f)
        SIZE_CASE(value::matrix2d)
        SIZE_CASE(value::matrix3d)
        SIZE_CASE(value::matrix4d)
        SIZE_CASE(value::color3h)
        SIZE_CASE(value::color3f)
        SIZE_CASE(value::color3d)
        SIZE_CASE(value::color4h)
        SIZE_CASE(value::color4f)
        SIZE_CASE(value::color4d)
        SIZE_CASE(value::point3h)
        SIZE_CASE(value::point3f)
        SIZE_CASE(value::point3d)
        SIZE_CASE(value::normal3h)
        SIZE_CASE(value::normal3f)
        SIZE_CASE(value::normal3d)
        SIZE_CASE(value::vector3h)
        SIZE_CASE(value::vector3f)
        SIZE_CASE(value::vector3d)
        SIZE_CASE(value::frame4d)
        SIZE_CASE(value::texcoord2h)
        SIZE_CASE(value::texcoord2f)
        SIZE_CASE(value::texcoord2d)
        SIZE_CASE(value::texcoord3h)
        SIZE_CASE(value::texcoord3f)
        SIZE_CASE(value::texcoord3d)
        default:
            return 0;  // Unknown type
    }
#undef SIZE_CASE
}


void pprint_timesamples(StreamWriter& writer, const value::TimeSamples& samples, uint32_t indent) {
    // Write opening brace
    writer.write("{\n");

    if (samples.empty()) {
        writer.write(pprint::Indent(indent));
        writer.write("}");
        return;
    }

    // Check if using binary storage (_times non-empty with _data/_data_offsets)
    // vs Sample-based storage (_samples vector)
    bool has_binary_data = samples.is_using_binary_storage() &&
                           (!samples.get_data().empty() ||
                            !samples.get_data_offsets().empty());

    if (has_binary_data) {

        // Binary storage path: use _data buffer with _data_offsets

        // Get type information
        uint32_t type_id = samples.type_id();
        uint32_t elem_size = samples.element_size();

        if (elem_size == 0) {
            writer.write(pprint::Indent(indent + 1));
            writer.write("/* Unknown element_size for type_id: ");
            writer.write(type_id);
            writer.write(" */\n");
            writer.write(pprint::Indent(indent));
            writer.write("}");
            return;
        }

        // Get arrays from binary storage
        const auto& times = samples.get_times();
        const auto& blocked = samples.get_blocked();
        const auto& data = samples.get_data();
        const auto& data_offsets = samples.get_data_offsets();
        bool is_array_type = samples.is_array();

        for (size_t i = 0; i < times.size(); ++i) {
            writer.write(pprint::Indent(indent + 1));
            writer.write(times[i]);
            writer.write(": ");

            // Check blocked: either via blocked buffer or BLOCKED_OFFSET sentinel
            bool is_blocked = (i < blocked.size()) ? blocked[i] : false;
            bool offset_is_blocked = (i < data_offsets.size()) &&
                                     (data_offsets[i] == value::TimeSamples::BLOCKED_OFFSET);
            if (is_blocked || offset_is_blocked) {
                writer.write("None");
            } else if (i < data_offsets.size()) {
                size_t byte_offset = data_offsets[i];
                const uint8_t* value_ptr = data.data() + byte_offset;

                if (is_array_type) {
                    size_t per_sample_count = samples.get_array_count(i);
                    pprint_binary_serializable_array_by_type(writer, value_ptr, type_id, per_sample_count);
                } else {
                    pprint_binary_serializable_value_by_type(writer, value_ptr, type_id);
                }
            } else {
                writer.write("/* ERROR: data_offsets index out of bounds */");
            }

            if (i < times.size() - 1) {
                writer.write(",");
            }
            writer.write("\n");
        }
    } else {
        // Non-binary-storage path: use regular samples
        const auto& samples_vec = samples.get_samples();

        for (size_t i = 0; i < samples_vec.size(); ++i) {
            const auto& sample = samples_vec[i];

            writer.write(pprint::Indent(indent + 1));
            writer.write(sample.t);
            writer.write(": ");

            if (sample.blocked || sample.value.is_none()) {
                writer.write("None");
            } else {
                // Pretty print the value using its pprint method
                std::string value_str = pprint_value(sample.value, indent + 1);

                // Remove leading/trailing whitespace for inline display
                size_t start = value_str.find_first_not_of(" \t\n\r");
                size_t end = value_str.find_last_not_of(" \t\n\r");

                if (start != std::string::npos && end != std::string::npos) {
                    writer.write(value_str.substr(start, end - start + 1));
                } else {
                    writer.write(value_str);
                }
            }

            if (i < samples_vec.size() - 1) {
                writer.write(",");
            }
            writer.write("\n");
        }
    }

    writer.write(pprint::Indent(indent));
    writer.write("}");
}

std::string pprint_timesamples(const value::TimeSamples& samples, uint32_t indent) {
    // Use StreamWriter internally for efficiency
    StreamWriter writer;
    pprint_timesamples(writer, samples, indent);
    return writer.str();
}

void set_threaded_print_threshold(size_t threshold) {
    g_threaded_print_config.thread_threshold = threshold;
}

void set_threaded_print_num_threads(unsigned int num_threads) {
    g_threaded_print_config.num_threads = num_threads;
}

} // namespace lightusd
