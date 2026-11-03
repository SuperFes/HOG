#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/atmosphere.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using namespace hog::weather;

namespace {

hog::globe::Planet small_earth() {
    hog::globe::Planet planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 20'000.0;
    return planet;
}

constexpr hog::DVec3 sun{1.0, 0.0, 0.0};

} // namespace

TEST_CASE("an atmosphere spins at the day it is given, not the planet's", "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    const Atmosphere air{generator, 14'400.0, 2, 8};
    CHECK(air.planet().day == 14'400.0);
    CHECK(generator.planet().day != 14'400.0);
}

TEST_CASE(
    "advancing steps the air in pieces no longer than the longest, at the sun of their middle",
    "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 8};
    std::vector<double> asked;
    air.advance(300.0, [&](double elapsed) {
        asked.push_back(elapsed);
        return sun;
    });
    REQUIRE(asked.size() == 3);
    CHECK(asked[0] == 60.0);
    CHECK(asked[1] == 180.0);
    CHECK(asked[2] == 270.0);
}

TEST_CASE("a day's weather is finite and a column's cloud, rain and wind are readable",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 8};
    air.advance(6.0 * 3600.0, [](double) { return sun; });
    float cloud = 0.0F;
    for (std::size_t column = 0; column < air.field().grid.columns(); ++column) {
        const Column shown = air.column(column);
        REQUIRE(std::isfinite(shown.cloud));
        REQUIRE(shown.cloud >= 0.0F);
        REQUIRE(shown.falling >= 0.0F);
        REQUIRE(shown.snow >= 0.0F);
        REQUIRE(shown.snow <= 1.0F);
        REQUIRE(shown.humidity >= 0.0F);
        REQUIRE(shown.humidity <= 1.0F);
        REQUIRE(std::isfinite(shown.wind.x));
        REQUIRE(shown.top >= shown.base);
        cloud += shown.cloud;
    }
    CHECK(cloud > 0.0F);
}

TEST_CASE("cold ground's rain is snow and warm ground's is rain", "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 8};
    const Field& field = air.field();
    // The pole's first open layer is cold, the equator's warm.
    float cold = 1.0F;
    float warm = 0.0F;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        const hog::DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(field.grid.level, column));
        const Column shown = air.column(column);
        if (up.z > 0.95) {
            cold = std::min(cold, shown.snow);
        }
        if (std::abs(up.z) < 0.1) {
            warm = std::max(warm, shown.snow);
        }
    }
    CHECK(cold == 1.0F);
    CHECK(warm == 0.0F);
}

TEST_CASE("cloud touching the ground is fog and cloud above it has a base and top",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 8};
    Field& field = air.field();
    std::size_t column = 0;
    while (field.solid(column, 0) || field.over_sea[column] == 0) {
        ++column;
    }
    for (Air& cell : std::span{field.air}.subspan(field.grid.index(column, 0), 8)) {
        cell.cloud = 0.0F;
    }
    field.at(column, 0).cloud = 1e-3F;
    field.at(column, 3).cloud = 1e-3F;
    field.at(column, 4).cloud = 1e-3F;
    const Column shown = air.column(column);
    const Grid& grid = field.grid;
    const auto depth = [&](int from, int to) {
        return static_cast<float>(grid.floor_of(to) - grid.floor_of(from));
    };
    CHECK(shown.fog_top > 0.0F);
    CHECK(shown.base == Catch::Approx(shown.fog_top + depth(1, 3)));
    CHECK(shown.top == Catch::Approx(shown.base + depth(3, 5)));
    CHECK(shown.cloud > 0.0F);
}

TEST_CASE("a column says whether the sea lies under it", "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    const Atmosphere air{generator, 14'400.0, 2, 8};
    std::size_t seas = 0;
    for (std::size_t column = 0; column < air.field().grid.columns(); ++column) {
        const bool sea = air.column(column).sea;
        CHECK(sea == (air.field().over_sea[column] != 0));
        seas += sea ? 1 : 0;
    }
    CHECK(seas > 0);
    CHECK(seas < air.field().grid.columns());
}

TEST_CASE("a cloud's base and top lie where its layers' cloud puts them, not on layer edges",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 10};
    const Grid& grid = air.field().grid;
    std::size_t open = air.field().grid.columns();
    for (std::size_t column = 0; column < grid.columns(); ++column) {
        if (!air.field().solid(column, 0)) {
            open = column;
            break;
        }
    }
    REQUIRE(open < grid.columns());
    constexpr int layer = 5;
    const auto heights = [&](float cloud) {
        for (int i = 0; i < grid.layers; ++i) {
            air.field().at(open, i).cloud = i == layer ? cloud : 0.0F;
        }
        const Column shown = air.column(open);
        const double sea = air.planet().radius;
        return std::pair{static_cast<double>(shown.base) - (grid.floor_of(layer) - sea),
                         static_cast<double>(shown.top) - (grid.floor_of(layer) - sea)};
    };
    // Full, a layer's cloud runs from its floor to its ceiling.
    const double depth = grid.depth_of(layer);
    const auto [full_base, full_top] = heights(1e-4F);
    CHECK_THAT(full_base, Catch::Matchers::WithinAbs(0.0, 1.0));
    CHECK_THAT(full_top, Catch::Matchers::WithinAbs(depth, 1.0));
    // Thinner, it closes in on the layer's middle.
    const auto [thin_base, thin_top] = heights(0.8e-4F);
    CHECK_THAT(thin_base, Catch::Matchers::WithinAbs(0.125 * depth, 1.0));
    CHECK_THAT(thin_top, Catch::Matchers::WithinAbs(0.875 * depth, 1.0));
    // And a layer with too little to show has no cloud at all.
    for (int i = 0; i < grid.layers; ++i) {
        air.field().at(open, i).cloud = i == layer ? 0.4e-4F : 0.0F;
    }
    CHECK(air.column(open).top == 0.0F);
    CHECK(air.column(open).base == 0.0F);
}

TEST_CASE("cover is how much of the sky cloud closes over, by what the air could hold",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 14'400.0, 2, 12};
    Field& field = air.field();
    std::size_t column = 0;
    while (column < field.grid.columns() &&
           (field.over_sea[column] == 0 || field.solid(column, 8))) {
        ++column;
    }
    REQUIRE(column < field.grid.columns());
    const auto clear = [&] {
        for (int layer = 0; layer < field.grid.layers; ++layer) {
            field.at(column, layer).cloud = 0.0F;
        }
    };
    const auto cover = [&] { return air.column(column).cover; };
    clear();
    CHECK(cover() == 0.0F);

    // The same cloud closes over more of cold air's sky than of warm air's.
    field.at(column, 6).cloud = 1e-4F;
    field.at(column, 6).temperature = 290.0F;
    const float warm = cover();
    field.at(column, 6).temperature = 235.0F;
    const float cold = cover();
    CHECK(warm > 0.0F);
    CHECK(cold > warm);
    CHECK(cold <= 1.0F);

    // Layers touching overlap fully; ones with clear air between add as chance has it.
    clear();
    field.at(column, 6).temperature = 290.0F;
    field.at(column, 7).temperature = 290.0F;
    field.at(column, 9).temperature = 290.0F;
    field.at(column, 6).cloud = 1e-4F;
    const float one = cover();
    field.at(column, 7).cloud = 1e-4F;
    CHECK(cover() == one);
    field.at(column, 9).cloud = 1e-4F;
    CHECK(cover() > one);
}
