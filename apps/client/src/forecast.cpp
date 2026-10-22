#include <hog/client/forecast.hpp>
#include <hog/sim/weather.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace hog::client {

namespace {

using Cells = std::vector<protocol::CellWeather>;

template <typename T>
T blend(T from, T to, float by) noexcept {
    return from + ((to - from) * by);
}

// Waves add by variance, so each system counts by its height's square.
protocol::CellWaves mixed(const protocol::CellWaves& from, const protocol::CellWaves& to,
                          float by) noexcept {
    const float before = from.peak > 0.0F ? (1.0F - by) * from.height * from.height : 0.0F;
    const float after = to.peak > 0.0F ? by * to.height * to.height : 0.0F;
    const float total = before + after;
    if (total <= 0.0F) {
        return {};
    }
    const float share = after / total;
    const Vec2 heading = blend(from.heading, to.heading, share);
    // Running opposite ways, as evenly, they run the stronger's way.
    const Vec2 stronger = share >= 0.5F ? to.heading : from.heading;
    return {.height = std::sqrt(total),
            .peak = blend(from.peak, to.peak, share),
            .heading = glm::length(heading) > 1e-6F ? glm::normalize(heading) : stronger};
}

protocol::CellWeather mixed(const protocol::CellWeather& from, const protocol::CellWeather& to,
                            float by) noexcept {
    return {.cloud = blend(from.cloud, to.cloud, by),
            .falling = blend(from.falling, to.falling, by),
            .snow = blend(from.snow, to.snow, by),
            .drift = blend(from.drift, to.drift, by),
            .wind = blend(from.wind, to.wind, by),
            .humidity = blend(from.humidity, to.humidity, by),
            .fog_top = blend(from.fog_top, to.fog_top, by),
            .base = blend(from.base, to.base, by),
            .top = blend(from.top, to.top, by),
            .wind_sea = mixed(from.wind_sea, to.wind_sea, by),
            .swells = {mixed(from.swells[0], to.swells[0], by),
                       mixed(from.swells[1], to.swells[1], by)},
            .current = blend(from.current, to.current, by)};
}

protocol::CellWeather interpolated(const sim::CellGrid& grid, const Cells& cells, DVec3 direction) {
    const auto field = [&](auto part) {
        return static_cast<float>(
            grid.interpolate(direction, [&](std::uint32_t index) { return part(cells[index]); }));
    };
    return {.cloud = field([](const auto& cell) { return static_cast<double>(cell.cloud); }),
            .falling = field([](const auto& cell) { return static_cast<double>(cell.falling); }),
            .snow = field([](const auto& cell) { return static_cast<double>(cell.snow); }),
            .drift = {field([](const auto& cell) { return static_cast<double>(cell.drift.x); }),
                      field([](const auto& cell) { return static_cast<double>(cell.drift.y); })},
            .wind = {field([](const auto& cell) { return static_cast<double>(cell.wind.x); }),
                     field([](const auto& cell) { return static_cast<double>(cell.wind.y); })},
            .humidity = field([](const auto& cell) { return static_cast<double>(cell.humidity); }),
            .fog_top = field([](const auto& cell) { return static_cast<double>(cell.fog_top); }),
            .base = field([](const auto& cell) { return static_cast<double>(cell.base); }),
            .top = field([](const auto& cell) { return static_cast<double>(cell.top); })};
}

std::uint8_t to_byte(double share) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(share, 0.0, 1.0) * 255.0));
}

struct Maps {
    std::vector<WeatherTexel> weather;
    std::vector<SkyTexel> sky;
};

Maps maps_of(const sim::CellGrid& grid, const Cells& cells) {
    constexpr std::uint32_t size = weather_map_size;
    Maps maps;
    maps.weather.reserve(std::size_t{6} * size * size);
    maps.sky.reserve(std::size_t{6} * size * size);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const protocol::CellWeather cell =
                    interpolated(grid, cells, cube_direction(face, x, y, size));
                maps.weather.push_back(
                    {to_byte(static_cast<double>(sim::cloud_cover(cell.cloud))),
                     to_byte(std::sqrt(std::max(static_cast<double>(cell.cloud), 0.0) / 16.0)),
                     to_byte(std::sqrt(std::max(static_cast<double>(cell.falling), 0.0) / 256.0)),
                     to_byte(static_cast<double>(cell.snow))});
                maps.sky.push_back(
                    {to_byte(static_cast<double>(cell.humidity)),
                     to_byte(static_cast<double>(cell.fog_top) / (255.0 * fog_texel_step)),
                     to_byte(static_cast<double>(cell.base) / (255.0 * height_texel_step)),
                     to_byte(static_cast<double>(cell.top) / (255.0 * height_texel_step))});
            }
        }
    }
    return maps;
}

} // namespace

