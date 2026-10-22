#include <hog/jobs/job_system.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/weather.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <utility>
#include <vector>

using namespace std::chrono;
using hog::DVec3;
using hog::GameTime;
using hog::Vec2;
using hog::jobs::JobSystem;
using hog::sim::Ground;
using hog::sim::Weather;
using hog::sim::WeatherConfig;
using hog::sim::WeatherFields;

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

// An ocean warm at the equator and cold at the poles, as Earth's is.
std::vector<Ground> zonal_sea(int grid_level) {
    const hog::sim::CellGrid grid{grid_level};
    std::vector<Ground> ground(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const double sin_latitude = grid.frame(index).up.z;
        ground[index].mean_temperature =
            static_cast<float>(28.0 - (35.0 * sin_latitude * sin_latitude));
    }
    return ground;
}

// A sea 120° wide between the poles, warm at the equator, with land all round it.
std::vector<Ground> basin() {
    std::vector<Ground> ground = zonal_sea(level);
    const hog::sim::CellGrid grid{level};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const DVec3 up = grid.frame(index).up;
        if (std::abs(std::atan2(up.y, up.x)) > 60.0 * degree) {
            ground[index].ocean = false;
            ground[index].wetness = 0.5F;
        }
    }
    return ground;
}

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
    const auto& grid = weather.grid();
    const auto total = [&](auto... parts) {
        double sum = 0.0;
        for (std::uint32_t index = 0; index < grid.size(); ++index) {
            sum += grid.area(index) * (static_cast<double>(parts[index]) + ...);
        }
        return sum;
    };
    for (int step = 1; step <= 300; ++step) {
        const double held = total(f.vapour, f.cloud);
        weather.advance_to(march_equinox + (step * minutes{10}), jobs);
        const double in = held + total(f.evaporation);
        const double out = total(f.vapour, f.cloud) + total(f.rain, f.snow, f.hail);
        REQUIRE(std::abs(in - out) < 1e-5 * in);
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

TEST_CASE("weather drifts east in the westerlies and west in the trades", "[sim][weather]") {
    JobSystem jobs{1};
    const WeatherConfig fine{.level = 4};
    const std::vector<Ground> ground(hog::sim::CellGrid{fine.level}.size(), sea);
    // How far east a warm patch has moved after two days, as the longitude of the middle of its
    // difference from undisturbed weather.
    const auto drift = [&](double latitude, std::vector<float> WeatherFields::* layer) {
        Weather calm{fine, ground, march_equinox};
        Weather warm{fine, ground, march_equinox};
        auto fields = warm.fields();
        const DVec3 up{std::cos(latitude * degree), 0.0, std::sin(latitude * degree)};
        (fields.*layer)[warm.grid().index(hog::planet::cell_at(up, fine.level))] += 0.5F;
        warm.set_fields(fields);
        calm.advance_to(march_equinox + hours{48}, jobs);
        warm.advance_to(march_equinox + hours{48}, jobs);
        double weight = 0.0;
        double longitude = 0.0;
        for (std::uint32_t index = 0; index < warm.grid().size(); ++index) {
            const double difference = std::max(
                static_cast<double>((warm.fields().*layer)[index] - (calm.fields().*layer)[index]),
                0.0);
            const DVec3 at = warm.grid().frame(index).up;
            if (glm::dot(at, up) < std::cos(40.0 * degree)) {
                continue;
            }
            weight += difference;
            longitude += difference * std::atan2(at.y, at.x) / degree;
        }
        return longitude / weight;
    };
    const double westerlies = drift(45.0, &WeatherFields::upper_temperature);
    const double trades = drift(15.0, &WeatherFields::temperature);
    // Patterns travel slower than the wind that carries them, as Earth's do.
    CHECK(westerlies > 3.0);
    CHECK(trades < -1.0);
}

TEST_CASE("air rises where the trades meet and sinks under the subtropical highs",
          "[sim][weather]") {
    const WeatherConfig fine{.level = 4};
    const Weather weather{fine, std::vector<Ground>(hog::sim::CellGrid{fine.level}.size(), sea),
                          march_equinox};
    double equator = 0.0;
    double subtropics = 0.0;
    for (std::uint32_t index = 0; index < weather.grid().size(); ++index) {
        const double latitude = std::abs(std::asin(weather.grid().frame(index).up.z)) / degree;
        const auto lift = static_cast<double>(weather.fields().lift[index]);
        if (latitude < 6.0) {
            equator += lift;
        } else if (latitude > 22.0 && latitude < 35.0) {
            subtropics += lift;
        }
    }
    CHECK(equator > 0.0);
    CHECK(subtropics < 0.0);
}

TEST_CASE("mountains rain on their windward side and shelter their lee", "[sim][weather]") {
    JobSystem jobs{1};
    const WeatherConfig fine{.level = 4};
    const hog::sim::CellGrid grid{fine.level};
    const auto longitude = [&](std::uint32_t index) {
        const DVec3 up = grid.frame(index).up;
        return std::atan2(up.y, up.x) / degree;
    };
    const auto latitude = [&](std::uint32_t index) {
        return std::asin(grid.frame(index).up.z) / degree;
    };
    // A ridge running north and south through the westerlies.
    const std::vector<Ground> flat(grid.size(), Ground{.mean_temperature = 12.0F});
    std::vector<Ground> ridged = flat;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (std::abs(longitude(index)) < 3.0 && latitude(index) > 30.0 && latitude(index) < 60.0) {
            // As cool as the climate makes high ground.
            ridged[index] = Ground{
                .mean_temperature = -4.0F, .elevation = 2'500.0F, .wetness = 0.5F, .ocean = false};
        }
    }
    // Rain upwind of the ridge and water vapour downwind, over five days.
    const auto rain = [&](const std::vector<Ground>& ground) {
        Weather weather{fine, ground, march_equinox};
        double windward = 0.0;
        double lee = 0.0;
        for (int hour = 1; hour <= 120; ++hour) {
            weather.advance_to(march_equinox + hours{hour}, jobs);
            for (std::uint32_t index = 0; index < grid.size(); ++index) {
                if (latitude(index) < 35.0 || latitude(index) > 55.0) {
                    continue;
                }
                if (longitude(index) > -9.0 && longitude(index) < -3.0) {
                    windward += static_cast<double>(weather.fields().rain[index] +
                                                    weather.fields().snow[index]);
                } else if (longitude(index) > 3.0 && longitude(index) < 9.0) {
                    lee += static_cast<double>(weather.fields().vapour[index]);
                }
            }
        }
        return std::pair{windward, lee};
    };
    const auto [flat_rain, flat_vapour] = rain(flat);
    const auto [windward_rain, lee_vapour] = rain(ridged);
    CHECK(windward_rain > flat_rain * 1.2);
    // Air coming down the far side is drier.
    CHECK(lee_vapour < flat_vapour * 0.85);
}

TEST_CASE("storm systems form in the middle latitudes and travel east", "[sim][weather]") {
    JobSystem jobs{1};
    const WeatherConfig fine{.level = 4, .seed = 7};
    Weather weather{fine, zonal_sea(fine.level), march_equinox};
    weather.advance_to(march_equinox + hours{144}, jobs);
    const auto before = weather.systems();
    REQUIRE(before.size() >= 4);
    for (const hog::sim::StormSystem& system : before) {
        const double latitude = std::abs(std::asin(system.centre.z)) / degree;
        CHECK(latitude > 20.0);
        CHECK(latitude < 75.0);
    }

    weather.advance_to(march_equinox + hours{156}, jobs);
    int tracked = 0;
    int eastward = 0;
    for (const hog::sim::StormSystem& was : before) {
        for (const hog::sim::StormSystem& is : weather.systems()) {
            if (is.id == was.id) {
                ++tracked;
                const double east = std::remainder(std::atan2(is.centre.y, is.centre.x) -
                                                       std::atan2(was.centre.y, was.centre.x),
                                                   2.0 * std::numbers::pi);
                eastward += east > 0.0 ? 1 : 0;
            }
        }
    }
    REQUIRE(tracked > 0);
    CHECK(eastward * 4 >= tracked * 3);

    Weather loaded{fine, zonal_sea(fine.level), june_solstice};
    REQUIRE(loaded.load(Weather::save_version, weather.save()));
    CHECK(loaded.systems() == weather.systems());
}

TEST_CASE("storm systems are lows the wind circles, with fronts around them", "[sim][weather]") {
    JobSystem jobs{1};
    const WeatherConfig fine{.level = 4, .seed = 7};
    Weather weather{fine, zonal_sea(fine.level), march_equinox};
    weather.advance_to(march_equinox + hours{144}, jobs);
    const auto& grid = weather.grid();
    const auto& fields = weather.fields();
    const auto strongest =
        std::ranges::max_element(weather.systems(), {}, [&](const hog::sim::StormSystem& system) {
            return system.strength(fine.tuning.systems.growing);
        });
    REQUIRE(strongest != weather.systems().end());
    const hog::sim::StormSystem& system = *strongest;
    REQUIRE(system.strength(fine.tuning.systems.growing) > 5.0);

    double centre_pressure = 2'000.0;
    double ring_pressure = 0.0;
    int ring = 0;
    double circulation = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto& frame = grid.frame(index);
        const double away = std::acos(std::clamp(glm::dot(frame.up, system.centre), -1.0, 1.0)) /
                            static_cast<double>(system.radius);
        if (away < 0.5) {
            centre_pressure =
                std::min(centre_pressure, static_cast<double>(fields.pressure[index]));
        } else if (away > 2.0 && away < 2.5) {
            ring_pressure += static_cast<double>(fields.pressure[index]);
            ++ring;
        }
        if (away > 0.5 && away < 1.5) {
            // Anticlockwise around the centre, seen from above it.
            const DVec3 around = glm::normalize(glm::cross(system.centre, frame.up));
            const hog::DVec2 local{glm::dot(around, frame.east), glm::dot(around, frame.north)};
            circulation += glm::dot(hog::DVec2{fields.wind[index]}, local);
        }
    }
    REQUIRE(ring > 0);
    CHECK(centre_pressure < (ring_pressure / ring) - 3.0);
    // Anticlockwise in the north, clockwise in the south.
    CHECK(circulation * system.centre.z > 0.0);

    CHECK(std::ranges::max(fields.front) > 0.5F);
    CHECK(std::ranges::min(fields.front) < -0.5F);
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
    CHECK(a.systems() == b.systems());
    CHECK(a.sea().fields() == b.sea().fields());
}

