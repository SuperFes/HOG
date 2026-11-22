#include "formed.hpp"
#include "ground_cells.hpp"
#include "joints.hpp"
#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/placed.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// A cave can carve the ground only this near it, the widest passage's radius and some.
constexpr double cave_surface_reach = 8.0;

// How far from a column at detail `lod` it keeps the caves' pieces: as far as a cave changes its
// samples, and as far as a node its width or two across asks after them.
double gathered(double lod) noexcept {
    return terms::cave_reach + (4.0 * lod);
}

// Metres about a column caves are gathered from at most to say where they may lie: a few cave
// cells about it, kept for what's near to read again.
constexpr double most_gathered = 1'000.0;

// The way the ground rises, in metres per metre, and how steeply.
// The ground's slope measured across a small step either way. Gullies turn with the slope they're
// cut in, which their own gradients can't know without the ground's curvature, and the loose
// cover lies by the slope, which its gradient can't know either.
double ground_slope(const terms::Context& context, DVec3 direction, double lod) {
    const double step = std::max(lod, context.planet->voxel) / 8.0;
    const DVec3 side = std::abs(direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side, direction));
    const DVec3 north = glm::cross(direction, east);
    const double turn = step / context.planet->body.radius;
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
    const auto formed = std::make_shared<const Formed>(planet_);
    shape_.sea = derived_.liquid && formed->has_sea();
    shape_.lowest = formed->lowest();
    shape_.highest = formed->highest();
    shape_.formed = formed;
    cells_ = std::make_shared<const GroundCells>();
    caves_ = std::make_shared<const CaveCells>();
    weights_ = std::make_shared<const WeightCells>();
    say("laying out the voxel layers");
    // No shell reaches more than halfway to the centre, however deep a small planet's seas run:
    // what lies under its floor is rock.
    const double depth =
        std::min(planet_.floor_depth + std::max(0.0, -shape_.lowest), planet_.body.radius / 2.0);
    layers_ = layers_about(planet_.body.radius, depth,
                           planet_.sky_height + std::max(0.0, shape_.highest), planet_.voxel);
}

std::optional<double> Generator::sea_level() const noexcept {
    if (!shape_.sea) {
        return std::nullopt;
    }
    return planet_.body.radius;
}

terms::Context Generator::context(bool crags) const noexcept {
    return {.planet = &planet_,
            .derived = &derived_,
            .shape = &shape_,
            .crags = crags,
            .cells = cells_.get(),
            .caves = caves_.get(),
            .weights = weights_.get()};
}

Generator::Column Generator::column(DVec3 direction, double lod) const noexcept {
    const terms::Context context = this->context();
    const DVec3 down = glm::normalize(direction);
    return finish(down, lod, terms::ground(context, down, lod), ground_slope(context, down, lod),
                  terms::fallen(context, down, lod));
}

std::vector<Rock> Generator::rocks_near(DVec3 point, double lod) const {
    const terms::Context context = this->context();
    const Cell cell = cell_under(glm::normalize(point), terms::rock_level(planet_));
    std::vector<Rock> found;
    // Each coarsest cell's block and those of the cells inside it.
    std::vector<Cell> waiting{cell};
    const CellNeighbors near = around(cell);
    for (const Cell beside : near.all()) {
        waiting.push_back(beside);
    }
    const int finest = terms::rock_level(planet_) + terms::rock_sizes - 1;
    while (!waiting.empty()) {
        const Cell at = waiting.back();
        waiting.pop_back();
        if (const std::optional<Rock> rock = terms::rock_in(context, at, lod)) {
            found.push_back(*rock);
        }
        if (std::cmp_less(at.level, finest)) {
            for (const Cell inside : children(at)) {
                waiting.push_back(inside);
            }
        }
    }
    return found;
}

std::vector<CavePiece> Generator::caves_within(DVec3 toward, double reach) const {
    return terms::caves_near(context(), glm::normalize(toward), reach);
}

