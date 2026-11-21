// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment, Inc.
//
// Synthetic USDA parser benchmark.

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "ascii-parser.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-types.hh"

namespace {

enum class BenchmarkProfile {
  Full,
  Quick,
};

struct BenchmarkConfig {
  size_t scalar_count;
  size_t tuple_count;
  size_t quat_count;
  size_t matrix_count;
  size_t usda_count;
  int iterations;
};

struct Options {
  BenchmarkProfile profile{BenchmarkProfile::Full};
  bool direct_only{false};
  bool usda_only{false};
  bool text_only{false};
};

BenchmarkConfig ConfigFor(BenchmarkProfile profile) {
  if (profile == BenchmarkProfile::Quick) {
    return BenchmarkConfig{/*scalar_count=*/2048,
                           /*tuple_count=*/1024,
                           /*quat_count=*/512,
                           /*matrix_count=*/64,
                           /*usda_count=*/256,
                           /*iterations=*/1};
  }

  return BenchmarkConfig{/*scalar_count=*/500000,
                         /*tuple_count=*/100000,
                         /*quat_count=*/100000,
                         /*matrix_count=*/10000,
                         /*usda_count=*/10000,
                         /*iterations=*/3};
}

void PrintUsage(const char *argv0) {
  std::cout << "Usage: " << argv0
            << " [--quick] [--direct-only] [--usda-only] [--text-only]\n";
}

Options ParseOptions(int argc, char **argv) {
  Options opts;
  for (int i = 1; i < argc; i++) {
    const std::string arg(argv[i]);
    if (arg == "--quick") {
      opts.profile = BenchmarkProfile::Quick;
    } else if (arg == "--direct-only") {
      opts.direct_only = true;
    } else if (arg == "--usda-only") {
      opts.usda_only = true;
    } else if (arg == "--text-only") {
      opts.text_only = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      PrintUsage(argv[0]);
      std::exit(1);
    }
  }

  if (opts.direct_only && opts.usda_only) {
    std::cerr << "--direct-only and --usda-only are mutually exclusive.\n";
    std::exit(1);
  }
  if (opts.text_only && opts.usda_only) {
    std::cerr << "--text-only and --usda-only are mutually exclusive.\n";
    std::exit(1);
  }

  return opts;
}

void AppendDoubleLiteral(std::string *out, double value, int precision) {
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
  if (n > 0) {
    out->append(buf, static_cast<size_t>(n));
  }
}

void AppendIntLiteral(std::string *out, int64_t value) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(value));
  if (n > 0) {
    out->append(buf, static_cast<size_t>(n));
  }
}

void AppendUIntLiteral(std::string *out, uint64_t value) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%llu",
                              static_cast<unsigned long long>(value));
  if (n > 0) {
    out->append(buf, static_cast<size_t>(n));
  }
}

double FloatValue(size_t i) {
  const int base = static_cast<int>(i % 20001) - 10000;
  return double(base) * 0.125 + double(i % 7) * 0.001;
}

double DoubleValue(size_t i) {
  const int base = static_cast<int>(i % 20001) - 10000;
  return double(base) * 0.0009765625 + double((i * 17) % 101) * 1.0e-9;
}

double HalfValue(size_t i) {
  const int base = static_cast<int>(i % 1025) - 512;
  return double(base) * 0.125;
}

int32_t IntValue(size_t i) {
  return static_cast<int32_t>(static_cast<int64_t>((i * 37) % 200000) -
                              100000);
}

uint32_t UIntValue(size_t i) {
  return static_cast<uint32_t>((i * 65537u) % 4000000000u);
}

int64_t Int64Value(size_t i) {
  return static_cast<int64_t>(i) * 2147483647ll - 900000000000ll;
}

uint64_t UInt64Value(size_t i) {
  return 100000000000ull + static_cast<uint64_t>(i) * 2654435761ull;
}

