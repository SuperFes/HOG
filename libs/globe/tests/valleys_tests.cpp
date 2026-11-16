#include "../src/valleys.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

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

TEST_CASE("a valley's water rises through the year as it does along it, at its own peak",
          "[globe][valleys]") {
    Valley river = falling();
    river.belt = 10.0;
    river.channel_width = 20.0;
    river.channel_depth = 2.0;
    river.channel_depth_to = 2.0;
    river.water_depth = 1.0;
    river.water_depth_to = 1.0;
    river.rise = 0.5;
    river.rise_to = 1.5;
    river.peak = 0.3;
    river.length = 1'000.0;
    for (const double east : {0.0, 500.0, 1'000.0}) {
        CAPTURE(east);
        const hog::globe::Season season = wall_at(river, at(east, 0.0), radius).season;
        CHECK_THAT(season.rise, WithinAbs(0.5 + (east / 1'000.0), 1e-3));
        CHECK(season.peak == 0.3);
    }
}

TEST_CASE("ways the ground could stand that run alike merge without digging under either",
          "[globe][valleys]") {
    Lowered lowered{50.0, 50.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    lowered.add({.value = 100.5, .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
    // But for the millimetres the soft turn between them takes.
    CHECK_THAT(lowered.lowest().value, WithinAbs(100.0, 0.005));
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
    // Turning softly between those meeting about as low, no lower than all thirteen as low.
    const double lowest = lowered.lowest().value;
    CHECK(lowest <= 90.0);
    CHECK(lowest > 90.0 - (hog::globe::lowest_softness * std::log(13.0)));
}

TEST_CASE("the lowest turns smoothly where another way comes lowest, so a crest rounded from it "
          "never steps",
          "[globe][valleys]") {
    // A rounded crest handing over to a plane below it, then that lowest rounded again against
    // a wall it meets: as wide as their gradients part, so a turn the first told as a kink would
    // step the second.
    const auto inner = [](double x) {
        Lowered lowered{40.0, 40.0};
        lowered.add({.value = 100.0 + (0.5 * x), .gradient = {0.5, 0.0, 0.0}}, 1.0, 0.0);
        lowered.add({.value = 100.0 - (0.5 * x), .gradient = {-0.5, 0.0, 0.0}}, 1.0, 0.0);
        lowered.add({.value = 93.0 + (0.05 * x), .gradient = {0.05, 0.0, 0.0}}, 1.0, 0.0);
        return lowered.lowest();
    };
    const auto outer = [&](double x) {
        Lowered lowered{40.0, 40.0};
        lowered.add(inner(x), 1.0, 0.0);
        lowered.add({.value = 100.0, .gradient = {0.0, 0.8, 0.0}}, 1.0, 0.0);
        return lowered.lowest().value;
    };
    constexpr double step = 0.01;
    double turning = 0.0;
    double stepping = 0.0;
    for (int k = -3'000; k < 3'000; ++k) {
        const double x = k * step;
        turning = std::max(turning, glm::length(inner(x + step).gradient - inner(x).gradient));
        stepping = std::max(stepping, std::abs(outer(x + step) - outer(x)));
    }
    CHECK(turning < 0.02);
    CHECK(stepping < 0.05);
}

TEST_CASE("the lowest rises the way it says, crests rounded and all", "[globe][valleys]") {
    std::mt19937_64 random{11};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    double worst = 0.0;
    for (int n = 0; n < 500; ++n) {
        // A few planes crossing about here, far over their floors.
        struct Plane {
            double height;
            DVec3 rise;
        };
        std::vector<Plane> planes;
        const int count = 2 + static_cast<int>(4.0 * unit(random));
        planes.reserve(static_cast<std::size_t>(count));
        for (int k = 0; k < count; ++k) {
            planes.push_back(
                {.height = 100.0 + (20.0 * (unit(random) - 0.5)),
                 .rise = {2.0 * (unit(random) - 0.5), 2.0 * (unit(random) - 0.5), 0.0}});
        }
        const auto lowest = [&](double x, double y) {
            Lowered lowered{20.0, 30.0};
            for (const Plane& plane : planes) {
                lowered.add({.value = plane.height + (plane.rise.x * x) + (plane.rise.y * y),
                             .gradient = plane.rise},
                            1.0, -1'000.0);
            }
            return lowered.lowest();
        };
        constexpr double step = 1e-3;
        for (int k = 0; k < 20; ++k) {
            const double x = 30.0 * (unit(random) - 0.5);
            const double y = 30.0 * (unit(random) - 0.5);
            const DVec3 rise = lowest(x, y).gradient;
            const double east =
                (lowest(x + step, y).value - lowest(x - step, y).value) / (2.0 * step);
            const double north =
                (lowest(x, y + step).value - lowest(x, y - step).value) / (2.0 * step);
            worst = std::max(worst, std::hypot(east - rise.x, north - rise.y));
        }
    }
    CHECK(worst < 0.01);
}

TEST_CASE("rounding a crest never sinks it past halfway down to its floor", "[globe][valleys]") {
    Lowered lowered{40.0, 40.0};
    lowered.add({.value = 100.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 96.0);
    lowered.add({.value = 100.0, .gradient = {-0.5, 0.0, 0.0}}, 1.0, 96.0);
    CHECK_THAT(lowered.lowest().value, WithinAbs(98.0, 1e-6));
    // On its floor, nothing rounds it lower.
    Lowered floor{40.0, 40.0};
    floor.add({.value = 96.0, .gradient = {0.5, 0.0, 0.0}}, 1.0, 96.0);
    floor.add({.value = 96.0, .gradient = {-0.5, 0.0, 0.0}}, 1.0, 96.0);
    // Only the soft turn between the two takes it under, a few centimetres.
    CHECK_THAT(floor.lowest().value,
               WithinAbs(96.0 - (hog::globe::lowest_softness * std::numbers::ln2), 1e-6));
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

TEST_CASE("loose ground holds walls in rock gentle only as high as it lies", "[globe][valleys]") {
    using hog::globe::wall_steepness;
    // Gentler than either holds: the ground's own fall.
    CHECK(wall_steepness(0.3, 100.0, 500.0, 0.7, 1.7) == 0.3);
    // Deeper than the walls climb, it holds them all at its repose.
    CHECK_THAT(wall_steepness(3.0, 100.0, 1000.0, 0.7, 1.7), WithinAbs(0.7, 1e-12));
    // A few metres of it over rock climbing 300 m: nearly as steep as the rock.
    const double shallow = wall_steepness(3.0, 100.0, 10.0, 0.7, 1.7);
    CHECK_THAT(shallow, WithinAbs(300.0 / ((10.0 / 0.7) + (290.0 / 1.7)), 1e-12));
    CHECK(shallow > 1.6);
    // None: as steep as the rock holds.
    CHECK_THAT(wall_steepness(3.0, 100.0, 0.0, 0.7, 1.7), WithinAbs(1.7, 1e-12));
    // Level ground has no walls.
    CHECK(wall_steepness(0.0, 100.0, 10.0, 0.7, 1.7) == 0.0);
}

TEST_CASE("a crest is rounded as far as its walls have climbed, never at once where a foot turns",
          "[globe][valleys]") {
    // One stretch's floor turning up into its wall at 0 over no width at all, another's wall
    // standing over it as near as rounds a crest between them.
    const auto lowered = [](double x) {
        Wall foot;
        foot.standing = 100.0 + (0.5 * std::max(x, 0.0));
        foot.standing_gradient = {x > 0.0 ? 0.5 : 0.0, 0.0, 0.0};
        foot.floor = 100.0;
        foot.steep = x > 0.0 ? 1.0 : 0.0;
        Wall wall;
        wall.standing = 110.0 - (0.3 * x);
        wall.standing_gradient = {-0.3, 0.0, 0.0};
        wall.floor = 40.0;
        wall.steep = 1.0;
        Lowest lowest{40.0, 40.0};
        lowest.add(foot);
        lowest.add(wall);
        Wall out;
        lowest.lower(out);
        return out.standing;
    };
    constexpr double step = 0.05;
    for (double x = -5.0; x < 5.0; x += step) {
        CHECK(std::abs(lowered(x + step) - lowered(x)) < step);
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

TEST_CASE("water ends where it meets the ground, never as a wall where a crest is rounded under it",
          "[globe][valleys]") {
    // Two stretches meeting at a crest at 0, rounded lower than either; one holds water a little
    // over its ground there.
    const auto depth = [](double x) {
        Wall wet;
        wet.standing = 100.0 + (0.5 * x);
        wet.standing_gradient = {0.5, 0.0, 0.0};
        wet.floor = 96.0;
        wet.water = 100.2;
        wet.bank = 100.2;
        wet.steep = 1.0;
        Wall dry;
        dry.standing = 100.0 - (0.5 * x);
        dry.standing_gradient = {-0.5, 0.0, 0.0};
        dry.floor = 96.0;
        dry.water = dry.standing;
        dry.bank = dry.standing;
        dry.steep = 1.0;
        Lowest lowest{40.0, 40.0};
        lowest.add(wet);
        lowest.add(dry);
        Wall out;
        lowest.lower(out);
        return out.water - out.standing;
    };
    double last = depth(-10.0);
    double most = last;
    for (int step = -199; step <= 200; ++step) {
        const double here = depth(step * 0.05);
        CHECK(std::abs(here - last) < 0.05);
        most = std::max(most, here);
        last = here;
    }
    CHECK(most > 0.1);
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

TEST_CASE("a wall's ground rises the way it says, channel, bends and ends and all",
          "[globe][valleys]") {
    std::mt19937_64 random{7};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    double worst = 0.0;
    for (int n = 0; n < 200; ++n) {
        Valley valley = falling();
        valley.to = glm::normalize(DVec3{1.0, (200.0 + (2'000.0 * unit(random))) / radius,
                                         (unit(random) - 0.5) * 1'000.0 / radius});
        valley.floor_to = 100.0 - (60.0 * unit(random));
        valley.reach = 200.0 + (600.0 * unit(random));
        // A belt as wide as the channel winds, as rivers' are.
        valley.channel_width = 12.0 * unit(random);
        valley.channel_depth = 3.0 * unit(random);
        valley.channel_depth_to = 3.0 * unit(random);
        valley.meander = 20.0 * unit(random);
        valley.belt = valley.meander + (valley.channel_width / 2.0);
        valley.meander_length = 11.0 * std::max(valley.channel_width, 1.0);
        valley.length = 3'000.0;
        valley.head = 0.2;
        valley.foot = 0.6;
        valley.wall = 0.2 + unit(random);
        valley.seed = static_cast<std::uint64_t>(n);
        // Every other one bends on into the stretches either side.
        if (n % 2 == 1) {
            const DVec3 along = valley.to - valley.from;
            const auto turned = [&] {
                return glm::normalize(
                    along + DVec3{0.0, 0.0, 0.15 * (unit(random) - 0.5) * glm::length(along)});
            };
            valley.runs_from = turned();
            valley.runs_to = turned();
        }
        for (int k = 0; k < 30; ++k) {
            const DVec3 point =
                at((2'500.0 * unit(random)) - 250.0, (1'200.0 * unit(random)) - 600.0);
            const DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, point));
            const DVec3 north = glm::cross(point, east);
            constexpr double step = 0.01;
            const auto standing = [&](DVec3 way) {
                return wall_at(valley, glm::normalize(point + (way * (step / radius))), radius)
                    .standing;
            };
            const DVec3 rise = wall_at(valley, point, radius).standing_gradient;
            const double east_error =
                ((standing(east) - standing(-east)) / (2.0 * step)) - glm::dot(rise, east);
            const double north_error =
                ((standing(north) - standing(-north)) / (2.0 * step)) - glm::dot(rise, north);
            worst = std::max(worst, std::hypot(east_error, north_error));
        }
    }
    CHECK(worst < 0.05);
}
