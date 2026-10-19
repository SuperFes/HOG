#include <hog/flora/mesh.hpp>
#include <hog/flora/skeleton.hpp>
#include <hog/flora/species.hpp>
#include <hog/flora/stand.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::Vec3;
using hog::flora::Bough;
using hog::flora::Detail;
using hog::flora::grow;
using hog::flora::mesh;
using hog::flora::pine;
using hog::flora::Skeleton;
using hog::flora::TreeMesh;

namespace {

std::uint64_t fingered(std::span<const std::byte> bytes, std::uint64_t hash) {
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 0x100'0000'01b3ULL;
    }
    return hash;
}

// The whole mesh as one number, for the golden tests.
std::uint64_t fingered(const TreeMesh& grown) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    hash = fingered(std::as_bytes(std::span{grown.wood}), hash);
    hash = fingered(std::as_bytes(std::span{grown.wood_indices}), hash);
    hash = fingered(std::as_bytes(std::span{grown.leaves}), hash);
    hash = fingered(std::as_bytes(std::span{grown.leaf_indices}), hash);
    return hash;
}

float length_of(const Bough& bough) {
    return glm::length(bough.rings.back().centre - bough.rings.front().centre);
}

} // namespace

TEST_CASE("the same seed grows the same pine, bit for bit", "[flora]") {
    const Skeleton first = grow(pine(), 7U);
    const Skeleton again = grow(pine(), 7U);
    const TreeMesh grown = mesh(first);
    CHECK(fingered(grown) == fingered(mesh(again)));
    CHECK(fingered(grown) != fingered(mesh(grow(pine(), 8U))));
    // The golden number: if this breaks, the same world now grows different trees on different
    // machines or releases, which the deterministic scatter must never do.
    CHECK(fingered(grown) == 0xef2c'5a3d'e417'3df2ULL);
}

TEST_CASE("a pine fits its frame", "[flora]") {
    const Skeleton tree = grow(pine(), 3U);
    REQUIRE(!tree.boughs.empty());
    const Bough& trunk = tree.boughs.front();
    CHECK(trunk.level == 0);
    // One trunk, its boughs, and foliage only on the boughs.
    CHECK(tree.boughs.size() == 1 + 64);
    CHECK(!tree.cards.empty());
    for (const Bough& bough : tree.boughs) {
        REQUIRE(bough.rings.size() >= 2);
        // Tapering, and rooted within the crown's spread.
        CHECK(bough.rings.back().radius < bough.rings.front().radius);
        const Vec3 root = bough.rings.front().centre;
        CHECK(glm::length(glm::vec2{root.x, root.y}) < 1.0F);
        for (const hog::flora::Ring& ring : bough.rings) {
            CHECK(ring.centre.z > -0.5F);
            CHECK(ring.centre.z < 20.0F);
            CHECK(glm::length(glm::vec2{ring.centre.x, ring.centre.y}) < 6.0F);
        }
    }
}

TEST_CASE("the crown narrows toward the top", "[flora]") {
    const Skeleton tree = grow(pine(), 11U);
    const auto lowest = std::min_element(
        tree.boughs.begin() + 1, tree.boughs.end(), [](const Bough& a, const Bough& b) {
            return a.rings.front().centre.z < b.rings.front().centre.z;
        });
    const auto highest = std::max_element(
        tree.boughs.begin() + 1, tree.boughs.end(), [](const Bough& a, const Bough& b) {
            return a.rings.front().centre.z < b.rings.front().centre.z;
        });
    CHECK(length_of(*highest) < length_of(*lowest) * 0.7F);
}

TEST_CASE("the mesh thins with detail, keeping the same silhouette", "[flora]") {
    const Skeleton tree = grow(pine(), 5U);
    const TreeMesh full = mesh(tree);
    const TreeMesh thinned = mesh(tree, Detail{.ring_stride = 3,
                                               .sides = 0.6F,
                                               .card_stride = 2,
                                               .card_scale = 1.4F,
                                               .wood_levels = 1});
    CHECK(!thinned.wood.empty());
    CHECK(!thinned.leaves.empty());
    CHECK(thinned.wood.size() < full.wood.size() / 2);
    CHECK(thinned.leaves.size() < full.leaves.size());
    // The thinned tips still reach where the full ones do.
    const auto top = [](const TreeMesh& grown) {
        float peak = 0.0F;
        for (const hog::flora::WoodVertex& vertex : grown.wood) {
            peak = std::max(peak, vertex.position.z);
        }
        return peak;
    };
    CHECK(std::abs(top(thinned) - top(full)) < 0.1F);
}

TEST_CASE("the stand field gathers woods and leaves clearings", "[flora]") {
    // Spots proven against an independent replica of the field: deep woods at one lattice base,
    // a clearing at another.
    for (std::uint32_t at = 0; at < 8; ++at) {
        const hog::Vec2 wooded{static_cast<float>(2163 + at) + 0.5F, 56619.5F};
        CHECK(hog::flora::stand_field(wooded, 0) > 0.6F);
        const hog::Vec2 clear{static_cast<float>(49849 + at) + 0.5F, 57876.5F};
        CHECK(hog::flora::stand_field(clear, 0) < 0.35F);
    }
}

TEST_CASE("trees stand by the same dice on every machine", "[flora]") {
    // The same golden discipline as grow(): if this number moves, the same world stands
    // different trees on different machines or releases.
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    std::size_t grown = 0;
    for (std::uint32_t i = 2048; i < 2112; ++i) {
        for (std::uint32_t j = 56576; j < 56640; ++j) {
            const auto tree = hog::flora::tree_grown(i, j, 0, hog::planet::Biome::BorealForest);
            if (!tree) {
                continue;
            }
            ++grown;
            const auto fold = [&hash](std::uint32_t bits) {
                hash ^= bits;
                hash *= 0x100'0000'01b3ULL;
            };
            fold(tree->hash);
            fold(std::bit_cast<std::uint32_t>(tree->height));
            fold(std::bit_cast<std::uint32_t>(tree->spot.x));
            fold(std::bit_cast<std::uint32_t>(tree->spot.y));
        }
    }
    // Boreal woods are thick: most of this wooded block stands a tree.
    CHECK(grown > 1500);
    CHECK(grown < 4096);
    CHECK(hash == 0xcfa9'fcc8'89ff'0b1dULL);
}

TEST_CASE("no tree grows where its biome grows none", "[flora]") {
    for (std::uint32_t i = 2048; i < 2112; ++i) {
        CHECK(!hog::flora::tree_grown(i, 56576, 0, hog::planet::Biome::Ocean));
        CHECK(!hog::flora::tree_grown(i, 56576, 0, hog::planet::Biome::Desert));
    }
    // The spot within a cell is answered even where nothing grows, for sampling the ground first.
    const hog::Vec2 spot = hog::flora::tree_spot(2048, 56576, 0);
    CHECK(spot.x >= 0.0F);
    CHECK(spot.x < 1.0F);
    CHECK(spot.y >= 0.0F);
    CHECK(spot.y < 1.0F);
}
