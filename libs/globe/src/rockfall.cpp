#include "terms.hpp"

#include <hog/core/hash.hpp>
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

double Rock::distance(DVec3 point, DVec3& normal) const noexcept {
    const DVec3 side = glm::cross(up, across);
    const DVec3 offset = point - middle;
    const DVec3 local{glm::dot(offset, across), glm::dot(offset, side), glm::dot(offset, up)};
    const DVec3 scaled = local / axes;
    const DVec3 squared = local / (axes * axes);
    const double k0 = glm::length(scaled);
    const double k1 = glm::length(squared);
    if (k1 <= 0.0) {
        normal = up;
        return -std::min({axes.x, axes.y, axes.z});
    }
    const DVec3 out = glm::normalize(squared);
    normal = (out.x * across) + (out.y * side) + (out.z * up);
    return k0 * (k0 - 1.0) / k1;
}

double Rock::top() const noexcept {
    return glm::length(middle) + axes.z;
}

namespace terms {
namespace {

// The share of cells that hold a rock where the ground sheds the most.
constexpr double full_density = 0.35;
// Rock fallen from the cliffs: the ground sheds it where it climbs steeper than 17° over a
// stretch this wide, and at 35° sheds all it can.
constexpr double shedding_scale = 32.0;
constexpr double sheds_fully = 0.7;
// Half-lengths of the largest block, and the smallest the cells keep whole.
constexpr double largest = 4.5;
constexpr double smallest = 0.8;
// The share of a block's height that lies under the ground.
constexpr double sunk = 0.3;

double unit(std::uint64_t seed, std::uint64_t k) {
    return static_cast<double>(hash_values(seed, k) >> 11U) * 0x1.0p-53;
}

} // namespace

double steepness(const Context& context, DVec3 direction) noexcept {
    const DVec3 side = std::abs(direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side, direction));
    const DVec3 north = glm::cross(direction, east);
    const double turn = shedding_scale / context.planet->radius;
    const auto height = [&](DVec3 along, double by) {
        return ground(context, glm::normalize(direction + (along * by)), shedding_scale).height;
    };
    const double across_east = (height(east, turn) - height(east, -turn)) / (2.0 * shedding_scale);
    const double across_north =
        (height(north, turn) - height(north, -turn)) / (2.0 * shedding_scale);
    return std::sqrt((across_east * across_east) + (across_north * across_north));
}

int rock_level(const Planet& planet) noexcept {
    return std::max(0, static_cast<int>(std::ceil(
                           std::log2(std::numbers::pi / 2.0 * planet.radius / rock_cell))));
}

std::optional<Rock> rock_in(const Context& context, Cell cell, double lod) noexcept {
    const Planet& planet = *context.planet;
    const std::uint64_t seed = hash_values(context.seed(Term::Rockfall), index_of(cell));
    const double chance = unit(seed, 0);
    if (chance >= full_density) {
        return std::nullopt;
    }
    // Size is drawn small-heavy, between the smallest and largest block.
    const double draw = unit(seed, 1);
    // The cell's narrowest side, which cube edges and corners squeeze below `rock_cell`.
    const std::array<DVec3, 4> corner = corners(cell);
    double width = rock_cell;
    for (std::size_t k = 0; k < corner.size(); ++k) {
        width = std::min(width,
                         glm::length(corner[(k + 1) % corner.size()] - corner[k]) * planet.radius);
    }
    const double length =
        std::min(smallest * std::pow(largest / smallest, draw * draw), 0.2 * width);
    if (2.0 * length < lod) {
        return std::nullopt;
    }
    const DVec3 middle_direction = center(cell);
    const DVec3 axis =
        std::abs(middle_direction.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, middle_direction));
    const DVec3 north = glm::cross(middle_direction, east);
    // Inside the middle of the cell, so the block never crosses into the next.
    const double by_east = (unit(seed, 2) - 0.5) * 0.5 * width / planet.radius;
    const double by_north = (unit(seed, 3) - 0.5) * 0.5 * width / planet.radius;
    const DVec3 at = glm::normalize(middle_direction + (east * by_east) + (north * by_north));
    const double shed =
        smoothstep((steepness(context, at) - sheds_from) / (sheds_fully - sheds_from));
    if (chance >= full_density * shed) {
        return std::nullopt;
    }

    const Ground found = ground(context, at, lod);
    Rock rock;
    rock.axes = {length, length * (0.55 + (0.4 * unit(seed, 4))),
                 length * (0.45 + (0.35 * unit(seed, 5)))};
    rock.up = at;
    const double turn = 2.0 * std::numbers::pi * unit(seed, 6);
    const DVec3 side = std::abs(at.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 level_east = glm::normalize(glm::cross(side, at));
    const DVec3 level_north = glm::cross(at, level_east);
    rock.across = (std::cos(turn) * level_east) + (std::sin(turn) * level_north);
    const double radius = planet.radius + found.height - context.shape->datum;
    rock.middle = at * (radius + ((1.0 - (2.0 * sunk)) * rock.axes.z));
    constexpr double depth = 4.0;
    rock.material = material_at(context, found.layers, depth, radius - depth);
    if (properties(rock.material).strength < 1.0e6) {
        rock.material = found.layers.basement;
    }
    return rock;
}

namespace {

// The rock in `cell`, if it reaches the column under `direction`.
std::optional<Rock> reaching(const Context& context, const std::optional<Rock>& rock,
                             DVec3 direction) noexcept {
    if (rock && glm::length(rock->up - direction) * context.planet->radius > rock->axes.x) {
        return std::nullopt;
    }
    return rock;
}

} // namespace

std::optional<Rock> rockfall(const Context& context, DVec3 direction, double lod) noexcept {
    const Cell cell = cell_under(direction, rock_level(*context.planet));
    // Only a column the block reaches is worth the ground under it.
    const std::uint64_t seed = hash_values(context.seed(Term::Rockfall), index_of(cell));
    if (unit(seed, 0) >= full_density) {
        return std::nullopt;
    }
    return reaching(context, rock_in(context, cell, lod), direction);
}

std::vector<std::optional<Rock>> rockfall(const Context& context, std::span<const Cell> cells,
                                          double lod) {
    const int level = rock_level(*context.planet);
    std::unordered_map<std::uint64_t, std::optional<Rock>> made;
    std::vector<std::optional<Rock>> found;
    found.reserve(cells.size());
    for (const Cell cell : cells) {
        const DVec3 direction = center(cell);
        // A cell's middle lies well inside the coarser cell holding it.
        const auto shift = static_cast<unsigned>(cell.level - level);
        const Cell home = std::cmp_less_equal(level, cell.level)
                              ? Cell{.level = static_cast<std::uint8_t>(level),
                                     .face = cell.face,
                                     .i = cell.i >> shift,
                                     .j = cell.j >> shift}
                              : cell_under(direction, level);
        const auto [rock, added] = made.try_emplace(index_of(home));
        if (added) {
            rock->second = rock_in(context, home, lod);
        }
        found.push_back(reaching(context, rock->second, direction));
    }
    return found;
}

} // namespace terms
} // namespace hog::globe
