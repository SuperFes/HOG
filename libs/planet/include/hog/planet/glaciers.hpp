#pragma once

#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hog::planet {

// Fixed-point metres of ice a year each square metre of a cell gains as snow at a fixed-point
// height, or loses melting where negative.
using IceBalance = std::function<std::int64_t(std::uint32_t cell, std::int64_t height)>;

// For the glaciers of an ice age.
struct Glaciation {
    IceBalance balance;
};

// Fixed-point metres the glaciers of an ice age cut each cell of `heights` down by. Ice gathers
// where more snow falls than melts, flows down the drainage and melts away below; wherever enough
// of it flows, it cuts a U-shaped trough, deeper and wider the more ice there is and deeper the
// steeper it has fallen, down past the sea where it reaches it: a fjord once the ice is gone. Ice
// spreading slowly over lowland barely cuts it. Small glaciers cut shallower than the
// large ones they join, so their valleys hang over the trough. Ice reaching the sea floats off
// and cuts no further. The arithmetic is integer, or double using only correctly rounded
// operations, so the result is the same everywhere.
[[nodiscard]] std::vector<std::int64_t> glaciate(const LevelGrid& grid,
                                                 std::span<const std::int64_t> heights,
                                                 const Glaciation& glaciation,
                                                 std::int64_t sea_level = 0);

} // namespace hog::planet
