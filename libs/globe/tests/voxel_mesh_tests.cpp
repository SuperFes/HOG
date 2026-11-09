#include "bed.hpp"

#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
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

// The tile column at `middle` and the eight about it, held.
void hold_about(Volume& volume, const TileColumn& column) {
    for (std::uint32_t j = 9; j <= 11; ++j) {
        for (std::uint32_t i = 9; i <= 11; ++i) {
            REQUIRE(
                volume.decode_column({.face = 2, .i = i * tile, .j = j * tile}, column.encode()));
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
    hold_about(volume, flat());
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
    hold_about(volume, flat(true));
    const VoxelMesh mesh = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(mesh) == std::size_t{2} * tile * tile);
    CHECK(std::ranges::count(mesh.vertices, Material::Water, &hog::globe::VoxelVertex::material) ==
          std::ptrdiff_t{4} * tile * tile);
}

TEST_CASE("water stands as high as it fills its voxel, as one sheet with the water beside it",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
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
    hold_about(volume, shallow);
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

    // Filled to half, one voxel's water is no step over its neighbours': the sheet rises to the
    // water about each of its corners, whole, with no sides.
    const Site deeper{.face = 2, .i = middle.i + 5, .j = middle.j + 7, .layer = ground + 1};
    REQUIRE(volume.set(deeper, {.liquid = Material::Water,
                                .liquid_amount = static_cast<std::uint16_t>(Voxel::full / 2)}));
    const VoxelMesh stepped = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(stepped) == quads(mesh));
    const std::vector<double> tops = water_tops(stepped);
    CHECK(std::ranges::count_if(tops, [&](double top) {
              return std::abs(top - (bottom + (0.3125 * layers.thickness))) < 0.01;
          }) == 16);
    CHECK(std::ranges::none_of(tops, [&](double top) {
        return std::abs(top - (bottom + (0.5 * layers.thickness))) < 0.01;
    }));
    // Every corner the sheet's quads share stands at one height.
    std::map<std::array<std::int64_t, 3>, double> heights;
    for (const hog::globe::VoxelVertex& vertex : stepped.vertices) {
        if (vertex.material != Material::Water) {
            continue;
        }
        const hog::DVec3 at = stepped.origin + hog::DVec3{vertex.position};
        const hog::DVec3 way = glm::normalize(at) * 1e6;
        const std::array<std::int64_t, 3> key{std::llround(way.x), std::llround(way.y),
                                              std::llround(way.z)};
        const auto [found, added] = heights.try_emplace(key, glm::length(at));
        CHECK(std::abs(found->second - glm::length(at)) < 1e-3);
    }

    // Too little to stand in it, it doesn't show: the sheet has a hole there, and no walls.
    REQUIRE(volume.set(deeper, {.liquid = Material::Water,
                                .liquid_amount = static_cast<std::uint16_t>(Voxel::full / 32)}));
    const VoxelMesh wet = hog::globe::mesh_tile_column(world, middle, 0);
    CHECK(quads(wet) == quads(mesh) - 1);
}

