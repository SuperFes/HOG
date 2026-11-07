#include "bed.hpp"

#include <hog/globe/volume.hpp>
#include <hog/globe/volume_build.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <utility>
#include <vector>

using hog::globe::Cell;
using hog::globe::Cube;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Volume;
using hog::globe::VoxelStore;
using hog::globe::VoxelTree;
using hog::globe::World;
using hog::globe::bed::earth;
using hog::globe::bed::somewhere;

namespace {

Cube full_of(Material material) {
    return {.solid = material, .solid_level = Cube::levels};
}

// Rock up to `top` across a few tiles, either side of a tile edge, made from the bottom.
Volume rock_to(std::uint32_t top) {
    Volume volume;
    volume.set_floor({.face = 2, .i = 0, .j = 5}, 0);
    volume.set_floor({.face = 2, .i = 64, .j = 5}, 0);
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
    volume.set_floor({.face = 1, .i = 9, .j = 9}, 0);
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

TEST_CASE("nothing is put or taken under a tile column's floor", "[globe][volume]") {
    Volume volume;
    const Site deep{.face = 3, .i = 70, .j = 70, .layer = 100};
    CHECK_FALSE(volume.made(deep));
    CHECK_FALSE(volume.set(deep, full_of(Material::Granite)));
    volume.set_floor(deep, 128);
    CHECK_FALSE(volume.made(deep));
    CHECK_FALSE(volume.set(deep, full_of(Material::Granite)));
    CHECK_FALSE(
        volume.put_on({.face = 3, .i = 70, .j = 70, .layer = 127}, full_of(Material::Soil)));
    CHECK(volume.tiles() == 0);
    // The floor is per tile column.
    CHECK_FALSE(volume.made({.face = 3, .i = 64 + 64, .j = 70, .layer = 200}));
    CHECK(volume.made({.face = 3, .i = 64, .j = 127, .layer = 128}));
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
    const Volume volume = hog::globe::generate_tiles(generator, cell.face, i, j);
    REQUIRE(volume.tiles() > 0);
    const std::vector<Generator::Column> columns =
        generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                           .face = cell.face,
                           .i = i * Volume::tile_side,
                           .j = j * Volume::tile_side},
                          Volume::tile_side, hog::globe::voxel_lod(generator));
    const Site corner{.face = cell.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side};
    // The tile from `bottom`, as building from every one of its voxels gives it, made from
    // `floor` up.
    const auto every = [&](std::uint32_t bottom, std::uint32_t floor) {
        return VoxelTree::build(Volume::tile_side, [&](int x, int y, int z) {
            const std::uint32_t layer = bottom + static_cast<std::uint32_t>(z);
            if (layer < floor) {
                return Cube{};
            }
            const hog::globe::Voxel made =
                hog::globe::generated_at(columns[(static_cast<std::size_t>(y) * Volume::tile_side) +
                                                 static_cast<std::size_t>(x)],
                                         layers, layer);
            return hog::globe::cube_of(made, made.temperature / 8.0);
        });
    };
    const auto tile_from = [](std::uint32_t layer) {
        return layer / Volume::tile_side * Volume::tile_side;
    };

    // Made from `made_depth` under the lowest ground.
    double lowest = columns.front().ground();
    for (const Generator::Column& each : columns) {
        lowest = std::min(lowest, each.ground());
    }
    const auto floor = volume.floor(corner);
    REQUIRE(floor);
    CHECK(std::cmp_equal(*floor, layers.layer_at(lowest) - hog::globe::made_depth));
    REQUIRE(*floor > Volume::tile_side);
    const Site under{.face = cell.face, .i = corner.i, .j = corner.j, .layer = *floor - 1};
    CHECK_FALSE(volume.made(under));
    CHECK(volume.made({.face = cell.face, .i = corner.i, .j = corner.j, .layer = *floor}));

    // The tile the ground lies in, and the one the floor lies in.
    const auto ground_tile =
        tile_from(static_cast<std::uint32_t>(layers.layer_at(columns.front().ground())));
    for (const std::uint32_t bottom : {ground_tile, tile_from(*floor)}) {
        CAPTURE(bottom);
        const VoxelTree* built =
            volume.tile_at({.face = cell.face, .i = corner.i, .j = corner.j, .layer = bottom});
        REQUIRE(built != nullptr);
        const VoxelTree expected = every(bottom, *floor);
        CHECK(built->same_as(expected));
        CHECK(built->leaves() < expected.unshared_nodes());
    }

    VoxelStore store{layers};
    const World world{generator, store};
    const auto matches_world = [&](const Volume& made_volume) {
        for (std::uint32_t a = 0; a < 64; a += 21) {
            for (std::uint32_t layer = 0; std::cmp_less(layer, layers.count); layer += 37) {
                const Site site{.face = cell.face,
                                .i = (i * Volume::tile_side) + a,
                                .j = (j * Volume::tile_side) + 63 - a,
                                .layer = layer};
                if (!made_volume.made(site)) {
                    continue;
                }
                const hog::globe::Voxel made = world.at(site);
                CAPTURE(a, layer);
                CHECK(made_volume.at(site) == hog::globe::cube_of(made, made.temperature / 8.0));
            }
        }
    };
    matches_world(volume);

