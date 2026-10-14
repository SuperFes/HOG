#include "shared_world.hpp"

#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

using hog::DVec3;
using hog::planet::CellId;
using hog::planet::Change;
using hog::planet::fixed_one;
using hog::planet::World;
using hog::terrain::bake_patch;
using hog::terrain::canonical;
using hog::terrain::Corner;
using hog::terrain::direction;
using hog::terrain::Patch;
using hog::terrain::patch_side;
using hog::terrain::sample_cell;
using hog::terrain::testing::land_patch;
using hog::terrain::testing::planet;
using hog::terrain::testing::shared_world;

namespace {

DVec3 world_at(const Patch& patch, std::size_t vertex) {
    return (patch.origin + DVec3(patch.vertices.at(vertex).position)).metres();
}

std::size_t vertex_index(int i, int j) {
    return (static_cast<std::size_t>(j) * patch_side) + static_cast<std::size_t>(i);
}

bool on_border(std::size_t vertex) {
    const auto i = static_cast<int>(vertex % patch_side);
    const auto j = static_cast<int>(vertex / patch_side);
    return i == 0 || j == 0 || i == patch_side - 1 || j == patch_side - 1;
}

// The vertex of `patch` within `tolerance` metres of `point`, if one is.
std::optional<std::size_t> vertex_near(const Patch& patch, DVec3 point, double tolerance) {
    for (std::size_t vertex = 0; vertex < patch.vertices.size(); ++vertex) {
        if (glm::distance(world_at(patch, vertex), point) <= tolerance) {
            return vertex;
        }
    }
    return std::nullopt;
}

// Floats a few kilometres from their origin are good to well under a millimetre.
constexpr double near_enough = 0.002;

} // namespace

TEST_CASE("a corner on a face's edge is named alike from either face", "[terrain][patch]") {
    // Face 0's +u edge is face 1's +v edge: y = 1 on +x is x = 1 on +y.
    const Corner from_x{.face = 0, .i = 16, .j = 5, .level = 4};
    const Corner from_y{.face = 1, .i = 5, .j = 16, .level = 4};
    CHECK(canonical(from_x) == from_x);
    CHECK(canonical(from_y) == from_x);
    CHECK(direction(from_y) == direction(from_x));
    CHECK(sample_cell(from_y) == sample_cell(from_x));
}

TEST_CASE("every naming of a corner gives the same bits", "[terrain][patch]") {
    constexpr int level = 3;
    constexpr std::int64_t side = 1 << level;
    // Points on the edges of faces, grouped by where they are.
    std::map<std::tuple<long, long, long>, std::vector<Corner>> by_point;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        for (std::int64_t j = 0; j <= side; ++j) {
            for (std::int64_t i = 0; i <= side; ++i) {
                if (i != 0 && j != 0 && i != side && j != side) {
                    continue;
                }
                const Corner corner{.face = face, .i = i, .j = j, .level = level};
                const DVec3 at = direction(corner) * 1e6;
                by_point[{std::lround(at.x), std::lround(at.y), std::lround(at.z)}].push_back(
                    corner);
            }
        }
    }
    // Each face has 4 * side edge corners; corners of the cube are shared three ways and the
    // rest two ways.
    CHECK(by_point.size() == 8 + (12 * (side - 1)));
    for (const auto& [point, corners] : by_point) {
        CHECK((corners.size() == 2 || corners.size() == 3));
        for (const Corner& corner : corners) {
            CHECK(canonical(corner) == canonical(corners.front()));
            CHECK(direction(corner) == direction(corners.front()));
            CHECK(sample_cell(corner) == sample_cell(corners.front()));
        }
    }
}

TEST_CASE("a corner is the same at every level it lies on", "[terrain][patch]") {
    // On face 0's +u edge, which face 0 names, being the lowest face there.
    const Corner coarse{.face = 0, .i = 8, .j = 3, .level = 3};
    const Corner fine{.face = 0, .i = 8 << 9, .j = 3 << 9, .level = 12};
    CHECK(direction(fine) == direction(coarse));
    CHECK(sample_cell(fine) == sample_cell(coarse));
    // The far edge takes the last leaf inside the face.
    CHECK(sample_cell(coarse).to_face_ij().i == (1U << hog::planet::max_level) - 1);
}

TEST_CASE("a step off a face lands a step onto the next", "[terrain][patch]") {
    constexpr int level = 4;
    const Corner edge{.face = 0, .i = 16, .j = 5, .level = level};
    const Corner inside{.face = 0, .i = 15, .j = 5, .level = level};
    const Corner beyond = canonical({.face = 0, .i = 17, .j = 5, .level = level});
    CHECK(beyond.face != 0);
    const double step = glm::distance(direction(inside), direction(edge));
    CHECK(std::abs(glm::distance(direction(beyond), direction(edge)) - step) < 0.1 * step);
    CHECK(glm::dot(direction(beyond) - direction(edge), direction(edge) - direction(inside)) > 0.0);

    const Corner below = canonical({.face = 0, .i = -1, .j = 5, .level = level});
    CHECK(below.face != 0);
    CHECK(below.i >= 0);
    CHECK(below.j >= 0);
}

TEST_CASE("a corner off its face both ways is refused", "[terrain][patch]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(canonical({.face = 2, .i = -1, .j = 17, .level = 4}),
                    hog::testing::AssertionFailed);
    CHECK_THROWS_AS(canonical({.face = 2, .i = 3, .j = 19, .level = 4}),
                    hog::testing::AssertionFailed);
}

