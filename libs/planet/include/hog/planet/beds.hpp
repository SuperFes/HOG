#pragma once

#include <hog/planet/cell_id.hpp>

#include <cstdint>
#include <optional>
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

// What the beds hold at one cell, for reading rock at many heights there cheaply.
struct BedColumn {
    // Fixed-point metres the layering is raised by its warps and folds.
    std::int32_t shift = 0;
    // Fixed-point metres: the top of the granite.
    std::int32_t basement = 0;
    // Fixed point from -1 to 1: which rock the region favours, sandstone to limestone.
    std::int32_t leaning = 0;
    bool continental = false;
};

// One sedimentary bed: its base and top in fixed-point metres, and its rock.
struct Bed {
    std::int64_t base = 0;
    std::int64_t top = 0;
    Rock rock = Rock::Shale;
};

// Ground at `height` within `bed` moved towards the bed's own profile, by `strength` in fixed
// point from 0 to 1: flat benches where one bed meets the next, and a riser between, up to
// 1.875 times as steep as the ground was.
[[nodiscard]] std::int64_t ledge(const Bed& bed, std::int64_t height, std::int64_t strength);

// Layered rock from the seed alone, so rivers can cut down through it while the landform
// evolves, before the geology exists. On continents, sandstone, shale and limestone lie in beds
// tens of metres thick on a granite basement; the beds are gently warped, folded hard in mountain
// belts, and each region favours its own rock. Belts lift the basement high enough to be laid
// bare. Away from continents it's basalt all the way down. Heights are fixed-point metres, on the
// rock as it lay before any uplift.
class Beds {
public:
    Beds(std::uint64_t seed, double planet_radius);

    [[nodiscard]] BedColumn column(CellId cell, BedSetting setting) const;
    [[nodiscard]] Rock rock_at(const BedColumn& column, std::int64_t height) const;
    [[nodiscard]] Rock rock_at(CellId cell, std::int64_t height, BedSetting setting) const {
        return rock_at(column(cell, setting), height);
    }

    // The bed at `height`, or none in the granite or volcanic rock, which lie in no beds.
    [[nodiscard]] std::optional<Bed> bed_at(const BedColumn& column, std::int64_t height) const;
    // Fixed-point metres: the top of the granite.
    [[nodiscard]] std::int64_t basement(CellId cell, BedSetting setting) const;

private:
    // Where bed `index` starts in the layering, unshifted.
    [[nodiscard]] std::int64_t base_of(std::int64_t index) const;
    // Which bed of the layering lies at `layer`.
    [[nodiscard]] std::int64_t index_at(std::int64_t layer) const;
    [[nodiscard]] Rock rock_of(std::int64_t index, const BedColumn& column) const;

    std::uint64_t seed_;
    double planet_radius_;
};

} // namespace hog::planet
