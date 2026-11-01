#include "../src/terms.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using hog::globe::terms::mantle;

TEST_CASE("loose cover lies its depth on even ground, pools in hollows and leaves knobs bare",
          "[globe][mantle]") {
    CHECK_THAT(mantle(0.0, 0.0, 1.5, 1.0), WithinAbs(1.5, 1e-12));
    // A hollow two metres under the smooth surface fills to the cover's top.
    CHECK_THAT(mantle(-2.0, 0.0, 1.5, 1.0), WithinAbs(3.5, 1e-12));
    // A knob standing over the cover's top gets none.
    CHECK(mantle(3.0, 0.0, 1.5, 1.0) == 0.0);
}

TEST_CASE("the cover feathers out against the rock, never jumping", "[globe][mantle]") {
    double last = mantle(-3.0, 0.0, 1.5, 1.0);
    for (int step = 0; step <= 600; ++step) {
        const double rock = -3.0 + (0.01 * step);
        const double here = mantle(rock, 0.0, 1.5, 1.0);
        CHECK(here <= last + 1e-9);
        CHECK(std::abs(here - last) < 0.02);
        last = here;
    }
}

TEST_CASE("steeper ground holds less of its cover, and none without any", "[globe][mantle]") {
    CHECK_THAT(mantle(-1.0, 0.0, 1.5, 0.5), WithinAbs(1.25, 1e-12));
    CHECK(mantle(-1.0, 0.0, 1.5, 0.0) == 0.0);
    CHECK(mantle(-1.0, 0.0, 0.0, 1.0) == 0.0);
    // Cover thinner than the width it feathers over still lies its depth on even ground.
    CHECK_THAT(mantle(0.0, 0.0, 0.1, 1.0), WithinAbs(0.1, 1e-12));
}
