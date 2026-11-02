#include <hog/weather/grid.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using hog::weather::Grid;

namespace {

Grid grid() {
    return {.level = 3, .layers = 10, .bottom = 1'000.0, .thickness = 100.0};
}

} // namespace

TEST_CASE("a grid holds a column of layers for each cell of its level", "[weather][grid]") {
    const Grid g = grid();
    CHECK(g.columns() == 6 * 8 * 8);
    CHECK(g.size() == g.columns() * 10);
    CHECK(g.index(0, 0) == 0);
    CHECK(g.index(0, 9) == 9);
    CHECK(g.index(1, 0) == 10);
    CHECK(g.index(g.columns() - 1, 9) == g.size() - 1);
}

TEST_CASE("layers lie between the bottom and top by radius", "[weather][grid]") {
    const Grid g = grid();
    CHECK(g.top() == 2'000.0);
    CHECK(g.radius_of(0) == 1'050.0);
    CHECK(g.floor_of(3) == 1'300.0);
    CHECK(g.layer_at(1'000.0) == 0);
    CHECK(g.layer_at(1'099.9) == 0);
    CHECK(g.layer_at(1'100.0) == 1);
    CHECK(g.layer_at(1'999.0) == 9);
    CHECK_FALSE(g.layer_at(999.0));
    CHECK_FALSE(g.layer_at(2'000.0));
    for (int layer = 0; layer < g.layers; ++layer) {
        CHECK(g.layer_at(g.radius_of(layer)) == layer);
    }
}

TEST_CASE("layers that thicken upward still tile the air and find their radii", "[weather][grid]") {
    const Grid g{.level = 3, .layers = 10, .bottom = 1'000.0, .thickness = 50.0, .ratio = 1.2};
    CHECK(g.floor_of(0) == 1'000.0);
    CHECK(g.depth_of(3) == Catch::Approx(50.0 * 1.2 * 1.2 * 1.2));
    for (int layer = 0; layer < g.layers; ++layer) {
        CHECK(g.floor_of(layer + 1) - g.floor_of(layer) == Catch::Approx(g.depth_of(layer)));
        CHECK(g.layer_at(g.radius_of(layer)) == layer);
        CHECK(g.layer_at(g.floor_of(layer)) == layer);
        CHECK(g.radius_of(layer + 1) - g.radius_of(layer) == Catch::Approx(g.between(layer)));
    }
    CHECK_FALSE(g.layer_at(g.top()));
    CHECK_FALSE(g.layer_at(999.0));
}
