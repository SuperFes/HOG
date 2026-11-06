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

// The planet-wide constants are found from columns sampled this finely: 24,576 of them.
constexpr int survey_level = 6;

// A cave can carve the ground only this near it, the widest passage's radius and some.
constexpr double cave_surface_reach = 8.0;


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
// cut in, which their own gradients can't know without the ground's curvature, and the loose
// cover lies by the slope, which its gradient can't know either.
Tilt ground_tilt(const terms::Context& context, DVec3 direction, double lod) {
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
    return {.slope = std::sqrt((across_east * across_east) + (across_north * across_north)),
            .rise = (across_east * east) + (across_north * north)};
}

// The steps the running water wears over, and the cell width its drainage is worked on.
constexpr int wear_steps = 20;
constexpr double wear_cell = 180.0;
// Standing water this deep over the worn ground is a lake rather than a wet hollow.
constexpr double lake_depth = 0.1;

// Levels finer than the drainage's that the lakes are flooded at: 64 columns across a cell.
constexpr int pond_depth = 6;
constexpr std::uint32_t pond_side = 1U << pond_depth;

int wear_level_for(double radius) {
    const double cells = radius * std::numbers::pi / 2.0 / wear_cell;
    return std::clamp(static_cast<int>(std::lround(std::log2(cells))), 3, 10);
}

// What a map of the drainage's cells holds under `direction`: that of the cell it lies in and
// those around it, weighted by nearness, so it changes smoothly from cell to cell.
double smoothed_under(const Generator::Shape& shape, const std::vector<float>& map,
                      DVec3 direction) {
    const double spacing = std::numbers::pi / 2.0 / static_cast<double>(1 << shape.cut_level);
    const Cell home = cell_under(direction, shape.cut_level);
    double total = 0.0;
    double weights = 0.0;
    const auto add = [&](Cell cell) {
        const double near = 1.0 - (glm::length(center(cell) - direction) / (1.5 * spacing));
        if (near > 0.0) {
            total += near * static_cast<double>(map[index_of(cell)]);
            weights += near;
        }
    };
    add(home);
    const CellNeighbors beside = around(home);
    for (const Cell cell : beside.all()) {
        add(cell);
    }
    return total / weights;
}

// Metres the water has cut from the ground under `direction`.
double cut_under(const Generator::Shape& shape, DVec3 direction) {
    return shape.cut.empty() ? 0.0 : smoothed_under(shape, shape.cut, direction);
}

// A column of a basin's region, waiting to be reached at the height of the rim it's reached over.
struct Waiting {
    float height;
    std::size_t at;

    // Lowest first.
    friend bool operator<(const Waiting& a, const Waiting& b) noexcept {
        return a.height > b.height;
    }
};

// A lake's cells and the ring round them, with the fine ground its water may flood.
class Basin {
public:
    Basin(const Planet& planet, const terms::Context& context, const Generator::Shape& shape,
          std::vector<std::uint64_t> lakes)
        : shape_{&shape}, lakes_{std::move(lakes)} {
        const auto admit = [&](std::uint64_t index) {
            if (slot_.try_emplace(index, region_.size()).second) {
                region_.push_back(index);
            }
        };
        for (const std::uint64_t index : lakes_) {
            admit(index);
            const CellNeighbors beside = around(cell_at_index(shape.cut_level, index));
            for (const Cell cell : beside.all()) {
                admit(index_of(cell));
            }
        }
        sample(planet, context);
    }

