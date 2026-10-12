#include <hog/viewer/daylight.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::GameTime;
using hog::sim::Calendar;
using hog::viewer::sunlight;

namespace {

constexpr double degree = std::numbers::pi / 180.0;

GameTime at(sys_days day, minutes time) {
    return hog::to_game_time(hog::WallClock::time_point{day + time});
}

// Noon at longitude 0 with four-hour days, within an hour of the March equinox.
GameTime equinox_noon() {
    return at(2026y / March / 20, 14h);
}

hog::probe::Welcome welcome(hog::GameDuration day, double tilt) {
    hog::probe::Welcome welcome;
    welcome.day_length = day;
    welcome.axial_tilt = tilt;
    return welcome;
}

} // namespace

TEST_CASE("daylight fades through twilight", "[viewer][daylight]") {
    CHECK(sunlight(90.0 * degree) == 1.0);
    CHECK(sunlight(0.0) == 1.0);
    CHECK_THAT(sunlight(-3.0 * degree), WithinAbs(0.5, 1e-12));
    CHECK(sunlight(-6.0 * degree) == 0.0);
    CHECK(sunlight(-90.0 * degree) == 0.0);
    for (int quarter = -28; quarter < 4; ++quarter) {
        const double elevation = quarter * 0.25 * degree;
        CHECK(sunlight(elevation) <= sunlight(elevation + (0.25 * degree)));
    }
}

TEST_CASE("the calendar comes from the server's welcome", "[viewer][daylight]") {
    const auto calendar = hog::viewer::calendar_of(welcome(hours{4}, 23.44));
    CHECK(calendar.transform(&Calendar::day_length) == std::optional{hog::GameDuration{hours{4}}});

    CHECK_FALSE(hog::viewer::calendar_of(hog::probe::Welcome{}));
    CHECK_FALSE(hog::viewer::calendar_of(welcome(-hours{4}, 23.44)));
    CHECK_FALSE(hog::viewer::calendar_of(welcome(hours{4}, -1.0)));
    CHECK_FALSE(hog::viewer::calendar_of(welcome(hours{4}, 90.5)));
    CHECK_FALSE(
        hog::viewer::calendar_of(welcome(hours{4}, std::numeric_limits<double>::quiet_NaN())));
}

TEST_CASE("the sun is overhead where the map says", "[viewer][daylight]") {
    const Calendar calendar{hours{4}, 23.44};
    const DVec2 noon = hog::viewer::subsolar_point(calendar, equinox_noon());
    CHECK_THAT(noon.x, WithinAbs(0.5, 1e-3));
    CHECK_THAT(noon.y, WithinAbs(0.5, 1e-3));

    // Six hours of four-hour days later, it's midnight at longitude 0.
    const DVec2 midnight = hog::viewer::subsolar_point(calendar, equinox_noon() + hours{6});
    CHECK_THAT(std::abs(midnight.x - 0.5), WithinAbs(0.5, 1e-3));

    const DVec2 solstice = hog::viewer::subsolar_point(calendar, at(2026y / June / 21, 8h + 24min));
    CHECK_THAT(solstice.y, WithinAbs(0.5 - (23.44 / 180.0), 1e-4));

    // An hour later the sun has moved a quarter of the way west.
    const DVec2 later = hog::viewer::subsolar_point(calendar, equinox_noon() + hours{1});
    CHECK_THAT(later.x, WithinAbs(0.25, 1e-3));
}

TEST_CASE("night darkens the far side of the map", "[viewer][daylight]") {
    constexpr int width = 64;
    constexpr std::uint8_t grey = 200;
    hog::mapgen::Image image{.width = width,
                             .height = width / 2,
                             .pixels = std::vector<std::uint8_t>(3 * width * width / 2, grey)};
    hog::jobs::JobSystem jobs{2};
    hog::viewer::shade_night(image, Calendar{hours{4}, 23.44}, equinox_noon(), jobs);

    const auto night = static_cast<std::uint8_t>(std::lround(grey * hog::viewer::night_light));
    // Noon at the centre, midnight at the edges, and dawn and dusk a quarter of the way in.
    CHECK(image.at(width / 2, width / 4) == hog::mapgen::Rgb{grey, grey, grey});
    CHECK(image.at(0, width / 4) == hog::mapgen::Rgb{night, night, night});
    CHECK(image.at(width - 1, width / 4) == hog::mapgen::Rgb{night, night, night});
    const std::uint8_t dusk = image.at((width / 4) - 1, width / 4)[0];
    CHECK(dusk > night);
    CHECK(dusk < grey);
}
