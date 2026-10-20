#include <hog/bot/wander.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <random>

using hog::DVec3;
using hog::WorldPosition;
using namespace hog::bot;

namespace {

constexpr double radius = 115'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};

[[nodiscard]] double along_ground(DVec3 a, DVec3 b) {
    return radius * std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

} // namespace

TEST_CASE("a fresh target lies within reach along the ground", "[bot][wander]") {
    std::mt19937_64 random{7};
    const WanderSettings settings{};
    for (int draw = 0; draw < 200; ++draw) {
        const Wander wander = pick(up, radius, random, settings);
        const double along = along_ground(up, wander.target);
        CHECK(along >= settings.nearest - 0.001);
        CHECK(along <= settings.furthest + 0.001);
        CHECK(wander.patience == settings.patience);
    }
}

TEST_CASE("the same seed wanders the same way", "[bot][wander]") {
    std::mt19937_64 one{42};
    std::mt19937_64 two{42};
    CHECK(pick(up, radius, one).target == pick(up, radius, two).target);
}

TEST_CASE("walking heads along the ground towards the target", "[bot][wander]") {
    std::mt19937_64 random{7};
    const Wander wander = pick(up, radius, random);
    const WorldPosition feet{up * radius};
    const Walked walked = walk(wander, feet, radius, 0.033);
    REQUIRE(walked.heading);
    const DVec3 heading = walked.heading.value_or(DVec3{});
    // Along the ground, and closing the distance.
    CHECK(std::abs(glm::dot(heading, up)) < 1e-9);
    CHECK(glm::dot(heading, walked.wander.target - up) > 0.0);
    CHECK(walked.wander.patience < wander.patience);
}

TEST_CASE("arriving, or losing patience, asks for somewhere new", "[bot][wander]") {
    std::mt19937_64 random{7};
    const Wander wander = pick(up, radius, random);
    // Standing on the target: arrived.
    const WorldPosition there{wander.target * radius};
    CHECK_FALSE(walk(wander, there, radius, 0.033).heading);
    // Far from it, but out of patience.
    const WorldPosition away{up * radius};
    CHECK_FALSE(walk(wander, away, radius, wander.patience + 1.0).heading);
}