TEST_CASE("the sea warms western coasts and the air over them", "[sim][weather]") {
    const Weather weather{config(), basin(), march_equinox};
    const auto ground = basin();
    const auto coast = [&](double latitude, double towards) {
        double longitude = 70.0 * towards;
        while (!ground[cell_at(weather, latitude, longitude)].ocean) {
            longitude -= towards;
        }
        return cell_at(weather, latitude, longitude);
    };
    for (const double latitude : {-30.0, 30.0}) {
        const std::uint32_t west = coast(latitude, -1.0);
        const std::uint32_t east = coast(latitude, 1.0);
        CHECK(weather.ocean().temperature[west] > weather.ocean().temperature[east] + 1.0F);
        CHECK(weather.fields().temperature[west] > weather.fields().temperature[east] + 1.0F);
    }
}

TEST_CASE("weather saves and loads", "[sim][weather]") {
    JobSystem jobs{1};
    Weather saved{config(), basin(), march_equinox};
    saved.advance_to(march_equinox + hours{5}, jobs);
    const auto bytes = saved.save();
    const WeatherFields saved_fields = saved.fields();

    Weather loaded{config(), basin(), june_solstice};
    REQUIRE(loaded.load(Weather::save_version, bytes));
    CHECK(loaded.time() == saved.time());
    CHECK(loaded.fields() == saved.fields());
    CHECK(loaded.ocean() == saved.ocean());
    CHECK(loaded.land().fields() == saved.land().fields());
    CHECK(loaded.sea().fields() == saved.sea().fields());
    saved.advance_to(march_equinox + hours{9}, jobs);
    loaded.advance_to(march_equinox + hours{9}, jobs);
    CHECK(loaded.fields() == saved.fields());
    CHECK(loaded.ocean() == saved.ocean());
    CHECK(loaded.land().fields() == saved.land().fields());
    CHECK(loaded.sea().fields() == saved.sea().fields());

    Weather finer{WeatherConfig{.level = level + 1}, std::vector<Ground>(6U << 8U), june_solstice};
    const auto other_level = finer.load(Weather::save_version, bytes);
    REQUIRE_FALSE(other_level);
    CHECK(other_level.error().code == hog::ErrorCode::InvalidArgument);
    const auto newer = loaded.load(Weather::save_version + 1, bytes);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == hog::ErrorCode::Unsupported);

    // Weather saved before air rose and sank, or storm systems formed, loads without them.
    Weather current{config(), everywhere(land), june_solstice};
    REQUIRE(current.load(Weather::save_version, bytes));
    Weather old{config(), everywhere(land), june_solstice};
    // Version 1 held a level, a time and fourteen floats per cell.
    const std::size_t version_one = 1 + 8 + (old.fields().size() * 14 * sizeof(float));
    REQUIRE(old.load(1, std::span{bytes}.first(version_one)));
    CHECK(std::ranges::all_of(old.fields().lift, [](float lift) { return lift == 0.0F; }));
    CHECK(old.systems().empty());
    CHECK(old.fields().temperature == current.fields().temperature);
    // Without saved means, they start from the weather as it stands.
    CHECK(old.fields().recent_temperature == old.fields().temperature);
    CHECK(old.fields().recent_precipitation[0] > 0.0F);
    // Without saved waves, the wind sea is raised under the saved winds, without swell.
    Weather calm{config(), basin(), june_solstice};
    const std::size_t wave_bytes = calm.ocean().size() * ((2 * sizeof(float)) + sizeof(Vec2) +
                                                          (hog::sim::swell_bins * sizeof(float)));
    REQUIRE(calm.load(5, std::span{bytes}.first(bytes.size() - wave_bytes)));
    CHECK(calm.land().fields() == current.land().fields());
    CHECK(std::ranges::any_of(calm.sea().fields().wind_sea,
                              [](float raised) { return raised > 0.0F; }));
    CHECK(
        std::ranges::all_of(calm.sea().fields().swell, [](float swell) { return swell == 0.0F; }));
    // Without saved land, it spins up under the saved time's seasons.
    Weather bare{config(), basin(), june_solstice};
    const std::size_t land_bytes =
        bare.ocean().size() * ((4 * sizeof(float)) + (4 * sizeof(double)));
    REQUIRE(bare.load(4, std::span{bytes}.first(bytes.size() - wave_bytes - land_bytes)));
    CHECK(bare.ocean() == current.ocean());
    CHECK(std::ranges::any_of(bare.land().fields().soil, [](float soil) { return soil > 0.1F; }));
    // Without a saved sea, it settles one under the saved winds.
    Weather dry{config(), basin(), june_solstice};
    const std::size_t sea_bytes =
        dry.ocean().size() * (sizeof(float) + (2 * sizeof(Vec2)) + sizeof(double));
    REQUIRE(
        dry.load(3, std::span{bytes}.first(bytes.size() - wave_bytes - sea_bytes - land_bytes)));
    CHECK(dry.fields() == saved_fields);
    CHECK(std::ranges::any_of(dry.ocean().current, [](Vec2 flow) { return flow != Vec2{0.0F}; }));

    const std::span<const std::byte> all{bytes};
    CHECK_FALSE(loaded.load(Weather::save_version, all.first(all.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(loaded.load(Weather::save_version, longer));
    CHECK(loaded.fields() == saved.fields());
}

TEST_CASE("land starts with the season's snow and rivers, and the weather moves them on",
          "[sim][weather]") {
    JobSystem jobs{1};
    Weather weather{config(), basin(), june_solstice};
    const hog::sim::Land& ground = weather.land();
    double winter = 0.0;
    double summer = 0.0;
    double flowing = 0.0;
    for (std::uint32_t index = 0; index < weather.grid().size(); ++index) {
        const double sin_latitude = weather.grid().frame(index).up.z;
        if (basin()[index].ocean) {
            continue;
        }
        const auto snow = static_cast<double>(ground.fields().snow[index]);
        if (sin_latitude < -std::sin(50.0 * degree)) {
            winter = std::max(winter, snow);
        } else if (sin_latitude > std::sin(35.0 * degree) &&
                   sin_latitude < std::sin(55.0 * degree)) {
            summer = std::max(summer, snow);
        }
        flowing = std::max(flowing, ground.fields().usual_flow[index]);
    }
    CHECK(winter > 50.0);
    CHECK(summer == 0.0);
    CHECK(flowing > 0.0);

    const hog::sim::LandFields before = ground.fields();
    weather.advance_to(june_solstice + hours{6}, jobs);
    CHECK(ground.fields().soil != before.soil);
    CHECK(ground.fields().channel != before.channel);
}

TEST_CASE("recent weather follows the weather slowly", "[sim][weather]") {
    JobSystem jobs{1};
    Weather weather{config(), everywhere(land), march_equinox};
    const auto start = weather.fields();
    weather.advance_to(march_equinox + days{3}, jobs);
    const auto& fields = weather.fields();
    double surface_swing = 0.0;
    double recent_swing = 0.0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        surface_swing = std::max(surface_swing,
                                 static_cast<double>(std::abs(fields.temperature[index] -
                                                              fields.recent_temperature[index])));
        recent_swing =
            std::max(recent_swing, static_cast<double>(std::abs(fields.recent_temperature[index] -
                                                                start.recent_temperature[index])));
        CHECK(fields.recent_precipitation[index] >= 0.0F);
    }
    // Days and nights swing the air, but the mean barely moves.
    CHECK(surface_swing > 3.0);
    CHECK(recent_swing < surface_swing);
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

namespace {

// A year's weather added up per cell.
struct Year {
    explicit Year(std::size_t size) : warmth(size), rain(size), snow(size), snow_warmth(size) {}

    void add(const Weather& weather, unsigned month) {
        const auto& f = weather.fields();
        ++steps;
        for (std::uint32_t index = 0; index < f.size(); ++index) {
            const auto temperature = static_cast<double>(f.temperature[index]);
            warmth[index] += temperature;
            rain[index] += static_cast<double>(f.rain[index] + f.snow[index] + f.hail[index]);
            snow[index] += static_cast<double>(f.snow[index]);
            snow_warmth[index] += static_cast<double>(f.snow[index]) * temperature;
            // Northern land in its summer against its winter.
            if (!weather.ground()[index].ocean && weather.grid().frame(index).up.z > 0.3) {
                const auto storm = static_cast<double>(f.storm[index]);
                summer_storms += month >= 6 && month <= 8 ? storm : 0.0;
                winter_storms += month == 12 || month <= 2 ? storm : 0.0;
            }
        }
    }

    std::vector<double> warmth;
    std::vector<double> rain;
    std::vector<double> snow;
    std::vector<double> snow_warmth;
    double summer_storms = 0.0;
    double winter_storms = 0.0;
    int steps = 0;
};

} // namespace

TEST_CASE("a year of weather matches the planet's climate", "[sim][weather][climate]") {
    const hog::planet::World world{hog::planet::PlanetConfig{.seed = 1, .radius = 115'000.0}};
    const hours step = hog::testing::full_or(hours{2}, hours{6});
    const WeatherConfig coarse{.level = 4, .step = step, .planet_radius = 115'000.0, .seed = 1};
    const hog::sim::CellGrid grid{coarse.level};
    Weather weather{coarse, hog::sim::ground_for(grid, world), at(2026y / January / 1)};
    JobSystem jobs{0};

    const std::size_t size = grid.size();
    Year year{size};
    for (GameTime time = weather.time(); time < at(2027y / January / 1); time += step) {
        weather.advance_to(time + step, jobs);
        const year_month_day date{
            floor<days>(sys_time<hog::GameDuration>{time.time_since_epoch()})};
        year.add(weather, static_cast<unsigned>(date.month()));
    }
    const auto& [warmth, rain, snow, snow_warmth, summer_storms, winter_storms, steps] = year;

    // Yearly means stay near the climate the planet was generated with.
    double drift = 0.0;
    for (std::uint32_t index = 0; index < size; ++index) {
        drift += std::abs((warmth[index] / steps) -
                          static_cast<double>(weather.ground()[index].mean_temperature));
    }
    CHECK(drift / static_cast<double>(size) < 2.0);

    // Wet biomes get more rain than dry ones, and the tropics more than the subtropics.
    const auto mean_rain = [&](auto keep) {
        double total = 0.0;
        int count = 0;
        for (std::uint32_t index = 0; index < size; ++index) {
            if (keep(index)) {
                total += rain[index];
                ++count;
            }
        }
        REQUIRE(count > 0);
        return total / count;
    };
    const auto biome = [&](std::uint32_t index) { return world.state(grid.cell(index)).biome; };
    const auto latitude = [&](std::uint32_t index) {
        return std::abs(std::asin(grid.frame(index).up.z)) / degree;
    };
    using hog::planet::Biome;
    const double wet = mean_rain([&](std::uint32_t index) {
        const Biome b = biome(index);
        return b == Biome::TropicalRainforest || b == Biome::TemperateForest;
    });
    const double dry = mean_rain([&](std::uint32_t index) {
        const Biome b = biome(index);
        return b == Biome::Desert || b == Biome::Shrubland;
    });
    CHECK(wet > dry);
    CHECK(mean_rain([&](std::uint32_t index) { return latitude(index) < 12.0; }) >
          mean_rain([&](std::uint32_t index) {
              return latitude(index) > 20.0 && latitude(index) < 35.0;
          }));

    // Thunderstorms come with summer, snow with cold.
    CHECK(summer_storms > winter_storms * 2.0);
    double snowfall = 0.0;
    double snowy_warmth = 0.0;
    for (std::uint32_t index = 0; index < size; ++index) {
        snowfall += snow[index];
        snowy_warmth += snow_warmth[index];
    }
    REQUIRE(snowfall > 0.0);
    CHECK(snowy_warmth / snowfall < 1.0);
}

namespace {

// One cell of air at 25 °C, which forms cloud from 45 mm.
WeatherFields air(float vapour) {
    WeatherFields fields{1};
    fields.temperature[0] = 25.0F;
    fields.vapour[0] = vapour;
    return fields;
}

hog::sim::Sky sky_of(const WeatherFields& fields, float elevation = 0.0F) {
    return hog::sim::sky_over(fields, Ground{.elevation = elevation, .ocean = elevation <= 0.0F},
                              hog::sim::WeatherTuning{}, 0);
}

} // namespace

TEST_CASE("humidity is the share of the water the air holds when it starts forming cloud",
          "[sim][weather]") {
    CHECK(std::abs(sky_of(air(22.5F)).humidity - 0.5F) < 1e-6F);
    CHECK(sky_of(air(80.0F)).humidity == 1.0F);
    CHECK(sky_of(air(0.0F)).humidity == 0.0F);
}

TEST_CASE("cloud sits as high as the air must rise to cool to its dew point", "[sim][weather]") {
    // Windy, so no fog brings it down.
    const auto windy = [](float vapour, float temperature = 25.0F) {
        WeatherFields fields = air(vapour);
        fields.temperature[0] = temperature;
        fields.wind[0] = {8.0F, 0.0F};
        return fields;
    };
    // 125 m a degree, the air holding 90% of what it could as it starts forming cloud.
    const float damp = sky_of(windy(45.0F)).base;
    CHECK(std::abs(damp - (125.0F * std::log(0.9F) / -0.06F)) < 1.0F);
    CHECK(std::abs(sky_of(windy(30.0F)).base - (125.0F * std::log(0.6F) / -0.06F)) < 1.0F);
    CHECK(sky_of(windy(0.0F)).base == 3'000.0F);
    CHECK(std::abs(sky_of(windy(45.0F), 1'000.0F).base - (1'000.0F + damp)) < 1.0F);
    CHECK(sky_of(windy(45.0F)).top > damp);
    // The same water in cooler air, nearer its dew point, as at night.
    CHECK(sky_of(windy(30.0F, 20.0F)).base < 0.5F * sky_of(windy(30.0F)).base);
}

TEST_CASE("more cloud and rising air build deeper cloud, and storms tower", "[sim][weather]") {
    const auto depth = [](const WeatherFields& fields) {
        const hog::sim::Sky sky = sky_of(fields);
        return sky.top - sky.base;
    };
    WeatherFields fields = air(45.0F);
    const float thin = depth(fields);
    CHECK(std::abs(thin - 300.0F) < 1.0F);
    fields.cloud[0] = 2.0F;
    const float thick = depth(fields);
    CHECK(thick > thin + 2'000.0F);
    fields.lift[0] = 0.2F;
    CHECK(depth(fields) > thick + 2'000.0F);
    fields.storm[0] = 1.0F;
    CHECK(sky_of(fields).top >= 11'000.0F);
}

TEST_CASE("fog lies in calm, damp air and fills the low ground first", "[sim][weather]") {
    WeatherFields calm = air(45.0F);
    const hog::sim::Sky at_sea = sky_of(calm);
    CHECK(at_sea.fog == 1.0F);
    CHECK(std::abs(at_sea.fog_top - 300.0F) < 1.0F);
    CHECK(std::abs(sky_of(calm, 500.0F).fog_top - 800.0F) < 1.0F);

    WeatherFields windy = calm;
    windy.wind[0] = {8.0F, 0.0F};
    CHECK(sky_of(windy).fog == 0.0F);
    CHECK(sky_of(windy).fog_top == 0.0F);
    CHECK(sky_of(air(30.0F)).fog == 0.0F);
    WeatherFields rising = calm;
    rising.lift[0] = 1.0F;
    CHECK(sky_of(rising).fog < 0.01F);

    // Thin fog over high ground lies only in the valleys, below the ground's mean.
    WeatherFields breezy = calm;
    breezy.wind[0] = {4.0F, 0.0F};
    const hog::sim::Sky partly = sky_of(breezy, 1'000.0F);
    CHECK(partly.fog > 0.0F);
    CHECK(partly.fog < 1.0F);
    CHECK(partly.fog_top < 1'000.0F);
}

TEST_CASE("fog is cloud come down to the ground, its base on the fog's top", "[sim][weather]") {
    const hog::sim::Sky fog = sky_of(air(45.0F));
    CHECK(fog.base == fog.fog_top);
    WeatherFields breezy = air(45.0F);
    breezy.wind[0] = {8.0F, 0.0F};
    const float clear = sky_of(breezy).base;
    breezy.wind[0] = {4.0F, 0.0F};
    const hog::sim::Sky lifting = sky_of(breezy, 1'000.0F);
    CHECK(lifting.base < 1'000.0F + clear);
    CHECK(lifting.base > lifting.fog_top);
}
