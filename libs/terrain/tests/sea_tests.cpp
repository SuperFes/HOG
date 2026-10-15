#include <hog/terrain/sea.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

using hog::DVec2;
using hog::DVec3;
using hog::WorldPosition;
using hog::terrain::Bend;
using hog::terrain::bend_from;
using hog::terrain::BendSettings;
using hog::terrain::bent;
using hog::terrain::sea_along;
using hog::terrain::sea_disc;
using hog::terrain::sea_from_eye;
using hog::terrain::sea_reach;
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
