#include "held_chunks.hpp"
#include "welding.hpp"

#include <hog/client/drawn.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/client/water_field.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/water_bodies.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::I64Vec3;
using hog::client::WaterField;
using hog::grid::Key;

namespace {

// 1 m nodes 11 deep: a piece of 64 m whose low corner lies 64 m below the planet's centre on x,
// its finest nodes marked with `body` wherever their middles lie less than `over` metres over
// `level` and short of `shore` metres along y; the root's piece round it holds no water, the finer
// piece's place in it left open for it.
constexpr Key piece_root{.depth = 5, .x = 15, .y = 16, .z = 16};
constexpr std::uint8_t top_levels = 5;
constexpr std::uint8_t piece_levels = 6;
constexpr double level = 50.0;
constexpr double shore = 40.0;

std::shared_ptr<const hog::grid::Piece> water_piece(Key root,
                                                    hog::grid::Tree<hog::grid::Water> tree) {
    auto piece = std::make_shared<hog::grid::Piece>();
    piece->root = root;
    piece->generator = 1;
    piece->water = std::move(tree);
    return piece;
}

struct Water {
    hog::grid::Grid grid{1000.0, 1.0};

    double over = 2.0;
    hog::client::PiecesSnapshot pieces;

    explicit Water(hog::grid::Water::Value body, double reach = 2.0)
        : over{reach}, pieces{grid, {{Key{}, top()}, {piece_root, piece(piece_root, body)}}} {}

    [[nodiscard]] static std::shared_ptr<const hog::grid::Piece> top() {
        using Leaf = hog::grid::Tree<hog::grid::Water>::Leaf;
        return water_piece(Key{}, hog::grid::Tree<hog::grid::Water>::grow(
                                      top_levels,
                                      [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                          std::uint32_t z) -> std::optional<Leaf> {
                                          const auto shift =
                                              static_cast<unsigned>(top_levels - depth);
                                          const bool over = piece_root.x >> shift == x &&
                                                            piece_root.y >> shift == y &&
                                                            piece_root.z >> shift == z;
                                          if (over && depth < top_levels) {
                                              return std::nullopt;
                                          }
                                          return Leaf{.value = 0, .open = over};
                                      }));
    }

    [[nodiscard]] std::shared_ptr<const hog::grid::Piece>
    piece(Key root, hog::grid::Water::Value body) const {
        const DVec3 low = grid.low(root);
        const double side = grid.size(static_cast<std::uint8_t>(root.depth + piece_levels));
        return water_piece(
            root, hog::grid::Tree<hog::grid::Water>::build(piece_levels, [&](int x, int y, int z) {
                const DVec3 middle = low + (DVec3{x + 0.5, y + 0.5, z + 0.5} * side);
                return glm::length(middle) < level + over && middle.y < shore
                           ? body
                           : hog::grid::Water::Value{0};
            }));
    }
};

hog::terrain::BlockMesh meshed(const Water& water, const hog::grid::WaterBodies& bodies,
                               hog::client::Block block) {
    WaterField field{water.pieces, bodies};
    const hog::client::ShapeMesh shape = mesh_block(water.grid, block, field.field());
    return hog::client::water_of(shape, field);
}

DVec3 placed(const hog::terrain::BlockMesh& mesh, const hog::terrain::BlockVertex& vertex) {
    return mesh.origin.metres() + DVec3{vertex.position};
}

} // namespace

