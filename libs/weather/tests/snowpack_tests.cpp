#include <hog/globe/planet.hpp>
#include <hog/weather/snowpack.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using hog::globe::Planet;
using namespace hog::weather;

namespace {

Planet earth() {
    return Planet{};
}

} // namespace

TEST_CASE("precipitation is snow when cold, rain when warm, sleet between", "[snowpack]") {
    CHECK(snow_share(260.0) == 1.0);
    CHECK(snow_share(285.0) == 0.0);
    CHECK(snow_share(273.15) == Catch::Approx(0.5));
}

TEST_CASE("a warm season melts what a cold one does not", "[snowpack]") {
    CHECK(warm_kelvin(-20.0, 10.0) == 0.0);
    CHECK(warm_kelvin(20.0, 10.0) == 20.0);
    CHECK(warm_kelvin(0.0, 10.0) == Catch::Approx(10.0 / 3.14159265).epsilon(1e-4));
    // Colder means never melts more, and a bigger swing melts more from a cold mean.
    CHECK(warm_kelvin(-3.0, 10.0) < warm_kelvin(0.0, 10.0));
    CHECK(warm_kelvin(-3.0, 15.0) > warm_kelvin(-3.0, 10.0));
}

TEST_CASE("height turns melt into snow gain", "[snowpack]") {
    const Planet planet = earth();
    const Climate climate{.surface = 283.0, .swing = 10.0, .precipitation = 0.15};
    const Balance low = ice_balance(planet, climate, 0.0);
    const Balance high = ice_balance(planet, climate, 5'000.0);
    CHECK(low.net() < 0.0);
    CHECK(high.net() > 0.0);
    CHECK(high.loss < low.loss);
    CHECK(high.gain > low.gain);
}

TEST_CASE("the snowline sits where the balance crosses zero", "[snowpack]") {
    const Planet planet = earth();
    const Climate temperate{.surface = 283.0, .swing = 10.0, .precipitation = 0.15};
    const auto line = snowline(planet, temperate, 8'000.0);
    REQUIRE(line.has_value());
    CHECK(ice_balance(planet, temperate, *line - 50.0).net() < 0.0);
    CHECK(ice_balance(planet, temperate, *line + 50.0).net() > 0.0);

    // Colder ground has its line lower, and a desert one never makes snow.
    Climate cold = temperate;
    cold.surface = 270.0;
    CHECK(*snowline(planet, cold, 8'000.0) < *line);
    Climate dry = temperate;
    dry.precipitation = 0.0;
    CHECK_FALSE(snowline(planet, dry, 8'000.0).has_value());
}

TEST_CASE("ground too warm for snow at any height has no snowline", "[snowpack]") {
    const Climate hot{.surface = 310.0, .swing = 5.0, .precipitation = 0.2};
    CHECK_FALSE(snowline(earth(), hot, 1'000.0).has_value());
}
