#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>

#include <cstddef>
#include <optional>

namespace hog::weather {

// The air as layers over the globe's cube-sphere columns: each column of `level` holds `layers`
// cells of equal thickness, from `bottom` metres from the planet's centre upward.
struct Grid {
    int level = 6;
    int layers = 20;
    double bottom = 0.0;
    double thickness = 0.0;

    [[nodiscard]] std::size_t columns() const noexcept {
        return static_cast<std::size_t>(globe::cells_at(level));
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return columns() * static_cast<std::size_t>(layers);
    }

    // A cell's place in the field: its column's (its cell's index in the level), then its layer.
    [[nodiscard]] std::size_t index(std::size_t column, int layer) const noexcept {
        return (column * static_cast<std::size_t>(layers)) + static_cast<std::size_t>(layer);
    }

    // Metres from the centre to a layer's middle, and its floor.
    [[nodiscard]] double radius_of(int layer) const noexcept {
        return bottom + ((static_cast<double>(layer) + 0.5) * thickness);
    }
    [[nodiscard]] double floor_of(int layer) const noexcept {
        return bottom + (static_cast<double>(layer) * thickness);
    }
    [[nodiscard]] double top() const noexcept { return floor_of(layers); }

    // The layer a radius lies in; none outside the air.
    [[nodiscard]] std::optional<int> layer_at(double radius) const noexcept;
};

} // namespace hog::weather
