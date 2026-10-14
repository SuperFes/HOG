#include <hog/planet/cell_id.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/cube_sphere.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <set>
#include <tuple>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::Vec3;

TEST_CASE("a cube sphere has a grid of unit vertices per face", "[render][cube_sphere]") {
    const auto mesh = hog::render::cube_sphere(4);
    CHECK(mesh.positions.size() == std::size_t{6} * 5 * 5);
    CHECK(mesh.indices.size() == std::size_t{6} * 4 * 4 * 6);
    for (const Vec3 position : mesh.positions) {
        CHECK_THAT(glm::length(position), WithinAbs(1.0, 1e-6));
    }
    for (const std::uint32_t index : mesh.indices) {
        CHECK(index < mesh.positions.size());
    }
}

TEST_CASE("cube sphere vertices sit on the planet's cell corners", "[render][cube_sphere]") {
    constexpr int level = 3;
    constexpr std::uint32_t cells = 1U << level;
    constexpr std::uint32_t across = cells + 1;
    const auto mesh = hog::render::cube_sphere(static_cast<int>(cells));
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t j = 0; j < cells; ++j) {
            for (std::uint32_t i = 0; i < cells; ++i) {
                const auto corners = hog::planet::cell_vertices(
                    hog::planet::CellId::from_face_ij(face, i, j, level));
                const std::size_t first = (static_cast<std::size_t>(face) * across * across) +
                                          (std::size_t{j} * across) + i;
                for (const auto [corner, offset] :
                     {std::tuple{0U, 0U}, {1U, 1U}, {2U, across + 1}, {3U, across}}) {
                    const DVec3 position{mesh.positions[first + offset]};
                    CHECK(glm::length(position - corners.at(corner)) < 1e-6);
                }
            }
        }
    }
}

TEST_CASE("cube sphere triangles face outwards and close the sphere", "[render][cube_sphere]") {
    constexpr int cells = 32;
    const auto mesh = hog::render::cube_sphere(cells);
    double area = 0.0;
    bool outward = true;
    for (std::size_t first = 0; first < mesh.indices.size(); first += 3) {
        const DVec3 a{mesh.positions[mesh.indices[first]]};
        const DVec3 b{mesh.positions[mesh.indices[first + 1]]};
        const DVec3 c{mesh.positions[mesh.indices[first + 2]]};
        const DVec3 normal = glm::cross(b - a, c - a);
        outward = outward && glm::dot(normal, a + b + c) > 0.0;
        area += glm::length(normal) / 2.0;
    }
    CHECK(outward);
    CHECK_THAT(area, WithinRel(4.0 * std::numbers::pi, 0.01));

    // Edge vertices of neighbouring faces coincide, leaving the sphere's 6n² + 2 corners.
    std::set<std::tuple<long, long, long>> distinct;
    for (const Vec3 position : mesh.positions) {
        distinct.emplace(std::lround(position.x * 1e5F), std::lround(position.y * 1e5F),
                         std::lround(position.z * 1e5F));
    }
    CHECK(distinct.size() == (std::size_t{6} * cells * cells) + 2);
}
