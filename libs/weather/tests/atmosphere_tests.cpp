#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/atmosphere.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using namespace hog::weather;

namespace {

hog::globe::Planet small_earth() {
    hog::globe::Planet planet;
    planet.body.radius = 20'000.0;
    return planet;
}

constexpr hog::DVec3 sun{1.0, 0.0, 0.0};

} // namespace

TEST_CASE(
    "advancing steps the air in pieces no longer than the longest, at the sun of their middle",
    "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 2, 8};
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
    Atmosphere air{generator, 2, 8};
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
        cloud += shown.cloud;
    }
    CHECK(cloud > 0.0F);
}

TEST_CASE("cold ground's rain is snow and warm ground's is rain", "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 2, 8};
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

TEST_CASE("a column says whether the sea lies under it", "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    const Atmosphere air{generator, 2, 8};
    std::size_t seas = 0;
    for (std::size_t column = 0; column < air.field().grid.columns(); ++column) {
        const bool sea = air.column(column).sea;
        CHECK(sea == (air.field().over_sea[column] != 0));
        seas += sea ? 1 : 0;
    }
    CHECK(seas > 0);
    CHECK(seas < air.field().grid.columns());
}

TEST_CASE("cover is how much of the sky cloud closes over, by what the air could hold",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 2, 12};
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

TEST_CASE("a column keeps each layer's cloud, those under the ground the lowest open one's",
          "[weather][atmosphere]") {
    const hog::globe::Generator generator{small_earth()};
    Atmosphere air{generator, 2, 10};
    Field& field = air.field();
    const Grid& grid = field.grid;
    // A column whose ground takes in its lowest layer.
    std::size_t column = 0;
    while (column < grid.columns() && (!field.solid(column, 0) || field.solid(column, 1))) {
        ++column;
    }
    REQUIRE(column < grid.columns());
    for (int layer = 1; layer < grid.layers; ++layer) {
        field.at(column, layer).cloud = 0.0F;
    }
    field.at(column, 1).cloud = 1e-4F;
    field.at(column, 6).cloud = 2e-4F;
    const Column shown = air.column(column);
    REQUIRE(shown.layers.size() == static_cast<std::size_t>(grid.layers));
    float total = 0.0F;
    for (int layer = 1; layer < grid.layers; ++layer) {
        const LayerCloud& here = shown.layers[static_cast<std::size_t>(layer)];
        CHECK((here.cloud > 0.0F) == (layer == 1 || layer == 6));
        CHECK((here.cover > 0.0F) == (layer == 1 || layer == 6));
        CHECK(here.cover <= 1.0F);
        total += here.cloud;
    }
    CHECK(total == Catch::Approx(shown.cloud));
    CHECK(shown.layers[0].cloud == shown.layers[1].cloud);
    CHECK(shown.layers[0].cover == shown.layers[1].cover);
}
