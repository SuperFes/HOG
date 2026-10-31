#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/crust_map.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t terrane_salt = 0x7465'7272'616E'6500ULL; // "terrane"
constexpr std::uint64_t gather_salt = 0x6761'7468'6572'0000ULL;  // "gather"
constexpr std::uint64_t rough_salt = 0x726F'7567'6800'0001ULL;

// Terranes grow against ground whose resistance varies this much either way, so their edges, and
// the coasts along them, wander instead of running straight.
constexpr double rough_frequency = 14.0;
constexpr int rough_octaves = 5;
constexpr std::int64_t rough_spread = (fixed_one * 9) / 10;
// A continent's first terrane is picked as the farthest from those before of so many tried.
constexpr int craton_candidates = 32;
// Ocean floor on a plate with no ridge is older than any cooling shows.
constexpr double oldest_ocean = 200.0;
// Rifts never thin crust to nothing.
constexpr double thinnest = 2.0;

constexpr auto none = std::numeric_limits<std::uint32_t>::max();
constexpr auto unreached = std::numeric_limits<std::int64_t>::max();

using Entry = std::tuple<std::int64_t, std::uint32_t, std::uint32_t>;
using Frontier = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;

[[nodiscard]] double unit(std::uint64_t hash) {
    return static_cast<double>(hash >> 11U) * 0x1p-53;
}

[[nodiscard]] DVec3 direction(std::uint64_t seed) {
    for (std::uint64_t attempt = 0;; ++attempt) {
        const DVec3 point{(2.0 * unit(hash_values(seed, attempt, 0U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 1U))) - 1.0,
                          (2.0 * unit(hash_values(seed, attempt, 2U))) - 1.0};
        const double length = glm::length(point);
        if (length <= 1.0 && length > 0.01) {
            return point / length;
        }
    }
}

[[nodiscard]] DVec3 tangent(DVec3 vector, DVec3 at) {
    return vector - (at * glm::dot(vector, at));
}

// 1 at `middle`, falling smoothly to 0 at `reach` either side.
[[nodiscard]] double bump(double at, double middle, double reach) {
    const double off = (at - middle) / reach;
    const double inside = std::max(0.0, 1.0 - (off * off));
    return inside * inside;
}

// The way along a boundary or suture at `at`, given the way across it.
[[nodiscard]] DVec3 along(DVec3 at, DVec3 across) {
    const DVec3 way = glm::cross(at, across);
    const double length = glm::length(way);
    return length > 0.0 ? way / length : DVec3{0.0};
}

struct Nearest {
    std::int64_t distance = unreached; // fixed-point metres
    std::uint32_t source = none;
};

// Each cell's distance to the nearest of `sources`, stepping only from one cell to another that
// `passes` allows, and which source that is.
template <typename Passes>
[[nodiscard]] std::vector<Nearest>
nearest(const LevelGrid& grid, std::span<const std::uint32_t> sources, const Passes& passes) {
    std::vector<Nearest> found(grid.size());
    Frontier frontier;
    for (const std::uint32_t source : sources) {
        frontier.emplace(0, source, source);
    }
    while (!frontier.empty()) {
        const auto [distance, index, source] = frontier.top();
        frontier.pop();
        if (found[index].source != none) {
            continue;
        }
        found[index] = {.distance = distance, .source = source};
        const auto around = grid.neighbors(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            if (found[around[k]].source == none && passes(index, around[k])) {
                frontier.emplace(distance + grid.distance(index, k), around[k], source);
            }
        }
    }
    return found;
}

