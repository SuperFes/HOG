#include "bed.hpp"

#include <hog/globe/column_cache.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

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

TEST_CASE("water stands as high as it fills its voxel, showing its side over shallower water",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    TileColumn shallow = flat();
    const auto quarter = static_cast<std::uint16_t>(Voxel::full / 4);
    shallow.tiles.front().second = VoxelTree::build(Volume::tile_side, [&](int, int, int z) {
        if (std::cmp_less_equal(z, ground)) {
            return Cube{.solid = Material::Granite, .solid_amount = Voxel::full};
        }
        if (std::cmp_equal(z, ground + 1)) {
            return Cube{.liquid = Material::Water, .liquid_amount = quarter};
        }
        return Cube{};
    });
    hold_about(cache, shallow, 0);
    const hog::globe::Layers& layers = earth().layers();
    const double bottom = layers.bottom(static_cast<int>(ground) + 1);
    const auto water_tops = [&](const VoxelMesh& mesh) {
        std::vector<double> tops;
        for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
            const hog::DVec3 at = mesh.origin + hog::DVec3{vertex.position};
            if (vertex.material == Material::Water &&
                glm::dot(hog::DVec3{vertex.normal}, glm::normalize(at)) > 0.99) {
                tops.push_back(glm::length(at));
            }
        }
        return tops;
    };
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(mesh) == std::size_t{2} * tile * tile);
    for (const double top : water_tops(mesh)) {
        CHECK(std::abs(top - (bottom + (0.25 * layers.thickness))) < 0.01);
    }

    // Filled to half, one voxel's water stands over its neighbours' by a quarter of a voxel.
    const Site deeper{.face = 2, .i = middle.i + 5, .j = middle.j + 7, .layer = ground + 1};
    REQUIRE(volume.set(deeper, {.liquid = Material::Water,
                                .liquid_amount = static_cast<std::uint16_t>(Voxel::full / 2)}));
    const VoxelMesh stepped = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(stepped) == quads(mesh) + 4);
    CHECK(std::ranges::count_if(water_tops(stepped), [&](double top) {
              return std::abs(top - (bottom + (0.5 * layers.thickness))) < 0.01;
          }) == 4);

    // Too little to stand in it, it doesn't show, and its neighbours show their sides over it.
    REQUIRE(volume.set(deeper, {.liquid = Material::Water,
                                .liquid_amount = static_cast<std::uint16_t>(Voxel::full / 32)}));
    const VoxelMesh wet = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(wet) == quads(mesh) - 1 + 4);
}

TEST_CASE("ground stands as high as its matter, keeping out water lower than it, and hangs as "
          "low",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    const auto share = [](double of) {
        return static_cast<std::uint16_t>(of * static_cast<double>(Voxel::full));
    };
    TileColumn sea = flat();
    sea.tiles.front().second = VoxelTree::build(Volume::tile_side, [&](int, int, int z) {
        if (std::cmp_less_equal(z, ground)) {
            return Cube{.solid = Material::Granite, .solid_amount = Voxel::full};
        }
        if (std::cmp_equal(z, ground + 1)) {
            return Cube{.liquid = Material::Water, .liquid_amount = share(0.21)};
        }
        return Cube{};
    });
    hold_about(cache, sea, 0);
    // Ground a third of a voxel high, over the water beside it.
    const Site dry{.face = 2, .i = middle.i + 5, .j = middle.j + 7, .layer = ground + 1};
    REQUIRE(volume.set(dry, {.solid = Material::Granite, .solid_amount = share(0.33)}));
    // A ledge two voxels up, with a share of a voxel hanging under it.
    const Site ledge{.face = 2, .i = middle.i + 20, .j = middle.j + 20, .layer = ground + 4};
    REQUIRE(volume.set(ledge, {.solid = Material::Basalt, .solid_amount = Voxel::full}));
    Site hanging = ledge;
    hanging.layer -= 1;
    REQUIRE(volume.set(hanging, {.solid = Material::Basalt, .solid_amount = share(0.4)}));

    const hog::globe::Layers& layers = earth().layers();
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 0);
    std::size_t water_sides = 0;
    std::vector<double> granite_tops;
    std::vector<double> basalt_bottoms;
    for (std::size_t face = 0; face + 3 < mesh.vertices.size(); face += 4) {
        const hog::globe::VoxelVertex& vertex = mesh.vertices[face];
        hog::DVec3 middle_of{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            middle_of += (mesh.origin + hog::DVec3{mesh.vertices[face + k].position}) / 4.0;
        }
        const double facing = glm::dot(hog::DVec3{vertex.normal}, glm::normalize(middle_of));
        const double stands = glm::length(middle_of);
        if (vertex.material == Material::Water && std::abs(facing) < 0.5) {
            ++water_sides;
        }
        if (vertex.material == Material::Granite && facing > 0.99) {
            granite_tops.push_back(stands);
        }
        if (vertex.material == Material::Basalt && facing < -0.99) {
            basalt_bottoms.push_back(stands);
        }
    }
    const double above = layers.bottom(static_cast<int>(ground) + 1);
    CHECK(water_sides == 0);
    CHECK(std::ranges::count_if(granite_tops, [&](double top) {
              return std::abs(top - (above + (0.33 * layers.thickness))) < 0.01;
          }) == 1);
    REQUIRE(basalt_bottoms.size() == 1);
    CHECK(basalt_bottoms.front() ==
          Catch::Approx(layers.bottom(static_cast<int>(ground) + 4) - (0.4 * layers.thickness))
              .margin(0.01));
}

TEST_CASE("a tile column held coarser meshes as its coarser voxels", "[globe][voxel_mesh]") {
    Volume volume;
    hog::globe::World world{earth(), volume};
    hog::globe::ColumnCache cache{volume};
    world.read_coarse(&cache);
    hold_about(cache, flat(), 2);
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 2);
    CHECK(quads(mesh) == std::size_t{tile / 4} * (tile / 4));
    // Rock to layer 40 fills ten coarse voxels and a quarter of the eleventh, which stands that
    // much higher on them: to the top of layer 40, as finer.
    const double top = earth().layers().bottom(static_cast<int>(ground) + 1);
    for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
        CHECK(std::abs(glm::length(mesh.origin + hog::DVec3{vertex.position}) - top) < 0.01);
    }
    // Not held at that level, or at all, there's nothing to mesh.
    CHECK(hog::globe::mesh_tile_column(world, middle, 1).vertices.empty());
    CHECK(hog::globe::mesh_tile_column(world, {.face = 2, .i = 30 * tile, .j = 4 * tile}, 0)
              .vertices.empty());
}