TEST_CASE("ground stands as high as its matter, keeping out water lower than it, and hangs as "
          "low",
          "[globe][voxel_mesh]") {
    Volume volume;
    const hog::globe::World world{earth(), volume};
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
    hold_about(volume, sea);
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

TEST_CASE("a tile meshes from its own voxels, reading the tiles beside it where there are any",
          "[globe][voxel_mesh]") {
    const hog::globe::Layers& layers = earth().layers();
    const VoxelTree rock = flat().tiles.front().second;
    const hog::globe::TileKey key{.level = 0, .face = 2, .i = 10, .j = 10, .k = 0};
    const VoxelMesh mesh = hog::globe::mesh_tile(layers, key, rock);
    CHECK(quads(mesh) == std::size_t{tile} * tile);
    const double top = layers.bottom(static_cast<int>(ground) + 1);
    for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
        const hog::DVec3 at = mesh.origin + hog::DVec3{vertex.position};
        CHECK(std::abs(glm::length(at) - top) < 0.01);
        CHECK(glm::dot(hog::DVec3{vertex.normal}, glm::normalize(at)) > 0.99);
    }
    CHECK(hog::globe::mesh_tile(layers, key, VoxelTree{Volume::tile_side}).indices.empty());

    // Open air beside it across: its rock shows its side there.
    const VoxelTree air{Volume::tile_side};
    const auto beside = [&](hog::globe::TileKey at) -> const VoxelTree* {
        return at.i == key.i + 1 && at.j == key.j && at.k == key.k ? &air : nullptr;
    };
    CHECK(quads(hog::globe::mesh_tile(layers, key, rock, beside)) ==
          quads(mesh) + (std::size_t{tile} * (ground + 1)));

    // Rock filled to its top with nothing known over it goes on up, showing no lid; held only
    // coarser beside it, the tile's read from that: under air held two levels coarser it shows its
    // top, and its side to air held coarser across.
    const VoxelTree full = VoxelTree::build(Volume::tile_side, [](int, int, int) {
        return Cube{.solid = Material::Granite, .solid_amount = Voxel::full};
    });
    const auto coarse_air = [&](hog::globe::TileKey at) -> const VoxelTree* {
        const hog::globe::TileKey over = key.parent().parent();
        return at.level == 2 && at.face == key.face && (at.k == over.k + 1 || at.i == over.i + 1)
                   ? &air
                   : nullptr;
    };
    const hog::globe::TileKey corner{.level = 0, .face = 2, .i = 10, .j = 10, .k = 3};
    CHECK(hog::globe::mesh_tile(layers, corner, full).indices.empty());
    CHECK(quads(hog::globe::mesh_tile(layers, corner, full, coarse_air)) ==
          std::size_t{tile} * tile);
    const hog::globe::TileKey edge{.level = 0, .face = 2, .i = 11, .j = 10, .k = 0};
    CHECK(quads(hog::globe::mesh_tile(layers, edge, full, coarse_air)) == std::size_t{tile} * tile);

    // Coarser, each voxel stands for a cube of them as wide.
    const hog::globe::TileKey coarse{.level = 2, .face = 2, .i = 2, .j = 2, .k = 0};
    const VoxelMesh wide = hog::globe::mesh_tile(layers, coarse, rock);
    CHECK(quads(wide) == std::size_t{tile} * tile);
    const double coarse_top = layers.bottom((static_cast<int>(ground) + 1) * 4);
    for (const hog::globe::VoxelVertex& vertex : wide.vertices) {
        CHECK(std::abs(glm::length(wide.origin + hog::DVec3{vertex.position}) - coarse_top) < 0.01);
    }
}

TEST_CASE("a tile beside which only finer tiles are held reads them where they meet it",
          "[globe][voxel_mesh]") {
    const hog::globe::Layers& layers = earth().layers();
    const VoxelTree full = VoxelTree::build(Volume::tile_side, [](int, int, int) {
        return Cube{.solid = Material::Granite, .solid_amount = Voxel::full};
    });
    const VoxelTree air{Volume::tile_side};
    // Rock a level coarser, the place across it held as air a level finer, all of it or its lower
    // half: the rock shows its side to that air, and only there.
    const hog::globe::TileKey key{.level = 1, .face = 2, .i = 5, .j = 5, .k = 1};
    const auto finer_air = [&](bool all) {
        return [&, all](hog::globe::TileKey at) -> const VoxelTree* {
            const bool across = at.level == 0 && at.face == key.face && at.i == (key.i * 2) + 2 &&
                                at.j / 2 == key.j && at.k / 2 == key.k;
            return across && (all || at.k == key.k * 2) ? &air : nullptr;
        };
    };
    CHECK(hog::globe::mesh_tile(layers, key, full).indices.empty());
    CHECK(quads(hog::globe::mesh_tile(layers, key, full, finer_air(true))) ==
          std::size_t{tile} * tile);
    CHECK(quads(hog::globe::mesh_tile(layers, key, full, finer_air(false))) ==
          std::size_t{tile} * tile / 2);
}

