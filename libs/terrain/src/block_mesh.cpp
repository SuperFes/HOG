#include <hog/core/assert.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hog::terrain {

void mix_corners(BlockMesh& mesh) {
    HOG_ASSERT(mesh.indices.size() % 3 == 0, "blocks are triangles");
    for (std::size_t first = 0; first < mesh.indices.size(); first += 3) {
        const std::array<std::uint8_t, 3> tiles{mesh.vertices[mesh.indices[first]].tiles[0],
                                                mesh.vertices[mesh.indices[first + 1]].tiles[0],
                                                mesh.vertices[mesh.indices[first + 2]].tiles[0]};
        if (tiles[0] == tiles[1] && tiles[1] == tiles[2]) {
            continue;
        }
        for (std::uint8_t corner = 0; corner < 3; ++corner) {
            std::uint32_t& index = mesh.indices[first + corner];
            BlockVertex own = mesh.vertices[index];
            own.tiles = tiles;
            own.corner = at_corner(own.corner, corner);
            index = static_cast<std::uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(own);
        }
    }
}

} // namespace hog::terrain
