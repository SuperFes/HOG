#include <hog/core/assert.hpp>
#include <hog/globe/hex_grid.hpp>

namespace hog::globe {

HexGrid::HexGrid(int level) : level_(level) {
    // Indices of a level fit 32 bits up to level 14, past any globe held whole.
    HOG_ASSERT(level >= 0 && level <= 14, "level out of range for a whole-globe grid");
    const auto count = static_cast<std::size_t>(cell_count(level));
    around_.resize(count);
    sides_.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        const HexNeighbors found = globe::neighbors(cell_at_index(level, index));
        sides_[index] = found.count;
        for (std::size_t k = 0; k < found.count; ++k) {
            around_[index][k] = static_cast<std::uint32_t>(index_of(found.cells[k]));
        }
    }
}

HexGrid::HexGrid(const Globe& globe) : level_(globe.level()) {
    if (globe.is_whole()) {
        *this = HexGrid{globe.level()};
        return;
    }
    const auto count = static_cast<std::size_t>(globe.size());
    around_.resize(count);
    sides_.resize(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        const HexNeighbors found = globe::neighbors(globe.cell(index));
        for (const HexCell next : found.all()) {
            if (globe.holds(next)) {
                around_[index][sides_[index]++] = static_cast<std::uint32_t>(globe.index_of(next));
            }
        }
    }
}

} // namespace hog::globe
