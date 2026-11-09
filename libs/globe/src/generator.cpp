#include "formed.hpp"
#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// A cave can carve the ground only this near it, the widest passage's radius and some.
constexpr double cave_surface_reach = 8.0;

// The way the ground rises, in metres per metre, and how steeply.
// The ground's slope measured across a small step either way. Gullies turn with the slope they're
// cut in, which their own gradients can't know without the ground's curvature, and the loose
// cover lies by the slope, which its gradient can't know either.
double ground_slope(const terms::Context& context, DVec3 direction, double lod) {
    const double step = std::max(lod, context.planet->voxel) / 8.0;
    const DVec3 side = std::abs(direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side, direction));
    const DVec3 north = glm::cross(direction, east);
    const double turn = step / context.planet->radius;
    const auto height = [&](DVec3 along, double by) {
        return terms::ground(context, glm::normalize(direction + (along * by)), lod).height;
    };
    const double across_east = (height(east, turn) - height(east, -turn)) / (2.0 * step);
    const double across_north = (height(north, turn) - height(north, -turn)) / (2.0 * step);
    return std::sqrt((across_east * across_east) + (across_north * across_north));
}

} // namespace

Generator::Generator(const Planet& planet, const Progress& progress)
    : planet_{planet}, derived_{derive(planet)} {
    const auto say = [&](std::string_view stage) {
        if (progress) {
            progress(stage);
        }
    };
    // A formed world's heights are already over its own sea, and its rivers have worn it.
    say("forming the land");
    const auto formed = std::make_shared<const Formed>(planet_, derived_);
    shape_.sea = derived_.liquid && formed->has_sea();
    shape_.lowest = formed->lowest();
    shape_.highest = formed->highest();
    shape_.formed = formed;
    say("laying out the voxel layers");
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
    return finish(down, lod, terms::ground(context, down, lod), ground_slope(context, down, lod),
                  terms::rockfall(context, down, lod));
}

std::vector<Rock> Generator::rocks_near(DVec3 point, double lod) const {
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    const Cell cell = cell_under(glm::normalize(point), terms::rock_level(planet_));
    std::vector<Rock> found;
    const auto add = [&](Cell at) {
        if (const std::optional<Rock> rock = terms::rock_in(context, at, lod)) {
            found.push_back(*rock);
        }
    };
    add(cell);
    const CellNeighbors near = around(cell);
    for (const Cell beside : near.all()) {
        add(beside);
    }
    return found;
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
    const std::vector<terms::Ground> found = terms::ground(context, cells, lod);
    std::unordered_map<std::uint64_t, double> heights;
    for (std::size_t k = 0; k < count; ++k) {
        heights.emplace(index_of(cells[k]), found[k].height);
    }
    // Those beside them the slopes are taken across, made together too.
    std::vector<Cell> beyond;
    for (const Cell cell : cells) {
        const CellNeighbors beside = sides(cell, apart);
        for (const Cell next : beside.all()) {
            if (!heights.contains(index_of(next))) {
                heights.emplace(index_of(next), 0.0);
                beyond.push_back(next);
            }
        }
    }
    const std::vector<terms::Ground> around_them = terms::ground(context, beyond, lod);
    for (std::size_t k = 0; k < beyond.size(); ++k) {
        heights[index_of(beyond[k])] = around_them[k].height;
    }
    const auto height_at = [&](Cell cell) { return heights.at(index_of(cell)); };

    const std::vector<std::optional<Rock>> rocks = terms::rockfall(context, cells, lod);
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
            const double rise = height_at(cell) - found[k].height;
            xx += x * x;
            xy += x * y;
            yy += y * y;
            xh += x * rise;
            yh += y * rise;
        }
        const double determinant = (xx * yy) - (xy * xy);
        const double across_east = ((yy * xh) - (xy * yh)) / determinant;
        const double across_north = ((xx * yh) - (xy * xh)) / determinant;
        made.push_back(finish(
            down, lod, found[k],
            std::sqrt((across_east * across_east) + (across_north * across_north)), rocks[k]));
    }
    return made;
}

Generator::Column Generator::finish(DVec3 down, double lod, const terms::Ground& made, double slope,
                                    const std::optional<Rock>& rock) const noexcept {
    Column column;
    column.generator_ = this;
    column.direction_ = down;
    column.lod_ = lod;
    column.ground_ = planet_.radius + made.height;
    if (shape_.sea) {
        column.water_ = planet_.radius;
    }
    if (derived_.liquid && made.lake) {
        column.water_ = std::max(column.water_, planet_.radius + *made.lake);
    }
    column.slope_ = slope;
    column.surface_temperature_ = made.surface_temperature;
    column.strata_ = made.layers;
    column.rock_ = rock;
    return column;
}

