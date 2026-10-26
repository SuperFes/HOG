#include <hog/core/hash.hpp>
#include <hog/planet/beds.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace hog::planet {

namespace {

constexpr std::uint64_t beds_salt = 0x6265'6473'6265'6473ULL; // "bedsbeds"
constexpr int octaves = 3;

// Each slot of layering holds one bed's base, placed by the bed and swayed by local noise, so a
// bed is up to two slots thick and here and there pinches out.
constexpr std::int64_t slot = 80 * fixed_one;
constexpr std::int64_t base_scatter = fixed_one * 3 / 10;
constexpr std::int64_t base_sway = fixed_one * 2;
constexpr double lateral_wavelength = 6'000.0;
// How far offshore a bed was laid: a long swing of the sea and a shorter ripple through the
// beds, a little chance, the region's leaning and local sway. Sandstone below the first
// threshold, shale below the second, limestone above.
constexpr std::int64_t sea_beds = 6;
constexpr std::int64_t ripple_beds = 2;
constexpr std::int64_t ripple = fixed_one * 3 / 10;
constexpr std::int64_t facies_chance = fixed_one * 15 / 100;
constexpr std::int64_t facies_lean = fixed_one * 6 / 10;
constexpr std::int64_t facies_sway = fixed_one * 4 / 10;
constexpr double facies_wavelength = 60'000.0;
constexpr std::int64_t sandstone_below = -fixed_one * 3 / 10;
constexpr std::int64_t shale_below = fixed_one * 3 / 10;
// A gentle warp everywhere. In the belts, folds a few kilometres across and thrust sheets, all
// drawn out along the grain: dips of 20 to 60 degrees where the belt is strongest.
constexpr std::int64_t warp = 150 * fixed_one;
constexpr double warp_wavelength = 40'000.0;
constexpr std::int64_t fold = 2'200 * fixed_one;
constexpr double fold_wavelength = 4'000.0;
constexpr std::int64_t thrust_throw = 2'500 * fixed_one;
constexpr double thrust_wavelength = 12'000.0;
constexpr std::int64_t sheets_per_swing = 3;
constexpr std::int64_t grain_stretch = 4;
// Metres over which the beds' dip is told.
constexpr double dip_step = 40.0;
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

// A fixed-point value in [-1, 1) per bed.
[[nodiscard]] std::int64_t signed_value(std::uint64_t seed, std::int64_t index) {
    return (2 * lattice_value(seed, index, 0, 0)) - fixed_one;
}

// Smooth noise in [-1, 1] through the beds, eased between values `period` beds apart.
[[nodiscard]] std::int64_t through_beds(std::uint64_t seed, std::int64_t index,
                                        std::int64_t period) {
    const std::int64_t at = floor_divide(index, period);
    const std::int64_t along = ((index - (at * period)) << fixed_fraction_bits) / period;
    const std::int64_t eased =
        detail::fade<fixed_fraction_bits>(along) >> (detail::weight_bits - fixed_fraction_bits);
    const std::int64_t from = signed_value(seed, at);
    return from + multiply(signed_value(seed, at + 1) - from, eased);
}

// The column's local noise, mixed differently for each bed.
[[nodiscard]] std::int64_t sway(std::uint64_t seed, std::int64_t index, const BedColumn& column) {
    return multiply(column.lateral[0], signed_value(hash_combine(seed, 0), index)) +
           multiply(column.lateral[1], signed_value(hash_combine(seed, 1), index));
}

// Noise drawn out along the grain, stretched along each axis by how far the belt runs along it,
// as the ridges are.
[[nodiscard]] std::int64_t grained(std::uint64_t seed, NoisePoint point,
                                   const std::array<std::int64_t, 3>& grain) {
    std::int64_t total = 0;
    std::int64_t weights = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::int64_t weight = multiply(grain.at(axis), grain.at(axis));
        if (weight < fixed_one / 50) {
            continue;
        }
        NoisePoint stretched = point;
        std::array<std::int64_t*, 3> along{&stretched.x, &stretched.y, &stretched.z};
        *along.at(axis) /= grain_stretch;
        total += weight * fbm(hash_combine(seed, axis), stretched, octaves);
        weights += weight;
    }
    return weights == 0 ? fbm(seed, point, octaves) : total / weights;
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
    const std::int64_t shift = multiply(warp, noise(1, warp_wavelength));
    const std::int64_t belt = this->belt(center, setting);
    // The granite folds and thrusts with the beds on it.
    return {.shift = static_cast<std::int32_t>(shift + belt),
            .basement = static_cast<std::int32_t>(basement(cell, setting) + belt),
            .leaning = static_cast<std::int32_t>(noise(4, facies_wavelength)),
            .lateral = {static_cast<std::int32_t>(noise(8, lateral_wavelength)),
                        static_cast<std::int32_t>(noise(9, lateral_wavelength))},
            .lava = static_cast<std::int32_t>(
                std::min<std::int64_t>(setting.lava, std::numeric_limits<std::int32_t>::max())),
            .continental = true};
}

