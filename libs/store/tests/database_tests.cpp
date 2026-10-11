#include "temporary_database.hpp"

#include <hog/core/time.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/change.hpp>
#include <hog/store/database.hpp>
#include <hog/store/sqlite_change_store.hpp>
#include <hog/store/sqlite_snapshot_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using hog::store::open_database;
using hog::store::schema_version;
using hog::store::SqliteChangeStore;
using hog::store::SqliteSnapshotStore;
using hog::store::testing::query;
using hog::store::testing::tamper;
using hog::store::testing::TemporaryFile;

TEST_CASE("a new database starts at the newest schema", "[store][database]") {
    const TemporaryFile file;
    REQUIRE(open_database(file.path()));
    CHECK(query(file.path(), "PRAGMA user_version") == schema_version());
    CHECK(query(file.path(), "SELECT count(*) FROM sqlite_schema WHERE name IN "
                             "('changes', 'snapshots')") == 2);
    // Opening again changes nothing.
    REQUIRE(open_database(file.path()));
    CHECK(query(file.path(), "PRAGMA user_version") == schema_version());
}

TEST_CASE("a schema 1 database is brought up to date", "[store][database]") {
    const TemporaryFile file;
    // Schema 1 as it was first released, with one change in it.
    tamper(file.path(), R"sql(
CREATE TABLE changes (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    cell INTEGER NOT NULL,
    time INTEGER NOT NULL,
    author INTEGER NOT NULL,
    layer INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    payload BLOB NOT NULL
);
CREATE INDEX changes_by_cell ON changes (cell);
CREATE INDEX changes_by_time ON changes (time);
CREATE INDEX changes_by_author ON changes (author, time);
INSERT INTO changes (cell, time, author, layer, kind, payload)
    VALUES (2305843009213693952, 5, 7, 2, 2, x'03');
PRAGMA user_version = 1;
)sql");

    auto changes = SqliteChangeStore::open(file.path());
    REQUIRE(changes);
    CHECK(query(file.path(), "PRAGMA user_version") == schema_version());
    const auto loaded = changes->load();
    REQUIRE(loaded);
    REQUIRE(loaded->size() == 1);
    CHECK(loaded->front().cell.raw() == 2305843009213693952ULL);
    CHECK(loaded->front().payload ==
          hog::planet::ChangePayload{hog::planet::SetBiome{.biome = hog::planet::Biome::Tundra}});

    auto snapshots = SqliteSnapshotStore::open(file.path());
    REQUIRE(snapshots);
    CHECK(snapshots->write(hog::GameTime{}, std::vector{std::byte{1}}));
}

TEST_CASE("a newer schema is refused", "[store][database]") {
    const TemporaryFile file;
    REQUIRE(open_database(file.path()));
    tamper(file.path(), "PRAGMA user_version = 99");
    const auto database = open_database(file.path());
    REQUIRE_FALSE(database);
    CHECK(database.error().code == hog::ErrorCode::Unsupported);
    CHECK_FALSE(SqliteSnapshotStore::open(file.path()));
}

TEST_CASE("stores share one file through their own connections", "[store][database]") {
    const TemporaryFile file;
    auto changes = SqliteChangeStore::open(file.path());
    auto snapshots = SqliteSnapshotStore::open(file.path());
    REQUIRE(changes);
    REQUIRE(snapshots);
    const hog::planet::Change change{.cell = hog::planet::CellId::from_face(2),
                                     .time = hog::GameTime{},
                                     .author = hog::planet::AuthorId{1},
                                     .layer = hog::planet::Layer::Player,
                                     .payload = hog::planet::Excavate{.depth = 3}};
    REQUIRE(changes->append(std::vector{change}));
    REQUIRE(snapshots->write(hog::GameTime{}, std::vector{std::byte{9}}));
    REQUIRE(changes->append(std::vector{change}));
    CHECK(changes->load()->size() == 2);
    CHECK(snapshots->list()->size() == 1);
}
