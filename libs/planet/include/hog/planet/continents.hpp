#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>

#include <cstdint>

namespace hog::planet {

// Large-scale land and sea: a low-frequency noise field cut at the sea level that puts the target
// fraction of the surface under water.
class Continents {
public:
    // Sea level is measured over every cell at this level.
    static constexpr int survey_level = 6;

    Continents(std::uint64_t seed, double ocean_fraction);

    // The raw field, in fixed point with `Bits` fractional bits, roughly within [-1, 1].
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t field(CellId cell) const;

    [[nodiscard]] std::int64_t sea_level() const noexcept { return sea_level_; }

    // Fixed-point height above sea level; land is positive.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t height(CellId cell) const {
        return field<Bits>(cell) - (sea_level_ << (Bits - fixed_fraction_bits));
    }
    // Told by the fine height, as the ground's elevation is.
    [[nodiscard]] bool is_land(CellId cell) const { return height<fine_fraction_bits>(cell) > 0; }

private:
    std::uint64_t seed_;
    std::int64_t sea_level_ = 0;
};

extern template std::int64_t Continents::field<fixed_fraction_bits>(CellId cell) const;
extern template std::int64_t Continents::field<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
