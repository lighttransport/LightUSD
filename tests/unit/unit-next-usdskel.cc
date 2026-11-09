#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-next-usdskel.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../../src/pcp/cache.hh"
#include "../../src/pcp/layer-registry.hh"
#include "resolver/asset-resolver.hh"
#include "schema/usd-skel.hh"
#include "stage/stage.hh"

// Regression tests for the `next` UsdSkel readers. Each of these guarded a bug
// that failed SILENTLY: the data came back empty (or as an identity transform),
// so a skinned scene simply rendered its rest pose with no warning and no error.
//
// A Skeleton + SkelAnimation with:
//   - bind/rest transforms authored PLAIN on the Skeleton (`bindTransforms`),
//     as matrix4d[] (doubles) -- the `primvars:skel:` prefix belongs on the
//     skinned MESH, not the Skeleton;
//   - a token[] `joints` ARRAY on the SkelAnimation;
//   - time-sampled quatf[] rotations, authored real-first (w, x, y, z).
namespace {

constexpr const char *kSkelUsda = R"(#usda 1.0
(
    defaultPrim = "Rig"
    startTimeCode = 1
    endTimeCode = 2
    timeCodesPerSecond = 24
)

def SkelRoot "Rig"
{
    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root", "Root/Bone1"]
        uniform matrix4d[] bindTransforms = [
            ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
            ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 2, 0, 1) )
        ]
        uniform matrix4d[] restTransforms = [
            ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) ),
            ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 2, 0, 1) )
        ]
        rel skel:animationSource = </Rig/Skel/Anim>

        def SkelAnimation "Anim"
        {
            uniform token[] joints = ["Root", "Root/Bone1"]
            quatf[] rotations.timeSamples = {
                1: [(1, 0, 0, 0), (1, 0, 0, 0)],
                2: [(1, 0, 0, 0), (0.70710677, 0.70710677, 0, 0)],
            }
            float3[] translations = [(0, 0, 0), (0, 2, 0)]
            half3[] scales = [(1, 1, 1), (1, 1, 1)]
        }
    }
}
)";

// Compose `kSkelUsda` into a next Stage (hermetic: parsed straight from memory).
bool BuildSkelStage(lightusd::Stage *stage) {
  std::string warn, err;
  const std::string src(kSkelUsda);
  std::shared_ptr<lightusd::Layer> layer =
      lightusd::pcp::LoadLayerFromMemory(
          "skel.usda", reinterpret_cast<const uint8_t *>(src.data()), src.size(),
          &warn, &err);
  if (!layer) {
    TEST_MSG("LoadLayerFromMemory failed: %s", err.c_str());
    return false;
  }
  lightusd::AssetResolver resolver;
  auto cache = lightusd::pcp::Cache::Open(resolver, layer);
  if (!cache) {
    TEST_MSG("Cache::Open failed: %s", cache.error().c_str());
    return false;
  }
  if (!cache->BuildStage(stage, &warn, &err)) {
    TEST_MSG("BuildStage failed: %s", err.c_str());
    return false;
  }
  return true;
}

}  // namespace

// A Skeleton's bindTransforms / restTransforms are authored PLAIN (not under the
// `primvars:skel:` prefix) and are matrix4d[], i.e. DOUBLES. Reading only the
// prefixed name -- or asking for a float array -- yields nothing, and an empty
// bind pose silently becomes the IDENTITY, which skews every skinning matrix.
void test_next_skel_bind_rest_transforms(void) {
  lightusd::Stage stage;
  if (!BuildSkelStage(&stage)) {
    TEST_CHECK(false);
    return;
  }
  lightusd::UsdPrim skel = stage.GetPrimAtPath("/Rig/Skel");
  TEST_CHECK(skel.IsValid());

  lightusd::SkeletonData data;
  TEST_CHECK(lightusd::GetSkeletonData(stage, skel, &data));

  TEST_CHECK(data.joints.size() == 2);

  // 16 doubles per joint. Empty here == identity bind pose == wrong skinning.
  TEST_CHECK_(data.bindTransforms.size() == 32,
              "bindTransforms: got %zu scalars, expected 32 (2 joints x 16). "
              "Empty means they were read under the wrong name (primvars:skel:) "
              "or as floats instead of matrix4d[] doubles.",
              data.bindTransforms.size());
  TEST_CHECK_(data.restTransforms.size() == 32,
              "restTransforms: got %zu scalars, expected 32",
              data.restTransforms.size());
  if (data.bindTransforms.size() == 32) {
    // Bone1's bind translation row (row 3) is (0, 2, 0).
    TEST_CHECK(std::fabs(data.bindTransforms[16 + 13] - 2.0) < 1e-9);
  }

  TEST_CHECK(data.hasAnimationSource);
  TEST_CHECK(data.animationSource == "/Rig/Skel/Anim");
}

