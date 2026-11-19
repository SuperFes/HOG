#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/habitat.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hog::globe {

namespace {

// Half a node's diagonal, in its sides: no surface further than this from its middle crosses it.
constexpr double half_diagonal = 0.866'025'403'784'438'6;
// Room for the distance field being true only near the surface: a node is taken as missed only
// this much further out.
constexpr double clear = 1.5 * half_diagonal;
// Leaves whose middle lies within this many sides of the surface can have finer detail to come.
constexpr double near = 2.0;
// Metres across the finest nodes woods are held in: stands are hundreds of metres across, and a
// tree is placed within a node by its own hash.
constexpr double woods_finest = 16.0;
static_assert(grid::Stand::kinds == flora::kinds, "the pages hold every kind of tree");

// What the generator gives at a node's middle.
struct Probe {
    double distance = 0.0; // metres, negative inside the solid
    Material material = Material::Air;
    double size = 0.0;   // metres across the node
    double radius = 0.0; // of its middle, from the centre
    double ground = 0.0; // under it, metres from the centre
    double water = 0.0;  // over it at its highest in the year, likewise; 0 none
    WaterBody body;      // which that water is
    // Its plane where the surface may cross it, else solid or open; none past what's filled.
    std::optional<grid::Surface> surface;
    bool splits = false; // the surface may cross it and finer nodes are filled under it
    bool caves = false;  // a cave may lie in it at finer detail
};

// Which way is up at a point. At the centre every way is down alike, so any will do.
DVec3 up_at(DVec3 point) noexcept {
    const double radius = glm::length(point);
    return radius > 0.0 ? point / radius : DVec3{0.0, 0.0, 1.0};
}

// Metres from the centre a column's water stands at its highest in the year; 0 where none does.
double highest_water(const Generator::Column& column) noexcept {
    return column.water() > 0.0 ? column.water() + column.water_rise() : 0.0;
}

// The four cells whose columns a direction lies between, and how much each counts.
struct Between {
    std::array<Cell, 4> cells;
    std::array<double, 4> weights{};
};

class Filler {
public:
    Filler(const Generator& generator, const grid::Grid& grid, grid::Key key, std::uint8_t levels)
        : generator_{&generator}, grid_{&grid}, key_{key}, levels_{levels} {
        survey();
    }

    // Each node of each tree has its own value as it's sampled at its own size, split or not, so
    // a node reads the same however deep it's filled.
    grid::Tree<grid::Shape>::Decided shape(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z) const {
        const Probe& here = probes_.at(grid::code(absolute(depth, x, y, z)));
        return {.value = here.surface.value(), .open = open(depth, here), .split = here.splits};
    }
    grid::Tree<grid::Material>::Decided material(std::uint8_t depth, std::uint32_t x,
                                                 std::uint32_t y, std::uint32_t z) const {
        const Probe& here = probes_.at(grid::code(absolute(depth, x, y, z)));
        return {
            .value = palette_of(here.material), .open = open(depth, here), .split = here.splits};
    }
    grid::Tree<grid::Water>::Decided water(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z) const {
        const Probe& here = probes_.at(grid::code(absolute(depth, x, y, z)));
        // The sea's basin: reaching under its level and over the ground, so not a cave under it,
        // nor rock the surface clearly misses. Where in it water stands is the level's to say, so
        // it needn't split along the sea's top.
        const double half = half_diagonal * here.size;
        const bool wet =
            here.water > 0.0 && here.radius - half < here.water && here.radius + half > here.ground;
        return {.value = wet ? body_value(here.body) : 0,
                .open = open(depth, here),
                .split = here.splits};
    }

    // The woods on the ground through a node, sampled at its middle with detail no finer than it,
    // down to nodes woods_finest across.
    grid::Tree<grid::Woods>::Decided woods(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z) const {
        const grid::Key key = absolute(depth, x, y, z);
        const Probe& here = probes_.at(grid::code(key));
        // Open by size alone, ground or none, so stands merge across the air beside them.
        const bool finer = here.size / 2.0 >= woods_finest && key.depth < grid_->depth();
        if (!here.splits && !here.surface.value().crosses()) {
            return {.value = {}, .open = finer, .split = false};
        }
        const bool split = finer && depth < levels_ && here.splits;
        if (generator_->shape().formed == nullptr) {
            return {.value = {.ground = true}, .open = finer, .split = split};
        }
        // The top node's middle is the planet's centre, which has no up of its own.
        const flora::Woods found = globe::woods(*generator_, up_at(grid_->centre(key)), here.size);
        return {.value = grid::Stand::of(found.cover, found.shares), .open = finer, .split = split};
    }

