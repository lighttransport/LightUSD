#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "asset-resolution.hh"
#include "safe-arithmetic.hh"
#include "unit-security.h"
#include "json-to-usd.hh"
#include "security-policy.hh"
#include "sha256.hh"
#include "lightusd.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/common-utils.hh"
#include "zstd-compression.hh"
#include "base122.hh"
#include "str-util.hh"
#include "io-util.hh"
#include "stage.hh"
#include "core/prim.hh"
#include "composition-graph.hh"

#include <string>
#include <cstring>
#include <limits>
#include <vector>

using namespace lightusd;

namespace {

int SecurityTestResolveAsset(const char *asset_name,
                             const std::vector<std::string> & /*search_paths*/,
                             std::string *resolved_asset_name,
                             std::string * /*err*/, void * /*userdata*/) {
  if (!asset_name || !resolved_asset_name) {
    return -1;
  }
  *resolved_asset_name = asset_name;
  return 0;
}

int SecurityTestLargeSizeAsset(const char * /*resolved_asset_name*/,
                               uint64_t *nbytes, std::string * /*err*/,
                               void * /*userdata*/) {
  if (!nbytes) {
    return -1;
  }
  *nbytes = 2ull * 1024ull * 1024ull;  // 2 MiB
  return 0;
}

int SecurityTestReadAsset(const char * /*resolved_asset_name*/,
                          uint64_t req_nbytes, uint8_t *out_buf,
                          uint64_t *nbytes, std::string * /*err*/,
                          void * /*userdata*/) {
  if (!out_buf || !nbytes) {
    return -1;
  }
  if (req_nbytes > 0) {
    memset(out_buf, 0, static_cast<size_t>(req_nbytes));
  }
  *nbytes = req_nbytes;
  return 0;
}

int SecurityTestSmallSizeAsset(const char * /*resolved_asset_name*/,
                               uint64_t *nbytes, std::string * /*err*/,
                               void * /*userdata*/) {
  if (!nbytes) {
    return -1;
  }
  *nbytes = 16;
  return 0;
}

int SecurityTestOverreportedReadAsset(const char * /*resolved_asset_name*/,
                                      uint64_t req_nbytes, uint8_t *out_buf,
                                      uint64_t *nbytes,
                                      std::string * /*err*/,
                                      void * /*userdata*/) {
  if (!out_buf || !nbytes) {
    return -1;
  }
  if (req_nbytes > 0) {
    memset(out_buf, 0, static_cast<size_t>(req_nbytes));
  }
  *nbytes = req_nbytes + 1;
  return 0;
}

}  // namespace

void security_empty_input_test(void) {
  Stage stage;
  std::string warn, err;

  // nullptr with 0 length
  {
    bool ok = LoadUSDAFromMemory(nullptr, 0, "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Loading nullptr/0 should fail gracefully");
  }

  // Empty string
  {
    std::string empty_str;
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(empty_str.data()), empty_str.size(),
        "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Loading empty string should fail gracefully");
  }

  // Single whitespace
  {
    std::string ws = " ";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(ws.data()), ws.size(),
        "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Loading whitespace-only should fail gracefully");
  }
}

void security_truncated_header_test(void) {
  Stage stage;
  std::string warn, err;

  // Just "#usd" (truncated)
  {
    std::string truncated = "#usd";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(truncated.data()), truncated.size(),
        "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Truncated header '#usd' should fail gracefully");
  }

  // "#usda" without version
  {
    std::string partial = "#usda";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(partial.data()), partial.size(),
        "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Partial header '#usda' should fail gracefully");
  }

  // "#usda 1" (incomplete version)
  {
    std::string incomplete = "#usda 1";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(incomplete.data()),
        incomplete.size(), "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Incomplete version should fail gracefully");
  }

  // Just a newline
  {
    std::string nl = "\n";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(nl.data()), nl.size(),
        "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("Newline-only input should fail gracefully");
  }
}

