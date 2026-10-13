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
constexpr double radius = 115'000.0;

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
    Land land{grid(), island(), LandTuning{}, radius};
    double ran_off = 0.0; // mm from cell 1

    void hold(const Spell& spell, double seconds) {
        const std::vector<float> rain(grid().size(), spell.rain);
        const std::vector<float> snow(grid().size(), spell.snow);
        const std::vector<float> evaporation(grid().size(), spell.evaporation);
        const std::vector<float> temperature(grid().size(), spell.temperature);
        const std::vector<float> hail(grid().size());
        const LandWeather weather{.rain = rain,
                                  .snow = snow,
                                  .hail = hail,
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

namespace {

const CellGrid& fine() {
    static const CellGrid grid{3};
    return grid;
}

// Sea south of the equator, and land rising northwards from it.
std::vector<Ground> hillside() {
    std::vector<Ground> ground(fine().size());
    for (std::uint32_t index = 0; index < fine().size(); ++index) {
        const double north = fine().frame(index).up.z;
        ground[index] = Ground{.elevation = static_cast<float>(std::max(north, 0.0) * 2'000.0),
                               .ocean = north < 0.0};
    }
    return ground;
}

struct Hillside {
    Land land{fine(), hillside(), LandTuning{}, radius};

    void hold(float rain, double seconds, std::uint32_t only = UINT32_MAX) {
        std::vector<float> falling(fine().size(), only == UINT32_MAX ? rain : 0.0F);
        if (only != UINT32_MAX) {
            falling[only] = rain;
        }
        const std::vector<float> none(fine().size());
        const std::vector<float> mild(fine().size(), 10.0F);
        const LandWeather weather{.rain = falling,
                                  .snow = none,
                                  .hail = none,
                                  .evaporation = none,
                                  .temperature = mild,
                                  .seconds = hour};
        const auto steps = static_cast<int>(std::lround(seconds / hour));
        for (int left = steps; left > 0; --left) {
            land.step(fine(), weather, nullptr);
        }
    }

    // m³ held on and under the land.
    [[nodiscard]] double stored() const {
        double total = 0.0;
        for (std::uint32_t index = 0; index < fine().size(); ++index) {
            total += (land.water(index) / 1'000.0 * fine().area(index) * radius * radius) +
                     land.fields().channel[index];
        }
        return total;
    }
};

// The land cell farthest north, where the rivers start.
std::uint32_t summit() {
    std::uint32_t best = 0;
    for (std::uint32_t index = 1; index < fine().size(); ++index) {
        if (fine().frame(index).up.z > fine().frame(best).up.z) {
            best = index;
        }
    }
    return best;
}

} // namespace

TEST_CASE("rivers run downhill to the sea", "[sim][land]") {
    const Hillside hill;
    const std::vector<Ground> ground = hillside();
    for (std::uint32_t index = 0; index < fine().size(); ++index) {
        if (ground[index].ocean) {
            continue;
        }
        // Follow it down: every step lower, and the sea in the end.
        std::uint32_t at = index;
        for (int steps = 0; !ground[at].ocean; ++steps) {
            const std::uint32_t next = hill.land.receiver(at);
            REQUIRE(next != at);
            REQUIRE(steps < 100);
            REQUIRE(ground[next].elevation <= ground[at].elevation);
            at = next;
        }
    }
}

TEST_CASE("rivers gather what falls upstream and carry it all to the sea", "[sim][land]") {
    Hillside hill;
    hill.hold(20.0F, 2.0 * day);
    const double fallen = 20.0 * 48.0 * [] {
        double area = 0.0;
        for (std::uint32_t index = 0; index < fine().size(); ++index) {
            area += hillside()[index].ocean ? 0.0 : fine().area(index) * radius * radius;
        }
        return area;
    }() / 1'000.0;
    CHECK(hill.stored() + hill.land.discharged() == Catch::Approx(fallen).epsilon(1e-4));

    Hillside one;
    const std::uint32_t top = summit();
    one.hold(50.0F, day, top);
    one.hold(0.0F, 2.0 * day);
    // Every cell below the summit carries its water, none beside it.
    std::uint32_t at = one.land.receiver(top);
    CHECK(one.land.fields().channel[top] > 0.0);
    double carried = 0.0;
    for (std::uint32_t index = 0; index < fine().size(); ++index) {
        carried += index == top ? 0.0 : one.land.fields().channel[index];
    }
    double along = 0.0;
    for (; !hillside()[at].ocean; at = one.land.receiver(at)) {
        along += one.land.fields().channel[at];
    }
    CHECK(along == Catch::Approx(carried));
}

TEST_CASE("rivers flood after heavy rain and run low through a drought", "[sim][land]") {
    Hillside hill;
    // Long enough for the ground water to fill.
    hill.hold(0.15F, 4.0 * 365.0 * day);
    const std::uint32_t top = summit();
    std::uint32_t mouth = top;
    while (!hillside()[hill.land.receiver(mouth)].ocean) {
        mouth = hill.land.receiver(mouth);
    }
    CHECK(hill.land.flow_ratio(mouth) == Catch::Approx(1.0).epsilon(0.2));

    Hillside flooded = hill;
    flooded.hold(15.0F, 12.0 * hour);
    CHECK(flooded.land.flow_ratio(mouth) > 3.0);
    // The flood passes within days.
    flooded.hold(0.15F, 5.0 * day);
    CHECK(flooded.land.flow_ratio(mouth) < 2.0);

    hill.hold(0.0F, 60.0 * day);
    const double low = hill.land.flow_ratio(mouth);
    CHECK(low < 0.8);
    // Still fed by the ground water.
    CHECK(low > 0.1);
}
