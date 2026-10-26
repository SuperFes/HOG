#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

#include <array>
#include <cstdint>
#include <limits>
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
    // Fixed point from 0 to 1: how hard a mountain belt has folded and thrust the beds and lifted
    // the basement.
    std::int64_t mountains = 0;
    // How much of the way along each axis the belt runs, in fixed point summing to about 1: its
    // folds and thrusts run with it.
    std::array<std::int64_t, 3> grain{};
    // Fixed-point metres: the height a volcano's lava lies above, burying the beds.
    std::int64_t lava = std::numeric_limits<std::int32_t>::max();
};

// What the beds hold at one cell, for reading rock at many heights there cheaply.
struct BedColumn {
    // Fixed-point metres the layering is raised by its warps, folds and thrusts.
    std::int32_t shift = 0;
    // Fixed-point metres: the top of the granite.
    std::int32_t basement = 0;
    // Fixed point from -1 to 1: how far offshore the region lay, from sandstone by the shore to
    // limestone far out.
    std::int32_t leaning = 0;
    // Fixed point, local noise that thickens and thins each bed and moves where its rock
    // changes, differently for each bed.
    std::array<std::int32_t, 2> lateral{};
    // Fixed-point metres: the base of any volcano's lava.
    std::int32_t lava = std::numeric_limits<std::int32_t>::max();
    bool continental = false;
};

// One sedimentary bed: its base and top in fixed-point metres, its rock, and which it is,
// counting upwards.
struct Bed {
    std::int64_t base = 0;
    std::int64_t top = 0;
    Rock rock = Rock::Shale;
    std::int64_t index = 0;
};

// Ground at `height` within `bed` moved towards the bed's own profile, by `strength` in fixed
// point from 0 to 1: flat benches where one bed meets the next, and a riser between, up to
// 1.875 times as steep as the ground was.
[[nodiscard]] std::int64_t ledge(const Bed& bed, std::int64_t height, std::int64_t strength);

// Layered rock from the seed alone, so rivers can cut down through it while the landform
// evolves, before the geology exists. On continents, sandstone, shale and limestone lie in beds
// tens of metres thick on a granite basement. Each bed's rock is how far offshore it was laid, as
// the sea rose and fell, so formations of one rock step through shale between sandstone and
// limestone; each region lay further out or in, and each rock tongues out sideways into the next
// as beds thin, thicken and pinch out. Away from the
// belts the beds lie nearly flat, gently warped; in them they are folded along the belt's grain
// and stacked in thrust sheets, each tilted, the layering repeating across each thrust. Belts
// lift the basement high enough to be laid bare, and volcanoes bury the beds in lava. Away from
// continents it's basalt all the way down. Heights are fixed-point metres, on the rock as it lay
// before any uplift.
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
    // In fixed point, the tangent of the angle the belt's folds and thrusts tilt the beds by;
    // the plains' gentle warp is taken as flat.
    [[nodiscard]] std::int64_t dip(CellId cell, const BedSetting& setting) const;

private:
    // Fixed-point metres the belt's folds and thrusts raise the layering by at `center`.
    [[nodiscard]] std::int64_t belt(DVec3 center, const BedSetting& setting) const;
    // Where bed `index` starts in the layering, unshifted.
    [[nodiscard]] std::int64_t base_of(std::int64_t index, const BedColumn& column) const;
    // Which bed of the layering lies at `layer`.
    [[nodiscard]] std::int64_t index_at(std::int64_t layer, const BedColumn& column) const;
    [[nodiscard]] Rock rock_of(std::int64_t index, const BedColumn& column) const;

    std::uint64_t seed_;
    double planet_radius_;
};

} // namespace hog::planet