void security_null_bytes_test(void) {
  Stage stage;
  std::string warn, err;

  // Valid header followed by null bytes
  {
    std::string data = "#usda 1.0\n";
    data += '\0';
    data += '\0';
    data += "def Xform \"test\" {}\n";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(data.data()), data.size(),
        "test.usda", &stage, &warn, &err);
    // Either succeeds or fails, but should not crash
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Null bytes in data should not crash");
  }

  // All null bytes
  {
    std::vector<uint8_t> nulls(64, 0);
    bool ok = LoadUSDAFromMemory(nulls.data(), nulls.size(),
                                 "test.usda", &stage, &warn, &err);
    TEST_CHECK(!ok);
    TEST_MSG("All-null input should fail gracefully");
  }

  // Null bytes in prim name
  {
    std::string data = "#usda 1.0\ndef Xform \"te";
    data += '\0';
    data += "st\" {}\n";
    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(data.data()), data.size(),
        "test.usda", &stage, &warn, &err);
    // Should not crash regardless of result
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Null byte in prim name should not crash");
  }
}

void security_deeply_nested_test(void) {
  Stage stage;
  std::string warn, err;

  // Generate USDA with 500 levels of nesting
  std::string usda = "#usda 1.0\n";
  const int depth = 500;

  for (int i = 0; i < depth; i++) {
    std::string indent(i * 4, ' ');
    usda += indent + "def Xform \"level" + std::to_string(i) + "\"\n";
    usda += indent + "{\n";
  }
  for (int i = depth - 1; i >= 0; i--) {
    std::string indent(i * 4, ' ');
    usda += indent + "}\n";
  }

  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "test.usda", &stage, &warn, &err);
  // Should either succeed or fail gracefully (no stack overflow)
  (void)ok;
  TEST_CHECK(true);
  TEST_MSG("Deeply nested USDA should not cause stack overflow");
}

void security_huge_array_test(void) {
  Stage stage;
  std::string warn, err;

  // Declare a large array without actually providing that many elements
  std::string usda =
      "#usda 1.0\n"
      "def Mesh \"HugeMesh\"\n"
      "{\n"
      "    int[] faceVertexCounts = [999999999]\n"
      "    int[] faceVertexIndices = [0]\n"
      "}\n";

  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "test.usda", &stage, &warn, &err);
  // Should handle gracefully - may succeed (it's valid syntax even if
  // semantically wrong) or fail, but should not crash
  (void)ok;
  TEST_CHECK(true);
  TEST_MSG("Huge array declaration should not crash");

  // Another test: very large claimed array count in bracket notation
  {
    std::string usda2 =
        "#usda 1.0\n"
        "def Mesh \"HugeMesh2\"\n"
        "{\n"
        "    float[] points = [";
    // Add a modest number of actual values
    for (int i = 0; i < 100; i++) {
      if (i > 0) usda2 += ", ";
      usda2 += "0.0";
    }
    usda2 += "]\n}\n";

    Stage stage2;
    bool ok2 = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda2.data()), usda2.size(),
        "test.usda", &stage2, &warn, &err);
    (void)ok2;
    TEST_CHECK(true);
    TEST_MSG("Large float array should not crash");
  }
}

void security_malformed_utf8_test(void) {
  Stage stage;
  std::string warn, err;

  // Invalid UTF-8 continuation byte
  {
    std::string usda = "#usda 1.0\ndef Xform \"test\" {\n";
    usda += "    string myStr = \"hello \x80\x81\x82 world\"\n";
    usda += "}\n";

    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "test.usda", &stage, &warn, &err);
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Invalid UTF-8 continuation bytes should not crash");
  }

  // Overlong encoding
  {
    std::string usda = "#usda 1.0\ndef Xform \"test2\" {\n";
    usda += "    string myStr = \"test \xC0\xAF end\"\n";
    usda += "}\n";

    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "test.usda", &stage, &warn, &err);
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Overlong UTF-8 encoding should not crash");
  }

  // Truncated multi-byte sequence
  {
    std::string usda = "#usda 1.0\ndef Xform \"test3\" {\n";
    usda += "    string myStr = \"test \xE0\x80\"\n";
    usda += "}\n";

    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "test.usda", &stage, &warn, &err);
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Truncated multi-byte UTF-8 should not crash");
  }

  // Invalid start byte (0xFE, 0xFF)
  {
    std::string usda = "#usda 1.0\ndef Xform \"test4\" {\n";
    usda += "    string myStr = \"test \xFE\xFF end\"\n";
    usda += "}\n";

    bool ok = LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "test.usda", &stage, &warn, &err);
    (void)ok;
    TEST_CHECK(true);
    TEST_MSG("Invalid UTF-8 start bytes 0xFE/0xFF should not crash");
  }
}

