#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>

using hog::globe::Planet;
using hog::globe::Volatile;

namespace {

Planet named(const char* name) {
    return hog::globe::preset(name).value_or(Planet{.seed = 0});
}

} // namespace

TEST_CASE("each preset names its body and Earth is the default", "[globe][planet]") {
    for (const char* name : {"earth", "mars", "venus", "moon"}) {
        CHECK(hog::globe::preset(name).has_value());
    }
    CHECK(named("earth").gravity == Planet{}.gravity);
    CHECK(named("mars").gravity == 3.71);
    CHECK(named("venus").pressure > 9e6);
    CHECK(named("moon").pressure == 0.0);
    CHECK_FALSE(hog::globe::preset("pluto"));
    for (const char* name : {"earth", "mars", "venus", "moon"}) {
        CHECK(named(name).radius == Planet{}.radius);
    }
}

TEST_CASE("craters turn complex and mountains top out where rock's strength says",
          "[globe][planet]") {
    // Fitted to Earth; the others are predictions, against what's measured.
    const auto earth = hog::globe::derive(named("earth"));
    CHECK(std::abs(earth.crater_transition - 3'200.0) < 1.0);
    CHECK(std::abs(earth.tallest - 8'800.0) < 1.0);
    const auto moon = hog::globe::derive(named("moon"));
    CHECK(moon.crater_transition > 15'000.0);
    CHECK(moon.crater_transition < 20'000.0);
    const auto mars = hog::globe::derive(named("mars"));
    CHECK(mars.crater_transition > 7'000.0);
    CHECK(mars.crater_transition < 10'000.0);
    // Olympus Mons, 21 to 27 km base to peak; Maxwell Montes, about 11 km.
    CHECK(mars.tallest > 21'000.0);
    CHECK(mars.tallest < 27'000.0);
    const auto venus = hog::globe::derive(named("venus"));
    CHECK(venus.tallest > 9'000.0);
    CHECK(venus.tallest < 12'000.0);
}

TEST_CASE("crust floats by its weight: Mars's two halves, and Venus all one level",
          "[globe][planet]") {
    // Mars's highlands stand about 5 km over its northern lowlands, not all of it by floating.
    const auto mars = hog::globe::derive(named("mars"));
    CHECK(mars.isostatic_step > 3'500.0);
    CHECK(mars.isostatic_step < 6'000.0);
    CHECK(hog::globe::derive(named("venus")).isostatic_step == 0.0);
    CHECK(hog::globe::derive(named("earth")).isostatic_step > 4'000.0);
}

TEST_CASE("a volatile boils as Clausius and Clapeyron say", "[globe][planet]") {
    const auto water = hog::globe::boiling_point(Volatile::Water, 101'325.0);
    CHECK(std::abs(water.value_or(0.0) - 373.15) < 3.0);
    const auto methane = hog::globe::boiling_point(Volatile::Methane, 101'325.0);
    CHECK(std::abs(methane.value_or(0.0) - 111.65) < 2.0);
    CHECK_FALSE(hog::globe::boiling_point(Volatile::Water, 600.0));
    CHECK_FALSE(hog::globe::boiling_point(Volatile::None, 101'325.0));
    CHECK(hog::globe::boiling_point(Volatile::Water, 1e9).value_or(0.0) == 647.096);
}

TEST_CASE("what wears a surface down follows from its air and warmth", "[globe][planet]") {
    const auto earth = hog::globe::derive(named("earth"));
    CHECK(earth.fluvial);
    CHECK(earth.glacial);
    CHECK(earth.aeolian);
    // Too cold for liquid water, with ice at the poles and wind enough for dunes.
    const auto mars = hog::globe::derive(named("mars"));
    CHECK_FALSE(mars.fluvial);
    CHECK(mars.glacial);
    CHECK(mars.aeolian);
    const auto venus = hog::globe::derive(named("venus"));
    CHECK_FALSE(venus.fluvial);
    CHECK_FALSE(venus.glacial);
    CHECK(venus.aeolian);
    const auto moon = hog::globe::derive(named("moon"));
    CHECK_FALSE(moon.fluvial);
    CHECK_FALSE(moon.glacial);
    CHECK_FALSE(moon.aeolian);

    // Titan's: methane rivers under a thick cold sky, too warm for methane ice.
    Planet titan = named("moon");
    titan.pressure = 146'700.0;
    titan.equator_temperature = 94.0;
    titan.pole_temperature = 91.0;
    titan.volatiles = Volatile::Methane;
    const auto alien = hog::globe::derive(titan);
    CHECK(alien.fluvial);
    CHECK_FALSE(alien.glacial);
    // Hot enough to boil it everywhere.
    titan.pole_temperature = 130.0;
    titan.equator_temperature = 140.0;
    CHECK_FALSE(hog::globe::derive(titan).fluvial);
}

TEST_CASE("temperature falls from equator to pole, averaging the measured mean",
          "[globe][planet]") {
    const Planet earth = named("earth");
    CHECK(hog::globe::temperature_at(earth, 0.0) == earth.equator_temperature);
    CHECK(std::abs(hog::globe::temperature_at(earth, std::numbers::pi / 2.0) -
                   earth.pole_temperature) < 1e-9);
    CHECK(std::abs(hog::globe::mean_temperature(earth) - 288.0) < 2.0);
    CHECK(std::abs(hog::globe::mean_temperature(named("mars")) - 208.0) < 2.0);
    CHECK(std::abs(hog::globe::mean_temperature(named("venus")) - 737.0) < 1e-9);
}

TEST_CASE("the crust's heat and widths follow from the knobs", "[globe][planet]") {
    const Planet earth = named("earth");
    const auto derived = hog::globe::derive(earth);
    // Continental crust warms 25 to 35 K a kilometre.
    CHECK(derived.geotherm > 0.025);
    CHECK(derived.geotherm < 0.035);
    CHECK(derived.floor_temperature > hog::globe::mean_temperature(earth));
    CHECK(std::abs(derived.lateral - (115.0 / 6'371.0)) < 1e-12);
    Planet wide = earth;
    wide.lateral_scale = 3.0;
    CHECK(std::abs(hog::globe::derive(wide).lateral - (3.0 * derived.lateral)) < 1e-12);
}

TEST_CASE("every preset is a planet that can be", "[globe][planet]") {
    for (const char* name : {"earth", "mars", "venus", "moon"}) {
        CHECK(hog::globe::validate(named(name)).has_value());
    }
    Planet sunk = named("earth");
    sunk.high.rock = hog::globe::Material::Iron;
    CHECK_FALSE(hog::globe::validate(sunk).has_value());
    Planet hollow = named("earth");
    hollow.floor_depth = hollow.radius;
    CHECK_FALSE(hog::globe::validate(hollow).has_value());
    Planet unknown = named("earth");
    unknown.gravity = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(hog::globe::validate(unknown).has_value());
}

TEST_CASE("a planet file starts from a preset and changes knobs by name", "[globe][planet]") {
    const auto mars = hog::globe::parse_planet(R"(
[globe]
preset = "mars"
seed = 7
gravity = 4
sea_fraction = 0.2
volatiles = "methane"
regime = "plates"

[globe.high]
thickness = 60_000
)");
    REQUIRE(mars.has_value());
    const Planet& planet = mars.value();
    CHECK(planet.seed == 7);
    CHECK(planet.gravity == 4.0);
    CHECK(planet.sea_fraction == 0.2);
    CHECK(planet.volatiles == Volatile::Methane);
    CHECK(planet.regime == hog::globe::Regime::Plates);
    CHECK(planet.high.thickness == 60'000.0);
    CHECK(planet.high.rock == named("mars").high.rock);
    CHECK(planet.pressure == named("mars").pressure);

    const auto earth = hog::globe::parse_planet("[globe]\n");
    REQUIRE(earth.has_value());
    CHECK(earth.value().pressure == Planet{}.pressure);
}

TEST_CASE("a planet file refuses what it doesn't know or can't be", "[globe][planet]") {
    for (const char* text : {
             "",
             "[globe]\npreset = \"pluto\"\n",
             "[globe]\ngravty = 3\n",
             "[globe]\ngravity = \"high\"\n",
             "[globe]\ngravity = -1\n",
             "[globe]\nseed = -1\n",
             "[globe]\nregime = \"wobbly\"\n",
             "[globe.high]\nrock = \"iron\"\n",
             "[globe.low]\ndepth = 3\n",
             "[globe]\nhigh = 3\n",
             "[globe\n",
         }) {
        CAPTURE(text);
        CHECK_FALSE(hog::globe::parse_planet(text).has_value());
    }
    CHECK(hog::globe::parse_planet("[globe\n").error().code == hog::ErrorCode::Parse);
    CHECK_FALSE(hog::globe::load_planet("no/such/planet.toml").has_value());
}
