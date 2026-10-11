#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

#include <array>

namespace hog::planet {

// Cube-face coordinates. (u, v) span [-1, 1] across the flat face; (s, t) span [0, 1] after a
// quadratic warp that keeps cell areas within about 2:1 of each other across the face. Cell
// positions are uniform in (s, t). Only +, *, / and sqrt are used, which IEEE 754 rounds exactly,
// so results are identical on every platform built without floating-point contraction.
[[nodiscard]] double st_to_uv(double s) noexcept;
[[nodiscard]] double uv_to_st(double u) noexcept;

struct FaceUV {
    int face = 0;
    double u = 0.0;
    double v = 0.0;
};

// A point on the face's cube surface (not unit length).
[[nodiscard]] DVec3 face_uv_to_xyz(int face, double u, double v);

// The face a non-zero direction passes through, and where; ties go to the lower axis.
[[nodiscard]] FaceUV xyz_to_face_uv(DVec3 direction);

// Unit directions from the planet centre.
[[nodiscard]] DVec3 cell_center(CellId cell);
// Corners in counter-clockwise order seen from outside, starting at the lowest i and j.
[[nodiscard]] std::array<DVec3, 4> cell_vertices(CellId cell);

// Area on the unit sphere, in steradians. Uses atan2, so it may differ in the last bits between
// platforms: fine for statistics, not for generation.
[[nodiscard]] double cell_area(CellId cell);

// The cell containing a non-zero direction, at the given level.
[[nodiscard]] CellId cell_at(DVec3 direction, int level = max_level);

// Planet-centred positions, for a planet of the given radius centred on the origin.
[[nodiscard]] WorldPosition surface_position(CellId cell, double radius);
[[nodiscard]] CellId cell_at(WorldPosition position, int level = max_level);

// The coarsest level whose cells are at most `cell_size` metres across on average.
[[nodiscard]] int level_for(double cell_size, double planet_radius);

} // namespace hog::planet
