#include "temporary_database.hpp"

#include <hog/core/hash.hpp>
#include <hog/planet/world.hpp>
#include <hog/store/sqlite_change_store.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <vector>

using hog::ErrorCode;
using hog::GameTime;
using hog::planet::AuthorId;
using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::Change;
using hog::planet::ChangeFilter;
using hog::planet::Excavate;
using hog::planet::fixed_one;
using hog::planet::Layer;
using hog::planet::SetBiome;
using hog::planet::SetElevation;
using hog::store::SqliteChangeStore;
using hog::store::testing::tamper;
using hog::store::testing::TemporaryFile;

namespace {

GameTime at(std::int64_t seconds) {
    return GameTime{std::chrono::seconds{seconds}};
}

SqliteChangeStore memory_store() {
    auto store = SqliteChangeStore::open(":memory:");
    REQUIRE(store);
    return *std::move(store);
}

// Changes that reach the edges of every field.
std::vector<Change> sample_changes() {
    constexpr auto most = std::numeric_limits<std::int64_t>::max();
    return {
        {.cell = CellId::from_face_ij(5, 1, 2, 30),
         .time = GameTime{hog::GameDuration{std::numeric_limits<std::int64_t>::min()}},
         .author = AuthorId{std::numeric_limits<std::uint64_t>::max()},
         .layer = Layer::Authored,
         .payload = SetElevation{.elevation = -most}},
        {.cell = CellId::from_face(0),
         .time = at(0),
         .author = AuthorId{0},
         .layer = Layer::Simulation,
         .payload = SetBiome{.biome = Biome::TropicalRainforest}},
        {.cell = CellId::from_face_ij(3, 77, 88, 12),
         .time = at(123),
         .author = AuthorId{42},
         .layer = Layer::Player,
         .payload = Excavate{.depth = most}},
        {.cell = CellId::from_face_ij(4, 5, 6, 14),
         .time = at(124),
         .author = AuthorId{42},
         .layer = Layer::Player,
         .payload = hog::planet::Hollow{.top = fixed_one, .bottom = most}},
    };
}

} // namespace

TEST_CASE("a new store is empty", "[store]") {
    const auto loaded = memory_store().load();
    REQUIRE(loaded);
    CHECK(loaded->empty());
}

TEST_CASE("changes come back as they were stored, in order", "[store]") {
    SqliteChangeStore store = memory_store();
    const auto changes = sample_changes();
    REQUIRE(store.append(changes));
    REQUIRE(store.append(std::span{changes}.first(1)));
    REQUIRE(store.append({}));
    const auto loaded = store.load();
    REQUIRE(loaded);
    REQUIRE(loaded->size() == changes.size() + 1);
    CHECK(std::vector(loaded->begin(), loaded->end() - 1) == changes);
    CHECK(loaded->back() == changes.front());
}

TEST_CASE("changes outlive the store", "[store]") {
    const TemporaryFile file;
    {
        auto store = SqliteChangeStore::open(file.path());
        REQUIRE(store);
        REQUIRE(store->append(sample_changes()));
    }
    auto reopened = SqliteChangeStore::open(file.path());
    REQUIRE(reopened);
    const auto loaded = reopened->load();
    REQUIRE(loaded);
    CHECK(*loaded == sample_changes());
}

TEST_CASE("erasing removes the matching changes", "[store]") {
    SqliteChangeStore store = memory_store();
    const auto changes = sample_changes();
    REQUIRE(store.append(changes));
    ChangeFilter filter;

    SECTION("by author") {
        filter.author = AuthorId{42};
    }
    SECTION("by the widest author id") {
        filter.author = AuthorId{std::numeric_limits<std::uint64_t>::max()};
    }
    SECTION("by layer") {
        filter.layer = Layer::Simulation;
    }
    SECTION("by time, excluding the end") {
        filter.from = at(0);
        filter.until = at(123);
    }
    SECTION("everything") {}
    std::vector<Change> kept;
    for (const Change& change : changes) {
        if (!filter.matches(change)) {
            kept.push_back(change);
        }
    }
    const auto erased = store.erase(filter);
    REQUIRE(erased);
    CHECK(*erased == changes.size() - kept.size());
    const auto loaded = store.load();
    REQUIRE(loaded);
    CHECK(*loaded == kept);
}

