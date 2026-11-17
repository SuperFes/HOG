#include <hog/baked/page_store.hpp>
#include <hog/baked/store.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

using hog::Result;
using hog::baked::Baking;
using hog::baked::PageStore;
using hog::grid::Key;
using hog::grid::Tree;

namespace {

// 1 m nodes 11 deep: a top page of 5 levels over pages of 6.
hog::grid::Grid test_grid() {
    return {1000.0, 1.0};
}

Baking baking(std::uint32_t generator = 1) {
    return {.world = 42, .grid = test_grid(), .generator = generator};
}

PageStore::Options small() {
    return {.map_bytes = std::uint64_t{64} << 20U, .readers = 64, .read_only = false};
}

PageStore open(const hog::testing::TemporaryDirectory& directory, std::uint32_t generator = 1) {
    auto store = PageStore::open(directory.path(), baking(generator), small());
    REQUIRE(store.has_value());
    return std::move(*store);
}

// Materials changing along x, `levels` deep.
Tree<hog::grid::Material> layers(std::uint8_t levels, std::uint16_t first) {
    return Tree<hog::grid::Material>::build(levels, [&](int x, int /*y*/, int /*z*/) {
        return static_cast<std::uint16_t>(first + (x / 4));
    });
}

template <typename Kind>
Result<std::uint32_t> put(PageStore& store, Key root, const Tree<Kind>& tree) {
    std::uint32_t version = 0;
    Result<void> done = store.write([&](PageStore::Batch& batch) -> Result<void> {
        auto held = batch.put(root, tree);
        if (!held) {
            return std::unexpected{held.error()};
        }
        version = *held;
        return {};
    });
    if (!done) {
        return std::unexpected{done.error()};
    }
    return version;
}

constexpr Key fine{.depth = 5, .x = 9, .y = 20, .z = 3};

} // namespace

TEST_CASE("a page store reads back each tree it was given, at its version", "[baked]") {
    REQUIRE(test_grid().depth() == 11);
    const hog::testing::TemporaryDirectory directory;
    PageStore store = open(directory);
    const Tree<hog::grid::Shape> shape{6, hog::grid::Surface::solid(), false};
    const Tree<hog::grid::Material> material = layers(6, 3);
    CHECK(put(store, fine, shape) == 1U);
    CHECK(put(store, fine, material) == 1U);

    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const std::optional<PageStore::Read> read = snapshot->page(fine);
    REQUIRE(read.has_value());
    const hog::grid::Page& page = read.value().page;
    CHECK(page.root == fine);
    CHECK(page.generator == 1U);
    REQUIRE(page.shape.has_value());
    CHECK(page.shape.value().version == 1U);
    CHECK(page.shape.value().tree.same_as(shape));
    REQUIRE(page.material.has_value());
    CHECK(page.material.value().tree.same_as(material));
    CHECK_FALSE(page.water.has_value());
    CHECK(read.value().stale == hog::baked::Trees{});
    CHECK_FALSE(snapshot->page(Key{.depth = 5, .x = 9, .y = 20, .z = 4}).has_value());

    // Its version steps on only when its bytes change.
    CHECK(put(store, fine, layers(6, 3)) == 1U);
    CHECK(put(store, fine, layers(6, 4)) == 2U);
    CHECK(put(store, fine, layers(6, 3)) == 3U);
    // A snapshot sees the store as it was until refreshed.
    CHECK(snapshot->page(fine).value().page.material.value().version == 1U);
    REQUIRE(snapshot->refresh().has_value());
    CHECK(snapshot->page(fine).value().page.material.value().version == 3U);
    CHECK(snapshot->page(fine).value().page.shape.value().version == 1U);
}