// Each grid cell's terrane, grown from origins spread at random over the planet.
[[nodiscard]] std::vector<std::uint32_t> grow_terranes(const LevelGrid& grid, std::uint64_t seed,
                                                       std::size_t count) {
    const std::uint64_t rough_seed = hash_combine(seed, rough_salt);
    std::vector<std::int64_t> rough(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto noise = fbm(
            rough_seed, noise_point(cell_center(grid.cell(index)), rough_frequency), rough_octaves);
        rough[index] = fixed_one + ((noise * rough_spread) >> fixed_fraction_bits);
    }
    std::vector<std::uint32_t> terrane(grid.size(), none);
    Frontier frontier;
    for (std::uint32_t origin = 0; origin < count; ++origin) {
        const DVec3 at = direction(hash_values(seed, terrane_salt, origin));
        frontier.emplace(0, grid.index(cell_at(at, grid.level())), origin);
    }
    while (!frontier.empty()) {
        const auto [cost, index, which] = frontier.top();
        frontier.pop();
        if (terrane[index] != none) {
            continue;
        }
        terrane[index] = which;
        const auto around = grid.neighbors(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            if (terrane[around[k]] == none) {
                frontier.emplace(
                    cost + ((grid.distance(index, k) * rough[around[k]]) >> fixed_fraction_bits),
                    around[k], which);
            }
        }
    }
    // Origins that landed in a cell another claimed first grow nothing; renumber the rest.
    std::vector<std::uint32_t> renumbered(count, none);
    std::uint32_t next = 0;
    for (std::uint32_t& which : terrane) {
        if (renumbered[which] == none) {
            renumbered[which] = next++;
        }
        which = renumbered[which];
    }
    return terrane;
}

struct Terranes {
    std::vector<double> areas;
    std::vector<DVec3> middles; // unit
    // Terranes each touches, ascending.
    std::vector<std::vector<std::uint32_t>> beside;
};

[[nodiscard]] Terranes terranes_of(const LevelGrid& grid, std::span<const std::uint32_t> terrane) {
    const std::uint32_t count = std::ranges::max(terrane) + 1;
    Terranes terranes{.areas = std::vector<double>(count, 0.0),
                      .middles = std::vector<DVec3>(count, DVec3{0.0}),
                      .beside = std::vector<std::vector<std::uint32_t>>(count)};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const std::uint32_t own = terrane[index];
        terranes.areas[own] += static_cast<double>(grid.area(index));
        terranes.middles[own] += cell_center(grid.cell(index));
        auto& beside = terranes.beside[own];
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            const std::uint32_t other = terrane[neighbor];
            if (other != own && std::ranges::find(beside, other) == beside.end()) {
                beside.push_back(other);
            }
        }
    }
    for (std::uint32_t which = 0; which < count; ++which) {
        terranes.middles[which] = glm::normalize(terranes.middles[which]);
        std::ranges::sort(terranes.beside[which]);
    }
    return terranes;
}

// Which continent each terrane joined, if any, and in what order.
struct Gathered {
    std::vector<std::uint32_t> continent;
    std::vector<std::uint32_t> order;
    std::uint32_t joined = 0;
};

// Terranes far apart, each the first of a continent.
[[nodiscard]] std::vector<std::uint32_t> pick_cratons(const Terranes& terranes, std::uint64_t seed,
                                                      int continents) {
    const auto count = static_cast<std::uint32_t>(terranes.areas.size());
    std::vector<std::uint32_t> cratons;
    for (int made = 0; made < continents; ++made) {
        std::uint32_t best = none;
        double farthest = -1.0;
        for (int candidate = 0; candidate < craton_candidates; ++candidate) {
            const auto which = static_cast<std::uint32_t>(
                hash_values(seed, gather_salt, static_cast<std::uint64_t>(made),
                            static_cast<std::uint64_t>(candidate)) %
                count);
            double closest = 4.0;
            for (const std::uint32_t craton : cratons) {
                closest = std::min(closest,
                                   glm::length(terranes.middles[which] - terranes.middles[craton]));
            }
            if (closest > farthest) {
                farthest = closest;
                best = which;
            }
        }
        if (farthest > 0.0) {
            cratons.push_back(best);
        }
    }
    return cratons;
}

// Terranes not yet joined beside those of `continent`, ascending.
[[nodiscard]] std::vector<std::uint32_t>
frontier_of(const Terranes& terranes, const Gathered& gathered, std::uint32_t continent) {
    std::vector<std::uint32_t> frontier;
    for (std::uint32_t which = 0; which < gathered.continent.size(); ++which) {
        if (gathered.continent[which] != continent) {
            continue;
        }
        for (const std::uint32_t other : terranes.beside[which]) {
            if (gathered.continent[other] == none) {
                frontier.push_back(other);
            }
        }
    }
    std::ranges::sort(frontier);
    const auto [first, last] = std::ranges::unique(frontier);
    frontier.erase(first, last);
    return frontier;
}