template <typename AppendValue>
std::string MakeScalarArray(size_t count, AppendValue append_value) {
  std::string out;
  out.reserve(count * 16 + 2);
  out.push_back('[');
  for (size_t i = 0; i < count; i++) {
    append_value(&out, i);
    if (i + 1 < count) {
      out.append(", ");
    }
  }
  out.push_back(']');
  return out;
}

template <typename AppendValue>
std::string MakeTupleArray(size_t count, size_t arity, AppendValue append_value) {
  std::string out;
  out.reserve(count * arity * 16 + count * 4 + 2);
  out.push_back('[');
  for (size_t i = 0; i < count; i++) {
    out.push_back('(');
    for (size_t j = 0; j < arity; j++) {
      append_value(&out, i, j);
      if (j + 1 < arity) {
        out.append(", ");
      }
    }
    out.push_back(')');
    if (i + 1 < count) {
      out.append(", ");
    }
  }
  out.push_back(']');
  return out;
}

std::string MakeMatrixArray(size_t count, size_t dimension, bool double_precision) {
  std::string out;
  const size_t values_per_matrix = dimension * dimension;
  out.reserve(count * (values_per_matrix * 18 + dimension * 4) + 2);
  out.push_back('[');
  for (size_t i = 0; i < count; i++) {
    out.append("( ");
    for (size_t row = 0; row < dimension; row++) {
      out.push_back('(');
      for (size_t col = 0; col < dimension; col++) {
        const size_t idx = i * values_per_matrix + row * dimension + col;
        const double value =
            (row == col) ? 1.0
                         : (double_precision ? DoubleValue(idx)
                                             : FloatValue(idx) * 0.001);
        AppendDoubleLiteral(&out, value, double_precision ? 17 : 9);
        if (col + 1 < dimension) {
          out.append(", ");
        }
      }
      out.push_back(')');
      if (row + 1 < dimension) {
        out.append(", ");
      }
    }
    out.append(" )");
    if (i + 1 < count) {
      out.append(", ");
    }
  }
  out.push_back(']');
  return out;
}

std::string MakeMatrix2fArray(size_t count) {
  return MakeMatrixArray(count, 2, false);
}

std::string MakeMatrix3fArray(size_t count) {
  return MakeMatrixArray(count, 3, false);
}

std::string MakeMatrix4fArray(size_t count) {
  return MakeMatrixArray(count, 4, false);
}

std::string MakeMatrix2dArray(size_t count) {
  return MakeMatrixArray(count, 2, true);
}

std::string MakeMatrix3dArray(size_t count) {
  return MakeMatrixArray(count, 3, true);
}

std::string MakeMatrix4dArray(size_t count) {
  return MakeMatrixArray(count, 4, true);
}

void AppendTokenLiteral(std::string *out, size_t i) {
  out->push_back('"');
  out->append("token_");
  AppendUIntLiteral(out, static_cast<uint64_t>(i % 997));
  out->push_back('_');
  AppendUIntLiteral(out, static_cast<uint64_t>((i * 17) % 65521));
  if ((i % 101) == 0) {
    out->append("_bracket]hash#");
  }
  out->push_back('"');
}

void AppendStringLiteral(std::string *out, size_t i) {
  out->push_back('"');
  out->append("string ");
  AppendUIntLiteral(out, static_cast<uint64_t>(i % 997));
  if ((i % 97) == 0) {
    out->append(" quoted \\\"value\\\"");
  } else if ((i % 89) == 0) {
    out->append(" line\\nvalue");
  } else if ((i % 83) == 0) {
    out->append(" bracket] hash#value");
  }
  out->push_back('"');
}

std::string MakeTokenArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendTokenLiteral(out, i);
  });
}

std::string MakeStringArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendStringLiteral(out, i);
  });
}

std::string MakeIntArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendIntLiteral(out, IntValue(i));
  });
}

std::string MakeUIntArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendUIntLiteral(out, UIntValue(i));
  });
}

