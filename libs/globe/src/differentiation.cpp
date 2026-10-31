#include <hog/globe/differentiation.hpp>
#include <hog/globe/hex_grid.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

std::uint16_t warmed(std::uint16_t temperature, double by) {
    return static_cast<std::uint16_t>(std::clamp(temperature + by, 0.0, 65'535.0));
}

struct Sorter {
    Globe* globe = nullptr;
    const HexGrid* grid = nullptr;
    Gravity gravity;
    double volume = 0.0;

    // Sinks the hexel in `column` at `shell` into the lightest of those a shell down, in its own
    // column or one beside it, that bears its weight less than it presses. True if it sank.
    [[nodiscard]] bool sink(std::uint32_t column, int shell) const {
        Hexel& upper = globe->column_at(column)[static_cast<std::size_t>(shell)];
        if (upper.material == Material::Air) {
            return false;
        }
        const Shells& shells = globe->shells();
        const Properties& heavy = properties(upper.material);
        const double below = 0.5 * (shells.floor(shell - 1) + shells.ceiling(shell - 1));
        const double pull = gravity.at(below);
        Hexel* lightest = nullptr;
        double most = 0.0;
        const auto consider = [&](std::uint32_t under) {
            Hexel& lower = globe->column_at(under)[static_cast<std::size_t>(shell - 1)];
            if (lower.material == Material::Air) {
                return;
            }
            const double denser = heavy.density - properties(lower.material).density;
            if (denser > most && denser * pull * shells.thickness(shell) > strength_now(lower)) {
                most = denser;
                lightest = &lower;
            }
        };
        consider(column);
        for (const std::uint32_t next : grid->neighbors(column)) {
            consider(next);
        }
        if (lightest == nullptr) {
            return false;
        }
        // What the pair loses in falling, shared as heat by both.
        const Properties& light = properties(lightest->material);
        const double middle = 0.5 * (shells.floor(shell) + shells.ceiling(shell));
        const double energy = most * pull * (middle - below) * volume;
        const double holds = volume * ((heavy.density * heavy.heat_capacity) +
                                       (light.density * light.heat_capacity));
        std::swap(upper, *lightest);
        upper.temperature = warmed(upper.temperature, energy / holds);
        lightest->temperature = warmed(lightest->temperature, energy / holds);
        return true;
    }

    // One sweep over the globe, each column from the top down; returns how many sank.
    [[nodiscard]] std::uint64_t sweep() const {
        std::uint64_t sunk = 0;
        for (std::uint32_t column = 0; column < grid->size(); ++column) {
            for (int shell = globe->shells().count - 1; shell > 0; --shell) {
                sunk += sink(column, shell) ? 1 : 0;
            }
        }
        return sunk;
    }
};

// Molten rock lets go of all the water and gas it holds.
void degas(Globe& globe) {
    const double volume = globe.hexel_volume();
    for (std::uint64_t index = 0; index < cell_count(globe.level()); ++index) {
        for (Hexel& hexel : globe.column_at(index)) {
            const Properties& material = properties(hexel.material);
            if (hexel.held == 0 || hexel.temperature < material.melts) {
                continue;
            }
            const double rock = hexel.held / 255.0 * material.density * volume;
            globe.freed().water += rock * globe.bound().water;
            globe.freed().gas += rock * globe.bound().gas;
            hexel.held = 0;
        }
    }
}

} // namespace

std::uint64_t differentiate(Globe& globe, const Gravity& gravity) {
    const HexGrid grid{globe.level()};
    const Sorter sorter{
        .globe = &globe, .grid = &grid, .gravity = gravity, .volume = globe.hexel_volume()};
    std::uint64_t sunk = 0;
    // Each sinking warms what's round it, which may let more sink: sweep until nothing does.
    for (std::uint64_t now = 1; now > 0;) {
        now = sorter.sweep();
        degas(globe);
        sunk += now;
    }
    std::vector<HexCell> every;
    every.reserve(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        every.push_back(cell_at_index(globe.level(), index));
    }
    Settler{globe, grid, gravity}.settle(every);
    return sunk;
}

} // namespace hog::globe
