#include <hog/globe/cell.hpp>
#include <hog/server/ground_wetting.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace hog::server {

flow::Sky GroundWetting::sky_of(const protocol::CellWeather& weather) {
    const auto falling = static_cast<double>(weather.falling);
    const bool rain = weather.snow < rainy;
    return {.rain = rain ? falling : 0.0,
            .dryness = falling > 0.0 ? 0.0 : 1.0 - static_cast<double>(weather.humidity)};
}

std::vector<flow::Simulation::Wetted> GroundWetting::around(const globe::World& world,
                                                            std::span<const Place> places) {
    const globe::Layers& layers = world.layers();
    constexpr int side = (2 * reach) + 1;
    std::vector<flow::Simulation::Wetted> wetted;
    std::unordered_set<std::uint64_t> taken;
    for (const Place& place : places) {
        const globe::Cell under = globe::cell_under(place.up, layers.level);
        const auto across = std::uint32_t{1} << layers.level;
        if (across < side) {
            continue;
        }
        const auto first = [&](std::uint32_t at) {
            return std::clamp(at, std::uint32_t{reach}, across - reach - 1) - reach;
        };
        const globe::Cell corner{
            .level = under.level, .face = under.face, .i = first(under.i), .j = first(under.j)};
        for (int k = 0; k < side * side; ++k) {
            const std::uint32_t i = corner.i + static_cast<std::uint32_t>(k % side);
            const std::uint32_t j = corner.j + static_cast<std::uint32_t>(k / side);
            const std::uint64_t key =
                (std::uint64_t{corner.face} << 56U) | (std::uint64_t{i} << 28U) | j;
            if (!taken.insert(key).second) {
                continue;
            }
            const globe::Cell cell{.level = corner.level, .face = corner.face, .i = i, .j = j};
            const std::optional<int> layer = globe::highest_layer(world, cell);
            if (!layer) {
                continue;
            }
            wetted.push_back({.column = {.face = corner.face,
                                         .i = i,
                                         .j = j,
                                         .layer = static_cast<std::uint32_t>(*layer)},
                              .sky = place.sky});
        }
    }
    return wetted;
}

} // namespace hog::server