std::vector<CavePiece> Generator::caves_begun(DVec3 toward) const {
    return *terms::caves_in(context(),
                            cell_under(glm::normalize(toward), terms::cave_level(planet_)));
}

std::vector<Rock> Generator::rocks_within(DVec3 toward, double reach, double lod) const {
    if (terms::widest_rock(planet_) < lod) {
        return {};
    }
    const terms::Context context = this->context();
    const int level = terms::rock_level(planet_);
    const DVec3 middle = glm::normalize(toward);
    // A cell's every point lies within half its diagonal of its middle; none is wider than this.
    const double slack = std::numbers::pi / 2.0 * planet_.body.radius / std::ldexp(1.0, level);
    const auto near = [&](Cell cell) {
        return glm::length(center(cell) - middle) * planet_.body.radius <= reach + slack;
    };
    std::vector<Cell> coarsest{cell_under(middle, level)};
    std::set<Cell> seen{coarsest.front()};
    for (std::size_t k = 0; k < coarsest.size(); ++k) {
        const CellNeighbors beside = sides(coarsest[k]);
        for (const Cell next : beside.all()) {
            if (near(next) && seen.insert(next).second) {
                coarsest.push_back(next);
            }
        }
    }
    return terms::rocks_in(context, coarsest, lod);
}

std::vector<Crag> Generator::crags_within(DVec3 toward, double reach, double lod) const {
    const double radius = planet_.body.radius;
    // Columns no further apart than the narrowest block standing, or the detail asked, so each
    // block with a crag shown is met.
    const double apart = std::max(lod, closest_joints);
    const int level =
        static_cast<int>(std::ceil(std::log2(std::numbers::pi / 2.0 * radius / apart)));
    const double slack = std::numbers::pi / 2.0 * radius / std::ldexp(1.0, level);
    const DVec3 middle = glm::normalize(toward);
    std::vector<Cell> cells{cell_under(middle, level)};
    std::set<Cell> seen{cells.front()};
    for (std::size_t k = 0; k < cells.size(); ++k) {
        const CellNeighbors beside = sides(cells[k]);
        for (const Cell next : beside.all()) {
            if (glm::length(center(next) - middle) * radius <= reach + slack &&
                seen.insert(next).second) {
                cells.push_back(next);
            }
        }
    }
    const std::vector<Column> made = columns(cells, lod, 1, false);
    std::vector<const Column*> standing;
    for (const Column& column : made) {
        if (column.crag()) {
            standing.push_back(&column);
        }
    }
    return crags_from(standing, lod);
}

namespace {

// Metres from the centre the top of `core` stands along the unit direction `up`, found coming
// down it; none where it misses.
std::optional<double> top_along(const Placed& core, DVec3 up) {
    const double across = 2.0 * glm::length(core.half);
    const double step = std::max(core.rounding, 0.05);
    const auto inside = [&](double radius) { return core.distance(up * radius) < 0.0; };
    const double highest = glm::dot(core.middle, up) + across;
    double below = highest;
    while (below > highest - (2.0 * across) && !inside(below)) {
        below -= step;
    }
    if (!inside(below)) {
        return std::nullopt;
    }
    double above = highest;
    for (int halving = 0; halving < 40; ++halving) {
        const double between = 0.5 * (above + below);
        (inside(between) ? below : above) = between;
    }
    return below;
}

} // namespace

