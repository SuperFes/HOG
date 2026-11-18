#include "../src/formed.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/habitat.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using hog::DVec3;

namespace {

// Directions spread nearly evenly over the planet.
std::vector<DVec3> spread(std::uint64_t count) {
    std::vector<DVec3> found;
    found.reserve(count);
    for (std::uint64_t n = 0; n < count; ++n) {
        found.push_back(
            hog::globe::center(hog::globe::cell_at_index(7, (n * 2'654'435'761U) % 98'304U)));
    }
    return found;
}

} // namespace

TEST_CASE("a place's climate warms and cools by the lapse rate with the ground's height",
          "[globe][habitat]") {
    const auto& world = hog::testing::small_world();
    const hog::globe::Formed& formed = *world.shape().formed;
    const double lapse = formed.world().body.lapse_rate;
    for (const DVec3& up : spread(20)) {
        const auto low = formed.climate_at(up, 100.0);
        const auto high = formed.climate_at(up, 600.0);
        CHECK(low.temperature - high.temperature == Catch::Approx(500.0 * lapse));
        CHECK(low.rain == high.rain);
        CHECK(low.rain >= 0.0);
        // Under the sea, as at its level.
        CHECK(formed.climate_at(up, -50.0).temperature == formed.climate_at(up, 0.0).temperature);
    }
}

TEST_CASE("a habitat is the place's own: its warmth at its height, its water, its ground",
          "[globe][habitat]") {
    const auto& world = hog::testing::small_world();
    const hog::globe::Formed& formed = *world.shape().formed;
    const double lod = hog::globe::voxel_lod(world);
    std::vector<std::pair<double, double>> land; // height, warmth
    int seas = 0;
    for (const DVec3& up : spread(300)) {
        const hog::flora::Habitat here = hog::globe::habitat(world, up);
        const hog::globe::Formed::Here formed_here = formed.at(up, lod);
        CAPTURE(up.x, up.y, up.z);
        CHECK(here.temperature ==
              Catch::Approx(formed.climate_at(up, formed_here.top.value).temperature));
        CHECK(here.swing >= 0.0);
        CHECK(here.rain >= 0.0);
        CHECK(here.over_drainage >= 0.0);
        CHECK(here.soil >= 0.0);
        CHECK(here.bare >= 0.0);
        CHECK(here.bare <= 1.0);
        CHECK(here.slope >= 0.0);
        CHECK(here.ice >= 0.0);
        CHECK(here.water >= 0.0);
        if (here.sea) {
            ++seas;
            CHECK(formed_here.top.value < 0.0);
            CHECK(here.water == Catch::Approx(-formed_here.top.value));
        } else if (here.water <= 0.0) {
            land.emplace_back(formed_here.top.value, here.temperature);
        }
    }
    CHECK(seas > 0);
    // Higher ground is colder.
    REQUIRE(land.size() >= 40);
    std::ranges::sort(land);
    const std::size_t quarter = land.size() / 4;
    double low = 0.0;
    double high = 0.0;
    for (std::size_t n = 0; n < quarter; ++n) {
        low += land[n].second / static_cast<double>(quarter);
        high += land[land.size() - 1 - n].second / static_cast<double>(quarter);
    }
    CHECK(high < low);
}

TEST_CASE("the water under the ground lies near in lakes' basins and far down on high ground",
          "[globe][habitat]") {
    const auto& world = hog::testing::small_world();
    const hog::globe::Formed& formed = *world.shape().formed;
    const std::vector<hog::globe::Formed::LakeAt> lakes = formed.lakes();
    REQUIRE_FALSE(lakes.empty());
    int wet = 0;
    for (const hog::globe::Formed::LakeAt& lake : lakes) {
        const hog::flora::Habitat here = hog::globe::habitat(world, lake.in);
        if (here.water > 0.0) {
            ++wet;
            CHECK_FALSE(here.sea);
            CHECK(here.over_drainage == 0.0);
        }
    }
    CHECK(wet > 0);
    // The highest of many places stands well over the water.
    const double lod = hog::globe::voxel_lod(world);
    DVec3 highest{0.0};
    double tallest = -1e30;
    for (const DVec3& up : spread(300)) {
        if (const double top = formed.at(up, lod).top.value; top > tallest) {
            tallest = top;
            highest = up;
        }
    }
    CHECK(hog::globe::habitat(world, highest).over_drainage > 10.0);
}
