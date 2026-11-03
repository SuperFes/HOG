#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/vents.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using hog::globe::Generator;
using hog::globe::Planet;

TEST_CASE("vents are scattered over dry land, the same each time", "[globe][vents]") {
    const Generator generator{Planet{}};
    const auto vents = hog::globe::vent_sites(generator);
    CHECK(!vents.empty());
    CHECK(vents.size() < 400);
    const double sea = generator.sea_level().value_or(0.0);
    for (const auto& vent : vents) {
        CHECK(vent.ground > sea);
        CHECK(std::abs(glm::length(vent.direction) - 1.0) < 1e-9);
    }
    const auto again = hog::globe::vent_sites(generator);
    REQUIRE(again.size() == vents.size());
    CHECK(again.front().key == vents.front().key);
}

TEST_CASE("some vents are pools on cave floors under the ground", "[globe][vents]") {
    const Generator generator{Planet{}};
    int pools = 0;
    for (const auto& vent : hog::globe::vent_sites(generator)) {
        if (!vent.pool) {
            continue;
        }
        ++pools;
        const auto ground = hog::globe::ground_below(generator, vent.direction, 0.5);
        CHECK(vent.ground < generator.column(vent.direction, 0.5).ground());
        // Open just over the floor, solid just under.
        CHECK(generator.column(vent.direction, 0.5).sample(vent.ground + 1.0).distance > 0.0);
        CHECK(generator.column(vent.direction, 0.5).sample(vent.ground - 0.5).distance < 0.0);
        (void)ground;
    }
    CHECK(pools > 0);
}

TEST_CASE("a planet that loses less heat has fewer vents", "[globe][vents]") {
    Planet cold;
    cold.heat_flow = 0.03;
    Planet hot;
    hot.heat_flow = 0.2;
    CHECK(hot.heat_flow > cold.heat_flow);
    CHECK(hog::globe::vent_sites(Generator{hot}).size() >
          hog::globe::vent_sites(Generator{cold}).size());
}
