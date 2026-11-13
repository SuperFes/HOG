#include "../src/valleys.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::globe::Lowered;
using hog::globe::Lowest;
using hog::globe::Valley;
using hog::globe::Wall;
using hog::globe::wall_at;

namespace {

constexpr double radius = 30'000.0;

// A kilometre-long valley running east along the equator, falling from 100 m to 80 m, its walls
// rising one in two.
Valley falling() {
    return {.from = {1.0, 0.0, 0.0},
            .to = glm::normalize(DVec3{1.0, 1'000.0 / radius, 0.0}),
            .floor_from = 100.0,
            .floor_to = 80.0,
            .wall = 0.5};
}

DVec3 at(double east, double north) {
    return glm::normalize(DVec3{1.0, east / radius, north / radius});
}

} // namespace

TEST_CASE("a valley's floor falls along it between its ends", "[globe][valleys]") {
    CHECK_THAT(wall_at(falling(), at(0.0, 0.0), radius).height, WithinAbs(100.0, 1e-6));
    CHECK_THAT(wall_at(falling(), at(500.0, 0.0), radius).height, WithinAbs(90.0, 0.01));
    CHECK_THAT(wall_at(falling(), at(1'000.0, 0.0), radius).height, WithinAbs(80.0, 1e-6));
}

TEST_CASE("a valley's walls rise from its floor as steeply as it's given", "[globe][valleys]") {
    CHECK_THAT(wall_at(falling(), at(500.0, 100.0), radius).height, WithinAbs(140.0, 0.05));
    CHECK_THAT(wall_at(falling(), at(500.0, -200.0), radius).height, WithinAbs(190.0, 0.05));
    // Past its end, up from the end's floor.
    CHECK_THAT(wall_at(falling(), at(1'300.0, 0.0), radius).height, WithinAbs(230.0, 0.5));
}

TEST_CASE("a valley's walls reach only so far, rising smoothly out of the way past half of it",
          "[globe][valleys]") {
    Valley near = falling();
    near.reach = 200.0;
    CHECK_THAT(wall_at(near, at(500.0, 50.0), radius).height, WithinAbs(115.0, 0.05));
    CHECK(wall_at(near, at(500.0, 200.0), radius).height > 1e4);
    // Without a step anywhere on the way.
    double last = wall_at(near, at(500.0, 0.0), radius).height;
    for (int metres = 1; metres <= 250; ++metres) {
        const double height = wall_at(near, at(500.0, metres), radius).height;
        CHECK(height >= last);
        CHECK(height - last < 2'000.0);
        last = height;
    }
}

TEST_CASE("a valley's walls rise away from it, and its floor falls along it", "[globe][valleys]") {
    const DVec3 north = wall_at(falling(), at(500.0, 100.0), radius).gradient;
    CHECK_THAT(north.z, WithinRel(0.5, 1e-3));
    CHECK_THAT(north.y, WithinAbs(-0.02, 1e-3));
    const DVec3 south = wall_at(falling(), at(500.0, -100.0), radius).gradient;
    CHECK_THAT(south.z, WithinRel(-0.5, 1e-3));
}

TEST_CASE("a valley's floor lies level across its middle, however narrow beside its channel",
          "[globe][valleys]") {
    // A channel wider than the floor it wanders across.
    Valley narrow = falling();
    narrow.belt = 2.0;
    narrow.channel_width = 6.0;
    for (const double north : {-0.01, 0.01}) {
        const auto wall = wall_at(narrow, at(500.0, north), radius);
        // Neither the ground nor the way water runs off it turns about sharply across the middle.
        CHECK(std::abs(wall.fall.z) < 0.01);
        CHECK(std::abs(wall.gradient.z) < 0.01);
    }
}

TEST_CASE("a valley's river stands in its channel, deepening along it to the next one's",
          "[globe][valleys]") {
    // A straight channel 20 m across, 2 m deep at the head and 3 m at the foot, its water 1 m and
    // 3 m deep: brimming at the foot, as where it meets the sea.
    Valley river = falling();
    river.belt = 10.0;
    river.channel_width = 20.0;
    river.channel_depth = 2.0;
    river.channel_depth_to = 3.0;
    river.water_depth = 1.0;
    river.water_depth_to = 3.0;
    river.length = 1'000.0;
    river.meander_length = 220.0;
    for (const double east : {0.0, 250.0, 500.0, 750.0, 1'000.0}) {
        CAPTURE(east);
        const double t = east / 1'000.0;
        const auto middle = wall_at(river, at(east, 0.0), radius);
        const double floor = 100.0 - (20.0 * t);
        CHECK_THAT(middle.bank, WithinAbs(floor, 0.01));
        CHECK_THAT(middle.floor, WithinAbs(floor - 2.0 - t, 0.01));
        CHECK_THAT(middle.water, WithinAbs(floor - 2.0 - t + 1.0 + (2.0 * t), 0.01));
        // Over the bank the ground stands over the water.
        CHECK(wall_at(river, at(east, 12.0), radius).standing > middle.water);
    }
    CHECK_THAT(wall_at(river, at(1'000.0, 0.0), radius).water, WithinAbs(80.0, 0.01));
}

TEST_CASE("ways the ground could stand that run alike merge without digging under either",
          "[globe][valleys]") {
    Lowered lowered{50.0, 50.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    lowered.add({.value = 100.5, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    CHECK_THAT(lowered.lowest().value, WithinAbs(100.0, 1e-9));
}

TEST_CASE("walls meeting head on round over as much height as they part across the rounding, "
          "never more than the blend",
          "[globe][valleys]") {
    Lowered lowered{40.0, 40.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    lowered.add({.value = 100.0, .gradient = {-0.5, 0.0, 0.0}}, 1.0, 0.0);
    CHECK_THAT(lowered.lowest().value, WithinAbs(90.0, 1e-9));
    // A channel's banks part far faster than walls; they're no crest to round over more.
    Lowered banks{40.0, 40.0};
    banks.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    banks.add({.value = 100.0, .gradient = {-9.5, 0.0, 0.0}}, 1.0, 0.0);
    CHECK_THAT(banks.lowest().value, WithinAbs(90.0, 1e-9));
}

TEST_CASE("many walls meeting round the lowest no more than the one it meets most",
          "[globe][valleys]") {
    Lowered lowered{40.0, 40.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    for (int k = 0; k < 12; ++k) {
        lowered.add({.value = 100.0 + (0.1 * k), .gradient = {-0.5, 0.0, 0.0}}, 1.0, 0.0);
    }
    CHECK_THAT(lowered.lowest().value, WithinAbs(90.0, 1e-9));
}

TEST_CASE("rounding a crest never sinks it past halfway down to its floor", "[globe][valleys]") {
    Lowered lowered{40.0, 40.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 96.0);
    lowered.add({.value = 100.0, .gradient = {-0.5, 0.0, 0.0}}, 1.0, 96.0);
    CHECK_THAT(lowered.lowest().value, WithinAbs(98.0, 1e-9));
    // On its floor, nothing rounds it lower.
    Lowered floor{40.0, 40.0};
    floor.add({.value = 96.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 96.0);
    floor.add({.value = 96.0, .gradient = {-0.5, 0.0, 0.0}}, 1.0, 96.0);
    CHECK_THAT(floor.lowest().value, WithinAbs(96.0, 1e-9));
}

TEST_CASE("a winding valley's ground stands over the floor of the stretch it stands as, handed "
          "over between them smoothly",
          "[globe][valleys]") {
    // Two stretches whose walls meet at a spur at 0, their floors 20 m apart.
    const auto stretch = [](double x, double rising, double floor) {
        Wall wall;
        wall.standing = 100.0 + (rising * x);
        wall.standing_gradient = {rising, 0.0, 0.0};
        wall.floor = floor;
        wall.steep = 1.0;
        return wall;
    };
    const auto lowered = [&](double x) {
        Lowest lowest{40.0, 40.0};
        lowest.add(stretch(x, 0.5, 50.0));
        lowest.add(stretch(x, -0.5, 30.0));
        Wall wall;
        lowest.lower(wall);
        return wall;
    };
    CHECK_THAT(lowered(-60.0).floor, WithinAbs(50.0, 1e-9));
    CHECK_THAT(lowered(60.0).floor, WithinAbs(30.0, 1e-9));
    double last = lowered(-60.0).standing - lowered(-60.0).floor;
    for (int x = -59; x <= 60; ++x) {
        const Wall wall = lowered(x);
        const double above = wall.standing - wall.floor;
        CHECK(above > 0.0);
        CHECK(std::abs(above - last) < 1.0);
        last = above;
    }
}

TEST_CASE("where one stretch's floor meets another's wall, the floor the ground stands over hands "
          "over without a step",
          "[globe][valleys]") {
    const auto lowered = [](double x) {
        Wall flat;
        flat.standing = 100.0;
        flat.floor = 100.0;
        Wall wall;
        wall.standing = 80.0 + (0.5 * x);
        wall.standing_gradient = {0.5, 0.0, 0.0};
        wall.floor = 60.0;
        wall.steep = 1.0;
        Lowest lowest{40.0, 40.0};
        lowest.add(flat);
        lowest.add(wall);
        Wall out;
        lowest.lower(out);
        return out;
    };
    CHECK_THAT(lowered(-100.0).floor, WithinAbs(60.0, 1e-9));
    CHECK_THAT(lowered(200.0).floor, WithinAbs(100.0, 1e-9));
    double last = lowered(-100.0).floor;
    for (int x = -99; x <= 200; ++x) {
        const double floor = lowered(x).floor;
        CHECK(std::abs(floor - last) < 1.0);
        last = floor;
    }
}

TEST_CASE("a wall stands within its bounds anywhere in a cap", "[globe][valleys]") {
    using hog::globe::Interval;
    std::mt19937_64 random{5};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    double sampled = 0.0;
    double bounded = 0.0;
    for (int n = 0; n < 300; ++n) {
        Valley valley = falling();
        valley.to = glm::normalize(DVec3{1.0, (200.0 + (2'000.0 * unit(random))) / radius,
                                         (unit(random) - 0.5) * 1'000.0 / radius});
        valley.floor_to = 100.0 - (60.0 * unit(random));
        valley.reach = 200.0 + (600.0 * unit(random));
        valley.belt = 40.0 * unit(random);
        valley.channel_width = 12.0 * unit(random);
        valley.channel_depth = 3.0 * unit(random);
        valley.channel_depth_to = 3.0 * unit(random);
        valley.meander = valley.belt / 2.0;
        valley.meander_length = 11.0 * std::max(valley.channel_width, 1.0);
        valley.length = 3'000.0;
        valley.head = 0.2;
        valley.foot = 0.6;
        valley.seed = static_cast<std::uint64_t>(n);
        const Interval steepness{.low = 0.2, .high = 0.2 + (0.6 * unit(random))};
        const DVec3 middle = at((2'500.0 * unit(random)) - 250.0, (1'200.0 * unit(random)) - 600.0);
        const double within = (1.0 + (300.0 * unit(random) * unit(random))) / radius;
        const hog::globe::WallBounds bounds =
            hog::globe::wall_bounds(valley, middle, within, radius, steepness);
        double low = 1e300;
        double high = -1e300;
        for (int k = 0; k < 100; ++k) {
            const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, middle));
            const DVec3 north = glm::cross(middle, east);
            const double turn = 2.0 * std::numbers::pi * unit(random);
            const double off = within * std::sqrt(unit(random));
            const DVec3 point = glm::normalize(
                middle + (((east * std::cos(turn)) + (north * std::sin(turn))) * std::tan(off)));
            valley.wall = steepness.low + ((steepness.high - steepness.low) * unit(random));
            const Wall wall = wall_at(valley, point, radius);
            CHECK(bounds.standing.contains(wall.standing));
            CHECK(bounds.height.contains(wall.height));
            low = std::min(low, wall.standing);
            high = std::max(high, wall.standing);
        }
        sampled += high - low;
        bounded += bounds.standing.high - bounds.standing.low;
    }
    WARN("sampled span " << sampled / bounded << " of the bounds'");
}
