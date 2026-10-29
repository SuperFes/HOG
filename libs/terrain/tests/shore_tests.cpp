#include <hog/jobs/job_system.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/terrain/river_water.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <optional>
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

// NOLINTNEXTLINE(cert-err58-cpp,bugprone-throwing-static-initialization): nothing here throws
const hog::planet::PlanetConfig planet{.seed = 4,
                                       .ocean_fraction = 0.7,
                                       .radius = radius,
                                       .erosion_cell = hog::testing::coarse_erosion_cell};

const hog::planet::World& world() {
    static const hog::planet::World world{planet};
    return world;
}

const hog::planet::Elevation& terrain() {
    return world().geology().terrain();
}

// A planet with lakes on it.
const hog::planet::World& lake_world() {
    static const hog::planet::World world{{.seed = 1,
                                           .ocean_fraction = 0.7,
                                           .radius = radius,
                                           .erosion_cell = hog::testing::coarse_erosion_cell}};
    return world;
}

const hog::planet::Elevation& lakeland() {
    return lake_world().geology().terrain();
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
    // Anchored on the coast nearest the start, which lies out at sea.
    DVec3 coast = terrain().nearest_land(hog::planet::start_direction);
    while (terrain().is_land(hog::planet::cell_at(coast))) {
        coast = along(coast, hog::planet::start_direction, 50.0);
    }
    const ShoreFrame frame = shore_frame(coast, radius);
    for (const double spacing : shore_spacing) {
        CAPTURE(spacing);
        const hog::terrain::ShoreGround ground =
            hog::terrain::shore_ground(world(), frame, radius, spacing);
        const std::vector<float> depths = shore_depths(ground);
        REQUIRE(depths.size() == static_cast<std::size_t>(shore_size) * shore_size);
        REQUIRE(ground.elevations.size() == depths.size());
        REQUIRE(ground.lakes.size() == depths.size());
        // Both land and sea.
        CHECK(std::ranges::count(depths, hog::terrain::shore_highest) > 0);
        CHECK(std::ranges::count_if(depths, [](float depth) { return depth > 1.0F; }) > 0);
        CHECK(std::ranges::min(depths) >= hog::terrain::shore_highest);
        for (const auto& [column, row] : {std::pair{0, 0}, std::pair{17, 200}, std::pair{128, 128},
                                          std::pair{255, 3}, std::pair{90, 255}}) {
            const double half = shore_size / 2.0;
            const DVec3 at = shore_direction(
                frame, radius, {(column + 0.5 - half) * spacing, (row + 0.5 - half) * spacing});
            const double elevation =
                hog::planet::fixed_to_double(terrain().elevation(hog::planet::cell_at(at)));
            const double expected =
                std::max(-elevation, static_cast<double>(hog::terrain::shore_highest));
            const std::size_t texel =
                (static_cast<std::size_t>(row) * shore_size) + static_cast<std::size_t>(column);
            CHECK_THAT(depths.at(texel), WithinAbs(expected, 1e-3));
            CHECK_THAT(ground.elevations.at(texel), WithinAbs(elevation, 1e-3));
        }
    }
}

