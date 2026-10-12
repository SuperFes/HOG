#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/cell_grid.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hog::sim {

// How far, in radians, natural_point moves points for a field held on a grid level: about half a
// cell, enough to hide the cells' straight edges without carrying features far.
[[nodiscard]] double natural_reach(int level) noexcept;

// A unit direction moved along smooth noise by about `reach` radians, the same for a seed however
// finely it's sampled. A field held in cells, sampled at the moved point, loses the cells' straight
// edges and grid-shaped gradients for wandering, natural-looking ones.
[[nodiscard]] DVec3 natural_point(DVec3 direction, double reach, std::uint64_t seed);

// A field's value at a unit direction, where value(index) gives it at a cell, blended bilinearly
// between the four nearest cell centres. Unlike CellGrid::interpolate it has no seams at cell
// edges, so it suits showing a field rather than stepping it.
template <typename Value>
[[nodiscard]] double blend(const CellGrid& grid, DVec3 point, Value value);

template <typename Value>
double blend(const CellGrid& grid, DVec3 point, Value value) {
    const int level = grid.level();
    const auto side = static_cast<int>(std::uint32_t{1} << level);
    const planet::FaceUV uv = planet::xyz_to_face_uv(point);
    // Cell centres sit at whole numbers.
    const double x = (planet::uv_to_st(uv.u) * side) - 0.5;
    const double y = (planet::uv_to_st(uv.v) * side) - 0.5;
    const double left = std::floor(x);
    const double low = std::floor(y);
    const auto on_face = [side](int index) { return index >= 0 && index < side; };
    // Off this face along one axis: whichever cell lies where that centre would be, next door.
    const auto near = [&](int i, int j) {
        if (on_face(i) && on_face(j)) {
            return static_cast<double>(value(grid.index(planet::CellId::from_face_ij(
                uv.face, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j), level))));
        }
        const DVec3 there = planet::face_uv_to_xyz(uv.face, planet::st_to_uv((i + 0.5) / side),
                                                   planet::st_to_uv((j + 0.5) / side));
        return static_cast<double>(value(grid.index(planet::cell_at(there, level))));
    };
    const auto at = [&](int i, int j) {
        if (on_face(i) || on_face(j)) {
            return near(i, j);
        }
        // Past a cube corner, where only three faces meet: the three cells around it, evenly, so
        // every face agrees there.
        const int inside_i = std::clamp(i, 0, side - 1);
        const int inside_j = std::clamp(j, 0, side - 1);
        return (near(inside_i, inside_j) + near(i, inside_j) + near(inside_i, j)) / 3.0;
    };
    const auto i = static_cast<int>(left);
    const auto j = static_cast<int>(low);
    const double across = x - left;
    const double up = y - low;
    const double below = std::lerp(at(i, j), at(i + 1, j), across);
    const double above = std::lerp(at(i, j + 1), at(i + 1, j + 1), across);
    return std::lerp(below, above, up);
}

} // namespace hog::sim
