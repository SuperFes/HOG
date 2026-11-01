#include <hog/globe/qef.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using hog::DVec3;
using hog::globe::nearest_to_planes;

namespace {

bool near(DVec3 a, DVec3 b) {
    return glm::length(a - b) < 1e-9;
}

} // namespace

TEST_CASE("three planes meet at a corner", "[globe][qef]") {
    const DVec3 corner{0.3, 0.6, 0.2};
    const std::array points{DVec3{0.3, 0.1, 0.9}, DVec3{0.0, 0.6, 0.4}, DVec3{0.7, 0.8, 0.2},
                            DVec3{0.3, 0.9, 0.0}};
    const std::array normals{DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0}, DVec3{0.0, 0.0, 1.0},
                             DVec3{1.0, 0.0, 0.0}};
    CHECK(near(nearest_to_planes(points, normals), corner));
}

TEST_CASE("two planes leave the point on their line, nearest the points' mean", "[globe][qef]") {
    const DVec3 up = glm::normalize(DVec3{0.0, 1.0, 1.0});
    const DVec3 side = glm::normalize(DVec3{0.0, 1.0, -1.0});
    const std::array points{DVec3{0.0, 0.5, 0.5}, DVec3{1.0, 0.5, 0.5}, DVec3{0.4, 1.0, 1.0}};
    const std::array normals{up, side, side};
    const DVec3 found = nearest_to_planes(points, normals);
    CHECK(std::abs(glm::dot(found - points[0], up)) < 1e-9);
    CHECK(std::abs(glm::dot(found - points[0], side)) < 1e-9);
    CHECK(std::abs(found.x - ((0.0 + 1.0 + 0.4) / 3.0)) < 1e-9);
}

TEST_CASE("nearly parallel planes are taken as one, through the points' mean", "[globe][qef]") {
    const DVec3 tilted = glm::normalize(DVec3{0.0, 0.1, 1.0});
    const std::array points{DVec3{0.0, 0.0, 0.5}, DVec3{1.0, 1.0, 0.6}};
    const std::array normals{DVec3{0.0, 0.0, 1.0}, tilted};
    const DVec3 found = nearest_to_planes(points, normals);
    // They cross half a metre off, at y = 0; it stays by the points' mean instead.
    CHECK(std::abs(found.x - 0.5) < 1e-9);
    CHECK(glm::length(found - DVec3{0.5, 0.5, 0.55}) < 0.05);
}

TEST_CASE("a single plane takes the points' mean onto it", "[globe][qef]") {
    const DVec3 normal{0.0, 0.0, 1.0};
    const std::array points{DVec3{0.0, 0.0, 0.4}, DVec3{1.0, 0.0, 0.4}, DVec3{0.5, 1.0, 0.4}};
    const std::array normals{normal, normal, normal};
    CHECK(near(nearest_to_planes(points, normals), DVec3{0.5, 1.0 / 3.0, 0.4}));
}