std::string MakeInt64Array(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendIntLiteral(out, Int64Value(i));
  });
}

std::string MakeUInt64Array(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendUIntLiteral(out, UInt64Value(i));
  });
}

std::string MakeHalfArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendDoubleLiteral(out, HalfValue(i), 9);
  });
}

std::string MakeFloatArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendDoubleLiteral(out, FloatValue(i), 9);
  });
}

std::string MakeDoubleArray(size_t count) {
  return MakeScalarArray(count, [](std::string *out, size_t i) {
    AppendDoubleLiteral(out, DoubleValue(i), 17);
  });
}

std::string MakeHalf3Array(size_t count) {
  return MakeTupleArray(count, 3, [](std::string *out, size_t i, size_t j) {
    AppendDoubleLiteral(out, HalfValue(i * 3 + j), 9);
  });
}

std::string MakeFloat3Array(size_t count) {
  return MakeTupleArray(count, 3, [](std::string *out, size_t i, size_t j) {
    AppendDoubleLiteral(out, FloatValue(i * 3 + j), 9);
  });
}

std::string MakeDouble3Array(size_t count) {
  return MakeTupleArray(count, 3, [](std::string *out, size_t i, size_t j) {
    AppendDoubleLiteral(out, DoubleValue(i * 3 + j), 17);
  });
}

std::string MakeQuatHalfArray(size_t count) {
  return MakeTupleArray(count, 4, [](std::string *out, size_t i, size_t j) {
    const double value = (j == 0) ? 1.0 : HalfValue(i * 3 + j);
    AppendDoubleLiteral(out, value, 9);
  });
}

std::string MakeQuatFloatArray(size_t count) {
  return MakeTupleArray(count, 4, [](std::string *out, size_t i, size_t j) {
    const double value = (j == 0) ? 1.0 : FloatValue(i * 3 + j) * 0.001;
    AppendDoubleLiteral(out, value, 9);
  });
}

std::string MakeQuatDoubleArray(size_t count) {
  return MakeTupleArray(count, 4, [](std::string *out, size_t i, size_t j) {
    const double value = (j == 0) ? 1.0 : DoubleValue(i * 3 + j);
    AppendDoubleLiteral(out, value, 17);
  });
}

template <typename Func>
bool Benchmark(const std::string &name, size_t bytes, int iterations, Func func) {
  const auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; i++) {
    if (!func()) {
      std::cerr << "Benchmark failed: " << name << "\n";
      return false;
    }
  }
  const auto end = std::chrono::high_resolution_clock::now();
  const double us =
      double(std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                 .count());
  const double avg_ms = us / 1000.0 / double(iterations);
  const double mib = double(bytes) / (1024.0 * 1024.0);
  const double mib_per_s = (avg_ms > 0.0) ? (mib / (avg_ms / 1000.0)) : 0.0;

  std::cout << "  " << std::left << std::setw(14) << name << std::right
            << " bytes=" << std::setw(10) << bytes
            << " avg_ms=" << std::setw(9) << std::fixed << std::setprecision(3)
            << avg_ms << " MiB/s=" << std::setw(9) << std::setprecision(1)
            << mib_per_s;
  if (iterations > 1) {
    std::cout << " runs=" << iterations;
  }
  std::cout << "\n";
  return true;
}

template <typename T>
bool RunArrayCase(const std::string &name, const std::string &data,
                  size_t expected_count, int iterations) {
  return Benchmark(name, data.size(), iterations, [&]() {
    lightusd::legacy::StreamReader sr(reinterpret_cast<const uint8_t *>(data.data()),
                              data.size(), false);
    lightusd::legacy::ascii::AsciiParser parser(&sr);
    std::vector<T> result;
    if (!parser.ParseBasicTypeArray(&result)) {
      std::cerr << "Parse error for " << name << ": " << parser.GetError()
                << "\n";
      return false;
    }
    if (result.size() != expected_count) {
      std::cerr << "Unexpected element count for " << name << ": got "
                << result.size() << ", expected " << expected_count << "\n";
      return false;
    }
    return true;
  });
}

