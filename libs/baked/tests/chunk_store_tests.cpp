#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>
#include <hog/testing/temporary_directory.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

using hog::Result;
using hog::baked::Baking;
using hog::baked::Bands;
using hog::baked::Chunk;
using hog::baked::ChunkStore;
using hog::grid::Key;
using hog::grid::Tree;

namespace {

// 1 m nodes 11 deep.
Baking baking(std::uint32_t generator = 1) {
    return {.world = 42, .grid = hog::grid::Grid{1000.0, 1.0}, .generator = generator};
}

ChunkStore::Options options(std::uint8_t band_height, std::size_t budget) {
    return {.map_bytes = std::uint64_t{256} << 20U,
            .readers = 64,
            .read_only = false,
            .band_height = band_height,
            .budget = budget};
}

ChunkStore scratch(const Baking& baked, std::uint8_t band_height, std::size_t budget) {
    auto store = ChunkStore::scratch(baked, options(band_height, budget));
    REQUIRE(store.has_value());
    return std::move(*store);
}

template <typename Kind>
std::vector<Chunk> put(ChunkStore& store, std::uint8_t band, Key root, const Tree<Kind>& tree) {
    std::vector<Chunk> touched;
    const Result<void> done = store.write([&](ChunkStore::Batch& batch) -> Result<void> {
        auto put = batch.put(band, root, tree);
        if (!put) {
            return std::unexpected{put.error()};
        }
        touched = std::move(*put);
        return {};
    });
    REQUIRE(done.has_value());
    return touched;
}

// A sheet of distinct materials across z at 0.37 of the way up, `levels` deep; elsewhere leaves
// of their own.
Tree<hog::grid::Material> sheet(std::uint8_t levels, std::uint64_t seed) {
    using Material = Tree<hog::grid::Material>;
    return Material::grow(levels,
                          [&](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
                              std::uint32_t z) -> Material::Decided {
                              const auto across = static_cast<double>(1U << depth);
                              const bool crosses = z / across <= 0.37 && 0.37 < (z + 1) / across;
                              return {.value = static_cast<std::uint16_t>(
                                          1 + (hog::hash_values(seed, depth, x, y, z) % 50)),
                                      .open = false,
                                      .split = crosses && depth < levels};
                          });
}

Key under(Key root, std::uint8_t depth, std::uint64_t pick) {
    Key key = root;
    while (key.depth < depth) {
        key = key.child(hog::hash_values(pick, key.depth) % 8);
    }
    return key;
}

// A direction whose ground stands over the sea, or under it by 30 m or more.
hog::DVec3 spread_until(bool land) {
    const auto& world = hog::testing::small_world();
    const double sea = world.sea_level().value_or(world.planet().body.radius);
    for (std::uint64_t n = 0;; ++n) {
        const hog::DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(7, (n * 2'654'435'761U) % 98'304U));
        const double ground =
            hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value_or(0.0);
        if (land ? ground > sea + 5.0 : ground < sea - 30.0) {
            return up * ground;
        }
    }
}

// Ground filled once for every test: over sea floor or land, under a node 5 levels over the
// finest.
struct Place {
    hog::DVec3 point;
    Key key;
    hog::globe::Filled filled;
};

const Place& place(bool land) {
    static const std::array<Place, 2> places = [] {
        const hog::globe::Generator& world = hog::testing::small_world();
        const hog::grid::Grid grid = hog::globe::grid_of(world);
        const auto make = [&](bool on_land) {
            const hog::DVec3 point = spread_until(on_land);
            const Key key = grid.key_at(point, static_cast<std::uint8_t>(grid.depth() - 5)).value();
            return Place{
                .point = point, .key = key, .filled = hog::globe::fill(world, grid, key, 5)};
        };
        return std::array<Place, 2>{make(false), make(true)};
    }();
    return places.at(land ? 1 : 0);
}

// The piece of `tree`, rooted at `root`, under `node`, down to `depth`.
template <typename Kind>
Tree<Kind> cut_at(const Tree<Kind>& tree, Key root, Key node, std::uint8_t depth) {
    const auto shift = static_cast<std::uint8_t>(node.depth - root.depth);
    return tree.cut(shift, node.x - (root.x << shift), node.y - (root.y << shift),
                    node.z - (root.z << shift), static_cast<std::uint8_t>(depth - node.depth));
}

// Holds each band `filled`, rooted at `key`, reaches whole, and gives the deepest held.
template <typename Kind>
std::uint8_t hold(ChunkStore& store, Key key, const Tree<Kind>& filled) {
    const Bands& bands = store.bands();
    std::uint8_t deepest = key.depth;
    for (std::uint8_t band = 0; band < bands.count(); ++band) {
        if (bands.roots(band) < key.depth || bands.foot(band) > key.depth + filled.levels()) {
            continue;
        }
        put(store, band, key,
            filled.cut(0, 0, 0, 0, static_cast<std::uint8_t>(bands.foot(band) - key.depth)));
        deepest = bands.foot(band);
    }
    return deepest;
}

// Every node on the way from `key` to `place`, read from the store to each depth held, matches
// what was filled.
template <typename Kind>
void check_pieces(const ChunkStore& store, const hog::grid::Grid& grid, Key key, hog::DVec3 place,
                  const Tree<Kind>& filled, std::uint8_t deepest) {
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    for (std::uint8_t at = key.depth; at <= deepest; ++at) {
        const Key node = grid.key_at(place, at).value();
        // The band holding the key's own depth lies over the key, so isn't held.
        for (auto depth = std::max(at, static_cast<std::uint8_t>(key.depth + 1)); depth <= deepest;
             ++depth) {
            CAPTURE(at, depth);
            const auto piece = snapshot->piece<Kind>(node, depth);
            REQUIRE(piece.has_value());
            CHECK(piece.value().tree.same_as(cut_at(filled, key, node, depth)));
            CHECK(!piece.value().chunks.empty());
        }
    }
}

} // namespace

