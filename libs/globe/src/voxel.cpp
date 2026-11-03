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

namespace {

// Steps across the octahedron's unfolded square, either way from its middle.
constexpr double normal_steps = 127.0;

std::uint16_t normal_byte(double at) noexcept {
    return static_cast<std::uint16_t>(
        std::lround((std::clamp(at, -1.0, 1.0) * normal_steps) + 128.0));
}

} // namespace

std::uint16_t packed_normal(DVec3 unit) noexcept {
    const double across = std::abs(unit.x) + std::abs(unit.y) + std::abs(unit.z);
    if (!(across > 0.0)) {
        return Voxel::no_normal;
    }
    double u = unit.x / across;
    double v = unit.y / across;
    // The lower half folds out over the corners.
    if (unit.z < 0.0) {
        const double fold_u = (1.0 - std::abs(v)) * std::copysign(1.0, u);
        v = (1.0 - std::abs(u)) * std::copysign(1.0, v);
        u = fold_u;
    }
    // Each byte from 1 to 255, so none is never made.
    return static_cast<std::uint16_t>((normal_byte(u) << 8U) | normal_byte(v));
}

DVec3 unpacked_normal(std::uint16_t packed) noexcept {
    if (packed == Voxel::no_normal) {
        return DVec3{0.0};
    }
    const double u = (static_cast<double>(packed >> 8U) - 128.0) / normal_steps;
    const double v = (static_cast<double>(packed & 0xFFU) - 128.0) / normal_steps;
    DVec3 unit{u, v, 1.0 - std::abs(u) - std::abs(v)};
    if (unit.z < 0.0) {
        unit.x = (1.0 - std::abs(v)) * std::copysign(1.0, u);
        unit.y = (1.0 - std::abs(u)) * std::copysign(1.0, v);
    }
    return glm::normalize(unit);
}

Voxel voxel_from(const Sample& sample, double band) noexcept {
    Voxel voxel;
    voxel.distance = distance_in_mm(std::clamp(sample.distance, -band, band));
    if (std::abs(sample.distance) < band) {
        voxel.normal = packed_normal(sample.normal);
    }
    voxel.temperature = temperature_in_eighths(sample.temperature);
    // The share of the voxel inside the surface: half where the surface passes through its middle,
    // as a plane across it, a voxel's thickness for the distance it takes to go from all to none.
    voxel.solid_amount = amount_of(0.5 - (sample.distance / (band / band_voxels)));
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