std::vector<Crag> Generator::crags_from(std::span<const Column* const> columns, double lod) {
    // The columns each block stands out of, by its key.
    std::map<std::uint64_t, std::vector<std::pair<const Column*, CragStanding>>> blocks;
    for (const Column* column : columns) {
        if (const std::optional<CragStanding>& standing = column->crag()) {
            blocks[standing->core.key].emplace_back(column, *standing);
        }
    }
    std::vector<Crag> crags;
    crags.reserve(blocks.size());
    for (const auto& [key, over] : blocks) {
        Crag crag = over.front().second.core;
        const Placed core = placed_crag(crag);
        // Lowered to stand as high at its highest as the crag stood out of the ground, the rotted
        // rock stripped from about it no deeper than it rotted; of the rock it stood highest over.
        double stood = -1e300;
        double stands = -1e300;
        const std::pair<const Column*, CragStanding>* highest = &over.front();
        for (const auto& standing : over) {
            if (const double top = standing.second.rock_top + standing.second.height; top > stood) {
                stood = top;
                highest = &standing;
            }
            stands =
                std::max(stands, top_along(core, standing.first->direction()).value_or(stands));
        }
        crag.middle -= glm::normalize(crag.middle) * std::max(0.0, stands - stood);
        crag.material = highest->first->inside(highest->second.rock_top - lod);
        crags.push_back(crag);
    }
    return crags;
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
                                                  int apart, bool placed) const {
    HOG_ASSERT(
        std::ranges::all_of(
            cells, [&](Cell cell) { return cell.is_valid() && cell.level == cells.front().level; }),
        "the cells must be of one level");
    const terms::Context context = this->context(placed);
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

    const std::vector<terms::Fall> falls = terms::fallen(context, cells, lod);
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
            const DVec3 offset = (center(cell) - down) * planet_.body.radius;
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
        terms::Fall fall = falls[k];
        if (!placed) {
            fall.rock.reset();
        }
        made.push_back(
            finish(down, lod, found[k],
                   std::sqrt((across_east * across_east) + (across_north * across_north)), fall));
    }
    return made;
}

Generator::Column Generator::finish(DVec3 down, double lod, const terms::Ground& made, double slope,
                                    const terms::Fall& fall) const noexcept {
    Column column;
    column.generator_ = this;
    column.direction_ = down;
    column.lod_ = lod;
    // Blocks too small to show lie spread over the ground as rubble.
    column.ground_ = planet_.body.radius + made.height + fall.rubble.depth;
    if (shape_.sea) {
        column.water_ = planet_.body.radius;
        column.body_ = {.kind = WaterBody::Kind::Sea, .index = 0};
    }
    // A lake or river standing over the sea rises and falls with the year as it does; the sea's
    // tide is its own.
    const std::optional<double> inland = made.lake ? made.lake : made.river;
    if (derived_.liquid && inland && planet_.body.radius + *inland > column.water_) {
        column.water_ = planet_.body.radius + *inland;
        column.body_ = made.body;
        column.water_rise_ = made.water_rise;
        column.water_peak_ = made.water_peak;
    }
    column.slope_ = slope;
    column.rest_ = made.rest;
    column.channel_away_ = made.channel_away;
    column.crag_ = made.crag;
    column.surface_temperature_ = made.surface_temperature;
    column.strata_ = made.layers;
    const terms::Context context = this->context();
    if (terms::rubble_shows(context, down, lod, fall.rubble.share)) {
        column.strata_.rubble = fall.rubble.depth;
        column.strata_.rubble_of = fall.rubble.of;
    }
    column.strata_.cover += fall.rubble.depth;
    column.scatter_ = terms::scatter_at(context, down, column.strata_);
    column.rock_ = fall.rock;
    // Coarser than any cave, a column's samples show none; a node may still ask after them.
    if (lod < terms::widest_cave) {
        column.gathered_ = gathered(lod);
        column.caves_ = terms::caves_near(context, down, column.gathered_);
    }
    return column;
}

bool Generator::Column::caves_may_lie(double lowest, double highest, double within) const {
    // Wider than caves are gathered, they may lie anywhere their depth allows under any ground.
    if (within > most_gathered) {
        const terms::Context context = generator_->context();
        const double radius = generator_->planet().body.radius;
        const Shape& shape = generator_->shape();
        return highest >= radius + shape.lowest - terms::cave_floor(context) - terms::widest_cave &&
               lowest <= radius + shape.highest + terms::widest_cave;
    }
    const auto lies = [&](const CavePiece& piece) {
        const auto span = piece.along(direction_, piece.radius + within);
        return span && span->first <= highest && span->second >= lowest;
    };
    if (within <= gathered_) {
        return std::ranges::any_of(caves_, lies);
    }
    return std::ranges::any_of(terms::caves_near(generator_->context(), direction_, within), lies);
}

