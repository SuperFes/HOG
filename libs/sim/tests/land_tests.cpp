#include <hog/sim/land.hpp>
#include <hog/sim/weather.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using hog::sim::CellGrid;
using hog::sim::Ground;
using hog::sim::Land;
using hog::sim::LandTuning;
using hog::sim::LandWeather;

namespace {

constexpr double hour = 3'600.0;
constexpr double day = 86'400.0;

const CellGrid& grid() {
    static const CellGrid grid{1};
    return grid;
}

// Land everywhere but the first cell.
std::vector<Ground> island() {
    std::vector<Ground> ground(grid().size(), Ground{.ocean = false});
    ground[0].ocean = true;
    return ground;
}

// The same weather over every cell, held for a while.
struct Spell {
    float rain = 0.0F;        // mm per hour
    float snow = 0.0F;        // mm per hour
    float evaporation = 0.0F; // mm per hour
    float temperature = 10.0F;
};

struct Run {
    Land land{grid(), island(), LandTuning{}};
    double ran_off = 0.0; // mm from cell 1

    void hold(const Spell& spell, double seconds) {
        const std::vector<float> rain(grid().size(), spell.rain);
        const std::vector<float> snow(grid().size(), spell.snow);
        const std::vector<float> evaporation(grid().size(), spell.evaporation);
        const std::vector<float> temperature(grid().size(), spell.temperature);
        const LandWeather weather{.rain = rain,
                                  .snow = snow,
                                  .evaporation = evaporation,
                                  .temperature = temperature,
                                  .seconds = hour};
        const auto steps = static_cast<int>(std::lround(seconds / hour));
        for (int left = steps; left > 0; --left) {
            land.step(grid(), weather, nullptr);
            ran_off += static_cast<double>(land.fields().runoff[1]);
        }
    }

    [[nodiscard]] double snow() const { return static_cast<double>(land.fields().snow[1]); }
    [[nodiscard]] double soil() const { return static_cast<double>(land.fields().soil[1]); }
    [[nodiscard]] double ground() const { return land.fields().groundwater[1]; }
};

} // namespace

TEST_CASE("snow lies below freezing and melts into the soil above it", "[sim][land]") {
    Run run;
    run.hold({.snow = 1.0F, .temperature = -5.0F}, 2.0 * day);
    CHECK(run.snow() == Catch::Approx(48.0));
    CHECK(run.soil() == 0.0);
    CHECK(run.land.snow_cover(1) > 0.5);

    // 3 mm a day for every degree: a day at 4 °C takes 12 mm.
    run.hold({.temperature = 4.0F}, day);
    CHECK(run.snow() == Catch::Approx(36.0).epsilon(1e-3));
    CHECK(run.soil() > 0.05);
    run.hold({.temperature = 4.0F}, 5.0 * day);
    CHECK(run.snow() == 0.0);
}

TEST_CASE("rain soaks in until the soil is full and runs off after", "[sim][land]") {
    Run run;
    run.hold({.rain = 1.0F}, 12.0 * hour);
    // Dry soil takes nearly all of it.
    CHECK(run.soil() * LandTuning{}.soil_capacity == Catch::Approx(12.0).epsilon(0.02));
    CHECK(run.ran_off < 0.2);

    run.hold({.rain = 10.0F}, 2.0 * day);
    // Full but for what drains away below.
    CHECK(run.soil() > 0.97);
    const double before = run.ran_off;
    run.hold({.rain = 10.0F}, hour);
    CHECK(run.ran_off - before > 9.5);
}

TEST_CASE("the sun dries the soil, and snow keeps the ground from giving up water", "[sim][land]") {
    Run run;
    run.hold({.rain = 5.0F}, day);
    const double wet = run.soil();
    const double open = run.land.wetness(1);
    CHECK(open == Catch::Approx(wet));
    run.hold({.evaporation = 0.2F}, 5.0 * day);
    CHECK(run.soil() < wet);
    run.hold({.evaporation = 5.0F}, 30.0 * day);
    CHECK(run.soil() == 0.0);

    Run snowy;
    snowy.hold({.rain = 5.0F}, day);
    snowy.hold({.snow = 5.0F, .temperature = -10.0F}, day);
    CHECK(snowy.land.wetness(1) < 0.5 * open);
}

TEST_CASE("ground water fills over a wet season and seeps out through the dry one", "[sim][land]") {
    Run run;
    run.hold({.rain = 0.3F}, 120.0 * day);
    const double filled = run.ground();
    CHECK(filled > 50.0);
    CHECK(run.land.water_table_rise(1) > 0.2);

    // Months later the springs still run, though less.
    const double before = run.ran_off;
    run.hold({.temperature = 10.0F}, 90.0 * day);
    const double seeped = run.ran_off - before;
    CHECK(seeped > 0.1 * filled);
    CHECK(run.ground() < filled);
    CHECK(run.ground() > 0.3 * filled);
    const double rise = run.land.water_table_rise(1);
    run.hold({.temperature = 10.0F}, 180.0 * day);
    CHECK(run.land.water_table_rise(1) < rise);
}

TEST_CASE("land keeps every drop it isn't sent away", "[sim][land]") {
    Run run;
    const std::array spells{
        Spell{.rain = 2.0F, .evaporation = 0.1F},
        Spell{.snow = 1.5F, .evaporation = 0.05F, .temperature = -8.0F},
        Spell{.evaporation = 0.3F, .temperature = 6.0F},
        Spell{.rain = 8.0F, .temperature = 3.0F},
    };
    // Within float rounding over the spell's steps.
    constexpr double margin = 0.01;
    for (int round = 0; round < 3; ++round) {
        for (const Spell& spell : spells) {
            const double before = run.land.water(1);
            const double ran = run.ran_off;
            run.hold(spell, 10.0 * day);
            const double in = 240.0 * static_cast<double>(spell.rain + spell.snow);
            const double kept = run.land.water(1) - before + (run.ran_off - ran);
            // Only evaporation takes water, and no more than the weather drew.
            CHECK(in - kept >= -margin);
            CHECK(in - kept <= (240.0 * static_cast<double>(spell.evaporation)) + margin);
        }
    }
}

TEST_CASE("the sea holds no land water", "[sim][land]") {
    Run run;
    run.hold({.rain = 5.0F, .snow = 5.0F, .temperature = -5.0F}, day);
    CHECK(run.land.water(0) == 0.0);
    CHECK(run.land.fields().runoff[0] == 0.0F);
    CHECK(run.land.water(1) > 0.0);
}
