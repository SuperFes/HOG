#pragma once

#include <hog/planet/cell_id.hpp>

#include <cstdint>

namespace hog::planet {

// Large-scale land and sea: a low-frequency noise field cut at the sea level that puts the target
// fraction of the surface under water.
class Continents {
public:
    // Sea level is measured over every cell at this level.
    static constexpr int survey_level = 6;

    Continents(std::uint64_t seed, double ocean_fraction);

    // The raw field, in fixed point, roughly within [-1, 1].
    [[nodiscard]] std::int64_t field(CellId cell) const;

    [[nodiscard]] std::int64_t sea_level() const noexcept { return sea_level_; }

    // Fixed-point height above sea level; land is positive.
    [[nodiscard]] std::int64_t height(CellId cell) const { return field(cell) - sea_level_; }
    [[nodiscard]] bool is_land(CellId cell) const { return height(cell) > 0; }

private:
    std::uint64_t seed_;
    std::int64_t sea_level_ = 0;
};

} // namespace hog::planet