std::vector<std::pair<double, double>> Generator::Column::caves(double lowest, double highest,
                                                                double within) const {
    if (lod_ >= terms::widest_cave) {
        return {};
    }
    // Wider than caves are gathered, all the depth they may lie at under the ground.
    if (within > most_gathered) {
        const double radius = generator_->planet().body.radius;
        const double bottom =
            std::max(lowest, radius + generator_->shape().lowest -
                                 terms::cave_floor(generator_->context()) - terms::widest_cave);
        const double top = std::min(highest, ground_);
        return bottom < top ? std::vector{std::pair{bottom, top}}
                            : std::vector<std::pair<double, double>>{};
    }
    const std::vector<CavePiece> regathered =
        within > gathered_ ? terms::caves_near(generator_->context(), direction_, within)
                           : std::vector<CavePiece>{};
    const std::span<const CavePiece> pieces = within > gathered_ ? regathered : caves_;
    const double top_most = std::min(highest, ground_);
    std::vector<std::pair<double, double>> found;
    for (const CavePiece& piece : pieces) {
        const double shown = shown_radius(piece.radius, lod_);
        if (shown <= 0.0) {
            continue;
        }
        if (const auto span = piece.along(direction_, shown + within)) {
            const double bottom = std::max(span->first, lowest);
            const double top = std::min(span->second, top_most);
            if (bottom < top) {
                found.emplace_back(bottom, top);
            }
        }
    }
    std::ranges::sort(found);
    std::vector<std::pair<double, double>> merged;
    for (const auto& [bottom, top] : found) {
        if (!merged.empty() && bottom <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, top);
        } else {
            merged.emplace_back(bottom, top);
        }
    }
    return merged;
}