TEST_CASE("bands take the depths a few levels at a time from the root", "[baked][chunks]") {
    const Bands bands{3, 11};
    CHECK(bands.count() == 4);
    CHECK(bands.band_of(0) == 0);
    CHECK(bands.band_of(3) == 0);
    CHECK(bands.band_of(4) == 1);
    CHECK(bands.band_of(11) == 3);
    CHECK(bands.roots(2) == 6);
    CHECK(bands.foot(2) == 9);
    CHECK(bands.foot(3) == 11);
    CHECK(Bands{4, 12}.count() == 3);
    CHECK(Bands{1, 5}.foot(4) == 5);
}

TEST_CASE("a piece read from chunks matches the tree the generator fills", "[baked][chunks]") {
    const hog::globe::Generator& world = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(world);
    const Baking baked = hog::baked::baking_of(world);
    for (const bool land : {false, true}) {
        const Place& at = place(land);
        for (const int height : {2, 3, 4}) {
            // Chunks of a band are rooted no deeper than its roots, so the fill is held from the
            // first node under it at one of those.
            const auto depth =
                static_cast<std::uint8_t>(((at.key.depth + height - 1) / height) * height);
            const Key key = grid.key_at(at.point, depth).value();
            const auto check = [&]<typename Kind>(ChunkStore& store, const Tree<Kind>& filled) {
                const Tree<Kind> under = cut_at(filled, at.key, key, at.key.depth + 5);
                check_pieces(store, grid, key, at.point, under, hold(store, key, under));
            };
            for (const std::size_t budget :
                 {std::size_t{256}, std::size_t{4} << 10U, std::size_t{1} << 20U}) {
                CAPTURE(land, height, budget);
                ChunkStore store = scratch(baked, static_cast<std::uint8_t>(height), budget);
                check(store, at.filled.shape);
                check(store, at.filled.material);
                check(store, at.filled.water);
                check(store, at.filled.woods);
            }
        }
    }
}

