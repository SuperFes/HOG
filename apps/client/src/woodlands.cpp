#include <hog/client/woodlands.hpp>

#include <cstdint>
#include <set>
#include <utility>

namespace hog::client {

std::vector<planet::CellId> Woodlands::receive(const protocol::Woodlands& told) {
    std::vector<flora::Fate> cells;
    cells.reserve(told.cells.size());
    for (const protocol::CellWoodland& cell : told.cells) {
        cells.push_back({.living = cell.living,
                         .established = cell.established,
                         .failing = cell.failing,
                         .dead = cell.dead,
                         .charred = cell.charred});
    }
    auto fates = std::make_shared<const flora::Fates>(told.level, std::move(cells));
    const flora::Fates& was = *fates_;
    const std::uint32_t side = std::uint32_t{1} << told.level;
    std::set<planet::CellId> changed;
    // Told at another level, every cell changed; told first, those not left untouched.
    const bool relevelled = !was.untouched() && was.level() != told.level;
    const bool comparable = !was.untouched() && !relevelled;
    for (std::uint32_t index = 0; index < fates->size(); ++index) {
        const flora::Fate before = comparable ? was.cell(index) : flora::Fate{};
        if (!relevelled && fates->cell(index) == before) {
            continue;
        }
        const std::uint32_t face = index / (side * side);
        const std::uint32_t i = index % side;
        const std::uint32_t j = (index / side) % side;
        for (std::uint32_t near_j = j == 0 ? 0 : j - 1; near_j <= j + 1 && near_j < side;
             ++near_j) {
            for (std::uint32_t near_i = i == 0 ? 0 : i - 1; near_i <= i + 1 && near_i < side;
                 ++near_i) {
                changed.insert(planet::CellId::from_face_ij(static_cast<int>(face), near_i, near_j,
                                                            told.level));
            }
        }
    }
    fates_ = std::move(fates);
    return {changed.begin(), changed.end()};
}

} // namespace hog::client
