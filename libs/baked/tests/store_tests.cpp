#include <hog/baked/store.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using hog::Result;
using hog::baked::Batch;
using hog::baked::encode_one;
using hog::baked::encode_tree;
using hog::baked::Leaf;
using hog::baked::Node;
using hog::baked::Options;
using hog::baked::Point;
using hog::baked::Record;
using hog::baked::Store;
using hog::globe::Cube;
using hog::globe::Material;
using hog::globe::Voxel;
using hog::globe::VoxelTree;

namespace {

Options small() {
    return {.map_bytes = std::uint64_t{64} << 20U, .readers = 256};
}

Cube rock() {
    return {.solid = Material::Granite, .solid_amount = Voxel::full};
}

Cube sand() {
    return {.solid = Material::Sand, .solid_amount = Voxel::full};
}

// Rock under a plane through the cube, air over it.
VoxelTree ground(int side) {
    return VoxelTree::build(side,
                            [&](int x, int y, int z) { return z < (x + y) / 2 ? rock() : Cube{}; });
}

Result<void> put(Store& store, const Node& node, const std::vector<std::byte>& page) {
    return store.write([&](Batch& batch) { return batch.put(node, page); });
}

} // namespace

TEST_CASE("the store reads back what it was given", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    auto store = Store::open(directory.path(), small());
    REQUIRE(store.has_value());
    REQUIRE(store->header().value().empty());

    const Node node{.face = 1, .size = 5, .x = 64, .y = 32, .z = 0};
    const VoxelTree tree = ground(32);
    const std::vector<std::byte> header{std::byte{7}, std::byte{8}};
    REQUIRE(store->write([&](Batch& batch) -> Result<void> {
        if (Result<void> set = batch.set_header(header); !set) {
            return set;
        }
        return batch.put(node, encode_tree(tree, 5, 0, 0));
    }));
    CHECK(store->header() == header);

    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    for (std::uint32_t z = 0; z < 32; ++z) {
        for (std::uint32_t y = 0; y < 32; y += 3) {
            for (std::uint32_t x = 0; x < 32; x += 5) {
                const auto found = snapshot->at(Point{.face = 1, .x = 64 + x, .y = 32 + y, .z = z});
                REQUIRE(found.has_value());
                REQUIRE(found.value_or(Leaf{}).cube ==
                        tree.at(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)));
            }
        }
    }
    CHECK_FALSE(snapshot->at(Point{.face = 1, .x = 63, .y = 32, .z = 0}));
    CHECK_FALSE(snapshot->at(Point{.face = 0, .x = 64, .y = 32, .z = 0}));
    CHECK_FALSE(snapshot->at(Point{.face = 1, .x = 96, .y = 32, .z = 0}));
}

TEST_CASE("the store keeps its records apart", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    auto store = Store::open(directory.path(), small());
    REQUIRE(store.has_value());
    const Node whole{.face = 2, .size = 8};
    REQUIRE(put(*store, whole, encode_one(8, 3, 0, rock(), 4)));

    // Finer within it: the coarse record is cut down to what's beside the fine one.
    const Node fine{.face = 2, .size = 3, .x = 40, .y = 200, .z = 8};
    REQUIRE(put(*store, fine, encode_one(3, 0, 0, sand(), 0)));
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->at(Point{.face = 2, .x = 41, .y = 207, .z = 15}).value_or(Leaf{}).cube ==
          sand());
    const auto found = snapshot->at(Point{.face = 2, .x = 39, .y = 200, .z = 8});
    REQUIRE(found.has_value());
    const Leaf beside = found.value_or(Leaf{});
    CHECK(beside.cube == rock());
    CHECK(beside.open == 4);
    CHECK(snapshot->at(Point{.face = 2, .x = 255, .y = 255, .z = 255}).value_or(Leaf{}).cube ==
          rock());
    // One record each for the fine node and its seven siblings at each size up to the whole.
    CHECK(store->stats().value_or(Store::Stats{}).records == 1 + (7 * 5));

    std::size_t seen = 0;
    snapshot->each(Node{.face = 2, .size = 4, .x = 32, .y = 192, .z = 0}, [&](const Record& r) {
        CHECK(Node{.face = 2, .size = 4, .x = 32, .y = 192, .z = 0}.contains(r.node));
        ++seen;
    });
    CHECK(seen == 8);

    // Coarse over it again: one record.
    REQUIRE(put(*store, whole, encode_one(8, 3, 0, sand(), 4)));
    CHECK(store->stats().value_or(Store::Stats{}).records == 1);
    REQUIRE(snapshot->refresh().has_value());
    CHECK(snapshot->at(Point{.face = 2, .x = 41, .y = 207, .z = 15}).value_or(Leaf{}).cube ==
          sand());

    REQUIRE(store->write([&](Batch& batch) { return batch.erase(fine); }));
    REQUIRE(snapshot->refresh().has_value());
    CHECK_FALSE(snapshot->at(Point{.face = 2, .x = 41, .y = 207, .z = 15}));
    CHECK(snapshot->at(Point{.face = 2, .x = 40, .y = 200, .z = 16}));
}

