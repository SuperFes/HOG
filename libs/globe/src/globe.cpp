#include <hog/core/assert.hpp>
#include <hog/globe/globe.hpp>

#include <cstddef>
#include <utility>

namespace hog::globe {

Globe::Globe(int level, Shells shells) : level_(level), shells_(shells) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    HOG_ASSERT(shells.count > 0 && shells.thickness > 0.0 && shells.inner >= 0.0,
               "shells must stand outward");
    hexels_.resize(cell_count(level) * static_cast<std::size_t>(shells.count));
}

std::span<Hexel> Globe::column(HexCell cell) {
    HOG_ASSERT(std::cmp_equal(cell.level, level_), "cell is not at the globe's level");
    const auto count = static_cast<std::size_t>(shells_.count);
    return std::span{hexels_}.subspan(index_of(cell) * count, count);
}

std::span<const Hexel> Globe::column(HexCell cell) const {
    HOG_ASSERT(std::cmp_equal(cell.level, level_), "cell is not at the globe's level");
    const auto count = static_cast<std::size_t>(shells_.count);
    return std::span{hexels_}.subspan(index_of(cell) * count, count);
}

Hexel& Globe::at(HexCell cell, int shell) {
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    return column(cell)[static_cast<std::size_t>(shell)];
}

const Hexel& Globe::at(HexCell cell, int shell) const {
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    return column(cell)[static_cast<std::size_t>(shell)];
}

std::array<double, material_count> Globe::masses() const {
    // Every hexagon's prism in a shell is the same; a pentagon's is five sixths of it.
    std::array<double, material_count> mass{};
    const HexCell hexagon = cell_at_index(level_, cell_count(level_) / 2);
    for (std::uint64_t index = 0; index < cell_count(level_); ++index) {
        const HexCell cell = cell_at_index(level_, index);
        const double share = is_pentagon(cell) ? 5.0 / 6.0 : 1.0;
        const auto hexels = column(cell);
        for (int shell = 0; shell < shells_.count; ++shell) {
            const Hexel& hexel = hexels[static_cast<std::size_t>(shell)];
            mass[static_cast<std::size_t>(hexel.material)] +=
                share * prism_volume(hexagon, shells_.floor(shell), shells_.ceiling(shell)) *
                properties(hexel.material).density;
        }
    }
    return mass;
}

} // namespace hog::globe