std::vector<std::pair<double, double>> Generator::Column::caves(double lowest, double highest,
                                                                double within) const {
    std::vector<std::pair<double, double>> found;
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    const double deepest = std::max(lowest, ground_ - terms::cave_floor(context));
    const double top_most = std::min(highest, ground_);
    if (deepest >= top_most || terms::cave_district(context, direction_) <= 0.0) {
        return found;
    }
    const Planet& planet = generator_->planet_;
    const auto cover_at = [&](double at) {
        const double slope = slope_ * planet.radius / at;
        return std::max(0.0, (ground_ - at) / std::sqrt(1.0 + (slope * slope)));
    };
    // Down by as far as no cave can lie within `within`. Nearer than a voxel's step counts as
    // within, so every step outside a stretch is one no cave lies across.
    const double least = std::max(lod_ / 2.0, 1.0);
    std::optional<double> top;
    double above = top_most;
    for (double radius = top_most; radius > deepest;) {
        const double away = terms::cave_bound(context, strata_, direction_, radius, ground_,
                                              cover_at(radius), lod_) -
                            within;
        if (away < least) {
            if (!top) {
                // Back within reach at the next sample: the same stretch.
                if (!found.empty() && found.back().first == above) {
                    top = found.back().second;
                    found.pop_back();
                } else {
                    top = above;
                }
            }
        } else if (top) {
            found.emplace_back(radius, *top);
            top.reset();
        }
        above = radius;
        radius -= std::max(away, least);
    }
    if (top) {
        found.emplace_back(deepest, *top);
    }
    std::ranges::reverse(found);
    return found;
}

std::vector<Generator::Column::Boundary> Generator::Column::boundaries(double lowest,
                                                                       double highest,
                                                                       double margin, bool beds,
                                                                       bool caves) const {
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    std::vector<Boundary> found;
    const auto add = [&](double bottom, double top, bool cave = false) {
        bottom -= margin;
        top += margin;
        if (top >= lowest && bottom <= highest) {
            found.push_back({.bottom = bottom, .top = top, .cave = cave});
        }
    };
    // The surface is a slope's width deep, and caves open it from a reach over it.
    const double across = (lod_ / 2.0) * std::sqrt(1.0 + (slope_ * slope_));
    add(ground_ - across, std::max(ground_ + across, ground_ + cave_surface_reach), caves);
    if (rock_) {
        const double reach = std::max({rock_->axes.x, rock_->axes.y, rock_->axes.z});
        const double middle = glm::length(rock_->middle);
        add(middle - reach, middle + reach);
    }
    if (water_ > 0.0) {
        add(water_ - (lod_ / 2.0), water_ + (lod_ / 2.0));
    }
    // A voxel shows the matter at its top, half a lod over its middle.
    const double shown = lod_ / 2.0;
    const double base = ground_ - strata_.cover;
    add(base - shown, base - shown);
    const double low = std::max(lowest, base);
    const double high = std::min(highest, ground_);
    if (beds && low < high) {
        const std::int64_t first = terms::bed_at(context, strata_, low + shown);
        const std::int64_t last = terms::bed_at(context, strata_, high + shown);
        for (std::int64_t bed = first; bed <= last + 1; ++bed) {
            const double edge = terms::bed_bottom(context, strata_, bed) - shown;
            add(edge, edge);
        }
    }
    // A cave changes a voxel from half its thickness away, the voxels as coarse as the detail.
    const double thickness =
        generator_->layers_.thickness * std::max(1.0, lod_ / voxel_lod(*generator_));
    const double reach = margin + (thickness / 2.0);
    if (caves) {
        for (const auto& [bottom, top] : this->caves(lowest, std::min(highest, ground_), reach)) {
            add(bottom, top + cave_surface_reach, true);
        }
    }
    std::ranges::sort(found);
    return found;
}

Material Generator::Column::inside(double radius) const noexcept {
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    const double shown = std::max(0.0, ground_ - radius - (lod_ / 2.0));
    return terms::material_at(context, strata_, shown, ground_ - shown);
}

