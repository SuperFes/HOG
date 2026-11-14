#include <hog/globe/maths.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::globe {

DVec3 middle_of(Site site, const Layers& layers) {
    return center(column_of(site, layers)) * layers.middle(static_cast<int>(site.layer));
}

std::uint16_t amount_of(double share) noexcept {
    return static_cast<std::uint16_t>(
        std::lround(std::clamp(share, 0.0, 1.0) * double{Voxel::full}));
}

void Seasons::add(std::uint32_t amount, std::uint8_t rise, std::uint8_t peak) noexcept {
    const double turn = 2.0 * std::numbers::pi * peak / 256.0;
    const double weight = static_cast<double>(amount) * rise;
    held_ += amount;
    risen_ += weight;
    x_ += weight * maths::cos(turn);
    y_ += weight * maths::sin(turn);
}

std::uint8_t Seasons::rise() const noexcept {
    return held_ > 0.0 ? static_cast<std::uint8_t>(std::lround(risen_ / held_)) : 0;
}

std::uint8_t Seasons::peak() const noexcept {
    if (x_ == 0.0 && y_ == 0.0) {
        return 0;
    }
    return peak_of(maths::atan2(y_, x_) / (2.0 * std::numbers::pi));
}

std::uint8_t rise_of(double metres) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(metres * 10.0, 0.0, 255.0)));
}

std::uint8_t peak_of(double share) noexcept {
    const double turned = share - std::floor(share);
    return static_cast<std::uint8_t>(std::lround(turned * 256.0) % 256);
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
        voxel.rise = rise_of(sample.rise);
        voxel.peak = peak_of(sample.peak);
    }
    return voxel;
}

} // namespace hog::globe