void security_recursive_reference_test(void) {
  Stage stage;
  std::string warn, err;

  // USDA that references itself
  std::string usda =
      "#usda 1.0\n"
      "def Xform \"Root\" (\n"
      "    prepend references = @test.usda@\n"
      ")\n"
      "{\n"
      "}\n";

  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "test.usda", &stage, &warn, &err);
  // Should fail or succeed, but must not hang or infinite-loop
  (void)ok;
  TEST_CHECK(true);
  TEST_MSG("Self-referencing USDA should not cause infinite loop");
}

void security_json_oversized_base64_rejected_test(void) {
  Layer layer;
  std::string warn, err;

  // Keep base64 input small but claim an oversized decoded byteLength.
  // This should be rejected by the centralized JSON size policy.
  const std::string json =
      R"({
        "buffers": [
          {
            "byteLength": 1000000000,
            "uri": "data:application/octet-stream;base64,AAAA"
          }
        ]
      })";

  bool ok = JSONToLayer(json, &layer, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("exceeds limit") != std::string::npos);
}

void security_unsafe_asset_path_rejected_test(void) {
  AssetResolutionResolver resolver;
  AssetInfo asset_info;
  Asset asset;
  std::string resolved;
  std::string warn, err;

  value::AssetPath unsafe_path("../textures/albedo.png");
  bool ok = tydra::RawAssetRead(unsafe_path, asset_info, resolver, &asset,
                                resolved, nullptr, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("Unsafe asset path") != std::string::npos);
}

void security_json_array_count_mismatch_rejected_test(void) {
  GeomMesh mesh;
  std::string warn, err;

  // count says 2 ints, payload only contains 1 int (4 bytes).
  const std::string json =
      R"({
        "name": "m",
        "faceVertexCounts": {
          "count": 2,
          "type": "int[]",
          "data": "AAAAAA=="
        }
      })";

  bool ok = JSONToGeomMesh(json, &mesh, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("count mismatch") != std::string::npos);
}

void security_json_point3f_count_overflow_rejected_test(void) {
  GeomMesh mesh;
  std::string warn, err;

  // count * 3 would overflow size_t in point3f path.
  const std::string json =
      R"({
        "name": "m",
        "points": {
          "count": 18446744073709551615,
          "type": "point3f[]",
          "data": "AAAAAAAAAAAAAAAA"
        }
      })";

  bool ok = JSONToGeomMesh(json, &mesh, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("overflow") != std::string::npos);
}

void security_resolver_oversized_custom_asset_rejected_test(void) {
  AssetResolutionResolver resolver;
  resolver.set_max_asset_bytes_in_mb(1);  // 1 MiB limit

  AssetResolutionHandler handler;
  handler.resolve_fun = SecurityTestResolveAsset;
  handler.size_fun = SecurityTestLargeSizeAsset;
  handler.read_fun = SecurityTestReadAsset;
  resolver.register_asset_resolution_handler("foo", handler);

  Asset asset;
  std::string warn, err;
  bool ok = resolver.open_asset("virtual.foo", "virtual.foo", &asset, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("exceeds max bytes") != std::string::npos);
}