    // The columns the lake floods, with the level its water stands at: the lake's, or lower where
    // the fine ground lets it out sooner. Water rises from the lake's own columns through the
    // lowest ground first, so each column is reached over the lowest rim it can be reached by;
    // where that way leads out of the region, the water can stand no higher than the rim it
    // passed, whatever the coarse ground says. What lies under that level and is reached from the
    // lake's columns is the lake, and it is closed: nothing beside it is lower than its surface.
    [[nodiscard]] std::pair<std::vector<Cell>, float> flooded() const {
        const float cap = level();
        constexpr float unreached = std::numeric_limits<float>::max();
        std::vector<float> rim(ground_.size(), unreached);
        std::priority_queue<Waiting> waiting;
        for (const std::uint64_t index : lakes_) {
            const Cell home = cell_at_index(shape_->cut_level, index);
            for (std::uint32_t a = 0; a < pond_side; ++a) {
                for (std::uint32_t b = 0; b < pond_side; ++b) {
                    const auto at = place(column_in(home, a, b));
                    if (at && ground_[*at] < cap && rim[*at] == unreached) {
                        rim[*at] = ground_[*at];
                        waiting.push({.height = rim[*at], .at = *at});
                    }
                }
            }
        }
        float spill = cap;
        while (!waiting.empty()) {
            const Waiting next = waiting.top();
            waiting.pop();
            if (next.height >= spill) {
                break;
            }
            if (next.height > rim[next.at]) {
                continue;
            }
            const Cell column = column_at(next.at);
            for (int di = -1; di <= 1; ++di) {
                for (int dj = -1; dj <= 1; ++dj) {
                    if (di == 0 && dj == 0) {
                        continue;
                    }
                    const auto beside = next_to(column, di, dj);
                    const auto there = beside ? place(*beside) : std::nullopt;
                    if (!there) {
                        spill = std::min(spill, next.height);
                    } else if (ground_[*there] < cap) {
                        const float over = std::max(next.height, ground_[*there]);
                        if (over < rim[*there]) {
                            rim[*there] = over;
                            waiting.push({.height = over, .at = *there});
                        }
                    }
                }
            }
        }
        std::vector<Cell> reached;
        for (std::size_t at = 0; at < rim.size(); ++at) {
            if (rim[at] < spill) {
                reached.push_back(column_at(at));
            }
        }
        return {std::move(reached), spill};
    }

    // The level the lake's cells stand at: they differ by no more than a fall per cell.
    [[nodiscard]] float level() const {
        float lowest = std::numeric_limits<float>::max();
        for (const std::uint64_t index : lakes_) {
            lowest = std::min(lowest, shape_->lake[index]);
        }
        return lowest;
    }

private:
    [[nodiscard]] Cell column_in(Cell home, std::uint32_t a, std::uint32_t b) const {
        return {.level = static_cast<std::uint8_t>(shape_->cut_level + pond_depth),
                .face = home.face,
                .i = (home.i << pond_depth) + a,
                .j = (home.j << pond_depth) + b};
    }

    [[nodiscard]] std::optional<std::size_t> place(Cell column) const {
        const Cell home{.level = static_cast<std::uint8_t>(shape_->cut_level),
                        .face = column.face,
                        .i = column.i >> pond_depth,
                        .j = column.j >> pond_depth};
        const auto found = slot_.find(index_of(home));
        if (found == slot_.end()) {
            return std::nullopt;
        }
        const std::size_t row = column.i & (pond_side - 1);
        const std::size_t across = column.j & (pond_side - 1);
        return (found->second * columns) + (row * pond_side) + across;
    }

    // The column a place among the region's holds.
    [[nodiscard]] Cell column_at(std::size_t at) const {
        const Cell home = cell_at_index(shape_->cut_level, region_[at / columns]);
        const std::size_t within = at % columns;
        return column_in(home, static_cast<std::uint32_t>(within / pond_side),
                         static_cast<std::uint32_t>(within % pond_side));
    }

    void sample(const Planet& planet, const terms::Context& context) {
        const int fine = shape_->cut_level + pond_depth;
        const double lod =
            planet.radius * std::numbers::pi / 2.0 / static_cast<double>(1ULL << fine);
        ground_.resize(region_.size() * columns);
        const unsigned workers =
            region_.size() < 8 ? 1U : std::max(1U, std::thread::hardware_concurrency());
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                for (std::size_t at = worker; at < region_.size(); at += workers) {
                    const Cell home = cell_at_index(shape_->cut_level, region_[at]);
                    for (std::uint32_t a = 0; a < pond_side; ++a) {
                        for (std::uint32_t b = 0; b < pond_side; ++b) {
                            const Cell column = column_in(home, a, b);
                            ground_[(at * columns) + (static_cast<std::size_t>(a) * pond_side) +
                                    b] =
                                static_cast<float>(
                                    terms::ground(context, center(column), lod).height -
                                    shape_->datum);
                        }
                    }
                }
            });
        }
    }

    static constexpr std::size_t columns = static_cast<std::size_t>(pond_side) * pond_side;

    const Generator::Shape* shape_;
    std::vector<std::uint64_t> lakes_;
    std::unordered_map<std::uint64_t, std::size_t> slot_;
    std::vector<std::uint64_t> region_;
    std::vector<float> ground_;
};

