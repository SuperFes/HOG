#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/accretion.hpp>
#include <hog/globe/hex_cell.hpp>
#include <hog/globe/settling.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace hog::globe {
namespace {

// A number in [0, 1) from the seed and what it's for, the same everywhere.
double unit(std::uint64_t seed, std::uint64_t body, std::uint64_t purpose) {
    constexpr double scale = 0x1p-53;
    return static_cast<double>(hash_values(seed, body, purpose) >> 11U) * scale;
}

DVec3 direction(std::uint64_t seed, std::uint64_t body) {
    const double z = (2.0 * unit(seed, body, 0)) - 1.0;
    const double turn = 2.0 * std::numbers::pi * unit(seed, body, 1);
    const double across = std::sqrt(std::max(0.0, 1.0 - (z * z)));
    return {across * std::cos(turn), across * std::sin(turn), z};
}

// Columns whose centres lie within `reach` radians of `middle`, found outward from it.
std::vector<HexCell> footprint(DVec3 middle, double reach, int level) {
    const HexCell first = cell_at(middle, level);
    std::vector<HexCell> found{first};
    for (std::size_t at = 0; at < found.size(); ++at) {
        const HexNeighbors around = neighbors(found[at]);
        for (const HexCell next : around.all()) {
            if (std::ranges::find(found, next) != found.end()) {
                continue;
            }
            const DVec3 there = center(next);
            if (std::atan2(glm::length(glm::cross(there, middle)), glm::dot(there, middle)) <=
                reach) {
                found.push_back(next);
            }
        }
    }
    return found;
}

} // namespace

namespace {

// How far apart the columns stand at the surface.
double column_width(const Accretion& setup) {
    return setup.radius * std::atan(2.0) / std::ldexp(1.0, setup.level);
}

} // namespace

Shells accretion_shells(const Accretion& setup) {
    return shells_about(0.0, 1.25 * setup.radius, setup.radius, column_width(setup));
}

Globe accrete(const Accretion& setup) {
    HOG_ASSERT(setup.radius > 0.0 && setup.gravity > 0.0, "a planet needs size and pull");
    const Shells shells = accretion_shells(setup);
    Globe globe{setup.level, shells};
    const HexGrid grid{setup.level};
    Settler settler{globe, grid, {.surface = setup.gravity, .radius = setup.radius}};

    // Grown when the hexels laid fill the sphere of its radius.
    const double full = 4.0 / 3.0 * std::numbers::pi * setup.radius * setup.radius * setup.radius /
                        globe.hexel_volume();
    double laid = 0.0;

    const double width = column_width(setup);
    const double smallest = 0.5 * width / setup.radius;
    // By volume, the metal's share of what fell.
    const double iron_density = properties(Material::Iron).density;
    const double rock_density = properties(Material::Mantle).density;
    const double iron_volume = (setup.iron / iron_density) /
                               ((setup.iron / iron_density) + ((1.0 - setup.iron) / rock_density));

    for (std::uint64_t body = 0; laid < full; ++body) {
        const DVec3 middle = direction(setup.seed, body);
        const double reach =
            std::min(setup.largest, smallest * std::pow(1.0 - unit(setup.seed, body, 2),
                                                        -1.0 / setup.size_spread));
        const double deepest = std::max(1.0, 0.4 * reach * setup.radius / width);
        std::uint64_t piece = 0;
        const std::vector<HexCell> struck = footprint(middle, reach, setup.level);
        for (const HexCell cell : struck) {
            const DVec3 there = center(cell);
            const double off =
                std::atan2(glm::length(glm::cross(there, middle)), glm::dot(there, middle));
            const double share = reach > 0.0 ? 1.0 - ((off / reach) * (off / reach)) : 1.0;
            const int depth = std::max(1, static_cast<int>(std::lround(deepest * share)));
            int top = top_of(globe.column(cell));
            for (int layer = 0; layer < depth && top < shells.count; ++layer, ++top) {
                const double height = shells.ceiling(top);
                // The fall from far off onto a body of the planet's density grown this big.
                const double fall = setup.gravity * height * height / setup.radius;
                const double heat = setup.heat_kept * fall / setup.heat_capacity;
                const bool metal = unit(setup.seed, body, 3 + piece++) < iron_volume;
                const double temperature = setup.disc_temperature + setup.decay_heat + heat;
                globe.at(cell, top) = {.material = metal ? Material::Iron : Material::Mantle,
                                       .held = static_cast<std::uint8_t>(metal ? 0 : 255),
                                       .temperature = static_cast<std::uint16_t>(
                                           std::clamp(temperature, 0.0, 65'535.0))};
                laid += 1.0;
            }
        }
        // What piled higher than it can bear slumps away round where it fell.
        settler.settle(struck);
    }
    return globe;
}

} // namespace hog::globe