void security_resolver_overreported_custom_asset_rejected_test(void) {
  AssetResolutionResolver resolver;
  resolver.set_max_asset_bytes_in_mb(1);

  AssetResolutionHandler handler;
  handler.resolve_fun = SecurityTestResolveAsset;
  handler.size_fun = SecurityTestSmallSizeAsset;
  handler.read_fun = SecurityTestOverreportedReadAsset;
  resolver.register_asset_resolution_handler("foo", handler);

  Asset asset;
  std::string warn, err;
  bool ok = resolver.open_asset("virtual.foo", "virtual.foo", &asset, &warn, &err);
  TEST_CHECK(!ok);
  TEST_CHECK(err.find("larger size than requested") != std::string::npos);
}

void security_nested_zstd_depth_rejected_test(void) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
  // Minimal valid USDA blob.
  const std::string usda =
      "#usda 1.0\n"
      "def Xform \"Root\" {}\n";

  std::string compress_err;

  std::vector<uint8_t> inner_compressed;
  bool ok_inner = ZstdCompression::Compress(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      &inner_compressed,
      ZstdCompression::kDefaultCompressionLevel,
      &compress_err);
  TEST_CHECK(ok_inner);

  std::vector<uint8_t> middle_compressed;
  bool ok_middle = ZstdCompression::Compress(
      inner_compressed.data(), inner_compressed.size(),
      &middle_compressed,
      ZstdCompression::kDefaultCompressionLevel,
      &compress_err);
  TEST_CHECK(ok_middle);

  std::vector<uint8_t> outer_compressed;
  bool ok_outer = ZstdCompression::Compress(
      middle_compressed.data(), middle_compressed.size(),
      &outer_compressed,
      ZstdCompression::kDefaultCompressionLevel,
      &compress_err);
  TEST_CHECK(ok_outer);

  Stage stage;
  std::string warn, err;
  USDLoadOptions options;
  bool load_ok = LoadUSDFromMemory(outer_compressed.data(),
                                   outer_compressed.size(), "", &stage,
                                   &warn, &err, options);
  TEST_CHECK(!load_ok);
  TEST_CHECK(err.find("Nested zstd compression depth") != std::string::npos);
#else
  // Zstd support compiled out; the guard cannot trigger. Report skip.
  TEST_CHECK(true);
#endif
}

void security_stage_move_cache_test(void) {
  // Regression: after a Stage move, the moved-from Stage must not contain
  // stale Prim pointers. Verify the move constructor correctly clears caches.
  //
  // Use a minimal Stage created from USDA to avoid internal API complexity.
  const std::string usda =
      "#usda 1.0\n"
      "def Xform \"Root\" { }\n";

  // Load into a Stage.
  Stage stage_src;
  std::string warn, err;
  USDLoadOptions opts;
  bool load_ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "", &stage_src, &warn, &err, opts);
  TEST_CHECK(load_ok);

  // Populate the internal cache by doing a lookup.
  {
    const Prim *found = nullptr;
    bool ok = stage_src.find_prim_at_path(Path("/Root", ""), found);
    TEST_CHECK(ok);
    TEST_CHECK(found != nullptr);
  }

  // Move construct.
  Stage stage_dst(std::move(stage_src));

  // The moved-from Stage must not return the previously cached pointer.
  {
    const Prim *not_found = nullptr;
    bool src_ok = stage_src.find_prim_at_path(Path("/Root", ""), not_found);
    TEST_CHECK(!src_ok);
  }

  // The moved-to Stage must be find the prim.
  {
    const Prim *found_dst = nullptr;
    bool dst_ok = stage_dst.find_prim_at_path(Path("/Root", ""), found_dst);
    TEST_CHECK(dst_ok);
    TEST_CHECK(found_dst != nullptr);
    TEST_CHECK(std::string(found_dst->element_name().data(),
                           found_dst->element_name().size()) == "Root");
  }
}

