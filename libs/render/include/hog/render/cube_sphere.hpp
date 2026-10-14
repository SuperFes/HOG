#pragma once

#include <hog/core/math.hpp>

#include <cstdint>
#include <vector>

namespace hog::render {

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices; // triangles, counter-clockwise seen from outside
};

// A unit sphere from the planet's six cube faces, each split `cells` times along both axes.
// Vertices lie on the planet's cell corners at the level with `cells` cells across a face, so a
// texture or field baked per cell lines up with the mesh. Faces keep their own edge vertices.
[[nodiscard]] Mesh cube_sphere(int cells);

} // namespace hog::render