// The layer of a cell's pond that holds water at `level`, made if there is none.
Generator::Shape::Pond::Layer& layer_at(Generator::Shape::Pond& pond, float level) {
    auto held = std::ranges::find(pond.layers, level, &Generator::Shape::Pond::Layer::level);
    if (held == pond.layers.end()) {
        held = pond.layers.insert(pond.layers.end(), {.level = level, .rows = {}, .rim = {}});
    }
    return *held;
}

// Lake cells that touch stand at one lake's level, within this many metres; a lake spilling into
// a lower one differs by more.
constexpr float same_lake = 0.5F;

// Floods each lake over the fine ground: water rises from the lake's cells through the lowest
// ground first, and stands as high as the lowest rim it can pass out of the lake's cells and the
// ring round them, so the water takes the ground's own shore and is closed by it. Lakes are
// flooded highest first, and a column a higher lake has flooded is left to it.
void flood_ponds(const Planet& planet, const terms::Context& context, Generator::Shape& shape) {
    std::vector<std::vector<std::uint64_t>> lakes;
    std::vector<bool> seen(shape.lake.size(), false);
    for (std::size_t first = 0; first < shape.lake.size(); ++first) {
        if (seen[first] || shape.lake[first] <= Generator::Shape::no_lake) {
            continue;
        }
        std::vector<std::uint64_t> group{first};
        seen[first] = true;
        for (std::size_t at = 0; at < group.size(); ++at) {
            const CellNeighbors beside = around(cell_at_index(shape.cut_level, group[at]));
            for (const Cell cell : beside.all()) {
                const std::uint64_t index = index_of(cell);
                if (!seen[index] && shape.lake[index] > Generator::Shape::no_lake &&
                    std::abs(shape.lake[index] - shape.lake[group[at]]) < same_lake) {
                    seen[index] = true;
                    group.push_back(index);
                }
            }
        }
        lakes.push_back(std::move(group));
    }
    const auto stands_at = [&](const std::vector<std::uint64_t>& group) {
        return shape.lake[group.front()];
    };
    std::ranges::sort(lakes,
                      [&](const auto& a, const auto& b) { return stands_at(a) > stands_at(b); });
    for (std::vector<std::uint64_t>& group : lakes) {
        const Basin basin{planet, context, shape, std::move(group)};
        const auto [columns, level] = basin.flooded();
        for (const Cell column : columns) {
            auto& pond = shape.ponds[index_of({.level = static_cast<std::uint8_t>(shape.cut_level),
                                               .face = column.face,
                                               .i = column.i >> pond_depth,
                                               .j = column.j >> pond_depth})];
            const std::size_t row = column.i & (pond_side - 1);
            const std::uint64_t bit = 1ULL << (column.j & (pond_side - 1));
            const bool taken = std::ranges::any_of(
                pond.layers, [&](const auto& layer) { return (layer.rows[row] & bit) != 0U; });
            if (taken) {
                continue;
            }
            layer_at(pond, level).rows[row] |= bit;
        }
        // The ring round them, for the voxels' narrower columns whose ground the wider ones miss.
        for (const Cell column : columns) {
            for (int di = -1; di <= 1; ++di) {
                for (int dj = -1; dj <= 1; ++dj) {
                    const auto beside =
                        (di != 0 || dj != 0) ? next_to(column, di, dj) : std::nullopt;
                    if (!beside) {
                        continue;
                    }
                    auto& pond =
                        shape.ponds[index_of({.level = static_cast<std::uint8_t>(shape.cut_level),
                                              .face = beside->face,
                                              .i = beside->i >> pond_depth,
                                              .j = beside->j >> pond_depth})];
                    layer_at(pond, level).rim[beside->i & (pond_side - 1)] |=
                        1ULL << (beside->j & (pond_side - 1));
                }
            }
        }
    }
}

