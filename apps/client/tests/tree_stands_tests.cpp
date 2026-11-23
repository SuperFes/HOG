#include "planted_ground.hpp"

#include <hog/client/held_pieces.hpp>
#include <hog/client/tree_stands.hpp>
#include <hog/flora/placement.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::client::StandingTree;
using hog::grid::Key;

namespace {

using namespace hog::planted;

hog::flora::Planting planting() {
    return {.species = {{.kind = hog::flora::Kind::Deciduous, .variants = 8, .impostors = 4}},
            .spacing = 7.0};
}

std::vector<std::pair<Key, StandingTree>> every_tree(const Ground& ground) {
    const std::uint8_t depth = hog::client::woods_depth(ground.pieces.grid());
    std::vector<std::pair<Key, StandingTree>> out;
    for (std::uint32_t z = 0; z < 4; ++z) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const Key node{.depth = depth,
                               .x = (piece_root.x * 4) + x,
                               .y = (piece_root.y * 4) + y,
                               .z = (piece_root.z * 4) + z};
                for (const StandingTree& tree :
                     hog::client::trees_in(ground.pieces, node, 9U, planting())) {
                    out.emplace_back(node, tree);
                }
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("woods nodes are the finest at least 16 m across", "[client][tree_stands]") {
    CHECK(hog::client::woods_depth(hog::grid::Grid{reach, 1.0}) == 8);
    CHECK(hog::client::woods_depth(hog::grid::Grid{50000.0, 0.25}) ==
          hog::grid::Grid{50000.0, 0.25}.depth() - 6);
    CHECK(hog::client::woods_depth(hog::grid::Grid{50000.0, 1.0 / 3.0}) ==
          hog::grid::Grid{50000.0, 1.0 / 3.0}.depth() - 6);
}

TEST_CASE("a line down meets the ground where the held surface lies", "[client][tree_stands]") {
    const Ground ground = ground_of({0.1, 0.05, 1.0}, 1.0);
    for (const auto& [x, y] : {std::pair{3.3, 60.1}, std::pair{32.0, 32.0}, std::pair{50.7, 9.2}}) {
        CAPTURE(x, y);
        const auto hit =
            hog::client::ground_along(ground.pieces, DVec3{x, y, 1020.0}, DVec3{x, y, 965.0});
        REQUIRE(hit.has_value());
        CHECK(std::abs(ground.above(hit.value().at)) < 1e-3);
        CHECK(glm::dot(hit.value().normal, ground.normal) > 0.999);
        // Slanting.
        const auto slant = hog::client::ground_along(ground.pieces, DVec3{x, y, 1020.0},
                                                     DVec3{64.0 - x, 64.0 - y, 965.0});
        REQUIRE(slant.has_value());
        CHECK(std::abs(ground.above(slant.value().at)) < 1e-3);
    }
}

TEST_CASE("lines in the air, rising out of the ground or leaving what's held meet none",
          "[client][tree_stands]") {
    const Ground ground = ground_of({0.1, 0.05, 1.0}, 1.0);
    CHECK_FALSE(hog::client::ground_along(ground.pieces, DVec3{20, 20, 1020}, DVec3{20, 20, 1000})
                    .has_value());
    CHECK_FALSE(hog::client::ground_along(ground.pieces, DVec3{20, 20, 965}, DVec3{20, 20, 1020})
                    .has_value());
    CHECK_FALSE(hog::client::ground_along(ground.pieces, DVec3{20, 20, 2100}, DVec3{20, 20, 965})
                    .has_value());
}

TEST_CASE("trees stand on the ground within their node, each once", "[client][tree_stands]") {
    const Ground ground = ground_of({0.1, 0.05, 1.0}, 1.0);
    const auto trees = every_tree(ground);
    std::set<std::tuple<double, double, double>> places;
    for (const auto& [node, tree] : trees) {
        CHECK(std::abs(ground.above(tree.at)) < 1e-3);
        CHECK(ground.pieces.grid().key_at(tree.at, node.depth) == node);
        places.emplace(tree.at.x, tree.at.y, tree.at.z);
    }
    CHECK(places.size() == trees.size());
    // One to a cell 16√3/4 m across, over the piece's 64 m.
    const double cell = 16.0 * std::numbers::sqrt3 / 4.0;
    const double expected = 64.0 * 64.0 / (cell * cell);
    CHECK(static_cast<double>(trees.size()) > 0.85 * expected);
    CHECK(static_cast<double>(trees.size()) < 1.15 * expected);
}

TEST_CASE("thinner woods keep some of the same trees", "[client][tree_stands]") {
    const auto whole = every_tree(ground_of({0.1, 0.05, 1.0}, 1.0));
    const auto thin = every_tree(ground_of({0.1, 0.05, 1.0}, 0.4));
    std::set<std::tuple<double, double, double>> places;
    for (const auto& [node, tree] : whole) {
        places.emplace(tree.at.x, tree.at.y, tree.at.z);
    }
    for (const auto& [node, tree] : thin) {
        CHECK(places.contains({tree.at.x, tree.at.y, tree.at.z}));
    }
    const double share = static_cast<double>(thin.size()) / static_cast<double>(whole.size());
    CHECK(share > 0.25);
    CHECK(share < 0.55);
}

TEST_CASE("no tree stands on ground too steep", "[client][tree_stands]") {
    CHECK(every_tree(ground_of({1.0, 0.0, 0.6}, 1.0)).empty());
    CHECK_FALSE(every_tree(ground_of({0.5, 0.0, 1.0}, 1.0)).empty());
}
