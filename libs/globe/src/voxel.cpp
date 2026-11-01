#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hog::globe {

DVec3 middle_of(Site site, const Layers& layers) {
    return center(column_of(site, layers)) * layers.middle(static_cast<int>(site.layer));
}

std::int16_t distance_in_mm(double metres) noexcept {
    return static_cast<std::int16_t>(
        std::lround(std::clamp(metres * 1'000.0, -double{Voxel::far}, double{Voxel::far})));
}

std::uint16_t amount_of(double share) noexcept {
    return static_cast<std::uint16_t>(
        std::lround(std::clamp(share, 0.0, 1.0) * double{Voxel::full}));
}

std::uint16_t temperature_in_eighths(double kelvin) noexcept {
    return static_cast<std::uint16_t>(std::lround(std::clamp(kelvin * 8.0, 0.0, 65'535.0)));
}

Voxel voxel_from(const Sample& sample, double band) noexcept {
    Voxel voxel;
    voxel.distance = distance_in_mm(std::clamp(sample.distance, -band, band));
    voxel.temperature = temperature_in_eighths(sample.temperature);
    if (sample.distance < 0.0) {
        voxel.solid = sample.material;
    } else if (sample.water > 0.0) {
        voxel.liquid = sample.material;
        voxel.liquid_amount = amount_of(sample.water);
    }
    return voxel;
}

} // namespace hog::globe
