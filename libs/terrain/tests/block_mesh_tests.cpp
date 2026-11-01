#include <hog/terrain/block_mesh.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using hog::terrain::BlockMesh;
using hog::terrain::BlockVertex;

TEST_CASE("a triangle wearing two tiles gets corners of its own naming both", "[terrain][blocks]") {
    const auto vertex = [](float x, std::uint8_t tile) {
        return BlockVertex{.position = {x, 0.0F, 0.0F},
                           .normal = {0.0F, 0.0F, 1.0F},
                           .tiles = {tile, tile, tile},
                           .corner = 0};
    };
    // Two triangles sharing an edge: the first all granite, the second with a water corner.
    BlockMesh mesh{.origin = {},
                   .vertices = {vertex(0.0F, 9), vertex(1.0F, 9), vertex(2.0F, 9), vertex(3.0F, 1)},
                   .indices = {0, 1, 2, 2, 1, 3}};
    hog::terrain::mix_corners(mesh);
    REQUIRE(mesh.vertices.size() == 7);
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2, 4, 5, 6});
    for (std::uint8_t corner = 0; corner < 3; ++corner) {
        const BlockVertex& own = mesh.vertices[4 + corner];
        CHECK(own.tiles == std::array<std::uint8_t, 3>{9, 9, 1});
        CHECK(own.corner == corner);
    }
    CHECK(mesh.vertices[4].position.x == 2.0F);
    CHECK(mesh.vertices[5].position.x == 1.0F);
    CHECK(mesh.vertices[6].position.x == 3.0F);
    // Nothing to do when every triangle wears one tile.
    hog::terrain::mix_corners(mesh);
    CHECK(mesh.vertices.size() == 7);
}
