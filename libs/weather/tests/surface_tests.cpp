#include <hog/core/time.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/weather/field.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using hog::GameTime;
using hog::globe::Planet;
using hog::sim::Calendar;
using namespace hog::weather;
using std::chrono::hours;

namespace {

Planet small_earth(double tilt = 23.44) {
    Planet planet;
    planet.body.radius = 20'000.0;
    planet.body.tilt = tilt;
    return planet;
}

struct Year {
    std::vector<float> high;
    std::vector<float> low;
    std::vector<float> june;
    std::vector<float> december;
};

// Runs a field three years in six-hour steps, keeping the last year's range and its two
// solstices' means.
Year run_years(const Planet& planet, Field& field) {
    const Calendar calendar{hog::GameDuration{std::chrono::hours{24}}, planet.body.tilt};
    const std::size_t columns = field.surface.size();
    Year year{.high = std::vector<float>(columns, 0.0F),
              .low = std::vector<float>(columns, 1e9F),
              .june = std::vector<float>(columns, 0.0F),
              .december = std::vector<float>(columns, 0.0F)};
    const int per_day = 4;
    const int days = 365;
    for (int step = 0; step < 3 * per_day * days; ++step) {
        const GameTime time{hours{6 * step}};
        warm_surface(field, planet, calendar.sun_direction(time), 6.0 * 3'600.0);
        if (step < 2 * per_day * days) {
            continue;
        }
        const int day = (step / per_day) % days;
        for (std::size_t column = 0; column < columns; ++column) {
            year.high[column] = std::max(year.high[column], field.surface[column]);
            year.low[column] = std::min(year.low[column], field.surface[column]);
            if (day == 171) {
                year.june[column] = field.surface[column];
            }
            if (day == 354) {
                year.december[column] = field.surface[column];
            }
        }
    }
    return year;
}

// The columns at mid-latitudes in the north, split by sea and land.
std::vector<std::size_t> columns_where(const Field& field, bool sea, double low, double high) {
    std::vector<std::size_t> found;
    for (std::size_t column = 0; column < field.surface.size(); ++column) {
        const double z = hog::globe::center(hog::globe::cell_at_index(field.grid.level, column)).z;
        if (z > low && z < high && (field.over_sea[column] != 0) == sea) {
            found.push_back(column);
        }
    }
    return found;
}

double mean_of(const std::vector<std::size_t>& columns, const std::vector<float>& values) {
    double total = 0.0;
    for (const std::size_t column : columns) {
        total += static_cast<double>(values[column]);
    }
    return total / static_cast<double>(columns.size());
}

} // namespace

TEST_CASE("a surface warms in sunlight and cools without it", "[weather][surface]") {
    const Planet planet = small_earth();
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 2, 4);
    const std::size_t column = columns_where(field, false, -0.2, 0.2).front();
    const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(2, column));
    const float start = field.surface[column];
    warm_surface(field, planet, up, 6.0 * 3'600.0);
    const float noon = field.surface[column];
    CHECK(noon > start);
    warm_surface(field, planet, -up, 12.0 * 3'600.0);
    CHECK(field.surface[column] < noon);
}

TEST_CASE("the sun holds the planet's mean climate through a year", "[weather][surface]") {
    const Planet planet = small_earth();
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 4);
    const Year year = run_years(planet, field);
    double total = 0.0;
    for (std::size_t column = 0; column < year.high.size(); ++column) {
        total +=
            0.5 * (static_cast<double>(year.high[column]) + static_cast<double>(year.low[column]));
    }
    const double mean = total / static_cast<double>(year.high.size());
    CHECK(std::abs(mean - hog::globe::mean_temperature(planet)) < 15.0);
}

TEST_CASE("tilt gives the north a warm summer, more over land than sea", "[weather][surface]") {
    const Planet planet = small_earth();
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 4);
    const Year year = run_years(planet, field);
    const auto land = columns_where(field, false, 0.5, 0.9);
    const auto sea = columns_where(field, true, 0.5, 0.9);
    REQUIRE_FALSE(land.empty());
    REQUIRE_FALSE(sea.empty());
    const double land_swing = mean_of(land, year.june) - mean_of(land, year.december);
    const double sea_swing = mean_of(sea, year.june) - mean_of(sea, year.december);
    CHECK(land_swing > 10.0);
    CHECK(land_swing > 1.5 * sea_swing);
}

TEST_CASE("an upright planet has no seasons", "[weather][surface]") {
    const Planet planet = small_earth(0.0);
    const hog::globe::Generator generator{planet};
    Field field = start_field(generator, 3, 4);
    const Year year = run_years(planet, field);
    const auto land = columns_where(field, false, 0.5, 0.9);
    REQUIRE_FALSE(land.empty());
    CHECK(std::abs(mean_of(land, year.june) - mean_of(land, year.december)) < 3.0);
}
