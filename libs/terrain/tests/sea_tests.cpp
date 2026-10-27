#include <hog/terrain/sea.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>

using hog::DVec2;
using hog::DVec3;
using hog::WorldPosition;
using hog::terrain::Bend;
using hog::terrain::bend_from;
using hog::terrain::BendSettings;
using hog::terrain::bent;
using hog::terrain::open_sea;
using hog::terrain::Optics;
using hog::terrain::optics;
using hog::terrain::sea_along;
using hog::terrain::sea_disc;
using hog::terrain::sea_from_eye;
using hog::terrain::sea_reach;
using hog::terrain::seabed_showing;
using hog::terrain::SeaDisc;
using hog::terrain::SeaReach;

namespace {

constexpr double radius = 115'000.0;
constexpr double earth = BendSettings{}.faked_radius;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

Bend bend_at(double eye_height) {
    return bend_from(WorldPosition{up * (radius + eye_height)}, radius, 0.0);
}

double horizon_on(double sphere, double eye_height) {
    return sphere * std::acos(sphere / (sphere + eye_height));
}

double schlick(double facing) {
    return hog::terrain::water_facing_reflection +
           ((1.0 - hog::terrain::water_facing_reflection) * std::pow(1.0 - facing, 5.0));
}

// As the sea and the ground beneath it are drawn: the surface's Fresnel where the line of sight
// crosses it, and the water passed on the way to the ground at `depth`, `round` radians from under
// the eye.
double seen_through_sea(double eye, double sea, double depth, double round, const Optics& water) {
    const double ground = sea - depth;
    const DVec2 from{0.0, eye};
    const DVec2 to{ground * std::sin(round), ground * std::cos(round)};
    const DVec2 looking = glm::normalize(to - from);
    const double b = glm::dot(from, looking);
    const double crossed = -b - std::sqrt((b * b) - ((eye * eye) - (sea * sea)));
    const double facing = glm::dot(-looking, (from + (looking * crossed)) / sea);
    const double down = std::max(glm::dot(-looking, to / ground), hog::terrain::water_least_facing);
    const DVec3 passed = glm::exp(-(water.dimming + (water.hiding / down)) * depth);
    return (1.0 - schlick(facing)) * std::max({passed.x, passed.y, passed.z});
}

} // namespace

TEST_CASE("sea level is drawn where bending puts it", "[terrain][sea]") {
    for (const double eye_height : {-20.0, 0.0, 3.0, 500.0, 20'000.0, 400'000.0}) {
        const Bend bend = bend_at(eye_height);
        for (const double along : {0.0, 0.5, 40.0, 6'000.0, 90'000.0}) {
            for (const DVec3 towards : {east, north, glm::normalize(east - north)}) {
                const double angle = along / radius;
                const DVec3 truly =
                    (((up * std::cos(angle)) + (towards * std::sin(angle))) * radius) -
                    (up * (radius + eye_height));
                const DVec3 drawn = sea_from_eye(bend, along, towards);
                CHECK(glm::length(drawn - bent(bend, truly)) <
                      1e-9 * std::max(glm::length(drawn), 1.0));
            }
        }
    }
}

