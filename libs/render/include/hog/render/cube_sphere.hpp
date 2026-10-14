#pragma once

#include <hog/core/math.hpp>

#include <cstdint>
#include <vector>

namespace hog::render {

struct Mesh {
    std::vector<Vec3> positions;
    // Where each vertex lies on its face: s and t from 0 to 1 along the face's u and v, then the
    // face. Cells are evenly spaced in s and t, so a texture per face has a texel per cell.
    std::vector<Vec3> face_points;
    // How each position moves as s and as t grow, for lifting the surface off the sphere. Their
    // cross product points outwards.
    std::vector<Vec3> along_s;
    std::vector<Vec3> along_t;
    std::vector<std::uint32_t> indices; // triangles, counter-clockwise seen from outside
};

// A unit sphere from the planet's six cube faces, each split `cells` times along both axes.
// Vertices lie on the planet's cell corners at the level with `cells` cells across a face, so a
// texture or field baked per cell lines up with the mesh. Faces keep their own edge vertices.
[[nodiscard]] Mesh cube_sphere(int cells);

} // namespace hog::render
