#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/plates.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace hog::planet {

// Large-scale land and sea: the plates' crust, rising towards continent on some and sinking to
// ocean floor on others, roughened by noise and cut at the sea level that puts the target fraction
// of the surface under water. Where plates meet, continents colliding or overriding ocean floor
// raise mountains, and continents parting sink into rifts; volcanoes rise in arcs above plates
// diving.
class Continents {
public:
    // Sea level is measured over every cell at this level.
    static constexpr int survey_level = 6;
    // Metres below the abyss that ocean floor sinks where it dives under another plate.
    static constexpr std::int64_t trench_depth = 3'000;

    Continents(std::uint64_t seed, double ocean_fraction, std::shared_ptr<const Plates> plates);

    [[nodiscard]] const Plates& plates() const noexcept { return *plates_; }

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

    // The height above sea level and how strongly mountains rise, from 0 to 1, both in fixed
    // point with `Bits` fractional bits; and the grain ranges follow, as how much of the way along
    // them lies along each axis, in fixed point and summing to about 1; metres the ocean floor
    // rises above the abyss, or sinks below it into a trench, in fixed point with `Bits`; and how
    // strongly volcanoes rise above plates diving, from 0 to 1 in fixed point with `Bits`.
    struct Crust {
        std::int64_t height = 0;
        std::int64_t mountains = 0;
        std::array<std::int64_t, 3> grain{};
        std::int64_t floor = 0;
        std::int64_t arcs = 0;
    };
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] Crust crust(CellId cell) const;

private:
    // Where the plates' fields are read for a cell.
    [[nodiscard]] CellId warped(CellId cell) const;
    template <int Bits>
    [[nodiscard]] std::int64_t field(CellId cell, CellId warped) const;

    std::uint64_t seed_;
    std::uint64_t warp_seed_;
    std::shared_ptr<const Plates> plates_;
    // Fixed point per plate grid cell: the plate's crust, changing smoothly into its
    // neighbours' across their boundaries.
    std::vector<std::int64_t> crust_;
    // Fixed point per plate grid cell: how far rifts lower the field, and how strongly mountains
    // rise.
    std::vector<std::int64_t> rifts_;
    std::vector<std::int64_t> mountains_;
    std::array<std::vector<std::int64_t>, 3> grain_;
    // Fixed point metres per plate grid cell.
    std::vector<std::int64_t> floor_;
    // Fixed point per plate grid cell: how strongly volcanoes rise.
    std::vector<std::int64_t> arcs_;
    std::int64_t sea_level_ = 0;
};

extern template std::int64_t Continents::field<fixed_fraction_bits>(CellId cell) const;
extern template std::int64_t Continents::field<fine_fraction_bits>(CellId cell) const;
extern template Continents::Crust Continents::crust<fixed_fraction_bits>(CellId cell) const;
extern template Continents::Crust Continents::crust<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
