#pragma once

#include <hog/globe/hex_cell.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::globe {

// One hexel: a hex prism of a column and a shell.
struct Hexel {
    Material material = Material::Air;
    // The share of the water and gas it formed with that rock still holds, out of 255; it lets
    // them go as it melts.
    std::uint8_t held = 0;
    std::uint16_t temperature = 0; // kelvin

    friend bool operator==(const Hexel&, const Hexel&) = default;
};

// Water and gas, in kilograms.
struct Volatiles {
    double water = 0.0;
    double gas = 0.0;

    friend bool operator==(const Volatiles&, const Volatiles&) = default;
};

// Shells stacked from one radius out to another, each holding the same volume: they thin as they
// widen, so every hexel of a level holds as much as any other, and moving hexels about keeps how
// much there is of everything.
struct Shells {
    double inner = 0.0; // metres from the centre to the lowest shell's floor
    double outer = 0.0; // metres from the centre to the highest shell's ceiling
    int count = 0;

    [[nodiscard]] double floor(int shell) const noexcept {
        if (shell <= 0) {
            return inner;
        }
        if (shell >= count) {
            return outer;
        }
        const double low = inner * inner * inner;
        const double high = outer * outer * outer;
        return std::cbrt(low + ((high - low) * shell / count));
    }
    [[nodiscard]] double ceiling(int shell) const noexcept { return floor(shell + 1); }
    [[nodiscard]] double thickness(int shell) const noexcept {
        return ceiling(shell) - floor(shell);
    }
    // Shells `from` up to `to` of these, as shells of their own.
    [[nodiscard]] Shells part(int from, int to) const noexcept {
        return {.inner = floor(from), .outer = floor(to), .count = to - from};
    }
    // The shells `levels` finer, each split in two per level so their floors land on these.
    [[nodiscard]] Shells finer(int levels) const noexcept {
        return {.inner = inner, .outer = outer, .count = count << levels};
    }
};

// Shells from `inner` to `outer` about `thickness` thick where they pass `radius`.
[[nodiscard]] Shells shells_about(double inner, double outer, double radius, double thickness);

// Columns of a level, each a stack of shells, held densely to work on: the whole globe at one size
// of hexel, or a window onto part of it, some of its columns and a run of its shells.
class Globe {
public:
    Globe(int level, Shells shells);
    // A window: `cells`, each through `shells`, which start at the whole's shell `first_shell`.
    Globe(int level, Shells shells, std::vector<HexCell> cells, int first_shell);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] const Shells& shells() const noexcept { return shells_; }
    [[nodiscard]] int first_shell() const noexcept { return first_shell_; }
    [[nodiscard]] bool is_whole() const noexcept { return cells_.empty(); }

    // Columns held, each by an index of its own from 0; for the whole globe, its index in the
    // level.
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] HexCell cell(std::uint64_t index) const;
    [[nodiscard]] bool holds(HexCell cell) const;
    [[nodiscard]] std::uint64_t index_of(HexCell cell) const;

    // A column's hexels, the lowest shell first.
    [[nodiscard]] std::span<Hexel> column(HexCell cell);
    [[nodiscard]] std::span<const Hexel> column(HexCell cell) const;

    // The same by the column's index.
    [[nodiscard]] std::span<Hexel> column_at(std::uint64_t index);
    [[nodiscard]] std::span<const Hexel> column_at(std::uint64_t index) const;

    [[nodiscard]] Hexel& at(HexCell cell, int shell);
    [[nodiscard]] const Hexel& at(HexCell cell, int shell) const;

    // Cubic metres in a hexel, any hexel: a pentagon's count as whole ones, so moving them about
    // keeps every amount.
    [[nodiscard]] double hexel_volume() const noexcept;

    // Kilograms of each material.
    [[nodiscard]] std::array<double, material_count> masses() const;

    // Kilograms of water and gas in a kilogram of rock that holds all it formed with.
    [[nodiscard]] const Volatiles& bound() const noexcept { return bound_; }
    void set_bound(const Volatiles& per_kilogram) noexcept { bound_ = per_kilogram; }
    // What rock still holds.
    [[nodiscard]] Volatiles held() const;
    // What rock has let go: in the air, and once it rains, the sea.
    [[nodiscard]] Volatiles& freed() noexcept { return freed_; }
    [[nodiscard]] const Volatiles& freed() const noexcept { return freed_; }

private:
    int level_;
    Shells shells_;
    std::vector<HexCell> cells_;
    std::unordered_map<std::uint64_t, std::uint64_t> local_;
    int first_shell_ = 0;
    std::vector<Hexel> hexels_;
    Volatiles bound_;
    Volatiles freed_;
};

// The globe a level finer. Each coarse hexel becomes eight: two shells of its centre child, and a
// hexel in each of the two shells of every child it shares with a neighbour, the denser lower.
// Pentagons have five such children, so theirs become seven.
[[nodiscard]] Globe refined(const Globe& coarse);

} // namespace hog::globe