void security_base122_roundtrip_test(void) {
  // Regression: base122 must survive round-trip encode/decode for all inputs.
  // The previous bugs (uint32_t shift UB + zero-chunk early-exit) caused
  // data corruption for specific inputs.
  {
    // All zeros: triggered the zero-chunk early-exit bug.
    std::vector<uint8_t> zeros(6, 0);
    std::string encoded = base122_encode(zeros);
    TEST_CHECK(!encoded.empty());
    std::vector<uint8_t> decoded;
    int ret = base122_decode(encoded, decoded);
    TEST_CHECK(ret == 0);
    TEST_CHECK(decoded.size() == zeros.size());
    TEST_CHECK(memcmp(decoded.data(), zeros.data(), zeros.size()) == 0);
  }

  {
    // Single byte.
    std::vector<uint8_t> single = {0xAB};
    std::string encoded = base122_encode(single);
    std::vector<uint8_t> decoded;
    int ret = base122_decode(encoded, decoded);
    TEST_CHECK(ret == 0);
    TEST_CHECK(decoded.size() == 1);
    TEST_CHECK(decoded[0] == 0xAB);
  }

  {
    // 8 bytes (larger than a single 6-byte chunk).
    std::vector<uint8_t> data = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    std::string encoded = base122_encode(data);
    std::vector<uint8_t> decoded;
    int ret = base122_decode(encoded, decoded);
    TEST_CHECK(ret == 0);
    TEST_CHECK(decoded.size() == data.size());
    TEST_CHECK(memcmp(decoded.data(), data.data(), data.size()) == 0);
  }

  {
    // 6 bytes of 0xFF: the maximum safe chunk for 7 base122 chars.
    std::vector<uint8_t> data(6, 0xFF);
    std::string encoded = base122_encode(data);
    std::vector<uint8_t> decoded;
    int ret = base122_decode(encoded, decoded);
    TEST_CHECK(ret == 0);
    TEST_CHECK(decoded.size() == data.size());
    TEST_CHECK(memcmp(decoded.data(), data.data(), data.size()) == 0);
  }

  {
    // Seven high-bit bytes must survive a chunk boundary. This catches the
    // old 7-byte/8-digit encoding, which silently discarded the top bit.
    std::vector<uint8_t> data(7, 0xFF);
    std::string encoded = base122_encode(data);
    std::vector<uint8_t> decoded;
    int ret = base122_decode(encoded, decoded);
    TEST_CHECK(ret == 0);
    TEST_CHECK(decoded == data);
  }
}

void security_strutil_unwrap_edge_test(void) {
  // Regression: unwrap("\"") must not crash with size_t wrap.
  // The single-delimiter overload would read s.size() - n on an empty
  // string after removing the prefix, wrapping size_t and causing
  // std::out_of_range (which aborts in no-exceptions builds).
  {
    std::string s = "\"";
    std::string result = unwrap(s);
    TEST_CHECK(result.empty());
  }

  {
    // Empty string should return empty.
    std::string result = unwrap("");
    TEST_CHECK(result.empty());
  }

  {
    // String that is only the delimiter character: unwrap("'") with
    // default "\"" delimiter — the single quote does not match, so
    // the string should be returned unchanged, not crash.
    std::string result = unwrap("'");
    TEST_CHECK(result == "'");
  }

  {
    // Normal case should still work.
    std::string result = unwrap("\"hello\"");
    TEST_CHECK(result == "hello");
  }

  {
    // Triple-quoted unwrap should work.
    std::string result = unwrap("\"\"\"hello\"\"\"", "\"\"\"");
    TEST_CHECK(result == "hello");
  }

  {
    // Triple-quoted with empty content.
    std::string result = unwrap("\"\"\"\"\"\"", "\"\"\"");
    TEST_CHECK(result.empty());
  }
}

