#include <hog/globe/cell.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using hog::globe::Cube;
using hog::globe::TileKey;
using hog::globe::VoxelTree;

namespace {

constexpr int side = hog::globe::Volume::tile_side;
constexpr int coarser = std::countr_zero(static_cast<unsigned>(side));

struct Kind {
    std::string_view name;
    Cube (*keep)(const Cube&);
};

constexpr std::array<Kind, 6> kinds{{
    {.name = "whole", .keep = [](const Cube& cube) { return cube; }},
    {.name = "shape",
     .keep =
         [](const Cube& cube) {
             return Cube{.solid_amount = cube.solid_amount, .grain_amount = cube.grain_amount};
         }},
    {.name = "material",
     .keep =
         [](const Cube& cube) {
             return Cube{.solid = cube.solid, .grain = cube.grain, .condition = cube.condition};
         }},
    {.name = "water",
     .keep =
         [](const Cube& cube) {
             return Cube{.liquid = cube.liquid,
                         .liquid_amount = cube.liquid_amount,
                         .rise = cube.rise,
                         .peak = cube.peak};
         }},
    {.name = "water (kind)", .keep = [](const Cube& cube) { return Cube{.liquid = cube.liquid}; }},
    {.name = "heat", .keep = [](const Cube& cube) { return Cube{.heat = cube.heat}; }},
}};

// The six tiles beside `key` at its level: across a face's edge, the next face's.
std::vector<TileKey> beside_keys(TileKey key, const hog::globe::Layers& layers) {
    const hog::globe::Cell cell{.level =
                                    static_cast<std::uint8_t>(layers.level - coarser - key.level),
                                .face = key.face,
                                .i = key.i,
                                .j = key.j};
    std::vector<TileKey> next;
    for (const auto [di, dj] :
         {std::array{-1, 0}, std::array{1, 0}, std::array{0, -1}, std::array{0, 1}}) {
        if (const auto over = hog::globe::next_to(cell, di, dj)) {
            next.push_back(
                {.level = key.level, .face = over->face, .i = over->i, .j = over->j, .k = key.k});
        }
    }
    if (key.k > 0) {
        next.push_back(
            {.level = key.level, .face = key.face, .i = key.i, .j = key.j, .k = key.k - 1});
    }
    next.push_back({.level = key.level, .face = key.face, .i = key.i, .j = key.j, .k = key.k + 1});
    return next;
}

// Tiles over the ground and the sea's top under spread directions, at the finest level.
std::vector<TileKey> surface_tiles(const hog::globe::Generator& generator, int count) {
    const hog::globe::Layers& layers = generator.layers();
    const double sea = generator.planet().body.radius;
    std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>> seen;
    std::vector<TileKey> keys;
    for (int n = 0; n < count; ++n) {
        const hog::DVec3 up = hog::globe::center(hog::globe::cell_at_index(
            7, (static_cast<std::uint64_t>(n) * std::uint64_t{2'654'435'761}) % 98'304));
        const auto ground = hog::globe::ground_below(generator, up, 0.5);
        if (!ground) {
            continue;
        }
        const hog::globe::Cell cell = hog::globe::cell_under(up, layers.level);
        for (const double radius : {*ground, std::max(*ground, sea)}) {
            const TileKey key{.face = cell.face,
                              .i = cell.i / side,
                              .j = cell.j / side,
                              .k = static_cast<std::uint32_t>(layers.layer_at(radius) / side)};
            if (seen.emplace(key.face, key.i, key.j, key.k).second) {
                keys.push_back(key);
            }
        }
    }
    return keys;
}

// The tiles over `fine` at `level` holding more than one node, made into `made` with the tiles
// beside them.
std::vector<TileKey> tiles_with_neighbours(const hog::globe::Generator& generator,
                                           const std::vector<TileKey>& fine, std::uint8_t level,
                                           std::map<TileKey, VoxelTree>& made) {
    const auto made_at = [&](TileKey key) -> const VoxelTree& {
        auto found = made.find(key);
        if (found == made.end()) {
            found = made.emplace(key, hog::globe::generate_tile(generator, key)).first;
        }
        return found->second;
    };
    std::vector<TileKey> keys;
    for (TileKey key : fine) {
        for (std::uint8_t up = 0; up < level; ++up) {
            key = key.parent();
        }
        if (made.contains(key) || made_at(key).nodes() == 1) {
            continue;
        }
        keys.push_back(key);
        for (const TileKey next : beside_keys(key, generator.layers())) {
            std::ignore = made_at(next);
        }
    }
    return keys;
}

} // namespace

// Whether tiles cost less held as one tree or as a tree per kind of data over the same keys. Counts
// nodes and today's encoded bytes for each.
TEST_CASE("one tree against trees split by kind", "[.bench][globe][voxel_tree]") {
    const auto& generator = hog::testing::small_world();
    const std::vector<TileKey> fine = surface_tiles(generator, 64);
    for (std::uint8_t level = 0; level <= 6; level += 2) {
        std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t>> seen;
        std::array<std::size_t, kinds.size()> nodes{};
        std::array<std::size_t, kinds.size()> leaves{};
        std::array<std::size_t, kinds.size()> bytes{};
        std::size_t tiles = 0;
        for (TileKey key : fine) {
            for (std::uint8_t up = 0; up < level; ++up) {
                key = key.parent();
            }
            if (!seen.emplace(key.face, key.i, key.j, key.k).second) {
                continue;
            }
            const VoxelTree made = hog::globe::generate_tile(generator, key);
            if (made.nodes() == 1) {
                continue;
            }
            ++tiles;
            for (std::size_t n = 0; n < kinds.size(); ++n) {
                const VoxelTree kept = VoxelTree::build(
                    side, [&](int x, int y, int z) { return kinds.at(n).keep(made.at(x, y, z)); });
                nodes.at(n) += kept.nodes();
                leaves.at(n) += kept.leaves();
                bytes.at(n) += kept.encode().size();
            }
        }
        std::string report = "level " + std::to_string(level) + ", " + std::to_string(tiles) +
                             " tiles with more than one node:";
        for (std::size_t n = 0; n < kinds.size(); ++n) {
            report += "\n  " + std::string{kinds.at(n).name} + ": " + std::to_string(nodes.at(n)) +
                      " nodes, " + std::to_string(leaves.at(n)) + " leaves, " +
                      std::to_string(bytes.at(n)) + " bytes";
        }
        WARN(report);
    }
}

// What meshing real tiles costs, each among the tiles beside it as the client meshes it.
TEST_CASE("meshing real tiles", "[.bench][globe][voxel_mesh]") {
    const auto& generator = hog::testing::small_world();
    const std::vector<TileKey> fine = surface_tiles(generator, 64);
    constexpr int repeats = 4;
    for (std::uint8_t level = 0; level <= 6; level += 2) {
        std::map<TileKey, VoxelTree> made;
        const std::vector<TileKey> keys = tiles_with_neighbours(generator, fine, level, made);
        const hog::globe::TileAt beside = [&](TileKey key) -> const VoxelTree* {
            const auto found = made.find(key);
            return found == made.end() ? nullptr : &found->second;
        };
        std::size_t quads = 0;
        std::size_t vertices = 0;
        const auto started = std::chrono::steady_clock::now();
        for (int pass = 0; pass < repeats; ++pass) {
            for (const TileKey key : keys) {
                const hog::globe::VoxelMesh mesh =
                    hog::globe::mesh_tile(generator.layers(), key, made.at(key), beside, 5.0);
                if (pass == 0) {
                    quads += mesh.indices.size() / 6;
                    vertices += mesh.vertices.size();
                }
            }
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto tiles = static_cast<double>(keys.size());
        WARN("level " << int{level} << ": " << keys.size() << " tiles, "
                      << seconds / (tiles * repeats) * 1e3 << " ms a tile, "
                      << static_cast<double>(quads) / tiles << " quads and "
                      << static_cast<double>(vertices) / tiles << " vertices a tile");
    }
}

TEST_CASE("generator sampling cost by way of asking", "[.bench][globe]") {
    const auto& generator = hog::testing::small_world();
    const double lod = hog::globe::voxel_lod(generator);
    const hog::globe::Layers& layers = generator.layers();
    std::vector<hog::DVec3> ups;
    std::vector<double> grounds;
    for (int n = 0; n < 200; ++n) {
        const hog::DVec3 up = hog::globe::center(
            hog::globe::cell_at_index(7, static_cast<std::uint64_t>(n) * 2'654'435'761U % 98'304U));
        ups.push_back(up);
        grounds.push_back(hog::globe::ground_below(generator, up, lod).value_or(0.0));
    }
    const auto time = [](auto&& work) {
        const auto started = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    double sink = 0.0;
    const double samples = time([&] {
        for (std::size_t n = 0; n < ups.size(); ++n) {
            sink += generator.sample(ups[n] * grounds[n], lod).distance;
        }
    });
    const double columns = time([&] {
        for (const hog::DVec3 up : ups) {
            sink += generator.column(up, lod).ground();
        }
    });
    const auto column = generator.column(ups[0], lod);
    const double along = time([&] {
        for (int n = 0; n < 2000; ++n) {
            sink += column.sample(grounds[0] + (n * 0.01) - 10.0).distance;
        }
    });
    hog::globe::Cell corner = hog::globe::cell_under(ups[0], layers.level);
    const std::uint32_t last = (std::uint32_t{1} << corner.level) - 32U;
    corner.i = std::min(corner.i, last);
    corner.j = std::min(corner.j, last);
    const double square =
        time([&] { sink += static_cast<double>(generator.columns(corner, 32, lod).size()); });
    WARN("sample " << samples / 200 * 1e3 << " ms, column " << columns / 200 * 1e3
                   << " ms, along a column " << along / 2000 * 1e6 << " us, columns in a 32 square "
                   << square / 1024 * 1e3 << " ms each; " << sink);
}
