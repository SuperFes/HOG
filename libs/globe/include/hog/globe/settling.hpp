#pragma once

#include <hog/globe/globe.hpp>
#include <hog/globe/hex_cell.hpp>
#include <hog/globe/hex_grid.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::globe {

// The pull of a planet of one density: growing with height inside it, falling off as the square
// of it above.
struct Gravity {
    double surface = 9.81; // m/s²
    double radius = 1.0;   // metres

    [[nodiscard]] double at(double height) const noexcept {
        return height <= radius ? surface * height / radius
                                : surface * radius * radius / (height * height);
    }
};

// Metres a sheer face of the hexel's material stands under `gravity` for good, weakening as it
// warms towards melting.
[[nodiscard]] double bearable(const Hexel& hexel, double gravity);

// Lets what stands higher than it can bear come down. A column's top hexel falls to the nearest
// column at least two shells lower that it can reach over columns open at its shell, if the slope
// between them is steeper than its rock holds; then everything round them is looked at again, out
// to wherever it reaches, until all stand. Rock with no strength left, like a fluid, levels out to
// within a shell.
class Settler {
public:
    // Columns may change between calls only where they're then handed in as unsettled.
    Settler(Globe& globe, const HexGrid& grid, Gravity gravity);

    // Settles from `unsettled` outward; returns the hexels moved.
    std::uint64_t settle(std::span<const HexCell> unsettled);

private:
    struct Fall {
        std::uint32_t column = 0;
        int top = 0;
    };

    // Rings of columns looked through for somewhere to fall.
    static constexpr int farthest = 32;

    [[nodiscard]] std::optional<Fall> fall_to(std::uint32_t column);
    void queue(std::uint32_t column);

    Globe* globe_;
    const HexGrid* grid_;
    Gravity gravity_;
    std::vector<int> tops_;
    std::vector<std::uint32_t> seen_;
    std::uint32_t mark_ = 0;
    std::vector<bool> queued_;
    std::vector<std::uint32_t> pending_;
    std::vector<Fall> ring_;
    std::vector<Fall> beyond_;
    double step_; // radians between neighbouring columns
};

// The same, once.
std::uint64_t settle(Globe& globe, std::span<const HexCell> unsettled, const Gravity& gravity);

// One past a column's highest hexel that isn't air.
[[nodiscard]] int top_of(std::span<const Hexel> column);

} // namespace hog::globe
