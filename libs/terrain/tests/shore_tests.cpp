#include <hog/jobs/job_system.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/shore.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
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
        for (const auto& [column, row] : {std::pair{0, 0}, std::pair{17, 200}, std::pair{128, 128},
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
    // As the terrain has them, but where the sea can't reach.
    std::vector<float> expected;
    for (const double spacing : shore_spacing) {
        const std::vector<float> one = shore_depths(terrain(), first.frame, radius, spacing);
        expected.insert(expected.end(), one.begin(), one.end());
    }
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(expected);
    CHECK(first.depths == expected);
    CHECK(first.still == still);
    CHECK(first.fetches == hog::terrain::shore_fetches(expected, still));

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

namespace {

constexpr auto layer_texels = static_cast<std::size_t>(shore_size) * shore_size;

// Two layers, finest first: each texel's depth by its column and row.
template <typename Fine, typename Coarse>
std::vector<float> layered(Fine fine, Coarse coarse) {
    std::vector<float> depths;
    depths.reserve(2 * layer_texels);
    for (const auto& depth_at : {std::function<float(int, int)>{fine}, {coarse}}) {
        for (int row = 0; row < shore_size; ++row) {
            for (int column = 0; column < shore_size; ++column) {
                depths.push_back(depth_at(column, row));
            }
        }
    }
    return depths;
}

float fine_at(const std::vector<float>& depths, int column, int row) {
    return depths.at((static_cast<std::size_t>(row) * shore_size) +
                     static_cast<std::size_t>(column));
}

} // namespace

TEST_CASE("the sea floods only what it can reach from the open sea", "[terrain][shore]") {
    // The open sea west of the middle, a beach rising out of it, a dune and beyond it a hollow and
    // a pond, each walled off; and one gap through the dune into a lagoon to the north.
    const auto fine = [](int column, int row) {
        if (column < 128) {
            return 2.0F;
        }
        if (column < 150) {
            return -1.0F; // the beach
        }
        if (column == 150) {
            return row == 200 ? -0.5F : -8.0F; // the dune, and the gap through it
        }
        if (column < 171) {
            if (row == 189) {
                return -8.0F;
            }
            return row > 189 ? 0.5F : -1.0F; // the lagoon, and the hollow
        }
        return column > 171 && column < 181 && row < 189 ? 1.0F : -20.0F; // the pond
    };
    const auto coarse = [](int column, int) { return column < 128 ? 30.0F : -20.0F; };
    std::vector<float> depths = layered(fine, coarse);
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(depths);
    constexpr float kept = hog::terrain::shore_highest;
    CHECK(fine_at(depths, 20, 30) == 2.0F);
    CHECK(fine_at(depths, 140, 30) == -1.0F);
    CHECK(fine_at(depths, 150, 30) == kept);
    CHECK(fine_at(depths, 160, 30) == kept); // the hollow
    CHECK(fine_at(depths, 175, 30) == kept); // the pond, below the sea but cut off from it
    CHECK(fine_at(depths, 150, 200) == -0.5F);
    CHECK(fine_at(depths, 160, 230) == 0.5F); // the lagoon
    // Only the pond's water is still.
    CHECK(fine_at(still, 175, 30) == 1.0F);
    CHECK(fine_at(still, 160, 30) == kept);
    CHECK(fine_at(still, 160, 230) == kept);
    CHECK(fine_at(still, 20, 30) == kept);
    // The coarse layer's open sea is kept, and its high ground as high as is kept.
    CHECK(depths.at(layer_texels + 10) == 30.0F);
    CHECK(depths.at(layer_texels + 200) == kept);
}

TEST_CASE("shallow water at the edge of what's known is taken as the open sea",
          "[terrain][shore]") {
    // Shallows out to the coarse layer's edge, and an island in them with a pond on it.
    const auto fine = [](int column, int row) {
        const int out = std::max(std::abs(column - 128), std::abs(row - 128));
        if (out < 4) {
            return 1.0F; // the pond
        }
        return out < 40 ? -6.0F : 3.0F;
    };
    const auto coarse = [](int column, int row) {
        return std::abs(column - 128) < 5 && std::abs(row - 128) < 5 ? -6.0F : 3.0F;
    };
    std::vector<float> depths = layered(fine, coarse);
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(depths);
    CHECK(fine_at(depths, 0, 0) == 3.0F);
    CHECK(fine_at(still, 128, 128) == 1.0F);
    CHECK(fine_at(depths, 128, 128) == hog::terrain::shore_highest);
    CHECK(depths.at(layer_texels + (std::size_t{10} * shore_size) + 10) == 3.0F);
}

namespace {

// The fetch at a fine texel for a wind blowing towards `heading`.
double fine_fetch(const std::vector<float>& fetches, int heading, int column, int row) {
    return static_cast<double>(fetches.at((static_cast<std::size_t>(heading) * layer_texels) +
                                          (static_cast<std::size_t>(row) * shore_size) +
                                          static_cast<std::size_t>(column)));
}

} // namespace

TEST_CASE("fetch runs upwind over still water to its shore", "[terrain][shore]") {
    // A pond ten texels east to west and four south to north, in land.
    const auto pond = [](int column, int row) {
        return column >= 100 && column < 110 && row >= 50 && row < 54;
    };
    const std::vector<float> depths =
        layered([](int, int) { return -3.0F; }, [](int, int) { return -3.0F; });
    const std::vector<float> still =
        layered([&](int column, int row) { return pond(column, row) ? 2.0F : -10.0F; },
                [](int, int) { return -10.0F; });
    const std::vector<float> fetches = hog::terrain::shore_fetches(depths, still);
    REQUIRE(fetches.size() == std::size_t{2} * hog::terrain::fetch_headings * layer_texels);
    const double spacing = shore_spacing.front();
    // Towards the east, from half a texel in at the west shore to its east shore.
    CHECK_THAT(fine_fetch(fetches, 0, 100, 51), WithinAbs(spacing / 2.0, 1e-3));
    CHECK_THAT(fine_fetch(fetches, 0, 109, 51), WithinAbs(spacing * 9.5, 1e-3));
    // Towards the west, the other way round.
    CHECK_THAT(fine_fetch(fetches, 4, 100, 51), WithinAbs(spacing * 9.5, 1e-3));
    // Towards the north, only across it.
    CHECK_THAT(fine_fetch(fetches, 2, 105, 53), WithinAbs(spacing * 3.5, 1e-3));
    // Diagonally, by the diagonal step.
    CHECK_THAT(fine_fetch(fetches, 1, 102, 52), WithinAbs(spacing * std::sqrt(2.0) * 2.5, 1e-3));
    // None over land.
    CHECK(fine_fetch(fetches, 0, 99, 51) == 0.0);
    CHECK(fine_fetch(fetches, 0, 110, 51) == 0.0);
}

TEST_CASE("fetch runs on into the coarser layer and out to sea", "[terrain][shore]") {
    // The open sea everywhere fine, and east of land coarse.
    const std::vector<float> depths =
        layered([](int, int) { return 30.0F; },
                [](int column, int) { return column >= 100 ? 30.0F : -3.0F; });
    const std::vector<float> still =
        layered([](int, int) { return -10.0F; }, [](int, int) { return -10.0F; });
    const std::vector<float> fetches = hog::terrain::shore_fetches(depths, still);
    const double fine = shore_spacing.front();
    const double coarse = shore_spacing.back();
    // West of the fine layer lies the coarse texel 111, 11.5 coarse texels from its shore.
    const double edge = (coarse * 11.5) + fine;
    CHECK_THAT(fine_fetch(fetches, 0, 0, 128), WithinRel(edge, 1e-5));
    CHECK_THAT(fine_fetch(fetches, 0, 10, 128), WithinRel(edge + (fine * 10.0), 1e-5));
    // Towards the west, from beyond all that's known.
    CHECK(fine_fetch(fetches, 4, 128, 128) == static_cast<double>(hog::terrain::longest_fetch));
}
