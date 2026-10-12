#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/weather.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

using namespace std::chrono;
using hog::DVec3;
using hog::GameTime;
using hog::jobs::JobSystem;
using hog::sim::Ground;
using hog::sim::Weather;
using hog::sim::WeatherConfig;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr int level = 3;

constexpr GameTime at(sys_days day, hours time = hours{0}) {
    return GameTime{(day + time).time_since_epoch()};
}

constexpr GameTime march_equinox = at(2026y / March / 20, hours{15});
constexpr GameTime june_solstice = at(2026y / June / 21, hours{9});

WeatherConfig config(minutes step = minutes{10}) {
    return WeatherConfig{.level = level, .step = step, .day = hours{24}, .axial_tilt = 23.44};
}

std::vector<Ground> everywhere(Ground ground) {
    return std::vector<Ground>(hog::sim::CellGrid{level}.size(), ground);
}

constexpr Ground land{
    .mean_temperature = 15.0F, .elevation = 0.0F, .wetness = 0.5F, .ocean = false};
constexpr Ground sea{};

std::uint32_t cell_at(const Weather& weather, double latitude, double longitude) {
    const DVec3 up{std::cos(latitude * degree) * std::cos(longitude * degree),
                   std::cos(latitude * degree) * std::sin(longitude * degree),
                   std::sin(latitude * degree)};
    return weather.grid().index(hog::planet::cell_at(up, level));
}

} // namespace

TEST_CASE("weather steps fall on whole steps of game time", "[sim][weather]") {
    JobSystem jobs{1};
    Weather weather{config(), everywhere(sea), march_equinox + minutes{3}};
    CHECK(weather.time() == march_equinox);
    CHECK(weather.advance_to(march_equinox + minutes{35}, jobs) == 3);
    CHECK(weather.time() == march_equinox + minutes{30});
    CHECK(weather.advance_to(march_equinox + minutes{39}, jobs) == 0);
}

TEST_CASE("summer warms its hemisphere, land more than sea", "[sim][weather]") {
    JobSystem jobs{1};
    const auto contrast = [&](Ground ground) {
        Weather weather{config(hours{3}), everywhere(ground), march_equinox};
        const float north_before = weather.fields().temperature[cell_at(weather, 45.0, 0.0)];
        weather.advance_to(june_solstice, jobs);
        const auto& temperature = weather.fields().temperature;
        CHECK(temperature[cell_at(weather, 45.0, 0.0)] > north_before);
        return temperature[cell_at(weather, 45.0, 0.0)] - temperature[cell_at(weather, -45.0, 0.0)];
    };
    const float on_land = contrast(land);
    const float at_sea = contrast(sea);
    CHECK(on_land > 15.0F);
    CHECK(at_sea > 1.0F);
    CHECK(at_sea < on_land / 2.0F);
}

TEST_CASE("land warms by day and cools by night; the sea barely does", "[sim][weather]") {
    JobSystem jobs{1};
    const auto daily_range = [&](Ground ground) {
        Weather weather{config(hours{1}), everywhere(ground), march_equinox};
        weather.advance_to(march_equinox + hours{48}, jobs);
        const std::uint32_t cell = cell_at(weather, 0.0, 0.0);
        float low = weather.fields().temperature[cell];
        float high = low;
        for (int hour = 1; hour <= 24; ++hour) {
            weather.advance_to(march_equinox + hours{48 + hour}, jobs);
            low = std::min(low, weather.fields().temperature[cell]);
            high = std::max(high, weather.fields().temperature[cell]);
        }
        return high - low;
    };
    CHECK(daily_range(land) > 5.0F);
    CHECK(daily_range(sea) < 1.0F);
}