TEST_CASE("the store splits pages too large to keep whole", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    Options options = small();
    options.page_bytes = 2048;
    auto store = Store::open(directory.path(), options);
    REQUIRE(store.has_value());
    // Ground with grains of sand strewn through it, so few nodes are alike.
    const VoxelTree tree = VoxelTree::build(32, [](int x, int y, int z) {
        if (hog::hash_values(std::uint64_t{4}, x, y, z) % 5 == 0) {
            return sand();
        }
        return z < (x + y) / 2 ? rock() : Cube{};
    });
    const std::vector<std::byte> page = encode_tree(tree, 5, 0, 0);
    REQUIRE(page.size() > 8 * options.page_bytes);
    REQUIRE(put(*store, Node{.face = 0, .size = 5}, page));
    CHECK(store->stats().value_or(Store::Stats{}).records > 8);
    auto snapshot = store->read();
    REQUIRE(snapshot.has_value());
    for (std::uint32_t n = 0; n < 500; ++n) {
        const auto x = static_cast<std::uint32_t>(hog::hash_values(std::uint64_t{1}, n) % 32);
        const auto y = static_cast<std::uint32_t>(hog::hash_values(std::uint64_t{2}, n) % 32);
        const auto z = static_cast<std::uint32_t>(hog::hash_values(std::uint64_t{3}, n) % 32);
        REQUIRE(snapshot->at(Point{.face = 0, .x = x, .y = y, .z = z}).value_or(Leaf{}).cube ==
                tree.at(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z)));
    }
}

TEST_CASE("a failed batch leaves the store as it was", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    auto store = Store::open(directory.path(), small());
    REQUIRE(store.has_value());
    const Node node{.face = 0, .size = 4};
    REQUIRE(put(*store, node, encode_one(4, 0, 0, rock(), 0)));
    const Result<void> failed = store->write([&](Batch& batch) -> Result<void> {
        REQUIRE(batch.put(node, encode_one(4, 0, 0, sand(), 0)));
        const auto record = batch.record(Point{});
        REQUIRE(record.has_value());
        CHECK(record->page.at(0, 0, 0).cube == sand());
        return hog::make_error(hog::ErrorCode::Internal, "stop");
    });
    CHECK_FALSE(failed);
    const auto after = store->read();
    REQUIRE(after.has_value());
    CHECK(after->at(Point{}).value_or(Leaf{}).cube == rock());
    // A page of another size than its node is refused.
    CHECK_FALSE(put(*store, node, encode_one(5, 0, 0, sand(), 0)));
}

TEST_CASE("snapshots read while a batch writes", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    auto store = Store::open(directory.path(), small());
    REQUIRE(store.has_value());
    constexpr std::uint32_t nodes = 64;
    // Each node rock, then sand once the writer reaches it; a reader never sees anything else.
    for (std::uint32_t n = 0; n < nodes; ++n) {
        REQUIRE(put(*store, Node{.face = 0, .size = 4, .x = n * std::uint32_t{16}},
                    encode_one(4, 0, 0, rock(), 0)));
    }
    std::atomic<bool> done{false};
    std::atomic<std::size_t> wrong{0};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    readers.reserve(16);
    for (std::uint64_t r = 0; r < 16; ++r) {
        readers.emplace_back([&, r] {
            auto snapshot = store->read();
            if (!snapshot) {
                ++wrong;
                return;
            }
            for (std::uint64_t n = 0; !done.load() || n < 1000; ++n) {
                const auto x = static_cast<std::uint32_t>(hog::hash_values(r, n) %
                                                          (std::uint64_t{nodes} * 16));
                const auto leaf = snapshot->at(Point{.x = x, .y = 3, .z = 7});
                if (!leaf || (leaf->cube != rock() && leaf->cube != sand())) {
                    ++wrong;
                }
                ++reads;
                if (n % 64 == 0 && !snapshot->refresh()) {
                    ++wrong;
                }
            }
        });
    }
    for (std::uint32_t n = 0; n < nodes; ++n) {
        REQUIRE(put(*store, Node{.face = 0, .size = 4, .x = n * std::uint32_t{16}},
                    encode_one(4, 0, 0, sand(), 0)));
    }
    done = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK(wrong == 0);
    CHECK(reads >= 16000);
    const auto last = store->read();
    REQUIRE(last.has_value());
    CHECK(last->at(Point{.x = 5}).value_or(Leaf{}).cube == sand());
}
