#include <hog/render/device.hpp>
#include <hog/render/weather_maps.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace hog::render {

namespace {

constexpr VkFormat map_format = VK_FORMAT_R8G8B8A8_UNORM;
// A weather texel's channel holding the root of a 256th of mm falling an hour.
constexpr std::size_t falling_channel = 2;
// A sky texel's channels holding the fog's top, in steps of `fog_step` metres, and the cloud's
// base and top, in steps of `height_step`.
constexpr std::size_t fog_channel = 1;
constexpr std::size_t base_channel = 2;
constexpr std::size_t top_channel = 3;
constexpr double fog_step = 32.0;
constexpr double height_step = 64.0;
// As weather.slang's.
constexpr double haze_per_metre = 3e-5;
constexpr double haze_height = 1000.0;
constexpr double driest_share = 0.97;
constexpr double fog_per_metre = 0.015;
constexpr double fog_edge = 60.0;
constexpr double inversion = 150.0;
constexpr double inversion_fog = 100.0;

double haze_extinction(double humidity, double fog_top, double height) noexcept {
    const double damp = std::clamp(humidity, 0.0, 1.0);
    const double above = std::max(height, 0.0);
    const double inverted = std::clamp(fog_top / inversion_fog, 0.0, 1.0);
    const double over_fog =
        1.0 + ((std::exp(-std::max(above - fog_top, 0.0) / inversion) - 1.0) * inverted);
    return haze_per_metre * ((1.0 / (1.0 - (driest_share * damp))) - 1.0) *
           std::exp(-above / haze_height) * over_fog;
}

} // namespace

double mist_extinction(double humidity, double fog_top, double height) noexcept {
    const double fog =
        fog_per_metre * std::clamp((fog_top - std::max(height, 0.0)) / fog_edge, 0.0, 1.0);
    return haze_extinction(humidity, fog_top, height) + fog;
}

double haze_above(double humidity, double fog_top, double height) noexcept {
    return haze_extinction(humidity, fog_top, height) * haze_height;
}

double mist_above(double humidity, double fog_top, double height) noexcept {
    return haze_above(humidity, fog_top, height) +
           (fog_per_metre * std::max(fog_top - std::max(height, 0.0), 0.0));
}

Result<WeatherMaps> WeatherMaps::create(Device& device, std::uint32_t map_size,
                                        std::uint32_t frames) {
    WeatherMaps maps;
    auto linear = make_sampler(device, VK_FILTER_LINEAR);
    if (!linear) {
        return std::unexpected(std::move(linear).error());
    }
    maps.linear_ = std::move(linear).value();
    const VkExtent2D extent{.width = map_size, .height = map_size};
    for (StreamedImage* map :
         {&maps.weather_before_, &maps.weather_after_, &maps.sky_before_, &maps.sky_after_}) {
        auto made = make_streamed_image(device, map_format, extent, frames, 6, true);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        *map = std::move(made).value();
    }
    // No cloud, nothing falling, dry air and no fog.
    constexpr std::array<std::byte, std::size_t{6} * 4> clear{};
    auto filled = make_filled_image(device, {.format = map_format, .layers = 6, .cube = true},
                                    std::span{clear});
    if (!filled) {
        return std::unexpected(std::move(filled).error());
    }
    maps.clear_ = std::move(filled).value();
    maps.map_size_ = map_size;
    return maps;
}

std::size_t WeatherMaps::map_bytes() const noexcept {
    return std::size_t{6} * 4 * map_size_ * map_size_;
}

Result<> WeatherMaps::update(VkCommandBuffer commands, std::uint32_t frame,
                             std::span<const std::byte> weather_before,
                             std::span<const std::byte> weather_after,
                             std::span<const std::byte> sky_before,
                             std::span<const std::byte> sky_after) {
    const std::array updates{
        std::pair{&weather_before_, weather_before}, std::pair{&weather_after_, weather_after},
        std::pair{&sky_before_, sky_before}, std::pair{&sky_after_, sky_after}};
    for (const auto& [map, texels] : updates) {
        if (auto updated = map->update(commands, frame, texels); !updated) {
            return updated;
        }
    }
    const auto falls = [](std::span<const std::byte> map) {
        for (std::size_t at = falling_channel; at < map.size(); at += 4) {
            if (map[at] != std::byte{0}) {
                return true;
            }
        }
        return false;
    };
    falling_ = falls(weather_before) || falls(weather_after);
    // Of a channel of both sky maps, the least and most.
    const auto bounds = [&](std::size_t channel) {
        std::uint8_t least = 255;
        std::uint8_t most = 0;
        for (const std::span<const std::byte> map : {sky_before, sky_after}) {
            for (std::size_t at = channel; at < map.size(); at += 4) {
                least = std::min(least, std::to_integer<std::uint8_t>(map[at]));
                most = std::max(most, std::to_integer<std::uint8_t>(map[at]));
            }
        }
        return std::pair{static_cast<double>(least), static_cast<double>(most)};
    };
    fog_top_ = bounds(fog_channel).second * fog_step;
    const auto [lowest, highest] = bounds(base_channel);
    lowest_base_ = lowest * height_step;
    highest_base_ = highest * height_step;
    highest_top_ = bounds(top_channel).second * height_step;
    return {};
}

bool WeatherMaps::ready() const noexcept {
    return weather_before_.ready() && weather_after_.ready() && sky_before_.ready() &&
           sky_after_.ready();
}

WeatherBindings WeatherMaps::bindings(double faded, DVec3 drifted) const noexcept {
    if (!ready()) {
        return {.weather_before = clear_.view(),
                .weather_after = clear_.view(),
                .sky_before = clear_.view(),
                .sky_after = clear_.view(),
                .sampler = linear_.handle(),
                .faded = faded,
                .falling = false,
                .fog_top = 0.0,
                .lowest_base = 0.0,
                .highest_base = 0.0,
                .highest_top = 0.0,
                .drifted = drifted};
    }
    return {.weather_before = weather_before_.image().view(),
            .weather_after = weather_after_.image().view(),
            .sky_before = sky_before_.image().view(),
            .sky_after = sky_after_.image().view(),
            .sampler = linear_.handle(),
            .faded = faded,
            .falling = falling_,
            .fog_top = fog_top_,
            .lowest_base = lowest_base_,
            .highest_base = highest_base_,
            .highest_top = highest_top_,
            .drifted = drifted};
}

} // namespace hog::render
