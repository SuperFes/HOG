#include <hog/core/hash.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <numbers>
#include <random>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::planet::CellId;
using hog::planet::max_level;

namespace {

constexpr double tolerance = 1e-12;

bool nearly_equal(DVec3 a, DVec3 b) {
    return glm::length(a - b) < tolerance;
}

DVec3 random_direction(std::mt19937_64& rng) {
    std::normal_distribution<double> normal;
    DVec3 direction{0.0};
    while (glm::length(direction) < 1e-6) {
        direction = DVec3{normal(rng), normal(rng), normal(rng)};
    }
    return glm::normalize(direction);
}

} // namespace

TEST_CASE("the face warp maps the edges and centre onto themselves", "[planet][projection]") {
    CHECK(hog::planet::st_to_uv(0.0) == -1.0);
    CHECK(hog::planet::st_to_uv(0.5) == 0.0);
    CHECK(hog::planet::st_to_uv(1.0) == 1.0);
    CHECK(hog::planet::uv_to_st(-1.0) == 0.0);
    CHECK(hog::planet::uv_to_st(0.0) == 0.5);
    CHECK(hog::planet::uv_to_st(1.0) == 1.0);

    double previous = -2.0;
    for (int step = 0; step <= 1000; ++step) {
        const double s = step / 1000.0;
        const double u = hog::planet::st_to_uv(s);
        CHECK(u > previous);
        CHECK_THAT(hog::planet::uv_to_st(u), WithinAbs(s, 1e-15));
        previous = u;
    }
}

TEST_CASE("face coordinates round-trip through cube points", "[planet][projection]") {
    for (int face = 0; face < hog::planet::face_count; ++face) {
        for (const double u : {-1.0, -0.3, 0.0, 0.7, 1.0}) {
            for (const double v : {-0.9, 0.0, 0.25}) {
                const auto uv =
                    hog::planet::xyz_to_face_uv(hog::planet::face_uv_to_xyz(face, u, v));
                // Face edges are shared, so a point on one can resolve to the neighbouring face.
                if (std::abs(u) < 1.0) {
                    CHECK(uv.face == face);
                    CHECK(uv.u == u);
                    CHECK(uv.v == v);
                }
            }
        }
        // Scaling a direction doesn't change its face coordinates.
        const auto near = hog::planet::xyz_to_face_uv(hog::planet::face_uv_to_xyz(face, 0.5, -0.5));
        const auto far =
            hog::planet::xyz_to_face_uv(hog::planet::face_uv_to_xyz(face, 0.5, -0.5) * 1e7);
        CHECK(near.face == far.face);
        CHECK_THAT(far.u, WithinAbs(near.u, tolerance));
    }
}

TEST_CASE("every cell contains its own centre", "[planet][projection]") {
    for (int level = 0; level <= 5; ++level) {
        const std::uint32_t size = 1U << level;
        for (int face = 0; face < hog::planet::face_count; ++face) {
            for (std::uint32_t i = 0; i < size; ++i) {
                for (std::uint32_t j = 0; j < size; ++j) {
                    const CellId cell = CellId::from_face_ij(face, i, j, level);
                    const DVec3 centre = hog::planet::cell_center(cell);
                    REQUIRE_THAT(glm::length(centre), WithinAbs(1.0, tolerance));
                    REQUIRE(hog::planet::cell_at(centre, level) == cell);
                }
            }
        }
    }
}

TEST_CASE("cell lookup is consistent down to the deepest level", "[planet][projection]") {
    std::mt19937_64 rng{7};
    for (int sample = 0; sample < 2'000; ++sample) {
        const DVec3 direction = random_direction(rng);
        const CellId leaf = hog::planet::cell_at(direction);
        REQUIRE(leaf.is_leaf());
        REQUIRE(hog::planet::cell_at(hog::planet::cell_center(leaf)) == leaf);
        // A direction's cell at a coarser level is the parent of its leaf.
        const int level = static_cast<int>(rng() % (max_level + 1));
        REQUIRE(hog::planet::cell_at(direction, level) == leaf.parent(level));
    }
}

