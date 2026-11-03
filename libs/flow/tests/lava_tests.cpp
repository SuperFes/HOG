#include <hog/flow/lava.hpp>
#include <hog/globe/material.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using hog::flow::cooled;
using hog::flow::is_molten;
using hog::flow::lava_mobility;
using hog::globe::Material;

TEST_CASE("lava thickens as it cools and sets stiff under its melting point", "[flow][lava]") {
    const double melts = hog::globe::properties(Material::Basalt).melts;
    CHECK(lava_mobility(Material::Basalt, melts + 100.0) == 1.0);
    CHECK(lava_mobility(Material::Basalt, melts) > lava_mobility(Material::Basalt, melts - 100.0));
    CHECK(lava_mobility(Material::Basalt, melts - 100.0) > 0.0);
    CHECK(lava_mobility(Material::Basalt, melts - 300.0) == 0.0);
    CHECK(lava_mobility(Material::Air, 2'000.0) == 0.0);
}

TEST_CASE("rock is molten from its melting point up", "[flow][lava]") {
    const double melts = hog::globe::properties(Material::Basalt).melts;
    CHECK(is_molten(Material::Basalt, melts));
    CHECK_FALSE(is_molten(Material::Basalt, melts - 1.0));
    CHECK_FALSE(is_molten(Material::Air, 2'000.0));
}

TEST_CASE("open lava cools towards the air, thin lava faster, buried lava not at all",
          "[flow][lava]") {
    const double hot = 1'500.0;
    const double after_hour = cooled(Material::Basalt, hot, 280.0, 1.0, 0.5, 3'600.0);
    CHECK(after_hour < hot);
    CHECK(after_hour > 280.0);
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.1, 3'600.0) < after_hour);
    CHECK(cooled(Material::Basalt, hot, 280.0, 0.0, 0.5, 3'600.0) == hot);
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.5, 0.0) == Catch::Approx(hot));
    // Never below the air.
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.1, 1e9) >= 280.0);
}
