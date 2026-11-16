#include <hog/client/drawn.hpp>
#include <hog/globe/voxel_mesh.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using hog::globe::Material;
using hog::globe::VoxelMesh;

// A face of four corners `metres` from the planet's centre, of `material`.
void add_face(VoxelMesh& mesh, double metres, Material material) {
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (const hog::DVec3 offset : {hog::DVec3{0.0, 0.0, 0.0}, hog::DVec3{0.0, 1.0, 0.0},
                                    hog::DVec3{0.0, 1.0, 1.0}, hog::DVec3{0.0, 0.0, 1.0}}) {
        mesh.vertices.push_back(
            {.position = hog::Vec3{hog::DVec3{metres, 0.0, 0.0} + offset - mesh.origin},
             .normal = hog::Vec3{1.0F, 0.0F, 0.0F},
             .material = material});
    }
    for (const std::uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        mesh.indices.push_back(base + corner);
    }
}

} // namespace

TEST_CASE("a tile's faces are drawn as ground, and water still or the sea's own",
          "[client][drawn]") {
    VoxelMesh mesh{.origin = {1000.0, 0.0, 0.0}, .vertices = {}, .indices = {}};
    add_face(mesh, 1000.0, Material::Granite);
    add_face(mesh, 1000.2, Material::Water);
    add_face(mesh, 1040.0, Material::Water);
    const hog::client::Drawn drawn = hog::client::drawn_of(mesh, 1000.0, 1.0);
    REQUIRE(drawn.ground.vertices.size() == 4);
    CHECK(drawn.ground.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    CHECK(drawn.ground.vertices.front().tiles[0] == static_cast<std::uint8_t>(Material::Granite));
    REQUIRE(drawn.water.vertices.size() == 8);
    CHECK(drawn.water.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7});
    CHECK((drawn.water.vertices[0].corner & hog::client::sea_corner) != 0);
    CHECK(drawn.water.vertices[4].corner == 0);
    // The sea's own stands at its level, the rest where the mesh has it.
    for (std::size_t k = 0; k < 4; ++k) {
        CHECK(glm::length(mesh.origin + hog::DVec3{drawn.water.vertices[k].position}) ==
              Catch::Approx(1000.0).margin(1e-3));
    }
    CHECK(glm::length(mesh.origin + hog::DVec3{drawn.water.vertices[4].position}) ==
          Catch::Approx(1040.0).margin(1e-2));
    CHECK(drawn.ground.origin == hog::WorldPosition{mesh.origin});
}

TEST_CASE("still water's corners say how it rises through the year; the ground's name its tile",
          "[client][drawn]") {
    VoxelMesh mesh{.origin = {1000.0, 0.0, 0.0}, .vertices = {}, .indices = {}};
    add_face(mesh, 1000.0, Material::Granite);
    add_face(mesh, 1040.0, Material::Water);
    for (std::size_t k = 4; k < 8; ++k) {
        mesh.vertices[k].rise = 31;
        mesh.vertices[k].peak = 140;
    }
    const hog::client::Drawn drawn = hog::client::drawn_of(mesh, std::nullopt, 1.0);
    const auto granite = static_cast<std::uint8_t>(Material::Granite);
    CHECK(drawn.ground.vertices.front().tiles ==
          std::array<std::uint8_t, 3>{granite, granite, granite});
    REQUIRE(drawn.water.vertices.size() == 4);
    for (const hog::terrain::BlockVertex& vertex : drawn.water.vertices) {
        CHECK(vertex.tiles[0] == static_cast<std::uint8_t>(Material::Water));
        CHECK(vertex.tiles[1] == 31);
        CHECK(vertex.tiles[2] == 140);
    }
}