TEST_CASE("the sea's surface stands at its level over what's marked as it",
          "[client][water_field]") {
    const Water water{1};
    hog::grid::WaterBodies bodies;
    bodies.bodies.emplace(1U, hog::grid::Sea{.level = level});

    const hog::terrain::BlockMesh mesh = meshed(water, bodies, {.node = piece_root, .cells = 5});
    REQUIRE_FALSE(mesh.indices.empty());
    int edge = 0;
    for (const hog::terrain::BlockVertex& vertex : mesh.vertices) {
        const DVec3 at = placed(mesh, vertex);
        CHECK(std::abs(glm::length(at) - level) < 1e-4);
        CHECK(at.y < shore + 2.0);
        CHECK((vertex.corner & hog::client::sea_corner) != 0);
        CHECK(vertex.tiles[0] == static_cast<std::uint8_t>(hog::globe::Material::Water));
        // Still, with no current.
        CHECK(vertex.normal == hog::Vec3{0.0F});
        edge += (vertex.corner & hog::client::shore_corner) != 0 ? 1 : 0;
    }
    // Where the marks stop short of the shore, the water's edge.
    CHECK(edge > 0);
    // Facing up, out of the water.
    for (std::size_t corner = 0; corner + 2 < mesh.indices.size(); corner += 3) {
        const DVec3 a = placed(mesh, mesh.vertices[mesh.indices[corner]]);
        const DVec3 b = placed(mesh, mesh.vertices[mesh.indices[corner + 1]]);
        const DVec3 c = placed(mesh, mesh.vertices[mesh.indices[corner + 2]]);
        CHECK(glm::dot(glm::cross(b - a, c - a), a) > 0.0);
    }
}

TEST_CASE("a lake's surface rises as its season has it, and an unknown body is dry",
          "[client][water_field]") {
    const Water water{5};
    hog::grid::WaterBodies bodies;
    bodies.bodies.emplace(5U,
                          hog::grid::Lake{.level = level, .season = {.rise = 2.0, .peak = 0.25}});

    const hog::terrain::BlockMesh mesh = meshed(water, bodies, {.node = piece_root, .cells = 4});
    REQUIRE_FALSE(mesh.indices.empty());
    for (const hog::terrain::BlockVertex& vertex : mesh.vertices) {
        CHECK(std::abs(glm::length(placed(mesh, vertex)) - level) < 1e-4);
        CHECK((vertex.corner & hog::client::sea_corner) == 0);
        CHECK(vertex.tiles[1] == hog::globe::rise_of(2.0));
        CHECK(vertex.tiles[2] == hog::globe::peak_of(0.25));
    }

    CHECK(meshed(water, {}, {.node = piece_root, .cells = 4}).indices.empty());
}

