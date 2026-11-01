#include <hog/globe/brick.hpp>
#include <hog/globe/brick_archive.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/brick_sharing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using hog::globe::Brick;
using hog::globe::BrickSite;
using hog::globe::Voxel;
using hog::globe::VoxelStore;
using hog::server::BrickSharing;

namespace {

constexpr hog::globe::Layers layers{
    .level = 18, .floor = 111'000.0, .thickness = 0.5, .count = 28'000};

BrickSite site_n(std::uint32_t n) {
    return {.face = 0, .i = n, .j = 0, .layer = 1};
}

} // namespace

TEST_CASE("every brick held is told to a player joining, and those changed since to everyone",
          "[server][bricks]") {
    VoxelStore store{layers};
    store.put(site_n(1), Brick{Voxel{.temperature = 2'300}});
    store.put(site_n(2), Brick{Voxel{.temperature = 2'301}});
    BrickSharing sharing{store};
    const std::vector<hog::protocol::Bricks> all = sharing.all();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].bricks.size() == 2);
    CHECK(all[0].bricks[0].key == hog::globe::brick_key(site_n(1)));
    CHECK_FALSE(all[0].bricks[0].bytes.empty());
    // Nothing has changed since the sharing began following.
    CHECK(sharing.changed().empty());
    store.put(site_n(3), Brick{Voxel{.temperature = 2'302}});
    store.erase(site_n(1));
    const std::vector<hog::protocol::Bricks> changed = sharing.changed();
    REQUIRE(changed.size() == 1);
    REQUIRE(changed[0].bricks.size() == 2);
    CHECK(changed[0].bricks[0].key == hog::globe::brick_key(site_n(1)));
    CHECK(changed[0].bricks[0].bytes.empty());
    CHECK(changed[0].bricks[1].key == hog::globe::brick_key(site_n(3)));
    CHECK_FALSE(changed[0].bricks[1].bytes.empty());
    CHECK(sharing.changed().empty());
    // Taken into another store, it holds the same.
    VoxelStore theirs{layers};
    for (const hog::protocol::SentBrick& brick : all[0].bricks) {
        REQUIRE(hog::globe::take_brick(theirs, {.key = brick.key, .bytes = brick.bytes}));
    }
    for (const hog::protocol::SentBrick& brick : changed[0].bricks) {
        REQUIRE(hog::globe::take_brick(theirs, {.key = brick.key, .bytes = brick.bytes}));
    }
    CHECK(theirs.sites() == store.sites());
}

TEST_CASE("bricks are told in messages of so many at most", "[server][bricks]") {
    VoxelStore store{layers};
    for (std::uint32_t n = 0; n < hog::protocol::most_bricks + 1; ++n) {
        store.put(site_n(n), Brick{Voxel{.temperature = static_cast<std::uint16_t>(n)}});
    }
    const BrickSharing sharing{store};
    const std::vector<hog::protocol::Bricks> all = sharing.all();
    REQUIRE(all.size() == 2);
    CHECK(all[0].bricks.size() == hog::protocol::most_bricks);
    CHECK(all[1].bricks.size() == 1);
}
