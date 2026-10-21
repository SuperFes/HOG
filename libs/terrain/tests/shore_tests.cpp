#include <hog/jobs/job_system.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/shore.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::DVec3;
using hog::terrain::shore_along;
using hog::terrain::shore_depths;
using hog::terrain::shore_direction;
using hog::terrain::shore_frame;
using hog::terrain::shore_size;
using hog::terrain::shore_spacing;
using hog::terrain::ShoreFrame;

namespace {

constexpr double radius = 115'000.0;

const hog::planet::Elevation& terrain() {
    static const hog::planet::Elevation elevation{
        {.seed = 4, .ocean_fraction = 0.7, .radius = radius}};
    return elevation;
}

// `metres` along the ground from `from`, towards `towards`.
DVec3 along(DVec3 from, DVec3 towards, double metres) {
    const DVec3 across = glm::normalize(towards - (from * glm::dot(towards, from)));
    return glm::normalize(from + (across * (metres / radius)));
}

} // namespace

TEST_CASE("a shore's frame is fixed to the world near the eye", "[terrain][shore]") {
    const DVec3 eye = glm::normalize(DVec3{0.3, -0.5, 0.8});
    const ShoreFrame frame = shore_frame(eye, radius);
    CHECK_THAT(glm::length(frame.anchor), WithinAbs(1.0, 1e-12));
    CHECK_THAT(glm::dot(frame.east, frame.anchor), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::dot(frame.north, frame.anchor), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::dot(frame.east, frame.north), WithinAbs(0.0, 1e-12));
    CHECK_THAT(glm::dot(glm::cross(frame.east, frame.north), frame.anchor), WithinAbs(1.0, 1e-12));
    // North is the way to the pole.
    CHECK(frame.north.z > 0.0);
    CHECK(glm::distance(eye, frame.anchor) * radius < hog::terrain::shore_anchoring);

    // A step away keeps it, or moves it to another anchor no further than the eye moves.
    const ShoreFrame stepped = shore_frame(along(eye, frame.north, 10.0), radius);
    CHECK(glm::distance(stepped.anchor, frame.anchor) * radius < hog::terrain::shore_anchoring);
    CHECK(shore_frame(eye, radius) == frame);
    CHECK(shore_frame(along(eye, frame.east, 3'000.0), radius) != frame);

    const ShoreFrame pole = shore_frame(DVec3{0.0, 0.0, 1.0}, radius);
    CHECK_THAT(glm::length(pole.east), WithinAbs(1.0, 1e-12));
}

TEST_CASE("a point on a shore's plane comes back where it was", "[terrain][shore]") {
    const ShoreFrame frame = shore_frame(glm::normalize(DVec3{-0.6, 0.2, 0.3}), radius);
    for (const DVec2 at : {DVec2{0.0, 0.0}, DVec2{1'000.0, -20.0}, DVec2{-8'000.0, 7'500.0}}) {
        const DVec3 direction = shore_direction(frame, radius, at);
        CHECK_THAT(glm::length(direction), WithinAbs(1.0, 1e-12));
        const DVec2 back = shore_along(frame, radius, direction);
        CHECK_THAT(back.x, WithinAbs(at.x, 1e-6));
        CHECK_THAT(back.y, WithinAbs(at.y, 1e-6));
    }
    // Near the anchor, metres on the plane are metres along the ground.
    const DVec3 near = shore_direction(frame, radius, {300.0, 0.0});
    CHECK_THAT(std::acos(glm::dot(near, frame.anchor)) * radius, WithinAbs(300.0, 0.01));
}

TEST_CASE("a shore's depths are the sea's at each texel", "[terrain][shore]") {
    // Anchored on an island's coast.
    const auto& island = terrain().archipelago().islands().front();
    DVec3 coast = island.center;
    while (terrain().is_land(hog::planet::cell_at(coast))) {
        coast = along(coast, island.center + island.axis, 50.0);
    }
    const ShoreFrame frame = shore_frame(coast, radius);
    for (const double spacing : shore_spacing) {
        CAPTURE(spacing);
        const std::vector<float> depths = shore_depths(terrain(), frame, radius, spacing);
        REQUIRE(depths.size() == static_cast<std::size_t>(shore_size) * shore_size);
        // Round an island, both land and sea.
        CHECK(std::ranges::count(depths, hog::terrain::shore_highest) > 0);
        CHECK(std::ranges::count_if(depths, [](float depth) { return depth > 1.0F; }) > 0);
        CHECK(std::ranges::min(depths) >= hog::terrain::shore_highest);
        for (const auto [column, row] : {std::pair{0, 0}, std::pair{17, 200}, std::pair{128, 128},
                                         std::pair{255, 3}, std::pair{90, 255}}) {
            const double half = shore_size / 2.0;
            const DVec3 at = shore_direction(
                frame, radius, {(column + 0.5 - half) * spacing, (row + 0.5 - half) * spacing});
            const double expected = std::max(
                -hog::planet::fixed_to_double(terrain().elevation(hog::planet::cell_at(at))),
                static_cast<double>(hog::terrain::shore_highest));
            CHECK_THAT(depths.at(static_cast<std::size_t>((row * shore_size) + column)),
                       WithinAbs(expected, 1e-3));
        }
    }
}

TEST_CASE("the depths round the eye are worked out anew as it moves on", "[terrain][shore]") {
    hog::jobs::JobSystem jobs{2};
    hog::terrain::ShoreStream stream{terrain(), radius, jobs};
    const DVec3 eye = terrain().archipelago().islands().front().center;
    CHECK_FALSE(stream.collect());
    stream.follow(eye);
    const auto finished = stream.finish();
    REQUIRE(finished.has_value());
    const hog::terrain::ShoreDepths first = finished.value_or(hog::terrain::ShoreDepths{});
    constexpr auto layer = static_cast<std::size_t>(shore_size) * shore_size;
    CHECK(first.frame == shore_frame(eye, radius));
    CHECK(first.depths.size() == shore_spacing.size() * layer);
    CHECK(std::ranges::equal(std::span{first.depths}.subspan(0, layer),
                             shore_depths(terrain(), first.frame, radius, shore_spacing[0])));

    // Nearby, the same; far off, others, and only the last wanted once those in flight are done.
    stream.follow(first.frame.anchor);
    CHECK_FALSE(stream.finish());
    const DVec3 further = along(eye, first.frame.east, 3'000.0);
    const DVec3 furthest = along(eye, first.frame.east, 6'000.0);
    stream.follow(further);
    stream.follow(furthest);
    const auto last = stream.finish();
    REQUIRE(last.has_value());
    CHECK(last.value_or(hog::terrain::ShoreDepths{}).frame == shore_frame(furthest, radius));
    CHECK_FALSE(stream.collect());
}
