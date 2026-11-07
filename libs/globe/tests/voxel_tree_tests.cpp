#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <array>
#include <cstdio>
#include <string>
#include <cstdint>
#include <span>
#include <vector>

using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::Voxel;
using hog::globe::VoxelTree;

namespace {

Cube rock() {
    return {.solid = Material::Granite, .solid_amount = Voxel::full};
}

Cube water() {
    return {.liquid = Material::Water, .liquid_amount = Voxel::full};
}

// A cube's voxels by place, x fastest.
std::size_t place(int x, int y, int z, int side) {
    return (((static_cast<std::size_t>(z) * static_cast<std::size_t>(side)) +
             static_cast<std::size_t>(y)) *
            static_cast<std::size_t>(side)) +
           static_cast<std::size_t>(x);
}

// A voxel that differs from its neighbours, but the same each time for a place.
Cube scattered(int x, int y, int z) {
    const std::uint64_t h = hog::hash_values(7, x, y, z);
    return {.solid = static_cast<Material>(h % 5),
            .solid_amount = static_cast<std::uint16_t>((h >> 40U) % (Voxel::full + 1))};
}

} // namespace

TEST_CASE("a tree of one voxel kind is one leaf however large", "[globe][voxel_tree]") {
    const VoxelTree tree{64, rock()};
    CHECK(tree.nodes() == 1);
    CHECK(tree.leaves() == 1);
    CHECK(tree.at(0, 0, 0) == rock());
    CHECK(tree.at(63, 31, 7) == rock());
    CHECK(tree.leaf_side(10, 10, 10) == 64);
}

TEST_CASE("a tree built from voxels tells each point as it was", "[globe][voxel_tree]") {
    const VoxelTree tree = VoxelTree::build(16, scattered);
    for (int z = 0; z < 16; ++z) {
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                CHECK(tree.at(x, y, z) == scattered(x, y, z));
            }
        }
    }
}

TEST_CASE("ground and air join into big leaves, and the boundary stays fine", "[globe][voxel_tree]") {
    // Rock under a level surface at layer 21 of 64, water over it up to layer 40, air above.
    const VoxelTree tree = VoxelTree::build(64, [](int, int, int z) {
        if (z < 21) {
            return rock();
        }
        return z < 40 ? water() : Cube{};
    });
    CHECK(tree.at(5, 5, 20) == rock());
    CHECK(tree.at(5, 5, 21) == water());
    CHECK(tree.at(5, 5, 39) == water());
    CHECK(tree.at(5, 5, 40) == Cube{});
    CHECK(tree.leaf_side(5, 5, 2) >= 16);
    CHECK(tree.leaf_side(5, 5, 60) >= 16);
    // Far fewer leaves than the 262,144 voxels, and the shared tree is smaller again.
    CHECK(tree.leaves() < 20'000);
    CHECK(tree.nodes() < tree.unshared_nodes() / 10);
}

TEST_CASE("setting a voxel splits its cube, and setting it back joins them again",
          "[globe][voxel_tree]") {
    VoxelTree tree{32, rock()};
    const VoxelTree before = tree;
    tree.set(9, 17, 3, water());
    CHECK(tree.at(9, 17, 3) == water());
    CHECK(tree.at(9, 17, 4) == rock());
    CHECK(tree.at(8, 17, 3) == rock());
    CHECK(tree.leaf_side(9, 17, 3) == 1);
    CHECK(tree.leaves() > 1);
    tree.set(9, 17, 3, rock());
    CHECK(tree.leaves() == 1);
    CHECK(tree.nodes() == 1);
    CHECK(tree.same_as(before));
}

TEST_CASE("the same voxels are the same tree however they were set", "[globe][voxel_tree]") {
    VoxelTree one{16};
    VoxelTree other = VoxelTree::build(16, scattered);
    for (int z = 0; z < 16; ++z) {
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                one.set(x, y, z, scattered(x, y, z));
            }
        }
    }
    CHECK(one.same_as(other));
    CHECK(one.nodes() == other.nodes());
    CHECK(one.leaves() == other.leaves());
}

