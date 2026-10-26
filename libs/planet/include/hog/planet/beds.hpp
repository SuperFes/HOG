#pragma once

#include <hog/planet/cell_id.hpp>

#include <cstdint>
#include <string_view>

namespace hog::planet {

enum class Rock : std::uint8_t {
    Soil,
    Sand,
    Sandstone,
    Shale,
    Limestone,
    Granite,
    Basalt,
    Peridotite,
    Magma,
};

inline constexpr int rock_count = static_cast<int>(Rock::Magma) + 1;

[[nodiscard]] std::string_view to_string(Rock rock) noexcept;

// What the beds under a cell depend on.
struct BedSetting {
    // Whether continental crust lies under the cell; elsewhere the rock is volcanic.
    bool continental = false;
    // Fixed point from 0 to 1: how hard a mountain belt has folded the beds and lifted the
    // basement.
    std::int64_t mountains = 0;
};

// Layered rock from the seed alone, so rivers can cut down through it while the landform
// evolves, before the geology exists. On continents, sandstone, shale and limestone lie in beds
// tens of metres thick on a granite basement; the beds are gently warped, folded hard in mountain
// belts, and each region favours its own rock. Belts lift the basement high enough to be laid
// bare. Away from continents it's basalt all the way down. Heights are fixed-point metres, on the
// rock as it lay before any uplift.
class Beds {
public:
    Beds(std::uint64_t seed, double planet_radius);

    [[nodiscard]] Rock rock_at(CellId cell, std::int64_t height, BedSetting setting) const;

    // Fixed-point metres: the top of the granite.
    [[nodiscard]] std::int64_t basement(CellId cell, BedSetting setting) const;

private:
    // The height of the beds' layering at the cell, for a height in the rock.
    [[nodiscard]] std::int64_t layered(CellId cell, std::int64_t height, BedSetting setting) const;

    std::uint64_t seed_;
    double planet_radius_;
};

} // namespace hog::planet