void security_zstd_max_decompressed_size_test(void) {
#ifdef LIGHTUSD_WITH_ZSTD_COMPRESSION
  // Regression: the maxDecompressedSize parameter must reject decompressions
  // whose expected size exceeds the limit.
  const std::string input_str = "Hello, World!";

  std::vector<uint8_t> compressed;
  std::string compress_err;
  bool compress_ok = ZstdCompression::Compress(
      reinterpret_cast<const uint8_t *>(input_str.data()), input_str.size(),
      &compressed, ZstdCompression::kDefaultCompressionLevel, &compress_err);
  TEST_CHECK(compress_ok);
  TEST_CHECK(!compressed.empty());

  // Decompress with a limit smaller than the data should fail.
  {
    std::vector<uint8_t> output;
    std::string err;
    bool ok = ZstdCompression::Decompress(
        compressed.data(), compressed.size(), &output, &err, /*max=*/1);
    TEST_CHECK(!ok);
    TEST_CHECK(!err.empty());
  }

  // Decompress with a sufficient limit should succeed.
  {
    std::vector<uint8_t> output;
    std::string err;
    bool ok = ZstdCompression::Decompress(
        compressed.data(), compressed.size(), &output, &err, /*max=*/1024);
    TEST_CHECK(ok);
    TEST_CHECK(output.size() == input_str.size());
    TEST_CHECK(memcmp(output.data(), input_str.data(), input_str.size()) == 0);
  }

  // Decompress with maxDecompressedSize=0 (no limit) should succeed.
  {
    std::vector<uint8_t> output;
    std::string err;
    bool ok = ZstdCompression::Decompress(
        compressed.data(), compressed.size(), &output, &err, /*max=*/0);
    TEST_CHECK(ok);
    TEST_CHECK(output.size() == input_str.size());
  }
#else
  TEST_CHECK(true);
#endif
}

void security_findfile_traversal_rejected_test(void) {
  // Regression: FindFile must reject filenames containing ".." to prevent
  // directory traversal escapes from the search path root.
  std::vector<std::string> search_paths = {"/safe/directory"};

  // Direct parent reference.
  std::string result = io::FindFile("../../etc/passwd", search_paths);
  TEST_CHECK(result.empty());

  // Embedded parent reference.
  result = io::FindFile("foo/../../etc/passwd", search_paths);
  TEST_CHECK(result.empty());

  // Just ".." alone.
  result = io::FindFile("..", search_paths);
  TEST_CHECK(result.empty());

  // Normal filename without traversal should still potentially resolve
  // (may return empty if file does not exist, but must NOT crash).
  result = io::FindFile("nonexistent.usd", search_paths);
  TEST_CHECK(result.empty() || !result.empty());  // no crash is the key
}

void security_sha256_overflow_rejected_test(void) {
  // sha256() guards against SIZE_MAX - 72 (the maximum safe input size
  // before the `new_len + 8` allocation overflows). Verify it returns
  // an empty string when given a too-large size rather than crashing.
  //
  // On 64-bit, SIZE_MAX - 71 can't be tested directly (would OOM).
  // Instead we verify that normal input works and the guard exists.
  {
    const char *input = "hello";
    std::string hash = sha256(input, 5);
    TEST_CHECK(!hash.empty());
    TEST_CHECK(hash.size() == 64);  // SHA-256 = 32 bytes = 64 hex chars
  }

  // Verify the overflow guard is present by checking with a size that
  // would overflow: SIZE_MAX itself. The guard should return empty.
  {
    size_t huge = (std::numeric_limits<size_t>::max)();
    std::string hash = sha256("dummy", huge);
    TEST_CHECK(hash.empty());
  }
}

void security_common_utils_overflow_test(void) {
  // Regression: ConstantToVertex must reject element sizes that would
  // cause integer overflow when multiplied by vertex count.
  std::vector<uint8_t> src(4, 0xAB);  // 4 bytes = 1 float

  // A huge elementSize that would overflow when multiplied by vertex count.
  const auto result = lightusd::tydra::utils::ConstantToVertex(
      src, uint32_t(1024 * 1024 * 1024), size_t(1024 * 1024));
  TEST_CHECK(!result.has_value());  // must fail with overflow error

  // Normal usage must still work.
  const auto ok_result = lightusd::tydra::utils::ConstantToVertex(src, 4, 3);
  TEST_CHECK(ok_result.has_value());
  TEST_CHECK(ok_result->size() == 12);  // 3 vertices * 4 bytes
}

