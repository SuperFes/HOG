#include "joints.hpp"
#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/core/portable_maths.hpp>
#include <hog/globe/cell.hpp>

#include <algorithm>
#include <array>
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

DVec3 side_of(const Rock& rock) noexcept {
    return glm::cross(rock.up, rock.across);
}

} // namespace

double Rock::distance(DVec3 point, DVec3& normal) const noexcept {
    const DVec3 side = side_of(*this);
    const DVec3 offset = point - middle;
    const DVec3 local{glm::dot(offset, across), glm::dot(offset, side), glm::dot(offset, up)};
    // A box `edge` smaller every way, grown by `edge` all round.
    const DVec3 beyond = glm::abs(local) - (axes - edge);
    const DVec3 out = glm::max(beyond, DVec3{0.0});
    const double outside = glm::length(out);
    const double inside = std::min(std::max({beyond.x, beyond.y, beyond.z}), 0.0);
    DVec3 way{0.0};
    if (outside > 0.0) {
        way = glm::sign(local) * out / outside;
    } else if (beyond.x >= beyond.y && beyond.x >= beyond.z) {
        way.x = local.x < 0.0 ? -1.0 : 1.0;
    } else if (beyond.y >= beyond.z) {
        way.y = local.y < 0.0 ? -1.0 : 1.0;
    } else {
        way.z = local.z < 0.0 ? -1.0 : 1.0;
    }
    normal = (way.x * across) + (way.y * side) + (way.z * up);
    return outside + inside - edge;
}

double Rock::top() const noexcept {
    const DVec3 out = glm::normalize(middle);
    return glm::length(middle) + (axes.x * std::abs(glm::dot(across, out))) +
           (axes.y * std::abs(glm::dot(side_of(*this), out))) +
           (axes.z * std::abs(glm::dot(up, out)));
}

double Rock::volume() const noexcept {
    // The box less what its rounded edges and corners leave out.
    const DVec3 inner = axes - edge;
    constexpr double quarter_round = 1.0 - (std::numbers::pi / 4.0);
    const double edges = 4.0 * quarter_round * edge * edge * 2.0 * (inner.x + inner.y + inner.z);
    const double corners =
        8.0 * ((edge * edge * edge) - (std::numbers::pi / 6.0 * edge * edge * edge));
    return (8.0 * axes.x * axes.y * axes.z) - edges - corners;
}