    // The finest columns made with a crag left out of their ground.
    [[nodiscard]] std::vector<const Generator::Column*> standing() const {
        const int level =
            std::max(generator_->layers().level - (grid_->depth() - (key_.depth + levels_)), 0);
        std::vector<const Generator::Column*> found;
        for (const auto& [slot, column] : columns_) {
            if (std::cmp_equal(slot & 31U, level) && column.crag()) {
                found.push_back(&column);
            }
        }
        // In one order however they're held, for the material each takes from the first tied.
        std::ranges::sort(found, {}, [](const Generator::Column* column) {
            const DVec3 way = column->direction();
            return std::array{way.x, way.y, way.z};
        });
        return found;
    }

    // Whether the ground passes within `reach` metres of the filled node.
    [[nodiscard]] bool near_ground(double reach) const {
        const Probe& root = probes_.at(grid::code(key_));
        return std::abs(root.distance) <= (half_diagonal * root.size) + reach;
    }

private:
    [[nodiscard]] grid::Key absolute(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                     std::uint32_t z) const noexcept {
        return {.depth = static_cast<std::uint8_t>(key_.depth + depth),
                .x = (key_.x << depth) + x,
                .y = (key_.y << depth) + y,
                .z = (key_.z << depth) + z};
    }

    [[nodiscard]] bool open(std::uint8_t depth, const Probe& here) const noexcept {
        return key_.depth + depth < grid_->depth() &&
               (std::abs(here.distance) < near * here.size || here.caves);
    }

    // Each depth's nodes down from the filled one, probed together: those the surface may cross
    // get their planes, and go on down above the foot.
    void survey() {
        std::vector<grid::Key> nodes{key_};
        for (std::uint8_t depth = 0; depth <= levels_ && !nodes.empty(); ++depth) {
            const auto at = static_cast<std::uint8_t>(key_.depth + depth);
            const double size = grid_->size(at);
            // Columns on cells about as wide as the nodes, detail no finer than them.
            const int level = std::max(generator_->layers().level - (grid_->depth() - at), 0);
            const double lod = std::max(size, voxel_lod(*generator_));
            std::vector<DVec3> middles;
            middles.reserve(nodes.size());
            for (const grid::Key node : nodes) {
                middles.push_back(grid_->centre(node));
            }
            const std::vector<double> distances = distances_at(middles, level, lod);
            std::vector<grid::Key> next;
            std::vector<std::size_t> crossing;
            for (std::size_t n = 0; n < nodes.size(); ++n) {
                Probe probe = probe_at(middles[n], distances[n], size, level);
                if (missed(probe, middles[n], level)) {
                    probe.surface =
                        probe.distance < 0.0 ? grid::Surface::solid() : grid::Surface::open();
                } else {
                    crossing.push_back(n);
                    if (depth < levels_) {
                        probe.splits = true;
                        for (std::size_t octant = 0; octant < 8; ++octant) {
                            next.push_back(nodes[n].child(octant));
                        }
                    }
                }
                probes_.emplace(grid::code(nodes[n]), probe);
            }
            if (!crossing.empty()) {
                planes(nodes, middles, crossing, size, level, lod);
            }
            nodes = std::move(next);
        }
    }

    // What the generator gives at a node's middle `distance` out of the solid, but its surface.
    [[nodiscard]] Probe probe_at(DVec3 middle, double distance, double size, int level) const {
        const double radius = glm::length(middle);
        const Generator::Column& column = column_at(middle, level);
        return {.distance = distance,
                .material =
                    distance > clear * size ? Material::Air : solid_at(middle, radius, level, size),
                .size = size,
                .radius = radius,
                .ground = column.ground(),
                .water = highest_water(column),
                .body = column.water_body(),
                .surface = std::nullopt};
    }

    // Whether no surface crosses the node: the ground's far from it, and in the solid, no cave
    // shown at its detail lies in it. One that may show finer is noted.
    bool missed(Probe& probe, DVec3 middle, int level) const {
        const double reach = clear * probe.size;
        if (std::abs(probe.distance) <= reach) {
            return false;
        }
        if (probe.distance > 0.0) {
            return true;
        }
        const Generator::Column& column = column_at(middle, level);
        if (!column.caves(probe.radius - reach, probe.radius + reach, reach).empty()) {
            return false;
        }
        const double half = half_diagonal * probe.size;
        probe.caves = column.caves_may_lie(probe.radius - half, probe.radius + half);
        return true;
    }