std::vector<Generator::Column::Boundary> Generator::Column::boundaries(double lowest,
                                                                       double highest,
                                                                       double margin, bool beds,
                                                                       bool caves) const {
    const terms::Context context = generator_->context();
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
    // A voxel shows the matter at its top, half a lod over its middle, each contact wandering
    // either way of where it was laid.
    const double shown = lod_ / 2.0;
    const double base = ground_ - strata_.cover;
    add(base - shown - terms::contact_reach, base - shown + terms::contact_reach);
    const double low = std::max(lowest, base);
    const double high = std::min(highest, ground_);
    if (beds && low < high) {
        terms::Beds bedding{context, strata_};
        const std::int64_t first = bedding.at(low + shown - terms::contact_reach);
        const std::int64_t last = bedding.at(high + shown + terms::contact_reach);
        for (std::int64_t bed = first; bed <= last + 1; ++bed) {
            const double edge = bedding.bottom(bed) - shown;
            add(edge - terms::contact_reach, edge + terms::contact_reach);
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
    const terms::Context context = generator_->context();
    const double shown = std::max(0.0, ground_ - radius - (lod_ / 2.0));
    return terms::material_at(context, strata_, scatter_, direction_, shown, ground_ - shown);
}

Sample Generator::Column::sample(double radius, bool caves) const noexcept {
    const Planet& planet = generator_->planet_;
    // Slope per metre across the ground at this radius, not at the raw radius it was taken at.
    const double slope = slope_ * planet.body.radius / radius;
    Sample sample;
    sample.distance = (radius - ground_) / std::sqrt(1.0 + (slope * slope));
    const terms::Context context = generator_->context();
    const double depth = ground_ - radius;

    // A cave takes its voxels out of the rock, and opens the ground where it reaches it. Only one
    // within reach counts: at the reach, none is known to be near, and the ground's distance
    // stands.
    if (caves && sample.distance < cave_surface_reach) {
        const DVec3 point = direction_ * radius;
        double cave = terms::cave_reach;
        for (const CavePiece& piece : caves_) {
            if (const double shown = shown_radius(piece.radius, lod_); shown > 0.0) {
                cave = std::min(cave, piece.distance(point, shown));
            }
        }
        if (cave < terms::cave_reach) {
            sample.distance = std::max(sample.distance, -cave);
        }
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
                                  : terms::material_at(context, strata_, scatter_, direction_,
                                                       shown, ground_ - shown);
    }
    // The sea fills the open space under its level, and the ground's pores below it. The voxel the
    // surface crosses holds only as much as lies under it, so water shallower than a voxel shows.
    const double water = water_;
    const double thickness = lod_ / 2.0;
    const double filled = std::clamp(((water - radius) / thickness) + 0.5, 0.0, 1.0);
    if (water > 0.0 && filled > 0.0) {
        sample.liquid = Material::Water;
        sample.rise = water_rise_;
        sample.peak = water_peak_;
        if (sample.distance < 0.0) {
            // The pores of the ground under the level, and over the ground as much of the voxel
            // as lies under it, where the ground's surface crosses the voxel.
            const double ground = std::clamp(0.5 - (sample.distance / thickness), 0.0, 1.0);
            sample.water = (std::min(ground, filled) * properties(sample.material).porosity) +
                           std::clamp(filled - ground, 0.0, 1.0 - ground);
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

Climate climate_at(const Generator& generator, DVec3 direction) {
    constexpr double kelvin = 273.15;
    const Formed::Climate formed =
        generator.shape().formed->climate_at(glm::normalize(direction), 0.0);
    return {.kelvin = formed.temperature + kelvin,
            .rain = formed.rain,
            .liquid = generator.derived().liquid};
}

std::optional<DVec3> land_near(const Generator& generator, DVec3 direction, double within) {
    const std::optional<double> sea = generator.sea_level();
    const DVec3 up = glm::normalize(direction);
    if (!sea) {
        return up;
    }
    const double radius = generator.planet().body.radius;
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
            const DVec3 at = glm::normalize(up + (east * (out * maths::cos(turn))) +
                                            (north * (out * maths::sin(turn))));
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

// The rounded core left of a block rotted `rotted` in from its joints, before it's lowered onto
// the ground.
Crag core_of(const Block& block, double rotted) noexcept {
    const DVec3 half = block.size() / 2.0;
    const double thinnest = std::min({half.x, half.y, half.z});
    const DVec3 centre = (block.low + block.high) / 2.0;
    return {.middle = block.origin + (block.sets[0] * centre.x) + (block.sets[1] * centre.y) +
                      (block.sets[2] * centre.z),
            .faces = block.sets,
            .half = half - rotted,
            .rounding = std::min(rotted, thinnest - rotted),
            .material = Material::Air,
            .key = block.id};
}

// How much each of forming's points counts at a seed-level cell's middle, weighed there; kept in
// `kept` where there is one.
CellWeights seed_weights(const Formed& formed, Cell cell, const WeightCells* kept) {
    if (kept != nullptr) {
        if (std::optional<CellWeights> found = kept->find(cell)) {
            return *std::move(found);
        }
    }
    auto made = std::make_shared<formation::Smooth::Weights>();
    formed.weigh(center(cell), *made);
    if (kept != nullptr) {
        kept->keep(cell, made);
    }
    return made;
}

// How much each of forming's points counts at `direction`: read between the four seed-level cells
// about it, so reads anywhere, at any detail, share one smooth set of weights; weighed there over
// a cube's edge, where the four make no square.
void weights_at(const Formed& formed, DVec3 direction, const WeightCells* kept,
                formation::Smooth::Weights& into) {
    const CellPlace place = place_under(direction, formed.seed_level());
    const int di = place.i < 0.0 ? -1 : 1;
    const int dj = place.j < 0.0 ? -1 : 1;
    const std::optional<Cell> across = next_to(place.cell, di, 0);
    const std::optional<Cell> along = next_to(place.cell, 0, dj);
    const std::optional<Cell> corner = across ? next_to(*across, 0, dj) : std::optional<Cell>{};
    const std::uint8_t face = place.cell.face;
    if (!across || !along || !corner || across->face != face || along->face != face ||
        corner->face != face) {
        formed.weigh(direction, into);
        return;
    }
    // In the order (0, 0), (1, 0), (0, 1), (1, 1) across the square.
    const std::size_t own = (di < 0 ? 1U : 0U) | (dj < 0 ? 2U : 0U);
    std::array<Cell, 4> square{};
    square.at(own) = place.cell;
    square.at(own ^ 1U) = *across;
    square.at(own ^ 2U) = *along;
    square.at(own ^ 3U) = *corner;
    std::array<CellWeights, 4> known;
    std::array<formation::Smooth::Corner, 4> corners;
    for (std::size_t c = 0; c < 4; ++c) {
        known.at(c) = seed_weights(formed, square.at(c), kept);
        corners.at(c) = {.direction = center(square.at(c)), .weights = known.at(c).get()};
    }
    const double s = di < 0 ? 1.0 + place.i : place.i;
    const double t = dj < 0 ? 1.0 + place.j : place.j;
    formation::Smooth::between(direction, corners, s, t, into);
}

// A formed world's ground: its heights, layers and lakes all read from what formed it.
Ground formed_ground(const Context& context, DVec3 direction, double lod) {
    const Formed& formed = *context.shape->formed;
    thread_local formation::Smooth::Weights weights;
    weights_at(formed, direction, context.weights, weights);
    const Formed::Here here = formed.at(direction, weights, lod, context.crags);
    Ground made;
    const double latitude = maths::atan2(
        direction.z, std::sqrt((direction.x * direction.x) + (direction.y * direction.y)));
    made.surface_temperature = temperature_at(*context.planet, latitude);
    made.layers = here.layers;
    made.height = here.top.value;
    made.lake = here.lake;
    if (here.lake) {
        made.body = {.kind = WaterBody::Kind::Lake, .index = here.lake_index};
    } else if (here.river) {
        made.river = here.river->level;
        made.body = {.kind = WaterBody::Kind::River, .index = here.river->stem};
    }
    made.water_rise = here.season.rise;
    made.water_peak = here.season.peak;
    if (here.standing) {
        made.crag = CragStanding{.core = core_of(here.standing->block, here.standing->rotted),
                                 .rock_top = here.standing->rock_top,
                                 .height = here.standing->height};
    }
    made.rise = tangential(here.top.gradient, direction);
    made.above_floor = here.above_floor;
    made.channel_away = here.channel_away;
    made.rest = here.rest;
    made.loosening = here.weathering.loosening;
    made.weathering = here.weathering.rate;
    made.worn = here.weathering.worn;
    made.years = here.weathering.years;
    made.strength = here.weathering.strength;
    made.bare = here.weathering.bare;
    return made;
}

} // namespace

Ground ground(const Context& context, DVec3 direction, double lod) noexcept {
    return formed_ground(context, direction, lod);
}

void weights_at(const Context& context, DVec3 direction, formation::Smooth::Weights& into) {
    weights_at(*context.shape->formed, direction, context.weights, into);
}

Ground ground(const Context& context, Cell cell, double lod) {
    if (context.cells == nullptr) {
        return formed_ground(context, center(cell), lod);
    }
    const GroundCells::Key key{.cell = cell, .lod = lod, .crags = context.crags};
    if (std::optional<Ground> kept = context.cells->find(key)) {
        return *std::move(kept);
    }
    Ground made = formed_ground(context, center(cell), lod);
    context.cells->keep(key, made);
    return made;
}

std::vector<Ground> ground(const Context& context, std::span<const Cell> cells, double lod) {
    std::vector<Ground> made;
    made.reserve(cells.size());
    for (const Cell& cell : cells) {
        made.push_back(ground(context, cell, lod));
    }
    return made;
}

} // namespace terms

} // namespace hog::globe