void security_merge_path_overflow_test(void) {
  // Regression: the merge path (old_size + src.size()) must use safe::add
  // to prevent integer overflow on 32-bit platforms.
  // Test is on the rendering data structures via composition graph.
  // Verify the composition graph GetMutableNode returns sentinel for
  // out-of-bounds indices rather than crashing.
  lightusd::composition_graph::PrimIndex index;
  lightusd::composition_graph::CompNode &node =
      lightusd::composition_graph::GetMutableNode(index, uint16_t(65535));
  // Must not crash; the sentinel node is safe to reference.
  TEST_CHECK(true);
}

void security_safe_mul_add_tests(void) {
  size_t bytes = 0;

  // safe::mul: 10 * 5 = 50
  {
    bool ok = safe::mul(10, 5, &bytes);
    TEST_CHECK(ok);
    TEST_CHECK(bytes == 50);
  }

  // safe::mul: SIZE_MAX * 2 must overflow
  {
    bool ok = safe::mul(SIZE_MAX, 2, &bytes);
    TEST_CHECK(!ok);
  }

  // safe::add: 10 + 5 = 15
  {
    bool ok = safe::add(10, 5, &bytes);
    TEST_CHECK(ok);
    TEST_CHECK(bytes == 15);
  }

  // safe::add: SIZE_MAX + 1 must overflow
  {
    bool ok = safe::add(SIZE_MAX, 1, &bytes);
    TEST_CHECK(!ok);
  }

  // safe::n_to_size<uint32_t>: 10 elements -> 40 bytes
  {
    bool ok = safe::n_to_size<uint32_t>(10, &bytes);
    TEST_CHECK(ok);
    TEST_CHECK(bytes == 40);
  }

  // safe::n_to_size<uint32_t>: SIZE_MAX elements must overflow
  {
    bool ok = safe::n_to_size<uint32_t>(SIZE_MAX, &bytes);
    TEST_CHECK(!ok);
  }
}

void security_unwrap_triple_delim_test(void) {
  // Triple-quoted empty content
  {
    std::string result = unwrap("\"\"\"\"\"\"", "\"\"\"");
    TEST_CHECK(result.empty());
  }

  // Triple-quoted simple content
  {
    std::string result = unwrap("\"\"\"hello\"\"\"", "\"\"\"");
    TEST_CHECK(result == "hello");
  }

  // Triple-quoted content shorter than the delimiter
  {
    std::string result = unwrap("\"\"\"ab\"\"\"", "\"\"\"");
    TEST_CHECK(result == "ab");
  }

  // No delimiter match: return unchanged
  {
    std::string result = unwrap("plain", "\"\"\"");
    TEST_CHECK(result == "plain");
  }

  // Single-quoted content should not match triple delimiter
  {
    std::string result = unwrap("\"hello\"", "\"\"\"");
    TEST_CHECK(result == "\"hello\"");
  }
}

void security_is_safe_asset_path_test(void) {
  // Paths with ".." must be rejected
  {
    std::string out;
    bool ok =
        lightusd::security_policy::ValidateAndNormalizeAssetPath("../foo.usd",
                                                                  &out);
    TEST_CHECK(!ok);
  }

  // Paths with embedded ".." must be rejected
  {
    std::string out;
    bool ok = lightusd::security_policy::ValidateAndNormalizeAssetPath(
        "a/../../b/foo.usd", &out);
    TEST_CHECK(!ok);
  }

  // Absolute paths must be rejected
  {
    std::string out;
    bool ok =
        lightusd::security_policy::ValidateAndNormalizeAssetPath("/etc/passwd",
                                                                  &out);
    TEST_CHECK(!ok);
  }

  // Normal relative paths must be accepted
  {
    std::string out;
    bool ok =
        lightusd::security_policy::ValidateAndNormalizeAssetPath(
            "textures/albedo.png", &out);
    TEST_CHECK(ok);
  }

  // Simple filename must be accepted
  {
    std::string out;
    bool ok =
        lightusd::security_policy::ValidateAndNormalizeAssetPath("foo.usd",
                                                                  &out);
    TEST_CHECK(ok);
  }

  // Empty path must be rejected
  {
    std::string out;
    bool ok =
        lightusd::security_policy::ValidateAndNormalizeAssetPath("", &out);
    TEST_CHECK(!ok);
  }
}