TEST_CASE("cubes that repeat are stored once", "[globe][voxel_tree]") {
    // A pattern that only changes with the height, in steps of three.
    const VoxelTree tree = VoxelTree::build(32, [](int, int, int z) {
        return z % 3 == 0 ? rock() : water();
    });
    CHECK(tree.at(4, 4, 3) == rock());
    CHECK(tree.at(4, 4, 4) == water());
    CHECK((tree.nodes() * 50) < tree.unshared_nodes());
}

TEST_CASE("a tree of the world's voxels tells each as the world does", "[globe][voxel_tree]") {
    using namespace hog::globe;
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    // Land: the highest ground over a coarse sweep.
    hog::DVec3 land{0.0, 0.0, 1.0};
    double top = 0.0;
    for (int k = 0; k < 2000; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 2000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        if (const double ground = generator.column(d, 200.0).ground(); ground > top) {
            top = ground;
            land = d;
        }
    }
    constexpr int side = 32;
    const Cell centre = cell_under(land, layers.level);
    const int low = std::max(layers.layer_at(top) - (side / 2), 0);
    std::vector<Cube> voxels(place(0, 0, side, side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const Cell cell{.level = centre.level,
                            .face = centre.face,
                            .i = centre.i + static_cast<std::uint32_t>(x),
                            .j = centre.j + static_cast<std::uint32_t>(y)};
            const std::vector<Generator::Column> columns{
                generator.column(center(cell), voxel_lod(generator))};
            for (int z = 0; z < side; ++z) {
                {
                    const hog::globe::Voxel voxel =
                        generated_at(columns[0], layers, static_cast<std::uint32_t>(low + z));
                    voxels[place(x, y, z, side)] = cube_of(voxel, hog::globe::kelvin(voxel.temperature));
                }
            }
        }
    }
    const VoxelTree tree = VoxelTree::build(side, [&](int x, int y, int z) {
        return voxels[place(x, y, z, side)];
    });
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                CHECK(tree.at(x, y, z) == voxels[place(x, y, z, side)]);
            }
        }
    }
    CHECK(tree.nodes() <= tree.unshared_nodes());
}

TEST_CASE("a tree's bytes read back as the same tree, and the same voxels are the same bytes",
          "[globe][voxel_tree]") {
    const VoxelTree tree = VoxelTree::build(16, scattered);
    const std::vector<std::byte> bytes = tree.encode();
    const auto back = VoxelTree::decode(bytes);
    REQUIRE(back.has_value());
    CHECK(back->same_as(tree));
    CHECK(back->encode() == bytes);

    VoxelTree set{16};
    for (int z = 0; z < 16; ++z) {
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                set.set(x, y, z, scattered(x, y, z));
            }
        }
    }
    CHECK(set.encode() == bytes);

    const VoxelTree plain{64, rock()};
    CHECK(plain.encode().size() < 32);
    const VoxelTree layered = VoxelTree::build(64, [](int, int, int z) {
        return z < 21 ? rock() : Cube{};
    });
    CHECK(layered.encode().size() < 400);
    CHECK(VoxelTree::decode(layered.encode())->same_as(layered));
}