// The SkelAnimation's `joints` is a token[] ARRAY. Reading it with the scalar
// token accessors leaves it EMPTY, which drops the entire animation (every joint
// keeps its rest transform, so the rig renders unposed at every time code).
void test_next_skelanim_joints_is_an_array(void) {
  lightusd::Stage stage;
  if (!BuildSkelStage(&stage)) {
    TEST_CHECK(false);
    return;
  }
  lightusd::UsdPrim anim = stage.GetPrimAtPath("/Rig/Skel/Anim");
  TEST_CHECK(anim.IsValid());

  lightusd::SkelAnimationData data;
  TEST_CHECK(lightusd::GetSkelAnimationData(stage, anim, &data, 1.0));

  TEST_CHECK_(data.joints.size() == 2,
              "SkelAnimation joints: got %zu, expected 2. Empty means the "
              "token[] ARRAY was read with a scalar accessor -- which silently "
              "drops the whole animation.",
              data.joints.size());
  if (data.joints.size() == 2) {
    TEST_CHECK(data.joints[0] == "Root");
    TEST_CHECK(data.joints[1] == "Root/Bone1");
  }
}

// next's canonical quaternion layout is REAL-FIRST (w, x, y, z): the crate reader
// swizzles disk's imaginary-first order into it, and USDA parses in authored
// order. A consumer that unpacks the flat array as (x, y, z, w) builds a garbage
// quaternion. Sanity anchor: the IDENTITY quat reads as (1,0,0,0), NOT (0,0,0,1).
void test_next_skelanim_rotations_are_real_first(void) {
  lightusd::Stage stage;
  if (!BuildSkelStage(&stage)) {
    TEST_CHECK(false);
    return;
  }
  lightusd::UsdPrim anim = stage.GetPrimAtPath("/Rig/Skel/Anim");
  TEST_CHECK(anim.IsValid());

  // t=1: both joints identity.
  {
    lightusd::SkelAnimationData d;
    TEST_CHECK(lightusd::GetSkelAnimationData(stage, anim, &d, 1.0));
    TEST_CHECK(d.hasRotations);
    TEST_CHECK(d.rotations.size() == 8);  // 2 joints x 4 floats
    if (d.rotations.size() == 8) {
      // Identity is (w=1, x=0, y=0, z=0). Under an (x,y,z,w) reading this would
      // be (0,0,0,1) -- i.e. rotations[0] would be 0.
      TEST_CHECK_(std::fabs(d.rotations[0] - 1.0f) < 1e-5f,
                  "identity quat: rotations[0] = %f, expected 1.0 (REAL-first). "
                  "0.0 here means the layout is being read imaginary-first.",
                  d.rotations[0]);
      TEST_CHECK(std::fabs(d.rotations[3] - 0.0f) < 1e-5f);
    }
  }

  // t=2: Bone1 is a 90-degree rotation about X = (w, x, y, z) = (.707, .707, 0, 0).
  // The time sample must also actually be evaluated at `time`.
  {
    lightusd::SkelAnimationData d;
    TEST_CHECK(lightusd::GetSkelAnimationData(stage, anim, &d, 2.0));
    TEST_CHECK(d.rotations.size() == 8);
    if (d.rotations.size() == 8) {
      const float w = d.rotations[4], x = d.rotations[5];
      const float y = d.rotations[6], z = d.rotations[7];
      TEST_CHECK_(std::fabs(w - 0.70710677f) < 1e-4f &&
                      std::fabs(x - 0.70710677f) < 1e-4f &&
                      std::fabs(y) < 1e-5f && std::fabs(z) < 1e-5f,
                  "Bone1 rotation at t=2: got (%f, %f, %f, %f), expected "
                  "(0.707, 0.707, 0, 0) real-first",
                  w, x, y, z);
    }
  }
}