TEST_CASE("water over ground soaked through stands on it, its pores' water unseen",
          "[globe][voxel_mesh]") {
    const auto soaked = VoxelTree::build(Volume::tile_side, [](int, int, int z) {
        if (z < 10) {
            return Cube{.solid = Material::Granite,
                        .solid_amount = Voxel::full,
                        .liquid = Material::Water,
                        .liquid_amount = Voxel::full / 8};
        }
        if (z < 13) {
            return Cube{.liquid = Material::Water, .liquid_amount = Voxel::full};
        }
        return Cube{};
    });
    const VoxelMesh mesh =
        hog::globe::mesh_tile(earth().layers(), {.level = 4, .face = 1, .i = 3, .j = 3}, soaked);
    CHECK(std::ranges::count(mesh.vertices, Material::Water, &hog::globe::VoxelVertex::material) ==
          4 * tile * tile);
    const double surface = earth().layers().bottom(13 * 16);
    for (const hog::globe::VoxelVertex& vertex : mesh.vertices) {
        if (vertex.material == Material::Water) {
            CHECK(std::abs(glm::length(mesh.origin + hog::DVec3{vertex.position}) - surface) <
                  0.01);
        }
    }
}

TEST_CASE("the sea meets itself level across a tile held a level coarser beside it",
          "[globe][voxel_mesh]") {
    const hog::globe::Layers& layers = earth().layers();
    const auto share = [](double of) {
        return static_cast<std::uint16_t>(of * static_cast<double>(Voxel::full));
    };
    // Water to 20.3 voxels up, finely, and the same water a level coarser: to 10.15 of its voxels.
    const VoxelTree fine = VoxelTree::build(Volume::tile_side, [&](int, int, int z) {
        if (z < 20) {
            return Cube{.liquid = Material::Water, .liquid_amount = Voxel::full};
        }
        return z == 20 ? Cube{.liquid = Material::Water, .liquid_amount = share(0.3)} : Cube{};
    });
    const VoxelTree coarse = VoxelTree::build(Volume::tile_side, [&](int, int, int z) {
        if (z < 10) {
            return Cube{.liquid = Material::Water, .liquid_amount = Voxel::full};
        }
        return z == 10 ? Cube{.liquid = Material::Water, .liquid_amount = share(0.15)} : Cube{};
    });
    const hog::globe::TileKey key{.level = 0, .face = 2, .i = 9, .j = 10, .k = 0};
    const hog::globe::TileKey beside{.level = 1, .face = 2, .i = 5, .j = 5, .k = 0};
    const VoxelMesh mesh =
        hog::globe::mesh_tile(layers, key, fine, [&](hog::globe::TileKey at) -> const VoxelTree* {
            return at == beside ? &coarse : nullptr;
        });
    const double surface = layers.bottom(20) + (0.3 * layers.thickness);
    std::size_t tops = 0;
    for (std::size_t face = 0; face + 3 < mesh.vertices.size(); face += 4) {
        const hog::globe::VoxelVertex& vertex = mesh.vertices[face];
        if (vertex.material != Material::Water) {
            continue;
        }
        const hog::DVec3 at = mesh.origin + hog::DVec3{vertex.position};
        const double facing = glm::dot(hog::DVec3{vertex.normal}, glm::normalize(at));
        CHECK(std::abs(facing) > 0.5);
        if (facing > 0.5) {
            ++tops;
            for (std::size_t k = 0; k < 4; ++k) {
                const hog::DVec3 corner =
                    mesh.origin + hog::DVec3{mesh.vertices[face + k].position};
                CHECK(std::abs(glm::length(corner) - surface) < 0.01);
            }
        }
    }
    CHECK(tops == std::size_t{tile} * tile);
}
