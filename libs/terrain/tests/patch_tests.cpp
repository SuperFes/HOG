#include "shared_world.hpp"

#include <hog/flora/stand.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <set>
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
using hog::terrain::GridPoint;
using hog::terrain::Patch;
using hog::terrain::patch_cells;
using hog::terrain::patch_depth;
using hog::terrain::patch_side;
using hog::terrain::reaches;
using hog::terrain::sample_cell;
using hog::terrain::testing::land_patch;
using hog::terrain::testing::planet;
using hog::terrain::testing::shared_world;

namespace {

DVec3 world_at(const Patch& patch, std::size_t vertex) {
    return (patch.origin + DVec3(patch.vertices.at(vertex).position)).metres();
}

constexpr auto grid_vertices = std::size_t{hog::terrain::patch_grid_vertices};

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
    for (std::size_t vertex = 0; vertex < grid_vertices; ++vertex) {
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
    REQUIRE(patch.vertices.size() == std::size_t{hog::terrain::patch_vertices});
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
            for (std::size_t vertex = 0; vertex < grid_vertices; ++vertex) {
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

TEST_CASE("a patch's skirts hang under its edges, lit alike", "[terrain][patch]") {
    const Patch patch = bake_patch(shared_world(), land_patch());
    const double spacing = planet.radius * std::numbers::pi / 2.0 /
                           static_cast<double>(1 << (land_patch().level() + patch_depth));
    for (int edge = 0; edge < 4; ++edge) {
        for (int along = 0; along < patch_side; ++along) {
            const GridPoint above = hog::terrain::on_edge(edge, along);
            const auto skirt = static_cast<std::size_t>(hog::terrain::skirt_vertex(edge, along));
            const std::size_t top = vertex_index(above.i, above.j);
            const DVec3 bottom = world_at(patch, skirt);
            CHECK(glm::length(glm::normalize(bottom) - glm::normalize(world_at(patch, top))) <
                  1e-6);
            CHECK(glm::length(bottom) - planet.radius <
                  patch.heights.lowest - (patch_cells / 4.0 * spacing) + near_enough);
            CHECK(patch.vertices.at(skirt).normal == patch.vertices.at(top).normal);
            CHECK(patch.vertices.at(skirt).biome == patch.vertices.at(top).biome);
        }
    }
}

TEST_CASE("a patch's skirts face out of it", "[terrain][patch]") {
    const Patch patch = bake_patch(shared_world(), land_patch());
    const DVec3 up = glm::normalize(patch.origin.metres());
    for (const std::uint8_t edges : {std::uint8_t{0}, std::uint8_t{15}}) {
        const auto indices = hog::terrain::skirt_indices(edges);
        REQUIRE(!indices.empty());
        for (std::size_t first = 0; first < indices.size(); first += 3) {
            const DVec3 a = world_at(patch, indices[first]);
            const DVec3 b = world_at(patch, indices[first + 1]);
            const DVec3 c = world_at(patch, indices[first + 2]);
            DVec3 out = ((a + b + c) / 3.0) - patch.origin.metres();
            out -= up * glm::dot(out, up);
            CHECK(glm::dot(glm::cross(b - a, c - a), out) > 0.0);
        }
    }
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

TEST_CASE("a patch is altered only by changes that reach it", "[terrain][patch]") {
    World& world = shared_world();
    const CellId patch = land_patch();
    const Patch before = bake_patch(world, patch);
    const auto altered_by = [&](CellId cell) {
        REQUIRE(
            world.apply({.cell = cell,
                         .time = {},
                         .author = {},
                         .layer = hog::planet::Layer::Player,
                         .payload = hog::planet::SetElevation{.elevation = 3'000 * fixed_one}}));
        const Patch after = bake_patch(world, patch);
        world.roll_back({});
        return !std::ranges::equal(
            before.vertices, after.vertices, [](const auto& a, const auto& b) {
                return a.position == b.position && a.normal == b.normal && a.biome == b.biome;
            });
    };

    const auto beside = hog::planet::edge_neighbors(patch);
    for (const CellId cell : {patch, patch.child(1).child(2), patch.parent(3), beside[0], beside[1],
                              beside[2], beside[3].child(0)}) {
        CAPTURE(cell.raw());
        CHECK(reaches(patch, cell));
        CHECK(altered_by(cell));
    }
    CHECK(!reaches(patch, hog::planet::edge_neighbors(patch.parent(4))[0]));
    // Every cell just beyond those beside it.
    std::set<CellId> beyond;
    const auto beside_patch = hog::planet::vertex_neighbors(patch);
    for (const CellId next : beside_patch.cells()) {
        const auto beside_next = hog::planet::vertex_neighbors(next);
        for (const CellId cell : beside_next.cells()) {
            if (!reaches(patch, cell)) {
                beyond.insert(cell);
            }
        }
    }
    CHECK(beyond.size() == 16);
    for (const CellId cell : beyond) {
        CAPTURE(cell.raw());
        CHECK(!altered_by(cell));
    }
}

TEST_CASE("a patch too fine for its vertices is refused", "[terrain][patch]") {
    const hog::testing::AssertionGuard guard;
    const CellId leafward = CellId::from_face_ij(1, 0, 0, hog::terrain::max_patch_level + 1);
    CHECK_THROWS_AS(bake_patch(shared_world(), leafward), hog::testing::AssertionFailed);
}

TEST_CASE("woods pack a byte a measure, crowding and height to their own ranges",
          "[terrain][patch]") {
    const hog::terrain::Woods woods = hog::terrain::packed_woods(
        {.grown = 0.6F, .height = 16.0F, .broadleaf = 0.5F, .maple = 1.0F, .palm = 0.25F}, 120.0);
    // Half a tree a hundred square metres, a quarter of the most.
    CHECK(woods == hog::terrain::Woods{64, 128, 128, 255, 64});
    CHECK(hog::terrain::packed_woods({.grown = 0.9F, .height = 60.0F}, 10.0)[0] == 255);
    CHECK(hog::terrain::packed_woods({.grown = 0.9F, .height = 60.0F}, 10.0)[1] == 255);
}

TEST_CASE("a patch carries the woods flora stands about each of its vertices", "[terrain][patch]") {
    const World& world = shared_world();
    const CellId cell = land_patch();
    const Patch patch = bake_patch(world, cell);
    const hog::planet::FaceIJ at = cell.to_face_ij();
    const int level = at.level + patch_depth;
    int wooded = 0;
    int bare = 0;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const Corner corner{.face = at.face,
                                .i = (std::int64_t{at.i} << patch_depth) + i,
                                .j = (std::int64_t{at.j} << patch_depth) + j,
                                .level = level};
            const hog::terrain::PatchVertex& vertex = patch.vertices.at(vertex_index(i, j));
            const int finer = hog::flora::lattice_level - level;
            const hog::flora::Habitat habitat =
                hog::flora::habitat_of(world.state(hog::terrain::sample_cell(corner)));
            const hog::flora::Woodland woodland = hog::flora::woodland_at(
                {static_cast<float>(std::ldexp(static_cast<double>(corner.i), finer)),
                 static_cast<float>(std::ldexp(static_cast<double>(corner.j), finer))},
                static_cast<std::uint32_t>(at.face), vertex.biome, habitat);
            const DVec3 point = world_at(patch, vertex_index(i, j));
            const double area =
                hog::planet::cell_area(hog::planet::cell_at(point, hog::flora::lattice_level)) *
                glm::dot(point, point);
            const hog::terrain::Woods expected = woodland.grown > 0.0F
                                                     ? hog::terrain::packed_woods(woodland, area)
                                                     : hog::terrain::Woods{};
            INFO("vertex " << i << ", " << j);
            // The point is rebuilt from the patch's floats, so the area may round a step apart.
            CHECK(std::abs(int{vertex.woods[0]} - int{expected[0]}) <= 1);
            CHECK(vertex.woods[1] == expected[1]);
            CHECK(vertex.woods[2] == expected[2]);
            CHECK(vertex.woods[3] == expected[3]);
            CHECK(vertex.woods[4] == expected[4]);
            wooded += vertex.woods[0] > 20 ? 1 : 0;
            bare += vertex.woods[0] == 0 ? 1 : 0;
        }
    }
    INFO(wooded << " wooded vertices, " << bare << " bare");
    CHECK(wooded > 0);
    CHECK(bare > 0);
}

TEST_CASE("a patch's woods thin where they died, and grow back younger", "[terrain][patch]") {
    const World& world = shared_world();
    const CellId cell = land_patch();
    const Patch untouched = bake_patch(world, cell);
    // Three in ten of the woods alive everywhere, most of those grown back since a loss: the young
    // in sqrt(0.25) of the spots, as far as the half standing dead leave room, so 0.45 of them.
    const hog::flora::Fates thinned{
        2, std::vector<hog::flora::Fate>(
               std::size_t{6} * 16,
               {.living = 0.3F, .established = 0.05F, .dead = 0.5F, .charred = 1.0F})};
    const Patch burned = bake_patch(world, cell, thinned);
    int wooded = 0;
    for (std::size_t at = 0; at < untouched.vertices.size(); ++at) {
        const hog::terrain::Woods& before = untouched.vertices[at].woods;
        const hog::terrain::Woods& after = burned.vertices[at].woods;
        if (before[0] < 40) {
            continue;
        }
        ++wooded;
        INFO("vertex " << at);
        CHECK(std::abs(int{after[0]} - static_cast<int>(std::lround(0.5 * before[0]))) <= 1);
        CHECK(after[1] < before[1]);
        // The kinds the same.
        CHECK(after[2] == before[2]);
        CHECK(after[4] == before[4]);
    }
    CHECK(wooded > 0);
    // Untouched fates bake as no fates at all.
    const Patch told =
        bake_patch(world, cell, hog::flora::Fates{2, std::vector<hog::flora::Fate>(96)});
    for (std::size_t at = 0; at < told.vertices.size(); ++at) {
        CHECK(told.vertices[at].woods == untouched.vertices[at].woods);
        CHECK(told.vertices[at].cover == hog::terrain::Cover{});
    }
}

TEST_CASE("a patch's ground keeps a fire's black, and how much of its woods are gone and fallen",
          "[terrain][patch]") {
    using hog::terrain::Cover;
    using hog::terrain::packed_cover;
    // Seven in ten of the woods gone, half of those still standing dead, the ground two fifths
    // black.
    const hog::flora::Fate fate{.living = 0.3F, .established = 0.3F, .dead = 0.35F, .scar = 0.4F};
    CHECK(packed_cover(fate, true) == Cover{102, (11U << 4U) | 5U});
    // Where no woods grow, only the black.
    CHECK(packed_cover(fate, false) == Cover{102, 0});
    CHECK(packed_cover(hog::flora::Fate{}, true) == Cover{});

    const World& world = shared_world();
    const CellId cell = land_patch();
    const Patch burned =
        bake_patch(world, cell,
                   hog::flora::Fates{2, std::vector<hog::flora::Fate>(std::size_t{6} * 16, fate)});
    int wooded = 0;
    for (const hog::terrain::PatchVertex& vertex : burned.vertices) {
        const bool woods = hog::flora::trees_of(vertex.biome).height > 0.0F;
        CHECK(vertex.cover == packed_cover(fate, woods));
        wooded += woods ? 1 : 0;
    }
    CHECK(wooded > 0);
}
