#pragma once

#include <hog/globe/hex_cell.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
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
};

// Shells from `inner` to `outer` about `thickness` thick where they pass `radius`.
[[nodiscard]] Shells shells_about(double inner, double outer, double radius, double thickness);

// Every column of a level, each a stack of shells: the whole globe at one size of hexel.
class Globe {
public:
    Globe(int level, Shells shells);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] const Shells& shells() const noexcept { return shells_; }

    // A column's hexels, the lowest shell first.
    [[nodiscard]] std::span<Hexel> column(HexCell cell);
    [[nodiscard]] std::span<const Hexel> column(HexCell cell) const;

    // The same by the column's index in its level.
    [[nodiscard]] std::span<Hexel> column_at(std::uint64_t index);
    [[nodiscard]] std::span<const Hexel> column_at(std::uint64_t index) const;

    [[nodiscard]] Hexel& at(HexCell cell, int shell);
    [[nodiscard]] const Hexel& at(HexCell cell, int shell) const;

    // Cubic metres in a hexel, any hexel: a pentagon's count as whole ones, so moving them about
    // keeps every amount.
    [[nodiscard]] double hexel_volume() const noexcept;

    // Kilograms of each material.
    [[nodiscard]] std::array<double, material_count> masses() const;

private:
    int level_;
    Shells shells_;
    std::vector<Hexel> hexels_;
};

} // namespace hog::globe