TEST_CASE("surface winds follow Earth's belts", "[sim][weather]") {
    const Weather weather{config(), everywhere(sea), march_equinox};
    const auto& wind = weather.fields().wind;
    for (const double longitude : {0.0, 90.0, 200.0}) {
        // Trade winds from the east, turned towards the equator; westerlies further out.
        const auto north_trades = wind[cell_at(weather, 15.0, longitude)];
        const auto south_trades = wind[cell_at(weather, -15.0, longitude)];
        CHECK(north_trades.x < -1.0F);
        CHECK(north_trades.y < 0.0F);
        CHECK(south_trades.x < -1.0F);
        CHECK(south_trades.y > 0.0F);
        CHECK(wind[cell_at(weather, 45.0, longitude)].x > 1.0F);
        CHECK(wind[cell_at(weather, -45.0, longitude)].x > 1.0F);
    }
}

TEST_CASE("winds blow partly down the pressure gradient", "[sim][weather]") {
    JobSystem jobs{1};
    Weather weather{config(hours{1}), everywhere(land), march_equinox};
    weather.advance_to(march_equinox + hours{30}, jobs);
    const auto& grid = weather.grid();
    const auto& fields = weather.fields();
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        hog::DVec2 gradient{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            gradient += grid.gradient_weights(index)[k] *
                        static_cast<double>(fields.pressure[grid.neighbors(index)[k]] -
                                            fields.pressure[index]);
        }
        if (glm::length(gradient) > 1.0) {
            CHECK(glm::dot(hog::DVec2{fields.wind[index]}, gradient) < 0.0);
        }
    }
}

TEST_CASE("a westerly jet runs above the middle latitudes", "[sim][weather]") {
    const Weather weather{config(), everywhere(sea), at(2026y / January / 15)};
    const auto& jet = weather.fields().jet;
    const auto winter = jet[cell_at(weather, 40.0, 0.0)];
    const auto summer = jet[cell_at(weather, -40.0, 0.0)];
    CHECK(winter.x > 20.0F);
    CHECK(summer.x > 10.0F);
    CHECK(winter.x > summer.x);
    CHECK(std::abs(jet[cell_at(weather, 2.0, 0.0)].x) < winter.x / 2.0F);
}

TEST_CASE("water is neither made nor lost", "[sim][weather]") {
    JobSystem jobs{2};
    constexpr Ground tropics{
        .mean_temperature = 27.0F, .elevation = 0.0F, .wetness = 1.0F, .ocean = false};
    Weather weather{config(), everywhere(tropics), march_equinox};
    const auto& f = weather.fields();
    for (int step = 1; step <= 300; ++step) {
        std::vector<double> before(f.size());
        for (std::size_t index = 0; index < f.size(); ++index) {
            before[index] =
                static_cast<double>(f.vapour[index]) + static_cast<double>(f.cloud[index]);
        }
        weather.advance_to(march_equinox + (step * minutes{10}), jobs);
        for (std::size_t index = 0; index < f.size(); ++index) {
            const double in = before[index] + static_cast<double>(f.evaporation[index]);
            const double out =
                static_cast<double>(f.vapour[index]) + static_cast<double>(f.cloud[index]) +
                static_cast<double>(f.rain[index]) + static_cast<double>(f.snow[index]) +
                static_cast<double>(f.hail[index]);
            REQUIRE(std::abs(in - out) < 1e-4 * std::max(in, 1.0));
        }
    }
}