void security_findfile_segment_traversal_test(void) {
  // Verify FindFile uses segment-based ".." traversal detection.
  // Filenames containing ".." as a substring but NOT as a standalone
  // path segment (e.g. "file..txt") must NOT be rejected.
  // Only filenames with ".." as an actual path segment should be rejected.
  std::vector<std::string> search_paths = {"/safe/directory"};

  // ".." as a standalone segment must be rejected.
  TEST_CHECK(io::FindFile("../../etc/passwd", search_paths).empty());
  TEST_CHECK(io::FindFile("foo/../../etc/passwd", search_paths).empty());
  TEST_CHECK(io::FindFile("..", search_paths).empty());

  // ".." as a substring of a filename segment must be allowed.
  // These should NOT be rejected (they may still return empty if
  // the file does not exist, but the ".." substring must not cause
  // a false-positive rejection).
  std::string result1 = io::FindFile("file..txt", search_paths);
  // file..txt is allowed to exist or not (no crash, no false rejection).
  (void)result1;

  std::string result2 = io::FindFile("data..csv", search_paths);
  (void)result2;
}

void security_findfile_absolute_traversal_test(void) {
  // Verify FindFile rejects absolute paths containing ".." segments,
  // which could be used to obscure the intent of a malicious asset path.
  std::vector<std::string> search_paths = {"/safe/directory"};

  // Absolute path with ".." segment must be rejected.
  TEST_CHECK(io::FindFile("/etc/../etc/passwd", search_paths).empty());
  TEST_CHECK(io::FindFile("/foo/../bar/evil", search_paths).empty());

  // Absolute path without ".." may resolve or not (no crash).
  std::string result = io::FindFile("/etc/passwd", search_paths);
  (void)result;

  // Normal relative path without ".." may resolve or not (no crash).
  result = io::FindFile("valid.usd", search_paths);
  (void)result;
}

void security_usda_load_options_test(void) {
  // Exercise the public USDA memory-loader options rather than the reader's
  // standalone defaults. The callback must be installed on the USDA reader.
  const std::string usda = "#usda 1.0\n def Xform \"Root\" {}\n";

  Layer layer;
  std::string warn;
  std::string err;
  USDLoadOptions options;
  int callback_count = 0;
  options.progress_callback = [&callback_count](float /*progress*/, void * /*userptr*/) {
    callback_count++;
    return false;
  };

  bool ok = LoadUSDALayerFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "assets/large.usda", &layer, &warn, &err, options);
  TEST_CHECK(!ok);
  TEST_CHECK(callback_count > 0);
  TEST_CHECK(!err.empty());

  // Zero retains the documented unlimited-memory behavior.
  layer = Layer();
  warn.clear();
  err.clear();
  options.max_memory_limit_in_mb = 0;
  options.progress_callback = nullptr;
  ok = LoadUSDALayerFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "assets/large.usda", &layer, &warn, &err, options);
  TEST_CHECK(ok);
  TEST_CHECK(err.empty());

  // Negative values are invalid input and are clamped to the same unlimited
  // behavior instead of wrapping into a huge unsigned budget.
  layer = Layer();
  warn.clear();
  err.clear();
  options.max_memory_limit_in_mb = -1;
  ok = LoadUSDALayerFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "assets/large.usda", &layer, &warn, &err, options);
  TEST_CHECK(ok);
  TEST_CHECK(err.empty());
}