// The ground's cuts by the running water, found by wearing the generated ground on the drainage.
void wear_ground(const Planet& planet, const terms::Context& context, Generator::Shape& shape,
                 const Generator::Progress& say) {
    const int level = wear_level_for(planet.radius);
    const auto count = static_cast<std::size_t>(cells_at(level));
    const double lod = planet.radius * std::numbers::pi / 2.0 / static_cast<double>(1 << level);
    say("sampling the ground for the water to wear");
    std::vector<float> ground(count);
    const unsigned workers = std::max(1U, std::thread::hardware_concurrency());
    {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                for (std::size_t cell = worker; cell < count; cell += workers) {
                    const DVec3 up = center(cell_at_index(level, cell));
                    ground[cell] =
                        static_cast<float>(terms::ground(context, up, lod).height - shape.datum);
                }
            });
        }
    }
    const double sea = shape.sea ? 0.0 : -1e30;
    say("wearing the ground away with running water");
    const std::vector<float> worn =
        erode(ground, level, planet.radius, sea, {.rate = planet.wear, .steps = wear_steps});
    say("working out where the water drains");
    const Drainage drained = drain(worn, level, planet.radius, sea);
    shape.lake.assign(count, Generator::Shape::no_lake);
    shape.catchment.resize(count);
    for (std::size_t cell = 0; cell < count; ++cell) {
        const auto at = static_cast<std::uint32_t>(cell);
        if (static_cast<double>(worn[cell]) > sea && drained.depth(at) > lake_depth) {
            shape.lake[cell] = drained.filled[cell];
        }
        shape.catchment[cell] = static_cast<float>(drained.catchment[cell]);
    }
    shape.receiver = drained.receiver;
    shape.table = drained.filled;
    shape.cut.resize(count);
    for (std::size_t cell = 0; cell < count; ++cell) {
        shape.cut[cell] = ground[cell] - worn[cell];
    }
    shape.cut_level = level;
    say("filling the lakes");
    flood_ponds(planet, context, shape);
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

Generator::Generator(const Planet& planet, const Progress& progress)
    : planet_{planet}, derived_{derive(planet)} {
    const auto say = [&](std::string_view stage) {
        if (progress) {
            progress(stage);
        }
    };
    say("surveying the provinces");
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

    say("surveying the heights");
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
    shape_.highest = std::ranges::max(heights, {}, &Surveyed::value).value - shape_.datum;
    if (planet_.wear > 0.0) {
        wear_ground(planet_, context, shape_, say);
        const float cut = *std::ranges::max_element(shape_.cut);
        shape_.lowest -= static_cast<double>(cut);
    }
    say("laying out the voxel layers");
    layers_ = layers_about(planet_.radius, planet_.floor_depth + std::max(0.0, -shape_.lowest),
                           planet_.sky_height, planet_.voxel);
}

Generator::Gathering Generator::gathered(DVec3 direction) const {
    if (shape_.lake.empty()) {
        return {};
    }
    const DVec3 down = glm::normalize(direction);
    const Cell home = cell_under(down, shape_.cut_level);
    // Only a lake's own cells hold its water: beside one, ground below the level is downstream
    // of its rim, and flooding it would stand the lake up as a wall.
    const float level = shape_.lake[index_of(home)];
    Gathering found{.lake = std::nullopt,
                    .catchment = static_cast<double>(shape_.catchment[index_of(home)])};
    if (level > Shape::no_lake) {
        found.lake = planet_.radius + static_cast<double>(level);
    }
    return found;
}

