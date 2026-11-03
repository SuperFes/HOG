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

TEST_CASE("a corner's byte keeps which corner it is and how wet, through mixing",
          "[terrain][blocks]") {
    using hog::terrain::corner_of;
    using hog::terrain::packed_corner;
    using hog::terrain::wetness_of;
    CHECK(packed_corner(0, 0.0) == 0);
    CHECK(packed_corner(2, 0.0) == 2);
    CHECK(packed_corner(1, 1.0) == 189);
    CHECK(packed_corner(1, 2.0) == 189);
    CHECK(corner_of(packed_corner(2, 0.5)) == 2);
    CHECK(wetness_of(packed_corner(2, 0.5)) > 0.48);
    CHECK(wetness_of(packed_corner(2, 0.5)) < 0.52);
    // Hot ground is dry, and any glow at all shows.
    CHECK(hog::terrain::glow_of(packed_corner(1, 0.0, 0.01)) > 0.0);
    CHECK(hog::terrain::glow_of(packed_corner(1, 0.0, 1.0)) == 1.0);
    CHECK(wetness_of(packed_corner(1, 0.7, 1.0)) == 0.0);
    CHECK(hog::terrain::glow_of(packed_corner(1, 0.7)) == 0.0);
    CHECK(corner_of(hog::terrain::at_corner(packed_corner(0, 0.0, 0.5), 2)) == 2);
    CHECK(wetness_of(packed_corner(0, 1.0)) == 1.0);
    const auto vertex = [](float x, std::uint8_t tile, double wet) {
        return BlockVertex{.position = {x, 0.0F, 0.0F},
                           .normal = {0.0F, 0.0F, 1.0F},
                           .tiles = {tile, tile, tile},
                           .corner = packed_corner(0, wet)};
    };
    BlockMesh mesh{.origin = {},
                   .vertices = {vertex(0.0F, 9, 1.0), vertex(1.0F, 9, 0.0), vertex(2.0F, 1, 0.5)},
                   .indices = {0, 1, 2}};
    hog::terrain::mix_corners(mesh);
    REQUIRE(mesh.vertices.size() == 6);
    CHECK(wetness_of(mesh.vertices[3].corner) == 1.0);
    CHECK(wetness_of(mesh.vertices[4].corner) == 0.0);
    CHECK(wetness_of(mesh.vertices[5].corner) > 0.48);
    for (std::uint8_t corner = 0; corner < 3; ++corner) {
        CHECK(corner_of(mesh.vertices[3 + corner].corner) == corner);
    }
}
