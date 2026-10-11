#include "temporary_database.hpp"

#include <hog/save/snapshot.hpp>
#include <hog/save/snapshot_store.hpp>
#include <hog/store/sqlite_snapshot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::ErrorCode;
using hog::GameTime;
using hog::save::Section;
using hog::save::section_id;
using hog::save::Snapshot;
using hog::store::SqliteSnapshotStore;
using hog::store::testing::query;
using hog::store::testing::tamper;
using hog::store::testing::TemporaryFile;

namespace {

constexpr std::uint64_t world = 0xBEEF;

SqliteSnapshotStore memory_store() {
    auto store = SqliteSnapshotStore::open(":memory:");
    REQUIRE(store);
    return *std::move(store);
}

Snapshot at_tick(std::uint64_t tick) {
    Snapshot snapshot{
        .world = world,
        .time = GameTime{std::chrono::seconds{static_cast<std::int64_t>(tick)}},
        .tick = hog::Tick{tick},
        .sections = {},
    };
    snapshot.add(Section{
        .id = section_id("TEST"), .version = 1, .bytes = std::vector(1'000, std::byte{0x5A})});
    return snapshot;
}

} // namespace

TEST_CASE("stored snapshots come back newest first", "[store][snapshot]") {
    SqliteSnapshotStore store = memory_store();
    CHECK(store.list()->empty());
    const auto first = store.write(GameTime{}, std::vector{std::byte{1}, std::byte{2}});
    const auto second = store.write(GameTime{}, {});
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*second > *first);
    CHECK(store.list() == std::vector{*second, *first});
    CHECK(store.read(*first) == std::vector{std::byte{1}, std::byte{2}});
    CHECK(store.read(*second) == std::vector<std::byte>{});

    const auto missing = store.read(*second + 1);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("pruning keeps the newest and never reuses ids", "[store][snapshot]") {
    SqliteSnapshotStore store = memory_store();
    std::vector<SqliteSnapshotStore::Id> ids;
    ids.reserve(5);
    for (int i = 0; i < 5; ++i) {
        ids.push_back(*store.write(GameTime{}, std::vector{std::byte{3}}));
    }
    CHECK(store.prune(10) == 0U);
    CHECK(store.prune(2) == 3U);
    CHECK(store.list() == std::vector{ids[4], ids[3]});
    CHECK(store.prune(0) == 2U);
    CHECK(store.list()->empty());
    CHECK(*store.write(GameTime{}, {}) > ids[4]);
}

TEST_CASE("snapshots outlive the store", "[store][snapshot]") {
    const TemporaryFile file;
    {
        auto store = SqliteSnapshotStore::open(file.path());
        REQUIRE(store);
        for (std::uint64_t tick = 1; tick <= 4; ++tick) {
            REQUIRE(hog::save::save(*store, at_tick(tick), 3));
        }
    }
    CHECK(query(file.path(), "SELECT count(*) FROM snapshots") == 3);
    CHECK(query(file.path(), "SELECT min(time) FROM snapshots") ==
          at_tick(2).time.time_since_epoch().count());

    auto store = SqliteSnapshotStore::open(file.path());
    REQUIRE(store);
    const auto loaded = hog::save::load_newest(*store, world);
    REQUIRE(loaded);
    CHECK(loaded->snapshot == at_tick(4));
    CHECK(loaded->skipped.empty());
}

TEST_CASE("a damaged snapshot falls back to the one before", "[store][snapshot]") {
    const TemporaryFile file;
    {
        auto store = SqliteSnapshotStore::open(file.path());
        REQUIRE(store);
        REQUIRE(hog::save::save(*store, at_tick(1), 3));
        REQUIRE(hog::save::save(*store, at_tick(2), 3));
    }
    tamper(file.path(), "UPDATE snapshots SET data = substr(data, 1, 100) "
                        "WHERE id = (SELECT max(id) FROM snapshots)");
    auto store = SqliteSnapshotStore::open(file.path());
    REQUIRE(store);
    const auto loaded = hog::save::load_newest(*store, world);
    REQUIRE(loaded);
    CHECK(loaded->snapshot == at_tick(1));
    CHECK(loaded->skipped.size() == 1);
}
