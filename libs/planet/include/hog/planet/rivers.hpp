#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/landform.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::planet {

// A river's channel at one point: the ground cut down into it and the water over it, fixed-point
// metres.
struct Channel {
    std::int64_t bed = 0;
    std::int64_t surface = 0;

    friend bool operator==(const Channel&, const Channel&) = default;
};

// Water standing over a point, the sea or a lake: its fixed-point level and which it is.
struct StandingWater {
    std::int64_t level = 0;
    bool sea = false;

    friend bool operator==(const StandingWater&, const StandingWater&) = default;
};

// A river from one landform cell's centre to the next downstream: its water falls from one end to
// the other, in fixed-point metres.
struct Reach {
    DVec3 from{};
    DVec3 to{};
    std::int64_t from_surface = 0;
    std::int64_t to_surface = 0;
    double width = 0.0; // metres
    std::int64_t depth = 0;
    // How far its water falls where it runs steeply enough to tumble over falls or rapids, with a
    // pool scoured at their foot; 0 elsewhere.
    std::int64_t fall = 0;
};

// Rivers from every landform cell that drains enough ground, each running to the cell it drains
// into and meandering about the line between their centres. Channels widen and deepen with the
// water they carry. A river's water only falls downstream, so it cuts through rises in the ground
// rather than pooling behind them, between banks sloping up to meet the ground. Basins in the
// landform hold lakes where enough water runs in to make up for what evaporates, and stay dry
// where it doesn't; rivers end at their shores and leave from their outlets. Only the landform's
// sea holds the sea: low ground cut off from it is a basin like any other.
class Rivers {
public:
    // Banks rise from the water at this slope until they meet the ground.
    static constexpr double bank_slope = 0.5;

    // Rise over run of a reach's water from which it falls rather than runs.
    static constexpr double falls_gradient = 0.1;
    // Square metres of rain-weighted ground a cell must drain for a river to start there.
    static constexpr double source_area = 20e6;

    // `ground` is the fixed-point height of the ground at each landform cell's centre, before
    // rivers cut it. Rivers bend with `meander`, and `scale` multiplies their width and depth.
    // The landform must outlive the rivers.
    Rivers(const Landform& landform, std::span<const std::int64_t> ground, Meander meander,
           double radius, double scale = 1.0);

    [[nodiscard]] std::span<const Reach> reaches() const noexcept { return reaches_; }

    // Per landform cell, the fixed-point level of the water it drains into first: its own river
    // or lake, the one downstream, or the sea.
    [[nodiscard]] std::span<const std::int64_t> levels() const noexcept { return levels_; }

    // The ground at a cell's centre once rivers have cut their channels and banks into it, where
    // `ground` is its height before.
    [[nodiscard]] std::int64_t cut(CellId cell, std::int64_t ground) const;

    // The channel through a cell's centre, or nothing out of the water.
    [[nodiscard]] std::optional<Channel> channel(CellId cell, std::int64_t ground) const;

    // The water standing over a cell's centre, where `ground` is the height there, or nothing out
    // of the water.
    [[nodiscard]] std::optional<StandingWater> standing(CellId cell, std::int64_t ground) const;

    // The fixed-point level of the lake over a cell's centre, where `ground` is the height there,
    // or nothing out of a lake.
    [[nodiscard]] std::optional<std::int64_t> lake(CellId cell, std::int64_t ground) const;

    // The widest reach whose water comes within `margin` metres of a cell's centre, or none: for
    // drawing rivers narrower than what they're drawn on.
    [[nodiscard]] const Reach* near(CellId cell, double margin) const;

    // Metres across and deep for a river draining `area` square metres of rain-weighted ground,
    // as on Earth.
    [[nodiscard]] static double width(double area);
    [[nodiscard]] static double depth(double area);

private:
    // Calls `visit(reach, level, away)` for each reach near the cell, with the fixed-point level
    // of its water there and the metres from its line.
    template <typename Visit>
    void each_reach(CellId cell, Visit visit) const;
    // The fixed-point level of a reach's water where its line is at `level` and the ground at
    // `ground`: no higher than the ground, nor deeper in it than its banks can climb.
    [[nodiscard]] std::int64_t sunk(const Reach& reach, std::int64_t level,
                                    std::int64_t ground) const;

    const Landform* landform_;
    const LevelGrid* grid_;
    Meander meander_;
    double radius_;
    std::vector<Reach> reaches_;
    std::vector<std::int64_t> levels_;
    // Landform cells under the sea.
    std::vector<bool> sea_;
    // Landform cells below the sea's level but cut off from it.
    std::vector<bool> sunken_;
    // Reaches coming within a cell of each landform cell.
    std::vector<std::uint32_t> first_;
    std::vector<std::uint32_t> nearby_;
};

} // namespace hog::planet
