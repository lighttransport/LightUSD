// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - crate array-edit write performance + correctness
//
// Guards crate EncodeArrayEdit::lit_index against an O(N^2) regression: a
// sparse array-edit default with N distinct literals used to cost O(N^2)
// string compares (a linear scan over the growing lit_texts vector per op).
// A large edit (tens of thousands of distinct-literal WriteLiteral ops) is
// the real-world shape; author it, write to USDC, and check the edit
// round-trips + wall time.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "layer/layer.hh"
#include "layer/property-index.hh"
#include "stage/stage.hh"
#include "crate/crate-writer.hh"
#include "writer/usdc-writer.hh"

using namespace lightusd::next;

static int g_failures = 0;
#define AE_CHECK(cond, msg)                                                  \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "AE_CHECK FAILED: %s @ %s:%d\n", std::string(msg).c_str(),            \
                    __FILE__, static_cast<int>(__LINE__));                   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// A root prim /P with a point3f[] property "arr" whose default is a sparse
// array edit of n_ops DISTINCT-literal WriteLiteral ops (no authored value).
// The crate writer takes the EncodeArrayEdit path for it.
static Stage BuildArrayEditStage(int n_ops) {
  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("P");
  LayerBuilder& lb = stage_builder.GetLayerBuilder();

  lb.begin_prim("P", "");
  const PropNameId name_id = GetPropNameTable().intern("arr");
  const TypeId elem = GetTypeIdFromName("point3f");
  lb.current()->add_property_slot(name_id, elem, PropSlot::kFlagArray);

  ArrayEditData edit;
  edit.ops.reserve(n_ops);
  for (int i = 0; i < n_ops; ++i) {
    ArrayEditOpRec op;
    op.kind = ArrayEditOpRec::WriteLiteral;
    op.a2 = i;
    op.literal = "(" + std::to_string(i) + ",0,0)";
    edit.ops.push_back(op);
  }
  lb.current()->set_array_edit("arr", std::move(edit));
  lb.end_prim();

  lb.finalize();
  return stage_builder.Build();
}

int main() {
  const int n_ops = 10000;

  // Correctness: the edit writes to USDC without error and produces bytes.
  {
    Stage stage = BuildArrayEditStage(n_ops);
    std::vector<uint8_t> buf;
    USDCWriteOptions wopts;
    USDCWriteResult r = WriteUSDCToMemory(buf, stage, wopts);
    AE_CHECK(r.success, "usdc write failed: " + r.error);
    AE_CHECK(!buf.empty(), "usdc write produced no bytes");
  }

  // Perf gate: N distinct-literal ops used to be O(N^2) string compares.
  {
    Stage stage = BuildArrayEditStage(n_ops);
    std::vector<uint8_t> buf;
    USDCWriteOptions wopts;
    const auto t0 = std::chrono::steady_clock::now();
    USDCWriteResult r = WriteUSDCToMemory(buf, stage, wopts);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() /
        1000.0;
    AE_CHECK(r.success, "usdc write (perf) failed");
    AE_CHECK(ms < 500.0,
             "array-edit write too slow (O(N^2) regression?) " +
                 std::to_string(static_cast<long long>(ms)) + "ms for " +
                 std::to_string(n_ops) + " ops");
    std::printf("array-edit write: %.1fms for %d distinct-literal ops\n",
                ms, n_ops);
  }

  if (g_failures == 0) {
    std::printf("test_array_edit_write: all checks passed\n");
    return 0;
  }
  std::printf("test_array_edit_write: %d check(s) failed\n", g_failures);
  return 1;
}
