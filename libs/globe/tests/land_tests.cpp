#include "bed.hpp"

#include <hog/globe/cube.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

using hog::globe::Cell;
using hog::globe::Cube;
using hog::globe::Land;
using hog::globe::Site;
using hog::globe::TileKey;
using hog::globe::VoxelTree;

namespace {

constexpr int side = hog::globe::Volume::tile_side;

bool same_voxels(const VoxelTree& a, const VoxelTree& b) {
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                if (!(a.at(x, y, z) == b.at(x, y, z))) {
                    return false;
                }
            }
        }
    }
    return true;
}

// The finest tile at the ground somewhere gentle.
TileKey ground_tile(const hog::globe::Generator& generator) {
    const hog::DVec3 up = hog::globe::bed::ground_between(generator, 10.0, 300.0);
    const Cell cell = hog::globe::cell_under(up, generator.layers().level);
    const hog::globe::Layers& layers = generator.layers();
    const int ground = layers.layer_at(hog::globe::ground_below(generator, up, 0.5).value_or(0.0));
    return {.face = cell.face,
            .i = cell.i / side,
            .j = cell.j / side,
            .k = static_cast<std::uint32_t>(ground / side)};
}

} // namespace

TEST_CASE("a tile asked of the land at any level is the generator's at that level",
          "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    CHECK_FALSE(land.known(fine));
    CHECK(land.at({.face = fine.face, .i = fine.i * side, .j = fine.j * side}) == Cube{});

    const TileKey coarse = fine.parent().parent();
    CHECK(same_voxels(land.tile(coarse), hog::globe::generate_tile(generator, coarse)));
    CHECK(land.known(coarse));
    CHECK_FALSE(land.known(fine));

    CHECK(land.tile(fine).same_as(hog::globe::generate_tile(generator, fine)));
    CHECK(land.known(fine));
}

TEST_CASE("the land's coarser tiles stand for what's known finer under them", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const VoxelTree made = land.tile(fine);
    const TileKey parent = fine.parent();
    const VoxelTree coarse = land.tile(parent);
    const VoxelTree generated = hog::globe::generate_tile(generator, parent);
    const int half = side / 2;
    const int at_x = static_cast<int>(fine.i % 2) * half;
    const int at_y = static_cast<int>(fine.j % 2) * half;
    const int at_z = static_cast<int>(fine.k % 2) * half;
    bool fine_eighth = true;
    bool generated_rest = true;
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const bool in = x >= at_x && x < at_x + half && y >= at_y && y < at_y + half &&
                                z >= at_z && z < at_z + half;
                if (in) {
                    fine_eighth = fine_eighth &&
                                  coarse.at(x, y, z) == made.average(2 * (x - at_x), 2 * (y - at_y),
                                                                     2 * (z - at_z), 2);
                } else {
                    generated_rest = generated_rest && coarse.at(x, y, z) == generated.at(x, y, z);
                }
            }
        }
    }
    CHECK(fine_eighth);
    CHECK(generated_rest);
}

TEST_CASE("a voxel set in the land is kept, and stood for at every level over it",
          "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const Site site{.face = fine.face,
                    .i = (fine.i * side) + 9,
                    .j = (fine.j * side) + 20,
                    .layer = (fine.k * side) + 33};
    const Cube water{.liquid = hog::globe::Material::Water,
                     .liquid_amount = hog::globe::Voxel::full};
    land.set(site, water);
    CHECK(land.at(site) == water);
    CHECK(land.known(fine));

    // Kept when coarser ground about it is made after.
    const TileKey coarse = fine.parent().parent();
    const VoxelTree over = land.tile(coarse);
    CHECK(land.at(site) == water);
    const VoxelTree tile = land.tile(fine);
    CHECK(tile.at(9, 20, 33) == water);
    const int x = static_cast<int>((fine.i % 4) * side) + 9;
    const int y = static_cast<int>((fine.j % 4) * side) + 20;
    const int z = static_cast<int>((fine.k % 4) * side) + 33;
    CHECK(over.at(x / 4, y / 4, z / 4) == tile.average(8, 20, 32, 4));
}