TEST_CASE("damaged stores are refused", "[store]") {
    const TemporaryFile file;
    {
        auto store = SqliteChangeStore::open(file.path());
        REQUIRE(store);
        REQUIRE(store->append(sample_changes()));
    }
    SECTION("an unknown schema") {
        tamper(file.path(), "PRAGMA user_version = 99");
        const auto store = SqliteChangeStore::open(file.path());
        REQUIRE_FALSE(store);
        CHECK(store.error().code == ErrorCode::Unsupported);
    }
    SECTION("a bad payload") {
        tamper(file.path(), "UPDATE changes SET payload = x'01' WHERE kind = 0");
    }
    SECTION("an unknown layer") {
        tamper(file.path(), "UPDATE changes SET layer = 9 WHERE sequence = 2");
    }
    SECTION("an unknown kind") {
        tamper(file.path(), "UPDATE changes SET kind = 300 WHERE sequence = 3");
    }
    if (auto store = SqliteChangeStore::open(file.path())) {
        const auto loaded = store->load();
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == ErrorCode::Parse);
    }
}

TEST_CASE("a directory is not a store", "[store]") {
    const auto store = SqliteChangeStore::open(std::filesystem::temp_directory_path());
    REQUIRE_FALSE(store);
    CHECK(store.error().code == ErrorCode::Io);
}

TEST_CASE("a world rebuilt from its store matches one that never saw the erased changes",
          "[store]") {
    const hog::planet::PlanetConfig planet{.seed = 1,
                                           .ocean_fraction = 0.7,
                                           .radius = 115'000.0,
                                           .erosion_cell = hog::testing::coarse_erosion_cell};
    static hog::planet::World rebuilt{planet};
    static hog::planet::World replayed{planet};
    const CellId cell = CellId::from_face_ij(2, 300, 400, 10);
    const std::vector<CellId> cells{cell, cell.child(1), cell.child(1).child(2), cell.parent()};
    for (std::uint64_t trial = 0; trial < 20; ++trial) {
        INFO("trial " << trial);
        const auto draw = [&](std::uint64_t index, std::uint64_t what, std::uint64_t range) {
            return hog::hash_values(trial, index, what) % range;
        };
        const std::uint64_t count = draw(0, 0, 100);
        std::vector<Change> all;
        all.reserve(count);
        for (std::uint64_t index = 0; index < count; ++index) {
            all.push_back({.cell = cells.at(draw(index, 1, cells.size())),
                           .time = at(static_cast<std::int64_t>(draw(index, 2, 30))),
                           .author = AuthorId{draw(index, 3, 3)},
                           .layer = static_cast<Layer>(draw(index, 4, hog::planet::layer_count)),
                           .payload = Excavate{.depth = fixed_one}});
        }
        ChangeFilter latest;
        latest.from = at(static_cast<std::int64_t>(draw(0, 5, 31)));
        std::vector<Change> kept;
        for (const Change& change : all) {
            if (!latest.matches(change)) {
                kept.push_back(change);
            }
        }

        SqliteChangeStore store = memory_store();
        REQUIRE(store.append(all));
        REQUIRE(store.erase(latest));
        const auto loaded = store.load();
        REQUIRE(loaded);
        CHECK(*loaded == kept);

        rebuilt.roll_back({});
        replayed.roll_back({});
        REQUIRE(rebuilt.replay(*loaded));
        REQUIRE(replayed.replay(kept));
        for (const CellId probe : cells) {
            CHECK(rebuilt.state(probe) == replayed.state(probe));
        }
    }
}
