#include "test_planet.hpp"

#include <hog/client/column_meshes.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using hog::client::ColumnMeshes;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::globe::Voxel;
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

TEST_CASE("a tile column's faces are drawn as ground, and water still or the sea's own",
          "[client][columns]") {
    VoxelMesh mesh{.origin = {1000.0, 0.0, 0.0}, .vertices = {}, .indices = {}};
    add_face(mesh, 1000.0, Material::Granite);
    add_face(mesh, 1000.2, Material::Water);
    add_face(mesh, 1040.0, Material::Water);
    const hog::client::ColumnDrawn drawn = hog::client::drawn_of(mesh, 1000.0, 1.0);
    REQUIRE(drawn.ground.vertices.size() == 4);
    CHECK(drawn.ground.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    CHECK(drawn.ground.vertices.front().tiles[0] == static_cast<std::uint8_t>(Material::Granite));
    REQUIRE(drawn.water.vertices.size() == 8);
    CHECK(drawn.water.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7});
    CHECK(drawn.water.vertices[0].corner == hog::client::sea_corner);
    CHECK(drawn.water.vertices[4].corner == 0);
    CHECK(drawn.ground.origin == hog::WorldPosition{mesh.origin});
}

TEST_CASE("tile columns are meshed as they're held, and gone once they're let go",
          "[client][columns]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    Volume volume;
    const hog::globe::World world{made.generator.planet(), made.generator.layers(), volume};
    const Site corner{.face = 3, .i = 64 * 20, .j = 64 * 30};
    volume.set_floor(corner, 10);
    REQUIRE(volume.set({.face = 3, .i = corner.i + 4, .j = corner.j + 9, .layer = 12},
                       {.solid = Material::Basalt, .solid_amount = Voxel::full}));
    hog::jobs::JobSystem jobs{2};
    ColumnMeshes meshes{world, jobs};
    const std::vector<Site> changed{corner};
    meshes.changed(changed);
    meshes.finish();
    CHECK(meshes.in_flight() == 0);
    auto changes = meshes.collect();
    REQUIRE(changes.shown.size() == 1);
    CHECK(changes.shown.front().key == hog::client::column_key(corner));
    CHECK_FALSE(changes.shown.front().ground->vertices.empty());
    CHECK(changes.shown.front().water->vertices.empty());
    CHECK(changes.gone.empty());
    CHECK(meshes.collect().empty());

    volume.drop_column(corner);
    meshes.changed(changed);
    changes = meshes.collect();
    CHECK(changes.shown.empty());
    CHECK(changes.gone == std::vector{hog::client::column_key(corner)});
}

TEST_CASE(
    "following the eye meshes the tile columns held near it, and lets go of those left behind",
    "[client][columns]") {
    const hog::client::GlobeWorld& made = hog::client::testing::globe();
    const hog::globe::Layers& layers = made.generator.layers();
    Volume volume;
    const hog::globe::World world{made.generator.planet(), layers, volume};
    const hog::DVec3 here =
        glm::normalize(hog::DVec3{0.3, 0.4, 0.86}) * made.generator.planet().radius;
    const std::vector<Site> near = hog::globe::tile_columns_near(layers, here, 300.0);
    for (const Site corner : near) {
        volume.set_floor(corner, 10);
        REQUIRE(volume.set({.face = corner.face, .i = corner.i, .j = corner.j, .layer = 10},
                           {.solid = Material::Basalt, .solid_amount = Voxel::full}));
    }
    hog::jobs::JobSystem jobs{2};
    ColumnMeshes meshes{world, jobs};
    meshes.follow(here, 300.0);
    meshes.finish();
    auto changes = meshes.collect();
    CHECK(changes.shown.size() == near.size());
    CHECK(changes.gone.empty());
    // Again from the same place, nothing more.
    meshes.follow(here, 300.0);
    CHECK(meshes.collect().empty());
    // Far off, all of it's gone.
    meshes.follow(-here, 300.0);
    changes = meshes.collect();
    CHECK(changes.shown.empty());
    CHECK(changes.gone.size() == near.size());
}