TEST_CASE("the sea reaches just past the horizon", "[terrain][sea]") {
    const auto reaches = [](double furthest, double horizon) {
        return furthest > horizon && furthest < 1.1 * horizon;
    };
    CHECK(reaches(sea_reach(bend_at(3.0)).furthest, horizon_on(earth, 3.0)));
    CHECK(reaches(sea_reach(bend_at(500.0)).furthest, horizon_on(bend_at(500.0).faked, 500.0)));
    CHECK(reaches(sea_reach(bend_at(400'000.0)).furthest, horizon_on(radius, 400'000.0)));
    // From under the sea, out to where it would be from just above.
    const Bend under = bend_at(-20.0);
    CHECK(reaches(sea_reach(under).furthest, horizon_on(under.faked, 1.0)));
}

TEST_CASE("the sea's rings start as far apart as the eye stands over the water below it",
          "[terrain][sea]") {
    CHECK(sea_reach(bend_at(500.0)).nearest == 500.0);
    const SeaReach over_lake = sea_reach(bend_at(500.0), 480.0);
    CHECK(over_lake.nearest == 20.0);
    CHECK(over_lake.furthest == sea_reach(bend_at(500.0)).furthest);
    CHECK(sea_reach(bend_at(500.0), 499.9).nearest == 1.0);
    CHECK(sea_reach(bend_at(-20.0)).nearest == 1.0);
}

TEST_CASE("the sea's rings spread with their distance from under the eye", "[terrain][sea]") {
    for (const double eye_height : {3.0, 500.0, 400'000.0}) {
        const SeaReach reach = sea_reach(bend_at(eye_height));
        CHECK(sea_along(reach, 0.0) == 0.0);
        CHECK(std::abs(sea_along(reach, 1.0) - reach.furthest) < 1e-9 * reach.furthest);

        constexpr int rings = 96;
        const auto ring = [&](int at) { return sea_along(reach, static_cast<double>(at) / rings); };
        CHECK(ring(1) < reach.nearest);
        // Each gap is the same share of the way out from a nearest's distance short of the eye.
        const double spread = (ring(1) + reach.nearest) / reach.nearest;
        for (int at = 1; at < rings; ++at) {
            CHECK(ring(at + 1) > ring(at));
            const double share = (ring(at + 1) + reach.nearest) / (ring(at) + reach.nearest);
            CHECK(std::abs(share - spread) < 1e-9);
        }
    }
}

TEST_CASE("the sea's disc is whole and faces up", "[terrain][sea]") {
    constexpr int rings = 5;
    constexpr int spokes = 12;
    const SeaDisc disc = sea_disc(rings, spokes);
    REQUIRE(disc.vertices.size() == static_cast<std::size_t>((rings * spokes) + 1));
    REQUIRE(disc.indices.size() == static_cast<std::size_t>(3 * spokes * ((2 * rings) - 1)));
    CHECK(disc.vertices.front().out == 0.0F);

    double area = 0.0;
    for (std::size_t corner = 0; corner < disc.indices.size(); corner += 3) {
        const auto flat = [&](std::size_t at) {
            REQUIRE(disc.indices[corner + at] < disc.vertices.size());
            const auto& vertex = disc.vertices[disc.indices[corner + at]];
            CHECK(std::abs(glm::length(DVec2{vertex.towards}) - 1.0) < 1e-6);
            return DVec2{vertex.towards} * static_cast<double>(vertex.out);
        };
        const DVec2 first = flat(1) - flat(0);
        const DVec2 second = flat(2) - flat(0);
        const double facing = (first.x * second.y) - (first.y * second.x);
        CHECK(facing > 0.0);
        area += facing / 2.0;
    }
    // Laid flat, a regular polygon reaching out to 1.
    const double polygon = spokes / 2.0 * std::sin(2.0 * std::numbers::pi / spokes);
    CHECK(std::abs(area - polygon) < 1e-6);
}

TEST_CASE("the seabed shows through the sea least at a glance, and the deeper it lies",
          "[terrain][sea]") {
    const double eye = earth + 400.0;
    const Optics water{.dimming = {0.3, 0.06, 0.03}, .hiding = {0.4, 0.07, 0.05}};
    const auto at = [&](double depth, double along) {
        return seabed_showing(eye, earth, earth - depth, earth - depth, along / earth,
                              along / earth, water);
    };
    // Straight down through ten metres, and back up, in the blue that gets through best.
    CHECK(std::abs(at(10.0, 0.0) - ((1.0 - 0.02) * std::exp(-(0.03 + 0.05) * 10.0))) < 1e-12);
    CHECK(at(30.0, 0.0) < at(10.0, 0.0));
    CHECK(at(10.0, 2'000.0) < at(10.0, 0.0));
    // Twenty kilometres off, the sea mirrors nearly all and the long way through the water does
    // the rest.
    CHECK(at(10.0, 20'000.0) < at(10.0, 0.0) / 100.0);
    CHECK(at(30.0, 20'000.0) < 1e-4);
}

TEST_CASE("ground not under the sea, or not seen from clear above it, shows whole",
          "[terrain][sea]") {
    const Optics water = optics(open_sea);
    CHECK(seabed_showing(earth + 400.0, earth, earth - 50.0, earth + 1.0, 0.01, 0.02, water) ==
          1.0);
    CHECK(seabed_showing(earth - 5.0, earth, earth - 50.0, earth - 40.0, 0.01, 0.02, water) == 1.0);
    // Over the sea's mean but under a wave's crest.
    CHECK(seabed_showing(earth + 0.5, earth, earth - 50.0, earth - 40.0, 0.01, 0.02, water) == 1.0);
}

TEST_CASE("no part of the seabed shows more than its bound says", "[terrain][sea]") {
    std::mt19937 dice{17};
    const auto uniform = [&](double low, double high) {
        return std::uniform_real_distribution<double>{low, high}(dice);
    };
    // Clear water, and water whose clearest colour is the red.
    const std::array<Optics, 2> waters{
        optics(open_sea), Optics{.dimming = {0.2, 3.0, 9.0}, .hiding = {0.3, 4.0, 12.0}}};
    for (int trial = 0; trial < 400; ++trial) {
        const Optics& water = waters.at(trial % 4 < 2 ? 0 : 1);
        const double sea = trial % 2 == 0 ? earth : radius;
        const double eye = sea + uniform(1.0, 5'000.0);
        const double shallowest = uniform(0.0, 100.0);
        const double deepest = shallowest + uniform(0.0, 300.0);
        const double nearest = uniform(0.0, 0.04);
        const double farthest = nearest + uniform(0.0, 0.01);
        const double bound =
            seabed_showing(eye, sea, sea - deepest, sea - shallowest, nearest, farthest, water);
        constexpr int steps = 8;
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; j <= steps; ++j) {
                const double depth = shallowest + ((deepest - shallowest) * i / steps);
                const double round = nearest + ((farthest - nearest) * j / steps);
                CAPTURE(trial, depth, round);
                CHECK(seen_through_sea(eye, sea, depth, round, water) <=
                      (bound * (1.0 + 1e-9)) + 1e-15);
            }
        }
    }
}