TEST_CASE("hot, wet afternoons bring thunderstorms; cold air brings snow", "[sim][weather]") {
    JobSystem jobs{1};
    constexpr Ground tropics{
        .mean_temperature = 30.0F, .elevation = 0.0F, .wetness = 1.0F, .ocean = false};
    Weather hot{config(), everywhere(tropics), march_equinox};
    float storm = 0.0F;
    float rain = 0.0F;
    for (int hour = 1; hour <= 48; ++hour) {
        hot.advance_to(march_equinox + hours{hour}, jobs);
        storm = std::max(storm, std::ranges::max(hot.fields().storm));
        rain += std::accumulate(hot.fields().rain.begin(), hot.fields().rain.end(), 0.0F);
    }
    CHECK(storm > 0.3F);
    CHECK(rain > 0.0F);

    constexpr Ground frozen{
        .mean_temperature = -15.0F, .elevation = 0.0F, .wetness = 1.0F, .ocean = false};
    Weather cold{config(), everywhere(frozen), march_equinox};
    float snow = 0.0F;
    for (int hour = 1; hour <= 48; ++hour) {
        cold.advance_to(march_equinox + hours{hour}, jobs);
        const auto& f = cold.fields();
        for (std::size_t index = 0; index < f.size(); ++index) {
            if (f.temperature[index] < -2.0F) {
                CHECK(f.rain[index] == 0.0F);
            }
            snow += f.snow[index];
        }
    }
    CHECK(snow > 0.0F);
}

TEST_CASE("weather comes out the same on any number of threads", "[sim][weather]") {
    JobSystem one{1};
    JobSystem many{4};
    Weather a{config(), everywhere(land), march_equinox};
    Weather b{config(), everywhere(land), march_equinox};
    const GameTime later = march_equinox + hours{30};
    a.advance_to(later, one);
    b.advance_to(later, many);
    CHECK(a.fields() == b.fields());
}

TEST_CASE("weather saves and loads", "[sim][weather]") {
    JobSystem jobs{1};
    Weather saved{config(), everywhere(land), march_equinox};
    saved.advance_to(march_equinox + hours{5}, jobs);
    const auto bytes = saved.save();

    Weather loaded{config(), everywhere(land), june_solstice};
    REQUIRE(loaded.load(Weather::save_version, bytes));
    CHECK(loaded.time() == saved.time());
    CHECK(loaded.fields() == saved.fields());
    saved.advance_to(march_equinox + hours{9}, jobs);
    loaded.advance_to(march_equinox + hours{9}, jobs);
    CHECK(loaded.fields() == saved.fields());

    Weather finer{WeatherConfig{.level = level + 1}, std::vector<Ground>(6U << 8U), june_solstice};
    const auto other_level = finer.load(Weather::save_version, bytes);
    REQUIRE_FALSE(other_level);
    CHECK(other_level.error().code == hog::ErrorCode::InvalidArgument);
    const auto newer = loaded.load(Weather::save_version + 1, bytes);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == hog::ErrorCode::Unsupported);

    const std::span<const std::byte> all{bytes};
    CHECK_FALSE(loaded.load(Weather::save_version, all.first(all.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(loaded.load(Weather::save_version, longer));
    CHECK(loaded.fields() == saved.fields());
}

TEST_CASE("weather ground comes from the planet", "[sim][weather]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = 115'000.0}};
    const hog::sim::CellGrid grid{level};
    const auto ground = hog::sim::ground_for(grid, world);
    REQUIRE(ground.size() == grid.size());
    const auto oceans = std::ranges::count_if(ground, [](const Ground& g) { return g.ocean; });
    CHECK(oceans > 0);
    CHECK(oceans < static_cast<std::ptrdiff_t>(ground.size()));
    for (const Ground& g : ground) {
        CHECK(g.wetness >= 0.0F);
        CHECK(g.wetness <= 1.0F);
        if (g.ocean) {
            CHECK(g.elevation == 0.0F);
        }
    }
}

TEST_CASE("weather rejects bad input", "[sim][weather]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS((Weather{config(), std::vector<Ground>(3), march_equinox}), AssertionFailed);
    auto no_step = config();
    no_step.step = hog::GameDuration::zero();
    CHECK_THROWS_AS((Weather{no_step, everywhere(sea), march_equinox}), AssertionFailed);
    auto no_day = config();
    no_day.day = hog::GameDuration::zero();
    CHECK_THROWS_AS((Weather{no_day, everywhere(sea), march_equinox}), AssertionFailed);
}
