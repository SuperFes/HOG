#include "column_reads.hpp"
#include "terms.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/globe/habitat.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
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
    double rest = 0.0;   // metres the ground's detail too fine for the node could move it
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
    Filler(const Generator& generator, const grid::Grid& grid, grid::Key key, std::uint8_t levels,
           Layered layered)
        : generator_{&generator}, grid_{&grid}, key_{key}, levels_{levels}, layered_{layered},
          base_{base_of(grid, layered)},
          seed_{hash_values(generator.planet().body.seed,
                            static_cast<std::uint64_t>(terms::Term::Layers))} {
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
    // down to nodes grid::Woods::finest across.
    grid::Tree<grid::Woods>::Decided woods(std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z) const {
        const grid::Key key = absolute(depth, x, y, z);
        const Probe& here = probes_.at(grid::code(key));
        // Open by size alone, ground or none, so stands merge across the air beside them.
        const bool finer = here.size / 2.0 >= grid::Woods::finest && key.depth < grid_->depth();
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
        const int level = read_level(static_cast<std::uint8_t>(key_.depth + levels_));
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

    // The level of the cells about as wide as nodes at depth `at`.
    [[nodiscard]] int level_at(std::uint8_t at) const noexcept {
        return std::max(generator_->layers().level - (grid_->depth() - at), 0);
    }

    // The level of the cells whose columns nodes at depth `at` are read between: their own, or
    // under the layers' base, the base's.
    [[nodiscard]] int read_level(std::uint8_t at) const noexcept {
        return level_at(std::min(at, base_));
    }

    // The deepest depth whose nodes are read from columns of their own size.
    static std::uint8_t base_of(const grid::Grid& grid, Layered layered) noexcept {
        std::uint8_t depth = grid.depth();
        while (layered.below > 0.0 && depth > 0 && grid.size(depth) < layered.below) {
            --depth;
        }
        return depth;
    }

    // The seeded layers: an octave for each size under the base's.
    [[nodiscard]] Octaves layers() const noexcept {
        return {.wavelength = grid_->size(base_), .gain = 0.5, .finest = 0.0};
    }

    // Metres the seeded layers raise the ground at `point`, read with detail `detail`, where the
    // columns say detail finer than theirs could move it `rest`.
    [[nodiscard]] double raised(DVec3 point, double rest, double detail) const noexcept {
        if (rest <= 0.0) {
            return 0.0;
        }
        const DVec3 at = up_at(point) * generator_->planet().body.radius;
        return layered_.strength * rest * fbm(at, layers(), detail, seed_).value /
               fbm_rest(layers(), grid_->size(base_));
    }

    // How far the layers finer than `detail` could move it, likewise.
    [[nodiscard]] double left(double rest, double detail) const noexcept {
        return layered_.strength * rest * fbm_rest(layers(), detail) /
               fbm_rest(layers(), grid_->size(base_));
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
            const std::vector<DVec3> middles = middles_of(nodes);
            const Read read = distances_at(middles, at);
            std::vector<grid::Key> next;
            std::vector<std::size_t> crossing;
            for (std::size_t n = 0; n < nodes.size(); ++n) {
                Probe probe = probe_at(middles[n], read.distances[n], size, read.under[n]);
                probe.rest = read.rests[n];
                if (missed(probe, read.under[n], read.spreads[n])) {
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
                planes(nodes, middles, read.under, crossing, at);
            }
            nodes = std::move(next);
        }
    }

    [[nodiscard]] std::vector<DVec3> middles_of(const std::vector<grid::Key>& nodes) const {
        std::vector<DVec3> middles;
        middles.reserve(nodes.size());
        for (const grid::Key node : nodes) {
            middles.push_back(grid_->centre(node));
        }
        return middles;
    }

    // What the generator gives at a node's middle `distance` out of the solid, but its surface.
    [[nodiscard]] Probe probe_at(DVec3 middle, double distance, double size, Cell under) const {
        const double radius = glm::length(middle);
        const Generator::Column& column = columns_.at(slot(under));
        return {.distance = distance,
                .material = distance > clear * size ? Material::Air : solid_at(radius, under, size),
                .size = size,
                .radius = radius,
                .ground = column.ground(),
                .water = highest_water(column),
                .body = column.water_body(),
                .surface = std::nullopt};
    }

    // Whether no surface crosses the node: the ground's far from it, past how far the columns it's
    // read between stray, and in the solid, no cave shown at its detail lies in it. One that may
    // show finer is noted.
    bool missed(Probe& probe, Cell under, double spread) const {
        if (!clear_of_surface({.distance = probe.distance, .spread = spread}, probe.size, clear)) {
            return false;
        }
        const double reach = (clear * probe.size) + spread;
        if (probe.distance > 0.0) {
            return true;
        }
        const Generator::Column& column = columns_.at(slot(under));
        if (!column.caves(probe.radius - reach, probe.radius + reach, reach).empty()) {
            return false;
        }
        const double half = half_diagonal * probe.size;
        probe.caves = column.caves_may_lie(probe.radius - half, probe.radius + half, half);
        return true;
    }

    // The planes of the nodes the surface may cross: each one's normal the way the distance rises
    // across it.
    void planes(const std::vector<grid::Key>& nodes, const std::vector<DVec3>& middles,
                const std::vector<Cell>& under, const std::vector<std::size_t>& crossing,
                std::uint8_t at) {
        constexpr std::array<DVec3, 3> axes{DVec3{1.0, 0.0, 0.0}, DVec3{0.0, 1.0, 0.0},
                                            DVec3{0.0, 0.0, 1.0}};
        const double size = grid_->size(at);
        const double step = size / 2.0;
        std::vector<DVec3> points;
        points.reserve(crossing.size() * 6);
        for (const std::size_t n : crossing) {
            for (const DVec3 axis : axes) {
                points.push_back(middles[n] + (axis * step));
                points.push_back(middles[n] - (axis * step));
            }
        }
        const Read read = distances_at(points, at);
        const std::vector<double>& distances = read.distances;
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
            const Material shown =
                solid_at(glm::length(inside), cell_under(up_at(inside), under[n].level), size);
            // As rough as the ground strays from it: its middle off the line through each pair
            // of faces, and what's too fine for the read.
            double bend = 0.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double faces =
                    (distances[first + (2 * axis)] + distances[first + (2 * axis) + 1]) / 2.0;
                bend = std::max(bend, std::abs(faces - probe.distance));
            }
            probe.surface = grid::Surface::plane(normal, probe.distance / size, palette_of(shown))
                                .with_roughness((bend + probe.rest) / size);
            if (probe.surface->is_open()) {
                probe.material = Material::Air;
                continue;
            }
            // Digging gives what most of it holds, as the nodes under it would sum up: the solid
            // at its middle and its faces' middles, with what it shows.
            std::array<grid::Material::Value, 8> held{palette_of(shown),
                                                      palette_of(solid_in(middles[n], under[n]))};
            for (std::size_t k = 0; k < 6; ++k) {
                held.at(2 + k) = palette_of(solid_in(points[first + k], read.under[first + k]));
            }
            probe.material = static_cast<Material>(grid::Material::summary(held));
        }
    }

    // The solid at a point, `under` it, as that cell's column has it; air where it's open.
    [[nodiscard]] Material solid_in(DVec3 point, Cell under) const {
        const double radius = glm::length(point);
        const auto found = columns_.find(slot(under));
        if (found == columns_.end()) {
            return Material::Air;
        }
        const Sample sample = found->second.sample(radius);
        return sample.distance < 0.0 ? sample.material : Material::Air;
    }

    // The distance at each point read for nodes at depth `at`, between the columns about it, the
    // cell each lies over and how far the ground's detail too fine for them could move it. Under
    // the layers' base, the seeded layers down to the nodes' detail raise the ground, and what's
    // finer is the layers'; where features too narrow for the base reach, the generator's own.
    struct Read {
        std::vector<double> distances;
        std::vector<Cell> under;
        std::vector<double> rests;
        // How far the columns it's read between stray from it, as ColumnRead has it.
        std::vector<double> spreads;
    };
    Read distances_at(const std::vector<DVec3>& points, std::uint8_t at) {
        const int level = read_level(at);
        const double lod = terms::cell_lod(generator_->planet(), level);
        const double detail =
            at > base_ ? terms::cell_lod(generator_->planet(), level_at(at)) : 0.0;
        std::vector<std::size_t> unique;
        std::vector<double> shares;
        std::vector<Between> between;
        between.reserve(points.size());
        std::vector<Smoothly> smoothly;
        std::vector<Cell> wanted;
        wanted.reserve(points.size() * (detail > 0.0 ? 20 : 4));
        for (const DVec3 point : points) {
            between.push_back(around(point, level));
            wanted.insert(wanted.end(), between.back().cells.begin(), between.back().cells.end());
            if (detail > 0.0) {
                smoothly.push_back(smoothly_between(point, level));
                wanted.insert(wanted.end(), smoothly.back().cells.begin(),
                              smoothly.back().cells.end());
            }
        }
        make_columns(wanted, lod);
        Read read;
        read.distances.reserve(points.size());
        read.under.reserve(points.size());
        read.rests.reserve(points.size());
        read.spreads.reserve(points.size());
        for (std::size_t n = 0; n < points.size(); ++n) {
            const double radius = glm::length(points[n]);
            double rest = 0.0;
            double slope = 0.0;
            double ground = 0.0;
            std::array<double, 4> each{};
            for (std::size_t k = 0; k < 4; ++k) {
                const Generator::Column& column = columns_.at(slot(between[n].cells.at(k)));
                const double weight = between[n].weights.at(k);
                each.at(k) = column.sample(radius).distance;
                rest += weight * column.rest();
                slope += weight * column.slope();
                ground += weight * column.ground();
            }
            const ColumnRead columns = read_between(each, between[n].weights);
            double distance = columns.distance;
            if (detail > 0.0) {
                // Many nodes to a cell, the ground's taken smoothly between them, not in facets a
                // cell across, and raised by the layers; raised, it comes nearer across its slope.
                double smooth = 0.0;
                for (std::size_t k = 0; k < 16; ++k) {
                    smooth += smoothly[n].weights.at(k) *
                              columns_.at(slot(smoothly[n].cells.at(k))).ground();
                }
                distance -= (smooth - ground + raised(points[n], rest, detail)) /
                            std::sqrt(1.0 + (slope * slope));
                rest = left(rest, detail);
                if (layered_.unique) {
                    if (const double share =
                            unique_share(between[n], radius, clear * grid_->size(at));
                        share > 0.0) {
                        unique.push_back(n);
                        shares.push_back(share);
                    }
                }
            }
            read.distances.push_back(distance);
            read.under.push_back(between[n].cells[0]);
            read.rests.push_back(rest);
            read.spreads.push_back(columns.spread);
        }
        if (!unique.empty()) {
            own_reads(points, unique, shares, level_at(at), detail, read);
        }
        return read;
    }

    // How much of the generator's own read a point under the layers' base takes, `radius` from
    // the centre between the base columns `about` it: all where a cave may lie within `reach`,
    // else as much as those columns ask.
    [[nodiscard]] double unique_share(const Between& about, double radius, double reach) const {
        if (columns_.at(slot(about.cells[0]))
                .caves_may_lie(radius - reach, radius + reach, reach)) {
            return 1.0;
        }
        double share = 0.0;
        for (std::size_t k = 0; k < 4; ++k) {
            share += about.weights.at(k) * unique_in(columns_.at(slot(about.cells.at(k))));
        }
        return share;
    }

    // How much of the generator's own read a base column asks for: all where a crag stands or a
    // cave may open near its ground, where a river's channel may wind or open water meets its
    // ground, fading out a base's size past them.
    [[nodiscard]] double unique_in(const Generator::Column& column) const {
        const double margin = grid_->size(base_);
        const double ground = column.ground();
        if (column.crag() || column.caves_may_lie(ground - margin, ground + margin, margin)) {
            return 1.0;
        }
        const auto fading = [margin](double away) {
            const double out = std::clamp(away / margin, 0.0, 1.0);
            return 1.0 - (out * out * (3.0 - (2.0 * out)));
        };
        double share = fading(column.channel_away());
        if (column.water() > 0.0) {
            const double off =
                std::max({column.water() - ground, ground - highest_water(column), 0.0});
            share = std::max(share, fading(off));
        }
        return share;
    }

    // The generator's own reads at `level`, with detail `lod`, at the points `which`, each taken
    // by its share over what's read.
    void own_reads(const std::vector<DVec3>& points, const std::vector<std::size_t>& which,
                   const std::vector<double>& shares, int level, double lod, Read& read) {
        std::vector<Between> between;
        between.reserve(which.size());
        std::vector<Cell> wanted;
        wanted.reserve(which.size() * 4);
        for (const std::size_t n : which) {
            between.push_back(around(points[n], level));
            wanted.insert(wanted.end(), between.back().cells.begin(), between.back().cells.end());
        }
        make_columns(wanted, lod);
        for (std::size_t f = 0; f < which.size(); ++f) {
            const std::size_t n = which[f];
            const double radius = glm::length(points[n]);
            double rest = 0.0;
            std::array<double, 4> each{};
            for (std::size_t k = 0; k < 4; ++k) {
                const Generator::Column& column = columns_.at(slot(between[f].cells.at(k)));
                each.at(k) = column.sample(radius).distance;
                rest += between[f].weights.at(k) * column.rest();
            }
            const ColumnRead columns = read_between(each, between[f].weights);
            const double share = shares[f];
            read.distances[n] = ((1.0 - share) * read.distances[n]) + (share * columns.distance);
            read.rests[n] = ((1.0 - share) * read.rests[n]) + (share * rest);
            read.spreads[n] = ((1.0 - share) * read.spreads[n]) + (share * columns.spread);
            read.under[n] = between[f].cells[0];
        }
    }

    // The solid at or just under `radius` in the column of the cell `under` it: where the column
    // has it open, the solid it reaches going down. Air only where it reaches none.
    [[nodiscard]] Material solid_at(double radius, Cell under, double size) const {
        const auto found = columns_.find(slot(under));
        if (found == columns_.end()) {
            return Material::Air;
        }
        Sample sample = found->second.sample(radius);
        if (sample.distance >= 0.0) {
            sample = found->second.sample(radius - sample.distance - (size / 4.0));
        }
        return sample.distance < 0.0 ? sample.material : Material::Air;
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

    // The sixteen cells about a point, four along each way, and how much each counts in a
    // Catmull-Rom spline through their columns: smooth across the cells, through each middle.
    struct Smoothly {
        std::array<Cell, 16> cells;
        std::array<double, 16> weights{};
    };
    static Smoothly smoothly_between(DVec3 point, int level) {
        const CellPlace place = place_under(up_at(point), level);
        const auto spline = [](double t) {
            const double t2 = t * t;
            const double t3 = t2 * t;
            return std::array{(-t3 + (2.0 * t2) - t) / 2.0, ((3.0 * t3) - (5.0 * t2) + 2.0) / 2.0,
                              ((-3.0 * t3) + (4.0 * t2) + t) / 2.0, (t3 - t2) / 2.0};
        };
        const int first_i = place.i < 0.0 ? -2 : -1;
        const int first_j = place.j < 0.0 ? -2 : -1;
        const auto along_i = spline(place.i < 0.0 ? 1.0 + place.i : place.i);
        const auto along_j = spline(place.j < 0.0 ? 1.0 + place.j : place.j);
        Smoothly found;
        for (std::size_t b = 0; b < 4; ++b) {
            for (std::size_t a = 0; a < 4; ++a) {
                const int di = first_i + static_cast<int>(a);
                const int dj = first_j + static_cast<int>(b);
                found.cells.at((4 * b) + a) =
                    next_to(place.cell, di, dj)
                        .value_or(next_to(place.cell, di, 0).value_or(place.cell));
                found.weights.at((4 * b) + a) = along_i.at(a) * along_j.at(b);
            }
        }
        return found;
    }

    static std::uint64_t slot(Cell cell) { return (index_of(cell) << 5U) | cell.level; }

    void make_columns(const std::vector<Cell>& wanted, double lod) {
        std::vector<std::pair<std::uint64_t, Cell>> missing;
        missing.reserve(wanted.size());
        for (const Cell cell : wanted) {
            if (const std::uint64_t at = slot(cell); !columns_.contains(at)) {
                missing.emplace_back(at, cell);
            }
        }
        std::ranges::sort(missing, {}, &std::pair<std::uint64_t, Cell>::first);
        const auto repeated =
            std::ranges::unique(missing, {}, &std::pair<std::uint64_t, Cell>::first);
        missing.erase(repeated.begin(), repeated.end());
        if (missing.empty()) {
            return;
        }
        std::vector<Cell> cells;
        cells.reserve(missing.size());
        for (const auto& [at, cell] : missing) {
            cells.push_back(cell);
        }
        std::vector<Generator::Column> made = generator_->columns(cells, lod, 1, false);
        for (std::size_t n = 0; n < missing.size(); ++n) {
            columns_.emplace(missing[n].first, std::move(made[n]));
        }
    }

    const Generator* generator_;
    const grid::Grid* grid_;
    grid::Key key_;
    std::uint8_t levels_;
    Layered layered_;
    std::uint8_t base_; // as base_of
    std::uint64_t seed_;
    std::unordered_map<std::uint64_t, Probe> probes_;
    std::unordered_map<std::uint64_t, Generator::Column> columns_;
};

} // namespace

grid::Grid grid_of(const Generator& generator) {
    return {generator.layers().ceiling(), generator.layers().thickness};
}

Filled fill(const Generator& generator, const grid::Grid& grid, grid::Key key, std::uint8_t levels,
            Layered layered) {
    HOG_ASSERT(key.depth + levels <= grid.depth(), "filled no deeper than the grid's finest");
    const Filler filler{generator, grid, key, levels, layered};
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