std::string describe(const protocol::CellWeather& weather) {
    const double cover = 100.0 * static_cast<double>(sim::cloud_cover(weather.cloud));
    std::string line = std::format("{:.0f}% cloud", cover);
    if (cover >= 0.5) {
        line += std::format(" at {:.0f}-{:.0f} m", static_cast<double>(weather.base),
                            static_cast<double>(weather.top));
    }
    if (weather.falling > 0.0F) {
        const char* falls = weather.snow >= 0.5F ? "snow" : "rain";
        line += std::format(", {} {:.1f} mm/h", falls, static_cast<double>(weather.falling));
    }
    line += std::format(", {:.0f}% humid", 100.0 * static_cast<double>(weather.humidity));
    if (weather.fog_top > 0.0F) {
        line += std::format(", fog to {:.0f} m", static_cast<double>(weather.fog_top));
    }
    return line +
           std::format(", drifting {:.0f} m/s", static_cast<double>(glm::length(weather.drift)));
}

DVec3 cube_direction(std::uint32_t face, std::uint32_t x, std::uint32_t y,
                     std::uint32_t size) noexcept {
    const double s = ((2.0 * (x + 0.5)) / size) - 1.0;
    const double t = ((2.0 * (y + 0.5)) / size) - 1.0;
    // Vulkan's table of which axis each face looks along, and which way s and t run on it.
    switch (face) {
    case 0:
        return glm::normalize(DVec3{1.0, -t, -s});
    case 1:
        return glm::normalize(DVec3{-1.0, -t, s});
    case 2:
        return glm::normalize(DVec3{s, 1.0, t});
    case 3:
        return glm::normalize(DVec3{s, -1.0, -t});
    case 4:
        return glm::normalize(DVec3{s, -t, 1.0});
    default:
        return glm::normalize(DVec3{-s, -t, -1.0});
    }
}

void Forecast::receive(const protocol::Weather& weather, MonotonicClock::time_point now) {
    ++received_;
    if (!grid_ || grid_->level() != weather.level) {
        grid_.emplace(weather.level);
        to_ = weather.cells;
        from_ = to_;
        fade_ = {};
    } else {
        const auto by = static_cast<float>(faded(now));
        for (std::size_t index = 0; index < from_.size(); ++index) {
            from_[index] = mixed(from_[index], to_[index], by);
        }
        to_ = weather.cells;
        fade_ = weather.fade;
        if (fade_ <= MonotonicClock::duration::zero()) {
            from_ = to_;
        }
    }
    since_ = now;
    Maps before = maps_of(*grid_, from_);
    Maps after = fade_ > MonotonicClock::duration::zero() ? maps_of(*grid_, to_) : before;
    before_ = std::move(before.weather);
    sky_before_ = std::move(before.sky);
    after_ = std::move(after.weather);
    sky_after_ = std::move(after.sky);
}

double Forecast::faded(MonotonicClock::time_point now) const noexcept {
    if (fade_ <= MonotonicClock::duration::zero()) {
        return 1.0;
    }
    return std::clamp(std::chrono::duration<double>(now - since_) /
                          std::chrono::duration<double>(fade_),
                      0.0, 1.0);
}

protocol::CellWeather Forecast::at(DVec3 direction, MonotonicClock::time_point now) const {
    if (!grid_) {
        return {};
    }
    return mixed(interpolated(*grid_, from_, direction), interpolated(*grid_, to_, direction),
                 static_cast<float>(faded(now)));
}

waves::SeaState Forecast::sea_at(DVec3 direction, MonotonicClock::time_point now) const {
    waves::SeaState sea;
    if (!grid_) {
        return sea;
    }
    const sim::CellGrid::Shares around = grid_->shares(direction);
    const double by = faded(now);
    const auto add = [&](const Cells& cells, double counts) {
        for (std::size_t k = 0; k < around.cells.size(); ++k) {
            const double weight = counts * around.weights.at(k) / around.total;
            if (weight <= 0.0) {
                continue;
            }
            const std::uint32_t index = around.cells.at(k);
            const planet::TangentFrame& frame = grid_->frame(index);
            const auto along = [&](Vec2 vector) {
                return (frame.east * static_cast<double>(vector.x)) +
                       (frame.north * static_cast<double>(vector.y));
            };
            const protocol::CellWeather& cell = cells[index];
            sea.current += along(cell.current) * weight;
            sea.wind += along(cell.wind) * weight;
            // Each system's variance counts as much as its cell does.
            const auto system = [&](const protocol::CellWaves& waves, double peakedness,
                                    double spread) {
                if (waves.height > 0.0F && waves.peak > 0.0F) {
                    sea.systems.push_back(
                        {.heading = glm::normalize(along(waves.heading)),
                         .height = static_cast<double>(waves.height) * std::sqrt(weight),
                         .peak = static_cast<double>(waves.peak),
                         .peakedness = peakedness,
                         .spread = spread});
                }
            };
            system(cell.wind_sea, waves::wind_sea_peakedness, 2.0);
            for (const protocol::CellWaves& swell : cell.swells) {
                system(swell, waves::swell_peakedness, waves::swell_spread);
            }
        }
    };
    add(from_, 1.0 - by);
    add(to_, by);
    return sea;
}

} // namespace hog::client
