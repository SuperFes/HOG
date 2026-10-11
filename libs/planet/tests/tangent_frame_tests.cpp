#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <random>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::planet::CellId;
using hog::planet::TangentFrame;

namespace {

constexpr double tolerance = 1e-12;

bool orthonormal_right_handed(const TangentFrame& frame) {
    const auto unit = [](DVec3 v) { return std::abs(glm::length(v) - 1.0) < tolerance; };
    return unit(frame.east) && unit(frame.north) && unit(frame.up) &&
           std::abs(glm::dot(frame.east, frame.north)) < tolerance &&
           std::abs(glm::dot(frame.north, frame.up)) < tolerance &&
           std::abs(glm::dot(frame.up, frame.east)) < tolerance &&
           glm::length(glm::cross(frame.east, frame.north) - frame.up) < tolerance;
}

} // namespace

TEST_CASE("tangent frames are orthonormal and right-handed everywhere", "[planet][frame]") {
    std::mt19937_64 rng{11};
    std::normal_distribution<double> normal;
    for (int sample = 0; sample < 5'000; ++sample) {
        const DVec3 direction{normal(rng), normal(rng), normal(rng)};
        const TangentFrame frame = hog::planet::tangent_frame(direction);
        REQUIRE(orthonormal_right_handed(frame));
        REQUIRE(glm::length(frame.up - glm::normalize(direction)) < tolerance);
        // North leans towards the pole and east has no polar component.
        REQUIRE(glm::dot(frame.north, hog::planet::north_pole) >= 0.0);
        REQUIRE_THAT(glm::dot(frame.east, hog::planet::north_pole), WithinAbs(0.0, tolerance));
    }
}

TEST_CASE("compass directions match the globe", "[planet][frame]") {
    // On the equator facing +x, east is +y and north is +z.
    const TangentFrame frame = hog::planet::tangent_frame(DVec3{5.0, 0.0, 0.0});
    CHECK(glm::length(frame.east - DVec3{0.0, 1.0, 0.0}) < tolerance);
    CHECK(glm::length(frame.north - DVec3{0.0, 0.0, 1.0}) < tolerance);
    CHECK(glm::length(frame.up - DVec3{1.0, 0.0, 0.0}) < tolerance);
}

TEST_CASE("the poles get a well-defined frame", "[planet][frame]") {
    for (const double z : {1.0, -1.0}) {
        const TangentFrame frame = hog::planet::tangent_frame(DVec3{0.0, 0.0, z});
        CHECK(orthonormal_right_handed(frame));
        CHECK(frame.east == DVec3{1.0, 0.0, 0.0});
    }
}

TEST_CASE("local and world components convert both ways", "[planet][frame]") {
    const TangentFrame frame = hog::planet::tangent_frame(DVec3{0.3, -0.8, 0.52});
    const DVec3 local{12.5, -3.0, 0.75};
    const DVec3 world = frame.to_world(local);
    CHECK(glm::length(frame.to_local(world) - local) < 1e-12);
    CHECK_THAT(glm::length(world), WithinAbs(glm::length(local), 1e-12));
    CHECK(glm::length(frame.to_world(DVec3{0.0, 0.0, 1.0}) - frame.up) < tolerance);
}

TEST_CASE("frames turn smoothly between neighbouring cells, across faces too", "[planet][frame]") {
    constexpr int level = 12;
    constexpr std::uint32_t last = (1U << level) - 1;
    // Cells beside a face edge away from the poles, so east is well defined on both sides.
    for (int face = 0; face < hog::planet::face_count; ++face) {
        const CellId cell = CellId::from_face_ij(face, last, last / 3, level);
        const TangentFrame here = hog::planet::tangent_frame(cell);
        for (const CellId neighbor : hog::planet::edge_neighbors(cell)) {
            const TangentFrame there = hog::planet::tangent_frame(neighbor);
            if (std::abs(glm::dot(here.up, hog::planet::north_pole)) > 0.99) {
                continue;
            }
            CHECK(glm::dot(here.east, there.east) > 0.999);
            CHECK(glm::dot(here.north, there.north) > 0.999);
        }
    }
}

TEST_CASE("cell and world-position frames agree", "[planet][frame]") {
    const CellId cell = CellId::from_face_ij(1, 300, 41, 9);
    const TangentFrame from_cell = hog::planet::tangent_frame(cell);
    const TangentFrame from_position =
        hog::planet::tangent_frame(hog::planet::surface_position(cell, 115'000.0));
    CHECK(glm::length(from_cell.up - from_position.up) < tolerance);
    CHECK(glm::length(from_cell.east - from_position.east) < tolerance);
}

TEST_CASE("tangent frames reject the planet centre", "[planet][frame]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(hog::planet::tangent_frame(DVec3{0.0}), hog::testing::AssertionFailed);
}