// Continents start from cratons far apart, then take on terranes beside them one at a time, the
// smallest continent first, so they grow out irregularly and about evenly until they cover
// `target` square metres.
[[nodiscard]] Gathered gather(const Terranes& terranes, std::uint64_t seed, int continents,
                              double target) {
    const std::size_t count = terranes.areas.size();
    Gathered gathered{.continent = std::vector<std::uint32_t>(count, none),
                      .order = std::vector<std::uint32_t>(count, none)};
    std::vector<double> grown;
    double total = 0.0;
    const auto join = [&](std::uint32_t which, std::uint32_t into) {
        gathered.continent[which] = into;
        gathered.order[which] = gathered.joined++;
        grown[into] += terranes.areas[which];
        total += terranes.areas[which];
    };
    for (const std::uint32_t craton : pick_cratons(terranes, seed, continents)) {
        grown.push_back(0.0);
        join(craton, static_cast<std::uint32_t>(grown.size() - 1));
    }
    std::uint64_t draw = 0;
    while (total < target) {
        const auto smallest = std::ranges::min_element(grown);
        if (std::isinf(*smallest)) {
            break;
        }
        const auto into = static_cast<std::uint32_t>(smallest - grown.begin());
        const std::vector<std::uint32_t> frontier = frontier_of(terranes, gathered, into);
        if (frontier.empty()) {
            // Hemmed in by the others, it stays as it is.
            *smallest = std::numeric_limits<double>::infinity();
            continue;
        }
        join(frontier[hash_values(seed, gather_salt, draw++) % frontier.size()], into);
    }
    return gathered;
}

// How a column meets the nearest boundary of its plate.
struct Meeting {
    double distance = 0.0; // metres from it
    double closing = 0.0;  // metres a million years; negative where the plates part
    // How much the boundary does, from 0 to 1, by how fast and head-on the plates meet.
    double strength = 0.0;
    bool own_continent = false; // at the boundary, on either side
    bool other_continent = false;
    bool own_dives = false;
};

// Kilometres of crust `column` gains where it meets another plate as `meeting` has it, or loses
// where negative, setting how strongly volcanoes rise in it and a trench sinks it.
[[nodiscard]] double meet(const CrustMap::Settings& s, const Meeting& meeting,
                          CrustMap::Column& column) {
    if (meeting.strength <= 0.0) {
        return 0.0;
    }
    const double strength = meeting.strength;
    const double distance = meeting.distance;
    if (meeting.closing < 0.0) {
        const bool rifting = meeting.own_continent && column.kind == CrustMap::Kind::Continental;
        return rifting ? -s.rift_thinning * strength * bump(distance, 0.0, s.rift_reach) : 0.0;
    }
    if (meeting.own_dives) {
        column.trench = strength * bump(distance, s.trench_out, s.trench_reach);
        return 0.0;
    }
    if (meeting.other_continent) {
        return s.collision_thickening * strength * bump(distance, 0.0, s.collision_reach);
    }
    const double rise = meeting.own_continent
                            ? bump(distance, s.cordillera_inland, s.cordillera_reach)
                            : bump(distance, s.arc_inland, s.arc_reach);
    column.arc = strength * rise;
    return (meeting.own_continent ? s.cordillera_thickening : s.arc_thickening) * strength * rise;
}

} // namespace

CrustMap::CrustMap(std::uint64_t seed, std::shared_ptr<const Plates> plates, Settings settings,
                   double ocean_fraction)
    : settings_{settings}, plates_{std::move(plates)}, columns_(plates_->grid().size()) {
    HOG_ASSERT(settings_.continental > 0.0 && settings_.continental < 1.0,
               "continental share out of range");
    HOG_ASSERT(settings_.continents >= 1, "at least one continent");
    assemble(seed);
    thin_margins();
    age_ocean();
    join_terranes();
    meet_at_boundaries();
    float_crust(ocean_fraction);
}