std::optional<double> Generator::pond_at(DVec3 direction) const {
    if (shape_.ponds.empty()) {
        return std::nullopt;
    }
    const Cell column = cell_under(direction, shape_.cut_level + pond_depth);
    const Cell home{.level = static_cast<std::uint8_t>(shape_.cut_level),
                    .face = column.face,
                    .i = column.i >> pond_depth,
                    .j = column.j >> pond_depth};
    const auto found = shape_.ponds.find(index_of(home));
    if (found == shape_.ponds.end()) {
        return std::nullopt;
    }
    const std::size_t row = column.i & (pond_side - 1);
    const std::uint64_t bit = 1ULL << (column.j & (pond_side - 1));
    // What a lake floods is its own; a rim only where no lake has flooded.
    for (const Shape::Pond::Layer& layer : found->second.layers) {
        if ((layer.rows[row] & bit) != 0U) {
            return planet_.radius + static_cast<double>(layer.level);
        }
    }
    for (const Shape::Pond::Layer& layer : found->second.layers) {
        if ((layer.rim[row] & bit) != 0U) {
            return planet_.radius + static_cast<double>(layer.level);
        }
    }
    return std::nullopt;
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
    return finish(down, lod, terms::ground(context, down, lod), tilt.slope, tilt.rise);
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
    std::vector<terms::Ground> found;
    found.reserve(count);
    std::unordered_map<std::uint64_t, double> heights;
    for (const Cell cell : cells) {
        found.push_back(terms::ground(context, center(cell), lod));
        heights.emplace(index_of(cell), found.back().height);
    }
    const auto height_at = [&](Cell cell) {
        const auto [at, added] = heights.try_emplace(index_of(cell), 0.0);
        if (added) {
            at->second = terms::ground(context, center(cell), lod).height;
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
        made.push_back(
            finish(down, lod, found[k],
                   std::sqrt((across_east * across_east) + (across_north * across_north)),
                   (across_east * east) + (across_north * north)));
    }
    return made;
}

Generator::Column Generator::finish(DVec3 down, double lod, const terms::Ground& made, double slope,
                                    DVec3 tilt) const noexcept {
    Column column;
    column.generator_ = this;
    column.direction_ = down;
    column.lod_ = lod;
    column.ground_ = planet_.radius + made.height - shape_.datum;
    if (shape_.sea) {
        column.water_ = planet_.radius;
    }
    if (derived_.liquid) {
        // The pond's columns are wider than this one, which holds water only where its own ground
        // lies under the level.
        if (const auto lake = pond_at(down); lake && column.ground_ < *lake) {
            column.water_ = std::max(column.water_, *lake);
        }
    }
    if (!shape_.table.empty()) {
        column.table_ = planet_.radius + smoothed_under(shape_, shape_.table, down);
    }
    column.slope_ = slope;
    column.tilt_ = tilt;
    column.surface_temperature_ = made.surface_temperature;
    column.strata_ = made.layers;
    const terms::Context context{.planet = &planet_, .derived = &derived_, .shape = &shape_};
    column.rock_ = terms::rockfall(context, down, lod);
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
    // Passages dissolved by the drainage lie outside the districts the chance caves gather in.
    const bool drained = !generator_->shape_.receiver.empty();
    if (deepest >= top_most || (!drained && terms::cave_district(context, direction_) <= 0.0)) {
        return found;
    }
    const Planet& planet = generator_->planet_;
    const auto cover_at = [&](double at) {
        const double slope = slope_ * planet.radius / at;
        return std::max(0.0, (ground_ - at) / std::sqrt(1.0 + (slope * slope)));
    };
    // Down by as far as no cave can lie within `within`, a metre at least.
    constexpr double least = 1.0;
    std::optional<double> top;
    double above = top_most;
    for (double radius = top_most; radius > deepest;) {
        const double away = terms::cave_bound(context, strata_, direction_, radius, ground_,
                                              cover_at(radius), lod_) -
                            within;
        if (away < 0.0) {
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

DVec3 Generator::Column::cave_rise(double radius) const noexcept {
    const Planet& planet = generator_->planet_;
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    const double slope = slope_ * planet.radius / radius;
    const DVec3 here = direction_ * radius;
    const DVec3 tilt = tilt_ * (planet.radius / radius);
    // Nearby, the ground stands as its tilt has it, and the beds follow it.
    const auto cave_at = [&](DVec3 point) {
        const double out = glm::length(point);
        const DVec3 offset = point - here;
        const double ground = ground_ + glm::dot(tilt, offset);
        terms::Layering beds = strata_;
        beds.fold += glm::dot(strata_.fold_tilt, offset);
        beds.cover += glm::dot(strata_.cover_tilt, offset);
        const double cover = std::max(0.0, (ground - out) / std::sqrt(1.0 + (slope * slope)));
        return terms::cave_distance(context, beds, point / out, out, ground, cover, lod_);
    };
    // By differences, as the cave's distance is its noise over its noise's rate, whose own rate
    // isn't to hand.
    const double step = lod_ / 16.0;
    DVec3 rise{0.0};
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        DVec3 along{0.0};
        along[axis] = step;
        rise[axis] = (cave_at(here + along) - cave_at(here - along)) / (2.0 * step);
    }
    return rise;
}

void Generator::Column::carve_cave(double radius, Sample& sample, DVec3& rise) const noexcept {
    const terms::Context context{.planet = &generator_->planet_,
                                 .derived = &generator_->derived_,
                                 .shape = &generator_->shape_};
    // The rock between the point and the open air over the ground.
    const double cover = std::max(0.0, -sample.distance);
    const double cave =
        terms::cave_distance(context, strata_, direction_, radius, ground_, cover, lod_);
    if (-cave <= sample.distance) {
        return;
    }
    sample.distance = -cave;
    // Only near a surface, where its normal is kept.
    if (std::abs(sample.distance) < distance_band(generator_->layers_, coarser(*generator_, lod_))) {
        rise = -cave_rise(radius);
    }
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

    // A cave takes its voxels out of the rock, and opens the ground where it reaches it.
    if (sample.distance < cave_surface_reach) {
        carve_cave(radius, sample, rise);
    }
    // A fallen rock stands out of the ground, or fills a hollow, where it is nearer than the
    // ground.
    bool on_rock = false;
    if (rock_) {
        DVec3 out{0.0};
        const double near = rock_->distance(direction_ * radius, out);
        if (near < sample.distance) {
            sample.distance = near;
            rise = out;
            on_rock = true;
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
        sample.material = on_rock ? rock_->material
                                  : terms::material_at(context, strata_, shown, ground_ - shown);
    }
    // The sea fills the open space under its level, and the ground's pores below it. The voxel the
    // surface crosses holds only as much as lies under it, so water shallower than a voxel shows.
    // Open space under the ground is flooded as far up as the water table.
    const double water =
        sample.distance > 0.0 && radius < ground_ ? std::max(water_, table_) : water_;
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

Column column(const Context& context, DVec3 direction, double lod) noexcept {
    const Province crust = province(context, direction);
    const double step = context.derived->isostatic_step;
    const Noise built = built_up(context, direction, crust);
    const Noise shaped = relief(context, direction, crust, built, lod);
    const Noise struck = craters(context, direction, lod);
    const Noise erupted = volcanoes(context, direction, lod);
    return {.height = {.value = (step * crust.high) + shaped.value + struck.value + erupted.value,
                       .gradient = (step * crust.gradient) + shaped.gradient + struck.gradient +
                                   erupted.gradient},
            .high = crust.high,
            .built = built.value};
}

Ground ground(const Context& context, DVec3 direction, double lod) noexcept {
    const Planet& planet = *context.planet;
    Ground made;
    made.rock = column(context, direction, lod);
    made.rock.height.value -= cut_under(*context.shape, direction);
    const double latitude = maths::atan2(
        direction.z, std::sqrt((direction.x * direction.x) + (direction.y * direction.y)));
    made.surface_temperature = temperature_at(planet, latitude);
    const double height = made.rock.height.value - context.shape->datum;
    made.layers = layering(context, direction, made.rock, height);
    made.height = made.rock.height.value;
    return made;
}

} // namespace terms

} // namespace hog::globe
