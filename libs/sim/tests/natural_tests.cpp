#include <hog/core/hash.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/natural.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

using Catch::Matchers::WithinAbs;
using hog::DVec3;

namespace {

// Directions spread over the planet.
DVec3 scattered(std::uint64_t index) {
    const auto unit = [index](std::uint64_t salt) {
        return static_cast<double>(hog::hash_values(index, salt) % 1'000'000) / 1'000'000.0;
    };
    return hog::planet::direction_at(std::asin((2.0 * unit(0)) - 1.0),
                                     ((2.0 * unit(1)) - 1.0) * std::numbers::pi);
}

double angle(DVec3 from, DVec3 to) {
    return std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
}

} // namespace

TEST_CASE("natural reach is half a cell", "[sim][natural]") {
    CHECK_THAT(hog::sim::natural_reach(0), WithinAbs(std::numbers::pi / 4.0, 1e-15));
    CHECK_THAT(hog::sim::natural_reach(5), WithinAbs(std::numbers::pi / 128.0, 1e-15));
}

TEST_CASE("natural points move about their reach, smoothly and repeatably", "[sim][natural]") {
    const double reach = hog::sim::natural_reach(5);
    double total = 0.0;
    double furthest = 0.0;
    constexpr int count = 2'000;
    for (int index = 0; index < count; ++index) {
        const DVec3 from = scattered(static_cast<std::uint64_t>(index));
        const DVec3 moved = hog::sim::natural_point(from, reach, 7);
        CHECK_THAT(glm::length(moved), WithinAbs(1.0, 1e-12));
        CHECK(moved == hog::sim::natural_point(from, reach, 7));
        const double moved_by = angle(from, moved);
        total += moved_by;
        furthest = std::max(furthest, moved_by);

        // A point a hundredth of the reach away moves almost the same way.
        const DVec3 near = glm::normalize(from + DVec3{reach / 100.0, 0.0, 0.0});
        CHECK(angle(moved, hog::sim::natural_point(near, reach, 7)) < reach / 10.0);
    }
    const double mean = total / count;
    CHECK(mean > reach * 0.2);
    CHECK(mean < reach * 1.5);
    CHECK(furthest < reach * 4.0);
}

TEST_CASE("each seed moves points its own way", "[sim][natural]") {
    const double reach = hog::sim::natural_reach(5);
    const DVec3 from = scattered(1);
    CHECK(angle(hog::sim::natural_point(from, reach, 1), hog::sim::natural_point(from, reach, 2)) >
          0.0);
}

TEST_CASE("blending passes through cell centres and has no seams", "[sim][natural]") {
    const hog::sim::CellGrid grid{3};
    // A field that differs between every pair of neighbours, so any jump between cells would show.
    const auto field = [](std::uint32_t cell) { return static_cast<double>(cell % 7); };
    for (std::uint32_t cell = 0; cell < grid.size(); cell += 5) {
        CHECK_THAT(hog::sim::blend(grid, hog::planet::cell_center(grid.cell(cell)), field),
                   WithinAbs(field(cell), 1e-9));
    }
    // Walking finely along great circles, over face edges and through every cube corner, the
    // value never jumps: a step is a four-hundredth of a cell, so it can change by little.
    const double step = hog::sim::natural_reach(3) / 200.0;
    const auto walk = [&](DVec3 from, DVec3 axis) {
        double before = hog::sim::blend(grid, from, field);
        for (int stride = 1; stride < 4'000; ++stride) {
            const double turned = step * stride;
            const DVec3 point =
                (from * std::cos(turned)) + (glm::cross(axis, from) * std::sin(turned));
            const double now = hog::sim::blend(grid, point, field);
            REQUIRE(std::abs(now - before) < 0.1);
            before = now;
        }
    };
    for (int line = 0; line < 40; ++line) {
        const DVec3 from = scattered(static_cast<std::uint64_t>(line) + 100);
        walk(from,
             glm::normalize(glm::cross(from, scattered(static_cast<std::uint64_t>(line) + 200))));
    }
    for (const double x : {-1.0, 1.0}) {
        for (const double y : {-1.0, 1.0}) {
            for (const double z : {-1.0, 1.0}) {
                const DVec3 corner = glm::normalize(DVec3{x, y, z});
                // Starting a little before the corner, heading through it.
                const DVec3 axis = glm::normalize(glm::cross(corner, DVec3{0.3, -0.5, 0.8}));
                const double back = -step * 1'000.0;
                walk((corner * std::cos(back)) + (glm::cross(axis, corner) * std::sin(back)), axis);
            }
        }
    }
}
