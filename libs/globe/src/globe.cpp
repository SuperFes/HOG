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

Globe::Globe(int level, Shells shells, std::vector<HexCell> cells, int first_shell)
    : level_(level), shells_(shells), cells_(std::move(cells)), first_shell_(first_shell) {
    HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
    HOG_ASSERT(shells.count > 0 && shells.outer > shells.inner && shells.inner >= 0.0,
               "shells must stand outward");
    HOG_ASSERT(!cells_.empty() && first_shell >= 0, "a window needs columns");
    local_.reserve(cells_.size());
    for (std::size_t local = 0; local < cells_.size(); ++local) {
        HOG_ASSERT(std::cmp_equal(cells_[local].level, level), "cell is not at the globe's level");
        const bool added = local_.emplace(globe::index_of(cells_[local]), local).second;
        HOG_ASSERT(added, "a window holds a column once");
    }
    hexels_.resize(cells_.size() * static_cast<std::size_t>(shells.count));
}

std::uint64_t Globe::size() const noexcept {
    return is_whole() ? cell_count(level_) : cells_.size();
}

HexCell Globe::cell(std::uint64_t index) const {
    return is_whole() ? cell_at_index(level_, index) : cells_[index];
}

bool Globe::holds(HexCell cell) const {
    return std::cmp_equal(cell.level, level_) &&
           (is_whole() || local_.contains(globe::index_of(cell)));
}

std::uint64_t Globe::index_of(HexCell cell) const {
    HOG_ASSERT(std::cmp_equal(cell.level, level_), "cell is not at the globe's level");
    if (is_whole()) {
        return globe::index_of(cell);
    }
    const auto found = local_.find(globe::index_of(cell));
    HOG_ASSERT(found != local_.end(), "cell is outside the window");
    return found->second;
}

std::span<Hexel> Globe::column(HexCell cell) {
    return column_at(index_of(cell));
}

std::span<const Hexel> Globe::column(HexCell cell) const {
    return column_at(index_of(cell));
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

Volatiles Globe::held() const {
    double rock = 0.0;
    for (const Hexel& hexel : hexels_) {
        rock += hexel.held / 255.0 * properties(hexel.material).density;
    }
    rock *= hexel_volume();
    return {.water = rock * bound_.water, .gas = rock * bound_.gas};
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

Globe refined(const Globe& coarse) {
    HOG_ASSERT(coarse.is_whole(), "only a whole globe refines");
    Globe fine{coarse.level() + 1, coarse.shells().finer(1)};
    fine.set_bound(coarse.bound());
    fine.freed() = coarse.freed();
    for (std::uint64_t index = 0; index < cell_count(fine.level()); ++index) {
        const HexShares over = parents(cell_at_index(fine.level(), index));
        const auto column = fine.column_at(index);
        for (int shell = 0; shell < coarse.shells().count; ++shell) {
            Hexel lower = coarse.at(over.cells[0], shell);
            Hexel upper = over.count == 1 ? lower : coarse.at(over.cells[1], shell);
            if (properties(upper.material).density > properties(lower.material).density) {
                std::swap(lower, upper);
            }
            column[2 * static_cast<std::size_t>(shell)] = lower;
            column[(2 * static_cast<std::size_t>(shell)) + 1] = upper;
        }
    }
    return fine;
}

} // namespace hog::globe