    // The planes of the nodes the surface may cross: each one's normal the way the distance rises
    // across it.
    void planes(const std::vector<grid::Key>& nodes, const std::vector<DVec3>& middles,
                const std::vector<std::size_t>& crossing, double size, int level, double lod) {
        constexpr std::array<DVec3, 3> axes{DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0},
                                            DVec3{0.0, 0.0, 1.0}};
        const double step = size / 2.0;
        std::vector<DVec3> points;
        points.reserve(crossing.size() * 6);
        for (const std::size_t n : crossing) {
            for (const DVec3 axis : axes) {
                points.push_back(middles[n] + (axis * step));
                points.push_back(middles[n] - (axis * step));
            }
        }
        const std::vector<double> distances = distances_at(points, level, lod);
        for (std::size_t f = 0; f < crossing.size(); ++f) {
            const std::size_t n = crossing[f];
            const std::size_t first = 6 * f;
            const DVec3 rise{distances[first] - distances[first + 1],
                             distances[first + 2] - distances[first + 3],
                             distances[first + 4] - distances[first + 5]};
            Probe& probe = probes_.at(grid::code(nodes[n]));
            const double length = glm::length(rise);
            const DVec3 normal = length > 0.0 ? rise / length : middles[n] / probe.radius;
            // It shows the solid just inside the surface, along its normal from the middle.
            const DVec3 inside = middles[n] - (normal * (probe.distance + (size / 4.0)));
            const Material shown = solid_at(inside, glm::length(inside), level, size);
            probe.surface = grid::Surface::plane(normal, probe.distance / size, palette_of(shown));
            if (probe.surface->is_open()) {
                probe.material = Material::Air;
                continue;
            }
            // Digging gives what most of it holds, as the nodes under it would sum up: the solid
            // at its middle and its faces' middles, with what it shows.
            std::array<grid::Material::Value, 8> held{palette_of(shown),
                                                      palette_of(solid_in(middles[n], level))};
            for (std::size_t k = 0; k < 6; ++k) {
                held.at(2 + k) = palette_of(solid_in(points[first + k], level));
            }
            probe.material = static_cast<Material>(grid::Material::summary(held));
        }
    }

    // The solid at a point as the nearest column made has it; air where it's open.
    [[nodiscard]] Material solid_in(DVec3 point, int level) const {
        const double radius = glm::length(point);
        const auto found = columns_.find(slot(cell_under(up_at(point), level)));
        if (found == columns_.end()) {
            return Material::Air;
        }
        const Sample sample = found->second.sample(radius);
        return sample.distance < 0.0 ? sample.material : Material::Air;
    }

