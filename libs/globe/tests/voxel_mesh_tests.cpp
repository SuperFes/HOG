#include "bed.hpp"

#include <hog/globe/column_cache.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::TileColumn;
using hog::globe::Volume;
using hog::globe::Voxel;
using hog::globe::VoxelMesh;
using hog::globe::VoxelTree;
using hog::globe::bed::earth;

constexpr std::uint32_t ground = 40; // the highest layer of rock
constexpr std::uint32_t tile = 64;
const Site middle{.face = 2, .i = 10 * tile, .j = 10 * tile};

// Rock up to `ground` right across a tile column, with water a layer deep over it if `wet`.
TileColumn flat(bool wet = false) {
    return {.floor = 0,
            .tiles = {{0, VoxelTree::build(Volume::tile_side, [&](int, int, int z) {
                           if (std::cmp_less_equal(z, ground)) {
                               return Cube{.solid = Material::Granite, .solid_amount = Voxel::full};
                           }
                           if (wet && std::cmp_equal(z, ground + 1)) {
                               return Cube{.liquid = Material::Water, .liquid_amount = Voxel::full};
                           }
                           return Cube{};
                       })}}};
}

// The tile column at `middle` and the eight about it, held at `level`.
void hold_about(hog::globe::ColumnCache& cache, const TileColumn& column, std::uint8_t level) {
    TileColumn sent = column;
    for (std::uint8_t k = 0; k < level; ++k) {
        sent = sent.halved();
    }
    for (std::uint32_t j = 9; j <= 11; ++j) {
        for (std::uint32_t i = 9; i <= 11; ++i) {
            REQUIRE(cache.take(2, i, j, level, sent.encode()));
        }
    }
}

std::size_t quads(const VoxelMesh& mesh) {
    return mesh.indices.size() / 6;
}

} // namespace

TEST_CASE("flat ground meshes to its top faces alone, facing up, where its rock ends",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    hold_about(cache, flat(), 0);
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(mesh) == std::size_t{tile} * tile);
    const double top = earth().layers().bottom(static_cast<int>(ground) + 1);
    for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
        const hog::DVec3 at = mesh.origin + hog::DVec3{vertex.position};
        CHECK(std::abs(glm::length(at) - top) < 0.01);
        CHECK(glm::dot(hog::DVec3{vertex.normal}, glm::normalize(at)) > 0.99);
        CHECK(vertex.material == Material::Granite);
    }

    // A voxel stood on it shows its four sides and top, and hides the ground under it.
    REQUIRE(volume.set({.face = 2, .i = middle.i + 5, .j = middle.j + 7, .layer = ground + 1},
                       {.solid = Material::Basalt, .solid_amount = Voxel::full}));
    const VoxelMesh stood = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(stood) == quads(mesh) - 1 + 5);
    CHECK(std::ranges::count(stood.vertices, Material::Basalt,
                             &hog::globe::VoxelVertex::material) == 5 * 4);
}

TEST_CASE("water shows its top to the open, and the ground under it shows through it",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    hold_about(cache, flat(true), 0);
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(mesh) == std::size_t{2} * tile * tile);
    CHECK(std::ranges::count(mesh.vertices, Material::Water, &hog::globe::VoxelVertex::material) ==
          std::ptrdiff_t{4} * tile * tile);
}

TEST_CASE("a tile column held coarser meshes as its coarser voxels", "[globe][voxel_mesh]") {
    Volume volume;
    hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    world.read_coarse(&cache);
    hold_about(cache, flat(), 2);
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 2);
    CHECK(quads(mesh) == std::size_t{tile / 4} * (tile / 4));
    // Rock to layer 40 fills ten coarse voxels and a quarter of the eleventh, which is open.
    const double top = earth().layers().bottom(40);
    for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
        CHECK(std::abs(glm::length(mesh.origin + hog::DVec3{vertex.position}) - top) < 0.01);
    }
    // Not held at that level, or at all, there's nothing to mesh.
    CHECK(hog::globe::mesh_tile_column(world, middle, 1).vertices.empty());
    CHECK(hog::globe::mesh_tile_column(world, {.face = 2, .i = 30 * tile, .j = 4 * tile}, 0)
              .vertices.empty());
}
