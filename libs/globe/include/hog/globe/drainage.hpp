#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace hog::globe {

// Where rain on a planet's ground goes, over the cells of one level: each cell's ground, the
// ground with every hollow filled to where it spills over, the cell water leaves each by, and how
// much ground drains through each. It only reads the ground; it carves and places nothing.
struct Drainage {
    int level = 0;
    std::vector<float> ground;   // metres, as given
    std::vector<float> filled;   // ground, raised where a hollow holds water before it spills
    // The neighbour water runs on to: the one it was reached from, lowest first. A cell at the
    // sea is its own.
    std::vector<std::uint32_t> receiver;
    std::vector<double> catchment; // square metres draining through the cell, its own included

    // How deep the water stands over a cell's ground once its hollow is full.
    [[nodiscard]] double depth(std::uint32_t cell) const noexcept {
        return static_cast<double>(filled[cell]) - static_cast<double>(ground[cell]);
    }
};

// Floods in from the sea, lowest first, so each hollow fills to its lowest rim and spills on
// toward the sea; the receivers then follow the filled ground downhill. `ground` is each cell's
// height over the radius in metres, in the level's cell order; cells at or under `sea` are where
// water leaves. Without any, the lowest cell is. `radius` is the planet's, for the cells' areas.
[[nodiscard]] Drainage drain(std::span<const float> ground, int level, double radius, double sea);

} // namespace hog::globe