    // The distance at each point, between the columns about it.
    std::vector<double> distances_at(const std::vector<DVec3>& points, int level, double lod) {
        std::vector<Between> between;
        between.reserve(points.size());
        std::vector<Cell> wanted;
        wanted.reserve(points.size() * 4);
        for (const DVec3 point : points) {
            between.push_back(around(point, level));
            wanted.insert(wanted.end(), between.back().cells.begin(), between.back().cells.end());
        }
        make_columns(wanted, lod);
        std::vector<double> distances;
        distances.reserve(points.size());
        for (std::size_t n = 0; n < points.size(); ++n) {
            const double radius = glm::length(points[n]);
            double distance = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                distance += between[n].weights.at(k) *
                            columns_.at(slot(between[n].cells.at(k))).sample(radius).distance;
            }
            distances.push_back(distance);
        }
        return distances;
    }

    // The solid at or just under a point, as the nearest column made has it: where the column
    // has the point open, the solid it reaches going down. Air only where it reaches none.
    [[nodiscard]] Material solid_at(DVec3 point, double radius, int level, double size) const {
        const auto found = columns_.find(slot(cell_under(up_at(point), level)));
        if (found == columns_.end()) {
            return Material::Air;
        }
        Sample sample = found->second.sample(radius);
        if (sample.distance >= 0.0) {
            sample = found->second.sample(radius - sample.distance - (size / 4.0));
        }
        return sample.distance < 0.0 ? sample.material : Material::Air;
    }

    // The column made nearest a point, among those about it.
    [[nodiscard]] const Generator::Column& column_at(DVec3 point, int level) const {
        return columns_.at(slot(cell_under(up_at(point), level)));
    }

    static Between around(DVec3 point, int level) {
        const CellPlace place = place_under(up_at(point), level);
        const int di = place.i < 0.0 ? -1 : 1;
        const int dj = place.j < 0.0 ? -1 : 1;
        const Cell across = next_to(place.cell, di, 0).value_or(place.cell);
        const Cell along = next_to(place.cell, 0, dj).value_or(place.cell);
        const Cell corner = next_to(across, 0, dj).value_or(across);
        const double wi = std::abs(place.i);
        const double wj = std::abs(place.j);
        return {.cells = {place.cell, across, along, corner},
                .weights = {(1.0 - wi) * (1.0 - wj), wi * (1.0 - wj), (1.0 - wi) * wj, wi * wj}};
    }

    static std::uint64_t slot(Cell cell) { return (index_of(cell) << 5U) | cell.level; }

    void make_columns(std::vector<Cell>& wanted, double lod) {
        std::erase_if(wanted, [&](Cell cell) { return columns_.contains(slot(cell)); });
        std::ranges::sort(wanted, {}, slot);
        const auto repeated = std::ranges::unique(wanted, {}, slot);
        wanted.erase(repeated.begin(), repeated.end());
        if (wanted.empty()) {
            return;
        }
        std::vector<Generator::Column> made = generator_->columns(wanted, lod, 1, false);
        for (std::size_t n = 0; n < wanted.size(); ++n) {
            columns_.emplace(slot(wanted[n]), made[n]);
        }
    }

    const Generator* generator_;
    const grid::Grid* grid_;
    grid::Key key_;
    std::uint8_t levels_;
    std::unordered_map<std::uint64_t, Probe> probes_;
    std::unordered_map<std::uint64_t, Generator::Column> columns_;
};

} // namespace

grid::Grid grid_of(const Generator& generator) {
    return {generator.layers().ceiling(), generator.layers().thickness};
}

Filled fill(const Generator& generator, const grid::Grid& grid, grid::Key key,
            std::uint8_t levels) {
    HOG_ASSERT(key.depth + levels <= grid.depth(), "filled no deeper than the grid's finest");
    const Filler filler{generator, grid, key, levels};
    auto shape = grid::Tree<grid::Shape>::grow(
        levels, [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return filler.shape(depth, x, y, z);
        });
    auto material = grid::Tree<grid::Material>::grow(
        levels, [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return filler.material(depth, x, y, z);
        });
    auto water = grid::Tree<grid::Water>::grow(
        levels, [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return filler.water(depth, x, y, z);
        });
    auto woods = grid::Tree<grid::Woods>::grow(
        levels, [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return filler.woods(depth, x, y, z);
        });
    // What the shape leaves out, its middle in the node, within this far along the ground.
    const DVec3 middle = grid.centre(key);
    const double reach = glm::length(middle) > 0.0
                             ? half_diagonal * grid.size(key.depth) *
                                   generator.planet().body.radius / glm::length(middle)
                             : std::numeric_limits<double>::infinity();
    std::vector<Placed> placed;
    // Rocks and crags, as the finest detail filled lays them.
    if (filler.near_ground(terms::rock_cell)) {
        const double lod = std::max(grid.size(static_cast<std::uint8_t>(key.depth + levels)),
                                    voxel_lod(generator));
        for (const Rock& rock : generator.rocks_within(up_at(middle), reach, lod)) {
            if (grid.key_at(rock.middle, key.depth) == key) {
                placed.push_back(placed_rock(rock));
            }
        }
        // Crags as the finest columns made stand them.
        for (const Crag& crag : Generator::crags_from(filler.standing(), lod)) {
            if (grid.key_at(crag.middle, key.depth) == key) {
                placed.push_back(placed_crag(crag));
            }
        }
    }
    // Vents, and pools on cavern floors however deep under the ground.
    for (const VentSite& site : vent_sites_within(generator, up_at(middle), reach)) {
        if (grid.key_at(site.direction * site.ground, key.depth) == key) {
            placed.push_back(placed_vent(site));
        }
    }
    std::ranges::sort(placed, {}, &Placed::key);
    return {.shape = std::move(shape),
            .material = std::move(material),
            .water = std::move(water),
            .woods = std::move(woods),
            .placed = std::move(placed)};
}

} // namespace hog::globe
