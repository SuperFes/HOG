#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// The planet-wide constants are found from columns sampled this finely: 24,576 of them.
constexpr int survey_level = 6;

// Air cools as it rises by g/cp; Earth's moist air at 6.5 K/km, scaled by gravity.
constexpr double earth_lapse = 0.0065;

struct Surveyed {
    double value;
    double area;
};

// The value that `share` of the surface lies below, by area.
double quantile(std::vector<Surveyed> values, double share) {
    std::ranges::sort(values, {}, &Surveyed::value);
    double total = 0.0;
    for (const Surveyed& value : values) {
        total += value.area;
    }
    double below = 0.0;
    for (const Surveyed& value : values) {
        below += value.area;
        if (below >= share * total) {
            return value.value;
        }
    }
    return values.back().value;
}

// A direction from the seed, even over the sphere.
DVec3 direction_from(std::uint64_t seed) {
    const auto unit = [&](std::uint64_t k) {
        return static_cast<double>(hash_values(seed, k) >> 11U) * 0x1.0p-53;
    };
    const double z = (2.0 * unit(0)) - 1.0;
    const double ring = std::sqrt(1.0 - (z * z));
    const double turn = 2.0 * std::numbers::pi * unit(1);
    return {ring * maths::cos(turn), ring * maths::sin(turn), z};
}

std::vector<Surveyed> survey(const auto& measure) {
    std::vector<Surveyed> values;
    values.reserve(cells_at(survey_level));
    for (std::uint64_t index = 0; index < cells_at(survey_level); ++index) {
        const Cell cell = cell_at_index(survey_level, index);
        values.push_back({.value = measure(center(cell)), .area = solid_angle(cell)});
    }
    return values;
}

} // namespace

Generator::Generator(const Planet& planet) : planet_{planet}, derived_{derive(planet)} {
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    shape_.one_side = direction_from(context.seed(terms::Term::OneSide));

    // The high crust covers its share of the surface, and its margin runs as wide as it should
    // where the field crosses at its mean steepness.
    double steepness = 0.0;
    const std::vector<Surveyed> fields = survey([&](DVec3 direction) {
        const Noise field = terms::province_field(context, direction);
        steepness += glm::length(field.gradient) / static_cast<double>(cells_at(survey_level));
        return field.value;
    });
    shape_.province_band = std::max(terms::margin(context) * steepness, 1e-9);
    const auto [lowest, highest] = std::ranges::minmax(fields, {}, &Surveyed::value);
    if (planet_.high_fraction <= 0.0) {
        shape_.province_threshold = highest.value + 1.0;
    } else if (planet_.high_fraction >= 1.0) {
        shape_.province_threshold = lowest.value - 1.0;
    } else {
        shape_.province_threshold = quantile(fields, 1.0 - planet_.high_fraction);
    }

    // The radius stands at the sea, filling its share of the surface, or at the mean ground.
    const double lod = planet_.radius * std::numbers::pi / 2.0 / (1 << survey_level);
    const std::vector<Surveyed> heights = survey(
        [&](DVec3 direction) { return terms::column(context, direction, lod).height.value; });
    shape_.sea = derived_.liquid && planet_.sea_fraction > 0.0;
    if (shape_.sea) {
        shape_.datum = quantile(heights, planet_.sea_fraction);
    } else {
        double total = 0.0;
        double area = 0.0;
        for (const Surveyed& height : heights) {
            total += height.value * height.area;
            area += height.area;
        }
        shape_.datum = total / area;
    }
    shape_.lowest = std::ranges::min(heights, {}, &Surveyed::value).value - shape_.datum;
    layers_ = layers_about(planet_.radius, planet_.floor_depth + std::max(0.0, -shape_.lowest),
                           planet_.sky_height, planet_.voxel);
}

std::optional<double> Generator::sea_level() const noexcept {
    if (!shape_.sea) {
        return std::nullopt;
    }
    return planet_.radius;
}

Sample Generator::sample(DVec3 point, double lod) const noexcept {
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    const double radius = glm::length(point);
    const DVec3 direction = point / radius;
    const terms::Column column = terms::column(context, direction, lod);

    const double ground = planet_.radius + column.height.value - shape_.datum;
    // Slope per metre across the ground at this radius, not at the raw radius it was taken at.
    const double slope = glm::length(column.height.gradient) * planet_.radius / radius;
    Sample sample;
    sample.distance = (radius - ground) / std::sqrt(1.0 + (slope * slope));

    if (sample.distance < 0.0) {
        sample.material = column.high >= 0.5 ? planet_.high.rock : planet_.low.rock;
    } else if (shape_.sea && radius < planet_.radius) {
        sample.material = planet_.volatiles == Volatile::Water ? Material::Water : Material::Air;
        sample.water = 1.0;
    }

    const double latitude = maths::atan2(
        direction.z, std::sqrt((direction.x * direction.x) + (direction.y * direction.y)));
    const double lapse = planet_.pressure > 0.0 ? earth_lapse * planet_.gravity / 9.81 : 0.0;
    const double surface =
        temperature_at(planet_, latitude) - (lapse * std::max(0.0, ground - planet_.radius));
    if (sample.distance < 0.0) {
        sample.temperature = surface + (derived_.geotherm * (ground - radius));
    } else if (sample.water > 0.0) {
        sample.temperature = surface;
    } else {
        sample.temperature =
            temperature_at(planet_, latitude) - (lapse * std::max(0.0, radius - planet_.radius));
    }
    return sample;
}

std::optional<double> ground_below(const Generator& generator, DVec3 direction, double lod) {
    const Planet& planet = generator.planet();
    const DVec3 down = glm::normalize(direction);
    const double floor = generator.layers().floor;
    const double least = std::max(lod, planet.voxel) / 4.0;
    double above = generator.layers().ceiling();
    double distance = generator.sample(down * above, lod).distance;
    if (distance <= 0.0) {
        return above;
    }
    // Walk down by most of the distance, which never overshoots a surface it measures truly.
    double below = 0.0;
    while (true) {
        below = above - std::max(0.9 * distance, least);
        if (below <= floor) {
            return std::nullopt;
        }
        const double next = generator.sample(down * below, lod).distance;
        if (next <= 0.0) {
            break;
        }
        above = below;
        distance = next;
    }
    // Then halve the step to the crossing, to a millimetre.
    while (above - below > 1e-3) {
        const double middle = 0.5 * (above + below);
        (generator.sample(down * middle, lod).distance > 0.0 ? above : below) = middle;
    }
    return 0.5 * (above + below);
}

namespace terms {

Column column(const Context& context, DVec3 direction, double /*lod*/) noexcept {
    const Province crust = province(context, direction);
    const double step = context.derived->isostatic_step;
    return {.height = {.value = step * crust.high, .gradient = step * crust.gradient},
            .high = crust.high};
}

} // namespace terms

} // namespace hog::globe
