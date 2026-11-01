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

// The ground's slope measured across a small step either way. Gullies turn with the slope they're
// cut in, which their own gradients can't know without the ground's curvature.
double ground_slope(const terms::Context& context, DVec3 direction, double lod) {
    const double step = std::max(lod, context.planet->voxel) / 8.0;
    const DVec3 side = std::abs(direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side, direction));
    const DVec3 north = glm::cross(direction, east);
    const double turn = step / context.planet->radius;
    const auto height = [&](DVec3 along, double by) {
        return terms::column(context, glm::normalize(direction + (along * by)), lod).height.value;
    };
    const double across_east = (height(east, turn) - height(east, -turn)) / (2.0 * step);
    const double across_north = (height(north, turn) - height(north, -turn)) / (2.0 * step);
    return std::sqrt((across_east * across_east) + (across_north * across_north));
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

Generator::Column Generator::column(DVec3 direction, double lod) const noexcept {
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    const DVec3 down = glm::normalize(direction);
    const terms::Column found = terms::column(context, down, lod);
    Column column;
    column.generator_ = this;
    column.ground_ = planet_.radius + found.height.value - shape_.datum;
    column.slope_ = ground_slope(context, down, lod);
    column.rock_ = found.high >= 0.5 ? planet_.high.rock : planet_.low.rock;
    const double latitude = maths::atan2(down.z, std::sqrt((down.x * down.x) + (down.y * down.y)));
    column.surface_temperature_ = temperature_at(planet_, latitude);
    return column;
}

Sample Generator::Column::sample(double radius) const noexcept {
    const Planet& planet = generator_->planet_;
    // Slope per metre across the ground at this radius, not at the raw radius it was taken at.
    const double slope = slope_ * planet.radius / radius;
    Sample sample;
    sample.distance = (radius - ground_) / std::sqrt(1.0 + (slope * slope));

    if (sample.distance < 0.0) {
        sample.material = rock_;
    } else if (generator_->shape_.sea && radius < planet.radius) {
        sample.material = planet.volatiles == Volatile::Water ? Material::Water : Material::Air;
        sample.water = 1.0;
    }

    const double lapse = planet.pressure > 0.0 ? earth_lapse * planet.gravity / 9.81 : 0.0;
    const double surface = surface_temperature_ - (lapse * std::max(0.0, ground_ - planet.radius));
    if (sample.distance < 0.0) {
        sample.temperature = surface + (generator_->derived_.geotherm * (ground_ - radius));
    } else if (sample.water > 0.0) {
        sample.temperature = surface;
    } else {
        sample.temperature = surface_temperature_ - (lapse * std::max(0.0, radius - planet.radius));
    }
    return sample;
}

Sample Generator::sample(DVec3 point, double lod) const noexcept {
    return column(point, lod).sample(glm::length(point));
}

std::optional<double> ground_below(const Generator& generator, DVec3 direction, double lod) {
    const Planet& planet = generator.planet();
    const Generator::Column column = generator.column(direction, lod);
    const double floor = generator.layers().floor;
    const double least = std::max(lod, planet.voxel) / 4.0;
    double above = generator.layers().ceiling();
    double distance = column.sample(above).distance;
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
        const double next = column.sample(below).distance;
        if (next <= 0.0) {
            break;
        }
        above = below;
        distance = next;
    }
    // Then halve the step to the crossing, to a millimetre.
    while (above - below > 1e-3) {
        const double middle = 0.5 * (above + below);
        (column.sample(middle).distance > 0.0 ? above : below) = middle;
    }
    return 0.5 * (above + below);
}

namespace terms {

Column column(const Context& context, DVec3 direction, double lod) noexcept {
    const Province crust = province(context, direction);
    const double step = context.derived->isostatic_step;
    const Noise shaped = relief(context, direction, crust, lod);
    const Noise struck = craters(context, direction, lod);
    return {.height = {.value = (step * crust.high) + shaped.value + struck.value,
                       .gradient = (step * crust.gradient) + shaped.gradient + struck.gradient},
            .high = crust.high};
}

} // namespace terms

} // namespace hog::globe
