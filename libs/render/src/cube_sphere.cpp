#include <hog/core/assert.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/cube_sphere.hpp>

#include <cstddef>

namespace hog::render {

Mesh cube_sphere(int cells) {
    HOG_ASSERT(cells > 0, "a cube sphere needs at least one cell across a face");
    const auto across = static_cast<std::uint32_t>(cells) + 1;
    Mesh mesh;
    mesh.positions.reserve(std::size_t{6} * across * across);
    mesh.indices.reserve(std::size_t{6} * 6 * static_cast<std::size_t>(cells) *
                         static_cast<std::size_t>(cells));
    for (int face = 0; face < 6; ++face) {
        const auto first = static_cast<std::uint32_t>(mesh.positions.size());
        for (std::uint32_t j = 0; j < across; ++j) {
            const double v = planet::st_to_uv(static_cast<double>(j) / cells);
            for (std::uint32_t i = 0; i < across; ++i) {
                const double u = planet::st_to_uv(static_cast<double>(i) / cells);
                mesh.positions.emplace_back(glm::normalize(planet::face_uv_to_xyz(face, u, v)));
            }
        }
        // Corners run counter-clockwise from outside in i then j, as the planet's cells do.
        for (std::uint32_t j = 0; j + 1 < across; ++j) {
            for (std::uint32_t i = 0; i + 1 < across; ++i) {
                const std::uint32_t corner = first + (j * across) + i;
                const std::uint32_t right = corner + 1;
                const std::uint32_t above = corner + across;
                mesh.indices.insert(mesh.indices.end(),
                                    {corner, right, above + 1, corner, above + 1, above});
            }
        }
    }
    return mesh;
}

} // namespace hog::render
