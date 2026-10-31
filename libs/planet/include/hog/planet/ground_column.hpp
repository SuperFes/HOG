#pragma once

#include <hog/planet/beds.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/materials.hpp>

#include <cstdint>
#include <vector>

namespace hog::planet {

// The material the volume holds a rock as.
[[nodiscard]] Material material_of(Rock rock);

// One material up a column, from the top of the run under it to `top`, fixed-point metres over the
// planet's radius.
struct Run {
    Material material = Material::Air;
    std::int64_t top = 0;

    friend bool operator==(const Run&, const Run&) = default;
};

// Fixed-point metres over the planet's radius of the ground at a cell's centre, and of the top of
// the sea, lake or river over it, or of the ground where it's dry.
struct ColumnTop {
    std::int64_t ground = 0;
    std::int64_t water = 0;

    friend bool operator==(const ColumnTop&, const ColumnTop&) = default;
};

[[nodiscard]] ColumnTop column_top(const Elevation& terrain, CellId cell);

// What fills the column through a cell's centre from `low` up to `high`, fixed-point metres over
// the planet's radius, as the terrain has it: rock as the beds lay it and the land rose, soil over
// the rock, the sea, a lake or a river over the ground, and air above. Runs go upward, each a
// different material from the one under it, the last ending at `high`. `top` is the cell's,
// as column_top has it.
// Precondition: low < high.
[[nodiscard]] std::vector<Run> ground_column(const Elevation& terrain, CellId cell,
                                             const ColumnTop& top, std::int64_t low,
                                             std::int64_t high);
[[nodiscard]] inline std::vector<Run> ground_column(const Elevation& terrain, CellId cell,
                                                    std::int64_t low, std::int64_t high) {
    return ground_column(terrain, cell, column_top(terrain, cell), low, high);
}

} // namespace hog::planet
