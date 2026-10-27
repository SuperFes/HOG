#include <hog/client/daylight.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::GameTime;
using hog::client::EyeLight;
using hog::client::PlanetClock;

namespace {

GameTime noon_utc() {
    return hog::to_game_time(
        hog::WallClock::time_point{std::chrono::sys_days{std::chrono::year{2026} / 10 / 15} + 12h});
}

const hog::atmosphere::Model& earth() {
    static const hog::atmosphere::Model model;
    return model;
}

} // namespace

TEST_CASE("the planet's clock runs at its rate, from real time up to its fastest",
          "[client][daylight]") {
    PlanetClock clock{noon_utc()};
    clock.advance(1.5);
    CHECK(clock.time() == noon_utc() + 1'500ms);
    clock.faster();
    clock.faster();
    CHECK(clock.rate() == 4.0);
    clock.advance(0.25);
    CHECK(clock.time() == noon_utc() + 2'500ms);
    // Time too short to tick over still counts once it adds up.
    clock.slower();
    clock.slower();
    clock.slower();
    CHECK(clock.rate() == 1.0);
    for (int step = 0; step < 4; ++step) {
        clock.advance(2.5e-7);
    }
    CHECK(clock.time() == noon_utc() + 2'500'001us);
    for (int step = 0; step < 20; ++step) {
        clock.faster();
    }
    CHECK(clock.rate() == hog::client::fastest_time);
}

TEST_CASE("the planet's day starts when asked where the eye is", "[client][daylight]") {
    const hog::sim::Calendar calendar{4h, 23.44};
    const DVec3 up = glm::normalize(DVec3{0.3, -0.8, 0.5});
    CHECK(hog::client::start_time(calendar, noon_utc(), up, std::nullopt) == noon_utc());
    for (const double wanted : {0.0, 0.25, 0.5, 0.9}) {
        const GameTime start = hog::client::start_time(calendar, noon_utc(), up, wanted);
        INFO("at " << wanted);
        CHECK(start >= noon_utc());
        CHECK(start < noon_utc() + 4h);
        const double found = calendar.local_time_of_day(start, up);
        CHECK(std::abs(std::remainder(found - wanted, 1.0)) < 1e-6);
    }
}

TEST_CASE("the eye is lit by the sun by day and by a faint glow by night", "[client][daylight]") {
    const EyeLight noon = hog::client::light_at_eye(earth(), 2.0, 1.0);
    const EyeLight night = hog::client::light_at_eye(earth(), 2.0, -0.5);
    CHECK(noon.sun.g > 0.8);
    CHECK(noon.sun.g > 5.0 * noon.sky.g);
    CHECK(night.sun == DVec3{0.0});
    CHECK(night.sky.b > 0.0);
    CHECK(night.sky.b < 0.01 * (noon.sun.b + noon.sky.b));
}

TEST_CASE("under cloud the sun is hidden and the sky lit by what it scatters through",
          "[client][daylight]") {
    const EyeLight noon = hog::client::light_at_eye(earth(), 2.0, 1.0);
    const EyeLight clear = hog::client::under_cloud(noon, hog::render::cloud_shade(0.0, 0.0));
    CHECK(clear.sun == noon.sun);
    CHECK(clear.sky == noon.sky);
    const EyeLight overcast = hog::client::under_cloud(noon, hog::render::cloud_shade(1.0, 0.1));
    CHECK(overcast.sun.g < 0.01 * noon.sun.g);
    CHECK(overcast.sky.g > noon.sky.g);
    CHECK(overcast.sun.g + overcast.sky.g < 0.5 * (noon.sun.g + noon.sky.g));
    // Darker, but adapted to it, not by as much.
    const double seen_clear = hog::client::exposure_for(noon) * (noon.sun.g + noon.sky.g);
    const double seen_overcast =
        hog::client::exposure_for(overcast) * (overcast.sun.g + overcast.sky.g);
    CHECK(seen_overcast < seen_clear);
    CHECK(seen_overcast > 0.25 * seen_clear);
}

TEST_CASE("exposure adapts to the light, but brighter light still looks brighter",
          "[client][daylight]") {
    const auto seen = [](double sun_mu) {
        const EyeLight light = hog::client::light_at_eye(earth(), 2.0, sun_mu);
        const double exposure = hog::client::exposure_for(light);
        return std::pair{exposure, exposure * (light.sun.g + light.sky.g)};
    };
    const auto [noon_exposure, noon] = seen(1.0);
    const auto [low_exposure, low] = seen(0.1);
    const auto [night_exposure, night] = seen(-0.5);
    CHECK_THAT(noon_exposure, WithinAbs(10.0, 2.0));
    CHECK(low_exposure > noon_exposure);
    CHECK(night_exposure > low_exposure);
    CHECK(noon > low);
    CHECK(low > night);
    CHECK(night > 0.01 * noon);
}

TEST_CASE("exposure moves towards the light's over time", "[client][daylight]") {
    CHECK(hog::client::adapted(10.0, 100.0, 0.0) == 10.0);
    const double soon = hog::client::adapted(10.0, 100.0, 0.25);
    const double later = hog::client::adapted(10.0, 100.0, 1.0);
    CHECK(soon > 10.0);
    CHECK(later > soon);
    CHECK(later < 100.0);
    CHECK_THAT(hog::client::adapted(10.0, 100.0, 60.0), WithinAbs(100.0, 1e-6));
    CHECK(hog::client::adapted(100.0, 10.0, 0.25) < 100.0);
}

TEST_CASE("the leaves keep the year where the eye is", "[client][daylight]") {
    const hog::sim::Calendar calendar{std::chrono::hours{4}, 23.44};
    const auto on = [](std::chrono::year_month_day day) {
        return hog::to_game_time(hog::WallClock::time_point{std::chrono::sys_days{day}});
    };
    const DVec3 north{std::cos(0.785), 0.0, std::sin(0.785)};
    const DVec3 south{north.x, 0.0, -north.z};
    using std::chrono::year;
    const auto october = hog::client::daylight_at(calendar, on(year{2026} / 10 / 20), north);
    CHECK_THAT(october.hours, WithinAbs(10.6, 0.3));
    CHECK_THAT(october.longest, WithinAbs(15.4, 0.1));
    CHECK_FALSE(october.lengthening);
    const auto april = hog::client::daylight_at(calendar, on(year{2026} / 4 / 20), north);
    CHECK(april.hours > 13.0F);
    CHECK(april.lengthening);
    // The south's spring as the north's autumn.
    CHECK(hog::client::daylight_at(calendar, on(year{2026} / 10 / 20), south).lengthening);
}
