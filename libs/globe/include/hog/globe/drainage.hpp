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
    // Every cell, each after the cell it runs on to: downstream first.
    std::vector<std::uint32_t> order;

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

// How hard running water wears the ground: each step, a cell is cut by the stream power law, a
// rate of K·A^m·slope, for `step` seconds-worth of K.
struct Erosion {
    double rate = 1.0e-4; // K times the step: metres of fall per unit of area^m
    double area_power = 0.5;
    int steps = 20;
};

// The ground after running water has cut it: each step the drainage is worked out again, and each
// cell lowered toward the one it runs on to by how much drains through it, solved so no step
// overshoots whatever its size. Hollows holding water aren't cut, and cells at the sea are left;
// ground is only ever lowered, and the rock cut away isn't put back anywhere.
[[nodiscard]] std::vector<float> erode(std::span<const float> ground, int level, double radius,
                                       double sea, const Erosion& erosion);

} // namespace hog::globe
