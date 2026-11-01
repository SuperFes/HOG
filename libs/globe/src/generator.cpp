#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// The planet-wide constants are found from columns sampled this finely: 24,576 of them.
constexpr int survey_level = 6;

// Air cools as it rises by g/cp; Earth's moist air at 6.5 K/km, scaled by gravity.
constexpr double earth_lapse = 0.0065;

// A cave can carve the ground only this near it, the widest passage's radius and some.
constexpr double cave_surface_reach = 8.0;
// Cave walls meet the ground in a fillet this wide rather than a sharp lip.
constexpr double cave_blend = 1.0;

// max(a, b), rounded where they meet over `width`.
double smooth_max(double a, double b, double width) {
    const double h = std::max(width - std::abs(a - b), 0.0);
    return std::max(a, b) + (h * h / (4.0 * width));
}

// How much of smooth_max(a, b, width)'s rate comes from a's.
double smooth_max_share(double a, double b, double width) {
    const double h = std::max(width - std::abs(a - b), 0.0);
    const double blend = h / (2.0 * width);
    return a >= b ? 1.0 - blend : blend;
}

// The detail soil and snow follow, in metres.
constexpr double broad_slope_scale = 16.0;

double lapse_rate(const Planet& planet) {
    return planet.pressure > 0.0 ? earth_lapse * planet.gravity / 9.81 : 0.0;
}

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

// The way the ground rises, in metres per metre, and how steeply.
struct Tilt {
    double slope = 0.0;
    DVec3 rise{0.0};
};

// The ground's slope measured across a small step either way. Gullies turn with the slope they're
// cut in, which their own gradients can't know without the ground's curvature.

Tilt ground_tilt(const terms::Context& context, DVec3 direction, double lod) {
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
    return {.slope = std::sqrt((across_east * across_east) + (across_north * across_north)),
            .rise = (across_east * east) + (across_north * north)};
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
    const Tilt tilt = ground_tilt(context, down, lod);
    return finish(down, lod, terms::column(context, down, lod), tilt.slope, tilt.rise);
}

std::vector<Generator::Column> Generator::columns(Cell corner, int side, double lod) const {
    HOG_ASSERT(side > 0 && corner.is_valid() &&
                   std::cmp_less_equal(corner.i + static_cast<std::uint32_t>(side),
                                       std::uint32_t{1} << corner.level) &&
                   std::cmp_less_equal(corner.j + static_cast<std::uint32_t>(side),
                                       std::uint32_t{1} << corner.level),
               "the square must lie on one face");
    std::vector<Cell> cells;
    cells.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            cells.push_back({.level = corner.level,
                             .face = corner.face,
                             .i = corner.i + static_cast<std::uint32_t>(i),
                             .j = corner.j + static_cast<std::uint32_t>(j)});
        }
    }
    return columns(cells, lod);
}

std::vector<Generator::Column> Generator::columns(std::span<const Cell> cells, double lod,
                                                  int apart) const {
    HOG_ASSERT(
        std::ranges::all_of(
            cells, [&](Cell cell) { return cell.is_valid() && cell.level == cells.front().level; }),
        "the cells must be of one level");
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    const std::size_t count = cells.size();
    std::vector<terms::Column> found;
    found.reserve(count);
    std::unordered_map<std::uint64_t, double> heights;
    for (const Cell cell : cells) {
        found.push_back(terms::column(context, center(cell), lod));
        heights.emplace(index_of(cell), found.back().height.value);
    }
    const auto height_at = [&](Cell cell) {
        const auto [at, added] = heights.try_emplace(index_of(cell), 0.0);
        if (added) {
            at->second = terms::column(context, center(cell), lod).height.value;
        }
        return at->second;
    };

    std::vector<Column> made;
    made.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
        const DVec3 down = center(cells[k]);
        const DVec3 axis = std::abs(down.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
        const DVec3 east = glm::normalize(glm::cross(axis, down));
        const DVec3 north = glm::cross(down, east);
        // The plane through the four heights `apart` beside it, by least squares; across a
        // cube's edge the cells beside it needn't line up.
        double xx = 0.0;
        double xy = 0.0;
        double yy = 0.0;
        double xh = 0.0;
        double yh = 0.0;
        const CellNeighbors beside = sides(cells[k], apart);
        for (const Cell cell : beside.all()) {
            const DVec3 offset = (center(cell) - down) * planet_.radius;
            const double x = glm::dot(offset, east);
            const double y = glm::dot(offset, north);
            const double rise = height_at(cell) - found[k].height.value;
            xx += x * x;
            xy += x * y;
            yy += y * y;
            xh += x * rise;
            yh += y * rise;
        }
        const double determinant = (xx * yy) - (xy * xy);
        const double across_east = ((yy * xh) - (xy * yh)) / determinant;
        const double across_north = ((xx * yh) - (xy * xh)) / determinant;
        made.push_back(
            finish(down, lod, found[k],
                   std::sqrt((across_east * across_east) + (across_north * across_north)),
                   (across_east * east) + (across_north * north)));
    }
    return made;
}

