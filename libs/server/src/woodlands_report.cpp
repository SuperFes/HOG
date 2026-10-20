#include <hog/server/woodlands_report.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::server {

namespace {

// Sums weighted by what each cell under one that's sent holds.
struct Sums {
    double weight = 0.0;
    double living = 0.0;
    double established = 0.0;
    double failing = 0.0;
    double dead = 0.0;
    double charred = 0.0; // weighted by the dead too
};

} // namespace

protocol::Woodlands woodlands_report(const sim::Forest& forest) {
    const sim::CellGrid& grid = forest.grid();
    const sim::ForestFields& fields = forest.fields();
    const auto level = static_cast<std::uint8_t>(
        std::min(grid.level(), static_cast<int>(protocol::most_weather_level)));
    const std::optional<sim::CellGrid> coarse =
        level < grid.level() ? std::optional{sim::CellGrid{level}} : std::nullopt;

    std::vector<Sums> sums(protocol::weather_cells(level));
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const double weight = forest.capacity(index) * grid.area(index);
        if (weight <= 0.0) {
            continue;
        }
        Sums& sum = sums[coarse ? coarse->index(grid.cell(index).parent(level)) : index];
        const auto dead = static_cast<double>(fields.dead[index]);
        sum.weight += weight;
        sum.living += weight * forest.living(index);
        sum.established += weight * static_cast<double>(fields.established[index]);
        sum.failing += weight * static_cast<double>(fields.failing[index]);
        sum.dead += weight * dead;
        sum.charred += weight * dead * static_cast<double>(fields.charred[index]);
    }

    protocol::Woodlands woodlands{.level = level, .cells = {}};
    woodlands.cells.reserve(sums.size());
    for (const Sums& sum : sums) {
        if (sum.weight <= 0.0) {
            woodlands.cells.emplace_back();
            continue;
        }
        woodlands.cells.push_back(
            {.living = static_cast<float>(sum.living / sum.weight),
             .established = static_cast<float>(sum.established / sum.weight),
             .failing = static_cast<float>(sum.failing / sum.weight),
             .dead = static_cast<float>(sum.dead / sum.weight),
             .charred = sum.dead > 0.0 ? static_cast<float>(sum.charred / sum.dead) : 0.0F});
    }
    return woodlands;
}

} // namespace hog::server