TEST_CASE("bytes that aren't a tree are refused", "[globe][voxel_tree]") {
    const std::vector<std::byte> bytes = VoxelTree::build(8, scattered).encode();
    // Cut short anywhere.
    for (std::size_t keep = 0; keep < bytes.size(); keep += 7) {
        CHECK_FALSE(VoxelTree::decode(std::span{bytes}.first(keep)).has_value());
    }
    // Run on past the nodes.
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(VoxelTree::decode(longer).has_value());
    // A side that isn't a power of two, and a node that is neither a leaf nor a cube.
    std::vector<std::byte> odd = bytes;
    odd[0] = std::byte{6};
    CHECK_FALSE(VoxelTree::decode(odd).has_value());
    std::vector<std::byte> tagged = VoxelTree{8, rock()}.encode();
    tagged[2] = std::byte{9};
    CHECK_FALSE(VoxelTree::decode(tagged).has_value());
    // Eight leaves alike aren't kept as a cube.
    hog::ByteWriter cube;
    cube.write_varint(2);
    cube.write_varint(2);
    cube.write(std::uint8_t{0});
    cube.write_signed(0);
    cube.write_varint(0);
    cube.write(std::uint8_t{0});
    cube.write(std::uint8_t{255});
    cube.write(std::uint8_t{0});
    cube.write_varint(0);
    cube.write(std::uint8_t{0});
    cube.write_varint(0);
    cube.write_varint(0);
    cube.write_varint(0);
    cube.write(std::uint8_t{1});
    for (int k = 0; k < 8; ++k) {
        cube.write_varint(1);
    }
    CHECK_FALSE(VoxelTree::decode(cube.bytes()).has_value());
}

// What the world's own bricks cost in bytes, and what the same voxels cost as trees of 32 across.
// Hidden: it generates the bricks.
TEST_CASE("generated ground as bricks and as trees, in bytes", "[.tree_size]") {
    using namespace hog::globe;
    const Generator generator{Planet{}};
    const Layers& layers = generator.layers();
    const double sea = generator.sea_level().value_or(generator.planet().radius);
    hog::DVec3 peak{0.0, 0.0, 1.0};
    hog::DVec3 coast{0.0, 0.0, 1.0};
    double highest = 0.0;
    double nearest = 1e18;
    for (int k = 0; k < 40000; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 40000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        const double ground = generator.column(d, 200.0).ground();
        if (ground > highest) {
            highest = ground;
            peak = d;
        }
        if (std::abs(ground - sea) < nearest) {
            nearest = std::abs(ground - sea);
            coast = d;
        }
    }
    constexpr int blocks_across = 4;  // 32-voxel blocks along each side of the patch
    constexpr int bricks_across = 4;  // bricks across a block
    constexpr int side = bricks_across * brick_side;
    for (const auto& [label, at] : {std::pair{"mountain", peak}, std::pair{"coast", coast}}) {
        const Cell centre = cell_under(at, layers.level);
        const auto base_i = (centre.i / side) * side - ((blocks_across / 2) * side);
        const auto base_j = (centre.j / side) * side - ((blocks_across / 2) * side);
        const int ground_layer = layers.layer_at(generator.column(center(centre), 1.0).ground());
        std::size_t brick_bytes = 0;
        std::size_t tree_bytes = 0;
        std::size_t blocks = 0;
        // Every block from deep to the sky that holds anything but one kind, over the patch.
        for (int bj = 0; bj < blocks_across; ++bj) {
            for (int bi = 0; bi < blocks_across; ++bi) {
                for (int layer0 = std::max((ground_layer / side) - 4, 0) * side;
                     layer0 < std::min(ground_layer + 4 * side, layers.count); layer0 += side) {
                    std::vector<Voxel> voxels(static_cast<std::size_t>(side) * side * side);
                    std::size_t packed = 0;
                    for (int cj = 0; cj < bricks_across; ++cj) {
                        for (int ci = 0; ci < bricks_across; ++ci) {
                            for (int cl = 0; cl < bricks_across; ++cl) {
                                const BrickSite brick{
                                    .face = centre.face,
                                    .i = (base_i + static_cast<std::uint32_t>(bi * side)) / brick_side + static_cast<std::uint32_t>(ci),
                                    .j = (base_j + static_cast<std::uint32_t>(bj * side)) / brick_side + static_cast<std::uint32_t>(cj),
                                    .layer = static_cast<std::uint32_t>(layer0 / brick_side + cl)};
                                const BrickVoxels held = generated(generator, brick);
                                packed += Brick::pack(held).bytes();
                                for (int index = 0; index < brick_voxels; ++index) {
                                    const Site site = site_in(brick, index);
                                    const auto x = static_cast<int>(site.i - (base_i + static_cast<std::uint32_t>(bi * side)));
                                    const auto y = static_cast<int>(site.j - (base_j + static_cast<std::uint32_t>(bj * side)));
                                    const auto z = static_cast<int>(site.layer) - layer0;
                                    voxels[place(x, y, z, side)] = held[static_cast<std::size_t>(index)];
                                }
                            }
                        }
                    }
                    // As cubes: what they are and how full, not the distance, normal or temperature
                    // the place can say.
                    const VoxelTree tree = VoxelTree::build(side, [&](int x, int y, int z) {
                        const Voxel& v = voxels[place(x, y, z, side)];
                        return cube_of(v, kelvin(v.temperature));
                    });
                    brick_bytes += packed;
                    tree_bytes += tree.encode().size();
                    ++blocks;
                }
            }
        }
        std::printf("%-9s %zu blocks of %d^3: bricks %zu bytes; cube trees %zu (%.1fx)\n", label,
                    blocks, side, brick_bytes, tree_bytes,
                    static_cast<double>(brick_bytes) / static_cast<double>(tree_bytes));
    }
}