Generator::Column Generator::finish(DVec3 down, double lod, const terms::Column& found,
                                    double slope, DVec3 tilt) const noexcept {
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    Column column;
    column.generator_ = this;
    column.direction_ = down;
    column.lod_ = lod;
    column.ground_ = planet_.radius + found.height.value - shape_.datum;
    column.slope_ = slope;
    column.tilt_ = tilt;
    const double latitude = maths::atan2(down.z, std::sqrt((down.x * down.x) + (down.y * down.y)));
    column.surface_temperature_ = temperature_at(planet_, latitude);
    const double height = column.ground_ - planet_.radius;
    // Soil and snow lie by the slope over tens of metres, not the ground's grain; nor by a
    // coarser detail's smoother ground, or a hillside bare at close hand grows soil from afar.
    const double broad = broad_slope_scale;
    const DVec3 broad_tilt =
        broad == lod ? found.height.gradient : terms::column(context, down, broad).height.gradient;
    column.strata_ = terms::layering(context, down, found, height, glm::length(broad_tilt),
                                     column.surface_temperature_ -
                                         (lapse_rate(planet_) * std::max(0.0, height)));
    return column;
}

Sample Generator::Column::sample(double radius) const noexcept {
    const Planet& planet = generator_->planet_;
    // Slope per metre across the ground at this radius, not at the raw radius it was taken at.
    const double slope = slope_ * planet.radius / radius;
    Sample sample;
    sample.distance = (radius - ground_) / std::sqrt(1.0 + (slope * slope));
    DVec3 rise = direction_ - (tilt_ * (planet.radius / radius));
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    const double depth = ground_ - radius;

    // Caves, carved out of the rock under any loose cover, can open the ground near them.
    if (sample.distance < cave_surface_reach) {
        // The rock between a point and the open air over the ground.
        const auto cover_at = [&](double at, double ground) {
            return std::max(0.0, (ground - at) / std::sqrt(1.0 + (slope * slope)));
        };
        const DVec3 here = direction_ * radius;
        const DVec3 tilt = tilt_ * (planet.radius / radius);
        // Nearby, the ground stands as its tilt has it, and the beds and cover follow it.
        const auto cave_at = [&](DVec3 point) {
            const double out = glm::length(point);
            const DVec3 offset = point - here;
            const double ground = ground_ + glm::dot(tilt, offset);
            terms::Layering beds = strata_;
            beds.fold += glm::dot(strata_.fold_tilt, offset);
            beds.cover += glm::dot(strata_.cover_tilt, offset);
            return terms::cave_distance(context, beds, point / out, out, ground,
                                        cover_at(out, ground), lod_);
        };
        const double cave = terms::cave_distance(context, strata_, direction_, radius, ground_,
                                                 cover_at(radius, ground_), lod_);
        const double ground = sample.distance;
        sample.distance = smooth_max(ground, -cave, cave_blend);
        const double cave_share = smooth_max_share(-cave, ground, cave_blend);
        // Only near a surface, where its normal is kept; there by differences, as the cave's
        // distance is its noise over its noise's rate, whose own rate isn't to hand.
        if (cave_share > 0.0 &&
            std::abs(sample.distance) <
                distance_band(generator_->layers_, coarser(*generator_, lod_))) {
            const double step = lod_ / 16.0;
            DVec3 cave_rise{0.0};
            for (glm::length_t axis = 0; axis < 3; ++axis) {
                DVec3 along{0.0};
                along[axis] = step;
                cave_rise[axis] = (cave_at(here + along) - cave_at(here - along)) / (2.0 * step);
            }
            rise = ((1.0 - cave_share) * glm::normalize(rise)) - (cave_share * cave_rise);
        }
    }
    if (glm::length(rise) > 0.0) {
        sample.normal = glm::normalize(rise);
    }

    if (sample.distance < 0.0) {
        // A voxel shows the matter at its top, nearest the surface, not at its middle: the first
        // one under the ground shows what's at the ground however coarse the voxels, as fine
        // ones find it, so the ground looks the same at every detail. Voxels stand half a lod
        // apart.
        const double shown = std::max(0.0, depth - (lod_ / 2.0));
        sample.material = terms::material_at(context, strata_, shown, ground_ - shown);
    } else if (generator_->shape_.sea && radius < planet.radius) {
        sample.material = planet.volatiles == Volatile::Water ? Material::Water : Material::Air;
        sample.water = 1.0;
    }

    const double lapse = lapse_rate(planet);
    const double surface = surface_temperature_ - (lapse * std::max(0.0, ground_ - planet.radius));
    if (sample.distance < 0.0) {
        sample.temperature = surface + (generator_->derived_.geotherm * std::max(0.0, depth));
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

double voxel_lod(const Generator& generator) {
    return 2.0 * generator.planet().voxel;
}

double coarser(const Generator& generator, double lod) {
    return std::max(1.0, lod / voxel_lod(generator));
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
    const Noise built = built_up(context, direction, crust);
    const Noise shaped = relief(context, direction, crust, built, lod);
    const Noise struck = craters(context, direction, lod);
    return {.height = {.value = (step * crust.high) + shaped.value + struck.value,
                       .gradient = (step * crust.gradient) + shaped.gradient + struck.gradient},
            .high = crust.high,
            .built = built.value};
}

} // namespace terms

} // namespace hog::globe