TEST_CASE("the land makes only what it doesn't know, spread as asked", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey fine = ground_tile(generator);
    const std::array keys{fine, fine.parent(), fine, fine.parent().parent()};
    std::size_t spread = 0;
    const hog::globe::EachOf each = [&](std::size_t count,
                                        const std::function<void(std::size_t)>& body) {
        spread += count;
        for (std::size_t k = 0; k < count; ++k) {
            body(k);
        }
    };
    land.make(keys, each);
    CHECK(spread == 3);
    land.make(keys, each);
    CHECK(spread == 3);
    for (const TileKey key : keys) {
        CHECK(land.known(key));
    }
}

TEST_CASE("a tile deep under the ground is hidden until something in it changes", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey surface = ground_tile(generator);
    TileKey deep = surface;
    deep.k -= 3;
    CHECK_FALSE(land.hidden(surface));
    CHECK(land.hidden(deep));
    CHECK_FALSE(land.all_one(deep));
    land.set({.face = deep.face, .i = deep.i * side, .j = deep.j * side, .layer = deep.k * side},
             Cube{});
    CHECK_FALSE(land.hidden(deep));
}

TEST_CASE("a tile is hidden only where nothing finer under it shows", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    Land land{generator};
    const TileKey ground = ground_tile(generator);
    for (std::uint8_t level = 1; level <= 3; ++level) {
        TileKey middle = ground;
        for (std::uint8_t up = 0; up < level; ++up) {
            middle = middle.parent();
        }
        for (const std::uint32_t i : {middle.i - 1, middle.i, middle.i + 1}) {
            for (const std::uint32_t j : {middle.j - 1, middle.j, middle.j + 1}) {
                for (const std::uint32_t k : {middle.k - 1, middle.k, middle.k + 1}) {
                    const TileKey key{.level = level, .face = middle.face, .i = i, .j = j, .k = k};
                    if (!land.hidden(key)) {
                        continue;
                    }
                    for (std::size_t octant = 0; octant < 8; ++octant) {
                        CAPTURE(level, i, j, k, octant);
                        CHECK(land.hidden(key.child(octant)));
                    }
                }
            }
        }
    }
}

TEST_CASE("a tile made is the generator's voxel by voxel, at every level", "[globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    const TileKey ground = ground_tile(generator);
    for (std::uint8_t level = 0; level <= 5; level += 5) {
        TileKey key = ground;
        for (std::uint8_t up = 0; up < level; ++up) {
            key = key.parent();
        }
        const hog::globe::Layers layers = hog::globe::coarser_layers(generator.layers(), level);
        const auto columns =
            generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                               .face = key.face,
                               .i = key.i * static_cast<std::uint32_t>(side),
                               .j = key.j * static_cast<std::uint32_t>(side)},
                              side, hog::globe::voxel_lod(generator) * (1 << level));
        // Caves close up in voxels wider than a tunnel.
        const bool caves = layers.thickness <= 7.0;
        for (const std::uint32_t k : {key.k, key.k - 1}) {
            TileKey at = key;
            at.k = k;
            const int bottom = static_cast<int>(k) * side;
            const VoxelTree expected = VoxelTree::build(side, [&](int x, int y, int z) {
                const int layer = bottom + z;
                if (layer >= layers.count) {
                    return Cube{};
                }
                const auto& column =
                    columns[(static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x)];
                const hog::globe::Voxel voxel = hog::globe::voxel_from(
                    column.sample(layers.middle(layer), caves), layers.thickness);
                return hog::globe::cube_of(voxel, voxel.temperature / 8.0);
            });
            CAPTURE(level, k);
            CHECK(hog::globe::generate_tile(generator, at).same_as(expected));
        }
    }
}