TEST_CASE("a river's surface falls along its course, its current running down it",
          "[client][water_field]") {
    const Water water{0x8000'0002U};
    hog::grid::WaterBodies bodies;
    // Down the piece along z, a metre lower at its foot.
    bodies.bodies.emplace(
        0x8000'0002U,
        hog::grid::River{
            .course = {{.place = DVec3{-30.0, 20.0, 0.0}, .level = level + 0.5, .flow = 8.0},
                       {.place = DVec3{-30.0, 20.0, 64.0}, .level = level - 0.5, .flow = 8.0}},
            .peak = 0.0});

    const hog::terrain::BlockMesh mesh = meshed(water, bodies, {.node = piece_root, .cells = 5});
    REQUIRE_FALSE(mesh.indices.empty());
    for (const hog::terrain::BlockVertex& vertex : mesh.vertices) {
        const DVec3 at = placed(mesh, vertex);
        const double standing = level + 0.5 - (std::clamp(at.z, 0.0, 64.0) / 64.0);
        CHECK(std::abs(glm::length(at) - standing) < 0.05);
        const DVec3 current{vertex.normal};
        const DVec3 up = glm::normalize(at);
        // On this small a planet, the course climbs straight up from its middle in places.
        if (std::abs(up.z) > 0.5) {
            continue;
        }
        CHECK(std::abs(glm::length(current) - hog::grid::running_speed(8.0)) < 1e-5);
        CHECK(std::abs(glm::dot(current, up)) < 1e-5);
        CHECK(glm::dot(current, DVec3{0.0, 0.0, 1.0}) > 0.0);
    }
}

TEST_CASE("blocks of water meet finer ones on the sea's surface", "[client][water_field]") {
    const Water water{1};
    hog::grid::WaterBodies bodies;
    bodies.bodies.emplace(1U, hog::grid::Sea{.level = level});
    WaterField field{water.pieces, bodies};
    const std::array<Key, 1> split{piece_root.child(5)};
    std::size_t triangles = 0;
    for (const hog::client::Block& block : hog::welding::blocks_of(piece_root, split, 3)) {
        const hog::terrain::BlockMesh mesh =
            hog::client::water_of(mesh_block(water.grid, block, field.field()), field);
        triangles += mesh.indices.size() / 3;
        for (const hog::terrain::BlockVertex& vertex : mesh.vertices) {
            // Moved in along finer sides only across the surface, never off it by much.
            CHECK(std::abs(glm::length(placed(mesh, vertex)) - level) < 0.1);
        }
    }
    CHECK(triangles > 0);
}

TEST_CASE("cells coarser than the water's marks still find its surface", "[client][water_field]") {
    // Marked as the server marks them: some of a node under the level, so a coarse cell's corner
    // over the water lies in nodes wholly over it.
    const Water water{1, std::numbers::sqrt3 / 2.0};
    hog::grid::WaterBodies bodies;
    bodies.bodies.emplace(1U, hog::grid::Sea{.level = level});
    constexpr double cell = 8.0;
    const hog::terrain::BlockMesh mesh = meshed(water, bodies, {.node = piece_root, .cells = 3});
    REQUIRE_FALSE(mesh.indices.empty());
    int inland = 0;
    for (const hog::terrain::BlockVertex& vertex : mesh.vertices) {
        const DVec3 at = placed(mesh, vertex);
        CHECK(std::abs(glm::length(at) - level) < 1e-4);
        // The water ends only at the shore, not across the open water short of it.
        if ((vertex.corner & hog::client::shore_corner) != 0 && at.y < shore - cell) {
            ++inland;
        }
    }
    CHECK(inland == 0);
}

TEST_CASE("only a node marked somewhere may hold water", "[client][water_field]") {
    const Water water{1};
    CHECK(hog::client::may_hold_water(water.pieces, piece_root));
    CHECK_FALSE(hog::client::may_hold_water(water.pieces, Key{.depth = 5, .x = 0, .y = 0, .z = 0}));
}

TEST_CASE("the eye is in a lake where the pieces mark it, not in the sea",
          "[client][water_field]") {
    const Water lake_water{5};
    hog::client::HeldPieces held{lake_water.grid};
    REQUIRE(hog::testing::hold(held, *lake_water.top()));
    REQUIRE(hog::testing::hold(held, *lake_water.piece(piece_root, 5)));
    hog::grid::WaterBodies bodies;
    bodies.bodies.emplace(1U, hog::grid::Sea{.level = level});
    bodies.bodies.emplace(5U,
                          hog::grid::Lake{.level = level, .season = {.rise = 1.0, .peak = 0.5}});
    REQUIRE(held.take_bodies(bodies.encode()));

    const DVec3 under{-20.0, 10.0, 30.0};
    const auto in = hog::client::still_water_in(held, under);
    REQUIRE(in.has_value());
    const hog::globe::StandingWater standing = in.value();
    CHECK(standing.top == level);
    CHECK(standing.rise == 1.0);
    CHECK(standing.peak == 0.5);
    // Dry past the shore, and far from the finer piece.
    CHECK_FALSE(hog::client::still_water_in(held, DVec3{-20.0, 45.0, 10.0}).has_value());
    CHECK_FALSE(hog::client::still_water_in(held, DVec3{500.0, 500.0, 500.0}).has_value());

    const Water sea_water{1};
    hog::client::HeldPieces sea{sea_water.grid};
    REQUIRE(hog::testing::hold(sea, *sea_water.piece(piece_root, 1)));
    REQUIRE(sea.take_bodies(bodies.encode()));
    CHECK_FALSE(hog::client::still_water_in(sea, under).has_value());
}
