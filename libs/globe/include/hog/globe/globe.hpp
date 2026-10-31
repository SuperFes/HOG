#pragma once

#include <hog/globe/hex_cell.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::globe {

// One hexel: a hex prism of a column and a shell.
struct Hexel {
    Material material = Material::Air;
    std::uint16_t temperature = 0; // kelvin

    friend bool operator==(const Hexel&, const Hexel&) = default;
};

// Shells of one thickness stacked out from a radius.
struct Shells {
    double inner = 0.0;     // metres from the centre to the lowest shell's floor
    double thickness = 0.0; // metres
    int count = 0;

    [[nodiscard]] double floor(int shell) const noexcept { return inner + (thickness * shell); }
    [[nodiscard]] double ceiling(int shell) const noexcept { return floor(shell + 1); }
    [[nodiscard]] double outer() const noexcept { return floor(count); }
};

// Every column of a level, each a stack of shells: the whole globe at one size of hexel.
class Globe {
public:
    Globe(int level, Shells shells);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] const Shells& shells() const noexcept { return shells_; }

    // A column's hexels, the lowest shell first.
    [[nodiscard]] std::span<Hexel> column(HexCell cell);
    [[nodiscard]] std::span<const Hexel> column(HexCell cell) const;

    [[nodiscard]] Hexel& at(HexCell cell, int shell);
    [[nodiscard]] const Hexel& at(HexCell cell, int shell) const;

    // Kilograms of each material.
    [[nodiscard]] std::array<double, material_count> masses() const;

private:
    int level_;
    Shells shells_;
    std::vector<Hexel> hexels_;
};

} // namespace hog::globe