TEST_CASE("tile making cost", "[.bench][globe][land]") {
    const auto& generator = hog::globe::bed::earth();
    const TileKey ground = ground_tile(generator);
    const auto seconds = [](auto from) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
    };
    for (std::uint8_t level = 0; level <= 6; level += 2) {
        TileKey key = ground;
        for (std::uint8_t up = 0; up < level; ++up) {
            key = key.parent();
        }
        const hog::globe::Layers layers = hog::globe::coarser_layers(generator.layers(), level);
        auto started = std::chrono::steady_clock::now();
        const auto columns =
            generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                               .face = key.face,
                               .i = key.i * static_cast<std::uint32_t>(side),
                               .j = key.j * static_cast<std::uint32_t>(side)},
                              side, hog::globe::voxel_lod(generator) * (1 << level));
        const double column_time = seconds(started);
        started = std::chrono::steady_clock::now();
        const VoxelTree tile = hog::globe::generate_tile(generator, key);
        const double tile_time = seconds(started);
        TileKey sky = key;
        sky.k += 3;
        started = std::chrono::steady_clock::now();
        const VoxelTree air = hog::globe::generate_tile(generator, sky);
        const double sky_time = seconds(started);
        WARN("level " << int{level} << ": columns " << column_time * 1e3 << " ms, tile "
                      << tile_time * 1e3 << " ms (" << tile.nodes() << " nodes, "
                      << tile.encode().size() << " bytes), sky tile " << sky_time * 1e3 << " ms ("
                      << air.nodes() << " nodes) " << columns.size());
    }
}

#include "../src/terms.hpp"

TEST_CASE("generator term cost", "[.bench][globe][land]") {
    namespace terms = hog::globe::terms;
    const auto& generator = hog::globe::bed::earth();
    const terms::Context context{.planet = &generator.planet(),
                                 .derived = &generator.derived(),
                                 .shape = &generator.shape()};
    const TileKey key = ground_tile(generator);
    std::vector<hog::DVec3> downs;
    for (std::uint32_t j = 0; j < 64; ++j) {
        for (std::uint32_t i = 0; i < 64; ++i) {
            downs.push_back(
                hog::globe::center({.level = static_cast<std::uint8_t>(generator.layers().level),
                                    .face = key.face,
                                    .i = (key.i * 64) + i,
                                    .j = (key.j * 64) + j}));
        }
    }
    for (const double lod : {2.5, 40.0, 640.0}) {
        double sink = 0.0;
        const auto time = [&](const char* name, auto body) {
            const auto started = std::chrono::steady_clock::now();
            for (const hog::DVec3& down : downs) {
                sink += body(down);
            }
            WARN("lod " << lod << " " << name << ": "
                        << std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                                   .count() /
                               static_cast<double>(downs.size()) * 1e6
                        << " us");
        };
        time("province", [&](hog::DVec3 d) { return terms::province(context, d).high; });
        time("built_up", [&](hog::DVec3 d) {
            return terms::built_up(context, d, terms::province(context, d)).value;
        });
        time("relief", [&](hog::DVec3 d) {
            const auto crust = terms::province(context, d);
            return terms::relief(context, d, crust, terms::built_up(context, d, crust), lod).value;
        });
        time("craters", [&](hog::DVec3 d) { return terms::craters(context, d, lod).value; });
        time("volcanoes", [&](hog::DVec3 d) { return terms::volcanoes(context, d, lod).value; });
        time("ground", [&](hog::DVec3 d) { return terms::ground(context, d, lod).height; });
        time("column", [&](hog::DVec3 d) { return generator.column(d, lod).ground(); });
        time("rockfall",
             [&](hog::DVec3 d) { return terms::rockfall(context, d, lod) ? 1.0 : 0.0; });
        time("pond", [&](hog::DVec3 d) { return generator.pond_at(d).value_or(0.0); });
        time("layering", [&](hog::DVec3 d) {
            const auto rock = terms::column(context, d, lod);
            return terms::layering(context, d, rock, rock.height.value).cover;
        });
        WARN(sink);
    }
}
