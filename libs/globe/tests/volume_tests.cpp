#include "bed.hpp"

#include <hog/globe/volume.hpp>
#include <hog/globe/volume_build.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <utility>
#include <vector>

using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::globe::VoxelTree;
using hog::globe::bed::earth;
using hog::globe::bed::somewhere;
using hog::globe::Cell;
using hog::globe::Generator;
using hog::globe::VoxelStore;
using hog::globe::World;

namespace {

Cube full_of(Material material) {
    return {.solid = material, .solid_level = Cube::levels};
}

// Rock up to `top` across a few tiles, either side of a tile edge.
Volume rock_to(std::uint32_t top) {
    Volume volume;
    for (std::uint32_t i = 60; i < 70; ++i) {
        for (std::uint32_t layer = 0; layer <= top; ++layer) {
            volume.set({.face = 2, .i = i, .j = 5, .layer = layer}, full_of(Material::Granite));
        }
    }
    return volume;
}

} // namespace

TEST_CASE("a volume holds no air", "[globe][volume]") {
    Volume volume;
    CHECK(volume.at({.face = 1, .i = 9, .j = 9, .layer = 9}) == Cube{});
    volume.set({.face = 1, .i = 9, .j = 9, .layer = 9}, Cube{});
    CHECK(volume.tiles() == 0);
    volume.set({.face = 1, .i = 9, .j = 9, .layer = 9}, full_of(Material::Basalt));
    CHECK(volume.tiles() == 1);
    CHECK(volume.at({.face = 1, .i = 9, .j = 9, .layer = 9}).solid == Material::Basalt);
    volume.remove({.face = 1, .i = 9, .j = 9, .layer = 9});
    CHECK(volume.tiles() == 0);
}

TEST_CASE("dirt is put on the rock as a voxel on top, and taken off by removing it",
          "[globe][volume]") {
    Volume volume = rock_to(70);
    const Site rock{.face = 2, .i = 63, .j = 5, .layer = 70};
    const Site over{.face = 2, .i = 63, .j = 5, .layer = 71};
    REQUIRE(volume.put_on(rock, full_of(Material::Soil)));
    CHECK(volume.at(rock).solid == Material::Granite);
    CHECK(volume.at(over).solid == Material::Soil);
    // Nothing goes on air, or where something already is.
    CHECK_FALSE(volume.put_on({.face = 2, .i = 63, .j = 5, .layer = 80}, full_of(Material::Soil)));
    CHECK_FALSE(volume.put_on(rock, full_of(Material::Clay)));
    volume.remove(over);
    CHECK(volume.at(over) == Cube{});
    CHECK(volume.at(rock).solid == Material::Granite);
}

TEST_CASE("a volume comes back from its bytes as it was", "[globe][volume]") {
    Volume volume = rock_to(70);
    REQUIRE(volume.put_on({.face = 2, .i = 64, .j = 5, .layer = 70}, full_of(Material::Clay)));
    const std::vector<std::byte> bytes = volume.encode();
    auto again = Volume::decode(bytes);
    REQUIRE(again);
    CHECK(again->same_as(volume));
    CHECK(again->at({.face = 2, .i = 64, .j = 5, .layer = 71}).solid == Material::Clay);
    std::vector<std::byte> cut = bytes;
    cut.pop_back();
    CHECK_FALSE(Volume::decode(cut));
}

TEST_CASE("the generator's voxels are made into the volume once, its air left out",
          "[globe][volume]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    const Cell cell = hog::globe::cell_under(somewhere(3), layers.level);
    const auto i = cell.i / Volume::tile_side;
    const auto j = cell.j / Volume::tile_side;
    static_cast<void>(hog::globe::generate_tiles(generator, cell.face, i + 1, j));
    const auto t0 = std::chrono::steady_clock::now();
    const Volume volume = hog::globe::generate_tiles(generator, cell.face, i, j);
    std::fprintf(stderr, "tile column %.3f s, %zu tiles, %zu bytes, %d layers\n",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                 volume.tiles(), volume.encode().size(), layers.count);
    REQUIRE(volume.tiles() > 0);
    // The tile the ground lies in, as building from every one of its voxels gives it.
    const std::vector<Generator::Column> columns =
        generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                           .face = cell.face,
                           .i = i * Volume::tile_side,
                           .j = j * Volume::tile_side},
                          Volume::tile_side, hog::globe::voxel_lod(generator));
    const auto bottom = static_cast<std::uint32_t>(layers.layer_at(columns.front().ground())) /
                        Volume::tile_side * Volume::tile_side;
    const VoxelTree every = VoxelTree::build(Volume::tile_side, [&](int x, int y, int z) {
        const hog::globe::Voxel made = hog::globe::generated_at(
            columns[(static_cast<std::size_t>(y) * Volume::tile_side) + static_cast<std::size_t>(x)],
            layers, bottom + static_cast<std::uint32_t>(z));
        return hog::globe::cube_of(made, made.temperature / 8.0);
    });
    const VoxelTree* built = volume.tile_at(
        {.face = cell.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side, .layer = bottom});
    REQUIRE(built != nullptr);
    CHECK(built->same_as(every));
    CHECK(built->leaves() < every.unshared_nodes());
    VoxelStore store{layers};
    const World world{generator, store};
    for (std::uint32_t a = 0; a < 64; a += 21) {
        for (std::uint32_t layer = 0; std::cmp_less(layer, layers.count); layer += 37) {
            const Site site{.face = cell.face,
                            .i = (i * Volume::tile_side) + a,
                            .j = (j * Volume::tile_side) + 63 - a,
                            .layer = layer};
            const hog::globe::Voxel made = world.at(site);
            CAPTURE(a, layer);
            CHECK(volume.at(site) == hog::globe::cube_of(made, made.temperature / 8.0));
        }
    }
}

TEST_CASE("the working world's tile columns, timed and sized", "[.time]") {
    auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const Generator generator{*planet};
    const hog::globe::Layers& layers = generator.layers();
    const Cell cell = hog::globe::cell_under(somewhere(3), layers.level);
    constexpr std::uint32_t count = 8;
    double seconds = 0.0;
    std::size_t tiles = 0;
    std::size_t bytes = 0;
    for (std::uint32_t n = 0; n < count; ++n) {
        const auto started = std::chrono::steady_clock::now();
        const Volume volume = hog::globe::generate_tiles(
            generator, cell.face, (cell.i / Volume::tile_side) + n, cell.j / Volume::tile_side);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        tiles += volume.tiles();
        bytes += volume.encode().size();
    }
    std::printf("%d layers: %.3f s, %zu tiles and %zu bytes a tile column\n", layers.count,
                seconds / count, tiles / count, bytes / count);
}