Sample Generator::Column::sample(double radius, bool caves) const noexcept {
    const Planet& planet = generator_->planet_;
    // Slope per metre across the ground at this radius, not at the raw radius it was taken at.
    const double slope = slope_ * planet.radius / radius;
    Sample sample;
    sample.distance = (radius - ground_) / std::sqrt(1.0 + (slope * slope));
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    const double depth = ground_ - radius;

    // A cave takes its voxels out of the rock, and opens the ground where it reaches it.
    if (caves && sample.distance < cave_surface_reach) {
        // The rock between the point and the open air over the ground.
        const double cover = std::max(0.0, -sample.distance);
        sample.distance =
            std::max(sample.distance, -terms::cave_distance(context, strata_, direction_, radius,
                                                            ground_, cover, lod_));
    }
    // A fallen rock stands out of the ground, or fills a hollow, where it is nearer than the
    // ground.
    bool on_rock = false;
    if (rock_) {
        DVec3 out{0.0};
        const double near = rock_->distance(direction_ * radius, out);
        if (near < sample.distance) {
            sample.distance = near;
            on_rock = true;
        }
    }

    if (sample.distance < 0.0) {
        // A voxel shows the matter at its top, nearest the surface, not at its middle: the first
        // one under the ground shows what's at the ground however coarse the voxels, as fine
        // ones find it, so the ground looks the same at every detail. Voxels stand half a lod
        // apart.
        const double shown = std::max(0.0, depth - (lod_ / 2.0));
        sample.material = on_rock ? rock_->material
                                  : terms::material_at(context, strata_, shown, ground_ - shown);
    }
    // The sea fills the open space under its level, and the ground's pores below it. The voxel the
    // surface crosses holds only as much as lies under it, so water shallower than a voxel shows.
    const double water = water_;
    const double thickness = lod_ / 2.0;
    const double filled = std::clamp(((water - radius) / thickness) + 0.5, 0.0, 1.0);
    if (water > 0.0 && filled > 0.0) {
        sample.liquid = planet.volatiles == Volatile::Water ? Material::Water : Material::Air;
        if (sample.distance < 0.0) {
            if (radius < water) {
                sample.water = properties(sample.material).porosity;
            }
        } else if (sample.liquid != Material::Air) {
            sample.material = sample.liquid;
            sample.water = filled;
        }
    }

    Surroundings surroundings = Surroundings::Open;
    if (sample.distance < 0.0) {
        surroundings = Surroundings::Inside;
    } else if (sample.water > 0.0) {
        surroundings = Surroundings::Water;
    }
    sample.temperature = baseline_temperature(planet, generator_->derived_, surface_temperature_,
                                              radius, ground_, surroundings);
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

std::optional<DVec3> land_near(const Generator& generator, DVec3 direction, double within) {
    const std::optional<double> sea = generator.sea_level();
    const DVec3 up = glm::normalize(direction);
    if (!sea) {
        return up;
    }
    const double radius = generator.planet().radius;
    // Coarsely, as land a kilometre across is sought, not an islet.
    const double lod = voxel_lod(generator) * 16.0;
    const auto land = [&](DVec3 at) {
        return ground_below(generator, at, lod).value_or(0.0) > *sea;
    };
    if (land(up)) {
        return up;
    }
    const DVec3 pole = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(pole, up));
    const DVec3 north = glm::cross(up, east);
    constexpr double ring = 1'000.0;
    for (int k = 1; k * ring <= within; ++k) {
        const double out = (k * ring) / radius;
        const int points = 8 * k;
        for (int n = 0; n < points; ++n) {
            const double turn = 2.0 * std::numbers::pi * n / points;
            const DVec3 at = glm::normalize(up + (east * (out * std::cos(turn))) +
                                            (north * (out * std::sin(turn))));
            if (land(at)) {
                return at;
            }
        }
    }
    return std::nullopt;
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

namespace {

// A formed world's ground: its heights, layers and lakes all read from what formed it.
Ground formed_ground(const Context& context, DVec3 direction, double lod) {
    const Formed::Here here = context.shape->formed->at(direction, lod);
    Ground made;
    const double latitude = maths::atan2(
        direction.z, std::sqrt((direction.x * direction.x) + (direction.y * direction.y)));
    made.surface_temperature = temperature_at(*context.planet, latitude);
    made.layers = here.layers;
    made.height = here.top.value;
    made.lake = here.lake;
    return made;
}

} // namespace

Ground ground(const Context& context, DVec3 direction, double lod) noexcept {
    return formed_ground(context, direction, lod);
}

std::vector<Ground> ground(const Context& context, std::span<const Cell> cells, double lod) {
    std::vector<Ground> made;
    made.reserve(cells.size());
    for (const Cell& cell : cells) {
        made.push_back(formed_ground(context, center(cell), lod));
    }
    return made;
}

} // namespace terms

} // namespace hog::globe
