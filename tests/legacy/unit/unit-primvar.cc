#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-primvar.h"
#include "lightusd.hh"
#include "primvar.hh"
#include "value-pprint.hh"
#include "usdGeom.hh"

using namespace lightusd::value;

using namespace lightusd::legacy::value;  // NOLINT
using namespace lightusd::legacy::primvar;

void primvar_test(void) {

  // geom primvar
  {
    lightusd::legacy::GeomMesh mesh;
    std::vector<float> scalar_array = {1.0, 2.0, 3.0, 4.0};
    lightusd::legacy::Attribute attr;
    attr.set_value(std::move(scalar_array));
    lightusd::legacy::Property prop(attr, /* custom */false);

    mesh.props.emplace("primvars:myvar", prop);

    lightusd::legacy::GeomPrimvar primvar;
    TEST_CHECK(mesh.get_primvar("myvar", &primvar) == true);

    // Exercise the (thinned) GeomPrimvar::get_value<T> overloads end-to-end:
    // they forward to the non-template get_value(value::Value*) cores and cast.
    {
      std::string gerr;

      std::vector<float> out;
      TEST_CHECK(primvar.get_value(&out, &gerr));
      TEST_CHECK(out.size() == 4);
      if (out.size() == 4) {
        TEST_CHECK(out[0] == 1.0f);
        TEST_CHECK(out[3] == 4.0f);
      }

      // Time-sampled overload at default time (no timesamples -> default value).
      std::vector<float> out_t;
      TEST_CHECK(primvar.get_value(lightusd::legacy::value::TimeCode::Default(), &out_t,
                                   lightusd::legacy::value::TimeSampleInterpolationType::Held,
                                   &gerr));
      TEST_CHECK(out_t.size() == 4);
      if (out_t.size() == 4) {
        TEST_CHECK(out_t[2] == 3.0f);
      }

      // Type mismatch must fail cleanly (requested int[] from a float[] primvar).
      std::vector<int32_t> bad;
      TEST_CHECK(primvar.get_value(&bad) == false);

      // flatten_with_indices (kept its templated fast path) — control.
      std::vector<float> flat;
      TEST_CHECK(primvar.flatten_with_indices(&flat, &gerr));
      TEST_CHECK(flat.size() == 4);
      if (flat.size() == 4) {
        TEST_CHECK(flat[1] == 2.0f);
      }
    }

    // non-existing primvar
    TEST_CHECK(mesh.get_primvar("myvar0", &primvar) == false);

  }

}