TEST_CASE("open sky is one chunk a band, and smooth ground takes fewer than rugged",
          "[baked][chunks]") {
    const hog::globe::Generator& world = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(world);
    constexpr std::uint8_t height = 2;
    std::vector<std::size_t> counts;
    for (const bool land : {false, true}) {
        const Place& at = place(land);
        ChunkStore store = scratch(hog::baked::baking_of(world), height, 1024);
        const auto depth = static_cast<std::uint8_t>(((at.key.depth + 1) / height) * height);
        const Key key = grid.key_at(at.point, depth).value();
        const std::uint8_t deepest =
            hold(store, key, cut_at(at.filled.shape, at.key, key, at.key.depth + 5));
        const auto snapshot = store.read();
        REQUIRE(snapshot.has_value());
        counts.push_back(
            snapshot->chunks<hog::grid::Shape>(store.bands().band_of(deepest), key).size());
    }
    CHECK(counts[0] < counts[1]);

    const double sea = world.sea_level().value_or(world.planet().body.radius);
    const hog::DVec3 sky = glm::normalize(place(false).point) * (sea + 1000.0);
    const Key key = grid.key_at(sky, static_cast<std::uint8_t>(grid.depth() - 6)).value();
    const hog::globe::Filled filled = hog::globe::fill(world, grid, key, 6);
    REQUIRE(filled.shape.nodes() == 1);
    ChunkStore store = scratch(hog::baked::baking_of(world), height, 64);
    const Key aligned = key.ancestor(static_cast<std::uint8_t>((key.depth / height) * height));
    hold(store, aligned,
         Tree<hog::grid::Shape>{static_cast<std::uint8_t>(grid.depth() - aligned.depth),
                                filled.shape.root().value, false});
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    for (std::uint8_t band = store.bands().band_of(aligned.depth + 1); band < store.bands().count();
         ++band) {
        CHECK(snapshot->chunks<hog::grid::Shape>(band, aligned).size() == 1);
    }
}

TEST_CASE("a chunk's version steps only when its bytes change", "[baked][chunks]") {
    ChunkStore store = scratch(baking(), 3, std::size_t{1} << 20U);
    const Key root{.depth = 3, .x = 2, .y = 5, .z = 1};
    const Tree<hog::grid::Material> first = sheet(6, 1);

    const std::vector<Chunk> made = put(store, 2, root, first);
    REQUIRE(made.size() == 1);
    CHECK(made[0].root == root);
    CHECK(made[0].version > 0);
    CHECK(put(store, 2, root, first).empty());
    {
        const auto snapshot = store.read();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->version<hog::grid::Material>(2, root) == made[0].version);
        CHECK(snapshot->version<hog::grid::Water>(2, root) == 0);
        CHECK(snapshot->version<hog::grid::Material>(1, root) == 0);
    }

    const std::vector<Chunk> changed = put(store, 2, root, sheet(6, 2));
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].version > made[0].version);
}

TEST_CASE("chunks another generator made read as not held, and are taken over on writing",
          "[baked][chunks]") {
    const hog::testing::TemporaryDirectory directory;
    const Key root{.depth = 3, .x = 2, .y = 5, .z = 1};
    const Tree<hog::grid::Material> tree = sheet(6, 1);
    std::uint64_t first = 0;
    {
        auto store =
            ChunkStore::open(directory.path(), baking(1), options(3, std::size_t{1} << 20U));
        REQUIRE(store.has_value());
        first = put(*store, 2, root, tree).at(0).version;
    }
    auto store = ChunkStore::open(directory.path(), baking(2), options(3, std::size_t{1} << 20U));
    REQUIRE(store.has_value());
    {
        const auto snapshot = store->read();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->version<hog::grid::Material>(2, root) == 0);
        CHECK(!snapshot->piece<hog::grid::Material>(root.child(0), 9).has_value());
        CHECK(snapshot->chunks<hog::grid::Material>(2, root).empty());
    }
    const std::vector<Chunk> taken = put(*store, 2, root, tree);
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].version > first);
}

TEST_CASE("a chunk past its budget is held as its root's children's, down to the band's roots",
          "[baked][chunks]") {
    ChunkStore store = scratch(baking(), 3, 512);
    const Key root{.depth = 3, .x = 2, .y = 5, .z = 1};
    const Tree<hog::grid::Material> tree = sheet(6, 1);
    const std::vector<Chunk> made = put(store, 2, root, tree);
    CHECK(made.size() > 8);
    std::set<std::uint64_t> versions;
    for (const Chunk& chunk : made) {
        CHECK(chunk.band == 2);
        CHECK(root.contains(chunk.root));
        CHECK(chunk.root.depth <= 6);
        versions.insert(chunk.version);
    }
    CHECK(versions.size() == made.size());

    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->version<hog::grid::Material>(2, root) == 0);
    CHECK(snapshot->chunks<hog::grid::Material>(2, root) == made);
    for (std::uint64_t pick = 0; pick < 20; ++pick) {
        const Key node = under(root, static_cast<std::uint8_t>(3 + (pick % 7)), pick);
        const auto shift = static_cast<std::uint8_t>(node.depth - root.depth);
        const auto piece = snapshot->piece<hog::grid::Material>(node, 9);
        // Bands 0 and 1 aren't held, so only nodes the chunks of band 2 hold whole read.
        if (!piece) {
            continue;
        }
        CHECK(piece->tree.same_as(tree.cut(shift, node.x - (root.x << shift),
                                           node.y - (root.y << shift), node.z - (root.z << shift),
                                           static_cast<std::uint8_t>(9 - node.depth))));
    }

    // Held again alike, nothing changes.
    CHECK(put(store, 2, root, tree).empty());
}