void CrustMap::assemble(std::uint64_t seed) {
    const LevelGrid& grid = this->grid();
    const double radius = plates_->radius();
    const double area = 4.0 * std::numbers::pi * radius * radius;
    const auto count = static_cast<std::size_t>(
        std::max(std::lround(area / (settings_.terrane_size * settings_.terrane_size)),
                 long{settings_.continents} + 1));
    const std::vector<std::uint32_t> terrane = grow_terranes(grid, seed, count);
    const Terranes terranes = terranes_of(grid, terrane);
    const Gathered gathered =
        gather(terranes, seed, settings_.continents, settings_.continental * area);

    // The first terranes joined are the oldest, the last the youngest, ages falling fastest at
    // first as cratons are few and old.
    const double span = settings_.oldest - settings_.youngest;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const std::uint32_t which = terrane[index];
        const std::uint32_t order = gathered.order[which];
        if (order == none) {
            columns_[index] = {.kind = Kind::Oceanic, .thickness = settings_.oceanic_thickness};
            continue;
        }
        const double late = static_cast<double>(order) / static_cast<double>(gathered.joined);
        const double spread =
            settings_.terrane_spread * ((2.0 * unit(hash_values(seed, terrane_salt, which))) - 1.0);
        columns_[index] = {.kind = Kind::Continental,
                           .thickness = settings_.continental_thickness + spread,
                           .age = settings_.youngest + (span * (1.0 - late) * (1.0 - late)),
                           .terrane = which};
    }
}

void CrustMap::thin_margins() {
    const LevelGrid& grid = this->grid();
    std::vector<std::uint32_t> shore;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (columns_[index].kind != Kind::Oceanic) {
            continue;
        }
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            if (columns_[neighbor].kind == Kind::Continental) {
                shore.push_back(index);
                break;
            }
        }
    }
    const auto from_ocean = nearest(grid, shore, [&](std::uint32_t /*from*/, std::uint32_t to) {
        return columns_[to].kind == Kind::Continental;
    });
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        Column& column = columns_[index];
        if (column.kind != Kind::Continental || from_ocean[index].source == none) {
            continue;
        }
        const double in =
            std::min(fixed_to_double(from_ocean[index].distance) / settings_.margin_reach, 1.0);
        column.thickness -=
            (column.thickness - settings_.margin_thickness) * (1.0 - in) * (1.0 - in);
    }
}

void CrustMap::age_ocean() {
    const LevelGrid& grid = this->grid();
    const auto plate = plates_->plate();
    const auto boundaries = plates_->boundaries();
    std::vector<std::uint32_t> ridges;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (columns_[index].kind == Kind::Oceanic && boundaries[index].distance == 0 &&
            boundaries[index].closing < 0.0) {
            ridges.push_back(index);
        }
    }
    const auto from_ridge = nearest(grid, ridges, [&](std::uint32_t from, std::uint32_t to) {
        return plate[to] == plate[from] && columns_[to].kind == Kind::Oceanic;
    });
    const double slowing = plates_->radius() / settings_.earth_radius;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        Column& column = columns_[index];
        if (column.kind != Kind::Oceanic) {
            continue;
        }
        column.terrane = plate[index];
        const Nearest& ridge = from_ridge[index];
        if (ridge.source == none) {
            column.age = oldest_ocean;
            continue;
        }
        // Each side of a ridge moves off at half the rate the plates part.
        const double half_rate = std::max(-boundaries[ridge.source].closing * 0.5 * slowing, 1e-3);
        column.age = std::min(fixed_to_double(ridge.distance) / half_rate, oldest_ocean);
    }
}

void CrustMap::join_terranes() {
    const LevelGrid& grid = this->grid();
    std::vector<std::uint32_t> sutures;
    std::vector<DVec3> across(grid.size(), DVec3{0.0});
    std::vector<double> ages(grid.size(), 0.0);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const Column& column = columns_[index];
        if (column.kind != Kind::Continental) {
            continue;
        }
        const DVec3 at = cell_center(grid.cell(index));
        double youngest = std::numeric_limits<double>::infinity();
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            const Column& other = columns_[neighbor];
            if (other.kind == Kind::Continental && other.terrane != column.terrane) {
                across[index] += glm::normalize(tangent(cell_center(grid.cell(neighbor)) - at, at));
                youngest = std::min({youngest, other.age, column.age});
            }
        }
        if (std::isfinite(youngest)) {
            sutures.push_back(index);
            ages[index] = youngest;
        }
    }
    const auto from_suture = nearest(grid, sutures, [&](std::uint32_t /*from*/, std::uint32_t to) {
        return columns_[to].kind == Kind::Continental;
    });
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        Column& column = columns_[index];
        const Nearest& suture = from_suture[index];
        if (column.kind != Kind::Continental || suture.source == none) {
            continue;
        }
        const double age = ages[suture.source];
        const double left = settings_.suture_halving / (settings_.suture_halving + age);
        column.worn = settings_.suture_thickening * left *
                      bump(fixed_to_double(suture.distance), 0.0, settings_.suture_reach);
        column.thickness += column.worn;
        column.grain = along(cell_center(grid.cell(index)), across[suture.source]);
    }
}