TEST_CASE("the depths round the eye are worked out anew as it moves on", "[terrain][shore]") {
    hog::jobs::JobSystem jobs{2};
    hog::terrain::ShoreStream stream{world(), radius, jobs};
    const DVec3 eye = terrain().nearest_land(hog::planet::start_direction);
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
    std::vector<float> elevations;
    std::vector<float> lakes;
    std::vector<std::uint8_t> sea;
    for (const double spacing : shore_spacing) {
        const hog::terrain::ShoreGround ground =
            hog::terrain::shore_ground(world(), first.frame, radius, spacing);
        const std::vector<float> one = shore_depths(ground);
        expected.insert(expected.end(), one.begin(), one.end());
        elevations.insert(elevations.end(), ground.elevations.begin(), ground.elevations.end());
        lakes.insert(lakes.end(), ground.lakes.begin(), ground.lakes.end());
        sea.insert(sea.end(), ground.sea.begin(), ground.sea.end());
    }
    std::vector<float> still = hog::terrain::keep_to_sea_reach(expected, sea);
    const std::vector<float> levels = hog::terrain::keep_lakes(elevations, lakes, still);
    CHECK(first.depths == expected);
    CHECK(first.still == still);
    CHECK(first.levels == levels);
    CHECK(first.fetches == hog::terrain::shore_fetches(expected, still));
    const hog::terrain::ShoreDepths again =
        hog::terrain::work_out_shore(world(), first.frame, radius);
    CHECK(again.depths == first.depths);
    CHECK(again.levels == first.levels);

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

// The sea standing everywhere in both layers.
std::vector<std::uint8_t> all_sea() {
    return std::vector<std::uint8_t>(2 * layer_texels, 1);
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
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(depths, all_sea());
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
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(depths, all_sea());
    CHECK(fine_at(depths, 0, 0) == 3.0F);
    CHECK(fine_at(still, 128, 128) == 1.0F);
    CHECK(fine_at(depths, 128, 128) == hog::terrain::shore_highest);
    CHECK(depths.at(layer_texels + (std::size_t{10} * shore_size) + 10) == 3.0F);
}

TEST_CASE("low ground the sea can't reach lies dry unless the sea stands over it",
          "[terrain][shore]") {
    // The open sea west of a ridge, and a deep basin east of it cut off from the sea, but for a
    // trench dug through the ridge in one case.
    const auto ground = [](bool trench) {
        return [trench](int column, int row) {
            if (column < 128) {
                return 30.0F;
            }
            if (column < 140) {
                return trench && row > 200 && row < 204 ? 2.0F : -8.0F;
            }
            return 15.0F;
        };
    };
    // The basin is deep enough in the coarse layer to be taken for the open sea, were it the sea.
    const auto coarse = [](int column, int) {
        if (column < 128) {
            return 30.0F;
        }
        return column < 130 ? -8.0F : 25.0F;
    };
    std::vector<std::uint8_t> sea(2 * layer_texels, 0);
    for (std::size_t at = 0; at < sea.size(); ++at) {
        sea[at] = at % shore_size < 128 ? 1 : 0;
    }
    constexpr float kept = hog::terrain::shore_highest;

    std::vector<float> walled = layered(ground(false), coarse);
    const std::vector<float> dry = hog::terrain::keep_to_sea_reach(walled, sea);
    CHECK(fine_at(walled, 20, 30) == 30.0F);
    CHECK(fine_at(walled, 160, 30) == kept);
    CHECK(fine_at(dry, 160, 30) == kept);
    CHECK(walled.at(layer_texels + 200) == kept);

    std::vector<float> dug = layered(ground(true), coarse);
    const std::vector<float> still = hog::terrain::keep_to_sea_reach(dug, sea);
    CHECK(fine_at(dug, 130, 202) == 2.0F);
    CHECK(fine_at(dug, 160, 30) == 15.0F);
    CHECK(fine_at(still, 160, 30) == kept);
}

TEST_CASE("dry low ground keeps its level under the ground", "[terrain][shore]") {
    constexpr float kept = hog::terrain::shore_highest;
    // Dry low ground, low ground under the sea, under still water, under a lake, and high ground.
    const std::vector<float> elevations{-20.0F, -20.0F, -20.0F, 5.0F, 30.0F};
    const std::vector<float> depths{kept, 20.0F, kept, kept, kept};
    const std::vector<float> still{kept, kept, 20.0F, 2.0F, kept};
    std::vector<float> levels{0.0F, 0.0F, 0.0F, 7.0F, 0.0F};
    hog::terrain::keep_dry(elevations, depths, still, levels);
    CHECK(levels == std::vector<float>{-20.0F + kept, 0.0F, 0.0F, 7.0F, 0.0F});
}

TEST_CASE("the shore's ground is the world's as changed, its water as generated",
          "[terrain][shore]") {
    hog::planet::World dug{planet};
    const ShoreFrame frame =
        shore_frame(terrain().nearest_land(hog::planet::start_direction), radius);
    const double spacing = shore_spacing.front();
    const std::size_t middle =
        ((shore_size / 2) * static_cast<std::size_t>(shore_size)) + (shore_size / 2);
    const DVec3 at = shore_direction(frame, radius, {0.5 * spacing, 0.5 * spacing});
    const hog::terrain::ShoreGround before =
        hog::terrain::shore_ground(dug, frame, radius, spacing);
    REQUIRE(dug.apply({.cell = hog::planet::cell_at(at).parent(16),
                       .time = {},
                       .author = {},
                       .layer = hog::planet::Layer::Player,
                       .payload = hog::planet::Excavate{.depth = 500 * hog::planet::fixed_one}}));
    const hog::terrain::ShoreGround after = hog::terrain::shore_ground(dug, frame, radius, spacing);
    CHECK_THAT(after.elevations.at(middle),
               WithinAbs(static_cast<double>(before.elevations.at(middle)) - 500.0, 1e-3));
    CHECK(after.sea == before.sea);
    CHECK(after.lakes == before.lakes);
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
    const double coarse = shore_spacing.at(1);
    // West of the fine layer lies the coarse texel 111, 11.5 coarse texels from its shore.
    const double edge = (coarse * 11.5) + fine;
    CHECK_THAT(fine_fetch(fetches, 0, 0, 128), WithinRel(edge, 1e-5));
    CHECK_THAT(fine_fetch(fetches, 0, 10, 128), WithinRel(edge + (fine * 10.0), 1e-5));
    // Towards the west, from beyond all that's known.
    CHECK(fine_fetch(fetches, 4, 128, 128) == static_cast<double>(hog::terrain::longest_fetch));
}

namespace {

// Of planes of `offshore` as shore_offshore lays them, plane `which` of the fine layer at a texel.
double fine_offshore(const std::vector<float>& offshore, int which, int column, int row) {
    return static_cast<double>(offshore.at((static_cast<std::size_t>(which) * layer_texels) +
                                           (static_cast<std::size_t>(row) * shore_size) +
                                           static_cast<std::size_t>(column)));
}

} // namespace

TEST_CASE("water lies as far offshore as its nearest waterline, and land as far inland",
          "[terrain][shore]") {
    // The sea east of a straight shore, and a pond inland.
    const auto pond = [](int column, int row) {
        return column >= 40 && column < 50 && row >= 40 && row < 50;
    };
    const std::vector<float> depths =
        layered([](int column, int) { return column >= 100 ? 5.0F : -3.0F; },
                [](int column, int) { return column >= 128 ? 30.0F : -3.0F; });
    const std::vector<float> still =
        layered([&](int column, int row) { return pond(column, row) ? 2.0F : -10.0F; },
                [](int, int) { return -10.0F; });
    const std::vector<float> offshore = hog::terrain::shore_offshore(depths, still);
    REQUIRE(offshore.size() == std::size_t{2} * hog::terrain::offshore_planes * layer_texels);
    const double spacing = shore_spacing.front();
    // The waterline lies halfway between the texels either side of it.
    CHECK_THAT(fine_offshore(offshore, 0, 100, 128), WithinAbs(spacing / 2.0, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 0, 120, 128), WithinAbs(spacing * 20.5, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 0, 99, 128), WithinAbs(-spacing / 2.0, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 0, 70, 128), WithinAbs(-spacing * 29.5, 1e-3));
    // Diagonally to the nearest corner of land.
    CHECK_THAT(fine_offshore(offshore, 0, 44, 44), WithinAbs(spacing * 4.5, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 0, 53, 53),
               WithinAbs(-spacing * (std::sqrt(32.0) - 0.5), 1e-3));
    // Growing a metre a metre out from the shore, straight out.
    CHECK_THAT(fine_offshore(offshore, 1, 120, 128), WithinAbs(1.0, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 2, 120, 128), WithinAbs(0.0, 1e-3));
    CHECK_THAT(fine_offshore(offshore, 1, 70, 128), WithinAbs(1.0, 1e-3));
}

TEST_CASE("a waterline beyond a finer layer is found in the coarser one", "[terrain][shore]") {
    // The open sea everywhere fine, and east of land coarse.
    const std::vector<float> depths =
        layered([](int, int) { return 30.0F; },
                [](int column, int) { return column >= 100 ? 30.0F : -3.0F; });
    const std::vector<float> still =
        layered([](int, int) { return -10.0F; }, [](int, int) { return -10.0F; });
    const std::vector<float> offshore = hog::terrain::shore_offshore(depths, still);
    const double fine = shore_spacing.front();
    const double coarse = shore_spacing.at(1);
    // The coarse layer's waterline lies 28 of its texels west of the anchor.
    const double waterline = -28.0 * coarse;
    for (const int column : {0, 64, 128, 255}) {
        const double centre = (column + 0.5 - (shore_size / 2.0)) * fine;
        CAPTURE(column);
        CHECK_THAT(fine_offshore(offshore, 0, column, 128), WithinRel(centre - waterline, 1e-4));
        CHECK_THAT(fine_offshore(offshore, 1, column, 128), WithinAbs(1.0, 1e-3));
    }
}

TEST_CASE("open sea with no land in reach lies as far offshore as is kept", "[terrain][shore]") {
    const std::vector<float> depths =
        layered([](int, int) { return 30.0F; }, [](int, int) { return 30.0F; });
    const std::vector<float> still =
        layered([](int, int) { return -10.0F; }, [](int, int) { return -10.0F; });
    const std::vector<float> offshore = hog::terrain::shore_offshore(depths, still);
    CHECK(fine_offshore(offshore, 0, 128, 128) ==
          static_cast<double>(hog::terrain::farthest_offshore));
    CHECK(fine_offshore(offshore, 1, 128, 128) == 0.0);
}

TEST_CASE("a lake's level is kept over it and the ground round it, lower ground told dry",
          "[terrain][shore]") {
    // A lake at 50 m over a bed at 45, in ground at 52, with a valley at 40 two texels east of its
    // east shore and a lower lake at 20 over a bed at 15 beyond that, and a ditch at 40 ten texels
    // west of its west shore.
    const auto in_lake = [](int column, int row) {
        return column >= 100 && column < 110 && row >= 50 && row < 60;
    };
    const auto elevation = [&](int column, int row) {
        if (in_lake(column, row)) {
            return 45.0F;
        }
        if (column == 111 || column == 90) {
            return 40.0F;
        }
        return column >= 113 && column < 116 && row >= 50 && row < 60 ? 15.0F : 52.0F;
    };
    const auto lake = [&](int column, int row) {
        if (in_lake(column, row)) {
            return 50.0F;
        }
        return column >= 113 && column < 116 && row >= 50 && row < 60 ? 20.0F : 0.0F;
    };
    const std::vector<float> elevations = layered(elevation, [](int, int) { return 52.0F; });
    const std::vector<float> lakes = layered(lake, [](int, int) { return 0.0F; });
    std::vector<float> still(elevations.size(), hog::terrain::shore_highest);
    const std::vector<float> levels = hog::terrain::keep_lakes(elevations, lakes, still);
    REQUIRE(levels.size() == elevations.size());
    // Over the lake, its level, and as deep as it lies under it.
    CHECK(fine_at(levels, 105, 55) == 50.0F);
    CHECK(fine_at(still, 105, 55) == 5.0F);
    // Out over the ground round it, as far as the margin, kept as far below it.
    CHECK(fine_at(levels, 98, 55) == 50.0F);
    CHECK(fine_at(levels, 98, 48) == 50.0F);
    CHECK(fine_at(still, 98, 55) == -2.0F);
    CHECK(fine_at(levels, 97, 55) == 50.0F);
    CHECK(fine_at(still, 97, 55) == -2.0F);
    CHECK(fine_at(levels, 100 - hog::terrain::lake_margin, 55) == 50.0F);
    CHECK(fine_at(levels, 99 - hog::terrain::lake_margin, 55) == 0.0F);
    CHECK(fine_at(still, 99 - hog::terrain::lake_margin, 55) == hog::terrain::shore_highest);
    // Over lower ground with no lower lake by, kept, but told dry, and kept on past it.
    CHECK(fine_at(levels, 90, 55) == 50.0F);
    CHECK(fine_at(still, 90, 55) == hog::terrain::shore_highest);
    CHECK(fine_at(levels, 89, 55) == 50.0F);
    CHECK(fine_at(still, 89, 55) == -2.0F);
    // Never over lower ground, which the lower lake's margin reaches instead.
    CHECK(fine_at(levels, 110, 55) == 50.0F);
    CHECK(fine_at(levels, 111, 55) == 20.0F);
    CHECK(fine_at(levels, 112, 55) == 20.0F);
    CHECK(fine_at(still, 112, 55) == -10.0F);
    // The lower lake keeps its own.
    CHECK(fine_at(levels, 114, 55) == 20.0F);
    CHECK(fine_at(still, 114, 55) == 5.0F);
    // Nor where there's no lake.
    CHECK(levels.at(layer_texels + 10) == 0.0F);
}

TEST_CASE("the sea never floods a lake's bed", "[terrain][shore]") {
    const hog::terrain::ShoreGround ground{.elevations = {2.0F, 2.0F, -3.0F, -3.0F},
                                           .lakes = {3.0F, 0.0F, 0.0F, 0.0F},
                                           .sea = {0, 0, 1, 1}};
    const std::vector<float> depths = shore_depths(ground);
    CHECK(depths == std::vector<float>{hog::terrain::shore_highest, -2.0F, 3.0F, 3.0F});
}

TEST_CASE("a lake on the terrain has its level and depth round it", "[terrain][shore]") {
    // A lake's middle, the first found along a spiral over the planet.
    std::optional<DVec3> found;
    constexpr int points = 40'000;
    for (int i = 0; i < points && !found; ++i) {
        const double z = 1.0 - ((2.0 * (i + 0.5)) / points);
        const double around = i * std::numbers::pi * (3.0 - std::sqrt(5.0));
        const double across = std::sqrt(1.0 - (z * z));
        const DVec3 at{across * std::cos(around), across * std::sin(around), z};
        if (lakeland().lake(hog::planet::cell_at(at))) {
            found = at;
        }
    }
    REQUIRE(found.has_value());
    const DVec3 at = found.value_or(DVec3{0.0, 0.0, 1.0});
    const auto found_lake = lakeland().lake(hog::planet::cell_at(at));
    REQUIRE(found_lake.has_value());
    const hog::planet::Channel lake = found_lake.value_or(hog::planet::Channel{});
    const double level = hog::planet::fixed_to_double(lake.surface);
    const double bed = hog::planet::fixed_to_double(lake.bed);
    const hog::terrain::ShoreDepths shore =
        hog::terrain::work_out_shore(lake_world(), shore_frame(at, radius), radius);
    // The texel the lake's middle lies in.
    const DVec2 along = shore_along(shore.frame, radius, at);
    const double spacing = shore_spacing.front();
    const auto texel = [&](double metres) {
        return static_cast<int>(std::floor((metres / spacing) + (shore_size / 2.0)));
    };
    const std::size_t middle = (static_cast<std::size_t>(texel(along.y)) * shore_size) +
                               static_cast<std::size_t>(texel(along.x));
    const DVec3 centre = shore_direction(shore.frame, radius,
                                         {(texel(along.x) + 0.5 - (shore_size / 2.0)) * spacing,
                                          (texel(along.y) + 0.5 - (shore_size / 2.0)) * spacing});
    const auto found_under = lakeland().lake(hog::planet::cell_at(centre));
    REQUIRE(found_under.has_value());
    const hog::planet::Channel under = found_under.value_or(hog::planet::Channel{});
    CHECK_THAT(shore.levels.at(middle),
               WithinAbs(hog::planet::fixed_to_double(under.surface), 1e-3));
    CHECK_THAT(shore.still.at(middle),
               WithinAbs(hog::planet::fixed_to_double(under.surface - under.bed), 1e-3));
    CHECK(shore.depths.at(middle) == hog::terrain::shore_highest);
    CHECK(level > bed);
}

TEST_CASE("a river's plume is strongest at its mouth and mixes away out to sea",
          "[terrain][shore]") {
    const hog::terrain::Plume plume{
        .origin = {0.0, 0.0}, .heading = {1.0, 0.0}, .half = 4.0, .length = 100.0};
    CHECK_THAT(hog::terrain::plume_at(plume, {0.0, 0.0}), WithinAbs(1.0, 1e-9));
    CHECK(hog::terrain::plume_at(plume, {10.0, 0.0}) > hog::terrain::plume_at(plume, {50.0, 0.0}));
    CHECK(hog::terrain::plume_at(plume, {300.0, 0.0}) < 0.1);
    // Behind the shore it fills the river's mouth, and no further.
    CHECK(hog::terrain::plume_at(plume, {-4.0, 0.0}) > 0.0);
    CHECK(hog::terrain::plume_at(plume, {-20.0, 0.0}) == 0.0);
}

TEST_CASE("a river's plume widens as it goes out", "[terrain][shore]") {
    const hog::terrain::Plume plume{
        .origin = {0.0, 0.0}, .heading = {0.0, 1.0}, .half = 4.0, .length = 100.0};
    CHECK(hog::terrain::plume_at(plume, {20.0, 100.0}) > 0.0);
    CHECK(hog::terrain::plume_at(plume, {20.0, 10.0}) == 0.0);
    CHECK(hog::terrain::plume_at(plume, {80.0, 100.0}) == 0.0);
    CHECK(hog::terrain::plume_at(plume, {-20.0, 100.0}) ==
          hog::terrain::plume_at(plume, {20.0, 100.0}));
}

TEST_CASE("eddies leave more or less of a plume about its front", "[terrain][shore]") {
    CHECK(hog::terrain::plume_front(0.5, 1.0) > hog::terrain::plume_front(0.5, 0.0));
    CHECK(hog::terrain::plume_front(0.5, 0.0) > hog::terrain::plume_front(0.5, -0.5));
    CHECK(hog::terrain::plume_front(0.0, 1.0) == 0.0);
    CHECK(hog::terrain::plume_front(1.0, 1.0) == 1.0);
    CHECK(hog::terrain::plume_front(0.2, -1.0) == 0.0);
}

TEST_CASE("bigger rivers carry their plumes further", "[terrain][shore]") {
    CHECK(hog::terrain::plume_length(0.0) == 0.0);
    CHECK(hog::terrain::plume_length(10.0) > hog::terrain::plume_length(1.0));
    CHECK_THAT(hog::terrain::plume_length(4.0), WithinRel(2.0 * hog::terrain::plume_length(1.0)));
}

TEST_CASE("rivers' plumes lie off their mouths round the anchor", "[terrain][shore]") {
    const hog::planet::Reach* mouth = nullptr;
    for (const auto& reach : terrain().rivers().reaches()) {
        if (reach.mouth && reach.to_surface == 0 && reach.flow > 1.0) {
            mouth = &reach;
            break;
        }
    }
    REQUIRE(mouth != nullptr);
    const ShoreFrame frame = shore_frame(mouth->to, radius);
    const std::vector<float> plumes = hog::terrain::shore_plumes(world(), frame, radius);
    constexpr auto texels = static_cast<std::size_t>(shore_size) * shore_size;
    REQUIRE(plumes.size() == shore_spacing.size() * texels);
    // At the shore, in the finest layer.
    const double shore = hog::terrain::river_shore(terrain(), *mouth, radius);
    const DVec2 along =
        shore_along(frame, radius,
                    terrain().rivers().meander().unshifted(hog::planet::course_at(*mouth, shore)));
    const double spacing = shore_spacing.front();
    const auto texel = [&](double metres) {
        return static_cast<std::size_t>(std::floor((metres / spacing) + (shore_size / 2.0)));
    };
    // Texels lie further apart than narrow rivers are wide.
    float near = 0.0F;
    for (std::size_t row = texel(along.y) - 3; row <= texel(along.y) + 3; ++row) {
        for (std::size_t column = texel(along.x) - 3; column <= texel(along.x) + 3; ++column) {
            near = std::max(near, plumes.at((row * shore_size) + column));
        }
    }
    CHECK(near > 0.2F);
    // Little of the water's mixed with any.
    const auto mixed = std::ranges::count_if(std::span{plumes}.first(texels),
                                             [](float plume) { return plume > 0.0F; });
    CHECK(mixed > 0);
    CHECK(static_cast<std::size_t>(mixed) < texels / 10);
    CHECK(std::ranges::all_of(plumes, [](float plume) { return plume >= 0.0F && plume <= 1.0F; }));
}

TEST_CASE("the water nearest below a height is the highest lake under it, or the sea",
          "[terrain][shore]") {
    // Two lakes in the fine layer, one under the other's margin, and one only in the coarse.
    hog::terrain::ShoreDepths shore;
    shore.levels = layered(
        [](int column, int) {
            if (column < 10) {
                return 120.0F;
            }
            return column < 20 ? 35.5F : 0.0F;
        },
        [](int, int) { return 900.0F; });
    shore.still = layered([](int column, int) { return column == 9 ? -2.0F : 3.0F; },
                          [](int, int) { return 3.0F; });
    const std::vector<float> levels = hog::terrain::lake_levels(shore);
    CHECK(levels == std::vector<float>{35.5F, 120.0F});
    CHECK(hog::terrain::water_below(levels, 2'000.0) == 120.0);
    CHECK(hog::terrain::water_below(levels, 120.0) == 120.0);
    CHECK(hog::terrain::water_below(levels, 100.0) == 35.5);
    CHECK(hog::terrain::water_below(levels, 20.0) == 0.0);
    CHECK(hog::terrain::water_below({}, 20.0) == 0.0);
}