    // Dug toward, a tile's height more is made under it as it would have been, and what was
    // made is kept.
    Volume deeper = volume;
    const Site edited{.face = cell.face, .i = corner.i, .j = corner.j, .layer = *floor + 1};
    REQUIRE(deeper.set(edited, full_of(Material::Clay)));
    REQUIRE(hog::globe::deepen(deeper, generator, cell.face, i, j));
    const std::uint32_t lower = *floor - Volume::tile_side;
    CHECK(deeper.floor(corner) == lower);
    CHECK(deeper.made(under));
    CHECK(deeper.at(edited).solid == Material::Clay);
    deeper.set(edited, volume.at(edited));
    for (const std::uint32_t bottom : {tile_from(lower), tile_from(*floor)}) {
        CAPTURE(bottom);
        const VoxelTree* built =
            deeper.tile_at({.face = cell.face, .i = corner.i, .j = corner.j, .layer = bottom});
        REQUIRE(built != nullptr);
        CHECK(built->same_as(every(bottom, lower)));
    }
    matches_world(deeper);
}

TEST_CASE("the working world's tile columns, timed and sized", "[.time]") {
    auto planet = hog::globe::load_planet(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(planet);
    const Generator generator{*planet};
    const hog::globe::Layers& layers = generator.layers();
    const double sea = generator.sea_level().value_or(layers.floor);
    // Land and sea apart.
    struct Sum {
        double seconds = 0.0;
        double boundaries = 0.0;
        double caves = 0.0;
        std::size_t tiles = 0;
        std::size_t bytes = 0;
        int count = 0;
    };
    Sum land;
    Sum water;
    const auto since = [](auto started) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    for (std::uint64_t n = 0; n < 24; ++n) {
        const Cell cell = hog::globe::cell_under(somewhere(n), layers.level);
        const auto i = cell.i / Volume::tile_side;
        const auto j = cell.j / Volume::tile_side;
        const std::vector<Generator::Column> columns =
            generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                               .face = cell.face,
                               .i = i * Volume::tile_side,
                               .j = j * Volume::tile_side},
                              Volume::tile_side, hog::globe::voxel_lod(generator));
        double lowest = columns.front().ground();
        for (const Generator::Column& each : columns) {
            lowest = std::min(lowest, each.ground());
        }
        const double floor =
            layers.bottom(std::max(layers.layer_at(lowest) - hog::globe::made_depth, 0));
        Sum& sum = columns.front().ground() > sea ? land : water;
        auto started = std::chrono::steady_clock::now();
        for (const Generator::Column& each : columns) {
            static_cast<void>(each.boundaries(floor, each.top(), 0.0));
        }
        sum.boundaries += since(started);
        started = std::chrono::steady_clock::now();
        for (const Generator::Column& each : columns) {
            static_cast<void>(each.caves(floor, each.ground(), layers.thickness / 2.0));
        }
        sum.caves += since(started);
        started = std::chrono::steady_clock::now();
        const Volume volume = hog::globe::generate_tiles(generator, cell.face, i, j);
        sum.seconds += since(started);
        // Every made voxel as the generator makes it, on the first few.
        if (n < 6) {
            int wrong = 0;
            Site first_wrong;
            const std::uint32_t lowest_made = *volume.floor(
                {.face = cell.face, .i = i * Volume::tile_side, .j = j * Volume::tile_side});
            for (std::uint32_t y = 0; y < 64; ++y) {
                for (std::uint32_t x = 0; x < 64; ++x) {
                    const Generator::Column& column = columns[(y * 64) + x];
                    const auto top = static_cast<std::uint32_t>(std::min(
                        layers.layer_at(std::max(column.top(), sea)) + 2, layers.count - 1));
                    for (std::uint32_t layer = lowest_made; layer <= top; ++layer) {
                        const hog::globe::Voxel made =
                            hog::globe::generated_at(column, layers, layer);
                        const Site site{.face = cell.face,
                                        .i = (i * Volume::tile_side) + x,
                                        .j = (j * Volume::tile_side) + y,
                                        .layer = layer};
                        if (volume.at(site) != hog::globe::cube_of(made, made.temperature / 8.0)) {
                            if (wrong == 0) {
                                first_wrong = site;
                            }
                            ++wrong;
                        }
                    }
                }
            }
            CAPTURE(n, first_wrong.i, first_wrong.j, first_wrong.layer);
            CHECK(wrong == 0);
        }
        sum.tiles += volume.tiles();
        sum.bytes += volume.encode().size();
        ++sum.count;
    }
    for (const auto& [name, sum] : {std::pair{"land", land}, std::pair{"sea", water}}) {
        if (sum.count > 0) {
            std::printf("%s, %d tile columns: %.3f s (boundaries %.3f s, caves %.3f s), %zu "
                        "tiles, %zu bytes each\n",
                        name, sum.count, sum.seconds / sum.count, sum.boundaries / sum.count,
                        sum.caves / sum.count, sum.tiles / static_cast<std::size_t>(sum.count),
                        sum.bytes / static_cast<std::size_t>(sum.count));
        }
    }
}
