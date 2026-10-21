#include <hog/server/weather_report.hpp>

#include <algorithm>
#include <cmath>
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
    DVec2 wind{0.0};
    double humidity = 0.0;
    double fog_top = 0.0;
    double base = 0.0;
    double top = 0.0;
    double wind_sea = 0.0;
    double peak = 0.0; // by the wind sea's variance
    DVec2 heading{0.0};
    DVec2 current{0.0};
};

protocol::CellWaves sent(const sim::WaveSystem& system) {
    return {.height = system.height, .peak = system.peak, .heading = system.heading};
}

} // namespace

protocol::Weather weather_report(const sim::CellGrid& grid, const sim::WeatherFields& fields,
                                 std::span<const sim::Ground> ground, const sim::SeaFields& sea,
                                 std::span<const Vec2> current, const sim::WeatherConfig& config,
                                 std::chrono::milliseconds fade) {
    const auto level = static_cast<std::uint8_t>(
        std::min(grid.level(), static_cast<int>(protocol::most_weather_level)));
    const std::optional<sim::CellGrid> coarse =
        level < grid.level() ? std::optional{sim::CellGrid{level}} : std::nullopt;
    const double hours =
        std::chrono::duration<double, std::chrono::hours::period>(config.step).count();
    const double steering = config.tuning.motion.jet_steering;

    std::vector<Sums> sums(protocol::weather_cells(level));
    // Each cell sent's swell, band by band and heading by heading, by area.
    std::vector<float> swell(sums.size() * sim::swell_bins);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const std::uint32_t into = coarse ? coarse->index(grid.cell(index).parent(level)) : index;
        Sums& sum = sums[into];
        const double area = grid.area(index);
        const DVec2 wind{fields.wind[index]};
        const DVec2 jet{fields.jet[index]};
        sum.area += area;
        sum.cloud += area * static_cast<double>(fields.cloud[index]);
        sum.falling +=
            area *
            static_cast<double>(fields.rain[index] + fields.snow[index] + fields.hail[index]) /
            hours;
        sum.snowing += area * static_cast<double>(fields.snow[index]) / hours;
        sum.drift += area * (wind + ((jet - wind) * steering));
        sum.wind += area * wind;
        const sim::Sky sky = sim::sky_over(fields, ground[index], config.tuning, index);
        sum.humidity += area * static_cast<double>(sky.humidity);
        sum.fog_top += area * static_cast<double>(sky.fog_top);
        sum.base += area * static_cast<double>(sky.base);
        sum.top += area * static_cast<double>(sky.top);
        const double raised = area * static_cast<double>(sea.wind_sea[index]);
        sum.wind_sea += raised;
        sum.peak += raised * static_cast<double>(sea.peak[index]);
        sum.heading += raised * DVec2{sea.heading[index]};
        sum.current += area * DVec2{current[index]};
        const std::span<const float> bins = sea.swell_at(index);
        for (std::size_t bin = 0; bin < bins.size(); ++bin) {
            swell[(into * sim::swell_bins) + bin] += static_cast<float>(area) * bins[bin];
        }
    }

    protocol::Weather weather{.level = level, .fade = fade, .cells = {}};
    weather.cells.reserve(sums.size());
    for (std::size_t into = 0; into < sums.size(); ++into) {
        const Sums& sum = sums[into];
        const std::span<float> bins =
            std::span{swell}.subspan(into * sim::swell_bins, sim::swell_bins);
        for (float& bin : bins) {
            bin = static_cast<float>(static_cast<double>(bin) / sum.area);
        }
        const auto swells = sim::strongest_swells(bins);
        protocol::CellWaves wind_sea;
        if (sum.wind_sea > 0.0) {
            wind_sea = {.height = static_cast<float>(4.0 * std::sqrt(sum.wind_sea / sum.area)),
                        .peak = static_cast<float>(sum.peak / sum.wind_sea),
                        .heading = Vec2{glm::normalize(sum.heading)}};
        }
        weather.cells.push_back(
            {.cloud = static_cast<float>(sum.cloud / sum.area),
             .falling = static_cast<float>(sum.falling / sum.area),
             .snow = sum.falling > 0.0 ? static_cast<float>(sum.snowing / sum.falling) : 0.0F,
             .drift = Vec2{sum.drift / sum.area},
             .wind = Vec2{sum.wind / sum.area},
             .humidity = static_cast<float>(sum.humidity / sum.area),
             .fog_top = static_cast<float>(sum.fog_top / sum.area),
             .base = static_cast<float>(sum.base / sum.area),
             .top = static_cast<float>(sum.top / sum.area),
             .wind_sea = wind_sea,
             .swells = {sent(swells[0]), sent(swells[1])},
             .current = Vec2{sum.current / sum.area}});
    }
    return weather;
}

} // namespace hog::server