TEST_CASE("edge neighbours share two corners", "[planet][projection]") {
    for (int level = 1; level <= 3; ++level) {
        const std::uint32_t size = 1U << level;
        for (int face = 0; face < hog::planet::face_count; ++face) {
            for (std::uint32_t i = 0; i < size; ++i) {
                for (std::uint32_t j = 0; j < size; ++j) {
                    const CellId cell = CellId::from_face_ij(face, i, j, level);
                    const auto corners = hog::planet::cell_vertices(cell);
                    for (const CellId neighbor : hog::planet::edge_neighbors(cell)) {
                        const auto theirs = hog::planet::cell_vertices(neighbor);
                        const auto shared = std::ranges::count_if(corners, [&](DVec3 corner) {
                            return std::ranges::any_of(
                                theirs, [&](DVec3 other) { return nearly_equal(corner, other); });
                        });
                        REQUIRE(shared == 2);
                    }
                }
            }
        }
    }
}

TEST_CASE("cell vertices wind counter-clockwise seen from outside", "[planet][projection]") {
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const CellId cell = CellId::from_face_ij(face, 2, 5, 3);
        const auto v = hog::planet::cell_vertices(cell);
        const DVec3 normal = glm::cross(v[1] - v[0], v[2] - v[0]);
        CHECK(glm::dot(normal, hog::planet::cell_center(cell)) > 0.0);
    }
}

TEST_CASE("cell areas stay within about two to one", "[planet][projection]") {
    constexpr int level = 6;
    constexpr std::uint32_t size = 1U << level;
    double smallest = 4.0 * 3.15;
    double largest = 0.0;
    double total = 0.0;
    for (std::uint32_t i = 0; i < size; ++i) {
        for (std::uint32_t j = 0; j < size; ++j) {
            const double area = hog::planet::cell_area(CellId::from_face_ij(0, i, j, level));
            smallest = std::min(smallest, area);
            largest = std::max(largest, area);
            total += area;
        }
    }
    CHECK(largest / smallest < 2.2);
    // One face is a sixth of the sphere's 4 pi steradians.
    CHECK_THAT(total, WithinAbs(4.0 * std::numbers::pi / 6.0, 1e-9));
}

TEST_CASE("world positions sit on the planet's surface", "[planet][projection]") {
    constexpr double radius = 115'000.0;
    const CellId cell = CellId::from_face_ij(4, 1'000, 70'000, 20);
    const hog::WorldPosition position = hog::planet::surface_position(cell, radius);

    CHECK_THAT(glm::length(position.metres()), WithinAbs(radius, 1e-6));
    CHECK(hog::planet::cell_at(position, 20) == cell);
}

TEST_CASE("cell centres are bit-identical on every platform", "[planet][projection]") {
    // Golden value: changing it means every stored or generated position moves.
    std::uint64_t hash = 0;
    for (int face = 0; face < hog::planet::face_count; ++face) {
        for (std::uint32_t i = 0; i < 16; ++i) {
            for (std::uint32_t j = 0; j < 16; ++j) {
                const DVec3 centre = hog::planet::cell_center(CellId::from_face_ij(face, i, j, 4));
                hash = hog::hash_values(hash, std::bit_cast<std::uint64_t>(centre.x),
                                        std::bit_cast<std::uint64_t>(centre.y),
                                        std::bit_cast<std::uint64_t>(centre.z));
            }
        }
    }
    const DVec3 deep = hog::planet::cell_center(
        CellId::from_face_ij(2, 123'456'789, 1'000'000'007 % (1U << 30), max_level));
    hash = hog::hash_values(hash, std::bit_cast<std::uint64_t>(deep.x),
                            std::bit_cast<std::uint64_t>(deep.y),
                            std::bit_cast<std::uint64_t>(deep.z));

    CHECK(hash == 0x1C3D'7A16'3A0E'19E2);
}

TEST_CASE("projection rejects degenerate input", "[planet][projection]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(hog::planet::xyz_to_face_uv(DVec3{0.0}), AssertionFailed);
    CHECK_THROWS_AS(hog::planet::cell_at(DVec3{1.0, 0.0, 0.0}, max_level + 1), AssertionFailed);
    CHECK_THROWS_AS(hog::planet::surface_position(CellId::from_face(0), 0.0), AssertionFailed);
}