namespace terms {
namespace {

// Rock falling from a crag comes to rest no further out than where a line down from it at the
// shadow angle meets the ground, 27.5 degrees at the least (Evans and Hungr).
const double shadow = maths::tan(27.5 * std::numbers::pi / 180.0);
// The way up from a cell is walked in steps a coarsest cell long, this many at most.
constexpr int most_steps = 16;
// The share of a block's height that lies in the ground.
constexpr double sunk = 0.2;
// The share of its narrowest half-width its edges are knocked off by in the fall.
constexpr double knocked = 0.2;
// A block lies in the middle of its cell, no wider corner to corner than this share of the
// cell's narrowest side; one wider breaks in the fall.
constexpr double fits = 0.5;

double unit(std::uint64_t seed, std::uint64_t k) noexcept {
    return static_cast<double>(hash_values(seed, k) >> 11U) * 0x1.0p-53;
}

// A cell's narrowest side in metres, which cube edges and corners squeeze.
double width_of(const Planet& planet, Cell cell) noexcept {
    const std::array<DVec3, 4> corner = corners(cell);
    double width = 1e30;
    for (std::size_t k = 0; k < corner.size(); ++k) {
        width = std::min(width, glm::length(corner[(k + 1) % corner.size()] - corner[k]) *
                                    planet.body.radius);
    }
    return width;
}

Cell coarsest_of(const Planet& planet, Cell cell) noexcept {
    const int level = rock_level(planet);
    const auto shift = static_cast<unsigned>(cell.level - level);
    return {.level = static_cast<std::uint8_t>(level),
            .face = cell.face,
            .i = cell.i >> shift,
            .j = cell.j >> shift};
}

// How long a block `size` wide lasts lying on `ground`: until it breaks down there as bare rock
// does, or is carried off as the ground under it wears, or for all the years the ground has had.
double lasting(const Ground& ground, double size) noexcept {
    double years = ground.years;
    for (const double rate : {ground.weathering, ground.worn}) {
        if (rate > 0.0) {
            years = std::min(years, size / rate);
        }
    }
    return years;
}

// The share of the ground about `direction` covered by blocks coming loose from the rock at
// `source`, `spread` metres along the ground under each metre across that it comes loose over.
double covered(const Ground& here, const Ground& source, double spread) noexcept {
    const double size = joint_spacing(source.strength);
    return source.loosening * source.bare / spread * lasting(here, size) / size;
}

// What lies fallen about a coarsest cell, and the rock most of it fell from.
struct Fallen {
    double cover = 0.0;
    Ground source;
    DVec3 from{0.0};
};

Fallen fallen_about(const Context& context, Cell coarsest) noexcept {
    const Planet& planet = *context.planet;
    const double step = width_of(planet, coarsest);
    // Read as coarsely as the step, so the way up isn't turned by every crag and gully.
    const double lod = step / 2.0;
    DVec3 at = center(coarsest);
    const Ground here = ground(context, at, lod);
    Fallen fallen;
    if (glm::length(here.rise) > maths::tan(properties(Material::Gravel).repose)) {
        return fallen; // it rolls on down
    }
    // What comes loose here stays here.
    double most = covered(here, here, 1.0);
    fallen = {.cover = most, .source = here, .from = at};
    Ground last = here;
    for (int k = 1; k <= most_steps; ++k) {
        const double climbs = glm::length(last.rise);
        if (climbs <= 0.0) {
            break;
        }
        at = glm::normalize(at + (last.rise / climbs * (step / planet.body.radius)));
        const Ground next = ground(context, at, lod);
        if (next.height <= last.height) {
            break; // over the crest
        }
        last = next;
        const double drop = next.height - here.height;
        // Rock falling from there spreads over the ground the shadow line runs down over.
        if (drop < shadow * k * step) {
            continue;
        }
        const double share = covered(here, next, drop / shadow) * step;
        fallen.cover += share;
        if (share > most) {
            most = share;
            fallen.source = next;
            fallen.from = at;
        }
    }
    fallen.cover = std::min(fallen.cover, 1.0);
    return fallen;
}

// The block in `cell` if its draw places one, ignoring the cells about it; `fallen` is what lies
// about its coarsest cell.
std::optional<Rock> drawn_in(const Context& context, Cell cell, const Fallen& fallen,
                             double lod) noexcept {
    const Planet& planet = *context.planet;
    if (fallen.cover <= 0.0) {
        return std::nullopt;
    }
    const std::uint64_t seed =
        hash_values(context.seed(Term::Rockfall), cell.level, index_of(cell));
    // A block of the rock it fell from, from somewhere along that rock's joints.
    const DVec3 east = glm::normalize(glm::cross(
        std::abs(fallen.from.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0}, fallen.from));
    const DVec3 north = glm::cross(fallen.from, east);
    const DVec3 somewhere = (fallen.from * (planet.body.radius + fallen.source.height)) +
                            (east * ((unit(seed, 1) - 0.5) * 100.0)) +
                            (north * ((unit(seed, 2) - 0.5) * 100.0));
    const Block block = block_at(somewhere, fallen.source.strength, seed);
    std::array<double, 3> sides{block.size().x, block.size().y, block.size().z};
    std::ranges::sort(sides, std::greater{});
    const auto across = [&] { return std::hypot(sides[0], sides[1]); };
    // Too wide for the coarsest cells, it breaks across its long side.
    const double widest = fits * width_of(planet, coarsest_of(planet, cell));
    while (across() > widest) {
        sides[0] /= 2.0;
        std::ranges::sort(sides, std::greater{});
    }
    // It lies in the finest cells it fits.
    const double room = fits * width_of(planet, cell);
    const bool finest = cell.level == rock_level(planet) + rock_sizes - 1;
    if (across() > room || (!finest && across() <= room / 2.0)) {
        return std::nullopt;
    }
    if (sides[0] < lod) {
        return std::nullopt;
    }
    const double cell_width = width_of(planet, cell);
    if (unit(seed, 0) >= fallen.cover * cell_width * cell_width / (sides[0] * sides[1])) {
        return std::nullopt;
    }

    // Inside the middle of the cell, so the block never crosses into the next.
    const DVec3 middle_direction = center(cell);
    const DVec3 axis =
        std::abs(middle_direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 cell_east = glm::normalize(glm::cross(axis, middle_direction));
    const DVec3 cell_north = glm::cross(middle_direction, cell_east);
    const double slack = (cell_width - across()) / 2.0 / planet.body.radius;
    const DVec3 at =
        glm::normalize(middle_direction + (cell_east * ((unit(seed, 3) - 0.5) * slack)) +
                       (cell_north * ((unit(seed, 4) - 0.5) * slack)));
    const Ground found = ground(context, at, lod);
    Rock rock;
    rock.axes = {sides[0] / 2.0, sides[1] / 2.0, sides[2] / 2.0};
    rock.edge = knocked * rock.axes.z;
    // Lying on its broad face, square to the ground under it.
    rock.up = glm::normalize(at - found.rise);
    const double turn = 2.0 * std::numbers::pi * unit(seed, 5);
    const DVec3 level_east = glm::normalize(glm::cross(axis, rock.up));
    const DVec3 level_north = glm::cross(rock.up, level_east);
    rock.across = (maths::cos(turn) * level_east) + (maths::sin(turn) * level_north);
    const double radius = planet.body.radius + found.height;
    rock.middle = (at * radius) + (rock.up * ((1.0 - (2.0 * sunk)) * rock.axes.z));
    constexpr double depth = 4.0;
    rock.material = material_at(context, fallen.source.layers, depth,
                                planet.body.radius + fallen.source.height - depth);
    if (properties(rock.material).strength < 1.0e6) {
        rock.material = fallen.source.layers.basement;
    }
    return rock;
}

// Rocks found in each cell and what lies fallen about each coarsest one, kept for the cells
// beside and inside them.
class Rocks {
public:
    Rocks(const Context& context, double lod) : context_{&context}, lod_{lod} {}

    // The block in `cell`, none where a block in a cell holding it lies across it.
    std::optional<Rock> in(Cell cell) {
        const Planet& planet = *context_->planet;
        // From the coarsest cell holding it down, each block kept unless one before lies
        // across its cell.
        std::array<Cell, rock_sizes> holding{};
        std::size_t count = 0;
        for (Cell at = cell;; at = parent(at)) {
            holding.at(count++) = at;
            if (std::cmp_less_equal(at.level, rock_level(planet))) {
                break;
            }
        }
        std::array<Rock, rock_sizes> kept{};
        std::size_t lying = 0;
        std::optional<Rock> found;
        for (std::size_t k = count; k-- > 0;) {
            const Cell at = holding.at(k);
            found.reset();
            // No block too small for the detail asked fits a cell this size.
            if (fits * width_of(planet, at) < lod_) {
                break;
            }
            const bool covered =
                std::ranges::any_of(std::span{kept}.first(lying), [&](const Rock& rock) {
                    return glm::length(glm::normalize(rock.middle) - center(at)) *
                               planet.body.radius <
                           std::hypot(rock.axes.x, rock.axes.y) + (0.75 * width_of(planet, at));
                });
            if (covered) {
                continue;
            }
            found = drawn(at);
            if (found) {
                kept.at(lying++) = *found;
            }
        }
        return found;
    }

    // The block lying across the column under `direction`, if one does.
    std::optional<Rock> under(DVec3 direction) {
        const int coarsest = rock_level(*context_->planet);
        for (int level = coarsest; level < coarsest + rock_sizes; ++level) {
            const std::optional<Rock> rock = in(cell_under(direction, level));
            if (rock && glm::length(glm::normalize(rock->middle) - direction) *
                                context_->planet->body.radius <=
                            std::hypot(rock->axes.x, rock->axes.y)) {
                return rock;
            }
        }
        return std::nullopt;
    }

private:
    // The block `cell`'s draw places in it, whatever lies about it.
    const std::optional<Rock>& drawn(Cell cell) {
        const auto [kept, added] = rocks_.try_emplace(hash_values(cell.level, index_of(cell)));
        if (added) {
            kept->second =
                drawn_in(*context_, cell, fallen(coarsest_of(*context_->planet, cell)), lod_);
        }
        return kept->second;
    }

    const Fallen& fallen(Cell coarsest) {
        const auto [kept, added] = fallen_.try_emplace(index_of(coarsest));
        if (added) {
            kept->second = fallen_about(*context_, coarsest);
        }
        return kept->second;
    }

    const Context* context_;
    double lod_;
    std::unordered_map<std::uint64_t, std::optional<Rock>> rocks_;
    std::unordered_map<std::uint64_t, Fallen> fallen_;
};

} // namespace

int rock_level(const Planet& planet) noexcept {
    return std::max(0, static_cast<int>(std::ceil(
                           std::log2(std::numbers::pi / 2.0 * planet.body.radius / rock_cell))));
}

double fallen_cover(const Context& context, Cell coarsest) noexcept {
    return fallen_about(context, coarsest).cover;
}

std::optional<Rock> rock_in(const Context& context, Cell cell, double lod) {
    return Rocks{context, lod}.in(cell);
}

std::optional<Rock> rockfall(const Context& context, DVec3 direction, double lod) {
    return Rocks{context, lod}.under(direction);
}

std::vector<std::optional<Rock>> rockfall(const Context& context, std::span<const Cell> cells,
                                          double lod) {
    Rocks rocks{context, lod};
    std::vector<std::optional<Rock>> found;
    found.reserve(cells.size());
    for (const Cell cell : cells) {
        found.push_back(rocks.under(center(cell)));
    }
    return found;
}

} // namespace terms
} // namespace hog::globe
