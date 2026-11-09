#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/field.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using Catch::Approx;
using hog::globe::Generator;
using hog::globe::Planet;
using hog::weather::Field;

namespace {

constexpr int level = 3;
constexpr int layers = 10;

Planet earth() {
    Planet planet;
    planet.body.radius = 20'000.0; // small, for a quick generator
    return planet;
}

const Field& earth_field() {
    static const Generator generator{earth()};
    static const Field field = hog::weather::start_field(generator, level, layers);
    return field;
}

// The column nearest a latitude, in degrees, on the prime meridian.
std::size_t column_at(double latitude) {
    const double angle = latitude * std::numbers::pi / 180.0;
    const hog::DVec3 direction{std::cos(angle), 0.0, std::sin(angle)};
    return static_cast<std::size_t>(hog::globe::index_of(hog::globe::cell_under(direction, level)));
}

} // namespace

TEST_CASE("still air cools with height at the lapse rate", "[weather][field]") {
    const Field& field = earth_field();
    const auto column = column_at(0.0);
    const double rise = field.grid.radius_of(layers - 1) - field.grid.radius_of(0);
    const double cooling = static_cast<double>(field.at(column, 0).temperature) -
                           static_cast<double>(field.at(column, layers - 1).temperature);
    CHECK(cooling == Approx(hog::weather::lapse_rate(earth()) * rise).epsilon(0.01));
    // About Earth's 6.5 K a kilometre.
    CHECK(hog::weather::lapse_rate(earth()) * 1'000.0 > 5.5);
    CHECK(hog::weather::lapse_rate(earth()) * 1'000.0 < 7.0);
}

TEST_CASE("the air is colder towards the poles", "[weather][field]") {
    const Field& field = earth_field();
    CHECK(field.at(column_at(0.0), 0).temperature > field.at(column_at(45.0), 0).temperature);
    CHECK(field.at(column_at(45.0), 0).temperature > field.at(column_at(85.0), 0).temperature);
}

TEST_CASE("pressure thins with height over a scale height of about eight kilometres",
          "[weather][field]") {
    const Planet planet = earth();
    CHECK(hog::weather::pressure_at(planet, 288.0, 0.0) == Approx(planet.system.air.pressure));
    const double high = hog::weather::pressure_at(planet, 288.0, 8'000.0);
    CHECK(high < 0.5 * planet.system.air.pressure);
    CHECK(high > 0.3 * planet.system.air.pressure);
    CHECK(hog::weather::pressure_at(planet, 288.0, 4'000.0) > high);
}

TEST_CASE("warm air can hold much more water", "[weather][field]") {
    const double cool = hog::weather::saturation_ratio(278.0, 101'325.0);
    const double warm = hog::weather::saturation_ratio(298.0, 101'325.0);
    // 15 °C air holds about 10.7 g of water a kilogram.
    CHECK(hog::weather::saturation_ratio(288.15, 101'325.0) == Approx(0.0107).epsilon(0.1));
    CHECK(warm > 2.5 * cool);
    // Boiling air is all vapour: nothing left to compare with.
    CHECK(hog::weather::saturation_ratio(400.0, 101'325.0) == 0.0);
}

TEST_CASE("the start holds less water than saturates, and none that the sea's humidity "
          "doesn't allow",
          "[weather][field]") {
    const Field& field = earth_field();
    const Planet planet = earth();
    const double lapse = hog::weather::lapse_rate(planet);
    bool sea = false;
    bool land = false;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        for (int layer = 0; layer < layers; ++layer) {
            const auto& air = field.at(column, layer);
            const double height = field.grid.radius_of(layer) - planet.body.radius;
            const auto temperature = static_cast<double>(air.temperature);
            const double pressure =
                hog::weather::pressure_at(planet, temperature + (lapse * height), height);
            const double saturated = hog::weather::saturation_ratio(temperature, pressure);
            REQUIRE(static_cast<double>(air.vapour) <= saturated);
            REQUIRE(air.cloud == 0.0F);
            REQUIRE(air.rain == 0.0F);
            const bool over_sea = field.over_sea[column] != 0;
            // Never wetter than the column's humidity of what the air could hold there, and
            // thinner than that aloft, where the sea's water hasn't been mixed up to.
            REQUIRE(static_cast<double>(air.vapour) <=
                    ((over_sea ? 0.8 : 0.5) * saturated) * (1.0 + 1e-4));
            (over_sea ? sea : land) = true;
        }
    }
    CHECK(sea);
    CHECK(land);
}

TEST_CASE("the start's water thins with height faster than the air does", "[weather][field]") {
    const Field& field = earth_field();
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        if (field.solid(column, 0) || field.at(column, 0).vapour <= 0.0F) {
            continue;
        }
        const auto bottom = static_cast<double>(field.at(column, 0).vapour);
        const auto top = static_cast<double>(field.at(column, layers - 1).vapour);
        REQUIRE(top < bottom);
        return;
    }
    FAIL("no column to look at");
}

TEST_CASE("the sea is a flat floor to the air and the land is where the ground is",
          "[weather][field]") {
    const Field& field = earth_field();
    bool sea = false;
    bool land = false;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        REQUIRE(field.ground[column] >= field.grid.bottom);
        if (field.over_sea[column] != 0) {
            sea = true;
            CHECK(field.ground[column] == field.grid.bottom);
        } else {
            land = true;
            CHECK(field.ground[column] > field.grid.bottom);
        }
    }
    CHECK(sea);
    CHECK(land);
    CHECK_FALSE(field.solid(0, layers - 1));
}

TEST_CASE("a world without a volatile has no vapour in its air", "[weather][field]") {
    Planet moon;
    moon.system.air.pressure = 0.0;
    moon.body.radius = 20'000.0;
    const Generator generator{moon};
    const Field field = hog::weather::start_field(generator, 2, 6);
    for (const auto& air : field.air) {
        REQUIRE(air.vapour == 0.0F);
        REQUIRE(air.temperature > 0.0F);
    }
}
