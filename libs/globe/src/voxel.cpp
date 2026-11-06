#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hog::globe {

DVec3 middle_of(Site site, const Layers& layers) {
    return center(column_of(site, layers)) * layers.middle(static_cast<int>(site.layer));
}

std::uint16_t amount_of(double share) noexcept {
    return static_cast<std::uint16_t>(
        std::lround(std::clamp(share, 0.0, 1.0) * double{Voxel::full}));
}

std::uint16_t temperature_in_eighths(double kelvin) noexcept {
    return static_cast<std::uint16_t>(std::lround(std::clamp(kelvin * 8.0, 0.0, 65'535.0)));
}

std::uint16_t amount_at(double distance, double thickness) noexcept {
    return amount_of(0.5 - (distance / thickness));
}

double distance_of(std::uint16_t amount, double thickness) noexcept {
    return (0.5 - (static_cast<double>(amount) / Voxel::full)) * thickness;
}

Voxel voxel_from(const Sample& sample, double thickness) noexcept {
    Voxel voxel;
    voxel.temperature = temperature_in_eighths(sample.temperature);
    voxel.solid_amount = amount_at(sample.distance, thickness);
    if (sample.distance < 0.0) {
        voxel.solid = sample.material;
    }
    if (sample.water > 0.0) {
        voxel.liquid = sample.liquid;
        voxel.liquid_amount = amount_of(sample.water);
    }
    return voxel;
}

} // namespace hog::globe
