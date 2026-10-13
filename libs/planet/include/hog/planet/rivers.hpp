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

// A river from one landform cell's centre to the next downstream: its water falls from one end to
// the other, in fixed-point metres.
struct Reach {
    DVec3 from{};
    DVec3 to{};
    std::int64_t from_surface = 0;
    std::int64_t to_surface = 0;
    double width = 0.0; // metres
    std::int64_t depth = 0;
};

// Rivers from every landform cell that drains enough ground, each running to the cell it drains
// into and meandering about the line between their centres. Channels widen and deepen with the
// water they carry. A river's water only falls downstream, so it cuts through rises in the ground
// rather than pooling behind them, between banks sloping up to meet the ground.
class Rivers {
public:
    // Square metres of rain-weighted ground a cell must drain for a river to start there.
    static constexpr double source_area = 20e6;

    // `ground` is the fixed-point height of the ground at each landform cell's centre, before
    // rivers cut it. Rivers bend with `meander`, and `scale` multiplies their width and depth.
    // The landform must outlive the rivers.
    Rivers(const Landform& landform, std::span<const std::int64_t> ground, Meander meander,
           double radius, double scale = 1.0);

    [[nodiscard]] std::span<const Reach> reaches() const noexcept { return reaches_; }

    // Per landform cell, the fixed-point level of the water it drains into first: its own river,
    // the one downstream, or the sea.
    [[nodiscard]] std::span<const std::int64_t> levels() const noexcept { return levels_; }

    // The ground at a cell's centre once rivers have cut their channels and banks into it, where
    // `ground` is its height before.
    [[nodiscard]] std::int64_t cut(CellId cell, std::int64_t ground) const;

    // The channel through a cell's centre, or nothing out of the water.
    [[nodiscard]] std::optional<Channel> channel(CellId cell, std::int64_t ground) const;

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

    const LevelGrid* grid_;
    Meander meander_;
    double radius_;
    std::vector<Reach> reaches_;
    std::vector<std::int64_t> levels_;
    // Reaches coming within a cell of each landform cell.
    std::vector<std::uint32_t> first_;
    std::vector<std::uint32_t> nearby_;
};

} // namespace hog::planet