TEST_CASE("chunks are given as stored and each decodes to its part of what was put",
          "[baked][chunks]") {
    ChunkStore store = scratch(baking(), 3, 512);
    const Key root{.depth = 3, .x = 2, .y = 5, .z = 1};
    const Tree<hog::grid::Material> tree = sheet(6, 1);
    const std::vector<Chunk> made = put(store, 2, root, tree);
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const std::vector<hog::baked::StoredChunk> stored =
        snapshot->stored<hog::grid::Material>(2, root);
    REQUIRE(stored.size() == made.size());
    for (std::size_t n = 0; n < stored.size(); ++n) {
        CHECK(stored[n].chunk == made[n]);
        const auto decoded = Tree<hog::grid::Material>::decode(stored[n].tree);
        REQUIRE(decoded.has_value());
        const Key at = stored[n].chunk.root;
        CHECK(decoded->same_as(cut_at(tree, root, at, 9)));
    }
    // The chunk over a node deep in one is that one alone.
    const Key deep = under(made.front().root, 9, 3);
    const std::vector<hog::baked::StoredChunk> over =
        snapshot->stored<hog::grid::Material>(2, deep);
    REQUIRE(over.size() == 1);
    CHECK(over.front().chunk == made.front());
    CHECK(snapshot->stored<hog::grid::Shape>(2, root).empty());
}

TEST_CASE("a tree put under a held chunk changes only what's under it", "[baked][chunks]") {
    ChunkStore store = scratch(baking(), 3, std::size_t{1} << 20U);
    const Key root{.depth = 3, .x = 2, .y = 5, .z = 1};
    const Tree<hog::grid::Material> tree = sheet(6, 1);
    const std::uint64_t first = put(store, 2, root, tree).at(0).version;

    const Key part = root.child(3).child(5);
    const Tree<hog::grid::Material> other = sheet(4, 7);
    const std::vector<Chunk> changed = put(store, 2, part, other);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].root == root);
    CHECK(changed[0].version > first);

    Tree<hog::grid::Material> expected = tree;
    expected.graft(2, part.x - (root.x << 2U), part.y - (root.y << 2U), part.z - (root.z << 2U),
                   other);
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const auto piece = snapshot->piece<hog::grid::Material>(root, 9);
    REQUIRE(piece.has_value());
    CHECK(piece.value().tree.same_as(expected));
    CHECK(piece.value().chunks == std::vector<Chunk>{changed[0]});
}

TEST_CASE("a chunk store refuses chunks off their band, and stores of another band height",
          "[baked][chunks]") {
    const hog::testing::TemporaryDirectory directory;
    auto opened = ChunkStore::open(directory.path(), baking(), options(3, std::size_t{1} << 20U));
    REQUIRE(opened.has_value());
    ChunkStore store = std::move(*opened);
    const Key root{.depth = 4, .x = 2, .y = 5, .z = 1};
    const Result<void> deep = store.write([&](ChunkStore::Batch& batch) -> Result<void> {
        auto put = batch.put(1, root, Tree<hog::grid::Material>{2, 1, false});
        if (!put) {
            return std::unexpected{put.error()};
        }
        return {};
    });
    CHECK(!deep.has_value());
    const Result<void> short_of = store.write([&](ChunkStore::Batch& batch) -> Result<void> {
        auto put = batch.put(2, root.parent(), Tree<hog::grid::Material>{5, 1, false});
        if (!put) {
            return std::unexpected{put.error()};
        }
        return {};
    });
    CHECK(!short_of.has_value());
    const Result<void> past = store.write([&](ChunkStore::Batch& batch) -> Result<void> {
        auto put = batch.put(4, Key{}, Tree<hog::grid::Material>{11, 1, false});
        if (!put) {
            return std::unexpected{put.error()};
        }
        return {};
    });
    CHECK(!past.has_value());

    store = scratch(baking(), 3, 1024);
    CHECK(!ChunkStore::open(directory.path(), baking(), options(4, 1024)).has_value());
    CHECK(ChunkStore::open(directory.path(), baking(), options(3, 1024)).has_value());
}
