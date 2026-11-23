#include "held_chunks.hpp"
#include "planted_ground.hpp"

#include <hog/client/forest.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/tree_stands.hpp>
#include <hog/flora/placement.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>

using hog::DVec3;
using hog::client::Grove;
using hog::grid::Key;
using namespace hog::planted;

namespace {

hog::flora::Planting planting() {
    return {.species = {{.kind = hog::flora::Kind::Deciduous, .variants = 8, .impostors = 4}},
            .spacing = 7.0};
}

constexpr DVec3 slope{0.1, 0.05, 1.0};

} // namespace

TEST_CASE("woods nodes near the eye are those the ground crosses, nearest first",
          "[client][forest]") {
    const Ground ground = ground_of(slope, 1.0);
    const hog::grid::Grid& grid = ground.pieces.grid();
    const DVec3 eye = on + DVec3{0.0, 0.0, 2.0};
    const auto nodes = hog::client::woods_nodes_near(ground.pieces, eye, 30.0);
    REQUIRE(!nodes.empty());
    double last = 0.0;
    for (const Key node : nodes) {
        CHECK(node.depth == hog::client::woods_depth(grid));
        CHECK(piece_root.contains(node));
        const double away = grid.distance(node, eye);
        CHECK(away <= 30.0);
        CHECK(away >= last);
        last = away;
        // The plane passes through it.
        CHECK(std::abs(ground.above(grid.centre(node))) < 8.0 * std::numbers::sqrt3);
    }
    // Over all of the piece, every node with trees in it.
    const auto all = hog::client::woods_nodes_near(ground.pieces, eye, 1000.0);
    std::size_t with_trees = 0;
    for (std::uint32_t z = 0; z < 4; ++z) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const Key node{.depth = hog::client::woods_depth(grid),
                               .x = (piece_root.x * 4) + x,
                               .y = (piece_root.y * 4) + y,
                               .z = (piece_root.z * 4) + z};
                if (!hog::client::trees_in(ground.pieces, node, 9U, planting()).empty()) {
                    ++with_trees;
                    CHECK(std::ranges::find(all, node) != all.end());
                }
            }
        }
    }
    CHECK(with_trees > 0);
}

TEST_CASE("the forest plants on the jobs, and again only when something changed",
          "[client][forest]") {
    const hog::grid::Grid grid{reach, 1.0};
    hog::jobs::JobSystem jobs{2};
    hog::client::HeldPieces held{grid};
    REQUIRE(hog::testing::hold(held, *root_of(slope)));
    REQUIRE(hog::testing::hold(held, *piece_of(slope, 1.0)));
    hog::client::Forest forest{grid, jobs, planting(), 9U, 600.0};
    const DVec3 eye = on + DVec3{0.0, 0.0, 2.0};
    forest.want(held, eye, true);
    forest.finish();
    const std::optional<Grove> first = forest.collect();
    REQUIRE(first.has_value());
    CHECK(first.value().eye == eye);
    const Grove direct = hog::client::plant_around(
        hog::client::PiecesSnapshot{grid, held.entries()}, eye, 600.0, 9U, planting());
    REQUIRE(first.value().trees.size() == direct.trees.size());
    CHECK(!direct.trees.empty());
    CHECK(!forest.collect().has_value());

    forest.want(held, eye + DVec3{20.0, 0.0, 0.0}, false);
    forest.finish();
    CHECK(!forest.collect().has_value());

    forest.want(held, eye, true);
    forest.finish();
    CHECK(forest.collect().has_value());

    forest.want(held, eye + DVec3{150.0, 0.0, 0.0}, false);
    forest.finish();
    CHECK(forest.collect().has_value());
}
