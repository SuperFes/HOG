#include <hog/formation/drifts.hpp>
#include <hog/formation/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::DVec3;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::downwind;
using hog::formation::dryness;
using hog::formation::Sphere;
using hog::formation::World;

namespace {

const World& world() {
    static const World made{Body{}, 4, 6};
    return made;
}

bool in_sea(const World& world, std::uint32_t point) {
    return world.rivers.receiver[point] == point;
}

double total(const std::vector<double>& field, const World& world) {
    double sum = 0.0;
    for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
        sum += field[point] * world.sphere.area(point);
    }
    return sum;
}

} // namespace

TEST_CASE("the wind lifts dry ground, not wet", "[formation][drifts]") {
    const Body body;
    CHECK(dryness(25.0, 0.05, body) > 0.99);
    CHECK(dryness(25.0, 3.0, body) < 0.01);
    CHECK(dryness(25.0, 0.6, body) > dryness(25.0, 1.0, body));
}

TEST_CASE("each point's wind blows on to the neighbour most nearly downwind",
          "[formation][drifts]") {
    const Sphere sphere{4, 3};
    Climate climate;
    climate.wind.resize(sphere.size());
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        // Blowing east everywhere but at the poles.
        const DVec3 here = sphere.point(point);
        const DVec3 east = glm::cross(DVec3{0.0, 0.0, 1.0}, here);
        climate.wind[point] = glm::length(east) > 1e-6 ? glm::normalize(east) : DVec3{0.0};
    }
    const std::vector<std::uint32_t> down = downwind(sphere, climate);
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        if (glm::length(climate.wind[point]) == 0.0) {
            CHECK(down[point] == point);
            continue;
        }
        REQUIRE(down[point] != point);
        const DVec3 way = glm::normalize(sphere.point(down[point]) - sphere.point(point));
        CHECK(glm::dot(way, climate.wind[point]) > 0.5);
    }
}

TEST_CASE("what the wind lays is what it took, none in the sea, dunes on bare dry sand",
          "[formation][drifts]") {
    const World& made = world();
    const auto& drifts = made.drifts;
    const std::uint32_t size = made.sphere.size();
    REQUIRE(drifts.sand.size() == size);
    double deflated = 0.0;
    for (std::uint32_t point = 0; point < size; ++point) {
        CHECK(drifts.sand[point] >= 0.0);
        CHECK(drifts.loess[point] >= 0.0);
        CHECK(drifts.deflated[point] <= made.body.deflation_depth + 1e-9);
        if (in_sea(made, point)) {
            CHECK(drifts.sand[point] == 0.0);
            CHECK(drifts.loess[point] == 0.0);
            CHECK(drifts.deflated[point] == 0.0);
        }
        if (drifts.dunes[point] > 0.0) {
            CHECK(drifts.sand[point] > 1.0);
        }
        deflated += drifts.deflated[point] * made.sphere.area(point);
    }
    // Some is lifted and laid, and silt comes only off the land: loess is never more than all
    // the wind took (beach sand comes off the sea floor besides).
    CHECK(deflated > 0.0);
    CHECK(total(drifts.sand, made) > 0.0);
    CHECK(total(drifts.loess, made) > 0.0);
    CHECK(total(drifts.loess, made) <= deflated);
    CHECK(std::ranges::any_of(drifts.dunes, [](double dunes) { return dunes > 0.5; }));
}

TEST_CASE("loess thins away downwind of where it was lifted", "[formation][drifts]") {
    const World& made = world();
    const std::vector<std::uint32_t> down = downwind(made.sphere, made.climate);
    // Following the wind on from the point where most silt was lifted, over land, it lies thinner
    // further on.
    std::uint32_t source = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.drifts.deflated[point] > made.drifts.deflated[source]) {
            source = point;
        }
    }
    REQUIRE(made.drifts.deflated[source] > 0.0);
    double near = 0.0;
    double far = 0.0;
    std::uint32_t at = source;
    for (int step = 0; step < 12; ++step) {
        (step < 3 ? near : far) += made.drifts.loess[at] / (step < 3 ? 3.0 : 9.0);
        at = down[at];
    }
    CHECK(near > far);
}
