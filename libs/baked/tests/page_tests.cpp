#include <hog/baked/page.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using hog::baked::encode_one;
using hog::baked::encode_part;
using hog::baked::encode_tree;
using hog::baked::Leaf;
using hog::baked::Page;
using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::Voxel;
using hog::globe::VoxelTree;

namespace {

Cube rock() {
    return {.solid = Material::Granite, .solid_amount = Voxel::full};
}

// Ground at a slant with some water over it and a few odd voxels: some cubes whole, some not.
VoxelTree slope(int side) {
    return VoxelTree::build(side, [&](int x, int y, int z) {
        if (hog::hash_values(std::uint64_t{9}, x, y, z) % 97 == 0) {
            return Cube{.solid = Material::Sand,
                        .solid_amount = static_cast<std::uint16_t>(x * 100),
                        .heat = static_cast<std::uint8_t>(y)};
        }
        if (z < (x + y) / 4) {
            return rock();
        }
        if (z < side / 2) {
            return Cube{.liquid = Material::Water,
                        .liquid_amount = Voxel::full,
                        .rise = static_cast<std::uint8_t>(x),
                        .peak = static_cast<std::uint8_t>(y * 7)};
        }
        return Cube{};
    });
}

} // namespace

TEST_CASE("a page reads as the tree it was made from", "[baked]") {
    constexpr int side = 32;
    const VoxelTree tree = slope(side);
    // Each tree voxel two finest voxels a side.
    const std::vector<std::byte> bytes = encode_tree(tree, 6, 1, Page::hidden);
    const auto page = Page::view(bytes);
    REQUIRE(page.has_value());
    CHECK(page->size() == 6);
    CHECK(page->voxel() == 1);
    CHECK(page->flags() == Page::hidden);
    for (int z = 0; z < side; ++z) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const Leaf leaf = page->at(static_cast<std::uint32_t>(2 * x) + 1,
                                           static_cast<std::uint32_t>(2 * y),
                                           static_cast<std::uint32_t>(2 * z) + 1);
                REQUIRE(leaf.cube == tree.at(x, y, z));
                REQUIRE((1 << leaf.size) == 2 * tree.leaf_side(x, y, z));
            }
        }
    }
}

TEST_CASE("a page stores alike nodes and cubes once", "[baked]") {
    const std::vector<std::byte> one = encode_one(10, 0, 0, rock(), 3);
    const auto page = Page::view(one);
    REQUIRE(page.has_value());
    CHECK(page->nodes() == 0);
    CHECK(page->cubes() == 1);
    const Leaf leaf = page->at(1000, 3, 77);
    CHECK(leaf.cube == rock());
    CHECK(leaf.open == 3);
    CHECK(leaf.size == 10);

    // Two halves alike, each split the same way: one node for both.
    const VoxelTree striped =
        VoxelTree::build(4, [](int x, int, int) { return x % 2 == 0 ? rock() : Cube{}; });
    const std::vector<std::byte> striped_bytes = encode_tree(striped, 2, 0, 0);
    const auto striped_page = Page::view(striped_bytes);
    REQUIRE(striped_page.has_value());
    CHECK(striped_page->nodes() == 2);
    CHECK(striped_page->cubes() == 2);
}

TEST_CASE("a page keeps its leaves' marks", "[baked]") {
    VoxelTree tree{8, Cube{}, 4};
    // The corner eighth known finely.
    tree.graft(0, 0, 0, VoxelTree{2, rock(), 0}, 2, 0);
    REQUIRE(tree.coarsest(4, 4, 4, 4) == 4);
    const std::vector<std::byte> bytes = encode_tree(tree, 3, 0, 0);
    const auto page = Page::view(bytes);
    REQUIRE(page.has_value());
    CHECK(page->at(0, 0, 0).open == 0);
    CHECK(page->at(7, 7, 7).open == 4);
}

TEST_CASE("a part of a page reads as that part of it", "[baked]") {
    const VoxelTree tree = slope(32);
    const std::vector<std::byte> bytes = encode_tree(tree, 5, 0, 0);
    const auto page = Page::view(bytes);
    REQUIRE(page.has_value());
    const std::vector<std::byte> part_bytes = encode_part(*page, 16, 0, 8, 3);
    const auto part = Page::view(part_bytes);
    REQUIRE(part.has_value());
    CHECK(part->size() == 3);
    for (std::uint32_t z = 0; z < 8; ++z) {
        for (std::uint32_t y = 0; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                REQUIRE(part->at(x, y, z).cube == page->at(16 + x, y, 8 + z).cube);
            }
        }
    }
    // Within one coarse voxel, the part is that voxel.
    const std::vector<std::byte> coarse = encode_tree(tree, 7, 2, 0);
    const auto coarse_page = Page::view(coarse);
    REQUIRE(coarse_page.has_value());
    const std::vector<std::byte> fine_bytes = encode_part(*coarse_page, 4, 8, 12, 1);
    const auto fine = Page::view(fine_bytes);
    REQUIRE(fine.has_value());
    CHECK(fine->voxel() == 1);
    CHECK(fine->at(1, 1, 1).cube == tree.at(1, 2, 3));
}

TEST_CASE("a page refuses bytes that aren't one", "[baked]") {
    std::vector<std::byte> bytes = encode_tree(slope(16), 4, 0, 0);
    CHECK_FALSE(Page::view(std::span{bytes}.first(10)));
    CHECK_FALSE(Page::view(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> wrong = bytes;
    wrong[0] = std::byte{99};
    CHECK_FALSE(Page::view(wrong));
    // A node pointing at itself would never end a walk.
    const auto page = Page::view(bytes);
    REQUIRE(page.has_value());
    REQUIRE(page->nodes() > 0);
    const std::size_t first_node = Page::header_bytes + (page->cubes() * Page::cube_bytes);
    bytes[first_node] = std::byte{0};
    bytes[first_node + 1] = std::byte{0};
    bytes[first_node + 2] = std::byte{0};
    bytes[first_node + 3] = std::byte{0};
    CHECK_FALSE(Page::view(bytes));
}