std::string MakeSyntheticUsda(size_t count, size_t matrix_count) {
  std::string out;
  out.reserve(count * 1000 + matrix_count * 1200);
  out.append("#usda 1.0\n\n");
  out.append("def Scope \"NumericBench\" {\n");
  out.append("  custom int[] intValues = ");
  out.append(MakeIntArray(count));
  out.append("\n  custom uint[] uintValues = ");
  out.append(MakeUIntArray(count));
  out.append("\n  custom token[] tokenValues = ");
  out.append(MakeTokenArray(count));
  out.append("\n  custom string[] stringValues = ");
  out.append(MakeStringArray(count));
  out.append("\n  custom int64[] int64Values = ");
  out.append(MakeInt64Array(count));
  out.append("\n  custom uint64[] uint64Values = ");
  out.append(MakeUInt64Array(count));
  out.append("\n  custom half[] halfValues = ");
  out.append(MakeHalfArray(count));
  out.append("\n  custom half3[] half3Values = ");
  out.append(MakeHalf3Array(count));
  out.append("\n  custom float[] floatValues = ");
  out.append(MakeFloatArray(count));
  out.append("\n  custom float3[] float3Values = ");
  out.append(MakeFloat3Array(count));
  out.append("\n  custom double[] doubleValues = ");
  out.append(MakeDoubleArray(count));
  out.append("\n  custom double3[] double3Values = ");
  out.append(MakeDouble3Array(count));
  out.append("\n  custom quath[] quathValues = ");
  out.append(MakeQuatHalfArray(count));
  out.append("\n  custom quatf[] quatfValues = ");
  out.append(MakeQuatFloatArray(count));
  out.append("\n  custom quatd[] quatdValues = ");
  out.append(MakeQuatDoubleArray(count));
  out.append("\n  custom matrix2f[] matrix2fValues = ");
  out.append(MakeMatrix2fArray(matrix_count));
  out.append("\n  custom matrix3f[] matrix3fValues = ");
  out.append(MakeMatrix3fArray(matrix_count));
  out.append("\n  custom matrix4f[] matrix4fValues = ");
  out.append(MakeMatrix4fArray(matrix_count));
  out.append("\n  custom matrix2d[] matrix2dValues = ");
  out.append(MakeMatrix2dArray(matrix_count));
  out.append("\n  custom matrix3d[] matrix3dValues = ");
  out.append(MakeMatrix3dArray(matrix_count));
  out.append("\n  custom matrix4d[] matrix4dValues = ");
  out.append(MakeMatrix4dArray(matrix_count));
  out.append("\n}\n");
  return out;
}

