#include <hog/weather/stencil.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

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
