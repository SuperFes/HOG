#include <hog/server/weather_report.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::server {

namespace {

// Area-weighted sums over the cells under one that's sent.
struct Sums {
    double area = 0.0;
    double cloud = 0.0;
    double falling = 0.0;
    double snowing = 0.0;
    DVec2 drift{0.0};
    double humidity = 0.0;
    double fog_top = 0.0;
    double base = 0.0;
    double top = 0.0;
};

} // namespace

protocol::Weather weather_report(const sim::CellGrid& grid, const sim::WeatherFields& fields,
                                 std::span<const sim::Ground> ground,
                                 const sim::WeatherConfig& config, std::chrono::milliseconds fade) {
    const auto level = static_cast<std::uint8_t>(
        std::min(grid.level(), static_cast<int>(protocol::most_weather_level)));
    const std::optional<sim::CellGrid> coarse =
        level < grid.level() ? std::optional{sim::CellGrid{level}} : std::nullopt;
    const double hours =
        std::chrono::duration<double, std::chrono::hours::period>(config.step).count();
    const double steering = config.tuning.motion.jet_steering;

    std::vector<Sums> sums(protocol::weather_cells(level));
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const std::uint32_t into = coarse ? coarse->index(grid.cell(index).parent(level)) : index;
        Sums& sum = sums[into];
        const double area = grid.area(index);
        const DVec2 wind{fields.wind[index]};
        const DVec2 jet{fields.jet[index]};
        sum.area += area;
        sum.cloud += area * double{fields.cloud[index]};
        sum.falling +=
            area * double{fields.rain[index] + fields.snow[index] + fields.hail[index]} / hours;
        sum.snowing += area * double{fields.snow[index]} / hours;
        sum.drift += area * (wind + ((jet - wind) * steering));
        const sim::Sky sky = sim::sky_over(fields, ground[index], config.tuning, index);
        sum.humidity += area * double{sky.humidity};
        sum.fog_top += area * double{sky.fog_top};
        sum.base += area * double{sky.base};
        sum.top += area * double{sky.top};
    }

    protocol::Weather weather{.level = level, .fade = fade, .cells = {}};
    weather.cells.reserve(sums.size());
    for (const Sums& sum : sums) {
        weather.cells.push_back(
            {.cloud = static_cast<float>(sum.cloud / sum.area),
             .falling = static_cast<float>(sum.falling / sum.area),
             .snow = sum.falling > 0.0 ? static_cast<float>(sum.snowing / sum.falling) : 0.0F,
             .drift = Vec2{sum.drift / sum.area},
             .humidity = static_cast<float>(sum.humidity / sum.area),
             .fog_top = static_cast<float>(sum.fog_top / sum.area),
             .base = static_cast<float>(sum.base / sum.area),
             .top = static_cast<float>(sum.top / sum.area)});
    }
    return weather;
}

} // namespace hog::server
