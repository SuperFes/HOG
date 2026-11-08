#include <hog/formation/bedrock.hpp>
#include <hog/formation/climate.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinRel;
using hog::formation::Bedrock;
using hog::formation::bedrock_on;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Meeting;
using hog::formation::Outcrop;
using hog::formation::outcrop_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Rock;
using hog::formation::rock_strength;
using hog::formation::Sphere;
using hog::formation::standing_strength;

namespace {

struct Made {
    explicit Made(Body made_with = {}) : body{made_with} {}
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Bedrock bedrock = bedrock_on(sphere, crust, body);
};

double total(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    return sum;
}

} // namespace

TEST_CASE("bedrock's layers are never negative and its shares stay in bounds",
          "[formation][bedrock]") {
    const Made made;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(made.bedrock.cover[point] >= 0.0);
        CHECK(made.bedrock.lava[point] >= 0.0);
        for (const double share : {made.bedrock.layering[point], made.bedrock.soluble[point],
                                   made.bedrock.folding[point]}) {
            CHECK(share >= 0.0);
            CHECK(share <= 1.0);
        }
    }
}

TEST_CASE("the sea floor gathers mud as it ages", "[formation][bedrock]") {
    const Made made;
    int floors = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.crust.light[point] > 0.0) {
            continue;
        }
        ++floors;
        CHECK_THAT(made.bedrock.cover[point],
                   WithinRel(made.body.sea_floor_mud * made.crust.floor_age[point], 1e-12));
    }
    CHECK(floors > 0);
}

TEST_CASE("layered rock lies folded where plates collide and flat far from them",
          "[formation][bedrock]") {
    Body body;
    body.old_folds = 0.0;
    const Made made{body};
    double belt = 0.0;
    int belts = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.crust.meeting[point] == Meeting::Collision &&
            made.crust.boundary[point] < 0.5 * body.belt_width) {
            belt += made.bedrock.folding[point];
            ++belts;
        }
        if (made.crust.boundary[point] > 4.0 * body.belt_width) {
            CHECK(made.bedrock.folding[point] < 1e-6);
        }
    }
    REQUIRE(belts > 0);
    CHECK(belt / belts > 0.7);
}

TEST_CASE("lava lies behind trenches, along rifts and where great floods spread",
          "[formation][bedrock]") {
    Body quiet;
    quiet.lava_floods = 0;
    const Made without{quiet};
    for (std::uint32_t point = 0; point < without.sphere.size(); ++point) {
        if (without.crust.meeting[point] == Meeting::Collision ||
            without.crust.meeting[point] == Meeting::Transform ||
            without.crust.meeting[point] == Meeting::None) {
            CHECK(without.bedrock.lava[point] == 0.0);
        }
    }
    const Made with;
    CHECK(total(with.bedrock.lava) > total(without.bedrock.lava));
}

TEST_CASE("rock that rises fast stands as strong as what wear lays bare", "[formation][bedrock]") {
    const Made made;
    const std::vector<double> strength =
        standing_strength(made.sphere, made.crust, made.bedrock, made.body);
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(strength[point] >= 0.0);
        CHECK(strength[point] <= 1.0);
        const double eroded = made.crust.uplift[point] * made.body.exhumed_years;
        if (made.crust.light[point] >= 0.5 &&
            eroded > made.bedrock.lava[point] + made.bedrock.cover[point]) {
            CHECK(strength[point] == rock_strength(Rock::Basement, 0.0, 0.0));
        }
    }
}

TEST_CASE("wear strips lava, then layered rock, down to the basement", "[formation][bedrock]") {
    const Made made;
    const Climate climate = climate_on(made.sphere, made.crust.base, made.crust.sea, made.body);
    const Rivers rivers = rivers_on(made.sphere, made.crust, climate, made.body);
    const Outcrop outcrop = outcrop_on(made.sphere, made.crust, made.bedrock, rivers, made.body);
    int basement = 0;
    int layered = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(outcrop.lava[point] <= made.bedrock.lava[point]);
        CHECK(outcrop.cover[point] <= made.bedrock.cover[point]);
        if (outcrop.lava[point] > 0.0) {
            CHECK(outcrop.rock[point] == Rock::Lava);
            CHECK(outcrop.cover[point] == made.bedrock.cover[point]);
        }
        if (outcrop.rock[point] == Rock::Basement || outcrop.rock[point] == Rock::SeaFloor) {
            CHECK(outcrop.cover[point] == 0.0);
            CHECK(outcrop.lava[point] == 0.0);
        }
        if (rivers.receiver[point] != point) {
            basement += outcrop.rock[point] == Rock::Basement ? 1 : 0;
            layered += outcrop.rock[point] == Rock::Layered ? 1 : 0;
        }
    }
    CHECK(basement > 0);
    CHECK(layered > 0);
}
