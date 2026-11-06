#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using hog::globe::Material;
using hog::globe::Voxel;
using hog::globe::VoxelTree;

namespace {

Voxel rock() {
    return {.solid = Material::Granite, .solid_amount = Voxel::full};
}

Voxel water() {
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
Voxel scattered(int x, int y, int z) {
    const std::uint64_t h = hog::hash_values(7, x, y, z);
    return {.solid = static_cast<Material>(h % 5), .solid_amount = static_cast<std::uint16_t>(h >> 40U)};
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
        return z < 40 ? water() : Voxel{};
    });
    CHECK(tree.at(5, 5, 20) == rock());
    CHECK(tree.at(5, 5, 21) == water());
    CHECK(tree.at(5, 5, 39) == water());
    CHECK(tree.at(5, 5, 40) == Voxel{});
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
    VoxelStore store{layers};
    const World world{generator, store};
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
    std::vector<Voxel> voxels(place(0, 0, side, side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const Cell cell{.level = centre.level,
                            .face = centre.face,
                            .i = centre.i + static_cast<std::uint32_t>(x),
                            .j = centre.j + static_cast<std::uint32_t>(y)};
            const std::vector<Generator::Column> columns{
                generator.column(center(cell), voxel_lod(generator))};
            const std::vector<Voxel> column = world.fill(std::array{cell}, columns, low, side, 1);
            for (int z = 0; z < side; ++z) {
                voxels[place(x, y, z, side)] = column[static_cast<std::size_t>(z)];
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