void CrustMap::meet_at_boundaries() {
    const LevelGrid& grid = this->grid();
    const auto plate = plates_->plate();
    const auto boundaries = plates_->boundaries();
    // What lies across each boundary cell: the crust of the other plate beside it.
    std::vector<std::uint32_t> edges;
    std::vector<std::uint32_t> facing(grid.size(), none);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        if (boundaries[index].distance != 0) {
            continue;
        }
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            if (plate[neighbor] == boundaries[index].other) {
                facing[index] = neighbor;
                break;
            }
        }
        if (facing[index] != none) {
            edges.push_back(index);
        }
    }
    const auto from_edge = nearest(grid, edges, [&](std::uint32_t from, std::uint32_t to) {
        return plate[to] == plate[from];
    });
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const Nearest& edge = from_edge[index];
        if (edge.source == none) {
            continue;
        }
        Column& column = columns_[index];
        const Plates::Boundary& boundary = boundaries[edge.source];
        const Column& own_edge = columns_[edge.source];
        const Column& other_edge = columns_[facing[edge.source]];
        const double motion = std::hypot(boundary.closing, boundary.sliding);
        const double head_on =
            motion > 0.0 ? (boundary.closing * boundary.closing) / (motion * motion) : 0.0;
        const bool own_continent = own_edge.kind == Kind::Continental;
        const bool other_continent = other_edge.kind == Kind::Continental;
        // Of two oceanic edges the older, colder floor dives; ties go to the lower plate.
        const bool own_dives =
            !own_continent && (other_continent || own_edge.age > other_edge.age ||
                               (own_edge.age == other_edge.age && plate[index] < boundary.other));
        const Meeting meeting{
            .distance = fixed_to_double(edge.distance),
            .closing = boundary.closing,
            .strength = std::sqrt(std::min(std::abs(boundary.closing) / settings_.full_rate, 1.0)) *
                        head_on,
            .own_continent = own_continent,
            .other_continent = other_continent,
            .own_dives = own_dives};
        double active = meet(settings_, meeting, column);
        if (active == 0.0) {
            continue;
        }
        active = std::max(active, thinnest - column.thickness);
        column.active = active;
        column.thickness += active;
        if (std::abs(active) > column.worn) {
            column.grain = along(cell_center(grid.cell(index)), boundary.normal);
        }
    }
}

double CrustMap::floated(Kind kind, double thickness, double age) const {
    if (kind == Kind::Continental) {
        return continent_freeboard +
               ((thickness - settings_.continental_thickness) * 1'000.0 * continental_buoyancy);
    }
    const double cooled = std::sqrt(std::min(age / cooling_years, 1.0));
    return (-cooled_depth * cooled) +
           ((thickness - settings_.oceanic_thickness) * 1'000.0 * oceanic_buoyancy);
}

void CrustMap::float_crust(double ocean_fraction) {
    const LevelGrid& grid = this->grid();
    const auto fixed = [](double metres) {
        return std::llround(metres * static_cast<double>(fixed_one));
    };
    heights_.resize(grid.size());
    standing_.resize(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const Column& column = columns_[index];
        const double trench = column.trench * settings_.trench_depth;
        heights_[index] = fixed(floated(column.kind, column.thickness, column.age) - trench);
        standing_[index] =
            fixed(floated(column.kind, column.thickness - column.active, column.age) - trench);
    }
    const double water = topped_up(grid, heights_, starting_water(grid), ocean_fraction);
    sea_level_ = planet::sea_level(grid, heights_, water);
}

} // namespace hog::planet
