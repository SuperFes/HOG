#include <hog/core/assert.hpp>
#include <hog/globe/globe.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>

namespace hog::globe {

Shells shells_about(double inner, double outer, double radius, double thickness) {
    HOG_ASSERT(outer > inner && inner >= 0.0 && radius > 0.0 && thickness > 0.0,
               "shells must stand outward");
    const double volume = (outer * outer * outer) - (inner * inner * inner);
    const auto count = static_cast<int>(std::lround(volume / (3.0 * radius * radius * thickness)));
    return {.inner = inner, .outer = outer, .count = std::max(count, 1)};
}

Globe::Globe(int level, Shells shells) : level_(level), shells_(shells) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    HOG_ASSERT(shells.count > 0 && shells.outer > shells.inner && shells.inner >= 0.0,
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

std::span<Hexel> Globe::column_at(std::uint64_t index) {
    const auto count = static_cast<std::size_t>(shells_.count);
    return std::span{hexels_}.subspan(index * count, count);
}

std::span<const Hexel> Globe::column_at(std::uint64_t index) const {
    const auto count = static_cast<std::size_t>(shells_.count);
    return std::span{hexels_}.subspan(index * count, count);
}

Hexel& Globe::at(HexCell cell, int shell) {
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    return column(cell)[static_cast<std::size_t>(shell)];
}

const Hexel& Globe::at(HexCell cell, int shell) const {
    HOG_ASSERT(shell >= 0 && shell < shells_.count, "shell out of range");
    return column(cell)[static_cast<std::size_t>(shell)];
}

double Globe::hexel_volume() const noexcept {
    const double hexagon = 4.0 * std::numbers::pi / (10.0 * std::ldexp(1.0, 2 * level_));
    const double low = shells_.inner * shells_.inner * shells_.inner;
    const double high = shells_.outer * shells_.outer * shells_.outer;
    return hexagon * (high - low) / (3.0 * shells_.count);
}

std::array<double, material_count> Globe::masses() const {
    std::array<std::uint64_t, material_count> counted{};
    for (const Hexel& hexel : hexels_) {
        ++counted[static_cast<std::size_t>(hexel.material)];
    }
    std::array<double, material_count> mass{};
    for (std::size_t material = 0; material < mass.size(); ++material) {
        mass[material] = static_cast<double>(counted[material]) * hexel_volume() *
                         properties(static_cast<Material>(material)).density;
    }
    return mass;
}

} // namespace hog::globe
