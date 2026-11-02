#include <hog/weather/stencil.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

using namespace hog::weather;

namespace {

const Grid& grid() {
    static const Grid g{.level = 3, .layers = 5, .bottom = 20'000.0, .thickness = 500.0};
    return g;
}

const Stencil& stencil() {
    static const Stencil s{grid()};
    return s;
}

} // namespace

TEST_CASE("each cell has four neighbours in its layer and the layers above and below",
          "[weather][stencil]") {
    const Grid& g = grid();
    for (std::size_t column = 0; column < g.columns(); ++column) {
        for (int layer = 0; layer < g.layers; ++layer) {
            const auto& links = stencil().links(g.index(column, layer));
            for (int side = 0; side < 4; ++side) {
                REQUIRE(links[static_cast<std::size_t>(side)].cell != Link::none);
            }
            REQUIRE((links[4].cell != Link::none) == (layer + 1 < g.layers));
            REQUIRE((links[5].cell != Link::none) == (layer > 0));
        }
    }
}

TEST_CASE("neighbours are mutual, as far apart both ways, and in opposite directions",
          "[weather][stencil]") {
    const Grid& g = grid();
    for (std::size_t cell = 0; cell < g.size(); ++cell) {
        for (const Link& link : stencil().links(cell)) {
            if (link.cell == Link::none) {
                continue;
            }
            bool found = false;
            for (const Link& back : stencil().links(link.cell)) {
                if (back.cell == cell) {
                    found = true;
                    REQUIRE(std::abs(back.distance - link.distance) < 1e-3F * link.distance);
                    // The tangents differ by the angle between the cells, a few tens of degrees.
                    REQUIRE(glm::dot(back.direction, link.direction) < -0.7F);
                }
            }
            REQUIRE(found);
        }
    }
}

TEST_CASE("a cell's neighbours are about a cell's width away, level with the ground",
          "[weather][stencil]") {
    const Grid& g = grid();
    // A quarter of the circumference over the cells round it.
    const double width = std::numbers::pi * 0.5 * 20'250.0 / 8.0;
    for (std::size_t column = 0; column < g.columns(); ++column) {
        const auto& links = stencil().links(g.index(column, 2));
        for (int side = 0; side < 4; ++side) {
            const Link& link = links[static_cast<std::size_t>(side)];
            REQUIRE(static_cast<double>(link.distance) > 0.5 * width);
            REQUIRE(static_cast<double>(link.distance) < 1.5 * width);
            REQUIRE(std::abs(glm::length(link.direction) - 1.0F) < 1e-4F);
            REQUIRE(std::abs(glm::dot(link.direction, stencil().up(column))) < 1e-4F);
        }
        CHECK(links[4].distance == 500.0F);
        CHECK(links[4].direction == stencil().up(column));
        CHECK(links[5].direction == -stencil().up(column));
    }
}

TEST_CASE("the links' fitted gradient takes a linear field exactly at any cell, skewed or not",
          "[weather][stencil]") {
    const Grid grid{.level = 4, .layers = 3, .bottom = 100'000.0, .thickness = 1000.0};
    const Stencil stencil{grid};
    const hog::DVec3 slope{0.3, -0.7, 0.2};
    double worst = 0.0;
    double square_worst = 0.0;
    for (std::size_t column = 0; column < grid.columns(); ++column) {
        for (int layer = 0; layer < grid.layers; ++layer) {
            const std::size_t cell = grid.index(column, layer);
            const auto& links = stencil.links(cell);
            // Tangent offsets, as a parcel's departure point lies, and a little up.
            const hog::Vec3 up = stencil.up(column);
            const hog::Vec3 across = glm::normalize(glm::cross(up, hog::Vec3{0.3F, 0.5F, 0.8F}));
            const hog::Vec3 offset =
                (across * 40.0F) + (glm::cross(up, across) * 25.0F) + (up * 3.0F);
            const hog::Vec3 w = gradient_weights(links, 0, offset);
            // A field linear in position: each neighbour differs by the slope along its way.
            double change = 0.0;
            double square = 0.0;
            for (const Link& link : links) {
                if (link.cell != Link::none) {
                    const double along = static_cast<double>(link.distance) *
                                         glm::dot(slope, hog::DVec3{link.direction});
                    change += along / static_cast<double>(link.distance) *
                              static_cast<double>(glm::dot(link.direction, w));
                    // As if the neighbours lay square about the cell.
                    square += along / static_cast<double>(link.distance) * 0.5 *
                              static_cast<double>(glm::dot(link.direction, offset));
                }
            }
            const double exact = glm::dot(slope, hog::DVec3{offset});
            // The edge layers lack a neighbour up or down, so only what's tangent is exact there.
            if (layer > 0 && layer + 1 < grid.layers) {
                worst = std::max(worst, std::abs(change - exact) / (1.0 + std::abs(exact)));
                square_worst =
                    std::max(square_worst, std::abs(square - exact) / (1.0 + std::abs(exact)));
            }
        }
    }
    CHECK(worst < 1e-2);
    // Where the cells are skewed, taking the neighbours as square is far out.
    CHECK(square_worst > 0.1);
}