std::int64_t Beds::belt(DVec3 center, const BedSetting& setting) const {
    if (setting.mountains == 0) {
        return 0;
    }
    const auto grained_at = [&](std::uint64_t salt, double wavelength) {
        return grained(hash_combine(seed_, salt), noise_point(center, planet_radius_ / wavelength),
                       setting.grain);
    };
    // Each sheet's beds rise towards the thrust that ends it, then start again lower behind.
    const std::int64_t swing = grained_at(6, thrust_wavelength) * sheets_per_swing;
    const std::int64_t along_sheet = swing - (floor_divide(swing, fixed_one) * fixed_one);
    const std::int64_t bend = multiply(fold, grained_at(2, fold_wavelength)) +
                              multiply(thrust_throw, along_sheet - (fixed_one / 2));
    return multiply(bend, setting.mountains);
}

std::int64_t Beds::dip(CellId cell, const BedSetting& setting) const {
    if (!setting.continental || setting.mountains == 0) {
        return 0;
    }
    const DVec3 center = cell_center(cell);
    const TangentFrame frame = tangent_frame(center);
    const double step = dip_step / planet_radius_;
    const std::int64_t here = belt(center, setting);
    const auto rise = [&](DVec3 toward) {
        return fixed_to_double(belt(glm::normalize(center + (toward * step)), setting) - here);
    };
    const double east = rise(frame.east);
    const double north = rise(frame.north);
    return std::llround(std::sqrt((east * east) + (north * north)) / dip_step *
                        static_cast<double>(fixed_one));
}

std::int64_t Beds::basement(CellId cell, BedSetting setting) const {
    const std::int64_t swell =
        fbm(hash_combine(seed_, 3),
            noise_point(cell_center(cell), planet_radius_ / basement_wavelength), octaves);
    return basement_depth + multiply(basement_lift, setting.mountains) +
           multiply(basement_swell, swell);
}

Rock Beds::rock_at(const BedColumn& column, std::int64_t height) const {
    if (!column.continental || height >= column.lava) {
        return Rock::Basalt;
    }
    if (height < column.basement) {
        return Rock::Granite;
    }
    return rock_of(index_at(height - column.shift, column), column);
}

std::optional<Bed> Beds::bed_at(const BedColumn& column, std::int64_t height) const {
    if (!column.continental || height < column.basement || height >= column.lava) {
        return std::nullopt;
    }
    const std::int64_t index = index_at(height - column.shift, column);
    return Bed{.base = base_of(index, column) + column.shift,
               .top = base_of(index + 1, column) + column.shift,
               .rock = rock_of(index, column),
               .index = index};
}

std::int64_t Beds::base_of(std::int64_t index, const BedColumn& column) const {
    // Kept within its own slot, every base lies above the last.
    const std::int64_t place = (fixed_one / 2) +
                               multiply(base_scatter, signed_value(seed_, index)) +
                               multiply(base_sway, sway(hash_combine(seed_, 7), index, column));
    return (index * slot) + multiply(slot, std::clamp<std::int64_t>(place, 0, fixed_one));
}

std::int64_t Beds::index_at(std::int64_t layer, const BedColumn& column) const {
    const std::int64_t index = floor_divide(layer, slot);
    return layer < base_of(index, column) ? index - 1 : index;
}

Rock Beds::rock_of(std::int64_t index, const BedColumn& column) const {
    const std::uint64_t facies = hash_combine(seed_, 5);
    const std::int64_t offshore =
        through_beds(hash_combine(facies, 0), index, sea_beds) +
        multiply(ripple, through_beds(hash_combine(facies, 1), index, ripple_beds)) +
        multiply(facies_chance, signed_value(hash_combine(facies, 2), index)) +
        multiply(facies_lean, column.leaning) +
        multiply(facies_sway, sway(hash_combine(facies, 3), index, column));
    if (offshore < sandstone_below) {
        return Rock::Sandstone;
    }
    return offshore < shale_below ? Rock::Shale : Rock::Limestone;
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
