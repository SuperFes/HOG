#include <hog/flora/mesh.hpp>
#include <hog/flora/skeleton.hpp>
#include <hog/flora/species.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