bool RunDirectArrayBenchmarks(const BenchmarkConfig &config) {
  std::cout << "\n=== Direct Array Literal Parsing ===\n";
  bool ok = true;
  ok &= RunArrayCase<int32_t>("int[]", MakeIntArray(config.scalar_count),
                              config.scalar_count, config.iterations);
  ok &= RunArrayCase<uint32_t>("uint[]", MakeUIntArray(config.scalar_count),
                               config.scalar_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::token>(
      "token[]", MakeTokenArray(config.scalar_count), config.scalar_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::StringData>(
      "string[]", MakeStringArray(config.scalar_count), config.scalar_count,
      config.iterations);
  ok &= RunArrayCase<int64_t>("int64[]", MakeInt64Array(config.scalar_count),
                              config.scalar_count, config.iterations);
  ok &= RunArrayCase<uint64_t>("uint64[]", MakeUInt64Array(config.scalar_count),
                               config.scalar_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::half>(
      "half[]", MakeHalfArray(config.scalar_count), config.scalar_count,
      config.iterations);
  ok &= RunArrayCase<float>("float[]", MakeFloatArray(config.scalar_count),
                            config.scalar_count, config.iterations);
  ok &= RunArrayCase<double>("double[]", MakeDoubleArray(config.scalar_count),
                             config.scalar_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::half3>(
      "half3[]", MakeHalf3Array(config.tuple_count), config.tuple_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::float3>(
      "float3[]", MakeFloat3Array(config.tuple_count), config.tuple_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::double3>(
      "double3[]", MakeDouble3Array(config.tuple_count), config.tuple_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::quath>(
      "quath[]", MakeQuatHalfArray(config.quat_count), config.quat_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::quatf>(
      "quatf[]", MakeQuatFloatArray(config.quat_count), config.quat_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::quatd>(
      "quatd[]", MakeQuatDoubleArray(config.quat_count), config.quat_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix2f>(
      "matrix2f[]", MakeMatrix2fArray(config.matrix_count),
      config.matrix_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix3f>(
      "matrix3f[]", MakeMatrix3fArray(config.matrix_count),
      config.matrix_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix4f>(
      "matrix4f[]", MakeMatrix4fArray(config.matrix_count),
      config.matrix_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix2d>(
      "matrix2d[]", MakeMatrix2dArray(config.matrix_count),
      config.matrix_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix3d>(
      "matrix3d[]", MakeMatrix3dArray(config.matrix_count),
      config.matrix_count, config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::matrix4d>(
      "matrix4d[]", MakeMatrix4dArray(config.matrix_count),
      config.matrix_count, config.iterations);
  return ok;
}

bool RunTextArrayBenchmarks(const BenchmarkConfig &config) {
  std::cout << "\n=== Direct Text Array Literal Parsing ===\n";
  bool ok = true;
  ok &= RunArrayCase<lightusd::legacy::value::token>(
      "token[]", MakeTokenArray(config.scalar_count), config.scalar_count,
      config.iterations);
  ok &= RunArrayCase<lightusd::legacy::value::StringData>(
      "string[]", MakeStringArray(config.scalar_count), config.scalar_count,
      config.iterations);
  return ok;
}

bool RunUsdaBenchmark(const BenchmarkConfig &config) {
  std::cout << "\n=== Synthetic USDA Parsing ===\n";
  const std::string usda = MakeSyntheticUsda(
      config.usda_count, (std::max)(size_t(1), config.usda_count / 16));
  std::cout << "  attributes=21 elements_per_array_attr=" << config.usda_count
            << " bytes=" << usda.size() << "\n";

  return Benchmark("LoadUSDA", usda.size(), config.iterations, [&]() {
    lightusd::legacy::Stage stage;
    std::string warn;
    std::string err;
    const bool ret = lightusd::legacy::LoadUSDFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "synthetic-numeric.usda", &stage, &warn, &err);
    if (!ret) {
      std::cerr << "LoadUSDFromMemory failed: " << err << "\n";
      return false;
    }
    return true;
  });
}

}  // namespace

int main(int argc, char **argv) {
  const Options opts = ParseOptions(argc, argv);
  const BenchmarkConfig config = ConfigFor(opts.profile);

  std::cout << "========================================\n";
  std::cout << "LightUSD Synthetic USDA Parser Benchmark\n";
  std::cout << "Profile: "
            << ((opts.profile == BenchmarkProfile::Quick) ? "quick" : "full")
            << "\n";
  std::cout << "========================================\n";

  bool ok = true;
  if (opts.text_only) {
    ok &= RunTextArrayBenchmarks(config);
  } else if (!opts.usda_only) {
    ok &= RunDirectArrayBenchmarks(config);
  }
  if (!opts.text_only && !opts.direct_only) {
    ok &= RunUsdaBenchmark(config);
  }

  std::cout << "\n========================================\n";
  std::cout << (ok ? "Benchmark Complete" : "Benchmark Failed") << "\n";
  std::cout << "========================================\n";

  return ok ? 0 : 1;
}
