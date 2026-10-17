#include <hog/client/forecast.hpp>
#include <hog/sim/weather.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>

namespace hog::client {

namespace {

using Cells = std::vector<protocol::CellWeather>;

float mix(float from, float to, float by) noexcept {
    return from + ((to - from) * by);
}

protocol::CellWeather mixed(const protocol::CellWeather& from, const protocol::CellWeather& to,
                            float by) noexcept {
    return {.cloud = mix(from.cloud, to.cloud, by),
            .falling = mix(from.falling, to.falling, by),
            .snow = mix(from.snow, to.snow, by),
            .drift = from.drift + ((to.drift - from.drift) * by)};
}

protocol::CellWeather interpolated(const sim::CellGrid& grid, const Cells& cells, DVec3 direction) {
    const auto field = [&](auto part) {
        return static_cast<float>(
            grid.interpolate(direction, [&](std::uint32_t index) { return part(cells[index]); }));
    };
    return {.cloud = field([](const auto& cell) { return double{cell.cloud}; }),
            .falling = field([](const auto& cell) { return double{cell.falling}; }),
            .snow = field([](const auto& cell) { return double{cell.snow}; }),
            .drift = {field([](const auto& cell) { return double{cell.drift.x}; }),
                      field([](const auto& cell) { return double{cell.drift.y}; })}};
}

std::uint8_t to_byte(double share) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(share, 0.0, 1.0) * 255.0));
}

std::vector<WeatherTexel> map_of(const sim::CellGrid& grid, const Cells& cells) {
    constexpr std::uint32_t size = weather_map_size;
    std::vector<WeatherTexel> texels;
    texels.reserve(std::size_t{6} * size * size);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const protocol::CellWeather cell =
                    interpolated(grid, cells, cube_direction(face, x, y, size));
                texels.push_back({to_byte(double{sim::cloud_cover(cell.cloud)}),
                                  to_byte(std::sqrt(std::max(double{cell.cloud}, 0.0) / 16.0)),
                                  to_byte(std::sqrt(std::max(double{cell.falling}, 0.0) / 256.0)),
                                  to_byte(double{cell.snow})});
            }
        }
    }
    return texels;
}

} // namespace

std::string describe(const protocol::CellWeather& weather) {
    std::string line =
        std::format("{:.0f}% cloud", 100.0 * double{sim::cloud_cover(weather.cloud)});
    if (weather.falling > 0.0F) {
        const char* falls = weather.snow >= 0.5F ? "snow" : "rain";
        line += std::format(", {} {:.1f} mm/h", falls, double{weather.falling});
    }
    return line + std::format(", drifting {:.0f} m/s", double{glm::length(weather.drift)});
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
    before_ = map_of(*grid_, from_);
    after_ = fade_ > MonotonicClock::duration::zero() ? map_of(*grid_, to_) : before_;
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

} // namespace hog::client