TEST_CASE("the holder of any node is the deepest page over it holding that tree", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    PageStore store = open(directory);
    const Key top{};
    REQUIRE(put(store, top, layers(5, 1)).has_value());
    REQUIRE(put(store, fine, layers(6, 7)).has_value());
    REQUIRE(put(store, fine, Tree<hog::grid::Water>{6, 1, false}).has_value());
    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());

    const Key in_fine{.depth = 11, .x = (9 * 64) + 13, .y = (20 * 64) + 63, .z = 3 * 64};
    CHECK(snapshot->holder<hog::grid::Material>(in_fine) == fine);
    CHECK(snapshot->holder<hog::grid::Water>(in_fine) == fine);
    CHECK(snapshot->holder<hog::grid::Material>(in_fine.ancestor(7)) == fine);
    // A page's root is held by the page under it, the node over it by the page above.
    CHECK(snapshot->holder<hog::grid::Material>(fine) == fine);
    CHECK(snapshot->holder<hog::grid::Material>(fine.parent()) == top);
    CHECK(snapshot->holder<hog::grid::Material>(top) == top);
    // Beside the fine page only the top page holds it, and nothing holds water.
    const Key beside{.depth = 11, .x = (10 * 64) + 13, .y = (20 * 64) + 63, .z = 3 * 64};
    CHECK(snapshot->holder<hog::grid::Material>(beside) == top);
    CHECK_FALSE(snapshot->holder<hog::grid::Water>(beside).has_value());
    CHECK_FALSE(snapshot->holder<hog::grid::Shape>(in_fine).has_value());
    CHECK_FALSE(snapshot->holder<hog::grid::Material>(Key{.depth = 12}).has_value());
}

TEST_CASE("trees another generator made are held stale, never read as current", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    {
        PageStore store = open(directory, 1);
        REQUIRE(put(store, fine, layers(6, 3)).has_value());
        REQUIRE(put(store, fine, Tree<hog::grid::Water>{6, 1, false}).has_value());
    }
    PageStore store = open(directory, 2);
    {
        auto snapshot = store.read();
        REQUIRE(snapshot.has_value());
        const std::optional<PageStore::Read> read = snapshot->page(fine);
        REQUIRE(read.has_value());
        CHECK_FALSE(read.value().page.material.has_value());
        CHECK_FALSE(read.value().page.water.has_value());
        CHECK(read.value().stale ==
              hog::baked::Trees{.shape = false, .material = true, .water = true});
        CHECK_FALSE(snapshot->holder<hog::grid::Material>(fine).has_value());
    }
    // Made again alike, it keeps its version; unlike, it steps on.
    CHECK(put(store, fine, layers(6, 3)) == 1U);
    CHECK(put(store, fine, Tree<hog::grid::Water>{6, 2, false}) == 2U);
    auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    const PageStore::Read read = snapshot->page(fine).value();
    CHECK(read.page.generator == 2U);
    CHECK(read.page.material.value().version == 1U);
    CHECK(read.page.water.value().version == 2U);
    CHECK(read.stale == hog::baked::Trees{});
}

TEST_CASE("a page store refuses other worlds, other stores, and trees off their pages", "[baked]") {
    const hog::testing::TemporaryDirectory directory;
    {
        PageStore store = open(directory);
        // Not a page's root, below the finest depth, or deeper than its page.
        CHECK_FALSE(put(store, fine.child(0), layers(5, 1)).has_value());
        CHECK_FALSE(put(store, Key{.depth = 11}, layers(0, 1)).has_value());
        CHECK_FALSE(put(store, Key{}, layers(6, 1)).has_value());
        CHECK(put(store, Key{}, layers(4, 1)) == 1U);
    }
    Baking other = baking();
    other.world = 43;
    CHECK(PageStore::open(directory.path(), other, small()).error().code ==
          hog::ErrorCode::InvalidArgument);
    CHECK(PageStore::open(directory.path(), {.world = 42, .grid = {1000.0, 0.5}, .generator = 1},
                          small())
              .error()
              .code == hog::ErrorCode::InvalidArgument);

    // A store of the cube-sphere's faces, or none at all.
    const hog::testing::TemporaryDirectory faces;
    REQUIRE(hog::baked::Store::open(faces.path()).has_value());
    CHECK(PageStore::open(faces.path(), baking(), small()).error().code ==
          hog::ErrorCode::Unsupported);
    const hog::testing::TemporaryDirectory empty;
    PageStore::Options read_only = small();
    read_only.read_only = true;
    CHECK_FALSE(PageStore::open(empty.path(), baking(), read_only).has_value());
}

TEST_CASE("a scratch store is a new one of its own, gone once closed", "[baked]") {
    std::filesystem::path first;
    {
        auto store = PageStore::scratch(baking(), small());
        REQUIRE(store.has_value());
        first = store->directory();
        CHECK(std::filesystem::is_directory(first));
        REQUIRE(put(*store, Key{}, layers(5, 3)).has_value());
        auto other = PageStore::scratch(baking(), small());
        REQUIRE(other.has_value());
        CHECK(other->directory() != first);
        auto snapshot = other->read();
        REQUIRE(snapshot.has_value());
        CHECK_FALSE(snapshot->page(Key{}).has_value());
    }
    CHECK_FALSE(std::filesystem::exists(first));
}
