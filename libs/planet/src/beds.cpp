#include <hog/core/hash.hpp>
#include <hog/planet/beds.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace hog::planet {

namespace {

constexpr std::uint64_t beds_salt = 0x6265'6473'6265'6473ULL; // "bedsbeds"
constexpr int octaves = 3;

// Each slot of layering holds one bed's base, somewhere in its middle half, so beds are from half
// a slot to one and a half thick.
constexpr std::int64_t slot = 80 * fixed_one;
// Bed rock by its hash shifted by the region's leaning: sandstone below the first share, shale
// below the second, limestone above.
constexpr std::int64_t sandstone_below = fixed_one * 35 / 100;
constexpr std::int64_t shale_below = fixed_one * 70 / 100;
constexpr std::int64_t facies_lean = fixed_one * 3 / 10;
constexpr double facies_wavelength = 60'000.0;
// A gentle warp everywhere, and folds in the belts.
constexpr std::int64_t warp = 150 * fixed_one;
constexpr double warp_wavelength = 40'000.0;
constexpr std::int64_t fold = 800 * fixed_one;
constexpr double fold_wavelength = 8'000.0;
// The basement's top: deep under the plains, raised in the belts.
constexpr std::int64_t basement_depth = -1'500 * fixed_one;
constexpr std::int64_t basement_lift = 3'000 * fixed_one;
constexpr std::int64_t basement_swell = 300 * fixed_one;
constexpr double basement_wavelength = 25'000.0;

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

[[nodiscard]] std::int64_t floor_divide(std::int64_t value, std::int64_t divisor) {
    const std::int64_t quotient = value / divisor;
    return (value % divisor < 0) ? quotient - 1 : quotient;
}

} // namespace

std::string_view to_string(Rock rock) noexcept {
    switch (rock) {
    case Rock::Soil:
        return "soil";
    case Rock::Sand:
        return "sand";
    case Rock::Sandstone:
        return "sandstone";
    case Rock::Shale:
        return "shale";
    case Rock::Limestone:
        return "limestone";
    case Rock::Granite:
        return "granite";
    case Rock::Basalt:
        return "basalt";
    case Rock::Peridotite:
        return "peridotite";
    case Rock::Magma:
        return "magma";
    }
    return "unknown";
}

Beds::Beds(std::uint64_t seed, double planet_radius)
    : seed_{hash_combine(seed, beds_salt)}, planet_radius_{planet_radius} {}

BedColumn Beds::column(CellId cell, BedSetting setting) const {
    if (!setting.continental) {
        return {};
    }
    const DVec3 center = cell_center(cell);
    const auto noise = [&](std::uint64_t salt, double wavelength) {
        return fbm(hash_combine(seed_, salt), noise_point(center, planet_radius_ / wavelength),
                   octaves);
    };
    const std::int64_t shift =
        multiply(warp, noise(1, warp_wavelength)) +
        multiply(multiply(fold, setting.mountains), noise(2, fold_wavelength));
    return {.shift = static_cast<std::int32_t>(shift),
            .basement = static_cast<std::int32_t>(basement(cell, setting)),
            .leaning = static_cast<std::int32_t>(noise(4, facies_wavelength)),
            .continental = true};
}

std::int64_t Beds::basement(CellId cell, BedSetting setting) const {
    const std::int64_t swell =
        fbm(hash_combine(seed_, 3),
            noise_point(cell_center(cell), planet_radius_ / basement_wavelength), octaves);
    return basement_depth + multiply(basement_lift, setting.mountains) +
           multiply(basement_swell, swell);
}

Rock Beds::rock_at(const BedColumn& column, std::int64_t height) const {
    if (!column.continental) {
        return Rock::Basalt;
    }
    if (height < column.basement) {
        return Rock::Granite;
    }
    return rock_of(index_at(height - column.shift), column);
}

std::optional<Bed> Beds::bed_at(const BedColumn& column, std::int64_t height) const {
    if (!column.continental || height < column.basement) {
        return std::nullopt;
    }
    const std::int64_t index = index_at(height - column.shift);
    return Bed{.base = base_of(index) + column.shift,
               .top = base_of(index + 1) + column.shift,
               .rock = rock_of(index, column),
               .index = index};
}

std::int64_t Beds::base_of(std::int64_t index) const {
    return (index * slot) + (slot / 4) + multiply(slot / 2, lattice_value(seed_, index, 0, 0));
}

std::int64_t Beds::index_at(std::int64_t layer) const {
    const std::int64_t index = floor_divide(layer, slot);
    return layer < base_of(index) ? index - 1 : index;
}

Rock Beds::rock_of(std::int64_t index, const BedColumn& column) const {
    const std::int64_t pick =
        lattice_value(hash_combine(seed_, 5), index, 0, 0) + multiply(facies_lean, column.leaning);
    if (pick < sandstone_below) {
        return Rock::Sandstone;
    }
    return pick < shale_below ? Rock::Shale : Rock::Limestone;
}

std::int64_t ledge(const Bed& bed, std::int64_t height, std::int64_t strength) {
    const std::int64_t thickness = bed.top - bed.base;
    const std::int64_t along = std::clamp<std::int64_t>(
        ((height - bed.base) << fixed_fraction_bits) / thickness, 0, fixed_one);
    constexpr int bits = detail::weight_bits;
    const std::int64_t profile =
        bed.base + (((thickness * detail::fade<fixed_fraction_bits>(along)) +
                     (std::int64_t{1} << (bits - 1))) >>
                    bits);
    return height + multiply(profile - height, strength);
}

} // namespace hog::planet