TEST_CASE("a tree halved stands each voxel for the eight it covers", "[globe][voxel_tree]") {
    // Ground up to half way, water over it in one corner.
    const VoxelTree tree = VoxelTree::build(8, [](int x, int y, int z) {
        if (z < 4) {
            return rock();
        }
        return x < 2 && y < 2 && z < 6 ? water() : Cube{};
    });
    const VoxelTree half = hog::globe::halved(tree);
    REQUIRE(half.side() == 4);
    CHECK(half.at(3, 3, 1) == rock());
    CHECK(half.at(0, 0, 2) == water());
    CHECK(half.at(3, 3, 2) == Cube{});
    CHECK(half.at(0, 0, 3) == Cube{});
    // Alike voxels still make big leaves.
    CHECK(half.leaf_side(0, 0, 0) == 2);

    // Three of eight granite and one basalt: as much solid as they hold, of the most of it.
    const Cube basalt{.solid = Material::Basalt, .solid_amount = Voxel::full};
    const VoxelTree mixed = VoxelTree::build(2, [&](int x, int y, int z) {
        const int k = x + (2 * y) + (4 * z);
        if (k < 3) {
            return rock();
        }
        return k == 3 ? basalt : Cube{};
    });
    const Cube one = hog::globe::halved(mixed).at(0, 0, 0);
    CHECK(one.solid == Material::Granite);
    CHECK(one.solid_amount == Voxel::full / 2);
    // A trace too small to count is none.
    const VoxelTree trace = VoxelTree::build(2, [](int x, int y, int z) {
        return x + y + z == 0 ? Cube{.grain = Material::Sand, .grain_amount = 3} : Cube{};
    });
    CHECK(hog::globe::halved(trace).at(0, 0, 0) == Cube{});
}

TEST_CASE("a tree halved from its big leaves is the tree halved voxel by voxel",
          "[globe][voxel_tree]") {
    const VoxelTree tree = VoxelTree::build(16, [](int x, int y, int z) {
        if (z < 5) {
            return rock();
        }
        return z < 9 ? scattered(x, y, z) : Cube{};
    });
    const VoxelTree half = hog::globe::halved(tree);
    const VoxelTree by_voxel = VoxelTree::build(8, [&](int x, int y, int z) {
        std::array<Cube, 8> eight;
        for (std::size_t k = 0; k < 8; ++k) {
            eight.at(k) = tree.at((2 * x) + static_cast<int>(k & 1U),
                                  (2 * y) + static_cast<int>((k >> 1U) & 1U),
                                  (2 * z) + static_cast<int>((k >> 2U) & 1U));
        }
        return hog::globe::halved(VoxelTree::build(2,
                                                   [&](int a, int b, int c) {
                                                       const int k = a + (2 * b) + (4 * c);
                                                       return eight.at(static_cast<std::size_t>(k));
                                                   }))
            .at(0, 0, 0);
    });
    CHECK(half.same_as(by_voxel));
}
