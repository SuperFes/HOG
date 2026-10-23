#include "temporary_database.hpp"

#include <hog/store/sqlite_player_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using hog::store::PlayerPlace;
using hog::store::SqlitePlayerStore;
using hog::store::testing::TemporaryFile;

TEST_CASE("a player's place is kept by account, the newest in place of the last",
          "[store][players]") {
    auto store = SqlitePlayerStore::open(":memory:");
    REQUIRE(store);
    const auto none = store->find(7);
    REQUIRE(none);
    CHECK_FALSE(*none);

    const PlayerPlace walking{.at = {115'000'000, -3, std::numeric_limits<std::int64_t>::min()},
                              .flying = false,
                              .heading = 65'535,
                              .pitch = -32'768};
    const PlayerPlace flying{.at = {1, 2, 3}, .flying = true, .heading = 0, .pitch = 32'767};
    const std::uint64_t last = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(store->keep(7, walking));
    REQUIRE(store->keep(last, flying));
    CHECK(store->find(7) == walking);
    CHECK(store->find(last) == flying);

    REQUIRE(store->keep(7, flying));
    CHECK(store->find(7) == flying);
}

TEST_CASE("a player's place outlasts the store that kept it", "[store][players]") {
    const TemporaryFile file;
    const PlayerPlace place{.at = {4, 5, 6}, .flying = true, .heading = 9, .pitch = -9};
    {
        auto store = SqlitePlayerStore::open(file.path());
        REQUIRE(store);
        REQUIRE(store->keep(3, place));
    }
    auto store = SqlitePlayerStore::open(file.path());
    REQUIRE(store);
    CHECK(store->find(3) == place);
}
