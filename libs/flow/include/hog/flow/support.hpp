#pragma once

#include <hog/globe/material.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hog::flow {

// Whether shaped matter holds itself up, over a box of voxels: columns across, layers up. A
// column's solid voxels between open ones make a run. A run standing on the box's floor, or at
// its sides where the matter goes on beyond, holds up by the ground; any other has open space
// under it and hangs from the runs beside it, overlapping it in height, out to a span its own
// depth and strength allow, as a beam fixed at its root and loaded by its own weight. A run that
// reaches no ground, or further than that, fails. Distances are true: diagonals are longer by
// root two.
class Support {
public:
    // What a box voxel holds that bears on this.
    struct Cell {
        globe::Material solid = globe::Material::Air;
        std::uint8_t condition = 255; // how whole it is; weakens the matter in proportion
    };

    // Tensile strength as a share of the compressive strength `Properties` gives.
    static constexpr double tensile = 0.1;

    Support(int across_i, int across_j, int layers, double thickness, double gravity);

    [[nodiscard]] Cell& at(int i, int j, int layer) { return cells_[index(i, j, layer)]; }
    [[nodiscard]] const Cell& at(int i, int j, int layer) const {
        return cells_[index(i, j, layer)];
    }

    // A run of solid voxels up one column, `bottom` to `top` inclusive.
    struct Run {
        int i = 0;
        int j = 0;
        int bottom = 0;
        int top = 0;
    };
    // The runs that fail, lowest column first.
    [[nodiscard]] std::vector<Run> failing() const;

    // The most a beam of `depth` metres of `material` at condition `condition` can reach out
    // from its root, in metres.
    [[nodiscard]] double reach(globe::Material material, double depth,
                               std::uint8_t condition) const;

private:
    [[nodiscard]] std::size_t index(int i, int j, int layer) const {
        return (((static_cast<std::size_t>(layer) * static_cast<std::size_t>(across_j_)) +
                 static_cast<std::size_t>(j)) *
                static_cast<std::size_t>(across_i_)) +
               static_cast<std::size_t>(i);
    }

    int across_i_;
    int across_j_;
    int layers_;
    double thickness_;
    double gravity_;
    std::vector<Cell> cells_;
};

} // namespace hog::flow
