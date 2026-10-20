#include <hog/flora/fates.hpp>
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
#include <optional>
#include <span>
#include <vector>

using hog::Vec3;
using hog::flora::aged;
using hog::flora::Bough;
using hog::flora::Detail;
using hog::flora::grow;
using hog::flora::maple;
using hog::flora::mesh;
using hog::flora::oak;
using hog::flora::palm;
using hog::flora::pine;
using hog::flora::prime_age;
using hog::flora::Skeleton;
using hog::flora::Species;
using hog::flora::TreeMesh;

namespace {

std::uint64_t fingered(std::span<const std::byte> bytes, std::uint64_t hash) {
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 0x100'0000'01b3ULL;
    }
    return hash;
}

// Mild enough for every kind but the palms.
constexpr hog::flora::Habitat temperate{};

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

// A grown tree's shape, averaged over a few seeds at one age.
struct Shape {
    float height = 0.0F;
    float fronds = 0.0F;
    float crown_base = 0.0F; // the lowest frond, of the height
    float girth = 0.0F;      // the trunk's base radius, of the height
    float spread = 0.0F;     // the crown across, of the height
};

Shape shape_at(float age, const Species& species = pine()) {
    constexpr std::uint32_t seeds = 8;
    Shape shape;
    for (std::uint32_t seed = 101; seed < 101 + seeds; ++seed) {
        const Skeleton tree = grow(aged(species, age), seed);
        float height = 0.0F;
        for (const Bough& bough : tree.boughs) {
            for (const hog::flora::Ring& ring : bough.rings) {
                height = std::max(height, ring.centre.z);
            }
        }
        float lowest = height;
        float widest = 0.0F;
        for (const hog::flora::Frond& frond : tree.fronds) {
            lowest = std::min(lowest, frond.spine.front().centre.z);
            for (const hog::flora::Ring& ring : frond.spine) {
                widest = std::max(widest, glm::length(hog::Vec2{ring.centre}));
            }
        }
        shape.height += height / seeds;
        shape.fronds += static_cast<float>(tree.fronds.size()) / seeds;
        shape.crown_base += lowest / height / seeds;
        shape.girth += tree.boughs.front().rings.front().radius / height / seeds;
        shape.spread += 2.0F * widest / height / seeds;
    }
    return shape;
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
    CHECK(fingered(grown) == 0xb7fe'e3d8'80e4'99e2ULL);
}

TEST_CASE("a pine fits its frame", "[flora]") {
    const Skeleton tree = grow(pine(), 3U);
    REQUIRE(!tree.boughs.empty());
    const Bough& trunk = tree.boughs.front();
    CHECK(trunk.level == 0);
    // One trunk, its boughs, and a frond for every bough.
    CHECK(tree.boughs.size() == 1 + 64);
    CHECK(tree.fronds.size() == 64);
    for (const hog::flora::Frond& frond : tree.fronds) {
        REQUIRE(frond.spine.size() >= 2);
        CHECK(frond.width > 0.1F);
        CHECK(frond.width < 1.0F);
        CHECK(frond.tilt > 0.0F);
        CHECK(frond.tilt < 1.5F);
    }
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
                                               .frond_stride = 2,
                                               .frond_scale = 1.4F,
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

TEST_CASE("a pine in its prime is the pine its species describes", "[flora]") {
    for (std::uint32_t seed = 101; seed < 109; ++seed) {
        CHECK(fingered(mesh(grow(aged(pine(), prime_age), seed))) ==
              fingered(mesh(grow(pine(), seed))));
    }
}

TEST_CASE("a pine ages from an upswept sapling to a stout, high-crowned old tree", "[flora]") {
    const Shape sapling = shape_at(0.15F);
    const Shape young = shape_at(0.4F);
    const Shape prime = shape_at(prime_age);
    const Shape old = shape_at(0.95F);
    CHECK(sapling.height < young.height);
    CHECK(young.height < prime.height);
    CHECK(prime.height < old.height);
    CHECK(sapling.height < 0.5F * prime.height);
    CHECK(sapling.fronds < young.fronds);
    CHECK(young.fronds <= prime.fronds);
    // Saplings are green to the ground; old trees have shed their lower boughs.
    CHECK(sapling.crown_base < prime.crown_base);
    CHECK(prime.crown_base < old.crown_base);
    CHECK(old.crown_base > 0.3F);
    CHECK(old.girth > prime.girth);
    // Out of range ages hold at either end.
    CHECK(fingered(mesh(grow(aged(pine(), -1.0F), 7U))) ==
          fingered(mesh(grow(aged(pine(), 0.0F), 7U))));
    CHECK(fingered(mesh(grow(aged(pine(), 2.0F), 7U))) ==
          fingered(mesh(grow(aged(pine(), 1.0F), 7U))));
    // The same golden discipline as grow(), for a sapling and an old tree.
    CHECK(fingered(mesh(grow(aged(pine(), 0.15F), 7U))) == 0x06ec'd623'ae4a'efe0ULL);
    CHECK(fingered(mesh(grow(aged(pine(), 0.95F), 7U))) == 0x2878'44a3'f070'6f84ULL);
}

TEST_CASE("a species ages by its own spans, only its height rising regardless", "[flora]") {
    hog::flora::Species unchanging = pine();
    unchanging.ageing = {};
    const hog::flora::Species sapling = aged(unchanging, 0.15F);
    CHECK(sapling.levels[0].length < 0.5F * unchanging.levels[0].length);
    CHECK(sapling.levels[0].radius == unchanging.levels[0].radius);
    CHECK(sapling.levels[0].children == unchanging.levels[0].children);
    CHECK(sapling.levels[0].start == unchanging.levels[0].start);
    CHECK(sapling.levels[1].length == unchanging.levels[1].length);
    CHECK(sapling.levels[1].rise == unchanging.levels[1].rise);

    hog::flora::Species reaching = unchanging;
    reaching.ageing.reach = {.young = 0.5F, .old = 2.0F};
    CHECK(aged(reaching, 0.0F).levels[1].length == 0.5F * reaching.levels[1].length);
    CHECK(aged(reaching, 1.0F).levels[1].length == 2.0F * reaching.levels[1].length);
}

TEST_CASE("an oak spreads a broad crown of leaves on gnarled limbs", "[flora]") {
    const Shape pine_prime = shape_at(prime_age);
    const Shape prime = shape_at(prime_age, oak());
    const Shape sapling = shape_at(0.15F, oak());
    const Shape old = shape_at(0.95F, oak());
    CHECK(prime.spread > 1.6F * pine_prime.spread);
    CHECK(sapling.spread < prime.spread);
    CHECK(prime.spread < old.spread);
    CHECK(sapling.height < prime.height);
    CHECK(old.girth > prime.girth);
    // Its limbs are wood, not left to the leaves to cover.
    const Skeleton tree = grow(oak(), 7U);
    const TreeMesh trunk_only = mesh(tree, Detail{.wood_levels = 1});
    const TreeMesh limbed = mesh(tree, Detail{.wood_levels = oak().woody});
    CHECK(limbed.wood.size() > 2 * trunk_only.wood.size());
    CHECK(fingered(mesh(grow(hog::flora::species_of(hog::flora::Genus::Oak), 7U))) ==
          fingered(mesh(tree)));
    for (std::uint32_t seed = 101; seed < 105; ++seed) {
        CHECK(fingered(mesh(grow(aged(oak(), prime_age), seed))) ==
              fingered(mesh(grow(oak(), seed))));
    }
    CHECK(fingered(mesh(tree, Detail{.wood_levels = oak().woody})) == 0xd26c'1353'cae4'6736ULL);
    CHECK(fingered(mesh(grow(aged(oak(), 0.15F), 7U))) == 0x42b3'4987'2012'bb37ULL);
    CHECK(fingered(mesh(grow(aged(oak(), 0.95F), 7U))) == 0x4028'5d46'9695'8f1bULL);
}

TEST_CASE("a maple holds a dense oval crown on a straight trunk", "[flora]") {
    const Shape pine_prime = shape_at(prime_age);
    const Shape oak_prime = shape_at(prime_age, oak());
    const Shape prime = shape_at(prime_age, maple());
    CHECK(prime.spread > 1.1F * pine_prime.spread);
    CHECK(prime.spread < 0.75F * oak_prime.spread);
    CHECK(prime.fronds > 1.1F * oak_prime.fronds);
    CHECK(shape_at(0.15F, maple()).spread < prime.spread);
    CHECK(prime.spread < shape_at(0.95F, maple()).spread);
    CHECK(maple().levels[0].gnarl < 0.5F * oak().levels[0].gnarl);
    const Skeleton tree = grow(maple(), 7U);
    CHECK(fingered(mesh(grow(hog::flora::species_of(hog::flora::Genus::Maple), 7U))) ==
          fingered(mesh(tree)));
    CHECK(fingered(mesh(tree, Detail{.wood_levels = maple().woody})) == 0x390d'd169'7c7e'd1e8ULL);
}

TEST_CASE("a palm leans from its foot and curves back up under a crown of arching fronds",
          "[flora]") {
    const Species species = palm();
    for (std::uint32_t seed = 101; seed < 109; ++seed) {
        INFO("seed " << seed);
        const Skeleton tree = grow(species, seed);
        const Bough& trunk = tree.boughs.front();
        const float height = trunk.rings.back().centre.z;
        CHECK(tree.fronds.size() == 21);
        CHECK(tree.boughs.size() == 1 + 21 + 9 + 5);
        // Bare to its crown, every frond sprouting from the top of the trunk.
        for (const hog::flora::Frond& frond : tree.fronds) {
            CHECK(frond.spine.front().centre.z > 0.9F * height);
        }
        // Arching: most fronds' tips fall below the highest point along them.
        int arching = 0;
        for (const hog::flora::Frond& frond : tree.fronds) {
            float highest = frond.spine.front().centre.z;
            for (const hog::flora::Ring& ring : frond.spine) {
                highest = std::max(highest, ring.centre.z);
            }
            arching += frond.spine.back().centre.z < highest - 0.3F ? 1 : 0;
        }
        CHECK(arching > 14);
        // Leaning at its foot no more than the species allows, and nearer upright at its top.
        const auto upright = [](const hog::flora::Ring& ring) {
            return (ring.frame * Vec3{0, 0, 1}).z;
        };
        CHECK(upright(trunk.rings.front()) >= std::cos(species.lean) - 1e-4F);
        CHECK(upright(trunk.rings.back()) > upright(trunk.rings.front()));
        CHECK(glm::length(hog::Vec2{trunk.rings.back().centre}) < 0.3F * height);
    }
    // Each its own way: some lean well over, and not all the same way.
    float most = 0.0F;
    hog::Vec2 ways{0.0F};
    for (std::uint32_t seed = 101; seed < 109; ++seed) {
        const hog::Vec2 top{grow(species, seed).boughs.front().rings.back().centre};
        most = std::max(most, glm::length(top));
        ways += top;
    }
    CHECK(most > 1.0F);
    CHECK(glm::length(ways) < 4.0F * most);
}

TEST_CASE("coconuts hang round a palm's trunk under its crown, once it is old enough to bear",
          "[flora]") {
    const Species species = palm();
    const Skeleton tree = grow(species, 7U);
    const Bough& trunk = tree.boughs.front();
    const hog::flora::Ring top = trunk.rings.back();
    float lowest_frond = top.centre.z;
    for (const hog::flora::Frond& frond : tree.fronds) {
        lowest_frond = std::min(lowest_frond, frond.spine.front().centre.z);
    }
    int hanging = 0;
    int fallen = 0;
    for (const Bough& bough : tree.boughs) {
        if (bough.rings.size() != 5) {
            continue;
        }
        // Wood, closed at both ends, as wide as the species says.
        CHECK(bough.level == 0);
        CHECK(bough.rings.front().radius == 0.0F);
        CHECK(bough.rings.back().radius == 0.0F);
        CHECK(bough.rings[2].radius == species.fruit.radius);
        const Vec3 middle = bough.rings[2].centre;
        const float from_crown = glm::length(hog::Vec2{middle} - hog::Vec2{top.centre});
        if (middle.z > 1.0F) {
            ++hanging;
            // Hanging downward, under the crown and against the trunk.
            CHECK(bough.rings.back().centre.z < bough.rings.front().centre.z);
            CHECK(middle.z < lowest_frond);
            CHECK(middle.z > lowest_frond - 1.0F);
            CHECK(from_crown < 1.0F);
            continue;
        }
        ++fallen;
        // On its side, part sunk, rolled out from under the crown but not far.
        const float span = bough.rings.back().centre.z - bough.rings.front().centre.z;
        CHECK(std::abs(span) < 0.5F * species.fruit.length * 2.0F * species.fruit.radius);
        CHECK(middle.z > 0.0F);
        CHECK(middle.z < species.fruit.radius);
        CHECK(from_crown > species.fruit.radius);
        CHECK(from_crown < 1.0F + species.fruit.scatter);
    }
    CHECK(hanging == species.fruit.count);
    CHECK(fallen == species.fruit.fallen);
    CHECK(aged(species, 0.15F).fruit.count < 3);
    CHECK(aged(species, 0.0F).fruit.count == 0);
    CHECK(aged(species, prime_age).fruit.count == species.fruit.count);
    CHECK(aged(species, 1.0F).fruit.count < species.fruit.count);
    // None under a sapling; more lie under an old palm than in its prime, as they gather.
    CHECK(aged(species, 0.0F).fruit.fallen == 0);
    CHECK(aged(species, prime_age).fruit.fallen == species.fruit.fallen);
    CHECK(aged(species, 1.0F).fruit.fallen > species.fruit.fallen);
    const Skeleton sapling = grow(aged(species, 0.0F), 7U);
    CHECK(std::ranges::none_of(sapling.boughs,
                               [](const Bough& bough) { return bough.rings.size() == 5; }));
    // Bare species grow as they always have: leaning and bearing roll dice only where set.
    CHECK(pine().lean == 0.0F);
    CHECK(pine().fruit.count == 0);
    CHECK(fingered(mesh(tree)) == 0x2f2b'8190'c3f8'9c0fULL);
}

TEST_CASE("leaves hold full along a frond, needles thin evenly", "[flora]") {
    Skeleton twig;
    twig.fronds.push_back({.spine = {}, .width = 1.0F, .tilt = 0.0F, .fullness = 0.0F, .seed = 1});
    for (int ring = 0; ring <= 2; ++ring) {
        twig.fronds.back().spine.push_back({.centre = Vec3{0.0F, static_cast<float>(ring), 0.0F},
                                            .frame = hog::Quat{0.7071068F, -0.7071068F, 0.0F, 0.0F},
                                            .radius = 0.01F});
    }
    Skeleton leafy = twig;
    leafy.fronds.back().fullness = 1.0F;
    // Half the width across the middle of the frond, as the blade's two middle vertices span.
    const auto middle = [](const TreeMesh& grown) {
        return glm::length(grown.leaves.at(4).position - grown.leaves.at(5).position) * 0.5F;
    };
    CHECK(std::abs(middle(mesh(twig)) - 0.5F) < 1e-4F);
    CHECK(std::abs(middle(mesh(leafy)) - 0.75F) < 1e-4F);
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
            const auto tree =
                hog::flora::tree_grown(i, j, 0, hog::planet::Biome::BorealForest, temperate);
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
            fold(std::bit_cast<std::uint32_t>(tree->age));
        }
    }
    // Boreal woods are thick: most of this wooded block stands a tree.
    CHECK(grown > 1500);
    CHECK(grown < 4096);
    CHECK(hash == 0xe56f'5251'4772'7255ULL);
}

TEST_CASE("stands are mostly grown, the young at their edges, height following age", "[flora]") {
    float deep_age = 0.0F;
    float edge_age = 0.0F;
    float young_height = 0.0F;
    float old_height = 0.0F;
    std::size_t deep = 0;
    std::size_t edge = 0;
    std::size_t young = 0;
    std::size_t old = 0;
    for (std::uint32_t i = 2048; i < 2112; ++i) {
        for (std::uint32_t j = 56576; j < 56640; ++j) {
            const auto tree =
                hog::flora::tree_grown(i, j, 0, hog::planet::Biome::BorealForest, temperate);
            if (!tree) {
                continue;
            }
            CHECK(tree->age >= 0.0F);
            CHECK(tree->age <= 1.0F);
            if (tree->wooded > 0.95F) {
                deep_age += tree->age;
                ++deep;
            } else if (tree->wooded < 0.5F) {
                edge_age += tree->age;
                ++edge;
            }
            if (tree->age < 0.25F) {
                young_height += tree->height;
                ++young;
            } else if (tree->age > 0.75F) {
                old_height += tree->height;
                ++old;
            }
        }
    }
    REQUIRE(deep > 100);
    REQUIRE(edge > 10);
    REQUIRE(young > 10);
    REQUIRE(old > 100);
    CHECK(deep_age / static_cast<float>(deep) > 0.6F);
    CHECK(edge_age / static_cast<float>(edge) < deep_age / static_cast<float>(deep));
    CHECK(young_height / static_cast<float>(young) < 0.5F * old_height / static_cast<float>(old));
}

TEST_CASE("broadleaves gather in groves among temperate pines, the boreal woods all pine",
          "[flora]") {
    using hog::flora::Genus;
    using hog::planet::Biome;
    constexpr std::uint32_t across = 256;
    constexpr std::uint32_t rows = 64;
    std::vector<Genus> grown(std::size_t{across} * rows, Genus::Pine);
    std::vector<bool> stood(grown.size(), false);
    std::size_t oaks = 0;
    std::size_t maples = 0;
    std::size_t trees = 0;
    float oak_height = 0.0F;
    float pine_height = 0.0F;
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t at = 0; at < across; ++at) {
            const auto tree = hog::flora::tree_grown(2048 + at, 56576 + row, 0,
                                                     Biome::TemperateForest, temperate);
            CHECK(hog::flora::tree_grown(2048 + at, 56576 + row, 0, Biome::BorealForest, temperate)
                      .value_or(hog::flora::Stood{})
                      .genus == Genus::Pine);
            if (!tree) {
                continue;
            }
            ++trees;
            const std::size_t cell = (std::size_t{row} * across) + at;
            stood.at(cell) = true;
            grown.at(cell) = tree->genus;
            if (tree->genus == Genus::Oak) {
                ++oaks;
                oak_height += tree->height;
            } else if (tree->genus == Genus::Maple) {
                ++maples;
            } else {
                pine_height += tree->height;
            }
            hash ^= static_cast<std::uint64_t>(tree->genus);
            hash *= 0x100'0000'01b3ULL;
        }
    }
    REQUIRE(trees > 1000);
    const std::size_t broadleaves = oaks + maples;
    const float share = static_cast<float>(broadleaves) / static_cast<float>(trees);
    CHECK(share > 0.3F);
    CHECK(share < 0.8F);
    const float maple_share = static_cast<float>(maples) / static_cast<float>(broadleaves);
    CHECK(maple_share > 0.15F);
    CHECK(maple_share < 0.65F);
    CHECK(oak_height / static_cast<float>(oaks) <
          pine_height / static_cast<float>(trees - broadleaves));
    // Grouped: a tree's neighbour is its own kind far more often than a random mix would make it.
    std::size_t pairs = 0;
    std::size_t alike = 0;
    for (std::size_t cell = 0; cell + 1 < grown.size(); ++cell) {
        if ((cell + 1) % across != 0 && stood.at(cell) && stood.at(cell + 1)) {
            ++pairs;
            alike += grown.at(cell) == grown.at(cell + 1) ? 1U : 0U;
        }
    }
    const float oak_share = static_cast<float>(oaks) / static_cast<float>(trees);
    const float of_maples = static_cast<float>(maples) / static_cast<float>(trees);
    const float of_pines = 1.0F - share;
    const float chance = (oak_share * oak_share) + (of_maples * of_maples) + (of_pines * of_pines);
    CHECK(static_cast<float>(alike) / static_cast<float>(pairs) > chance + 0.15F);
    CHECK(hash == 0xee88'7392'8ab1'264eULL);
}

TEST_CASE("maples crowd the wet woods more than the dry", "[flora]") {
    using hog::flora::Genus;
    using hog::planet::Biome;
    const auto maple_share = [](Biome biome) {
        std::size_t broadleaves = 0;
        std::size_t maples = 0;
        for (std::uint32_t i = 2048; i < 2304; ++i) {
            for (std::uint32_t j = 56576; j < 56640; ++j) {
                const auto tree = hog::flora::tree_grown(i, j, 0, biome, temperate);
                if (tree && tree->genus != Genus::Pine) {
                    ++broadleaves;
                    maples += tree->genus == Genus::Maple ? 1U : 0U;
                }
            }
        }
        REQUIRE(broadleaves > 100);
        return static_cast<float>(maples) / static_cast<float>(broadleaves);
    };
    CHECK(maple_share(Biome::Wetland) > 0.4F);
    CHECK(maple_share(Biome::Wetland) > 2.0F * maple_share(Biome::Shrubland));
}

TEST_CASE("each kind grows within its own climate, thinning at its edges", "[flora]") {
    using hog::flora::Genus;
    using hog::flora::Habitat;
    using hog::flora::suited;
    const auto at = [](float temperature) { return Habitat{.temperature = temperature}; };
    // Frost-tender palms only where it is warm all year; pines to a cold treeline, giving way to
    // the broadleaves in the heat.
    CHECK(suited(Genus::Palm, at(13.0F)) == 0.0F);
    CHECK(suited(Genus::Palm, at(15.0F)) > 0.0F);
    CHECK(suited(Genus::Palm, at(15.0F)) < 1.0F);
    CHECK(suited(Genus::Palm, at(17.0F)) == 1.0F);
    CHECK(suited(Genus::Palm, at(30.0F)) == 1.0F);
    CHECK(suited(Genus::Pine, at(-11.0F)) == 0.0F);
    CHECK(suited(Genus::Pine, at(-3.0F)) == 1.0F);
    CHECK(suited(Genus::Pine, at(26.0F)) == 0.0F);
    CHECK(suited(Genus::Oak, at(27.0F)) == 1.0F);
    CHECK(suited(Genus::Maple, at(24.0F)) == 0.0F);
    // Maples want wet ground and thin out on drier; pines bear the driest; oaks drown where the
    // ground is waterlogged.
    const Habitat dry{.moisture = 0.3F};
    CHECK(suited(Genus::Maple, dry) == 0.0F);
    CHECK(suited(Genus::Maple, Habitat{.moisture = 0.4F}) > 0.0F);
    CHECK(suited(Genus::Maple, Habitat{.moisture = 0.4F}) < 1.0F);
    CHECK(suited(Genus::Oak, dry) == 1.0F);
    CHECK(suited(Genus::Pine, Habitat{.moisture = 0.16F}) == 1.0F);
    CHECK(suited(Genus::Oak, Habitat{.moisture = 0.16F}) < 1.0F);
    CHECK(suited(Genus::Pine, Habitat{.moisture = 0.04F}) == 0.0F);
    CHECK(suited(Genus::Oak, Habitat{.moisture = 0.95F}) == 0.0F);
    CHECK(suited(Genus::Maple, Habitat{.moisture = 0.95F}) == 1.0F);
    // A water table within the roots' reach wets dry ground, the shallower the wetter.
    CHECK(hog::flora::wetness(dry) == 0.3F);
    CHECK(hog::flora::wetness(Habitat{.moisture = 0.3F, .water_table = 1.0F}) == 0.75F);
    CHECK(hog::flora::wetness(Habitat{.moisture = 0.3F, .water_table = 0.0F}) == 1.0F);
    CHECK(suited(Genus::Maple, Habitat{.moisture = 0.3F, .water_table = 1.0F}) == 1.0F);
    for (const Genus genus : {Genus::Pine, Genus::Oak, Genus::Maple, Genus::Palm}) {
        const hog::flora::Niche niche = hog::flora::niche_of(genus);
        CHECK(niche.warmth.low < niche.warmth.high);
        CHECK(niche.wetness.low < niche.wetness.high);
        // The mild, moderately wet woods suit every kind but the palm.
        if (genus != Genus::Palm) {
            CHECK(suited(genus, temperate) == 1.0F);
        }
        CHECK(suited(genus, Habitat{.temperature = 0.5F * (niche.warmth.low + niche.warmth.high),
                                    .moisture = 0.5F * (niche.wetness.low + niche.wetness.high)}) ==
              1.0F);
    }
}

TEST_CASE("palms line warm shores and stand among the tropics' broadleaves, never in the cold",
          "[flora]") {
    using hog::flora::Genus;
    using hog::planet::Biome;
    struct Count {
        std::size_t trees = 0;
        std::size_t palms = 0;
        std::size_t pines = 0;
        std::size_t maples = 0;
    };
    const auto count = [](Biome biome, float temperature) {
        const hog::flora::Habitat habitat{.temperature = temperature};
        Count counted;
        for (std::uint32_t i = 2048; i < 2304; ++i) {
            for (std::uint32_t j = 56576; j < 56640; ++j) {
                const auto tree = hog::flora::tree_grown(i, j, 0, biome, habitat);
                if (!tree) {
                    continue;
                }
                ++counted.trees;
                counted.palms += tree->genus == Genus::Palm ? 1U : 0U;
                counted.pines += tree->genus == Genus::Pine ? 1U : 0U;
                counted.maples += tree->genus == Genus::Maple ? 1U : 0U;
            }
        }
        return counted;
    };
    // A warm beach is all palms; a cold one bare; a cool one thinner.
    const Count warm_shore = count(Biome::Beach, 24.0F);
    CHECK(warm_shore.trees > 1000);
    CHECK(warm_shore.palms == warm_shore.trees);
    CHECK(count(Biome::Beach, 10.0F).trees == 0);
    const Count cool_shore = count(Biome::Beach, 15.0F);
    CHECK(cool_shore.trees > warm_shore.trees / 4);
    CHECK(cool_shore.trees < 3 * warm_shore.trees / 4);
    // The rainforest a mix of palms and broadleaves, too hot for pines or maples.
    const Count rainforest = count(Biome::TropicalRainforest, 27.0F);
    CHECK(rainforest.trees > 3000);
    CHECK(rainforest.palms > rainforest.trees / 8);
    CHECK(rainforest.palms < rainforest.trees / 2);
    CHECK(rainforest.pines == 0);
    CHECK(rainforest.maples == 0);
    // No palms in mild woods, and nothing past the pines' treeline.
    CHECK(count(Biome::TemperateForest, temperate.temperature).palms == 0);
    CHECK(count(Biome::BorealForest, -11.0F).trees == 0);
}

TEST_CASE("trees live, fail, die and grow back in the order their own dice set", "[flora]") {
    using hog::flora::Condition;
    using hog::flora::Habitat;
    using hog::flora::Stood;
    using hog::planet::Biome;
    // Each lattice cell's tree under a habitat, by cell.
    const auto grown = [](const Habitat& habitat) {
        std::vector<std::optional<Stood>> trees;
        for (std::uint32_t i = 2048; i < 2176; ++i) {
            for (std::uint32_t j = 56576; j < 56640; ++j) {
                trees.push_back(hog::flora::tree_grown(i, j, 0, Biome::TemperateForest, habitat));
            }
        }
        return trees;
    };
    const auto counted = [](const std::vector<std::optional<Stood>>& trees, Condition condition) {
        return static_cast<double>(std::ranges::count_if(
            trees, [&](const auto& tree) { return tree && tree->condition == condition; }));
    };
    const std::vector<std::optional<Stood>> untouched = grown(temperate);
    const double all = counted(untouched, Condition::Sound);
    REQUIRE(all > 3000.0);
    CHECK(counted(untouched, Condition::Sound) ==
          static_cast<double>(
              std::ranges::count_if(untouched, [](const auto& tree) { return tree.has_value(); })));

    // A drought killing a fifth, with a fifth more dying; a fire and its standing dead.
    const std::vector<std::optional<Stood>> drought =
        grown(Habitat{.living = 0.6F, .failing = 0.2F, .dead = 0.2F});
    const std::vector<std::optional<Stood>> later =
        grown(Habitat{.living = 0.4F, .established = 0.4F, .dead = 0.4F});
    const std::vector<std::optional<Stood>> burned =
        grown(Habitat{.living = 0.05F, .established = 0.05F, .dead = 0.9F, .charred = 1.0F});
    const std::vector<std::optional<Stood>> regrowing =
        grown(Habitat{.living = 0.5F, .established = 0.1F, .dead = 0.1F, .charred = 1.0F});
    CHECK(std::abs((counted(drought, Condition::Sound) / all) - 0.4) < 0.03);
    CHECK(std::abs((counted(drought, Condition::Failing) / all) - 0.2) < 0.03);
    CHECK(std::abs((counted(drought, Condition::Dead) / all) - 0.2) < 0.03);
    CHECK(counted(drought, Condition::Charred) == 0.0);
    CHECK(std::abs((counted(burned, Condition::Charred) / all) - 0.9) < 0.03);
    CHECK(std::abs((counted(burned, Condition::Scorched) / all) - 0.05) < 0.02);
    CHECK(counted(burned, Condition::Sound) == 0.0);
    for (std::size_t at = 0; at < untouched.size(); ++at) {
        // Every tree standing is one the untouched woods stand, of the same kind and spot.
        for (const auto* fared : {&drought, &later, &burned, &regrowing}) {
            const std::optional<Stood>& tree = (*fared)[at];
            if (tree) {
                REQUIRE(untouched[at].has_value());
                CHECK(tree->genus == untouched[at]->genus);
                CHECK(tree->hash == untouched[at]->hash);
                CHECK(tree->age <= untouched[at]->age);
            }
        }
        // Those failing are the next to die; those dead stay dead.
        if (drought[at] && drought[at]->condition == Condition::Failing) {
            REQUIRE(later[at].has_value());
            CHECK(later[at]->condition == Condition::Dead);
        }
        if (drought[at] && drought[at]->condition == Condition::Dead) {
            CHECK(!(later[at] && hog::flora::alive(later[at]->condition)));
        }
        // Grown back since the fire: the trees above the old growth younger, and shorter.
        if (regrowing[at] && hog::flora::alive(regrowing[at]->condition) &&
            regrowing[at]->age < untouched[at]->age) {
            CHECK(regrowing[at]->height < untouched[at]->height);
        }
    }
    double younger = 0.0;
    double kept = 0.0;
    for (std::size_t at = 0; at < untouched.size(); ++at) {
        if (regrowing[at] && regrowing[at]->condition == Condition::Sound) {
            (regrowing[at]->age < untouched[at]->age ? younger : kept) += 1.0;
        }
    }
    // Of the old growth, those scorched as the dead still stand thick (4 x 0.1 of them) about them.
    CHECK(std::abs((kept / all) - 0.06) < 0.02);
    CHECK(std::abs((counted(regrowing, Condition::Scorched) / all) - 0.04) < 0.02);
    CHECK(std::abs((younger / all) - 0.4) < 0.03);

    // The golden number for woods the sim has touched.
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ULL;
    for (const auto* fared : {&drought, &burned, &regrowing}) {
        for (const std::optional<Stood>& tree : *fared) {
            const std::uint32_t parts[] = {tree ? static_cast<std::uint32_t>(tree->condition) + 1U
                                                : 0U,
                                           tree ? std::bit_cast<std::uint32_t>(tree->age) : 0U,
                                           tree ? std::bit_cast<std::uint32_t>(tree->height) : 0U};
            hash = fingered(std::as_bytes(std::span{parts}), hash);
        }
    }
    CHECK(hash == 0xbdce'5fbd'628f'3f43ULL);
}

TEST_CASE("no tree grows where its biome grows none", "[flora]") {
    for (std::uint32_t i = 2048; i < 2112; ++i) {
        CHECK(!hog::flora::tree_grown(i, 56576, 0, hog::planet::Biome::Ocean, temperate));
        CHECK(!hog::flora::tree_grown(i, 56576, 0, hog::planet::Biome::Desert, temperate));
    }
    // The spot within a cell is answered even where nothing grows, for sampling the ground first.
    const hog::Vec2 spot = hog::flora::tree_spot(2048, 56576, 0);
    CHECK(spot.x >= 0.0F);
    CHECK(spot.x < 1.0F);
    CHECK(spot.y >= 0.0F);
    CHECK(spot.y < 1.0F);
}

TEST_CASE("an oak's leaves turn and fall as the days shorten, and come back in spring",
          "[flora][season]") {
    using hog::flora::Daylight;
    using hog::flora::leaves_at;
    const hog::flora::Leafing& leafing = oak().leafing;
    // Earth's days at 45 degrees north.
    constexpr float longest = 15.4F;
    const auto shortening = [](float hours) {
        return Daylight{.hours = hours, .longest = longest, .lengthening = false};
    };
    const auto lengthening = [](float hours) {
        return Daylight{.hours = hours, .longest = longest, .lengthening = true};
    };

    const auto july = leaves_at(leafing, 0.0F, shortening(15.0F));
    CHECK(july.turned == 0.0F);
    CHECK(july.held == 1.0F);
    const auto october = leaves_at(leafing, 0.0F, shortening(10.9F)); // the 20th or so
    CHECK(october.turned > 0.2F);
    CHECK(october.turned < 1.0F);
    CHECK(october.held == 1.0F);
    CHECK(leaves_at(leafing, 0.0F, shortening(9.4F)).held == 0.0F);   // late November
    CHECK(leaves_at(leafing, 0.0F, lengthening(9.0F)).held == 0.0F);  // January
    CHECK(leaves_at(leafing, 0.0F, lengthening(12.6F)).held == 0.0F); // April
    const auto june = leaves_at(leafing, 0.0F, lengthening(15.3F));
    CHECK(june.turned == 0.0F);
    CHECK(june.held == 1.0F);

    // Some trees turn early, the same every year.
    const auto early = leaves_at(leafing, 1.0F, shortening(10.9F));
    const auto late = leaves_at(leafing, -1.0F, shortening(10.9F));
    CHECK(early.turned > october.turned);
    CHECK(late.turned < october.turned);

    // Pines keep their needles.
    for (const float hours : {9.0F, 11.0F, 15.0F}) {
        CHECK(leaves_at(pine().leafing, 1.0F, shortening(hours)).held == 1.0F);
        CHECK(leaves_at(pine().leafing, 1.0F, shortening(hours)).turned == 0.0F);
    }
}

TEST_CASE("oaks keep their leaves near the equator, and grow them back further from it",
          "[flora][season]") {
    using hog::flora::Daylight;
    using hog::flora::leaves_at;
    const hog::flora::Leafing& leafing = oak().leafing;
    // The tropics barely change: the earliest trees are only tinged.
    for (const bool lengthening : {false, true}) {
        const auto tropical = leaves_at(
            leafing, 1.0F, {.hours = 11.7F, .longest = 12.3F, .lengthening = lengthening});
        CHECK(tropical.held == 1.0F);
        CHECK(tropical.turned < 0.3F);
    }
    // At 25 degrees, a winter's turning but no more, and a green summer.
    constexpr float longest = 13.6F;
    CHECK(leaves_at(leafing, 0.0F, {.hours = 10.4F, .longest = longest}).turned > 0.5F);
    CHECK(leaves_at(leafing, 0.0F, {.hours = 10.4F, .longest = longest}).held > 0.5F);
    const auto summer =
        leaves_at(leafing, 0.0F, {.hours = 13.5F, .longest = longest, .lengthening = true});
    CHECK(summer.turned == 0.0F);
    CHECK(summer.held == 1.0F);
}

TEST_CASE("maples turn before the oaks, brighter, and are bare sooner", "[flora][season]") {
    using hog::flora::Daylight;
    using hog::flora::leaves_at;
    constexpr float longest = 15.4F;
    const auto shortening = [](float hours) {
        return Daylight{.hours = hours, .longest = longest, .lengthening = false};
    };
    const auto oaks = [&](float hours) {
        return leaves_at(oak().leafing, 0.0F, shortening(hours));
    };
    const auto maples = [&](float hours) {
        return leaves_at(maple().leafing, 0.0F, shortening(hours));
    };
    CHECK(maples(15.0F).turned == 0.0F);
    CHECK(maples(11.3F).turned > 0.3F); // early October
    CHECK(oaks(11.3F).turned < 0.25F * maples(11.3F).turned);
    CHECK(maples(10.9F).turned > 0.9F);
    CHECK(maples(10.9F).held < 1.0F);
    CHECK(maples(10.2F).held == 0.0F); // the first days of November
    CHECK(oaks(10.2F).held > 0.5F);
    // Out before the oaks.
    const Daylight may{.hours = 14.6F, .longest = longest, .lengthening = true};
    CHECK(leaves_at(maple().leafing, 0.0F, may).held > leaves_at(oak().leafing, 0.0F, may).held);
    // Their reds and oranges, brighter than an oak's browns.
    const auto bright = [](Vec3 colour) { return colour.r + colour.g + colour.b; };
    CHECK(bright(maple().leafing.first) > 1.5F * bright(oak().leafing.first));
    CHECK(maple().leafing.last.r > 4.0F * maple().leafing.last.g);
    // Near the equator, the earliest only colour; none lose their leaves.
    const auto tropical =
        leaves_at(maple().leafing, 1.0F, {.hours = 11.7F, .longest = 12.3F, .lengthening = false});
    CHECK(tropical.held == 1.0F);
    CHECK(tropical.turned < 0.7F);
}

TEST_CASE("each tree keeps its own dates, the old turning first", "[flora][season]") {
    using hog::flora::own_season;
    CHECK(own_season(1234U, 0.5F) == own_season(1234U, 0.5F));
    float least = 1.0F;
    float most = -1.0F;
    float young = 0.0F;
    float old = 0.0F;
    constexpr std::uint32_t trees = 2'000;
    for (std::uint32_t hash = 0; hash < trees; ++hash) {
        const float own = own_season(hash * 7919U, 0.5F);
        least = std::min(least, own);
        most = std::max(most, own);
        young += own_season(hash * 7919U, 0.1F);
        old += own_season(hash * 7919U, 0.95F);
    }
    CHECK(least < -0.6F);
    CHECK(most > 0.6F);
    CHECK(old > young + (0.3F * trees));
}

TEST_CASE("woodland far off holds the trees grown there: as many, as tall, of the same kinds",
          "[flora]") {
    using hog::flora::Genus;
    using hog::planet::Biome;
    constexpr std::uint32_t across = 320;
    constexpr std::uint32_t block = 32;
    using hog::flora::Habitat;
    struct Site {
        Biome biome;
        Habitat habitat;
    };
    // Mild woods of every kind, and those where a kind thins at the edge of its climate: the oaks
    // in a cold wetland and over a shallow water table, the pines at their treeline, the palms on
    // a cool shore, the maples in drier woods.
    for (const Site site :
         {Site{.biome = Biome::TemperateForest, .habitat = temperate},
          {.biome = Biome::BorealForest, .habitat = temperate},
          {.biome = Biome::Grassland, .habitat = temperate},
          {.biome = Biome::Shrubland, .habitat = temperate},
          {.biome = Biome::Wetland, .habitat = temperate},
          {.biome = Biome::Wetland, .habitat = {.temperature = 2.5F}},
          {.biome = Biome::Wetland, .habitat = {.water_table = 0.5F}},
          {.biome = Biome::TemperateForest, .habitat = {.moisture = 0.42F}},
          {.biome = Biome::BorealForest, .habitat = {.temperature = -8.0F}},
          {.biome = Biome::Beach, .habitat = {.temperature = 15.0F}},
          {.biome = Biome::TropicalRainforest, .habitat = {.temperature = 27.0F}},
          {.biome = Biome::Savanna, .habitat = {.temperature = 28.0F}},
          // Growing back after a fire, and dying back in a drought.
          {.biome = Biome::TemperateForest,
           .habitat = {.living = 0.6F, .established = 0.15F, .dead = 0.2F, .charred = 1.0F}},
          {.biome = Biome::BorealForest,
           .habitat = {.living = 0.7F, .failing = 0.2F, .dead = 0.25F}}}) {
        const Biome biome = site.biome;
        INFO("biome " << static_cast<int>(biome) << " at " << site.habitat.temperature << " C, "
                      << hog::flora::wetness(site.habitat) << " wet");
        // Counted from the trees, then expected from the woodland, over the whole and by block.
        double trees = 0.0;
        double heights = 0.0;
        double broadleaves = 0.0;
        double maples = 0.0;
        double palms = 0.0;
        double grown = 0.0;
        double tall = 0.0;
        double broad = 0.0;
        double maple = 0.0;
        double palm = 0.0;
        double worst_block = 0.0;
        for (std::uint32_t block_j = 0; block_j < across; block_j += block) {
            for (std::uint32_t block_i = 0; block_i < across; block_i += block) {
                double counted = 0.0;
                double expected = 0.0;
                for (std::uint32_t j = block_j; j < block_j + block; ++j) {
                    for (std::uint32_t i = block_i; i < block_i + block; ++i) {
                        const std::uint32_t lattice_i = 4096 + i;
                        const std::uint32_t lattice_j = 9000 + j;
                        const hog::flora::Woodland woodland =
                            hog::flora::woodland_at({static_cast<float>(lattice_i) + 0.5F,
                                                     static_cast<float>(lattice_j) + 0.5F},
                                                    1, biome, site.habitat);
                        const auto share = static_cast<double>(woodland.grown);
                        const double others = 1.0 - double{woodland.palm};
                        expected += share;
                        tall += share * double{woodland.height};
                        broad += share * others * double{woodland.broadleaf};
                        maple +=
                            share * others * double{woodland.broadleaf} * double{woodland.maple};
                        palm += share * double{woodland.palm};
                        const auto tree =
                            hog::flora::tree_grown(lattice_i, lattice_j, 1, biome, site.habitat);
                        if (!tree || !hog::flora::alive(tree->condition)) {
                            continue;
                        }
                        counted += 1.0;
                        heights += double{tree->height / hog::flora::stature(tree->genus)};
                        broadleaves +=
                            tree->genus == Genus::Oak || tree->genus == Genus::Maple ? 1.0 : 0.0;
                        maples += tree->genus == Genus::Maple ? 1.0 : 0.0;
                        palms += tree->genus == Genus::Palm ? 1.0 : 0.0;
                    }
                }
                trees += counted;
                grown += expected;
                worst_block = std::max(worst_block, std::abs(counted - expected) / (block * block));
            }
        }
        REQUIRE(trees > 2000.0);
        INFO("trees " << trees << " against " << grown << ", worst block off by " << worst_block
                      << " of its cells");
        CHECK(std::abs((grown / trees) - 1.0) < 0.03);
        CHECK(worst_block < 0.05);
        CHECK(std::abs((tall / grown) / (heights / trees) - 1.0) < 0.03);
        CHECK(std::abs((broad / grown) - (broadleaves / trees)) < 0.03);
        CHECK(std::abs((palm / grown) - (palms / trees)) < 0.03);
        if (broadleaves > 500.0) {
            CHECK(std::abs((maple / broad) - (maples / broadleaves)) < 0.04);
        }
    }
}

TEST_CASE("the woods' fates are read between the cells' middles, held at a face's edges",
          "[flora]") {
    using hog::flora::Fate;
    using hog::flora::Fates;
    // Untouched where nothing was told.
    CHECK(Fates{}.at(3, 0.3, 0.8) == Fate{});
    CHECK(Fates{}.untouched());

    // Level 1: two cells a side, each face's cells their own.
    std::vector<Fate> cells(24);
    for (std::uint32_t index = 0; index < cells.size(); ++index) {
        cells[index] = {.living = static_cast<float>(index) / 32.0F,
                        .established = static_cast<float>(index) / 64.0F,
                        .failing = 0.0F,
                        .dead = 1.0F - (static_cast<float>(index) / 32.0F),
                        .charred = 0.5F};
    }
    const Fates fates{1, cells};
    CHECK(fates.level() == 1);
    CHECK(fates.size() == 24);
    // Face 2's cells: (0,0) is 8, (1,0) 9, (0,1) 10, (1,1) 11. Each its own at its middle.
    CHECK(fates.at(2, 0.25, 0.25) == cells[8]);
    CHECK(fates.at(2, 0.75, 0.75) == cells[11]);
    // Halfway between two, half of each; at the face's edges and corners, the cell there.
    CHECK(fates.at(2, 0.5, 0.25).living == (cells[8].living + cells[9].living) / 2.0F);
    CHECK(fates.at(2, 0.5, 0.5).living ==
          (cells[8].living + cells[9].living + cells[10].living + cells[11].living) / 4.0F);
    CHECK(fates.at(2, 0.0, 0.0) == cells[8]);
    CHECK(fates.at(2, 1.0, 0.1) == cells[9]);
    CHECK(fates.at(2, 0.9, 1.0) == cells[11]);
    // Read between, the shares keep their order: never more established or dead than they can be.
    for (double s = 0.0; s <= 1.0; s += 0.0625) {
        for (double t = 0.0; t <= 1.0; t += 0.0625) {
            const Fate fate = fates.at(4, s, t);
            CHECK(fate.established <= fate.living);
            CHECK(fate.living + fate.dead <= 1.0F + 1e-6F);
        }
    }
    // Taken into a habitat, its climate kept.
    const hog::flora::Habitat fared = hog::flora::fared(temperate, cells[5]);
    CHECK(fared.temperature == temperate.temperature);
    CHECK(fared.living == cells[5].living);
    CHECK(fared.dead == cells[5].dead);
    CHECK(fared.charred == 0.5F);
}