TEST_CASE("a patch lies on the ground over its cell", "[terrain][patch]") {
    const World& world = shared_world();
    const CellId cell = land_patch();
    const Patch patch = bake_patch(world, cell);
    REQUIRE(patch.vertices.size() == std::size_t{patch_side} * patch_side);
    CHECK(std::abs(glm::length(patch.origin.metres()) - planet.radius) < 1e-6);

    const hog::planet::FaceIJ at = cell.to_face_ij();
    double lowest = 1e9;
    double highest = -1e9;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const Corner corner{.face = at.face,
                                .i = (std::int64_t{at.i} << hog::terrain::patch_depth) + i,
                                .j = (std::int64_t{at.j} << hog::terrain::patch_depth) + j,
                                .level = at.level + hog::terrain::patch_depth};
            const auto state = world.state(sample_cell(corner));
            const double height = static_cast<double>(state.elevation) / fixed_one;
            const DVec3 point = world_at(patch, vertex_index(i, j));
            CHECK(std::abs(glm::length(point) - planet.radius - height) < near_enough);
            CHECK(glm::length(glm::normalize(point) - direction(corner)) < 1e-9);
            CHECK(patch.vertices.at(vertex_index(i, j)).biome == state.biome);
            lowest = std::min(lowest, height);
            highest = std::max(highest, height);
        }
    }
    CHECK(highest > 0.0);
    CHECK(highest - lowest > 1.0);
    CHECK(patch.heights == hog::terrain::HeightRange{.lowest = lowest, .highest = highest});
}

TEST_CASE("neighbouring patches meet along their edges", "[terrain][patch]") {
    const World& world = shared_world();
    // One patch inside a face and one at a face's corner, whose neighbours lie on other faces.
    const CellId inner = land_patch();
    const CellId cornered = CellId::from_face_ij(3, 0, 0, 6);
    for (const CellId cell : {inner, cornered}) {
        const Patch patch = bake_patch(world, cell);
        for (const CellId next : hog::planet::edge_neighbors(cell)) {
            const Patch neighbour = bake_patch(world, next);
            int shared = 0;
            for (std::size_t vertex = 0; vertex < patch.vertices.size(); ++vertex) {
                if (!on_border(vertex)) {
                    continue;
                }
                const auto match = vertex_near(neighbour, world_at(patch, vertex), near_enough);
                if (!match) {
                    continue;
                }
                ++shared;
                CHECK(on_border(*match));
                CHECK(glm::dot(patch.vertices.at(vertex).normal,
                               neighbour.vertices.at(*match).normal) > 0.9999F);
            }
            CHECK(shared == patch_side);
        }
    }
}

TEST_CASE("a patch's children meet it at its own vertices", "[terrain][patch]") {
    const World& world = shared_world();
    const CellId cell = land_patch();
    const Patch parent = bake_patch(world, cell);
    double lowest = 1e9;
    double highest = -1e9;
    for (int index = 0; index < 4; ++index) {
        const Patch child = bake_patch(world, cell.child(index));
        int shared = 0;
        for (int j = 0; j < patch_side; j += 2) {
            for (int i = 0; i < patch_side; i += 2) {
                if (vertex_near(parent, world_at(child, vertex_index(i, j)), near_enough)) {
                    ++shared;
                }
            }
        }
        CHECK(shared == ((patch_side + 1) / 2) * ((patch_side + 1) / 2));
        lowest = std::min(lowest, child.heights.lowest);
        highest = std::max(highest, child.heights.highest);
    }
    // Every vertex of the parent is one of its children's.
    CHECK(lowest <= parent.heights.lowest);
    CHECK(highest >= parent.heights.highest);
}

TEST_CASE("a patch faces outwards and tilts with its slopes", "[terrain][patch]") {
    const Patch patch = bake_patch(shared_world(), land_patch());
    float least = 1.0F;
    for (std::size_t vertex = 0; vertex < patch.vertices.size(); ++vertex) {
        const auto up = hog::Vec3(glm::normalize(world_at(patch, vertex)));
        const float upright = glm::dot(patch.vertices.at(vertex).normal, up);
        CHECK(upright > 0.0F);
        least = std::min(least, upright);
    }
    CHECK(least < 0.9999F);
}

TEST_CASE("a patch shows changes to the ground", "[terrain][patch]") {
    World& world = shared_world();
    const CellId cell = land_patch();
    constexpr std::int64_t raised = 1'234 * fixed_one;
    constexpr std::int64_t dug = 34 * fixed_one;
    const auto change = [&](hog::planet::ChangePayload payload) {
        return Change{.cell = cell,
                      .time = {},
                      .author = {},
                      .layer = hog::planet::Layer::Player,
                      .payload = payload};
    };
    REQUIRE(world.apply(change(hog::planet::SetElevation{.elevation = raised})));
    REQUIRE(world.apply(change(hog::planet::Excavate{.depth = dug})));
    const Patch patch = bake_patch(world, cell);
    world.roll_back({});

    // The far edges take their ground from the cells beyond.
    const DVec3 point = world_at(patch, vertex_index(10, 20));
    CHECK(std::abs(glm::length(point) - planet.radius - 1'200.0) < near_enough);
}

TEST_CASE("a patch too fine for its vertices is refused", "[terrain][patch]") {
    const hog::testing::AssertionGuard guard;
    const CellId leafward = CellId::from_face_ij(1, 0, 0, hog::terrain::max_patch_level + 1);
    CHECK_THROWS_AS(bake_patch(shared_world(), leafward), hog::testing::AssertionFailed);
}
