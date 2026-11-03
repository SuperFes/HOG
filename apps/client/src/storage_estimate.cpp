#include <hog/client/storage_estimate.hpp>

#include <array>
#include <format>
#include <string_view>

namespace hog::client {

StorageEstimate estimate_storage(double columns, const StorageCosts& costs) {
    return {.bricks = columns * costs.bricks,
            .narrow_band = columns * costs.narrow_band,
            .surface = columns * costs.surface};
}

std::string readable_bytes(double bytes) {
    constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};
    std::size_t unit = 0;
    while (bytes >= 1'000.0 && unit + 1 < units.size()) {
        bytes /= 1'000.0;
        ++unit;
    }
    return std::format("{:.{}f} {}", bytes, bytes >= 100.0 || unit == 0 ? 0 : 1, units.at(unit));
}

std::string describe_storage(double walked_columns, double planet_columns, double cell_metres,
                             double walked_square_metres) {
    const StorageEstimate walked = estimate_storage(walked_columns);
    const StorageEstimate planet = estimate_storage(planet_columns);
    return std::format(
        "storing the world, estimated for cells {:.3g} m wide from measurements at 0.5 m:\n"
        "  ground drawn at full detail: {:.3f} km2, {:.0f} columns\n"
        "    bricks as today {}, narrow-band 2 cm {}, surface only {}\n"
        "  the whole planet: {:.3g} columns\n"
        "    bricks as today {}, narrow-band 2 cm {}, surface only {}\n",
        cell_metres, walked_square_metres / 1e6, walked_columns, readable_bytes(walked.bricks),
        readable_bytes(walked.narrow_band), readable_bytes(walked.surface), planet_columns,
        readable_bytes(planet.bricks), readable_bytes(planet.narrow_band),
        readable_bytes(planet.surface));
}

} // namespace hog::client
